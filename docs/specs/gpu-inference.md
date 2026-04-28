# GPU / Accelerator Inference — Fact Sheet

Current inference paths and what each platform delivers today.

## Matrix

| Sub-capability | QEMU | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| GPU / accelerator hardware | — | None (VideoCore blocked) | GA10B integrated Ampere | RTX 3050 (GA107, PCIe) |
| AI HAT+ / NPU path | — | ⏸️ planned (#260) | — | — |
| GPU driver stub | ✅ | ✅ | Full shim + nvgpu bringup | Full shim + GSP-RM |
| Platform shim (`gsp_platform_ops`) | — | — | Complete (11/11 fns) | Complete (11/11 fns) |
| BAR0/1 MMIO access | — | — | ✅ at NS EL2 | ✅ via PCI |
| Engine reset + PIO IMEM/DMEM | — | — | Working on GSP Falcon | Working on GSP + SEC2 |
| Firmware embedded at build | — | — | 17 GA10B blobs via `.incbin` (gated on `-DGA10B_FIRMWARE_DIR`) | Discrete Ampere R535 firmware |
| Boot phases 1-5 | — | — | ✅ ACR / FECS / GPCCS / PMU skip / FECS method gateway | 🟡 E3 (FWSEC-FRTS succeeds) |
| Channel create / submit | — | — | ❌ (#258 — see Skipped) | ❌ (#185 — see Skipped) |
| Compute dispatch | — | — | ❌ blocked at channel submit | ❌ blocked at Booter Load |
| Host-side test count | — | — | 41 tests (26 bringup + 15 platform shim) | 117 tests across 5 suites |
| Inference backend | CPU NEON (AI sched MLP) | CPU NEON | CPU NEON (default) + GA10B GPU fastpath for MNIST when `gpu use inference on` + `--no-gpu-suspend` kexec | CPU SSE (GPU not in inference path) |
| Model formats supported | ONNX via rust runtime | Same | Same | Same |
| Operator toggle surface | — | — | `gpu use inference on/off` (master, MNIST), `model use-gpu <name> on/off` (per-model), `gpu use sched on/off` (WIRED — ai_mlp on GA10B via `slm_gpu_run_sched_inference`, PR-3 of `gpu-policy-models.md`), `gpu use eviction on/off` (scaffold; warn) | Same toggle surface (master flag is no-op until GA10x dispatch lands) |

## Skipped / Blocked

- **#258 — Jetson GPU Phase 7: PBDMA doorbell blocked.** Channel inherit from Linux (Phase 6) works end-to-end — SLM-OS writes `GP_PUT` to USERD in DRAM and the write lands — but `NV_USERMODE` doorbell aperture (BAR0 + `0x800000`) is CBB-firewalled at NS EL2. Reads return `0xbadf1100` PRI poison. PBDMA never advances GP_GET. No software path to ring the doorbell from NS EL2.
- **#185 — x86-64 SEC2 priv-lockdown.** E3.4.d (Booter Load) blocked by SEC2 PLM raised above VFIO-userspace access level. Confirmed by `--sec2-plm-scan` (865/1024 offsets locked). Present at bare-metal UEFI handoff too, not just VFIO. `nouveau` clears the lock via VBIOS DEVINIT replay on the same board; porting that is beyond current scope.
- **#190 — Jetson GSP Falcon priv-lockdown** (resolved 2026-04-17). Fixed by `--no-gpu-suspend` flag on kexec helper + `ga10b_bringup_inherit()` path that detects Linux's post-boot FECS/GPCCS PASS state.
- **x86-64 Phases 5-8 (RPC, compute, inference loop)** — transitively blocked on #185. Code ships and transfers cleanly to any platform where SEC2 is accessible.
- **Jetson Phases 6-8 beyond inherit** — blocked on #258. Remaining work (SASS kernel, QMD dispatch, inference loop) cannot be reached until the doorbell path is open.
- **Jetson CBB firewall apertures for GPU channel setup** — PFIFO (`0x002000`), CHRAM (channel enable/disable), NV_USERMODE (`0x800000`), per-runlist PRI config. All locked at NS EL2. See `docs/jetson-cbb-report.md` §5.
- **Pi 5 GPU (VideoCore)** — stub driver only. No NVIDIA hardware, no intent to bring up VideoCore as a compute engine. AI HAT+ (Hailo-8L) is the planned accelerator path for Pi 5 (#260).
- **QEMU GPU** — no GPU device in `virt` machine; stub driver returns "no device."

## Capabilities delivered regardless of blockers

- Portable GSP bringup core (`kernel/gpu/nvidia/`) reusable across any platform with a compatible Ampere GPU.
- FECS method gateway verified on Jetson hardware — first bare-metal SLM-OS GPU method submission (DISCOVER_IMAGE_SIZE → 513,280-byte reply).
- Complete host-side test matrix — Falcon driver, VBIOS parser, nvfw container, GSP bringup helpers, GA10B bringup.
- Detection and identification on both Jetson (GA10B, 1024 CUDA cores) and x86-64 (RTX 3050).

## See also

- `docs/nvidia-gsp.md` — Platform shim contract
- `docs/jetson-cbb-report.md` — CBB firewall impact on GPU path
- `docs/x86-64-gpu-inference-status.md` — x86-64 GPU status
- `docs/specs/gpu-policy-models.md` — Plan for wiring sched + eviction policy MLPs to GA10B compute dispatch (the `gpu use sched|eviction` toggles' missing backend)
- `docs/specs/admin-telemetry-suite.md` §9 — `gpu use` shell command + `gpu_consumer_set` validation contract
- `docs/archive/plans/capstone-feature-status.md` §GPU-Based Inference (narrative)
- Issues: #258 (Jetson — closed 2026-04-21), #185 (x86-64), #190 (resolved)

*Last updated: 26 April 2026*
