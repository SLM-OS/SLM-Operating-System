# SLM-OS Design Specs

Engineering proposals and design specs for forward-looking work.

**Not** fact sheets — fact sheets describe what the codebase does *today*
across platforms and live in [`../fact-sheets/`](../fact-sheets/). These
documents describe what we *intend to build*, the rationale, and the
trade-offs being weighed. Many will move into permanent capability
coverage (a fact sheet) once shipped; some will be archived if the
proposal is dropped.

If a doc you want to add answers "does X work on platform Y?", it
belongs in `../fact-sheets/`. If it answers "how should we approach
building X?" or "here is the proposed shape of feature Y", it belongs
here.

## Current design specs

| Doc | Subject |
|---|---|
| [admin-telemetry-suite.md](admin-telemetry-suite.md) | Operator-facing admin + telemetry surface for live SLM-OS shells (per-consumer enable/disable, hot-swap of policies, streaming telemetry feed). |
| [gpu-per-op-dispatch.md](gpu-per-op-dispatch.md) | Replace name-based whole-graph GPU eligibility with graph-aware per-op dispatch. |
| [gpu-policy-models.md](gpu-policy-models.md) | Wire AI-scheduler and page-eviction policy models to the same GA10B compute-dispatch path used by the MNIST inference engine. |
| [gpu-slm-handoff.md](gpu-slm-handoff.md) | Pre-kexec SLM model staging + bare-metal pushbuffer dispatch for the SLM integration milestones (M6.A → M6.D). |

## See also

- [`../fact-sheets/`](../fact-sheets/) — descriptive per-capability cross-platform coverage.
- [`../contracts/`](../contracts/) — stable interface contracts (formats, layouts).
- [`../plans/`](../plans/) — multi-phase plans of record (delivery milestones, not capability docs).
