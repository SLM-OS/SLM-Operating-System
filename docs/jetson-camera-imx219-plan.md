# Jetson Camera IMX219-160 + MNIST Demo Plan

**Tracking:** 🎫 #396

**Status:** ☐ Phase 0 hardware recon not yet run; no driver code started. Pre-hardware code-read tasks unblocked and ready.

**Progress:** 0 / 20 tasks complete.

| Section | ✅ done | ☐ open | ☐🔗 blocked | ⏸️ deferred |
|---------|--------|---------|-------------|-------------|
| Pre-Hardware Tasks | 0 | 7 | 0 | 0 |
| Phase 0 — Hardware Recon | 0 | 0 | 4 | 0 |
| Hardware Tasks (post-Phase-0) | 0 | 0 | 6 | 0 |
| QEMU-Side Tasks | 0 | 3 | 0 | 0 |
| **Total** | **0** | **10** | **10** | **0** |

Icon legend (per project root `CLAUDE.md`): ✅ done · ☐ pending · ☐🔗 blocked on dependency · ⏸️ deferred to a future phase. The 🎫 above tracks the whole feature; per-bullet 🎫 is omitted as the convention allows.

---

## Goal

Drive the IMX219-160 CSI camera on the Jetson Orin Nano dev kit from
SLM-OS, capture a frame, preprocess it to MNIST input format, and run
the captured digit through the existing MNIST pipeline — end-to-end
from Lua.

The target demo flow:

```lua
-- camera_mnist_demo.lua
local cam = slm.camera.open("imx219-0")          -- I2C + NVCSI + VI bring-up
local frame = cam:capture()                       -- one RAW10 Bayer frame
local mnist = slm.camera.preprocess_mnist(frame) -- 3 136-byte fp32 (28*28*4)
local logits, pred = slm.model_infer_bytes(mnist_idx, mnist)
slm.print(string.format("predicted digit: %d", pred))
cam:close()
```

The existing `slm.model_infer_bytes(idx, bytes)` binding (PR #386,
commit `886dd0d`) already executes the MNIST CNN on the GA10B GPU when
a v6 handoff is loaded, with a CPU fallback otherwise. **The
inference half of the demo is a one-line Lua call.** Everything in
this plan is the camera-side work that must land before that call can
be made.

---

## Relation to Existing Work

This plan sits next to (not on top of) several Jetson-track items:

- `docs/jetson-cbb-report.md` — defines the CBB firewall envelope at
  NS EL2. Reachability of NVCSI, VI, and the camera I2C bus is the
  single biggest unknown for this work.
- `docs/jetson-bpmp-ipc-plan.md` + commits `6615da5`/`1f261e2` —
  established a working BPMP MRQ stack, which now gives SLM-OS clock
  enable / power-domain set / reset deassert at runtime. Camera
  bring-up depends on those primitives.
- `kernel/drivers/bpmp/` — `bpmp_clk_enable(id)`,
  `bpmp_reset_deassert(id)`, `bpmp_pg_set_state(domain, on)`. The
  Tegra234 clock IDs for I2C, NVCSI, and VI are defined in
  Linux's `dt-bindings/clock/tegra234-clock.h` and need to be
  ported into a header on the SLM-OS side.
