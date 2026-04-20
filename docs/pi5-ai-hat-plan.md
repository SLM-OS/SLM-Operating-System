# Pi 5 AI HAT+ (Hailo-8/8L) Support Plan

**Target:** GeeekPi AI HAT+ (Hailo-8L, 13 TOPS) and AI HAT+ 26 TOPS (Hailo-8) on Raspberry Pi 5, end-to-end to running AI inference workloads from SLM-OS.

**Status:** Phase 0–4 landed in software (no-hardware work). The probe/boot path is compiled in but unexercised against a real HAT+ until the lab unit is available. Tracked in [#253](https://github.com/SLM-OS/SLM-Operating-System/issues/253).

**Phase summary (2026-04-18):**

| Phase | State | Notes |
|---|---|---|
| 0 — Research | ✅ done | `docs/reference/hailo-driver-notes.md` + `docs/pi5-pcie1-registers.md` + 22 cached source files + `hef.proto` fetched |
| 1 — ARM64 PCIe host controller | ✅ done | `kernel/drivers/pcie/` with QEMU GPEX + BCM2712 backends, 7 QEMU tests |
| 2 — Inference-device abstraction | ✅ done | `kernel/include/inference_device.h` + CPU-MLP backend, `ai_mlp_assign_cpu` routes through it |
| 3 — Hailo driver scaffolding | ✅ done (software) | `kernel/ai_accel/hailo/` + mocked-ops tests; probe/boot/FW-upload need hardware |
| 4 — nanopb + `.hef` parser | ✅ partial | nanopb vendored (0.4.9.1) + `.hef` outer-header validator + smoke tests; full `ProtoHEFHef` decode deferred until a real `.hef` is available |
| 5.1 — HEF tensor metadata | ✅ done | I/O pad shapes captured from the first NG |
| 5.2 — Control-channel RPC transport | ✅ tier-1 + tier-2 + tier-3 (2026-04-18) | IDENTIFY + WRITE_MEMORY + READ_MEMORY + CONFIG_STREAM all round-tripped on pi-5-1; shell bindings `peek/poke/cfgstream` |
| 5.3 — hailo_load + weight DMA | ✅ software (2026-04-18) | HEF CCW parser, DMA tensor allocator, WRITE_MEMORY-based upload loop all landed; awaits compiled `.hef` + CONFIG_STREAM-with-context for end-to-end hardware test |
| 5.4 — Inference submit + `hailo infer` | ✅ software (2026-04-18) | VDMA descriptor list + programming + channel start/stop/submit + hailo_infer orchestrator + `hailo infer` shell; pi-5-1 runs the full pipeline and times out at output submit (no firmware context yet) — expected |
| 6 — AI scheduler Hailo policy | ☐🔗 hardware-gated | requires Phase 5.3/5.4 |
| 7 — Shell / demo polish | ☐🔗 hardware-gated | `hailo probe/boot/fw/peek/poke/cfgstream/infer` wired; `hailo load <path>` prints HEF metadata |

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

**Config-space gotcha (superseded 2026-04-17):** An earlier revision said `dtparam=pciex1` in `config.txt` enables pcie1. **It does not.** Testing on pi-5-1 (Sep 2024 EEPROM) confirmed `dtparam=pciex1` is not a recognized firmware parameter — `vcgencmd get_config pciex1` returns "pciex1 is unknown". Under stock Raspberry Pi OS the HAT+ enumerates as `0001:01:00.0 [1e60:2864]` with NO pcie-related setting in `config.txt`; the bring-up is done by Linux's `brcm-pcie` kernel driver at probe time. See §2.3 for what this means for SLM-OS.

### 2.3 Pi 5 firmware constraints — REVISED 2026-04-17

**Earlier (wrong) version of this section said:** *"The Pi 5 VideoCore firmware brings up pcie1 and trains the link during early boot. SLM-OS inherits a live, trained link."*

**What actually happens** (confirmed on pi-5-1 with AI HAT+ mounted and Sep 2024 EEPROM):

- **`pcie2`/RP1** — firmware DOES train at boot. Status register `0x10_00124068` reads `0x3e0b0` (PHY + DL both set) before any OS runs. SLM-OS inherits the trained link and just needs to access RP1 peripherals at `0x1F00000000+`.
- **`pcie1`/external** — firmware leaves in reset. Status register `0x10_00114068` reads `0x1e08f` (PHY + DL both clear) and CTRL `0x10_00114064` reads `0x00000000` (PERSTB=0 → endpoint held in reset) until a Linux kernel driver brings it up. Under Pi OS, `brcm-pcie`'s `brcm_pcie_setup()` does the work at `~1.9s` into kernel boot (visible in dmesg). SLM-OS has no equivalent driver today.
- **EEPROM constraint** — the Sep 2024 EEPROM is required for SLM-OS's RP1 UART to survive the firmware → kernel handoff (see `pi5_eeprom_findings.md`: firmware ≥ v2025.01.22 silently breaks writes to `0x1F00030000`). So "upgrade the EEPROM and hope firmware auto-trains pcie1" is not an option — we need to do the training ourselves.
- **`dtparam=pciex1`** is not a valid firmware option on this EEPROM. Setting it in `config.txt` has no effect.

**Consequence:** SLM-OS must implement its own PCIe link-training path for pcie1 — porting `brcm_pcie_setup()` from `drivers/pci/controller/pcie-brcmstb.c` (full reference cached at `docs/reference/rpi-linux-pcie-brcmstb.c`). This is **Phase 1.5** below, inserted between the existing Phase 1 (enumerator) and Phase 3 (Hailo driver).

What SLM-OS needs to do (now that the assumption about firmware training is gone):

1. **Reset/train pcie1** at boot (Phase 1.5 new work):
   - Take RC out of reset (platform reset domains 7, 43 per `bcm2712.dtsi`).
   - Run the rescal PHY calibration.
   - Program MPS/MRRS, CLKREQ, HARD_DEBUG.
   - Deassert PERST# to the endpoint.
   - Wait for `PCIE_MISC_PCIE_STATUS.{PHY_LINKUP, DL_ACTIVE}` both set, with a ~100 ms timeout.
2. Read the endpoint's BARs via config space (Phase 1 existing).
3. Map BARs into SLM-OS's MMU (Phase 1 existing).
4. Enable bus master (Phase 1 existing).
5. Allocate MSI vectors via MIP1 and program the endpoint's MSI capability (Phase 1 existing).

**Phase 0 hardware smoke test result (2026-04-17):** AI HAT+ physically works. Under Raspberry Pi OS on the same pi-5-1:
```
0001:01:00.0 Co-processor [0b40]: Hailo Technologies Ltd. Hailo-8 AI Processor [1e60:2864] (rev 01)
[    1.983256] brcm-pcie 1000110000.pcie: link up, 5.0 GT/s PCIe x1 (!SSC)
```
No hardware or seating problems — the gap is purely the missing link-training code in SLM-OS.

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
1. **`.hef` body is a protobuf blob** (`hef_proto_size` bytes after the header), not a flat binary. Follow-up research (2026-04-17) confirmed `hef.proto` is published under MIT license in `hailo-ai/hailort` as a single self-contained file (proto3, 1059 LOC, 87 messages, no imports, no `map`/`Any`/extensions). Originally cached at `docs/reference/hailo-hef.proto`; moved to `kernel/ai_accel/hailo/hef.proto` as part of Phase 4 since it's now a first-class build input (not a cached external reference). **Parse in-kernel with [nanopb](https://github.com/nanopb/nanopb)** (zlib license, ~1500 LOC portable C, used in Zephyr RTOS). The large weight payloads are in the CCWS block that follows the proto body, not in the proto itself — so nanopb only parses metadata (layer shapes, I/O directions, ops config), keeping memory pressure low. ~45 `repeated`/`bytes` fields need `pb_callback_t` glue backed by PMM. Sidecar pre-parse has been ruled out. If nanopb's generated output trips `-std=c23 -Wpedantic`, consider an upstream contribution.
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

### Phase 1.5: brcm-pcie link training for pcie1 (NEW, 2026-04-17)

**Why this phase exists** — see §2.3 above for the full story. Short version: firmware does NOT train pcie1; Linux's `brcm-pcie` driver does, at kernel boot. SLM-OS has to do the same if it wants `hailo probe` to succeed.

**Implementation landed (2026-04-17, partial):** `kernel/drivers/pcie/pcie_bcm2712.c` now ports the core of `brcm_pcie_setup()`:
- `bcm_reset_assert/deassert` for reset ID 43 (bridge reset) — toggles `0x10_01504318 + bank*0x18`.
- `rescal_bring_up` at `0x10_00119500` — START + poll STATUS + clear START.
- `munge_pll_54mhz` — 7 MDIO writes to PLL block 0x1600 via `PCIE_RC_DL_MDIO_{ADDR,WR_DATA}`.
- MISC_CTRL (SCB_ACCESS_EN / CFG_READ_UR_MODE / max_burst=128B), RC_BAR2 inbound window, SCB0_SIZE, UBUS error suppression, timeouts, RC_BAR1/3 disable, class code, two outbound windows, PERST# deassert, 100 ms link-up wait.
- Bridge bus numbers (primary/secondary/subordinate) programmed at RC config offset 0x18.
- New `pcie_host_ops::get_mmio_window` hook lets `pcie_core` bump-allocate BARs for endpoints with firmware-unprogrammed BAR addresses (required because the standalone boot flow has no UEFI or equivalent resource manager).

**Result on pi-5-1 with AI HAT+ mounted:** Link trains successfully. Status register transitions from `0x1e08f` (PHY + DL clear) to `0x3e0bf` (both set). Endpoint enumerates at `01:00.0` with vendor `0x1e60` / device `0x2864` — `pcie_find_device` finds it.

**New blocker encountered (2026-04-17):** Config-space reads past offset `0x07` return `0xFFFFFFFF`, while offsets `0x00`–`0x07` (vendor/device/command/status) read correctly. Specifically `config[0x08]` (class/revision) and all BAR offsets `0x10..0x24` all read as all-ones. Linux on the same hardware reads these offsets correctly (visible in dmesg as `type 00 class 0x0b4000`), so the endpoint IS responding there — something in the SLM-OS-initialized bridge path is truncating TLP completions at the dword boundary. BAR probing can't proceed until this is fixed. Candidates to investigate next session:
- CRS timeout/retry interaction (RC_CONFIG_RETRY_TIMEOUT = `0x0ABA0000` matches the reference, but maybe the hardware needs CRSVis=1 for the Hailo specifically).
- MISC_CTRL bits — `MAX_BURST_SIZE` = 1 (128B) is what the reference driver picks for 2712, but a wrong encoding could cause TLP fragmentation issues.
- Missing initialisation of RC's own PCIe capability registers before the first downstream config cycle (Linux's `pci_host_probe` sets a lot of state SLM-OS skips).
- Bridge Memory-Base / Memory-Limit at config offsets `0x20`/`0x22` — not programmed today, might be required before the bridge forwards memory-space config reads.
- The MDIO PLL values from the Linux reference driver are literally "settings Danny wrote down" (the comment in `pcie-brcmstb.c:473` is verbatim). Maybe one of those seven values differs from what the live Pi 5 firmware uses, and the mismatch causes flaky config-space reads.

