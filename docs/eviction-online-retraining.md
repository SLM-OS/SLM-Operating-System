# AI Eviction: Online Retraining Architecture

Design document for closing the loop between runtime eviction
decisions and model weight updates. Currently, eviction policy
weights are imported once via `scripts/import_eviction_weights.sh`
from the sibling `slm-os-page-sim` training pipeline. This document
describes an architecture for adapting those weights from real
workload traces collected on the running OS.

**Status:** Design only. Not implemented.

**Tracking:** #119

---

## Problem

The trained XGBoost / MLP weights were optimized on the sibling
project's synthetic workload mix. If the production workload
diverges (different model sizes, access patterns, or eviction
pressure), the trained policy may under-perform relative to a
classical baseline that has no training-distribution assumption.

CACHEUS's online learning partially addresses this — expert weights
adapt via multiplicative updates at each feedback event — but the
underlying XGBoost / MLP decision boundaries are frozen.

## Proposed Architecture

```
+─────────────────────+     +──────────────────────+
│  Kernel runtime     │     │  Training pipeline   │
│  (alloc / evict /   │     │  (offline or         │
│   access events)    │     │   background task)   │
│                     │     │                      │
│  ┌───────────────┐  │     │  ┌────────────────┐  │
│  │ Trace ring    │──┼──→──┼──│ Feature extract │  │
│  │ (per-event)   │  │     │  └───────┬────────┘  │
│  └───────────────┘  │     │          │           │
│                     │     │  ┌───────▼────────┐  │
│  ┌───────────────┐  │     │  │ XGBoost / MLP  │  │
│  │ Flush to VFS  │──┼──→──┼──│ retraining     │  │
│  │ (periodic)    │  │     │  └───────┬────────┘  │
│  └───────────────┘  │     │          │           │
│                     │     │  ┌───────▼────────┐  │
│  ┌───────────────┐  │     │  │ Weight export  │  │
│  │ Weight reload │──┼──←──┼──│ (C arrays)     │  │
│  │ (hot-swap)    │  │     │  └────────────────┘  │
│  └───────────────┘  │     +──────────────────────+
+─────────────────────+
```

### Phase 1: Trace Collection

Add a kernel-side ring buffer that records every allocator event:

| Field | Size | Description |
|-------|------|-------------|
| timestamp_ns | 8 B | CNTPCT-based wall clock |
| event_type | 1 B | ALLOC / FREE / EVICT / ACCESS / FAULT |
| pool_type | 1 B | Weight / Workspace |
| model_id | 1 B | Model registry index |
| layer_idx | 2 B | Layer within model |
| block_id | 4 B | Pool slot identifier |

Ring size: 64 KB (4096 events at 16 bytes each). Overflow
overwrites oldest entries.

Periodic flush writes the ring to `/mnt/files/traces/eviction.bin`
via VFS (LittleFS). Flush can be triggered by:
- Timer tick (every N seconds)
- Pool-pressure threshold (>90% utilization)
- Shell command (`eviction trace flush`)

### Phase 2: Offline Retraining

The sibling project's training pipeline
(`slm-os-page-sim/scripts/train_*.py`) consumes CSV traces. A
converter reads `eviction.bin` and emits the CSV format.

Retraining runs on the development host (not on the target):

```bash
# On host:
scp pi5:/mnt/files/traces/eviction.bin .
python3 tools/convert_trace.py eviction.bin > trace.csv
python3 scripts/train_xgboost.py --input trace.csv
python3 scripts/train_mlp.py --input trace.csv
bash scripts/import_eviction_weights.sh
make kernel EVICTION_MODELS=ON
```

### Phase 3: Hot-Swap Weight Reload (Future)

Add an FFI hook that reloads the generated C-array weights at
runtime without rebooting:

```rust
/// Replace the active XGBoost / MLP weights from a buffer.
/// The new weights take effect on the next select_victim call.
pub fn reload_weights(xgb: &[u8], mlp: &[u8]) -> Result<(), Error>;
```

This requires:
- A stable serialization format for the weight arrays
- Validation that the new weights match the expected feature count
- Atomic swap under the registry lock

### Phase 4: On-Target Training (Research)

Run a stripped-down training loop as a kernel task:
- Gradient-free optimization (evolutionary strategy or Bayesian
  optimization) for the XGBoost split thresholds
- Fixed MLP architecture; retrain only the weight matrix
- Compute budget: 100 ms per retraining epoch, triggered every
  10,000 eviction decisions

This is the most speculative phase. Feasibility depends on:
- Available heap memory for training state (~256 KB)
- FP arithmetic cost on ARM64 without hardware FP acceleration
  (the kernel compiles with `-mgeneral-regs-only`; training
  would need to run in a Rust task with full FP context)
- Whether a 100 ms training window produces meaningful improvement

## Milestones

| Phase | Effort | Deliverable |
|-------|--------|-------------|
| 1 — Trace collection | 1 week | Ring buffer + VFS flush + shell command |
| 2 — Offline retraining | 1 week | Host-side converter + integration with sibling pipeline |
| 3 — Hot-swap reload | 1 week | FFI + validation + atomic swap |
| 4 — On-target training | 2-4 weeks | Kernel training task + convergence experiments |

Phases 1-2 are feasible for a follow-up sprint. Phases 3-4 are
research-stage.

---

*Created: 16 April 2026*
