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
| Channel create / submit | — | — | ✅ inherited from L4T post-kexec; **PBDMA doorbell now reachable** via the channel-preserving kexec path (PR #740 default) | ❌ (#185 — see Skipped) |
| Compute dispatch (v7 SASS pipeline) | — | — | ✅ end-to-end — v7 + HMMA tensor-core MNIST shipped (#710 closed) | ❌ blocked at Booter Load |
| GMMU (read-only walker → multi-page alloc → TLB invalidate + safe VA reuse) | — | — | ✅ #666 milestones A / C / D landed | — |
| Operator library | — | — | ✅ embedded `operator_library.bin` via `.incbin`, parsed at boot, staged into GPU VA (#663, #717, #718) | — |
| Operator dispatcher | — | — | ✅ per-op dispatch metadata registry + cbuf builders + GMMU constant-buffer + launch shape (#714 / #730 / #732 / #737 fired) | — |
| Operator coverage on GA10B | — | — | RMSNORM, GQA_ATTN FP16 (#540), EMBEDDING.Q4K FP16 (#540), HMMA matmul; MNIST MLP fastpath | — |
| L2 cross-dispatch coherency | — | — | ✅ 3-point GA10B L2 evict on cross-dispatch boundary (#722, narrowed to post_launch-only by #723→#727 — ~40 µs/dispatch) | — |
| Channel-preserve kexec inherit | — | — | ✅ `slmos-kexec` defaults to channel-preserving + HMMA (PR #740, May 2026) | — |
| Host-side test count | — | — | 41 tests (26 bringup + 15 platform shim) + per-op golden vectors | 117 tests across 5 suites |
| Inference backend | CPU NEON (AI sched MLP) | CPU NEON | CPU NEON (default) + **GA10B GPU fastpath via the v7 dispatch loop**, gated by `gpu use inference on` | CPU SSE (GPU not in inference path) |
| Model formats supported | ONNX via rust runtime | Same | Same + GGUF via rust slm runtime | Same |
| Operator toggle surface | — | — | `gpu use inference on/off` (master), `model use-gpu <name> on/off` (per-model), `gpu use sched on/off` (WIRED — ai_mlp on GA10B via `slm_gpu_run_sched_inference`, rate-limited under bursts per #651/#653), `gpu use eviction on/off` (scaffold; warn) | Same toggle surface (master flag is no-op until GA10x dispatch lands) |

## Resolved blockers

- **#258 — Jetson GPU Phase 7 PBDMA doorbell (RESOLVED 2026-04-21).** Channel-inherit-from-Linux path bypasses the NS EL2 CBB block on `NV_USERMODE`. The kexec channel-preserving path (PR #740) is now the default `slmos-kexec` mode.
- **#190 — Jetson GSP Falcon priv-lockdown (RESOLVED 2026-04-17).** Fixed by `--no-gpu-suspend` on the kexec helper + `ga10b_bringup_inherit()` detecting Linux's post-boot FECS/GPCCS PASS state.
- **#710, #715 — v7 + HMMA cross-dispatch coherency (RESOLVED 2026-05).** GA10B's LTC was leaving stale cachelines across dispatches; #722 added a 3-point L2 evict; #723→#727 narrowed it to post_launch-only (~40 µs/dispatch, was ~120 µs). pre_launch L2 ops without `set_input` were unsafe (wedged channel) and dropped.
- **#666 — GA10B GMMU bring-up (RESOLVED 2026-04 / 05).** Milestone A (read-only walker), Milestone C (multi-page alloc / free), Milestone D (TLB invalidate + safe VA reuse + FECS inst-block discovery) all landed.

## Skipped / Blocked

- **#185 — x86-64 SEC2 priv-lockdown.** E3.4.d (Booter Load) blocked by SEC2 PLM raised above VFIO-userspace access level. Confirmed by `--sec2-plm-scan` (865/1024 offsets locked). Present at bare-metal UEFI handoff too, not just VFIO. `nouveau` clears the lock via VBIOS DEVINIT replay on the same board; porting that is beyond current scope.
- **x86-64 Phases 5-8 (RPC, compute, inference loop)** — transitively blocked on #185. Code ships and transfers cleanly to any platform where SEC2 is accessible.
- **Jetson channel/runlist *creation* from scratch (vs inherit)** — the inherit path covers the capstone deliverable. Creating channels without inheriting from Linux still hits the CBB firewall on `NV_USERMODE`. Not currently planned.
- **Jetson SLM-class operator library (full Qwen forward chain on GPU)** — the dispatcher and per-op metadata registry exist; current GPU operator coverage is RMSNORM, GQA_ATTN FP16, EMBEDDING.Q4K FP16, plus the MNIST MLP. The remaining transformer ops (RoPE, SwiGLU, LM-head) are scoped under `docs/design/gpu-policy-models.md` and `docs/design/gpu-qmd-per-dispatch.md`.
- **Pi 5 GPU (VideoCore)** — stub driver only. No NVIDIA hardware, no intent to bring up VideoCore as a compute engine. AI HAT+ (Hailo-8L) is the planned accelerator path for Pi 5 (#260).
- **QEMU GPU** — no GPU device in `virt` machine; stub driver returns "no device."

## Capabilities delivered regardless of blockers

- Portable GSP bringup core (`kernel/gpu/nvidia/`) reusable across any platform with a compatible Ampere GPU.
- FECS method gateway verified on Jetson hardware — first bare-metal SLM-OS GPU method submission (DISCOVER_IMAGE_SIZE → 513,280-byte reply).
- Operator library format + parser + builder (#663) + boot-time embed via `.incbin` (#717) + GPU VA staging (#718) + per-op dispatch registry (#714/#730) + dispatcher fire (#737).
- v7 + HMMA tensor-core MNIST end-to-end on Jetson — `gpu use inference on` runs the MLP through `nvcuda::wmma`-derived SASS.
- AI scheduler MLP (`ai_mlp`) running on GA10B via `slm_gpu_run_sched_inference` — first AI policy fully on GPU.
- Complete host-side test matrix — Falcon driver, VBIOS parser, nvfw container, GSP bringup helpers, GA10B bringup, oplib parser, per-op cbuf golden vectors.
- Detection and identification on both Jetson (GA10B, 1024 CUDA cores) and x86-64 (RTX 3050).

## See also

- `docs/nvidia-gsp.md` — Platform shim contract
- `docs/jetson-cbb-report.md` — CBB firewall impact on GPU path
- `docs/x86-64-gpu-inference-status.md` — x86-64 GPU status
- `docs/design/gpu-policy-models.md` — Plan for wiring sched + eviction policy MLPs to GA10B compute dispatch (the `gpu use sched|eviction` toggles' missing backend)
- `docs/design/admin-telemetry-suite.md` §9 — `gpu use` shell command + `gpu_consumer_set` validation contract
- `docs/archive/plans/capstone-feature-status.md` §GPU-Based Inference (narrative)
- `docs/design/gpu-qmd-per-dispatch.md` — per-dispatch QMD plan (#723/#727 narrowing notes)
- Issues: #258 / #190 / #710 / #666 / #715 (Jetson — all closed); #185 (x86-64, open)

*Last updated: 8 May 2026*