**Deliverables:**

- Port of `brcm_pcie_setup()` from `drivers/pci/controller/pcie-brcmstb.c` (cached at `docs/reference/rpi-linux-pcie-brcmstb.c`) into `kernel/drivers/pcie/pcie_bcm2712.c`. The Linux function does ~400 lines of work — SLM-OS only needs the subset for the `brcm,bcm2712-pcie` compatible string (2712-specific paths).
- Reset-domain access: `pcie_rescal` + reset IDs 7 and 43 from `bcm2712.dtsi:1048`. Needs either a minimal reset-controller driver or a direct-register implementation for the CPR / BCM reset block at `0x10_00000000+`.
- PHY / rescal programming: MDIO-style access via `PCIE_RC_DL_MDIO_{ADDR,WR_DATA,RD_DATA}` at RC offsets 0x1100/0x1104/0x1108 (`pcie-brcmstb.c:61-63`).
- Outbound window programming: `PCIE_MISC_CPU_2_PCIE_MEM_WIN*` — tell the RC which PCIe-side addresses map to which CPU phys addresses. Today's code assumes firmware set these; with pcie1 reset, we have to program them ourselves.
- Inbound window programming: `PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_*` for MSI routing through MIP1.
- Link-training wait: poll `PCIE_MISC_PCIE_STATUS` at offset `0x4068` for `PHY_LINKUP | DL_ACTIVE`, 100 ms budget with 1 ms poll interval.

**Test plan:**
- No unit tests possible for this — register sequences are platform-specific and depend on real HW responses. Validated by `hailo probe` on pi-5-1: `vendor=0x1E60 device=0x2864` → link trained, Phase 3 unblocked.
- Boot-reliability check via `labctl boot_test --count 10`: full boot including link train, no hangs, no aborts.

**Risks:**
- Reset controller registers at `0x10_00000000+` are not documented in any datasheet available to the project. The Pi kernel's `drivers/reset/reset-brcmstb-rescal.c` is the only reference. Likely 1–2 days of register-poking.
- Some brcm-pcie code paths in Linux depend on a fully-initialised clock tree. SLM-OS bypasses most clocks (firmware leaves them in a stable state). If any required clock is gated at SLM-OS boot, link training will hang — and the failure mode is silent (same `status=0x1e08f` as now).

**Scope boundary:** Phase 1.5 does NOT require Linux-compatible DT parsing or a generic reset-controller framework. Direct MMIO pokes with the constants from `pcie-brcmstb.c` are enough, on the assumption that Pi 5 firmware leaves clocks in a usable state (which it does — pcie2/RP1 proves this).

### Phase 2: Inference Device Abstraction ✅ complete (2026-04-17)

**Delivered:**
- `kernel/include/inference_device.h` — vtable (`init`, `shutdown`, `load_model`, `run`, `free_model`), tensor descriptor (`inference_tensor_t` with dtype + rank + shape), model handle typedef, capability bitmask, error codes.
- `kernel/inference/inference_device.c` — always-compiled dispatcher/registry (4-device capacity, no FP, compiles under `-mgeneral-regs-only`).
- `kernel/sched/ai/inference_cpu.c` — "cpu-mlp" backend inside the `ai_sched` static library (FP-enabled). Wraps `ai_mlp_forward_logits` with `FP_CONTEXT_SAVE`/`_RESTORE`. Registered from `main.c` when `AI_SCHED=ON`.
- `kernel/sched/ai/ai_inference.c/.h` — split `forward_pass` into raw-logits `forward_logits` + scheduler-specific argmax+decode. Exposed `ai_mlp_forward_logits()` for the CPU backend.
- `kernel/sched/ai/sched_ai.c` — `ai_mlp_assign_cpu` routes MLP inference through `inference_run` with graceful fallback to the direct `ai_schedule_mlp` call when the registry is empty.
- `kernel/tests/test_inference_device.c` — 3 base tests (fake-backend register + run, NULL dispatch) + 1 conditional (cpu-mlp registered).

When Hailo inference lands (Phase 5), switching the MLP policy to the NPU is one `inference_device_set_default()` call — no edit to `sched_ai.c`.

### Phase 3: Hailo Device Probe & Control Plane ✅ (2026-04-18)

**Delivered (software):**
- `kernel/ai_accel/hailo/hailo.h` — API + `hailo_platform_ops` vtable (register I/O, BAR4 bulk R/W, DMA alloc, cache clean/invalidate, MSI registration, memory barrier, udelay). PCIe IDs (`0x1E60:0x2864`), BAR indices, BAR0 register offsets (ISTATUS, IMASK, ATR[0..3]), firmware header struct, Hailo-8 device-side load addresses, state machine.
- `kernel/ai_accel/hailo/hailo_core.c` — `hailo_init` (vtable validation + state reset), `hailo_probe` (vendor/device ID read + boot_status liveness via ATR[0]), `hailo_validate_firmware` (magic + size bounds), ATR[0] save/set_target/restore helpers, `dev_read`/`dev_read32`/`dev_write`/`dev_write32`/`dev_write_chunked` through BAR4, `hailo_boot` full state machine (app + core FW upload → trigger doorbell → ATR[1] FW-loaded poll, 5 s budget), and the extracted `hailo_decode_cert` / `hailo_decode_core_fw` blob-decode helpers (declared in `hailo_internal.h` so unit tests can exercise the bounds/format logic without driving the full boot path). `hailo_get_firmware_version` remains a Phase 5 stub pending the control-channel RPC.
- `kernel/ai_accel/hailo/hailo_internal.h` — module-private declarations shared between `hailo_core.c` and the unit tests (currently just the two decode helpers). Not part of the public API; lives alongside the public `hailo.h`.
- `kernel/ai_accel/hailo/hailo_pi5.c` — Pi 5 platform shim: `pcie_find_device` + `pcie_map_bar` (BAR0/2/4), `pcie_enable_bus_master`, PMM-backed DMA with `+0x10_00000000` inbound offset, `cache_clean/invalidate_range`, CNTPCT `udelay`, `pcie_alloc_msi` + handler trampoline.
- `kernel/ai_accel/hailo/hailo_stub.c` — `hailo_platform_install` returns `HAILO_ERR_NODEV` on non-Pi5 platforms.
- `kernel/ai_accel/hailo/hailo_shell.c` — `hailo` / `hailo probe` / `hailo boot` / `hailo fw` / `hailo cfgdump` shell commands. `hailo boot` references weakly-linked `hailo_fw_start`/`hailo_fw_end` symbols; the blob is embedded only when CMake `HAILO_FW_BLOB=path/to/hailo8_fw.bin` is set.
- `kernel/ai_accel/hailo/hailo_fw.S` — `.incbin` shim, compiled only when `HAILO_FW_BLOB` is set.
- `kernel/tests/test_hailo.c` — 20+ tests against a mocked `hailo_platform_ops`: init validation, probe paths, firmware-header validator, and **nine new boot-state-machine tests** covering wrong-state rejection, bad magic, missing cert, cert oversize, unexpected boot_status, happy path (verifies each upload section lands at the right device address), stuck-boot-ROM timeout, stuck-FW-loaded timeout, and multi-chunk code uploads through the 4 KB ATR window.

**Hardware-validated on pi-5-1 (2026-04-18):**
- Link trains, BARs mapped, endpoint found at 01:00.0 vendor=0x1E60 device=0x2864.
- `hailo probe` returns OK with `boot_status=0x1` read through the ATR[0] window — proves BAR4 mapping and ATR programming are live end-to-end, not just config space.
- `hailo boot` reports "firmware not embedded" cleanly when built without `HAILO_FW_BLOB` (the blob is not yet checked in).

