# AI Task Scheduler — Fact Sheet

MLP, PPO, and XGBoost scheduling policies running in the kernel
scheduler. Hailo-8 NPU offload is available on Pi 5. All four are
runtime-selectable via `sched_set_policy(name)` — see #848 for the
"pluggable policies as first-class" framing.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Policy registry | `kernel/include/sched_policy.h` vtable | Same | Same | Same |
| Policies registered | round-robin (baseline), `ai_mlp`, `ai_ppo`, `ai_xgb` | + `ai_hailo` (NPU) | + `ai_hailo` reg'd, no model | round-robin / `ai_mlp` / `ai_ppo` / `ai_xgb` (no `ai_hailo`) |
| Runtime policy switch | `sched_set_policy(name)` shell cmd `sched policy <name>` | Same | Same | Same |
| State vector dimensions | 108 (6 cores × 6 + 8 tasks × 8 + 8 global) | 108 | 108 | 108 |
| Action space | 24 (platform-specific) | 24 | 42 (6 cores × 7 actions) | 24 |
| MLP architecture | Fully-connected, 2 hidden layers, ReLU | Same | Same | Same |
| PPO architecture | Actor-critic (MLP policy head + value head) | Same | Same | Same |
| XGBoost architecture | 3-classifier cascade (`core` → `priority(+core)` → `preempt(+core+priority)`) | Same (CPU-id clamping on 4 cores) | Same | Same |
| Weight storage | Embedded via `ai_weights_mlp.c` / `ai_weights_ppo.c`; XGBoost cascade is **runtime-only** (`xgb_sched.smb` blob) | Same | Same | Same |
| Weight format | float32, auto-generated C arrays. XGBoost: SEMB+XGBC binary blob (~9 MB on trained model). | Same | Same | Same |
| Binary size impact | +1-3 MB with `AI_SCHED=ON` (XGBoost cascade adds 0 — runtime-loaded) | Same | Same | Same |
| SIMD backend (CPU) | NEON (AArch64) | NEON | NEON | SSE (`kernel/arch/x86_64/sse_kernels.c`, `-msse -msse2`) |
| GPU backend (`ai_mlp` on Ampere) | — | — | ✅ shipped — `slm_gpu_run_sched_inference` runs the MLP on GA10B; toggled via `gpu use sched on/off`; rate-limited under bursts (#651/#653) | — |
| FFI path to runtime | extern-C from `ops.rs` | Same | Same | Same |
| Forward-pass file | `kernel/sched/ai/ai_inference.c` | Same | Same | Same |
| Decision trigger | Every `scheduler_tick()` | Hardware tick (opt-in `SECONDARY_PREEMPT=ON`) / synthesized at yield (default) | Same (synthesized at yield) | Every tick |
| Inference latency target | <50 μs on Cortex-A76 | Same | Same (A78AE) | <50 μs on Skylake |
| Build gate | `AI_SCHED=ON` → `ENABLE_AI_SCHEDULER=ON` | Same | Same | Same |
| Observability | `sched stats`, AI-policy counter | Same | Same | Same |

## State-vector `cache_pressure` (#872)

Per-core `cache_pressure` (feature index `c*6+2` in the state vector)
is now live on ARM64 — it carries the L1D miss-rate EWMA fed from each
CPU's own PMU sample, normalised to [0.0, 1.0] against a 100K-misses-
per-million-cycles saturation ceiling. Sampler hooks into
`scheduler_tick` so every CPU updates its own slot independently;
`ai_state.c` reads via `pmu_get_cache_pressure_q16()` and converts to
float in the FP-clean compile unit. Pre-#872 this slot was hardcoded
to 0.0.

**Distribution-shift caveat (training data).** The shipped MLP / PPO
weights were trained with `cache_pressure = 0.0` baked in (synthetic
training pipeline, sibling repo `~/projects/slm-os-scheduler-ai`).
Flipping the feature to a real value is a mild input-distribution
shift — the policy still sees the same other 107 features and is
free to learn-or-ignore the new signal. The exploratory-OS framing
(#848) accepts the shift rather than gating the feature behind a
build flag; if `bench sched-policy` regresses materially, a #61
follow-up will retrain.

x86-64 still reads 0.0 for this slot (sibling work #870).

## Skipped / Blocked

- **GPU-backed scheduler inference on Pi 5 / x86-64** — path described in `docs/pi5-ai-hat-plan.md` §6 (Hailo MLP on Pi 5). On Pi 5 it remains gated by the AI HAT+ link-training work (#260). On x86-64 it's transitively blocked by SEC2 priv-lock (#185). Jetson GA10B is shipped (see matrix).
- **Online weight update / training in-kernel** — not implemented. Weights are compile-time frozen. Plan B was to reuse the CACHEUS online-retraining infrastructure but it's separate from the scheduler MLP today.
- **Full Plan A exported weights real-workload validation** — listed in `docs/future-work.md` §"Real AI Scheduler Weights." The trained weights are integrated; the "realistic-workload benchmark comparison" between AI and heuristic is partially done (`bench sched-policy` exists, with `ai_xgb` row added in #851 once a cascade is staged).
- **On-target bit-equality test against Python `TripleClassifier.predict`** — sibling-repo `expected_actions_xgb.bin` ships 1000 ground-truth `(core, priority, preempt)` triples. Stage the corresponding `xgb_sched.smb`, replay the test vectors through `slm.sched_set_policy("ai_xgb")` from the shell, and diff. A scripted shell verb is the smallest follow-on; the kernel-side `rust_run_tests` covers a 3-classifier synthetic cascade today.
- **Automatic policy selection per workload** — `sched_set_policy` requires an explicit name. No workload-adaptive auto-switch.
- **Multi-dim per-task features beyond the fixed 8** — encoded slot is capped at 8 tasks. Tasks beyond that are dropped from the state vector (not prioritized). No plan to expand.

## See also

- `docs/scheduler.md` §AI scheduling
- `docs/cross-platform-inference-bench.md` — per-platform inference latency
- `kernel/sched/ai/ai_types.h` — state/action encoding
- `kernel/sched/ai/ai_inference.h` — forward-pass API

*Last updated: 15 May 2026*
