# AI Eviction: Extended Feature Vector Design

Design document for extending the 27-feature eviction input vector
with kernel-side signals not available to the sibling project's
simulator.

**Status:** Design only. Requires sibling project changes + retraining.

**Tracking:** #122

---

## Current 27-Feature Layout

Features 0-14 are per-block; 15-26 are global. Defined in
`runtime/src/mm/eviction/features.rs` and introspectable via
`eviction features` (#112).

All 27 features mirror the sibling `slm-os-page-sim` simulator's
`FeatureConfig.feature_names` exactly. The trained XGBoost and MLP
models expect this layout byte-for-byte.

## Candidate Kernel-Side Signals

| Signal | Source | Expected impact | Effort |
|--------|--------|----------------|--------|
| CPU utilization (per-CPU %) | `cpu_runqueue.running_ticks / total_ticks` | High — tells the policy whether the requesting CPU is under load | Low (data already collected under AI_SCHED) |
| Scheduler tick rate | `pit_ticks` delta | Medium — measures system activity | Low |
| Active task count | `sched_stats.task_count` | Medium — proxy for memory pressure | Low |
| model_active_inferences (live) | `slm_heuristic::get_active()` via #113 | High — already wired, just needs feature slot | Low (slot 11 is placeholder 0 today) |
| Deadline urgency (max across tasks) | `task.deadline_ns - now` | Medium — eviction under deadline pressure should be conservative | Medium |
| GPU queue depth | `gpu_get_info().pending_ops` (when real GPU) | Low in QEMU (always 0); high on Jetson | Medium |
| IRQ pressure | `timer_handler_count` delta over window | Low — mostly noise | Low |

## Recommended First Extension

**Slot 11 (model_active_inferences)** and **slot 17 (num_loaded_models)**
are already defined in the feature layout but hardcoded to 0. Wiring
them requires:

1. `features.rs` reads `slm_heuristic::get_active(block.model_id)` for
   slot 11 — this is a single function call, already available via #113.
2. `features.rs` queries model registry count for slot 17.
3. Retrain on the sibling project with the new signals populated.
4. Re-import weights.

This is the lowest-risk extension because it fills existing placeholder
slots — no schema bump, no model-version change, just populating
inputs that were always in the trained model's input shape.

## Schema Bump (Future)

Adding entirely new features (beyond the existing 27 slots) requires:

1. Bump `BlockFeatures` from `[f32; 27]` to `[f32; N]`.
2. Update the sibling's `PER_BLOCK_FEATURES` / `GLOBAL_FEATURES`.
3. Retrain all models against the new schema.
4. Update `scripts/import_eviction_weights.sh` to handle the new
   weight dimensions.
5. Runtime-side version check: the generated weight files carry an
   implicit feature count; `FEATURE_NAMES.len()` must match at import
   time.

## Effort Estimate

| Phase | Work | Owner |
|-------|------|-------|
| Wire slot 11 + 17 | 1 day | Runtime (this project) |
| Retrain with new signals | 1 day | Sibling project |
| Full schema extension (5+ new features) | 1 week | Both projects |

---

*Created: 16 April 2026*