**Hardware-validated on pi-5-1 (2026-04-18):**
- `hailo boot` with the 164 KB `hailo8_fw.bin` (HailoRT 4.23.0 .deb) uploads the full [app hdr, code, cert, core hdr, core code] sequence to the device via ATR[0]+BAR4, triggers the boot doorbell, and reaches `state=running`. Two bugs fixed along the way:
  - Blob layout: Hailo-8 production firmware bundles BOTH app AND core-firmware sections. Linux's `FW_VALIDATION__validate_fw_headers` enforces a second `[header+code]` pair after the cert for NNC accelerators. The initial `hailo_boot` implementation only uploaded the app section; the endpoint sat at boot_status=1 forever because the boot ROM couldn't find core FW. Fixed by parsing + uploading the trailing `[core_fw_header, core_code]` to `core_fw_header` (0xA0000) + `core_code_ram_base` (0xC0000), in code-then-header order to avoid the boot ROM's polling race.
  - Post-trigger poll: Hailo's `BOOT_STATUS_UNINITIALIZED = 0x1` is a misleading name — it actually means "boot ROM in ready state", not "device uninitialized". Linux's `hailo_pcie_wait_for_boot` waits FOR this value; `hailo_pcie_wait_for_firmware` polls ATR[1] for the FW-loaded magic. Our initial sequence waited for `boot_status != 1` which never happens during a healthy boot. Fixed by dropping that poll stage entirely and going straight to the ATR[1] handshake (5 s budget, 50 ms interval).

**Hardware-gated follow-ups (Phase 5):**
- Control-channel RPC (`hailo_get_firmware_version` etc.).
- MSI routing validation end-to-end.

### Phase 4: Firmware Load & Model Load ✅ (2026-04-18)

**Revised 2026-04-17 from Phase 0 findings.** The kernel parses `.hef` directly using nanopb; no workstation sidecar.

**Delivered (software):**
- `kernel/lib/nanopb/` — vendored nanopb 0.4.9.1 (zlib license, ~4500 LOC). `pb_syshdr.h` provides freestanding glue (memcpy/memset/strlen from `kernel/src/string.c`; INT_MAX/CHAR_BIT constants). Builds as a separate static CMake library with `-DPB_SYSTEM_HEADER="pb_syshdr.h"`. **Clean compile under `-std=c23 -Wpedantic -Werror` on the first try** — no upstream PR needed.
- `kernel/ai_accel/hailo/hef_header.{c,h}` — flat `.hef` outer-header validator. Parses magic (`0x01484546`), version (v0..v3), proto_size, and the per-version trailer (MD5 for v0, CRC + CCWS for v1/v2/v3). All fields big-endian; explicit byte-shuffle avoids host-byte-order dependency. Returns `hef_outer_header` with proto and CCWS offsets.
- `kernel/ai_accel/hailo/hef.proto` + `hef.options` + `hef.pb.{c,h}` — the upstream Hailo protobuf schema (87 messages, 69 repeated/bytes/string fields) generated via nanopb into the kernel. `* type:FT_CALLBACK` in the options file routes every variable-length field through pb_callback_t so the kernel image doesn't carry 6 MB of never-used bss per repeated field. Regenerated on schema change via `scripts/tools/regen-hef-proto.sh` (sets up a throwaway venv at `build/nanopb-venv/`).
- `kernel/ai_accel/hailo/hef_parser.{c,h}` — `hef_parse_body(blob, size, out)` drives nanopb's `pb_decode` with callbacks wired only on the fields the loader consumes today: `header.hw_arch`, `header.sdk_version_str`, and `network_groups` (count + first name). Unused fields fall through to nanopb's default skip. Extracts into a 128-byte `struct hef_info` — no dynamic allocations inside the parser.
- `kernel/ai_accel/hailo/hailo_shell.c` — `hailo load <vfs-path>` subcommand. Two-pass read from LittleFS: stage 1 pulls the outer header into a 64-byte stack buffer, stage 2 PMM-allocates a page-aligned buffer for the proto body (typical size 1-3 MB for a compiled yolov5s), runs the parser, prints extracted metadata, frees the buffer.
- `kernel/tests/test_hef_parser.c` — 7 tests against synthetic protobuf wire-format blobs: header-field decode, empty proto, proto without a header, multi-network-group counting, string truncation, malformed-varint rejection, null-arg rejection.
- `scripts/tools/fetch-hef-fixtures.sh` — downloads `yolov5s.hef` (and optionally others) from the Hailo Model Zoo's public S3 bucket (`hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.18.0/hailo8/`). MIT-licensed per Hailo's repo. Fixtures land in `kernel/tests/fixtures/` which `.gitignore`s `*.hef` so the repo stays small.

**Verified:**
- QEMU tests pass (all 7 hef_parser tests + existing 10 hef_header tests).
- Pi 5 boot: `hailo load /mnt/files/nosuch.hef` reports "stat failed"; `hailo load` (no args) prints usage. Confirms VFS + PMM + parser plumbing are wired end-to-end on hardware.
- All four platforms build clean (QEMU ARM64, RASPI5, JETSON_ORIN_NANO, X86_64).

**Hardware-gated / deferred follow-ups (Phase 5):**
- Full end-to-end test: fetch `yolov5s.hef`, upload to the Pi 5 FAT32 boot partition or grow the LittleFS ramdisk, run `hailo load` against the real file, compare extracted metadata against Hailo's published model card.
- Deeper field extraction as the inference path needs it (ops, layers, weight ranges) — attach more pb_callback_t handlers in `hef_parser.c`.
- Firmware `.incbin` for `hailo8_fw.bin` — wired via CMake `HAILO_FW_BLOB=path` option (Phase 3); still waiting for the blob itself.

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

**Phase 5 is split into four sub-tracks; 5.1 lands without hardware, 5.2 completes the control-channel transport on real silicon (IDENTIFY round-trip), and 5.3/5.4 add the remaining opcodes needed for weight upload and inference.**

#### Phase 5.1: HEF tensor metadata ✅ (2026-04-18, software-only)

- `struct hef_info` extended with `op_count`, `pad_count`, and a bounded `pads[HEF_PARSER_MAX_PADS]` array recording each I/O pad's `index`, `name`, `is_input` flag, and tensor dims (`height`, `width`, `features` + padded variants). Captured for the first network group only — the loader runs one NG at a time.
- Callback chain extended to `ProtoHEFHef → NetworkGroup → Op → Pad → TensorShape`. Oneof awareness: the `shape_info` oneof shares a callback slot between `tensor_shape` (tag 6) and `nms_shape` (tag 7); the callback filters by `field->tag` so an NMS pad doesn't get mis-decoded as tensor dims.
- `hailo load <path>` now prints per-pad lines like `in pad[0] "input_layer1" shape=224x224x3 (padded 224x224x4)`.
- Seven new `test_hef_parser.c` tests cover: pad-with-shape decode, multi-pad ordering, truncation, no-shape pad, NMS-branch skip, second-NG pad isolation, pad-name truncation.

#### Phase 5.2: Control-channel RPC transport ✅ tier-1 + tier-2 + tier-3 (2026-04-18, hardware-verified)

