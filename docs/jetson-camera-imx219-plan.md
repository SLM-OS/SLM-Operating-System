# Jetson Camera IMX219-160 + MNIST Demo Plan

**Tracking:** 🎫 #396

**Status:** ✅ Phase 0 hardware recon **GREEN** (jetson-nano-1, 2026-04-25): NVCSI MMIO, RCE HSP, and the camera I²C bus are all reachable from NS EL2; RCE is actively running and quiescent (R5 in WFI), so SLM-OS inherits a usable camera RTCPU post-kexec. IMX219 module physically attached to connector A (J17) and verified working under Linux. Hardware Task 1 (Tegra HSI2C driver) landed and verified — controller init / packet xfer all clean. End-to-end CHIP_ID readback is queued behind Hardware Task 2 (IMX219 sensor driver) because the sensor needs a power-up sequence (XCLK + reset GPIO) SLM-OS doesn't drive yet.

**Progress:** 16 / 21 tasks complete.

| Section | ✅ done | ☐ open | ☐🔗 blocked | ⏸️ deferred |
|---------|--------|---------|-------------|-------------|
| Pre-Hardware Tasks | 8 | 0 | 0 | 0 |
| Phase 0 — Hardware Recon | 4 | 0 | 0 | 0 |
| Hardware Tasks (post-Phase-0) | 1 | 5 | 0 | 0 |
| QEMU-Side Tasks | 3 | 0 | 0 | 0 |
| **Total** | **16** | **5** | **0** | **0** |

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
- `docs/archive/plans/jetson-bpmp-ipc-plan.md` + commits `6615da5`/`1f261e2` —
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
- **No NVIDIA programmer's manual.** The programming sequence comes
  from the L4T sources cached at `docs/reference/l4t-csi*.c` /
  `docs/reference/l4t-nvcsi*.c`. Distilled into
  `docs/jetson-camera-nvcsi-driver-notes.md`, including the 20-step
  direct-MMIO bring-up sequence and the RTCPU IPC fallback path.
- **Two viable architectures** (per the code-read):
  - **Option A — Direct MMIO** (preferred): use the dead-but-still-
    valid `csi4_fops.c` sequence from L4T. The hardware register
    layout is unchanged from T194 to T234 even though L4T R35 routes
    everything through RTCPU. No new IPC stack required. *Gated on
    Phase 0 confirming NS EL2 can reach `0x15A00000`.*
  - **Option B — RTCPU IVC** (fallback): two-message setup
    (`CAPTURE_PHY_STREAM_OPEN_REQ` +
    `CAPTURE_CSI_STREAM_SET_CONFIG_REQ`) over a new HSP-based IVC
    channel pair. Adds a camera-rtcpu IPC layer on top of the existing
    BPMP IVC. Shares the same transport SLM-OS would need for VI
    (which has no Option A — see §4 below).
- Scope (both options):
  - Single CSI port (whichever J17 / J20 connector the camera is on).
  - 2 data lanes + 1 clock lane.
  - RAW10 datatype, no embedded data, no virtual channel switching.
  - Stream-on / stream-off only; no run-time reconfig.
- BPMP clocks needed: `TEGRA234_CLK_NVCSI` only — there is no
  separate `NVCSILP` clock on T234 despite the upstream binding
  defining the symbol. Reset is **per-CIL** via the
  `NVCSI_CIL_*_SW_RESET` MMIO registers themselves; the L4T DT does
  not consume `TEGRA234_RESET_NVCSI` (the symbol exists in
  `kernel/include/tegra234_clocks.h` but is unused today).

### 4. VI driver

- The Video Input engine. Receives NVCSI frames, DMAs them into DRAM.
- **VI5 is RTCPU-only on T234.** The code-read of L4T `vi5_fops.c`
  (cached at `docs/reference/l4t-vi5_fops.c`, distilled in
  `docs/jetson-camera-vi-driver-notes.md`) confirms there is **no
  AP-programmable register interface** for VI5: zero `request_irq`,
  zero MMIO peeks. Every operation (`CAPTURE_CHANNEL_SETUP_REQ`,
  `CAPTURE_REQUEST_REQ`, `CAPTURE_STATUS_IND`) is an IVC round-trip
  to the camera RTCPU (RCE), which drives the VI Falcon microcode.
  The AP only sees mailbox messages and the descriptor memory it
  shares with RCE.