- `slm.model_infer_bytes` (PR #386) — fp32-bytes inference path,
  already on `main`. Camera frames just need to be reshaped into
  this contract.

This plan deliberately does **not** depend on the GA10B GPU compute
path being healthy. The MNIST CPU fallback in `slm-runtime` is enough
to demo end-to-end; the GPU path is a "nice to have" speed-up.

---

## Hardware Background

### IMX219-160

- Sony IMX219 CMOS image sensor, 8 MP (3280×2464), Bayer RGGB.
- The "-160" suffix denotes a 160° FoV lens — sensor silicon and
  register set are identical to the standard IMX219.
- Reference clock (XCLK): 24 MHz, supplied by the SoC.
- Control: I²C (slave address 0x10 by default; some modules use 0x36).
- Data: 2-lane or 4-lane MIPI CSI-2; the Pi-camera-style modules used
  on Jetson dev kits are 2-lane.
- Default output for SLM-OS purposes: RAW10 Bayer at a low resolution
  (e.g. 640×480 binned mode), single frame on demand.
- Public datasheet: yes (Sony "IMX219PQ Datasheet, v0.6", widely
  mirrored).
- Existing reference driver: Linux `drivers/media/i2c/imx219.c`. Full
  init register list, mode tables, exposure / gain control.

### Jetson Orin Nano dev kit camera connectors

- **J17** and **J20**: 22-pin FPC connectors, Pi-camera-compatible
  pinout. Either can host an IMX219.
- Each connector exposes 2 CSI data lanes + clock + I²C + GPIOs (reset,
  power-down) + 24 MHz XCLK.
- The dev-kit carrier multiplexes I²C, GPIOs, and the CSI lanes
  through SoC pinmux. Linux device tree configures pinmux at boot;
  SLM-OS inherits that state via kexec.

### Tegra234 camera subsystem

The capture path on Orin is:

```
IMX219 ─MIPI CSI-2 (2 lanes)─▶ NVCSI ─Tegra-internal─▶ VI ─AXI/SMMU─▶ DRAM
   ▲                                                                    │
   │ I²C (mode + exposure)                                              │
   └────── HSI2C controller ────────────────────────────────────────────┘
              ▲
              │ clock + reset (BPMP MRQ)
   ┌──────────┴──────────────────────────────────────────────────────┐
   │ Power: AVDD (2.8V), DVDD (1.2V), DOVDD (1.8V) on the carrier   │
   │ XCLK: 24 MHz from SoC                                          │
   │ Reset / Power-down: GPIOs                                      │
   └────────────────────────────────────────────────────────────────┘
```

- **NVCSI** (NVIDIA CSI-2 receiver): Tegra234 MMIO base
  approximately `0x15a00000` (per Linux DT). Configures lane mapping,
  performs CSI-2 calibration, raises an interrupt on each frame.
- **VI** (Video Input): MMIO base approximately `0x15c00000`. Reads
  the NVCSI stream, optionally passes through ISP, and DMAs frames
  into a system-memory ring buffer.
- **HSI2C**: Tegra234 has multiple I²C controllers
  (`0x3160000`-family). The camera modules on J17 / J20 sit on
  specific instances pinned by the dev-kit DT.
- **SMMU**: VI's DMA traverses `arm-smmu` (the same SMMU that
  affected #266 USB networking); a stream-id translation must be in
  place before VI writes can land in DRAM.
- **GPIOs**: reset and power-down are on Tegra GPIO controllers; the
  exact pin assignments are in
  `arch/arm64/boot/dts/nvidia/tegra234-p3768-0000+p3767-0005.dts`.

NVIDIA does **not** publish a programmer's manual for NVCSI or VI.
Practical programming sequences live in the L4T Linux fork
(`drivers/media/platform/tegra/csi/` and `drivers/media/platform/tegra/vi/`)
and are partially documented in the Orin TRM (developer-login required;
local copy is referenced in the project root `CLAUDE.md`).

---

## Architecture

Six new subsystems on top of what already exists.

### 1. I²C driver — Tegra HSI2C

- Greenfield. No I²C bus master exists in SLM-OS today.
- Reference: Linux `drivers/i2c/busses/i2c-tegra.c`.
- Scope:
  - Polled (no DMA, no IRQ) start / repeated-start / stop.
  - 7-bit addressing only.
  - Up to 32-byte transfers (sufficient for IMX219 register writes).
  - Bus speed: 100 kHz standard, 400 kHz fast — the IMX219 happily
    runs at 100 kHz, so don't bother with timing tuning beyond that.
- Initialization needs:
  - BPMP `bpmp_clk_enable(TEGRA234_CLK_I2C<n>)` (n depends on which
    bus the camera connector uses; identify from DT).
  - `bpmp_reset_deassert(TEGRA234_RESET_I2C<n>)`.
  - Pinmux is inherited from Linux pre-kexec — no programming needed
    if SLM-OS keeps Linux's pinmux state intact (already true).
- API surface:
  ```c
  int  i2c_init(uint32_t bus_id, uint32_t base_mmio);
  int  i2c_write_reg16(uint32_t bus_id, uint8_t slave, uint16_t reg, uint8_t  val);
  int  i2c_read_reg16 (uint32_t bus_id, uint8_t slave, uint16_t reg, uint8_t *out);
  int  i2c_write_burst(uint32_t bus_id, uint8_t slave, const uint8_t *buf, size_t n);
  ```
  IMX219 uses 16-bit register addresses, hence the `_reg16` shape.

### 2. IMX219 sensor driver

- Pure I²C-side state machine. No CSI lane manipulation here.
- Initialization sequence (from Sony datasheet + Linux):
  1. Toggle reset GPIO low → high.
  2. Wait ~100 µs for chip ID read-back.
  3. Verify CHIP_ID at register `0x0000`.
  4. Apply mode register table (a sequence of ~50 16-bit reg writes
     selecting binned / cropped output mode).
  5. Set exposure (`0x015a`/`0x015b`) and gain (`0x0157`).
  6. `MODE_SELECT` (`0x0100`) ← `1` → streaming.
- Modes wanted:
  - **2-lane RAW10, 1640×1232 @ 30 fps** (full-FoV binned). Frame
    size: 1640 × 1232 × 10 bits = ~2.5 MB.
  - Optional later: **640×480 @ 30 fps** (further binned + cropped)
    to halve the per-frame memory cost.
- Reference: Linux `drivers/media/i2c/imx219.c` mode tables —
  copy verbatim into a header (it's BSD-friendly Linux GPL but the
  register sequence itself is Sony's, derived from the datasheet).

### 3. NVCSI driver

- Tegra234 MIPI CSI-2 receiver. Configures lane mapping, performs
  D-PHY calibration, and routes the stream to VI.
- **No NVIDIA programmer's manual.** The programming sequence has to
  be reverse-engineered from the L4T sources at
  `drivers/media/platform/tegra/csi/nvcsi.c` and the Orin TRM's
  partial register reference.
- Scope:
  - Single CSI port (whichever J17 / J20 connector the camera is on).
  - 2 data lanes + 1 clock lane.
  - RAW10 datatype, no embedded data, no virtual channel switching.
  - Stream-on / stream-off only; no run-time reconfig.
- BPMP clocks needed: `TEGRA234_CLK_NVCSI`,
  `TEGRA234_CLK_NVCSILP`, plus reset deassert.

### 4. VI driver

- The Video Input engine. Reads NVCSI frames, DMAs them into DRAM.
- **Even less documented than NVCSI.** Practical reference:
  L4T `drivers/media/platform/tegra/vi/vi5.c` for Orin (Tegra234 is
  the "VI5" generation).
- Scope:
  - Single channel, single capture.
  - No ISP pipeline; raw Bayer straight to memory.
  - Programmed I/O for a single full-frame DMA (no ring, no double
    buffer) — sufficient for "snap one picture."
  - Frame-done interrupt on the GIC, drives a wait-for-completion
    primitive.
- DMA target: a contiguous identity-mapped DRAM region allocated up
  front. **Does not** require a working SMMU translation if the
  region is configured for bypass — same trick already used for NC
  scheduler queues (see `kernel/CLAUDE.md` §"Non-Cacheable Shared
  Memory"). If SMMU bypass is not possible for VI, see Risk 3.
- BPMP clocks: `TEGRA234_CLK_VI`, plus power-domain
  `TEGRA234_POWER_DOMAIN_VIC`.

### 5. Image preprocessing → MNIST input

- Output of capture: ~2.5 MB of RAW10 Bayer data.
- Output expected by `slm.model_infer_bytes`: 3 136 bytes
  (`28 * 28 * 4`-byte fp32).
- Pipeline:
  1. **Crop** a centered square (e.g. 1232×1232) from the 1640×1232
     Bayer frame.
  2. **Bayer → grayscale**: take the green channels only (50% of
     pixels) and average each 2×2 block — much cheaper than full
     demosaic and visually adequate for digit recognition.
  3. **Resize** to 28×28 by box-averaging.
  4. **Normalize**: convert uint8 → fp32 in `[0.0, 1.0]`. The MNIST
     model expects mean ~0.13, std ~0.31 normalization; clarify with
     the runtime team whether the embedded model wants raw `[0, 1]`
     or the normalized form.
  5. **Optional inversion**: MNIST digits are white-on-black; if the
     scene is a black digit on a light background, invert.
- All in fixed-point or naive fp32 — no NEON, no SIMD, runs on the
  capture-completion task.

### 6. Lua bindings + demo

- Module: `slm.camera`.
  ```lua
  local cam = slm.camera.open(name)            -- returns a userdata or nil
  cam:capture()                                 -- returns frame buffer, fp32 bytes, or nil
  cam:close()
  slm.camera.preprocess_mnist(frame_bytes,      -- separate so unit tests can hit it
                              width, height,
                              bayer_pattern)
                              → 3136 bytes (28*28*4 fp32)
  ```
- Component name: built-in component `imx219_camera` with state
  init/capture/teardown, similar to existing components.
- Demo script: `scripts/camera_mnist_demo.lua` — embedded via
  `.incbin` like the other demos.

---

## Effort Estimates

Engineering days, solo focused work. **Conditioned on the CBB Phase 0
recon coming back green.** If NVCSI or VI are blocked, see "Fallback
Paths" below.

| Piece | Low | High | Notes |
|-------|-----|------|-------|
| Phase 0: CBB recon — peek NVCSI + VI + I²C MMIO from EL2 | 1d | 2d | Single shell session: `mem peek <addr>` from running kernel; no driver work. Outputs go/no-go. |
| Tegra HSI2C driver | 4d | 6d | Polled-only, no IRQ, single bus. Linux ref. ~1k lines; SLM-OS port maybe 400. |
| IMX219 sensor driver | 3d | 5d | Reset GPIO + chip-id verify + mode table + stream-on. The mode table is the bulk; comes from Linux. |
| NVCSI driver | 7d | 12d | Undocumented. Code-read of L4T `nvcsi.c` (~2k lines) + cross-reference Orin TRM is the long tail. |
| VI driver (single-shot capture) | 8d | 12d | Even less documented. SMMU bypass investigation could blow up scope; see Risk 3. |
| Image preprocessing | 2d | 3d | Pure compute. CPU is fine. |
| Lua bindings + component wrapper | 1d | 2d | Mirrors existing component patterns. |
| Demo script + embedded launcher | 0.5d | 1d | One Lua file via `.incbin`. |
| Hardware bring-up iteration buffer | 4d | 8d | "It boots clean three times in a row" iteration. |
| QEMU-side regression tests | 2d | 3d | Mock camera path so the preprocessing + Lua bindings have CI coverage even on QEMU. |

**Totals**

- **Realistic happy path: 32–54 d (~7–11 weeks elapsed).** Most of
  the variance is in NVCSI + VI bring-up time, which is the
  reverse-engineering-heavy portion.
- **If Phase 0 reveals NVCSI or VI is CBB-blocked:** see fallbacks.
  The full bare-metal driver path collapses to "not viable from
  NS EL2 without bootloader BCT changes."

For a capstone project that's already in late stages, **this is a
multi-month commitment** — comparable in scope to the GA10B GPU
compute bring-up (#356) or the dynamic-kernel-replace plan
(#369-family).

---

## Risks

### Risk 1 — CBB firewall on NVCSI / VI / camera I²C

**Question:** can SLM-OS at NS EL2 read and write the NVCSI MMIO
window (~`0x15a00000`), the VI MMIO window (~`0x15c00000`), and the
HSI2C controller the camera connector multiplexes onto?

- The CBB firewall blocks on a per-master / per-peripheral basis. The
  pattern observed so far: peripherals Linux uses at NS EL1 are
  generally inherited at NS EL2-VHE, but with exceptions (UARTA is
  blocked despite Linux using it). NVCSI and VI are "regular"
  peripherals (no secure-only role), but neither has been probed.
- A CBB-blocked access produces a RAS Uncorrectable Error and EL3
  powers off the offending CPU core. Recon must therefore use a
  read-only `peek` on a single address, not a write or a register
  scan.
- Mitigation if blocked:
  - **Option B** (Linux pre-kexec capture): Linux captures a frame
    via its existing camera stack, parks it at a known DRAM
    address; SLM-OS reads the buffer post-kexec and runs inference.
    Loses "live capture from SLM-OS" but salvages the demo.
  - **Option C** (USB UVC camera): plug a USB webcam into the
    Jetson xHCI host port. Requires fixing the residual
    SMMU-at-kexec issue (#266 / #285) and writing a UVC class
    driver. Different — and probably larger — scope.
- Phase 0 must run before any bring-up code is written.

**Hardware required:** yes, a one-shot peek session.

### Risk 2 — NVCSI / VI documentation

**Question:** is the L4T source code enough to bring up NVCSI and VI,
or are there opaque firmware steps (RTCPU on Orin, for example) that
SLM-OS would also have to drive?

- The Orin RTCPU (Camera RTCPU, runs on a Cortex-R5 inside the
  SoC) participates in capture under L4T, communicating with the
  CPU side via IVC. Whether single-shot capture in
  bypass-RTCPU mode is supported is **not** documented publicly.
- Mitigation:
  - Code-read the `nvcsi-rtcpu.c` and `vi5-rtcpu.c` sources before
    committing; identify whether RTCPU is a hard dependency or a
    smarter-faster path.
  - Worst case: SLM-OS would need an IVC-over-HSP path to RTCPU,
    similar to the existing BPMP IVC. Doable, but adds 1–2 weeks.

**Hardware required:** no; code-read can answer it.

### Risk 3 — SMMU translations for VI DMA

**Question:** can VI's DMA writes target a DRAM buffer with no SMMU
translation in place, or does the SMMU forcibly drop transactions
from the VI stream-id?

- `arm-smmu` is the same block that has caused #266 (USB networking)
  trouble. The SMMU on Orin is configured by Linux at boot; SLM-OS
  inherits whatever streams Linux had translated, and Linux's kexec
  path can drop translations.
- For VI specifically, two scenarios:
  - **Stream-bypass mode**: SMMU passes VI transactions
    untranslated. If Linux sets this up for the VI stream-id (as it
    does for some accelerators), SLM-OS gets DMA "for free" into any
    physical address.
  - **Translated**: SMMU has a translation for VI; if Linux didn't
    leave a usable translation in place, VI writes drop and the
    capture silently produces zeros.
- Mitigation:
  - Code-read Linux DT to determine VI's `iommus =` property and
    SMMU stream-id.
  - Probe at runtime: capture into a known buffer, check whether the
    pattern landed.
  - If translated and broken: scope an SMMU programming layer,
    similar to what #266 chose to mothball. Significant.

**Hardware required:** partial — code-read scopes it, hardware
verifies.

### Risk 4 — Camera physical setup

**Question:** is an IMX219-160 actually wired to a Jetson dev kit in
the lab, and which connector / I²C bus / GPIO assignment?

- Memory entry on `jetson_lab_setup` does not mention a camera
  module. Coordinate with the lab maintainer (John) before any
  hardware Phase work begins.
- Mitigation: a $25 module + $5 ribbon cable arrives in a few days
  if the lab does not already have one. Connector choice (J17 vs
  J20) determines I²C bus and GPIO pins.

**Hardware required:** trivially yes.

### Risk 5 — Linux pre-kexec camera state

**Question:** does the Linux kernel that hands off to SLM-OS leave
the camera, NVCSI, VI, and HSI2C clocks running, or does it gate them
when no userspace is using them?

- Headless boot is the SLM-OS-on-Jetson default; nothing is opening
  `/dev/video0` before kexec. Most Linux platform drivers
  runtime-suspend their hardware when there are no users, so the
  camera subsystem is **probably gated** at kexec time.
- BPMP control gives SLM-OS the ability to enable clocks itself, so
  this is more an "extra steps" concern than a hard blocker.
- Mitigation:
  - Either probe-and-enable each clock from SLM-OS via the existing
    BPMP driver (preferred), or
  - Pre-kexec hold each required clock via a Linux script, the same
    pattern used for `slmos-kexec` GPU-suspend
    (`scripts/jetson-kexec-slmos.sh`).

**Hardware required:** yes for end-to-end verification.

---

## Pre-Hardware Tasks

Items that can land before Phase 0 hardware probing.

- ☐ Add Tegra234 clock-id and reset-id constants to a new
  `kernel/include/tegra234_clocks.h`, sourced from Linux's
  `dt-bindings/clock/tegra234-clock.h`. At minimum: I2C controllers,
  NVCSI, NVCSILP, VI, VI memory clock, plus matching reset IDs.
- ☐ Code-read NVCSI driver: pin which registers must be programmed,
  in what order, and whether RTCPU IVC is required for single-shot
  capture.
- ☐ Code-read VI driver: same, plus identify SMMU stream-id and DMA
  setup expectations.
- ☐ Code-read IMX219 driver: extract the mode register table for
  2-lane RAW10 1640×1232 mode into a header.
- ☐ Identify the camera I²C bus, reset/PWDN GPIO assignments, and
  XCLK source from
  `arch/arm64/boot/dts/nvidia/tegra234-p3768-0000+p3767-0005.dts`
  (and any IMX219 DT overlay that ships with L4T).
- ☐ Confirm with the lab whether an IMX219-160 module is on hand and
  on which connector it lives (J17 / J20).
- ☐ Cache reference sources under `docs/reference/`:
  Linux `imx219.c`, `i2c-tegra.c`, `nvcsi.c`, `vi5.c` for offline
  read.

## Phase 0 — Hardware Recon (CBB Probe)

Before writing any driver code, the single most informative experiment
is a one-shot MMIO peek. Run on `jetson-nano-2` (or `nano-1`) via the
existing `mem peek <addr>` shell command:

- ☐🔗 `mem peek 0x15a00000` (NVCSI base — first 32-bit word).
  Expected on EL2 access if reachable: a Tegra HW revision register
  or `0x0`. A CBB-blocked access will RAS-fault and power off the
  CPU.
- ☐🔗 `mem peek 0x15c00000` (VI base).
- ☐🔗 `mem peek 0x031c0000` (HSI2C-3 — example camera bus; address
  TBD from DT).
- ☐🔗 If any of the above blocks: stop and switch to the fallback
  evaluation phase. If all three return data: proceed with the
  driver stack.

## Hardware Tasks (post-Phase-0)

Blocked until Phase 0 returns green.

- ☐🔗 Implement `i2c_init` + `i2c_write_reg16` against the camera
  bus. Verify by reading IMX219 CHIP_ID at register `0x0000`
  (expected: `0x0219`).
- ☐🔗 Implement IMX219 `init` + `stream_on`. Verify with a logic
  analyzer or the carrier's CSI status that the sensor is producing
  HSYNC/VSYNC.
- ☐🔗 Implement NVCSI receiver. Verify with internal counters that
  packets are arriving on the configured port.
- ☐🔗 Implement VI single-shot capture. Verify by hashing the
  captured buffer; the hash must change between two captures of
  different scenes.
- ☐🔗 Implement Lua bindings + preprocessing. Verify by capturing a
  known printed digit and asserting that
  `slm.model_infer_bytes` returns the expected class.
- ☐🔗 End-to-end demo: `lua /scripts/camera_mnist_demo.lua` outputs
  the correct prediction for a held-up digit.

## QEMU-Side Tasks (CI coverage without hardware)

Items that can run on QEMU alongside the existing test suite, so the
non-hardware portions don't regress between bring-up sessions.

- ☐ Add a mock camera component that returns a fixed 1640×1232 RAW10
  buffer drawn from a baked-in test image. `slm.camera.open("mock")`
  resolves to it.
- ☐ Unit-test `preprocess_mnist`: pin the output bytes of the mock
  image and assert the MD5 across runs.
- ☐ Lua-level integration test: open mock camera, capture, preprocess,
  feed to the embedded MNIST CPU path (no GPU required), assert
  predicted class.

---

## Fallback Paths (If CBB Blocks Phase 0)

If NVCSI or VI is firewalled at NS EL2, the bare-metal-camera path
is not viable from SLM-OS as currently architected. Two pragmatic
alternatives.

### Fallback A — Pre-kexec frame snapshot

- Linux captures a frame via its existing GStreamer / V4L2 stack
  before kexec.
- The frame is parked at a known physical address (a chunk of the
  kexec reserved-mem region, similar to how `slmos-kexec` already
  preserves boot args).
- SLM-OS reads the buffer post-kexec, runs preprocessing + inference.
- **Effort**: 1–2 weeks (mostly the Linux-side capture script, tiny
  SLM-OS-side reader).
- **Demo cost**: not "live" — only a single pre-baked frame per boot.
  Still demonstrates the inference half of the pipeline, but the
  "press a button to capture" interaction is gone.

### Fallback B — USB UVC camera

- USB webcam → Jetson xHCI host → UVC class driver in SLM-OS.
- Requires #266 (`Jetson USB networking`) Phase 3A or equivalent
  XHCI DMA path to be working post-kexec — currently mothballed
  due to the SMMU-at-kexec blocker (`docs/jetson-usb-networking-plan.md`
  §8).
- **Effort**: large. UVC class driver is ~2 000 lines of C; xHCI
  DMA fix is itself an open question.
- **Tradeoff**: no IMX219-specific work, but adds USB stack
  complexity.

### Fallback C — Skip live capture, keep the demo

- Embed a small fp32 sample image in the kernel (the M10 demo
  already has 10 MNIST test digits embedded as `digit_N.bin` under
  `/mnt/files/digits/`).
- The "demo" reads a digit file and runs inference. No camera at all.
- **Effort**: zero — already exists.
- **Demo cost**: not actually a camera demo. Reframes the deliverable.

---

## Open Questions

- Which J17 / J20 connector is the lab's IMX219 (if any) installed
  on, and what does the corresponding DT overlay name?
- Is RTCPU (Camera-RTCPU on the Orin Cortex-R5 cluster) a hard
  dependency for single-shot capture, or only for the streaming
  + ISP-pipeline path?
- Does Linux's idle (no `/dev/video0` open) state leave NVCSI / VI
  power-gated? If so, BPMP `pg_set_state` must run before clock
  enables.
- What's the SMMU stream-id assignment for VI on Tegra234, and is it
  in bypass mode by default?
- Does the existing M10 MNIST handoff bundle accept normalized fp32
  in `[0, 1]`, or does it want the standard
  `(x − 0.1307) / 0.3081` MNIST normalization? `runtime/src/lib.rs`
  in the inference path will answer this.
- Does the demo want a single capture-and-report, or a continuous
  classify-the-digit-in-front-of-the-camera loop? The plan above
  scopes single-shot; continuous adds VI ring-buffer work
  (~1 week extra).

---

## Recommended Sequencing

1. **Land Phase 0 first** as a 1–2 day investigation issue. The
   answer to "can SLM-OS at EL2 even read NVCSI and VI?" decides
   whether to proceed with this plan, fall back to A/B, or reframe
   with C.
2. If green: prioritize the QEMU-side mock-camera + preprocessing
   first. That delivers a runnable demo on QEMU within a week and
   de-risks the Lua / inference plumbing before any Tegra
   reverse-engineering work begins.
3. Tegra HSI2C + IMX219 next. These can be developed and verified
   in isolation (CHIP_ID readback is the gate) without NVCSI or VI.
4. NVCSI + VI last, behind their own iteration buffer because of the
   reverse-engineering tail.

---

## References

### Internal

- `docs/jetson-cbb-report.md` — CBB firewall envelope and recon
  pattern.
- `docs/jetson-bpmp-ipc-plan.md` + `kernel/drivers/bpmp/` — BPMP
  MRQ stack used for clock / reset / power control.
- `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory" — DMA-friendly
  memory carveout pattern that VI may be able to reuse.
- `runtime/` + `slm.model_infer_bytes` (PR #386, `886dd0d`) —
  inference-side contract that camera frames feed into.

### External

- Sony IMX219PQ datasheet (public mirrors).
- Linux `drivers/media/i2c/imx219.c` — sensor driver,
  rpi-6.12.y / mainline.
- Linux `drivers/i2c/busses/i2c-tegra.c` — I²C controller.
- L4T `drivers/media/platform/tegra/csi/nvcsi*.c` — NVCSI driver.
- L4T `drivers/media/platform/tegra/vi/vi5*.c` — VI capture engine.
- Orin TRM (developer-login required; local copy referenced in
  project root `CLAUDE.md`).
- Tegra234 DT bindings:
  `Documentation/devicetree/bindings/media/tegra*.yaml` and
  `dt-bindings/clock/tegra234-clock.h`.