- `kernel/ai_accel/hailo/hailo_control.{c,h}` implements the HailoRT control-channel wire protocol — MD5-stamped request/response over BAR4 with a BCS_ISTATUS_HOST SW_IRQ completion.
- **Tier-1 (IDENTIFY)**: empty-payload request, response carries firmware version + board metadata. Proved every piece of the transport.
- **Tier-2 (WRITE_MEMORY + READ_MEMORY)**: parameterized address/length opcodes with 1 KB chunking that matches HailoRT's `CONTROL__MAX_WRITE_MEMORY_CHUNK_SIZE`. Shell bindings `hailo peek <addr> [len]` and `hailo poke <addr> <u32>` exercise both.
- **Tier-3 (CONFIG_STREAM)**: PCIe input and output variants (the only communication_type relevant for the AI HAT+; UDP / MIPI / INTER_CPU are for other SKUs). `hailo cfgstream <in|out> <ch>` shell binding fires a minimal probe; response carries the firmware-assigned `dataflow_manager_id`.
- Hardware proof on pi-5-1:
  - IDENTIFY — `hailo fw` returns `firmware 4.23.536870912` (0x20000000), matching the boot-log fingerprint across three consecutive runs.
  - WRITE/READ_MEMORY — both opcodes round-trip against firmware with correct BE wire format and response parsing; firmware returns `major_status=0x40000058` for arbitrary-address access without an active stream context (expected HailoRT behavior).
  - CONFIG_STREAM — both input and output variants round-trip, firmware returns `major_status=0x40030050, minor_status=0x40030005` for our minimal skip_nn_stream_config probe (requires a real `.hef`'s context-switch state to accept). Observed firmware convention: on rejection it returns `opcode_echo=0xFFFFFFFF` rather than mirroring the request opcode; the driver checks `major_status` first so this surfaces as `HAILO_ERR_IO` with diagnostic codes, not `HAILO_ERR_BAD_FIRMWARE`.
- Four non-obvious wire-format gotchas surfaced during bring-up and are recorded in `docs/reference/hailo-driver-notes.md` §4.5 and the `hailo_control_wire_gotchas` auto-memory: big-endian header scalars, IMASK-before-ISTATUS unmask, FW_CONTROL-bit-specific polling, and the 4-byte `parameter_count` gap between response header and body (plus `__packed` on the body struct).
- 36 QEMU-mocked tests in `test_hailo.c` cover the transport — 7 IDENTIFY cases (`test_control_identify_*`) + 22 WRITE/READ_MEMORY cases (`test_control_{write,read}_memory_*`) + 7 CONFIG_STREAM cases (`test_control_config_stream_*`). The mock has a 4 KB smart backing store that simulates firmware memory so WRITE pattern → READ back round-trips can be asserted locally.

#### Phase 5.3: `hailo_load` with weight DMA ✅ software-complete (2026-04-18)

- **CCW action extraction (done):** `hef_parser.c` walks `network_group[0].preliminary_config.operation[].actions[].write_data_ccw` and records each action's `(data_offset_in_blob, data_size, cfg_channel_index)` into `struct hef_info`. Data bytes are NOT copied — the offset points into the caller-owned HEF blob so multi-megabyte weight sections don't balloon hef_info. Cap at `HEF_PARSER_MAX_CCW_ACTIONS=256` with a truncation flag; `ccw_total_bytes` sums across all actions.
- **DMA tensor buffer API (done):** `hailo_tensor.{c,h}` wraps platform `dma_alloc` / `dma_free` / `cache_clean` / `cache_invalidate` into `struct hailo_tensor`. Page-aligned (4 KB), zero-init, size-from-shape with overflow guard. `prepare_for_device` / `prepare_for_host` fire the host↔device cache-maintenance hooks (no-ops on NC memory today).
- **CCW upload loop (done):** `hailo_control_upload_ccw(info, blob_base, device_base_addr, &uploaded)` walks `info->ccw_actions[]` and issues `WRITE_MEMORY` for each, appending at `device_base_addr + cumulative_bytes`. Rejects null args, truncated info, and address wrap before touching the transport. Per-action failure bubbles up the first non-OK rc (caller handles all-or-nothing if needed).
- **CONFIG_STREAM opcode (done, via PR #299 folded in):** `hailo_control_config_stream_pcie()` sets up a PCIe input or output stream and obtains firmware's assigned `dataflow_manager_id`. Full BE wire format + HailoRT-quirk replication (`htons` on u32 `periph_buffers_per_frame` / `buffer_padding_payload`, native-LE `desc_page_size`). `hailo cfgstream <in|out> <ch>` shell exercises the round-trip.
- **Shell:** `hailo load <path>` now prints `ccw: N action(s), total X bytes`. Adding `upload <hex-base>` kicks off the CCW upload right after the parse — useful once a real `.hef` is in the lab.
- **Tests:** 27 new QEMU cases — 7 in `test_hef_parser.c` (synthetic-HEF extraction), 12 in `test_hailo.c` (tensor), 8 in `test_hailo.c` (CCW upload via smart-memory backing store with WRITE→READ round-trips).
- **Hardware gate:** end-to-end verification awaits a compiled `mobilenet_v1.hef` + a successful `CONFIG_STREAM` with real HEF-derived parameters (current minimal-probe `hailo cfgstream in 0` gets `major=0x40030050` rejection — expected without HEF context). Code is structured so the first HEF arrival exercises the full path without scaffolding changes.

#### Phase 5.4: `hailo infer` ✅ software-complete (2026-04-18)

- **VDMA descriptor list allocator** (`hailo_vdma.{c,h}`) — power-of-2-sized lists in [2, 65536], 64 KB-aligned via platform `dma_alloc`, zero-init so unprogrammed entries are inert.
- **Descriptor programming** — `hailo_vdma_program_descriptor` packs the 16-byte wire layout (page_size << 8 | 0x02 | addr_l & 0xFFFFFFC0 | data_id | addr_h). `hailo_vdma_program_buffer` slices a contiguous DMA buffer across the list with a residue-sized last descriptor, handling circular wrap.
- **Channel start/stop/submit** — `hailo_vdma_channel_start` writes DEPTH_ID / ADDR_L / ADDR_H / CONTROL(=START) to BAR2 register blocks at `channel_index << 5`. `hailo_vdma_channel_stop` issues ABORT_PAUSE with an "already stopped" short-circuit. `hailo_vdma_submit_and_wait` publishes `num_avail` to the base dword bits [31:16] and polls `num_proc` (BAR2 offset 0x04) on 100 µs intervals until match or timeout.
- **Orchestrator** (`hailo_infer.{c,h}`) — `hailo_infer_run(cfg, input, output, *elapsed_us)` allocates tensor buffers, allocates + programs two descriptor lists, starts both channels, submits input-first, waits on output, cache-maintains both directions, and cleans up via a single goto-label. Latency measured via CNTPCT.
- **Shell** — `hailo infer <hex-bytes>` runs the pipeline with channels 0/1 and data_id 0 against a zeroed synthetic tensor (cap 64 KB).
- **Tests** — 36 new QEMU cases in this step (11 allocator + 9 programming + 9 channel/submit + 7 infer orchestrator). Mock grows a `mock_bar2[4096]` backing + `mock_vdma_auto_advance` flag that mirrors `num_avail` writes into `num_proc` so end-to-end tests complete.
- **Hardware**: pi-5-1 smoke-tested. `hailo infer 0x400` runs the full pipeline, returns `HAILO_ERR_TIMEOUT (-4)` at output-submit — expected, since firmware has no active stream context; a real CONFIG_STREAM from a compiled `.hef` will supply data_id + start the inference engine.
- **Hardware gate** (shared with Phase 5.3): end-to-end verification awaits a compiled `.hef` + CONFIG_STREAM success. Latency histogram (#196) and the "Hailo-8L on Pi 5" bench-doc row can only land once real inference produces real numbers.

### Phase 6: AI Scheduler Integration (1–2 weeks)

#### Phase 6.1: Scheduler MLP → `.hef` toolchain ✅ (2026-04-18)

- **ONNX export** (`scripts/hailo/export_scheduler_mlp_onnx.py`) — parses the C99 hex-float arrays in `kernel/sched/ai/ai_weights_mlp.c`, reconstructs the 4-layer MLP (108 → 256 → 256 → 128 → N), and emits two opset-11 ONNX files: `scheduler_mlp_jetson.onnx` (42 actions, full layer-3) and `scheduler_mlp_pi5.onnx` (24 actions, sliced layer-3). Gemm + Relu only — DFC-friendly op set. Verified against a pure-numpy reference forward pass (max |ONNX − numpy| well below float32 ULP at layer-1 intermediate magnitudes).
- **Calibration dataset** (`scripts/hailo/generate_calibration_data.py`) — synthesizes 256 scheduler state vectors matching `ai_state.c:ai_extract_state`'s normalized feature layout exactly. Mixes 4-core (Pi 5) and 6-core (Jetson) samples. Deterministic under seed so re-runs produce identical calibration data.
- **DFC compile wrapper** (`scripts/hailo/compile_hef.sh`) — 3-phase DFC 3.33.1 pipeline (parser onnx → optimize with calibration → compiler) with `--arch hailo8|hailo8l` and `--variant pi5|jetson`. Produces `build/hailo/scheduler_mlp_{variant}.hef` (~633/644 KB int8-quantized).
- **Compiled artifacts** — both `.hef` files build cleanly on DFC 3.33.1 against Hailo-8. Compilation uses 4 of 8 clusters with ~40% control / 21% compute / 13% memory utilization on the tiny network. `.hef` files are not checked in (`build/` is gitignored) — reproduce via the three scripts above.
- **Test coverage** — 14 Python functional tests in `scripts/hailo/test_hailo_scripts.py` exercise the hex-float parser (round-trip + missing-symbol error), the numpy reference forward, ONNX model validation, full end-to-end exports on the real checked-in weights, calibration-data shape/dtype/range/determinism/zero-fill, and all four `compile_hef.sh` error paths (bad arch, bad variant, missing hailo CLI, missing ONNX input).
- **Dev-env setup docs** — `docs/setup.md` §Hailo Toolchain documents the Python 3.10 venv, pygraphviz C-header packages, Hailo Developer Zone wheel download, DFC 3.33.1 install, and `scripts/hailo/` invocation. Three troubleshooting entries cover the Python-3.12 pin miss, pygraphviz `Python.h` error, and DFC 5.x vs 3.x wheel-track confusion.

#### Phase 6.2: Kernel integration ✅ (2026-04-19)

**6.2a — inference_device backend + policy plumbing:**

- **`inference_device` Hailo backend** (`kernel/inference/inference_device_hailo.c`) — vtable bridge from the Phase 2 `inference_device` abstraction to the Phase 5.4 `hailo_infer_run` orchestrator. `load_model` parses the HEF outer header + proto body, extracts input/output pad shapes, and allocates a fixed-size slot (4 max). `run` size-checks the INT8 tensors and forwards. `free_model` releases the slot. Registered as `"hailo-8"` on Pi 5 and Jetson (non-x86 builds); inert on QEMU + x86 where the Hailo platform shim never installs.
- **`ai_policy_hailo` scheduler policy** (`kernel/sched/ai/sched_ai.c`) — mirrors `ai_mlp` but routes through `"hailo-8"`. Maintains its own handle slot set by the shell via `ai_policy_hailo_set_model` (public in `ai_policy_hailo.h`). Pre-inference quantizes the fp32 state vector to INT8 with a per-tensor scale+zero-point; post-inference argmax on the INT8 output (monotonic under dequant so no need to float-ify logits). Falls back to the heuristic policy on `assign_cpu` when no device is present or no model is loaded — the policy switch still succeeds so the user can load a model after switching, and QEMU/x86 users get a loud warning explaining that CPU fallback is in effect.
- **Shell wire-up** — `hailo load <path> sched` loads the HEF into the `"hailo-8"` device and installs the handle into `ai_policy_hailo`. `sched policy ai_hailo` activates the policy at runtime.

**6.2b — HEF edge-layer metadata extraction (`hef_parser.c`):**

- Extended the nanopb callback tree to walk the first network group's `contexts[].metadata.edge_layers[]`. Each edge layer is joined back to the matching pad by `pad_index`.
- Captures per-tensor quantization: `qp_scale` + `qp_zp` from `ProtoHEFEdgeLayerNumericInfo` (stored as IEEE-754 bit patterns in `struct hef_pad_info.qp_scale_raw` / `qp_zp_raw` so `hef_parser.c` stays compiled with `-mgeneral-regs-only`).
- Captures per-tensor stream config: `sys_index` (→ VDMA `data_id`), `core_bytes_per_buffer` (→ descriptor page size), `core_buffers_per_frame` from `ProtoHEFEdgeLayerBase`.
- `inference_device_hailo.c::load_model` now prefers HEF-derived `data_id` + `page_size` over placeholder defaults — each loaded HEF configures its slot's `hailo_infer_config` with real firmware-readable values.
- `ai_policy_hailo_set_model_from_raw` installs HEF-derived quantization in the policy; the shell's `hailo load <path> sched` path falls back to placeholder quant (1/128 scale) only when a pad lacks `quant_info`.

**6.2c — hardware verify on pi-5-1:**

Deployed `AI_SCHED=ON HAILO_FW_BLOB=.../hailo8_fw.bin PLATFORM=RASPI5` kernel. Verified on real hardware:

- `sched policy` lists 4 policies (heuristic, ai_mlp, ai_ppo, ai_hailo).
- `sched policy ai_hailo` activates cleanly with the expected "no model loaded" WARN + the "Scheduler policy: heuristic -> ai_hailo" confirmation. No crash — scheduler keeps running, falling back to heuristic per design.
- `bench sched-policy` runs successfully: `cpu-mlp` completes 1000/1000 forward passes at **40 µs per decision (25 021 decisions/sec)** on Cortex-A76. `hailo-8` branch correctly prints `(no model loaded — hailo load <hef> first)` since the VFS-side `.hef` isn't present.
- `hailo probe` + `hailo boot` continue working as in Phase 5 (firmware 4.23 rev 0x20000000).

**6.2d — `bench sched-policy` benchmark:**

New shell subcommand measures decisions/sec for each backend in the inference-device registry. Pi 5 numbers above; QEMU and Jetson numbers await their respective lab passes.

**HEF delivery path landed:** `SCHEDULER_HEF_BLOB=path/to/scheduler_mlp_pi5.hef` at build time `.incbin`s the file into the kernel (`kernel/src/sched_hef_embed.S` + `sched_hef_init.c`). At boot, `sched_hef_init` writes the embedded bytes into `/mnt/files/scheduler_mlp.hef` so `hailo load /mnt/files/scheduler_mlp.hef sched` can reach them. Stub-compiles when the option is unset — zero kernel-image impact for builds that don't embed.

**V2 HEF outer-header support landed (bonus):** DFC 3.33.1 emits HEF v2 binaries whose trailer is 32 bytes (not 20 as our original guess), and whose CCWS size is implicit (file size minus proto_end). `hef_header.c` now parses v2 correctly — confirmed against a real DFC 3.33.1 output: proto body starts at offset 44, top-level proto fields (hw_arch, sdk_version, network_groups) decode cleanly.

**Edge-layer extraction is primary source now:** `hef_parser.c::decode_edge_layer_cb` was extended to CREATE pad entries when `ops[]` is empty (not just back-fill existing ones). Direction (input/output), tensor shape (height/width/features), stream info (sys_index, core_bytes), and quantization (scale/zp) all come out of `contexts[].metadata.edge_layers[]` when the ops path is unused. Unit-tested via `test_hef_parser_edge_layer_creates_pad_when_ops_empty`.

**Fix (2026-04-19, pi5-hef-preliminary-config branch):** DFC 3.33.1 simple MLPs DO populate `contexts[].metadata.edge_layers[]` — an earlier hex-trace error (miscalculated `preliminary_config` end offset by 32 bytes) made it look like only `preliminary_config` was used. With the arithmetic corrected, the real HEF has a 2509-byte `contexts[]` field following `preliminary_config`, containing two `ProtoHEFEdgeLayer` entries (input + output) with full `edge_layer_base` shapes (1×1×108 input, 1×1×24 output), `sys_index` values, and `numeric_info.qp_scale` quantization.

The actual bug was our parser's requirement that `pad_index` (field 7) be present before creating a pad entry. DFC 3.33.1 doesn't emit `pad_index` on boundary edge_layers, and also doesn't emit `direction` on input layers (proto3 strips zero-valued scalars; direction=HOST_TO_DEVICE=0 is the default). With pad_index and direction both absent, our parser bailed at `!seen_pad_index → return true`.

Fix is minimal: in `decode_edge_layer_cb`, fall back to `edge_layer_base.sys_index` as the pad key when `pad_index` is absent, and default `is_input` to true when `direction` is unseen (matching the proto3 default). 2 new QEMU tests pin the behavior (`test_hef_parser_edge_layer_uses_sys_index_when_pad_index_absent`, `test_hef_parser_edge_layer_direction_1_means_output`).

**Hardware verify on pi-5-1 (2026-04-19):**

```
slmos> hailo load /mnt/files/scheduler_mlp.hef sched
hailo: hef v2 proto_size=425092 (total 633464 bytes)
  hw_arch = hailo8l (1)
  sdk_version = 3.33.1
  network_groups = 1
  first NG ops = 0, pads captured = 2
    in pad[1] "<unnamed>" shape=1x1x108 (padded 1x1x108)
      stream: sys_index=1 core_bytes=864 core_buffers=1
      quant: scale_raw=0x3b808081 zp_raw=0x00000000
    out pad[0] "<unnamed>" shape=1x1x24 (padded 1x1x24)
      stream: sys_index=0 core_bytes=24 core_buffers=1
      quant: scale_raw=0x3e565ffd zp_raw=0x430c0000
hailo: sched: model loaded (handle=1), ai_policy_hailo armed with HEF quant
slmos> bench sched-policy
  Input dim: 108, Output dim: 24 (AI_SCHED_N_ACTIONS)
  cpu-mlp     1000/1000 ok   39469 ns/decision   25336 decisions/sec
  hailo-8     0/1000 ok   0 ns/decision   0 decisions/sec
```

Parser fix is working: both input + output pads extracted with full HEF-derived stream config and quant info. Policy armed. `AI_SCHED_N_ACTIONS=24` picked up correctly on Pi 5 (previously 42 from the Jetson default in `ai_types.h`).

**CCW + CONFIG_STREAM chain wired into `load_model` (2026-04-19):** `inference_device_hailo::load_model` now iterates HEF CCW actions via `hailo_control_upload_ccw`, then fires `hailo_control_config_stream_pcie` for input + output. Works end-to-end against mock firmware (test suite exercises it).

**New parser work — `write_data_ccw_ptr` (v2+):** DFC 3.33.1 emits weight CCWs as `ProtoHEFActionWriteDataCcwPtr` (field 16 of the action oneof) — offset+size pointers into the separate CCWS block that follows the proto body. Added nanopb decoder for this variant; `struct hef_ccw_action.is_ccw_ptr` tells the uploader to resolve `data_offset_in_blob` against `ccws_base = hef_file + outer.ccws_offset` instead of against the proto body. Our scheduler_mlp_pi5.hef yields **46 CCW_PTR actions totalling 208 328 bytes** — matches the CCWS block's physical size.

**Next real blocker — CONTEXT_SWITCH protocol (not CCW delivery itself):**

Hardware verify on pi-5-1 with the full chain — CCW upload + CONFIG_STREAM per stream — reveals that both fail at the firmware layer:

```
[WARN] hailo: WRITE_MEMORY failed (major=0x40030098 minor=0x40030098 opcode_echo=0x1)
[WARN] hailo backend: CCW upload failed (rc=-3 after 0 bytes) — v2+ HEFs require context-switch protocol
[WARN] hailo: CONFIG_STREAM failed (major=0x40030050 minor=0x40030005 opcode_echo=0xffffffff)
[WARN] hailo backend: CONFIG_STREAM best-effort failed (in rc=-3, out rc=-3) — slot stays live; inference will time out.
hailo: sched: model loaded (handle=1), ai_policy_hailo armed with HEF quant
```

Our `hailo_control_upload_ccw` uses `WRITE_MEMORY` — a generic "poke bytes at address X" RPC that works for v0/v1 HEFs' inline CCWs but not for v2+ where weights go via the firmware's own context-switch dataflow path. The real HailoRT driver at `libhailort/src/hef/hef_*.cpp` uses two different control opcodes:

- `CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER` — declares a network group to firmware
- `CONTEXT_SWITCH_SET_CONTEXT_INFO` — sends an action list that firmware's context switcher executes, INCLUDING pulling CCW payloads from host-side DMA buffers

Same reason CONFIG_STREAM fails (`0x40030050` = `STREAM__INVALID_CONFIG_STREAM_INDEX`): firmware doesn't know the streams exist because `SET_CONTEXT_INFO` hasn't been sent yet.

**Scope assessment:** Implementing `CONTEXT_SWITCH_SET_CONTEXT_INFO` is a full new subsystem — action-list packing + CCW DMA buffer management + maybe `CORE_IDENTIFY` + bridge opcodes. Estimate 500-1000 more LoC, needs a clean proto-level understanding of the action stream we're encoding. Not Phase 6.2 scope; tracked as the natural next phase (call it 6.3).

**Best-effort design choice (2026-04-19):** upload + CONFIG_STREAM are now non-fatal — warnings log, slot stays alive, `bench sched-policy` runs through the Hailo path and reports `0/1000 ok` with 10 ms timeouts per decision. That's the CORRECT signal for "pipeline complete, firmware has no context". When Phase 6.3 lands CONTEXT_SWITCH, the WARN lines flip to INFO and `hailo-8` numbers show up.

**Cross-platform:** QEMU, Pi 5, Jetson, x86-64 all build clean under AI_SCHED=ON. 195 Hailo+HEF tests pass in both AI_SCHED=OFF and ON modes.

**Platform override for AI_SCHED_N_ACTIONS (ai_types.h):** `#if defined(PLATFORM_RASPI5)` → 24, else → 42. The in-tree MLP weights are 42-action; Pi 5 reads only the first 24 rows of w3. Matches the 24-action `scheduler_mlp_pi5.hef` produced by `scripts/hailo/compile_hef.sh --variant pi5`.

**Weight-array decoupling (ai_weights.h):** layer-3 extern declarations now use `AI_MLP_LAYER3_MAX_ROWS=42` instead of `AI_MLP_LAYER3_OUT=AI_SCHED_N_ACTIONS`, so the Pi 5 override doesn't conflict with the physical `[42 × 128]` size in `ai_weights_mlp.c`.

**Test coverage summary** (34 new cases vs pre-Phase-6.2 baseline):

- **6.2 PR #304 (merged):** 29 cases — 22 in test_hailo.c (backend + policy + early edge-layer paths), 3 in test_hef.c (v2 outer header), 4 policy (set/get/clear/set_from_raw).
- **6.2 continuation (this PR):** 5 more — 2 edge-layer parser tests (`sys_index` fallback when `pad_index` absent; `direction` proto3-default), 2 CCW_PTR upload tests (ccws_base resolution happy path + NULL rejection), 1 load_model tolerates upload/config_stream failure (best-effort slot stays live).

Cross-platform: QEMU ARM64, Pi 5 (PLATFORM=RASPI5), Jetson Orin Nano, x86-64 all build clean under `AI_SCHED=ON`. Full suite green in both `make test` (AI_SCHED=OFF) and `make test AI_SCHED=ON` modes.

#### Phase 6.3: Context-switch transport layer ✅ (2026-04-19)

Ships the three CORE-CPU context-switch opcodes to firmware:

- `CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER` (0x20) — declares a network group
- `CONTEXT_SWITCH_SET_CONTEXT_INFO` (0x21) — per-context action-list bytes, chunked at 1461 B
- `CHANGE_CONTEXT_SWITCH_STATUS` (0x25) — drives the firmware state machine between RESET and ENABLED

New transport primitive: `hailo_control_send_recv_cpu(cpu_id, ...)` routes the doorbell write to bit 0 (APP CPU) or bit 1 (CORE CPU) of `raise_ready_offset`; context-switch opcodes live on the CORE CPU while every tier-1/2/3 opcode (IDENTIFY, WRITE_MEMORY, CONFIG_STREAM, …) stays on APP CPU.

**Hardware validation (pi-5-1, 2026-04-19):** The `hailo ctxsmoke` shell command exercises the full chain against firmware v4.23:

```
slmos> hailo ctxsmoke
hailo: ctxsmoke:
  [1/3] CHANGE_CONTEXT_SWITCH_STATUS(RESET)...        rc=0
  [2/3] SET_NETWORK_GROUP_HEADER...                   rc=0
  [3/3] SET_CONTEXT_INFO (preliminary, 5-byte HALT)
        major=0x4013006e minor=0x4013006e opcode_echo=0x21
        rc=-3
```

Transport works end-to-end on real hardware. Remaining rejection is at the firmware's action-list parser (the HALT placeholder is a 5-byte common_action_header only — not a valid context composition); unblocking it requires the **Phase 6.4 translator** below.

**Firmware v4.23 struct-size lesson:** `application_header_t` is 32 bytes on v4.23 firmware (3 INFER bools + 4 config channels), not the 53 bytes the newer cached reference header shows (4 INFER bools + 24 config channels). First hardware iteration returned `major=0x40030060` = `CONTROL_PROTOCOL_STATUS_INVALID_CONTEXT_SWITCH_APP_HEADER_LENGTH`. Also: `external_action_list_address=0` is interpreted as a valid DDR pointer and rejected — use `HAILO_CS_NO_DDR_ACTION_LIST` (0xFFFFFFFF) for the control-channel path.

**Test coverage — 10 new cases:**
- 2 CORE CPU doorbell routing (send_recv_cpu goes to bit 1; send_recv default goes to bit 0)
- 3 SET_NETWORK_GROUP_HEADER (wire format byte-for-byte, null rejection, bad config count rejection)
- 5 SET_CONTEXT_INFO (single-chunk wire format, multi-chunk 3000-byte payload with is_first/is_last flags, zero-length single-chunk path, oversize rejection, null-with-nonzero-len rejection)

### Phase 6.4: HEF → action-list translator (in progress, 2026-04-19)

The natural continuation of 6.3. HEF proto stores structured `ProtoHEFAction` oneof messages (WriteDataCcw, EnableLcu, TriggerSequencer, …). HailoRT translates these into the wire-format action stream defined in `docs/reference/hailort-context_switch_defs.h` (45 action types, repeated-action compression, common-header + per-type body, `host_buffer_info_t` embedded for CCW DMA pulls). SLM-OS needs to implement the same translator.

#### What landed in 6.4a–d

**`kernel/ai_accel/hailo/hailo_cs_actions.h`** — host-side mirrors of the wire structs:

- `enum hailo_cs_action_type` — all 45 action-type values (v4.23 order).
- `struct hailo_cs_common_action_header` — **8 bytes on the wire** (1-byte action_type + 3 pad + 4-byte time_stamp). This is a firmware-discovered fact; see "Key finding" below.
- `struct hailo_cs_host_buffer_info` (19 B) — embedded in `ACTIVATE_*` actions for firmware to DMA-pull from host buffers.
- `struct hailo_cs_stream_reg_info` (19 B) — NN-stream buffer geometry.
- Preliminary-context bodies: `ACTIVATE_CFG_CHANNEL` (21 B), `FETCH_CCW_BURSTS` (3 B), `DEACTIVATE_CFG_CHANNEL` (2 B).

Each struct has a `_Static_assert` on its expected wire size to catch accidental packing drift.

**`kernel/ai_accel/hailo/hailo_cs_builder.{c,h}`** — append-only byte accumulator for building per-context action streams:

```c
struct hailo_cs_builder b;
hailo_cs_builder_init(&b, buf, sizeof(buf));
hailo_cs_builder_append(&b, HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL, &body, sizeof(body));
hailo_cs_builder_append(&b, HAILO_CS_ACT_FETCH_CCW_BURSTS,     &body, sizeof(body));
hailo_control_set_context_info(HAILO_CS_CONTEXT_TYPE_PRELIMINARY,
                               hailo_cs_builder_data(&b),
                               hailo_cs_builder_size(&b));
```

**`hailo ctxsmoke` shell command** — exercises the full 6-step bring-up chain (RESET → SET_NETWORK_GROUP_HEADER → 4 SET_CONTEXT_INFO calls) against live firmware with per-context minimum action stubs. Used for hardware-level wire-format validation; kept as a permanent diagnostic probe.

#### Key finding — `common_action_header_t` is **8 bytes**, not 5

Firmware v4.23 compiles `CONTEXT_SWITCH_DEFS__common_action_header_t` with natural alignment despite the `#pragma pack(push, 1)` wrapping the reference header. The packed u8 `action_type` is followed by 3 pad bytes, then the u32 `time_stamp` — total 8 bytes on the wire. 5-byte headers produce `major=0x40130016` = `CONTEXT_SWITCH_STATUS_MISALIGNMENT_ERROR_WHILE_READING_ACTIONS`; 8-byte headers are accepted.

Root cause is firmware-internal (likely compiled without the pragma visible, or a wrapper struct re-imposes alignment). Takeaway: **cross-check wire struct sizes against hardware, don't trust the pragma**. Body structs (`host_buffer_info_t`, `activate_cfg_channel_t`, etc.) still appear to be 1-byte packed.

#### Hardware iteration on pi-5-1 fw v4.23 (2026-04-19)

Each iteration consumed one firmware rejection code to decode the next layer:

| Attempt | Context payload | Firmware response | Decode |
|---|---|---|---|
| PRELIMINARY-first, no other contexts | 34 B (ACTIVATE_CFG_CHANNEL + FETCH_CCW_BURSTS) | `0x4013006e` | UNEXPECTED_CONTEXT_ORDER |
| All 4 contexts, stub APPLICATION_CHANGE_INTERRUPT bodies | 5 B each + 34 B preliminary | `0x40130016` on every call | MISALIGNMENT_ERROR_WHILE_READING_ACTIONS (5-byte header wrong) |
| All 4 contexts with legal per-type actions, 5-byte header | 5–34 B | `0x40130016` ACTIVATION/BATCH, `0x40130018` PRELIMINARY | Header size (5 vs 8) |
| All 4 contexts, **8-byte common header** | 8–40 B | **ACTIVATION rc=0** ✓ | First legal context accepted by firmware |

Subsequent `SET_CONTEXT_INFO` calls (BATCH_SWITCHING, PRELIMINARY, DYNAMIC) fail with truncated/invalid responses after ACTIVATION succeeds — firmware's internal state machine advances after the first accepted context, and our tight-loop of RPCs lands before firmware's response buffer has finished processing. This is the next frontier and needs investigation into firmware response-buffer timing or an explicit inter-context poll.

#### Per-context minimum actions (confirmed from HailoRT `resource_manager_builder.cpp`)

| Context | Minimum actions | Why |
|---|---|---|
| `ACTIVATION` | `BURST_CREDITS_TASK_RESET` (zero-body) | Resets HW credit counters before any stream activates |
| `BATCH_SWITCHING` | `DDR_BUFFERING_RESET` + `BURST_CREDITS_TASK_START` (both zero-body) | Resets DDR state; starts the credit-task thread |
| `PRELIMINARY` | `ACTIVATE_CFG_CHANNEL` + `FETCH_CCW_BURSTS` (+ optionally `DEACTIVATE_CFG_CHANNEL`) | Binds config VDMA; pulls CCW payloads into firmware |
| `DYNAMIC` | `APPLICATION_CHANGE_INTERRUPT` (zero-body, tail only) | Legal tail marker for single-context inference |

`APPLICATION_CHANGE_INTERRUPT` is zero-body and is **only** legal at the tail of the final DYNAMIC context — not in ACTIVATION/BATCH_SWITCHING/PRELIMINARY. Sending it in the wrong context returns MISALIGNMENT because firmware's walker doesn't expect it there.

#### Phase 6.4e landed (2026-04-19): HEF parser captures compute-action inventory

`hef_parser` now walks `ProtoHEFContext.operations[].actions[]` and records per-context action summaries into `hef_info.context_actions[]`: `context_index`, `action_count`, `action_type_mask` (bitmap keyed on ProtoHEFAction oneof field numbers), and a capped-size `action_types[]` array preserving decode order. 6 unit tests; caps of `HEF_PARSER_MAX_CONTEXTS=8` and `HEF_PARSER_MAX_CONTEXT_ACTIONS=64`; overflow flags per-context and at the top level.

Per-action parameter extraction (`packed_lcu_id` for `EnableLcu`, `cluster_index` for `TriggerSequencer`, etc.) is deliberately deferred — the current summary is enough to dispatch on action type in the translator.

#### Phase 6.4f landed (2026-04-19): HEF → wire-action translator (skeleton)

`kernel/ai_accel/hailo/hailo_cs_translator.{c,h}` composes over `hailo_cs_builder` and emits the four per-context action byte streams plus the 32-byte `application_header`. Per-context minimums match HailoRT's `fill_*_context_recipes` (v4.23 source).

Hardware-verified on pi-5-1 (2026-04-19) — the translator-driven `ctxsmoke` produces **identical firmware behavior** to the pre-refactor hand-rolled version: `ACTIVATION rc=0`, same downstream truncated-response signal on BATCH_SWITCHING. Byte-level equivalence end-to-end.

What's still a skeleton: the `DYNAMIC` context carries only `APPLICATION_CHANGE_INTERRUPT` as its tail marker — no compute actions translated from the HEF's `operations[].actions[]` yet. Firmware accepts this structurally, but a real inference would need EnableLcu / TriggerSequencer / AllowInputDataflow translated, which requires per-action parameter extraction in `hef_parser` first.

#### Phase 6.4: MSI-driven control response notification (2026-04-19)

Replaces the 100µs-polled `BCS_ISTATUS_HOST` loop in `wait_for_response` with an MSI-signaled wait, matching HailoRT's architecture. A new `control_msi_handler` runs in ISR context: reads + write-1-to-clears ISTATUS, sets an atomic `control_msi_pending` flag when `FW_CONTROL_IRQ` fires. `wait_for_response` checks the flag first (zero-overhead acquire on hit), falls back to ISTATUS polling for platforms without `register_irq` (stub, test mock absent this hook).

MSI handler is registered lazily on first `control_arm_interrupts` call via `hailo_platform->register_irq`. Platforms without it stay on the polling fallback — no regression.

Pre-doorbell in `send_recv_locked` now clears **both** a stale `BCS_ISTATUS_HOST` and the `control_msi_pending` flag, so any signal observed during `wait_for_response` belongs to THIS RPC. Closes a race where the MSI handler and polling path both see a given completion, with the second to fire leaving state for the next RPC to mistakenly consume.

Hardware result on pi-5-1 fw v4.23: MSI fires within ~2s of the doorbell for every CORE-CPU RPC (`hailo: MSI vector 287 bound` in boot log; BATCH_SWITCHING exits `wait_for_response` at ~2s instead of timing out at 10s).

#### Phase 6.4g landed (2026-04-19): EnableLcu per-action parameter extraction

Extends `hef_parser`'s compute-action walker to capture `ProtoHEFActionEnableLcu` (oneof tag 8) parameters into `hef_info.enable_lcu_actions[]`. Each entry records all six scalar fields (`lcu_index`, `cluster_index`, `kernel_done_address`, `kernel_done_count`, `lcu_enable_address`, `network_index`) plus the source `context_index`.

Translator emits the matching wire action per captured entry:
- `ENABLE_LCU_DEFAULT` (2 B body) when `kernel_done_count == 0 && kernel_done_address == 0`
- `ENABLE_LCU_NON_DEFAULT` (8 B body) otherwise

`packed_lcu_id = (cluster_index << 4) | (lcu_index & 0xF)` via `hailo_cs_pack_lcu_id()` helper — matches HailoRT's convention.

Pattern is the template for future action types (DisableLcu, TriggerSequencer, WaitForSequencer, AllowInputDataflow): add a `hef_<action>_action` struct, a `decode_<action>_body` function, a dispatcher case in `decode_compute_action_inner_cb`, a wire struct + `action_type` in `hailo_cs_actions.h`, and a `translate_<action>` call from `translate_dynamic`.

#### What's still needed for real inference

1. **Firmware responds with garbage to CORE-CPU RPCs after ACTIVATION.** Even with MSI delivering the response-ready signal within 2s, `BAR4+0x640` reads as `buffer_len=0xFFFFFFFF`. Driver-side fixes (stale-ISTATUS clear, one-shot ATR retarget, pre-doorbell MSI-pending clear, timeout extension, MSI itself) exhausted. Suspect the `BURST_CREDITS_TASK_RESET`-only ACTIVATION is not rich enough content to let firmware's state machine transition cleanly — once 6.4h adds the real boundary-channel actions per-edge-layer that HailoRT's `fill_activation_config_recepies` emits, firmware may settle. See memory note `hailo_core_cpu_response_content.md` for the full matrix of what was tried.
2. **Additional per-action parameter extraction.** Phase 6.4g shipped EnableLcu; the same pattern needs to extend to DisableLcu, TriggerSequencer, WaitForSequencer, AllowInputDataflow before the DYNAMIC context carries real compute.
3. **Richer ACTIVATION/BATCH_SWITCHING contexts.** Current stubs (`BURST_CREDITS_TASK_RESET`, `DDR_BUFFERING_RESET + BURST_CREDITS_TASK_START`) pass firmware's parser but are minimum-viable. HailoRT's real emission adds `OpenBoundaryInput`/`OpenBoundaryOutput` per boundary edge layer in ACTIVATION; these need an edge-layer → VDMA-channel mapping we haven't built yet.
4. **Wire translator into `inference_device_hailo::load_model`.** Replace the best-effort WRITE_MEMORY + CONFIG_STREAM path with the SET_NETWORK_GROUP_HEADER + 4 SET_CONTEXT_INFO chain. Gated on (1), (2), (3).

#### Test coverage — Phase 6.4 cumulative (beyond 6.3's 10)

Builder (5):
- `test_cs_builder_append_emits_header_then_body` — single-action byte-for-byte layout including the 3 pad bytes at offsets 1–3.
- `test_cs_builder_appends_concatenate` — 3-action preliminary-context sequence with `ACTIVATE_CFG_CHANNEL` + `FETCH_CCW_BURSTS` + `DEACTIVATE_CFG_CHANNEL`, asserts action_type bytes + host_buffer_info DMA-address offset.
- `test_cs_builder_returns_nomem_on_overflow` — buffer capacity enforced.
- `test_cs_builder_rejects_null_buffer` — null-pointer API check.
- `test_cs_builder_accepts_zero_body_action` — `BURST_CREDITS_TASK_RESET`-style zero-body actions produce an 8-byte header-only entry.

Change-context-status (2):
- `test_change_context_switch_status_reset_wire_layout` — RESET state transition byte-for-byte.
- `test_change_context_switch_status_enabled_carries_batch_params` — ENABLED carries the batch-size/batch-count fields.

MSI response path (2):
- `test_control_registers_msi_on_first_send` — first control RPC triggers `register_irq`; subsequent RPCs don't re-register (one-shot gating).
- `test_msi_handler_sets_pending_and_clears_istatus` — directly-invoked MSI handler W1Cs the `FW_CONTROL_IRQ` bit (preseeded via `mock_istatus_one_shot_preload`) so the next polling iteration sees a clean state.

HEF parser context-actions walker, 6.4e (6):
- `test_decode_context_actions_single_action` — 1-context / 1-action HEF → `action_types[0]==8`, `mask==(1<<8)`.
- `test_decode_context_actions_mixed_types` — 5 different action kinds in order; order preserved, mask OR'd.
- `test_decode_context_actions_multiple_contexts` — 2 contexts each get their own slot.
- `test_decode_context_actions_overflow_truncates` — MAX+1 actions in one context; `truncated` flag set.
- `test_decode_context_actions_context_overflow` — MAX+1 contexts; top-level `truncated` flag set.
- `test_decode_context_actions_no_contexts` — NG with no `contexts[]` field.

HEF parser EnableLcu extraction, 6.4g (3):
- `test_decode_enable_lcu_captures_all_fields` — distinct non-default values for all 6 scalars land in the right slots.
- `test_decode_enable_lcu_defaults_zero_when_absent` — proto3 default semantics preserved.
- `test_decode_enable_lcu_tracks_context_index` — multi-context HEF correctly records which context each action belongs to.

Translator application_header + contexts, 6.4f (6):
- `test_cs_translate_application_header_fills_defaults` — verifies every derived field in the 32-byte header.
- `test_cs_translate_application_header_rejects_null` — null-arg.
- `test_cs_translate_contexts_produces_all_four` — byte-for-byte assertion of all four context streams (lengths 8/16/40/8, action_type bytes at expected offsets, `host_buffer_info.dma_address` round-trip).
- `test_cs_translate_contexts_uses_ccw_count_for_burst_count` — `info.ccw_action_count=7` → `FETCH_CCW_BURSTS.ccw_bursts=7`.
- `test_cs_translate_contexts_clamps_burst_count_to_u16` — 100000 actions → clamped to UINT16_MAX.
- `test_cs_translate_contexts_rejects_null` — null-arg.

Translator EnableLcu, 6.4g (3):
- `test_cs_translate_enable_lcu_default_variant` — `packed_lcu_id` correctly encoded; `ENABLE_LCU_DEFAULT` emitted + tail marker.
- `test_cs_translate_enable_lcu_non_default_variant` — non-zero `kernel_done_count` switches to `ENABLE_LCU_NON_DEFAULT`; 8-byte body byte-for-byte asserted.
- `test_cs_translate_multiple_enable_lcu_preserves_order` — two EnableLcu entries emitted in HEF order ahead of the tail.

Total Phase 6.4 unit-test additions: **27 cases** across `test_hailo.c` and `test_hef_parser.c`.

**Firmware constraints pinned from v4.23:**
- Zero-length contexts are rejected with `0x40130004`; each `SET_CONTEXT_INFO` must carry ≥1 valid wire action.
- Firmware expects exactly `dynamic_contexts_count + 3` SET_CONTEXT_INFO calls per load, in fixed order: ACTIVATION, BATCH_SWITCHING, PRELIMINARY, then each DYNAMIC.
- `common_action_header_t` is 8 bytes (natural alignment, not 5 as the packed reference suggests).
- `csm_buffer_size` must match the VDMA descriptor page size (typically 512 or 4096).

#### Phase 6.5 landed (2026-04-19): Boundary channels + OpenBoundary actions

Commit `91e837c` (#178). Extended `hailo_cs_translate_cfg` with `boundary_input_desc_list_iova`, `boundary_output_desc_list_iova`, page size, and descriptor counts. `translate_activation` now walks `info.pads[]` and emits `OPEN_BOUNDARY_INPUT_CHANNEL` (action 32) + `OPEN_BOUNDARY_OUTPUT_CHANNEL` (action 33) per boundary edge alongside `BURST_CREDITS_TASK_RESET`. `boundary_channels_bitmap` in `application_header` now reflects both input and output boundary channels (shifted `& 0x1Fu` to stay within the 32-bit bitmap).

#### Phase 6.6 landed (2026-04-19): Translator wired into `load_model`

Commit `bebe4f2` (#179). `inference_device_hailo::load_model` now allocates boundary DMA tensors + descriptor lists up front and passes their IOVAs into the translator, so ACTIVATION carries real boundary channel info sourced from the caller rather than synthetic shell stubs. Both input and output host buffers are programmed into the VDMA at load time (not on each `run()`).

#### Phase 6.7 landed (2026-04-19): `run()` reuses load-time boundary resources

Commit `4f2b744` (#338). Previously `run()` allocated fresh DMA tensors + descriptor lists on every inference; now it reuses the slot's load-time allocations, eliminating 2 × `hailo_tensor_alloc` + 2 × `hailo_vdma_desc_list_alloc` per call. Boundary IOVAs and descriptor counts are cached in the slot struct.

#### Phase 6.8 landed (2026-04-20): Pre-configure handshake — `CLEAR_CONFIGURED_APPS` + `GET_HW_CONSTS`

Commits on branch `pi5-phase-6-run-rewire`. Added two firmware RPCs that HailoRT issues between `CHANGE_CONTEXT_SWITCH_STATUS(RESET)` and `SET_NETWORK_GROUP_HEADER`:

- **`CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS`** (opcode 0x47, `CPU_ID_CORE_CPU`) — empty-body RPC. Clears firmware's internal bookkeeping for previously-configured network groups so the next load starts from a clean state.
- **`GET_HW_CONSTS`** (opcode 0x48, `CPU_ID_CORE_CPU`) — empty-body RPC; firmware returns a packed struct of hardware constants (51 bytes on Hailo-8L fw v4.23). Body contents are not consumed by SLM-OS today — the call is made for the side-effect of completing firmware's pre-configure handshake.

**Byte-for-byte verified** against HailoRT v4.23's wire on pi-5-1 via instrumented `hailo_pcie_write_firmware_control` (`print_hex_dump` on the Pi OS card). Both request bodies are 20 bytes identical to HailoRT.

**Also corrected `HAILO_VDMA_MAX_CHANNELS` from 16 → 32** (`kernel/ai_accel/hailo/hailo_vdma.h`). The reference driver's `MAX_VDMA_CHANNELS_PER_ENGINE = 32`; the prior 16 cap would silently reject any future D2H channel index ≥ 16 with `HAILO_ERR_INVAL` on `hailo_vdma_channel_start`.

**Hardware result on pi-5-1 fw v4.23, 2026-04-20:** ctxsmoke now progresses cleanly through 4 RPCs before hitting ACTIVATION:
```
[1/8] RESET                        rc=0
[2/8] CLEAR_CONFIGURED_APPS        rc=0  ← NEW
[3/8] GET_HW_CONSTS                rc=0 resp_len=51  ← NEW
[4/8] SET_NETWORK_GROUP_HEADER     rc=0
[5/8] SET_CONTEXT_INFO(ACTIVATION, 72 B)  rc=-3 (major=0x402d001b)
```

**BREAKTHROUGH on #180:** Firmware no longer silent-wedges on `BATCH_SWITCHING` (BAR4 returning `0xFFFFFFFF`). It now returns a real diagnostic error code on `ACTIVATION`, which is what lets further root-causing happen.

#### Phase 6.9 remaining (2026-04-20): ACTIVATION rejected with `INVALID_ENGINE_INDEX`

ACTIVATION fails with `major_status = 0x402d001b = VDMA_SERVICE_STATUS_INVALID_ENGINE_INDEX` (module 0x2D = FIRMWARE_MODULE__VDMA_SERVICE, status 0x1B within that module — counted from `VDMA_SERVICE_START`). PRELIMINARY fails with `0x402d0004 = HOST_DESCRIPTOR_BASE_ADDRESS_IS_NOT_64KB_ALIGNED` (cascade — firmware state machine is in error after ACTIVATION).

The error name is misleading: `packed_vdma_channel_id` values emitted (`0x02` for input, `0x03` for output) have `engine_index = (packed >> 5) & 0x3 = 0`, which is the only valid engine on Hailo-8 PCIe (`HAILO_PCIE_DMA_ENGINES_COUNT = 1`). Something else in the ACTIVATION body is tripping the firmware's VDMA-service path.

**Ruled out during this session:**

- **Action-type enum IDs** — verified `OPEN_BOUNDARY_INPUT_CHANNEL = 32`, `OPEN_BOUNDARY_OUTPUT_CHANNEL = 33`, `BURST_CREDITS_TASK_RESET = 30` against `hailort-v4.23.0-context_switch_defs.h`.
- **Body layouts** — `CONTEXT_SWITCH_DEFS__open_boundary_input_channel_data_t` (28 B) and `..._output_channel_data_t` (20 B) match SLM-OS structs byte-for-byte, including `CONTROL_PROTOCOL__host_buffer_info_t` (19 B, inside a `#pragma pack(push,1)` region per control-protocol.h:273).
- **H2D/D2H channel-index split hypothesis** — initially suspected output `packed=0x03` was falling in the H2D range [0..15] and firmware was rejecting it as a D2H. `hailo-vdma-common.c:576-587` (`get_channel_regs`) and `hailo-pcie-common.h:30` (`HAILO_PCIE_DMA_ENGINES_COUNT = 1`) + `hailo-ioctl-common.h:20-21` (`MAX_VDMA_CHANNELS_PER_ENGINE = 32`, `VDMA_CHANNELS_PER_ENGINE_PER_DIRECTION = 16`) show `src_channels_bitmask = 0x0000FFFF` only selects which register pair within each channel's 32-byte window comes first (host-side vs device-side) — NOT a per-direction channel reservation. Channel-index space is a shared 0..31 pool; direction is differentiated by the action type. Hypothesis disproven by source-reading; tentative `OUTPUT_CHANNEL_OFFSET = 17` change was reverted.

**Not yet investigated (candidates for next session):**

1. **64 KB-alignment of boundary desc-list IOVAs.** `hailo_vdma_desc_list_alloc` returns page-aligned (4 KB) IOVAs. Firmware's PRELIMINARY cascade returns `HOST_DESCRIPTOR_BASE_ADDRESS_IS_NOT_64KB_ALIGNED`; it's plausible fw lumps all "descriptor-list base address" violations on boundary channels into `INVALID_ENGINE_INDEX` (since the engine-lookup table is keyed off per-channel state). Fix: route boundary desc lists through a 64 KB-aligned allocator (allocate 64 KB, align up), or find an alternate allocator API that already gives 64 KB alignment.
2. **Pre-load multi-call cadence.** HailoRT's wire capture on 2026-04-20 shows `GET_HW_CONSTS` called **twice** and an interleaved `GET_DEVICE_INFORMATION` (opcode 0x33) between `CLEAR_CONFIGURED_APPS` and the second `GET_HW_CONSTS`. SLM-OS only calls each once. Unlikely to matter semantically, but listed here since the ground-truth HEF load didn't proceed past `GET_HW_CONSTS`.
3. **Ground-truth ACTIVATION body.** Still unavailable — HailoRT v4.23.0 errors before `SET_CONTEXT_INFO` on every Model Zoo HEF tried (`HAILO_INTERNAL_FAILURE` due to Model Zoo HEFs requiring `max_desc_page_size = 16384` but HailoRT hardcodes 4096 for Hailo-8L). Next-session options: (a) patch HailoRT's `desc_page_size` guard to bypass; (b) find a HEF compiled with `page_size ≤ 4096`; (c) synthesize a minimal `SET_CONTEXT_INFO` test from first principles and bisect by swapping individual action bytes.

**Artifacts left on branch for next session:**

- `kernel/ai_accel/hailo/hailo_shell.c` — `ctxsmoke` retains post-failure BAR4 scope scan + IDENTIFY(APP) diagnostic + 500 ms inter-RPC delay. Kept as a permanent probe; removing once #180 resolves.
- `/home/pi/Downloads/hailort-drivers/common/pcie_common.c` (on pi-5-1 Pi OS card) — `print_hex_dump` patch in `hailo_pcie_write_firmware_control` captures every HailoRT RPC body on dmesg. Rebuilt driver at `/home/pi/Downloads/hailort-drivers/linux/pcie/hailo_pci.ko`; swap in via `sudo rmmod hailo_pci && sudo insmod ...`.

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

*Last updated: 2026-04-20*