- **Consequence**: the original "program VI MMIO at ~0x15c00000"
  scope collapses into IVC + descriptor construction work — same
  shape as the existing BPMP IVC path. Only viable architecture is
  via the camera-rtcpu IVC channel.
- Scope:
  - Single channel, single capture.
  - No ISP pipeline; raw Bayer straight to memory.
  - Single-shot capture descriptor submitted to RCE; one
    `CAPTURE_STATUS_IND` returned on frame-done.
  - **Completion is an HSP shared-mailbox doorbell SPI**, not a VI
    peripheral IRQ on the GIC. Same family of interrupt SLM-OS
    already handles for BPMP IVC.
- DMA target: a contiguous DRAM region. The IOVA in the capture
  descriptor is resolved by RCE through the **camera-rtcpu's** SMMU
  domain, not VI's own stream-id (see Risk 3 for the reframed SMMU
  question). The non-cacheable carveout pattern in
  `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory" still applies for
  the descriptor memory shared with RCE.
- BPMP clocks: `TEGRA234_CLK_VI`, plus power-domain
  `TEGRA234_POWER_DOMAIN_VI` (id 28). The original draft listed
  `TEGRA234_POWER_DOMAIN_VIC` — that's the Video Image Compositor,
  not Video Input. Both `kernel/include/tegra234_clocks.h` and the
  notes doc use the corrected name.

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
recon coming back green.** Revised April 2026 after the NVCSI / VI /
IMX219 code-reads — VI shrunk significantly (no AP MMIO; collapses
into IVC + descriptor work), camera-rtcpu IVC added as a new
load-bearing piece, and the QEMU-side rows are now done.

| Piece | Low | High | Notes |
|-------|-----|------|-------|
| Phase 0: CBB recon — peek NVCSI + camera-rtcpu HSP + I²C MMIO from EL2 | 1d | 2d | Targets revised — see §"Phase 0". Decision matrix selects NVCSI Option A vs B. |
| Camera-rtcpu IVC layer | 4d | 8d | NEW. Surfaced by the VI code-read: VI is RTCPU-only and NVCSI Option B is also via RTCPU. Bridge the existing BPMP IVC pattern to the `tegra-camera-rtcpu` HSP channel pair + capture wire format (`CAPTURE_*_REQ` / `CAPTURE_STATUS_IND`). Shared between NVCSI Option B and VI. |
| Tegra HSI2C driver | 4d | 6d | Polled-only, no IRQ, single bus. Linux ref `i2c-tegra.c` cached; SLM-OS port ~400 lines. |
| IMX219 sensor driver | 2d | 4d | Down from 3-5d — the 1640×1232 mode register table is already extracted in `docs/jetson-camera-imx219-driver-notes.md`, ready to drop into a header. |
| NVCSI Option A (direct MMIO) | 5d | 8d | The 20-step direct-MMIO sequence is in `docs/jetson-camera-nvcsi-driver-notes.md`. Down from 7-12d because the code-read is done. Conditional on Phase 0 NVCSI MMIO being reachable. |
| NVCSI Option B (via RTCPU) — fallback | 2d | 4d | Two IVC messages on top of the camera-rtcpu IVC layer above. Used iff Option A is blocked at Phase 0. |
| VI driver (via RTCPU) | 3d | 6d | Down from 8-12d. Capture descriptor + `CAPTURE_REQUEST_REQ` / `CAPTURE_STATUS_IND` round-trip on the camera-rtcpu IVC layer. Was over-scoped under the (wrong) assumption of AP-programmable VI MMIO. |
| Image preprocessing | ✅ | ✅ | Landed in #396 follow-up (`kernel/src/camera.c`). |
| Lua bindings + component wrapper | ✅ | ✅ | Landed in #396 follow-up (`slm.camera.*` in `kernel/src/lua_slm.c`). |
| Demo script + embedded launcher | 0.5d | 1d | One Lua file via `.incbin`. The QEMU e2e test (`test_slm_camera_e2e_mnist_mock`) plays the same role for CI; this row is for the user-facing demo wrapper. |
| Hardware bring-up iteration buffer | 4d | 8d | "It boots clean three times in a row" iteration on the lab Jetson. |
| QEMU-side regression tests | ✅ | ✅ | Landed in #396 follow-up. |

**Totals**

- **Realistic happy path (NVCSI Option A): 23.5–43 d (~5-9 weeks
  elapsed).** Down from 32-54d, mostly because VI shrunk by 5-6d and
  the QEMU-side rows are already done.
- **NVCSI Option B (CBB blocks NVCSI MMIO but not the camera-rtcpu
  HSP region): 20.5–39 d.** Slightly faster than Option A because the
  IVC two-message setup is smaller than the 20-step direct-MMIO
  sequence — though the savings are eaten by the iteration cost of
  debugging IVC over a fresh transport.
- **If Phase 0 reveals the camera-rtcpu HSP region is CBB-blocked:**
  see fallbacks. Both NVCSI Option B and VI become unreachable, and
  the bare-metal-capture path collapses entirely.

For a capstone project that's already in late stages, **this is
still a multi-week commitment** but no longer multi-month — the
research pass cut about 9-11 days off the high estimate by digesting
NVCSI / VI / IMX219 in advance and revealing VI's actual scope.

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

### Risk 3 — SMMU translations for camera-rtcpu DMA

**Reframed by the VI code-read (April 2026).** The original Risk 3
asked about VI's own SMMU stream-id. Because VI5 is RTCPU-only on
T234 (see §"VI driver"), the relevant stream-id is the **camera
RTCPU's**, not VI's. RCE resolves capture-buffer IOVAs through its
own SMMU domain via `dma_buf_attach(buf, rtcpu_dev)` and then issues
the bus-master transaction; VI hardware DMAs through a stream-id that
aliases the same translation table.

**Question:** can the camera-rtcpu's SMMU stream resolve IOVAs for a
SLM-OS-supplied buffer post-kexec — either via inherited translation
or stream-bypass?

- `arm-smmu` is the same block that has caused #266 (USB networking)
  trouble. The SMMU on Orin is configured by Linux at boot; SLM-OS
  inherits whatever streams Linux had translated, and Linux's kexec
  path can drop translations.
- Two scenarios for the camera-rtcpu stream:
  - **Stream-bypass mode**: SMMU passes RCE transactions
    untranslated. SLM-OS gets DMA "for free" into any physical
    address. There is a fallback path in L4T's `capture-common.c:611`
    that uses `sg_phys` when `sg_dma_address == 0`, which proves
    stream-bypass is a real configuration RCE accepts.
  - **Translated**: SMMU has a translation for RCE; if Linux didn't
    leave a usable translation in place at kexec, the IOVAs SLM-OS
    submits in the capture descriptor will fault.
- Failure mode: SMMU drops writes (zeroed buffer) **and** raises a
  context-bank fault SPI to the AP. So the failure is observable —
  not silent corruption.
- Mitigation:
  - The L4T DT `iommus =` property for `tegra-camera-rtcpu` pins the
    stream-id; needs a runtime probe of the SMMU stream table at
    `0x12000000` to confirm what's configured post-kexec (the L4T BSP
    DT lives in a separate tarball not in the OE4T mirror).
  - Probe at runtime: capture into a known buffer, check whether the
    pattern landed; watch for the SMMU fault SPI in parallel.
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

- ✅ Tegra234 clock and reset IDs ported from Linux's
  `dt-bindings/clock/tegra234-clock.h` to
  `kernel/include/tegra234_clocks.h` (camera-relevant subset only:
  every I²C controller, NVCSI / NVCSILP, VI / VI2, plus VI and ISPA
  power-domain IDs). Three gotchas captured in the header comments:
  I2C5 has no `MRQ_RESET` pair (BPMP-internal CAM_I2C); NVCSILP
  shares the NVCSI reset; no `VI_M` clock exists upstream
  (`VI_CONST` is the closest match).
- ✅ Code-read NVCSI driver — see
  `docs/jetson-camera-nvcsi-driver-notes.md`. Headline: L4T R35
  routes 100% through Camera RTCPU IVC, but the dead `csi4_fops.c`
  direct-MMIO sequence is still valid since the register layout is
  unchanged T194→T234. Two viable architectures (Option A direct
  MMIO, Option B via RTCPU); choice gated on Phase 0.
- ✅ Code-read VI driver — see
  `docs/jetson-camera-vi-driver-notes.md`. Headline: VI5 has **no**
  AP-programmable register interface on T234; everything is RTCPU
  IVC. Reframed Risk 3 (SMMU question is about camera-rtcpu's
  stream-id, not VI's). Reframed Phase 0 recon targets.
- ✅ Code-read IMX219 driver — see
  `docs/jetson-camera-imx219-driver-notes.md`. Mode tables are
  computed on the fly in Linux, not static; the notes doc translates
  the 1640×1232 RAW10 binned-mode sequence into a copy-pastable C
  array. Flags two non-obvious quirks: 12 mandatory "undocumented
  registers" at 0x4540-0x479b, and a probe-time MODE_SELECT toggle
  the D-PHY needs before it'll enter LP-11.
- ✅ Identify the camera I²C bus, reset/PWDN GPIO assignments from
  the live `jetson-nano-1` device tree (2026-04-25 SSH session).
  Findings:
  - **Camera I²C bus = `0x03180000`** (Linux alias `i2c2`, DT label
    `cam_i2c`, `nvidia,tegra194-i2c`). Plan previously guessed
    `0x031c0000`/HSI2C-3 — that was wrong; HSI2C-3 is disabled.
  - Both connectors (CAM-A / CAM-C in L4T overlay names) **share the
    same I²C bus through a GPIO-controlled MUX** (compatible
    `i2c-mux-gpio`, MUX selector on AON GPIO line 19). So the I²C
    answer is identical regardless of physical connector choice.
  - Reset GPIOs differ per connector: CAM-A on main GPIO line 62,
    CAM-C on main GPIO line 160. Active-high.
  - NVCSI port assignments differ: CAM-A on port-index 1
    (`serial_b`), CAM-C on port-index 2 (`serial_c`). 2-lane RAW10.
  - L4T already ships pre-built overlays in `/boot/`:
    `tegra234-p3767-camera-p3768-imx219-{A,C,dual,...}.dtbo` —
    just need extlinux to load one once a camera is attached.
  - RCE HSP base **`0x0B950000`** (not the previously guessed
    `~0x03c00000` — that's BPMP HSP). RCE main MMIO at `0x0BC00000`,
    RCE PM at `0x0B9F0000`.
- ✅ IMX219 module attached to `jetson-nano-1` connector A (J17 /
  CAM0) verified working under Linux 2026-04-25. dmesg confirms
  `imx219 9-0010: tegracam sensor driver:imx219_v2.0.6` and
  `tegra-camrtc-capture-vi: subdev imx219 9-0010 bound`; `/dev/video0`
  is exposed. The Linux IMX219 driver wouldn't have probed without a
  successful CHIP_ID read at I²C address 0x10, so the hardware path
  end-to-end (carrier wiring → MUX → I²C → sensor) is good.

  **Operational note** — on JetPack 5+ (R35.x / R36.x) the older
  `FDTOVERLAYS` extlinux directive is silently ignored. Use
  `/opt/nvidia/jetson-io/config-by-hardware.py -n 2='Camera IMX219-A'`
  instead — it writes a separate `LABEL JetsonIO` block with the
  supported `OVERLAYS` directive and bumps the `DEFAULT` to it.
  Reboot to apply. (The lab nano-1 is now configured this way, so the
  overlay loads automatically every boot until reverted.)
- ✅ Code-read camera-rtcpu IVC bring-up — see
  `docs/jetson-camera-rtcpu-ivc-driver-notes.md`. Headlines:
  - HSP wire format is shared-mailbox + shared-semaphore (not the
    doorbell pattern BPMP uses); needs ~250 LoC of new SM TX/RX/SS
    accessors plus a `CAMRTC_HSP_MSG` request/response state machine
    (HELLO / PROTOCOL / RESUME / CH_SETUP).
  - IVC ring layout itself is identical — SLM-OS's
    `kernel/drivers/bpmp/ivc.c` lifts in directly.
  - RCE firmware is bootloader-loaded; SLM-OS doesn't need to
    `request_firmware`. AST regions are bootloader-programmed too.
  - Default IVC topology is 1 region, 6 channels; the two
    load-bearing for IMX219 are `ivccontrol@3` (capture-control,
    64×320 B) and `ivccapture@4` (capture, 512×64 B).
  - Total port estimate: **~600-900 LoC new** + reuse of existing
    `bpmp/ivc.c` and `bpmp/hsp.c`.
- ✅ Cache reference sources under `docs/reference/`:
  Linux `imx219.c`, `i2c-tegra.c`, the L4T `csi*.c` / `nvcsi*.c` /
  `vi5*.c` files, and the camera-rtcpu IVC headers
  (`camrtc-capture*.h`). 24 files cached during the four-agent code-
  read pass.

## Phase 0 — Hardware Recon (CBB Probe)

**Run on `jetson-nano-1` 2026-04-25. All three primary targets
returned data; CBB does not block any of them at NS EL2.** Decision-
matrix outcome: NVCSI Option A (direct MMIO) + VI via RTCPU + IMX219
sensor — smallest scope.

Peek targets and observed values (each measured one-shot via
`peek <addr>` from the SLM-OS shell, with the addresses identity-
mapped in `kernel/mm/vmm.c` under the `#396 Phase 0` block):

