# Pi 5 AI HAT+ (Hailo-8/8L) Support Plan

**Target:** GeeekPi AI HAT+ (Hailo-8L, 13 TOPS) and AI HAT+ 26 TOPS (Hailo-8) on Raspberry Pi 5, end-to-end to running AI inference workloads from SLM-OS.

**Status:** Phase 0–4 landed in software (no-hardware work). The probe/boot path is compiled in but unexercised against a real HAT+ until the lab unit is available. Tracked in [#253](https://github.com/SLM-OS/SLM-Operating-System/issues/253).

**Phase summary (2026-04-17):**

| Phase | State | Notes |
|---|---|---|
| 0 — Research | ✅ done | `docs/reference/hailo-driver-notes.md` + `docs/pi5-pcie1-registers.md` + 22 cached source files + `hef.proto` fetched |
| 1 — ARM64 PCIe host controller | ✅ done | `kernel/drivers/pcie/` with QEMU GPEX + BCM2712 backends, 7 QEMU tests |
| 2 — Inference-device abstraction | ✅ done | `kernel/include/inference_device.h` + CPU-MLP backend, `ai_mlp_assign_cpu` routes through it |
| 3 — Hailo driver scaffolding | ✅ done (software) | `kernel/ai_accel/hailo/` + mocked-ops tests; probe/boot/FW-upload need hardware |
| 4 — nanopb + `.hef` parser | ✅ partial | nanopb vendored (0.4.9.1) + `.hef` outer-header validator + smoke tests; full `ProtoHEFHef` decode deferred until a real `.hef` is available |
| 5 — Single-model inference | ☐🔗 hardware-gated | requires Phase 4 full parse + real HAT+ |
| 6 — AI scheduler Hailo policy | ☐🔗 hardware-gated | requires Phase 5 |
| 7 — Shell / demo polish | ☐🔗 hardware-gated | `hailo probe` / `hailo fw` shell commands already wired |

---

## 1. Summary

The AI HAT+ is a PCIe Gen3 x1 add-in board exposing a Hailo-8/8L NPU through the Pi 5's external PCIe connector (CM4-style FFC). Linux drives it through the `hailo_pci` + `hailort` userspace stack; there is no bare-metal reference.

Getting it working on SLM-OS requires crossing three boundaries the project has not crossed before:

1. **A real PCIe host controller driver on ARM64.** The kernel has x86-64 PCIe (ECAM + legacy I/O) and has touched RP1's internal PCIe link indirectly through hardcoded MMIO, but there is no general PCIe enumerator for BCM2712 and no code at all for `pcie1` (the external RC at `0x1000110000` that the AI HAT+ attaches to).
2. **A closed-vendor accelerator driver.** Hailo's firmware load sequence, doorbell/ring-buffer protocol, and `.hef` (Hailo Executable Format) binary layout are partially documented in their open-source Linux driver (`hailort/hailo_pci`, dual-licensed GPLv2). The bring-up work is analogous in spirit to the NVIDIA GSP bring-up — reuse that pattern: a shared core plus a platform shim.
3. **An "inference device" abstraction in the kernel.** Today, all AI inference in SLM-OS is CPU-only via NEON/SSE (AI scheduler MLP/PPO, CACHEUS eviction). A new vtable is needed so the scheduler (and, eventually, page eviction and Lua scripting) can dispatch to a Hailo backend instead of the CPU path.

**Estimated effort:** 10–15 weeks of focused work, staged across 7 phases. Realistic capstone-scale target is **Phase 0–4 (probe + boot + basic inference)**; Phases 5–7 are follow-on work.

---

## 2. Hardware Context

### 2.1 AI HAT+ variants

| Variant | Accelerator | TOPS | PCIe link | Power |
|---|---|---|---|---|
| AI HAT+ 13 TOPS | Hailo-8L | 13 | Gen3 x1 | ~2.5 W |
| AI HAT+ 26 TOPS | Hailo-8 | 26 | Gen3 x1 | ~3.5 W |

Both use the same Hailo PCIe programming model — the driver is identical, only the compiled `.hef` model file differs.

### 2.2 Pi 5 PCIe topology

The BCM2712 exposes two PCIe root complexes relevant here:

| RC | Base (config) | Purpose | Current SLM-OS support |
|---|---|---|---|
| `pcie1` | `0x1000110000` | External connector (AI HAT+, NVMe, etc.) | None |
| `pcie2` | `0x1000120000` | Internal, fixed to RP1 I/O controller | Hardcoded MMIO only (no enumeration) |

**Key consequence:** The AI HAT+ is on `pcie1`, which has zero existing code. The RP1-specific MSIX_CFG bug tracked in #247 does **not** apply — `pcie1` endpoints use a MIP (Message-Signaled Interrupt Peripheral) address programmed into the endpoint's MSI table. Phase 0 research discovered Hailo's driver uses **plain MSI (not MSI-X)** via `pci_enable_msi()` — one vector per device. Plan calls throughout for `pcie_alloc_msix`; the Phase 1 PCIe API must also expose `pcie_alloc_msi()`.

**Config-space gotcha:** `pcie1` is **disabled** in the stock Pi 5 DTS. It only enables when `dtparam=pciex1` (or the AI HAT+ HAT overlay) is in `config.txt`. Phase 0 smoke test must confirm the HAT+ is visible under stock Linux before SLM-OS attempts probe — otherwise SLM-OS sees a dead RC. See `docs/reference/rpi-linux-pciex1-compat-pi5-overlay.dts`.

### 2.3 Pi 5 firmware constraints

The Pi 5 VideoCore firmware brings up `pcie1` and trains the link during early boot if `dtparam=pciex1` is set (the default on Pi 5 HAT+ builds). SLM-OS inherits a live, trained link — it does not need to own the PCIe controller reset or retraining sequence. This matches how `pcie2`/RP1 is inherited today.

What SLM-OS **does** need to do:
- Read the device's BARs via config space.
- Map BARs into SLM-OS's MMU.
- Enable bus master on the device.
- Allocate MSI(-X) vectors and program the device's MSI-X table.
- Route MSI-X writes through MIP1 (the external-PCIe counterpart to MIP0) into GIC SPIs.

### 2.4 MIP1 (external-PCIe MSI)

**Resolved by Phase 0 research** (`docs/pi5-pcie1-registers.md`). MIP0 and MIP1 are stacked at 4 KB apart, not mirrored at a larger offset as originally speculated:

| MIP | Base              | Vectors | SPI range    | Serves |
|-----|-------------------|---------|--------------|--------|
| MIP0 | `0x10_00130000` | 64      | 128–191      | pcie2 (RP1) |
| MIP1 | `0x10_00131000` | **8**   | **247–254**  | pcie1 (external HAT) |

MIP1's 8-vector limit is fine for Hailo (one MSI) but constrains how many PCIe-attached endpoints Phase 1+ can handle simultaneously. See `docs/reference/rpi-linux-irq-bcm2712-mip.c` for the doorbell layout.

---

## 3. Phased Implementation Plan

Each phase is self-contained and deliverable. Later phases assume earlier ones landed.

### Phase 0: Research & Prerequisites ✅ complete (2026-04-17)

**Deliverables:**
- ✅ `docs/reference/hailo-driver-notes.md` — annotated notes from Hailo's open-source Linux driver (`hailo8` branch — `master` has dropped Hailo-8). Covers firmware upload, VDMA, `.hef` layout, submit/complete, register map, IDs, gotchas.
- ✅ `docs/pi5-pcie1-registers.md` — `pcie1` RC register layout + MIP1 programming model with line references into rpi-6.6.y sources.
- ✅ 21 cached source files under `docs/reference/` (`hailo-*`, `rpi-linux-*`).
- ✅ Model frozen: MobileNetV1 INT8 from Hailo Model Zoo, Hailo Dataflow Compiler v5.3.0. `.hef` compile deferred to Phase 4 (dev workstation only).
- ☐🔗 Confirm AI HAT+ link trains under stock Linux — hardware-gated, deferred to when the Pi 5 + HAT+ lab unit is available.

**Key findings that reshape later phases:**
1. **`.hef` body is a protobuf blob** (`hef_proto_size` bytes after the header), not a flat binary. Follow-up research (2026-04-17) confirmed `hef.proto` is published under MIT license in `hailo-ai/hailort` as a single self-contained file (proto3, 1059 LOC, 87 messages, no imports, no `map`/`Any`/extensions). Saved to `docs/reference/hailo-hef.proto`. **Parse in-kernel with [nanopb](https://github.com/nanopb/nanopb)** (zlib license, ~1500 LOC portable C, used in Zephyr RTOS). The large weight payloads are in the CCWS block that follows the proto body, not in the proto itself — so nanopb only parses metadata (layer shapes, I/O directions, ops config), keeping memory pressure low. ~45 `repeated`/`bytes` fields need `pb_callback_t` glue backed by PMM. Sidecar pre-parse has been ruled out. If nanopb's generated output trips `-std=c23 -Wpedantic`, consider an upstream contribution.
2. **Device IDs:** vendor `0x1E60`, Hailo-8 / Hailo-8L both enumerate as `0x2864` (SKU differentiation happens in firmware config, not PCI).
3. **Firmware upload is not VDMA on Hailo-8.** It uses the ATR[0] address translation window plus direct MMIO writes to BAR4. `hailo-driver-notes.md` §4 documents the ~5 s boot-status + ATR[1] poll sequence.
4. **Plain MSI, not MSI-X.** Driver calls `pci_enable_msi(pdev)` with one vector. Phase 1 API must expose `pcie_alloc_msi()` in addition to the originally planned `pcie_alloc_msix()`.
5. **Track the `hailo8` branch / v5.3.0 tag** — not the driver's master branch.

### Phase 1: ARM64 PCIe Host Controller ✅ complete (2026-04-17)

**Delivered:**
- `kernel/include/pcie.h` — public API (cross-platform): `pcie_find_device`, `pcie_enable_bus_master`, `pcie_map_bar`, `pcie_alloc_msi`, `pcie_alloc_msix`, `pcie_bind_irq_handler`, `pcie_find_capability`, `pcie_config_read/write{8,16,32}`.
- `kernel/drivers/pcie/pcie_core.c` — platform-independent enumeration: bus walk, BAR size-probe (32/64-bit), capability walk, device table, vtable forwarders.
- `kernel/drivers/pcie/pcie_qemu_gpex.c` — QEMU virt ECAM backend. ECAM base corrected to `0x40_10000000` (high memory) per runtime DT dump; the `0x3f000000` address documented earlier is the PIO window and aborts on read.
- `kernel/drivers/pcie/pcie_bcm2712.c` — Pi 5 `pcie1` backend: EXT_CFG index/data pair at `0x9000`/`0x9004` (variant offset, not `0x8000`), BAR outbound window translation (`0x00_80000000`→`0x1b_80000000` non-pref, `0x04_00000000`→`0x18_00000000` pref), MIP1 MSI allocation + 8-vector trampoline fan-out into GIC SPIs 247–254.
- `kernel/drivers/pcie/pcie_stub.c` — Jetson link stub.
- `kernel/mm/vmm.c` — QEMU_VIRT mappings: low MMIO (0x10000000..0x3effffff) as 2 MB blocks in `l2_mmio`, ECAM (0x40_10000000..0x40_1fffffff) as a 1 GB L1 block.
- `Makefile` — `-device virtio-rng-pci,bus=pcie.0` on `make test` for QEMU virt.
- `kernel/tests/test_pcie.c` — 7 tests (backend-registered, find-by-vendor, memory-BAR present/sized, capability walk, map-BAR returns VA, config-read consistency, bus-master toggle).

**Hardware-gated follow-ups** (unblocked when the HAT+ lab unit is available):
- Live BCM2712 enumeration (QEMU tests don't cover `pcie_bcm2712.c` — mocked-ops test possible but was deferred).
- Real MIP1 MSI delivery into GIC SPI 247..254.
- Pi 5 BAR resource programming (currently inherited from VideoCore firmware).

### Phase 2: Inference Device Abstraction ✅ complete (2026-04-17)

**Delivered:**
- `kernel/include/inference_device.h` — vtable (`init`, `shutdown`, `load_model`, `run`, `free_model`), tensor descriptor (`inference_tensor_t` with dtype + rank + shape), model handle typedef, capability bitmask, error codes.
- `kernel/inference/inference_device.c` — always-compiled dispatcher/registry (4-device capacity, no FP, compiles under `-mgeneral-regs-only`).
- `kernel/sched/ai/inference_cpu.c` — "cpu-mlp" backend inside the `ai_sched` static library (FP-enabled). Wraps `ai_mlp_forward_logits` with `FP_CONTEXT_SAVE`/`_RESTORE`. Registered from `main.c` when `AI_SCHED=ON`.
- `kernel/sched/ai/ai_inference.c/.h` — split `forward_pass` into raw-logits `forward_logits` + scheduler-specific argmax+decode. Exposed `ai_mlp_forward_logits()` for the CPU backend.
- `kernel/sched/ai/sched_ai.c` — `ai_mlp_assign_cpu` routes MLP inference through `inference_run` with graceful fallback to the direct `ai_schedule_mlp` call when the registry is empty.
- `kernel/tests/test_inference_device.c` — 3 base tests (fake-backend register + run, NULL dispatch) + 1 conditional (cpu-mlp registered).

When Hailo inference lands (Phase 5), switching the MLP policy to the NPU is one `inference_device_set_default()` call — no edit to `sched_ai.c`.

### Phase 3: Hailo Device Probe & Control Plane ✅ partial (2026-04-17)

**Delivered (software):**
- `kernel/ai_accel/hailo/hailo.h` — API + `hailo_platform_ops` vtable (register I/O, BAR4 bulk R/W, DMA alloc, cache clean/invalidate, MSI registration, memory barrier, udelay). PCIe IDs (`0x1E60:0x2864`), BAR indices, BAR0 register offsets (ISTATUS, IMASK, ATR[0..3]), firmware header struct, Hailo-8 device-side load addresses, state machine.
- `kernel/ai_accel/hailo/hailo_core.c` — `hailo_init` (vtable validation), `hailo_probe` (vendor/device ID read + boot_status liveness via ATR[0]), `hailo_validate_firmware` (magic + size bounds), ATR[0] save/set_target/restore helpers, `dev_read`/`dev_read32` through BAR4. `hailo_boot` + `hailo_get_firmware_version` are hardware-gated stubs returning `HAILO_ERR_UNSUPPORTED`.
- `kernel/ai_accel/hailo/hailo_pi5.c` — Pi 5 platform shim: `pcie_find_device` + `pcie_map_bar` (BAR0/2/4), `pcie_enable_bus_master`, PMM-backed DMA with `+0x10_00000000` inbound offset, `cache_clean/invalidate_range`, CNTPCT `udelay`, `pcie_alloc_msi` + handler trampoline.
- `kernel/ai_accel/hailo/hailo_stub.c` — `hailo_platform_install` returns `HAILO_ERR_NODEV` on non-Pi5 platforms.
- `kernel/ai_accel/hailo/hailo_shell.c` — `hailo` / `hailo probe` / `hailo fw` shell commands.
- `kernel/tests/test_hailo.c` — 12 tests against a mocked `hailo_platform_ops` (init validation, probe paths, firmware-header validator).

**Hardware-gated follow-ups:**
- Real probe on Pi 5 (vendor ID readback, boot_status).
- `hailo_boot` full state machine (ATR[0] FW upload → boot_status poll → ATR[1] FW-loaded poll).
- Control-channel RPC (`hailo_get_firmware_version` etc.).
- MSI routing validation end-to-end.

### Phase 4: Firmware Load & Model Load ✅ partial (2026-04-17)

**Revised 2026-04-17 from Phase 0 findings.** The kernel parses `.hef` directly using nanopb; no workstation sidecar.

**Delivered (software):**
- `kernel/lib/nanopb/` — vendored nanopb 0.4.9.1 (zlib license, ~4500 LOC). `pb_syshdr.h` provides freestanding glue (memcpy/memset/strlen from `kernel/src/string.c`; INT_MAX/CHAR_BIT constants). Builds as a separate static CMake library with `-DPB_SYSTEM_HEADER="pb_syshdr.h"`. **Clean compile under `-std=c23 -Wpedantic -Werror` on the first try** — no upstream PR needed.
- `kernel/ai_accel/hailo/hef_header.{c,h}` — flat `.hef` outer-header validator. Parses magic (`0x01484546`), version (v0..v3), proto_size, and the per-version trailer (MD5 for v0, CRC + CCWS for v1/v2/v3). All fields big-endian; explicit byte-shuffle avoids host-byte-order dependency. Returns `hef_outer_header` with proto and CCWS offsets.
- `kernel/tests/test_hef.c` — 10 tests (8 HEF-header cases + 2 nanopb smoke tests proving the runtime links against our freestanding glue).

**Hardware-gated / deferred follow-ups:**
- Generating `hef.pb.c/h` from `hef.proto` via the nanopb Python generator (deferred until a real `.hef` is available to validate the generated types against).
- Full `ProtoHEFHef` decode with `pb_callback_t` glue backed by PMM for the ~45 variable-size fields.
- Firmware `.incbin` for `hailo8_fw.bin` (blob not yet in the repo; trivial to add as a CMake option).
- `hailo_boot()` implementation (the protocol is commented in `hailo_core.c`'s Phase-4 stub).
- `hailo load <vfs-path>` shell command.

**Deliverables:**
- **nanopb integration** under `kernel/lib/nanopb/`:
  - Vendored nanopb source (zlib license, ~1500 LOC portable C) — pinned to a specific release tag (e.g. `0.4.9.1`).
  - `kernel/lib/nanopb/pb_syshdr.h` providing freestanding-friendly glue (no `<stdlib.h>`, no `<string.h>`; SLM-OS has its own `memcpy`/`memset`). Verify clean compile under `-std=c23 -Wpedantic`; file upstream PR if pedantic warnings surface and fixes are non-invasive.
  - PMM-backed `pb_callback_t` helpers for the ~45 `repeated`/`bytes` fields in `hef.proto`.
  - Generator invocation wired into the build: `hef.proto` → `hef.pb.c`/`hef.pb.h` at `make` time (or committed, updated on schema bumps).
  - `make test-nanopb` — unit tests that decode a fixture `.hef` (committed, small) into the generated message structs and spot-check key fields.
- Firmware image embedded via `.incbin` (same pattern as scheduler weights; Hailo firmware is open-distribution and ships with the Linux driver).
- Firmware upload via **ATR[0] + BAR4 MMIO** (not VDMA) per `hailo-driver-notes.md` §4: boot-status poll (10 ms), staged write, ATR[1] flag poll (5 s timeout).
- `.hef` header validator in C: magic (`0x01484546`), version (0–3), CRC or MD5 per version (big-endian outer fields).
- `.hef` body parser (kernel): nanopb decode of the top-level `ProtoHEFHef` and the subset of nested messages actually consumed by the loader (~10–15 of 87). Nanopb `.options` files strip unused fields to keep generated code size down.
- Model load API: `hailo_load(const void *hef, size_t hef_size)` — validates header, decodes proto metadata, locates CCWS, DMAs weights, programs control-plane, confirms "model ready".
- `hailo load <vfs-path>` shell command wired to VFS via the existing LittleFS path.

### Phase 5: Single-Model Inference (2 weeks)

**Deliverables:**
- Tensor buffer API: allocate input/output tensors in NC DMA memory with platform cache sync handled by the driver.
- Inference submit: post descriptors, ring doorbell, wait on MSI-X completion (or polled CNTPCT timeout fallback — see #247 for why polling is a valid long-term fallback on Pi 5).
- `hailo infer <model> <input-tensor>` shell command; output tensor dumped as hex or post-processed per a known model's output layout.
- Latency histogram integration (#196) — add `hailo_infer` as a first-class bench histogram source.
- Cross-platform inference bench doc (`docs/cross-platform-inference-bench.md`) updated with a "Hailo-8L on Pi 5" row.

### Phase 6: AI Scheduler Integration (1–2 weeks)

**Deliverables:**
- New scheduler policy: `ai_policy_hailo` that uses the inference device abstraction (Phase 2) to run the scheduler MLP on Hailo instead of NEON.
- Requires retraining/recompiling the scheduler MLP as a `.hef`. Constraint: Hailo's tooling supports a narrower op set than full ONNX; the 108-dim → 24/42-action MLP with ReLU should compile.
- Benchmark comparison: `bench sched-policy` comparing:
  - Round-robin (baseline).
  - CPU MLP (current AI scheduler).
  - Hailo MLP (new).
  - Metrics: decisions/sec, end-to-end task makespan, device power.
- Policy switch via existing `sched_set_policy()` runtime API — the whole point of the Phase 2 abstraction.

### Phase 7: Shell Integration & Demo Polish (1 week)

**Deliverables:**
- Lua binding: `slm.hailo.load(path)`, `slm.hailo.infer(model, tensor)`, `slm.hailo.status()`.
- Demo script that runs MobileNetV1 classification on a small embedded image (test data in VFS), shows live FPS in `top`-style UI.
- `docs/demo.md` updated with an AI HAT+ demo path.
- Update `docs/capstone-feature-status.md` GPU Inference row: Pi 5 column goes from "—" to "Hailo-8L via AI HAT+".

---

## 4. Risks & Open Questions

| Risk | Likelihood | Mitigation |
|---|---|---|
| Hailo firmware upload protocol is undocumented outside their Linux driver — might take longer to reverse | Medium | Phase 0 research budget; fall back to `hailort` userspace as a cross-check in Linux |
| MIP1 programming model differs from MIP0 in non-obvious ways | Low | Cross-reference Pi 5 downstream DTS and rpi-6.6.y driver; if it really differs, fall back to polled completion (Phase 5 already has this path) |
| `.hef` format is versioned; Hailo Model Zoo models may require a compiler version the capstone timeline can't match | Medium | Freeze a specific Hailo Compiler version in Phase 0 and document it; treat `.hef` as a build artifact, not a source asset |
| Pi 5 PCIe Gen3 link instability under thermal load (AI HAT+ runs hot) | Low | Use passive heatsink that ships with AI HAT+; monitor link status register during inference |
| Effort exceeds capstone timeline | **High** | Land Phases 0–4 as a hard target (minimum viable demo: "SLM-OS loaded a model onto a real NPU and ran a forward pass on bare metal"). Phases 5–7 as stretch. |

---

## 5. Dependencies

- **Codebase:** None on other in-flight PRs. Independent workstream.
- **Hardware:** GeeekPi AI HAT+ physical unit; Pi 5 in lab with the HAT mounted and `dtparam=pciex1` in config.txt.
- **Tooling:** Hailo Compiler (Linux x86-64 only, runs on dev workstation). Free for non-commercial use.
- **Labctl:** If the AI HAT+ becomes part of the standard Pi 5 lab setup, `labctl`'s claims feature (SPEC in `../Embedded-Lab-Control/docs/SPEC_claims.md`) should cover it implicitly — no labctl changes needed.

---

## 6. Test Plan

Per the project's post-change checklist (run on every phase):

1. **QEMU unit tests** (`make test`) — PCIe enumeration, inference-device abstraction, Hailo core via mocked BAR vtable.
2. **Pi 5 hardware smoke test** — `labctl sdwire_update` + `serial_send "hailo probe"` — must pass before every commit to Phase 3+.
3. **`make test-hailo`** — offline driver tests (added in Phase 3).
4. **Demo script** — end-to-end from `slm.hailo.load()` to classification output (Phase 7).
5. **Reliability** — `labctl boot_test --count 10` with a `hailo probe` step in the boot-verification script.

---

## 7. Out of Scope

- Multi-context / multi-model concurrent inference (Hailo supports this, but adds significant driver complexity; follow-on issue).
- Hailo integrated memory features like power gating, DVFS (trivial; follow-on).
- Model hot-swap (#232) — orthogonal; can layer on top once the basic load/run path works.
- x86-64 AI accelerator support — x86-64's path forward is the RTX 3050 via GSP-RM (separate work).
- Jetson Hailo support — the Jetson already has an integrated NPU (NVDLA) plus GPU; adding Hailo on Jetson is non-capstone-relevant.

---

*Last updated: 2026-04-17*
