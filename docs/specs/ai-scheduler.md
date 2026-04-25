# AI Task Scheduler — Fact Sheet

MLP and PPO scheduling policies running in the kernel scheduler.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Policy registry | `kernel/include/sched_policy.h` vtable | Same | Same | Same |
| Policies registered | round-robin (baseline), `ai_mlp`, `ai_ppo` | Same | Same | Same |
| Runtime policy switch | `sched_set_policy(name)` shell cmd `sched policy <name>` | Same | Same | Same |
| State vector dimensions | 108 (6 cores × 6 + 8 tasks × 8 + 8 global) | 108 | 108 | 108 |
| Action space | 24 (platform-specific) | 24 | 42 (6 cores × 7 actions) | 24 |
| MLP architecture | Fully-connected, 2 hidden layers, ReLU | Same | Same | Same |
| PPO architecture | Actor-critic (MLP policy head + value head) | Same | Same | Same |
| Weight storage | Embedded via `ai_weights_mlp.c` / `ai_weights_ppo.c` | Same | Same | Same |
| Weight format | float32, auto-generated C arrays | Same | Same | Same |
| Binary size impact | +1-3 MB with `AI_SCHED=ON` | Same | Same | Same |
| SIMD backend | NEON (AArch64) | NEON | NEON | SSE (`kernel/arch/x86_64/sse_kernels.c`, `-msse -msse2`) |
| FFI path to runtime | extern-C from `ops.rs` | Same | Same | Same |
| Forward-pass file | `kernel/sched/ai/ai_inference.c` | Same | Same | Same |
| Decision trigger | Every `scheduler_tick()` | Same (synthesized at yield) | Same (synthesized at yield) | Every tick |
| Inference latency target | <50 μs on Cortex-A76 | Same | Same (A78AE) | <50 μs on Skylake |
| Build gate | `AI_SCHED=ON` → `ENABLE_AI_SCHEDULER=ON` | Same | Same | Same |
| Observability | `sched stats`, AI-policy counter | Same | Same | Same |

## Skipped / Blocked

- **GPU / accelerator-backed inference for the scheduler** — path described in `docs/pi5-ai-hat-plan.md` §6 (Hailo MLP on Pi 5) and implied for Jetson via the inherit-channel GPU path. Today the scheduler MLP always runs on CPU via NEON/SSE. GPU execution remains gated on the same blockers as main GPU inference (#258, #185).
- **Online weight update / training in-kernel** — not implemented. Weights are compile-time frozen. Plan B was to reuse the CACHEUS online-retraining infrastructure but it's separate from the scheduler MLP today.
- **Full Plan A exported weights real-workload validation** — listed in `docs/future-work.md` §"Real AI Scheduler Weights." The trained weights are integrated; the "realistic-workload benchmark comparison" between AI and heuristic is partially done (`bench sched-policy` exists).
- **Automatic policy selection per workload** — `sched_set_policy` requires an explicit name. No workload-adaptive auto-switch.
- **Multi-dim per-task features beyond the fixed 8** — encoded slot is capped at 8 tasks. Tasks beyond that are dropped from the state vector (not prioritized). No plan to expand.

## See also

- `docs/scheduler.md` §AI scheduling
- `docs/cross-platform-inference-bench.md` — per-platform inference latency
- `kernel/sched/ai/ai_types.h` — state/action encoding
- `kernel/sched/ai/ai_inference.h` — forward-pass API
- `docs/archive/plans/capstone-feature-status.md` §"AI Task Scheduling"

*Last updated: 18 April 2026*