- ✅ `peek 0x15A00000` (NVCSI base) → `0xFFFFFFFF`. Read completed
  cleanly (no exception, no CBB external abort). The all-1s value is
  expected: the L4T platform driver leaves NVCSI clock-gated when no
  camera overlay is loaded, and clock-gated MMIO returns 0xFFFFFFFF.
  **NVCSI MMIO is reachable from NS EL2.** Future driver code will
  enable `TEGRA234_CLK_NVCSI` via BPMP before reading live values.
- ✅ `peek 0x0B950380` (RCE HSP_DIMENSIONING) → `0x00080048`.
  Decoded per `tegra186-hsp.c` field layout: 8 doorbells, 4 shared
  mailboxes, 8 shared semaphores. Real read of a live RCE HSP
  controller. **RCE HSP is reachable from NS EL2** — gates both
  NVCSI Option B and VI (which both go through the RCE IVC).
- ✅ `peek 0x03180000` (HSI2C-2 = `cam_i2c`) → `0x00022C00`.
  The `I2C_CNFG` register, in the configuration Linux left after
  enabling the controller. **Camera I²C bus is reachable.**
- ✅ Bonus state probes (per the `tegra-camera-rtcpu` notes):
  - `peek 0x0B9F0040` (RCE_PM R5_CTRL) → `0x00000002` — bit 1 set
    means the Camera RTCPU R5 cluster is **actively running**.
  - `peek 0x0B9F0020` (RCE_PM PWR_STATUS) → `0x04600000` — bit 21
    set means the R5 is in WFI (idle, waiting for events).
  - SLM-OS will inherit a live and quiescent RCE post-kexec —
    no firmware load required, no R5 reset required.

