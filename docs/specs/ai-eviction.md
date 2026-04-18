# AI Page Eviction — Fact Sheet

CACHEUS ensemble + classical + ML-based page-eviction policies.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Classical policies | LRU, LFU, MRU, random, FIFO | Same | Same | Same |
| ML-based policies | XGBoost, int8-quantized MLP | Same | Same | Same |
| Ensemble policy | CACHEUS (adaptive weighted) | Same | Same | Same |
| Feature vector dimensions | 27 | 27 | 27 | 27 |
| MLP size | ~20 KB quantized | Same | Same | Same |
| XGBoost tree count | Varies by trained model | Same | Same | Same |
| Two-pool model | Cold / hot with promotion heuristic | Same | Same | Same |
| Policy switch | Runtime via `eviction policy <name>` | Same | Same | Same |
| Online retraining | ✅ (landed per `docs/eviction-online-retraining.md`) | Same | Same | Same |
| Online update cadence | Per-eviction feedback weight adjustment | Same | Same | Same |
| Extended features | ✅ (landed per `docs/eviction-extended-features.md`) | Same | Same | Same |
| Build gate | Part of default kernel (no opt-in flag) | Same | Same | Same |
| Observability | `eviction stats`, trajectory plots via `sched trace` (#194) | Same | Same | Same |

## Skipped / Blocked

- **GPU-accelerated eviction inference** — not implemented. The int8 MLP is small enough that CPU inference is not a bottleneck. Would depend on the same GPU path as the AI scheduler (blocked on #258 / #185 on NVIDIA; planned via Hailo on Pi 5 #260).
- **Cross-platform feature-vector variation** — all platforms use the same 27-dim vector. Platform-specific features (e.g., cache topology) are not currently exposed.
- **Eviction trajectory as a Lua-readable stream** — `sched trace` surfaces events but not via `slm.eviction` Lua API yet.
- **Multi-process-space eviction** — no MMU multi-AS support yet (see [memory.md](memory.md)), so per-process eviction policy is moot.

## See also

- `docs/eviction.md` (narrative, 550+ lines)
- `docs/eviction-extended-features.md`
- `docs/eviction-online-retraining.md`
- `docs/capstone-feature-status.md` §"AI Page Eviction"

*Last updated: 18 April 2026*
