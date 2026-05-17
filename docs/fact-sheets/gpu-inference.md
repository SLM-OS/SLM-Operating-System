# GPU / Accelerator Inference — Fact Sheet

Current inference paths and what each platform delivers today.

## Matrix

| Sub-capability | QEMU | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| GPU / accelerator hardware | — | None (VideoCore blocked) | GA10B integrated Ampere | RTX 3050 (GA107, PCIe) |
| AI HAT+ / NPU path | — | ❌ blocked at boundary IN ch=2 (#682) | — | — |
| GPU driver stub | ✅ | ✅ | Full shim + nvgpu bringup | Full shim + GSP-RM |
| Platform shim (`gsp_platform_ops`) | — | — | Complete (11/11 fns) | Complete (11/11 fns) |
| BAR0/1 MMIO access | — | — | ✅ at NS EL2 | ✅ via PCI |
| Engine reset + PIO IMEM/DMEM | — | — | Working on GSP Falcon | Working on GSP + SEC2 |
| Firmware embedded at build | — | — | 17 GA10B blobs via `.incbin` (gated on `-DGA10B_FIRMWARE_DIR`) | Discrete Ampere R535 firmware |
| Boot phases 1-5 | — | — | ✅ ACR / FECS / GPCCS / PMU skip / FECS method gateway | 🟡 E3 (FWSEC-FRTS succeeds) |
| Channel create / submit | — | — | ✅ inherited from L4T post-kexec via channel-preserving kexec path | ❌ (#185 — see Skipped) |
| Compute dispatch (v7 SASS pipeline) | — | — | ✅ end-to-end — v7 + HMMA tensor-core MNIST | ❌ blocked at Booter Load |
| GMMU (walker, multi-page alloc, TLB invalidate, safe VA reuse) | — | — | ✅ | — |
| Operator library | — | — | ✅ embedded `operator_library.bin` via `.incbin`, parsed at boot, staged into GPU VA | — |
| Operator dispatcher | — | — | ✅ per-op dispatch metadata registry + cbuf builders + GMMU constant-buffer + launch shape | — |
| Operator coverage on GA10B | — | — | RMSNORM, GQA_ATTN FP16, EMBEDDING.Q4K FP16, HMMA matmul, MNIST MLP fastpath | — |
| L2 cross-dispatch coherency | — | — | ✅ 3-point GA10B L2 evict, narrowed to post_launch-only (~40 µs/dispatch) | — |
| Channel-preserve kexec inherit | — | — | ✅ `slmos-kexec` defaults to channel-preserving + HMMA | — |
| Host-side test count | — | — | 41 tests (26 bringup + 15 platform shim) + per-op golden vectors | 117 tests across 5 suites |
| Inference backend | CPU NEON (AI sched MLP) | CPU NEON | CPU NEON (default) + **GA10B GPU fastpath via v7 dispatch loop**, gated by `gpu use inference on` | CPU SSE (GPU not in inference path) |
| Model formats supported | ONNX via rust runtime | Same | Same + GGUF via rust slm runtime | Same |
| Operator toggle surface | — | — | `gpu use inference on/off` (master), `model use-gpu <name> on/off` (per-model), `gpu use sched on/off` (ai_mlp on GA10B, rate-limited under bursts), `gpu use eviction on/off` (scaffold; warn) | Same toggle surface (master flag is no-op until GA10x dispatch lands) |

## Skipped / Blocked

- **#185 — x86-64 SEC2 priv-lockdown.** Booter Load blocked by SEC2 PLM raised above VFIO-userspace access level (865/1024 offsets locked). Present at bare-metal UEFI handoff too, not just VFIO. `nouveau` clears the lock via VBIOS DEVINIT replay; porting that is out of scope.
- **x86-64 RPC / compute / inference loop** — transitively blocked on #185. Code ships and transfers cleanly to any platform where SEC2 is accessible.
- **Jetson channel/runlist *creation* from scratch (vs inherit)** — the inherit path covers the capstone deliverable. Creating channels without inheriting from Linux still hits the CBB firewall on `NV_USERMODE`.
- **Jetson SLM-class operator library (full Qwen forward chain on GPU)** — dispatcher and per-op metadata registry exist; current coverage is RMSNORM, GQA_ATTN FP16, EMBEDDING.Q4K FP16, MNIST MLP. RoPE, SwiGLU, LM-head are scoped under `docs/design/gpu-policy-models.md` and `docs/design/gpu-qmd-per-dispatch.md`.
- **Pi 5 GPU (VideoCore)** — stub driver only. No NVIDIA hardware, no intent to bring up VideoCore as a compute engine. AI HAT+ (Hailo-8L) is the accelerator path for Pi 5 (#260).
- **#682 — Pi 5 AI HAT+ Hailo-8L boundary IN wedge.** Driver brings the chip through `state=running`, byte-identical FW_CONTROL RPCs vs HailoRT, HEF load completes (`last_err=0`), CCW upload reaches expected `num_proc`. First inference submit to boundary IN ch=2 wedges: `dev_proc` stays at 0 across the full 500 ms timeout, `dev_base[31:16]` never advances to `0x001f` (HailoRT shows fw pre-arms this during load). Correlated symptoms: `CPU_ECC_ERROR/FATAL` with `memory_bitmap=0x1000` (`SAGE1_ISP_12`) on every runtime CORE-CPU RPC (HailoRT gets 0/10 boots), and EP-internal `DEVSTA.UR=1` with `rcv_mabort=0` post-timeout (RC bridge clean). Disconfirmed: hyp-R prefetch=1, hyp-S D3 hold, hyp-T VDM QoS, hyp-V `SCB0` size widen (kept for Linux parity), hyp-X-1 IRQ drain, hyp-X-2 production build (no `HAILO_WIRE_DEBUG`), hyp-X-3 longer post-BOOT_IRQ settle (2000 ms), ATR audit. Suspect: fw-internal SAGE1_ISP scrub trigger we can't drive from the host. Awaiting Hailo support — see `docs/archive/investigations/hailo-support-ticket-draft.md`.
- **QEMU GPU** — no GPU device in `virt` machine; stub driver returns "no device."

## See also

- `docs/nvidia-gsp.md` — Platform shim contract
- `docs/jetson-cbb-report.md` — CBB firewall impact on GPU path
- `docs/x86-64-gpu-inference-status.md` — x86-64 GPU status
- `docs/design/gpu-policy-models.md` — Plan for wiring sched + eviction policy MLPs to GA10B compute dispatch
- `docs/design/admin-telemetry-suite.md` §9 — `gpu use` shell command + `gpu_consumer_set` validation contract
- `docs/design/gpu-qmd-per-dispatch.md` — per-dispatch QMD plan

*Last updated: 10 May 2026*