**Outcome summary**: every gating address is reachable, and the
Camera RTCPU is in the ideal state for SLM-OS to attach to its IVC
without re-bringup. The plan proceeds with **NVCSI Option A +
VI-via-RTCPU + IMX219**; the Fallback Paths section below is now
dormant.

## Hardware Tasks (post-Phase-0)

Phase 0 is GREEN; tasks are unblocked.

- ✅ Tegra HSI2C driver — see `kernel/include/i2c_tegra.h` and
  `kernel/drivers/i2c/i2c_tegra.c`. Polled, no-IRQ, no-DMA, single-
  master, packet-mode. Public API: `tegra_i2c_init` /
  `tegra_i2c_write_reg16` / `tegra_i2c_read_reg16`. Pre-configured
  `tegra_i2c_cam_bus` instance pinned to TEGRA234_CAM_I2C_BASE +
  TEGRA234_CLK_I2C2 + TEGRA234_RESET_I2C2.

  **Verification status**: controller path is verified end-to-end on
  jetson-nano-1 (`tegra_i2c_init` returns rc=0; packet transactions
  complete cleanly with no controller wedge). The `imx219` shell
  command exercises `tegra_i2c_read_reg16` against the IMX219 sensor
  at I²C 0x10. The CHIP_ID = 0x0219 gate is **deferred to the next
  task** because it requires the sensor to be powered: Linux's
  tegracam IMX219 driver runtime-suspends the sensor (XCLK off +
  reset GPIO LOW) when no v4l2 client is streaming, and the kexec
  orderly shutdown closes any open v4l2 fd which triggers the same
  teardown. The HSI2C driver itself is sound; what's missing is the
  GPIO + extperiph1 (XCLK) power-up sequence, which properly belongs
  to the IMX219 sensor driver below.
