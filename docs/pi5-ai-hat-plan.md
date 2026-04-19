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

**Known gap — DFC 3.33.1 simple MLPs use neither `ops[]` nor `contexts[]`:**

Hand-decoding the scheduler_mlp_pi5.hef wire format on real hardware (hexdump of the proto body at offset 44) revealed that DFC 3.33.1 for a simple 108→24 MLP packs everything into `preliminary_config` (3063-byte field 2 inside the first network group). No `ops[]`, no `contexts[]`. Both pad-extraction paths our parser supports come up empty, so `hailo load <path> sched` decodes the outer header + top-level proto fields (hw_arch, sdk_version, network_groups=1) but reports pad_count=0, and the backend's `load_model` correctly rejects with `INF_ERR_BAD_MODEL`.

Extracting pad/stream/quant info from inside `preliminary_config.operation[].actions[]` requires a new callback chain (actions already do decode `write_data_ccw` for weight upload in Phase 5.3 — one of the 18 oneof branches). Which specific action type carries the pad metadata for this HEF revision needs fresh investigation against HailoRT source. That's a standalone follow-up; the Phase 6.2 plumbing + policy path is all in place and validated up to the pad-shape-unavailable point.

**Test coverage** — 29 new QEMU cases:

- `test_hailo.c` (22 cases): 13 backend tests (register / load happy + error paths / run dtype + size + handle rejection / auto-advance happy / free / shutdown / threads HEF stream info), 5 policy tests (set/get handle + detach + set_from_raw happy + zero-scale reject + NaN reject), 4 edge-layer extraction tests (quant capture, create-if-missing, back-fill shape on shapeless pad, end-to-end load-with-stream-info).
- `test_hef.c` (3 cases): v2 header accepts with/without trailing CCWS; v2 rejects short trailer. Covers the new 32-byte v2 trailer parsing path against hand-built binaries.

Cross-platform: QEMU ARM64, Pi 5 (PLATFORM=RASPI5), Jetson Orin Nano, x86-64 all build clean under `AI_SCHED=ON`. Full suite green in both `make test` (AI_SCHED=OFF) and `make test AI_SCHED=ON` modes.

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