- ✅ IMX219 sensor driver — see `kernel/include/imx219.h` and
  `kernel/drivers/camera/imx219.c`. Full power-up sequence: enable
  extperiph1 XCLK at 24 MHz via BPMP, force PH.06/PCC.03 pinmux to
  GPIO mode, position cam_i2cmux for connector A (channel-0 = LOW),
  release cam_reset (PH.06 HIGH), wait 6.2 ms, init HSI2C, read
  CHIP_ID. Skips cam_pwr (PH.03) — Linux keeps it LOW even during
  active streaming on this carrier so the camera rails are
  always-on; driving it would risk an unrelated side effect.

  Companion: `kernel/include/gpio_tegra.h` + `kernel/drivers/gpio/gpio_tegra.c`
  (per-pin Tegra234 GPIO API + pinmux helper). `kernel/include/bpmp.h`
  picked up `bpmp_clk_set_rate` for the 24 MHz XCLK programming.

  **Verification status (jetson-nano-1, 2026-04-26)**: GREEN.
  `imx219` shell command prints `chip_id = 0x0219` end-to-end after
  kexec-from-Linux, with no Linux-side keepalive (the kexec helper
  doesn't restart the IMX219 v4l2 driver). HSYNC/VSYNC verification
  with a logic analyzer is deferred to Hardware Task 3 / 4 (NVCSI +
  VI bring-up) where the sensor is actually streaming.

  **What unlocked it**: `kernel/drivers/i2c/i2c_tegra.c` was using
  the Tegra210-era legacy FIFO_CONTROL/STATUS at 0x05C/0x060 and the
  0x19 std-mode clock divisor. Tegra194/234 inherits a different
  register set: MST_FIFO_CONTROL/STATUS at 0x0B4/0x0B8, std-mode
  divisor 0x4F, and a mandatory MSTR_CONFIG_LOAD write to
  I2C_CONFIG_LOAD (0x08C) after every CNFG/timing change. Without
  CONFIG_LOAD the controller's bus-timing FSM keeps running on stale
  defaults — packets complete with PACKET_XFER_COMPLETE but the
  slave's response byte never lands in RX_FIFO. See
  `docs/jetson-camera-imx219-driver-notes.md` for the full bug
  story so future Tegra register-set ports don't repeat it.
- ✅ Implement NVCSI receiver via Camera RTCPU IVC.
  - **Option A (direct MMIO) — BLOCKED 2026-04-26.** Hardware
    verification on jetson-nano-1 confirmed NVCSI MMIO is not
    accessible from any AP context: `peek 0x15a00000` returns
    0xFFFFFFFF from both SLM-OS at NS EL2 and Linux at EL1
    (`devmem`), even while gstreamer is actively streaming via the
    Linux camera stack. `bpmp_reset_deassert(TEGRA234_RESET_NVCSI)`
    returns -13 (EACCES). Phase 0's "NVCSI MMIO reachable" was a
    false positive — the architectural reality matches the L4T R35
    code-read: NVCSI is RTCPU-exclusive on T234.
    `kernel/drivers/camera/nvcsi.c` and the `nvcsi` shell command
    are kept as diagnostics that surface the failure mode
    unambiguously. See `docs/jetson-camera-nvcsi-driver-notes.md`
    "Update — 2026-04-26: Option A is blocked" for the evidence.
  - **Option B (Camera RTCPU IVC) — DONE 2026-04-27.** Brought up
    in five PRs:
    1. **PR #441 / #446** — HSP-VM transport
       (`kernel/drivers/camrtc/camrtc.c`). HELLO / PROTOCOL /
       RESUME boot-sync over the rce-hsp shared mailboxes. The
       pre-HELLO BPMP poweron sequence (RCE_CPU_NIC + RCE_NIC +
       RCE_CPU clk_enable + RESET_RCE_ALL deassert) recovers from
       Linux's kexec teardown that asserted the RCE reset and
       gated the rce clocks. Closes issue #438.
    2. **PR #456** — `CAMRTC_HSP_CH_SETUP` capture-control channel
       binding. The CH_SETUP region must lie in RCE's compiled-in
       VM1 IOVA aperture (0xA0000000..0xC0000000); also wires the
       drain-IRQ-before-response logic in `camrtc_send_msg` since
       RCE emits unidirectional IRQ notifications interleaved with
       command responses.
    3. **PR #460** — `kernel/drivers/camrtc/camrtc_ivc.c` tegra-IVC
       ring transport (init / can_send / can_recv / send / recv /
       recv_wait + SS[0]+IRQ-msg notify) + `CAPTURE_PHY_STREAM_OPEN_REQ`
       wrapper in `kernel/drivers/camrtc/camrtc_capture.c`. The
       half-zero-queue-headers + rate-limited-SYNC-handshake gotchas
       are documented in `docs/jetson-camera-rtcpu-ivc-driver-notes.md`.
    4. **PR #461** — `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` wrapper.
       Configures the NVCSI brick + CIL + error masks (D-PHY,
       2 lanes, 456 MHz MIPI clock for the IMX219 default link
       freq).

    `csidiag` shell command on jetson-nano-1 round-trips both
    messages with `rc=0 result=0x0`:

    ```
    capture_init:    rc=0
    PHY_STREAM_OPEN: rc=0 result=0x0
    CSI_SET_CONFIG:  rc=0 result=0x0
    *** NVCSI configured for IMX219 (2-lane D-PHY 456 MHz). ***
    ```

    NVCSI is now configured by RCE for the IMX219 wire. **Counting
    received packets requires VI capture** (NVCSI INTR_STATUS is
    not directly reachable from AP — it's behind the same CBB
    firewall as NVCSI MMIO). The packet-count verification is
    therefore folded into Hardware Task 4 (VI single-shot capture)
    where the captured frame contents are the proof of life.
- ☐🔗 Implement VI single-shot capture. Verify by hashing the
  captured buffer; the hash must change between two captures of
  different scenes.
  - **Wire-format port DONE 2026-04-27.** Four PRs landed the full
    request/response wire format for VI capture:
    1. **PR #469** — extend CH_SETUP TLV array to bind both IVC
       channels (capture-control + capture). Capture channel = 64
       frames × 64 B at `0xBDFEB100` rx / `0xBDFEC180` tx.
    2. **PR #472** — `CAPTURE_CHANNEL_SETUP_REQ` (msg 0x1E) wrapper.
       Ports `capture_channel_config` (272 B body) plus sub-structs
       (`csi_stream_config`, `syncpoint_info`). Verified with a
       smoke-test (queue_depth=0 → INVALID_PARAMETER expected) on
       jetson-nano-1.
    3. **PR #473** — VI request region carveout at `0xBDFD0000`
       (64 KB inside the existing NC mapping). Holds the request
       ring + memoryinfo ring. With real IOVAs, RCE accepts
       CHANNEL_SETUP and assigns a physical VI channel:
       ```
       CHANNEL_SETUP: rc=0 result=0x0 channel_id=0x0 vi_mask=0x800000000
       ```
       (vi_mask bit 35 = VI hardware channel #35 allocated.)
    4. **PR #474** — `CAPTURE_REQUEST_REQ` (msg 0x01) +
       `CAPTURE_STATUS_IND` (msg 0x02) wrappers. Sends over the
       *capture* IVC channel (not capture-control). Verified RCE
       consumes the request and emits its own scheduler/VI
       diagnostic logs over TCU; the wire format is correct end-
       to-end.

  **What remains for actual frame capture** (not yet started):
  1. Port `vi_channel_config` (~352 B with C bitfields — fragile
     for wire-format use). Required so RCE's VI register
     programming finds a valid frame format.
  2. Power-on the IMX219 sensor + write `MODE_SELECT = STREAMING`
     (extend the existing `imx219` shell command).
  3. Allocate a 2.5 MB frame buffer for IMX219 binned-mode RAW10
     (1640×1232) inside RCE's VM1 IOVA aperture. The current 64 KB
     NC carveouts are too small; either grow the NC mapping or
     allocate from a different NC-mapped region.
  4. Set the atomp surface IOVAs in the descriptor's `vi_channel_config`.
  5. Inspect `capture_status.status` field in the descriptor for
     `CAPTURE_STATUS_SUCCESS` after `STATUS_IND` arrives.

  Each is a separate PR. The wire-format port is now complete;
  what's left is the data-path / hardware-config surface.
- ☐🔗 Implement Lua bindings + preprocessing. Verify by capturing a
  known printed digit and asserting that
  `slm.model_infer_bytes` returns the expected class.
- ☐🔗 End-to-end demo: `lua /scripts/camera_mnist_demo.lua` outputs
  the correct prediction for a held-up digit.

## QEMU-Side Tasks (CI coverage without hardware)

Items that can run on QEMU alongside the existing test suite, so the
non-hardware portions don't regress between bring-up sessions.

- ✅ Mock camera backend that returns a fixed 1640×1232 RAW10 buffer
  drawn from a baked-in test image. `slm.camera.open("mock")` resolves
  to it. Source digit is `scripts/fixtures/mock_camera_digit.bin` (a
  single MNIST test digit, class 3); `scripts/generate-mock-camera-frame.py`
  upscales + RAW10-packs it into `build/mock_camera_frame.bin` at
  build time, embedded via `kernel/src/camera_mock_embed.S`. Gated on
  the `MOCK_CAMERA_FRAME` CMake option (default ON).
- ✅ Unit-test `preprocess_mnist` (`test_slm_camera_preprocess_mnist_md5`
  in `kernel/tests/test_lua.c`): pins the MD5 of the 3,136 output
  bytes — `7c5feeda578897848946a912aa6a80ea` — so any drift in the
  upscale → green-extract → box-average → fp32 pipeline is caught.
- ✅ Lua-level integration test (`test_slm_camera_e2e_mnist_mock`):
  open mock camera → `cam:capture()` → `slm.camera.preprocess_mnist`
  → `slm.model_infer_bytes` → assert `argmax == 3` (the baked digit's
  known class). Runs against the embedded MNIST CPU path; no GPU
  required.

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
- `docs/archive/plans/jetson-bpmp-ipc-plan.md` + `kernel/drivers/bpmp/` — BPMP
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
