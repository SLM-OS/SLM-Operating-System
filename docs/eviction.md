# AI-Driven Page Eviction

The `ModelAllocator` uses a pluggable `EvictionPolicy` trait to decide
which 2 MB block to evict when the weight or workspace pool is full.
Phase AI-Eviction lands the trait, the classical policies (LRU, LFU,
ARC, SLM-Heuristic), the trained XGBoost and int8 MLP predictors, and
the CACHEUS adaptive ensemble.

Source of truth: `docs/TODO - PHASE AI-Eviction.md`. This document
covers build flags, the ML import flow, and the runtime layout of the
eviction subsystem.

---

## Build Flags

Two tiers, both off by default:

| Flag | Adds | Binary cost |
|------|------|-------------|
| `AI_EVICTION=ON` | Trait, registry, classical policies, stub ML predictors (`xgb_stub`, `mlp_stub` return 0.5) | ~30 KB |
| `AI_EVICTION_MODELS=ON` | Trained XGBoost (~1.3 MB source) + int8 MLP (~5 KB) | still ~30 KB until M4 calls them; LTO drops the unused weight tables |

`AI_EVICTION_MODELS=ON` implies `AI_EVICTION=ON` — the Makefile auto-
promotes the flag. CMake enforces the dependency at configure time:
setting `ENABLE_AI_EVICTION_MODELS=ON` without `ENABLE_AI_EVICTION=ON`
aborts with a `FATAL_ERROR`.

### Build Examples

```bash
# Default: no AI eviction code
make kernel

# Pluggable trait + classical policies (stubs for XGBoost/MLP)
make kernel AI_EVICTION=ON

# Full: real trained models (requires import step first)
./scripts/import_eviction_weights.sh
make kernel AI_EVICTION_MODELS=ON

# Combined with AI scheduler
make kernel AI_SCHED=ON AI_EVICTION=ON
```

The Cargo feature names match the Makefile flags: `ai_eviction` and
`ai_eviction_models`. Building the runtime crate directly:

```bash
cd runtime
cargo build --target aarch64-unknown-none                          # default
cargo build --target aarch64-unknown-none --features ai_eviction   # stub
cargo build --target aarch64-unknown-none --features ai_eviction_models
```

---

## Importing Trained Weights

The trained models live in the sibling `slm-os-page-sim` project.
Run its export pipeline first, then stage the files into the runtime:

```bash
# 1. In the sibling project, regenerate the exports after any retrain.
cd ~/projects/slm-os-page-sim
python scripts/export_to_slmos.py --output-dir data/export/
python scripts/verify_rust_export.py   # cross-checks Python vs Rust

# 2. In this repo, pull them into runtime/src/mm/eviction/generated/.
cd ~/projects/CS-496-Capstone-SLM-Operating-System
./scripts/import_eviction_weights.sh
```

`import_eviction_weights.sh` does three things:

1. **Copies** `xgb_policy_generated.rs`, `mlp_policy_generated.rs`, and
   the optional `mlp_policy_f32.rs` (used by cross-check tests) from
   the sibling's `data/export/` into `runtime/src/mm/eviction/generated/`.
2. **Rewrites** the sibling's `use crate::mm::eviction_policy::*` path
   to the runtime's `use crate::mm::eviction::policy::*`, and
   substitutes `(expr).exp()` with `libm::expf(expr)` so the code is
   `no_std`-safe. A second comment line is prepended recording the
   import timestamp.
3. **Validates** that `pub fn xgb_predict`, `pub fn mlp_predict`, and
   the MLP weight constants (`W_L1`, `W_L2`, `W_L3`, `W_OUT`) are
   present. Missing required symbols abort the import; missing optional
   ones print a warning.

Pass `--source <path>` to override the default source directory
(`~/projects/slm-os-page-sim/data/export/`).

---

## Runtime Layout

```
runtime/src/mm/eviction/
├── mod.rs           # init(), public re-exports (gated on `ai_eviction`)
├── policy.rs        # EvictionPolicy trait, BlockMeta, PoolType,
│                    # BlockFeatures = [f32; 27]
├── registry.rs      # ACTIVE_POLICY (default LruPolicy),
│                    # FirstCandidatePolicy (test-only trivial policy),
│                    # set/get/with_active_policy,
│                    # select_victim / score / update_feedback helpers
├── lru.rs           # LruPolicy — ported from sibling Rust crate
├── lfu.rs           # LfuPolicy — ported from sibling Rust crate
├── slm_heuristic.rs # SlmHeuristicPolicy — ported from sibling Rust crate
├── arc.rs           # ARCPolicy — translated from sibling Python
├── features.rs      # extract_features — 15 per-block + 12 global features,
│                    # normalised to match the simulator's FeatureNormalizer
├── xgboost.rs       # XGBoostPolicy — thin wrapper over generated::xgb_predict
├── mlp.rs           # MlpPolicy — thin wrapper over generated::mlp_predict
├── cacheus.rs       # CacheusSelector — weighted expert ensemble with
│                    # multiplicative weight updates. ml_only() is the
│                    # recommended runtime configuration.
├── tracker.rs       # EvictedContentTracker — recent-eviction FIFO that
│                    # feeds CACHEUS's update_feedback when a just-
│                    # allocated content key matches a past eviction
└── generated/
    ├── mod.rs       # Selects stub vs real at compile time
    ├── xgb_stub.rs  # xgb_predict(_) -> 0.5 when models are off
    ├── mlp_stub.rs  # mlp_predict(_) -> 0.5 when models are off
    # After `import_eviction_weights.sh` runs:
    ├── xgb_policy_generated.rs   # Real XGBoost if-else chain
    ├── mlp_policy_generated.rs   # Real int8-quantized MLP
    └── mlp_policy_f32.rs         # Float32 reference (consumed by the
                                  # int8-vs-f32 cross-check test in
                                  # rust_eviction_run_tests when
                                  # ai_eviction_models is on)
```

### Classical Policies

Four classical policies, installable via `set_eviction_policy`:

| Policy | Behaviour | Source |
|--------|-----------|--------|
| `LruPolicy` | Picks the candidate with the smallest `last_access_time`; first-index tie break. `score()` returns inverse recency in [0, 1]. | Ported from sibling Rust |
| `LfuPolicy` | Picks the smallest `access_count`; LRU tie-break within the tied set. | Ported from sibling Rust |
| `SlmHeuristicPolicy` | Priority cascade: workspace → inactive-model weights → global LRU. `set_active_inferences(map)` and `bump_active(id, delta)` feed the scheduler signal. | Ported from sibling Rust |
| `ARCPolicy` | Adaptive Replacement Cache (Megiddo & Modha FAST 2003). Four LRU lists (T1, T2, B1, B2) with adaptive `p`. `notify_access` / `notify_eviction` drive the ghost-list adaptation; `update_feedback` bridges the generic trait hook. | Translated from sibling Python (`src/policies/arc.py`) |

`LruPolicy` is the default installed by `mm::eviction::init()`. M1 used a
minimal `FirstCandidatePolicy` placeholder; that is still exported
(`mm::eviction::FirstCandidatePolicy`) for tests that need a
deterministic trivial policy.

### ML Policies (M4)

| Policy | Predictor | Size | Source |
|--------|-----------|------|--------|
| `XGBoostPolicy` | `generated::xgb_predict` — 200-tree if-else chain with sigmoid | ~1.3 MB generated Rust (dead-code-eliminated until actually called) | Imported from sibling `data/export/xgb_policy_generated.rs` |
| `MlpPolicy` | `generated::mlp_predict` — int8-quantised 4-layer MLP (27→64→32→16→1, sigmoid) | ~20 KB of weights + predict fn | Imported from sibling `data/export/mlp_policy_generated.rs` |

Both policies funnel their input through `features::extract_features`,
which mirrors the sibling's `FeatureExtractor` + `FeatureNormalizer`
pipeline:

- **15 per-block features**: `recency_rank/(n-1)`, `frequency_rank/(n-1)`,
  `log1p(access_count)/log1p(1024)`, `time_since_access / 1s`,
  `time_since_load / 1s`, `ref_count`, `is_gpu_mapped`, `pool_type`,
  `is_dirty`, `layer_idx_norm`, `model_priority/7`,
  `model_active_inferences` (placeholder 0 until M5/M6 scheduler feed),
  `access_pattern` (placeholder 0 — `BlockMeta` doesn't yet track this),
  `predicted_reuse_dist` (Sequential-branch heuristic), `eviction_cost`.
- **12 global features**: `weight_pool_util`, `workspace_pool_util`,
  `total_gpu_mapped/n`, plus nine zeros reserved for the scheduler
  feed (`num_loaded_models`, `pending_loads`, `avg_model_priority`,
  `max_deadline_pressure`, `recent_fault_rate`, `hot_swap_active`, and
  the three `req_block_*` signals).

**Approximation vs the sibling training pipeline**: our runtime measures
time in real nanoseconds; the simulator uses a logical tick counter.
We normalise both `time_since_*` values against `AI_HORIZON_NS` (1 s)
to keep them in the simulator's training range. Access-count
normalisation uses a fixed `log1p(1024)` denominator rather than a
running max — acceptable because `log1p` is forgiving past the
training ceiling.

### CACHEUS Adaptive Ensemble (M5)

`CacheusSelector` combines a weighted pool of expert policies and
updates the weights online based on feedback from the
`EvictedContentTracker` (below). Per-candidate scores are the
weighted sum of expert scores; the ensemble picks the argmax. On
feedback (`update_feedback(block_id, was_fault)`):

- **was_fault = true** (evicted block was re-accessed): experts that
  agreed with the ensemble's choice are penalised by `(1 - lr)`;
  experts that disagreed get a small reward `(1 + 0.5 * lr)`.
- **was_fault = false**: agreeing experts get a reward `(1 + lr)`.
- Weights are floored at `min_weight = 0.01` and renormalised so no
  expert is silenced permanently.

Two named constructors:

| Constructor | Experts | Default use |
|-------------|---------|-------------|
| `CacheusSelector::ml_only()` | XGBoost + int8 MLP | Recommended runtime configuration. Wins the sibling Phase 5 sweep at 0.212 mean normalised fault rate. |
| `CacheusSelector::all_5()` | LRU + LFU + SLM-Heuristic + XGBoost + MLP | Ablation experiments only. Loses to `ml_only` (0.427 vs 0.212) because the classical experts dilute the ensemble on the SLM workload. |

Tuning constants (`CACHEUS_DEFAULT_LR = 0.4`,
`CACHEUS_DEFAULT_WINDOW = 200`) match the sibling's Phase 5 sweep.
Accessors (`weights`, `expert_names`, `expert_faults`,
`expert_decisions`, `history_len`) enable runtime introspection for
the upcoming `eviction stats` shell subcommand (M7).

### Eviction-Feedback Tracker

`EvictedContentTracker` records a `ContentKey`
(`pool_type`, `model_id`, `layer_idx`) alongside the evicted
`block_id` and the eviction timestamp. When the allocator later
admits a new block with the same key, `probe_on_alloc` reports a hit
and clears the entry — M6's allocator integration funnels this back
into `update_feedback(_, true)` so CACHEUS learns the eviction was
bad. Entries older than `EVICTION_FEEDBACK_WINDOW_NS` (200 ms
default) are drained as "good" evictions and trigger
`update_feedback(_, false)`.

The tracker is separate from CACHEUS so it can also drive future
non-CACHEUS feedback paths (e.g. a traced ARC variant). `probe_and_report_fault`
and `drain_and_report_good` are the one-call-does-both wrappers M6
invokes from the allocator's slow path.

### Allocator Integration (M6)

`alloc_weights` / `alloc_workspace` now route through
`alloc_with_eviction(pool_id)`:

1. **Fast path**: take the allocator lock, try the pool's free-block
   scan. Success returns immediately.
2. **Slow path** (only when `ai_eviction` is on, and only on
   `OutOfMemory`):
   1. Drain expired tracker entries and fire
      `update_feedback(block_id, was_fault=false)` for each (credits
      good evictions).
   2. Snapshot non-pinned candidates for the requested pool via
      `snapshot_evictable_blocks` and filter by `PoolType`.
   3. If `candidates.is_empty()` (every block pinned): return
      `AllocError::OutOfMemory`.
   4. Ask `ACTIVE_POLICY.select_victim(&candidates)` for a victim.
   5. Free the victim under the pool lock, record the
      `(pool_type, model_id, layer_idx)` key in the tracker, bump
      `evictions_total`.
   6. Retry the allocation. Any stale-handle race on the victim's
      free is tolerated — the retry still succeeds if another slot
      opens up.

`set_metadata(handle, model_id, layer_idx, ...)` probes the tracker
after writing the block's content key. A hit means the caller just
"re-admitted" a content key that was evicted recently, so the tracker
reports it to the active policy via `update_feedback(old_id,
was_fault=true)`. The allocator itself never sees content keys — they
arrive once the caller labels the block — so this split keeps the
feedback loop complete without forcing callers to set metadata before
allocation.

**Lock ordering**: the allocator's `LOCK` and the eviction registry's
`REGISTRY_LOCK` are never held simultaneously. `evict_and_retry`
releases the allocator lock before calling `eviction::select_victim`,
then re-acquires it to free the victim and retry. Feedback callbacks
follow the same pattern.

`mm::eviction::generated::MODELS_AVAILABLE` is a `const bool` callers
can check to decide whether to fall back to a classical policy when
the real weights are not present.

### Shell Command (M7)

`eviction` is a shell subcommand modelled on `sched`:

```
slmos> eviction
AI eviction:
  Policy:              LRU
  Models:              trained (xgb + mlp)
  Weight pool:         12 / 128 blocks allocated
  Workspace pool:       3 / 64  blocks allocated
  Evictions (weight):  0
  Evictions (ws):      0
  Evictable candidates (snapshot): 15

slmos> eviction policy
Available eviction policies:
  lru (active)
  lfu
  arc
  slm
  xgboost
  mlp
  cacheus
  cacheus_all5
  first_candidate

slmos> eviction policy cacheus
Switched to policy: CACHEUS

slmos> eviction stats
AI eviction:
  Policy:              CACHEUS
  ...

CACHEUS expert weights (2 experts):
  expert0:  50.00%
  expert1:  50.00%
```

The shell layer lives in `kernel/src/shell_sys.c` (`cmd_eviction`) and
calls into four Rust FFI entry points (`rust_eviction_policy_name`,
`_policy_list`, `_policy_set`, `_get_stats`). Weights cross the FFI in
basis points (0..10000) so the kernel's `-mgeneral-regs-only` C code
stays clear of float arithmetic.

### Public FFI (Phase AI-Eviction M1 / M2)

The C kernel calls Rust through the following entry points. All are
safe to call regardless of the `ai_eviction` feature — the block
tracking fields are always on so baseline and AI builds don't diverge.

**Tracking setters** (always on; return `0` on success, `-1` on error):

| Symbol | Purpose |
|--------|---------|
| `rust_model_touch(handle)` | Bump `access_count` + `last_access_time` |
| `rust_model_set_metadata(handle, model_id, layer_idx, priority)` | Attach identity to a block |
| `rust_model_set_gpu_mapped(handle, mapped)` | Flip the GPU-mapped flag |
| `rust_model_set_dirty(handle, dirty)` | Flip the dirty flag |

**Tracking getters** (always on; sentinels for invalid handles
documented in `kernel/tests/test_eviction.c`):

| Symbol | Sentinel |
|--------|----------|
| `rust_model_get_access_count(handle) -> u32` | `0` |
| `rust_model_get_load_time(handle) -> u64` | `0` |
| `rust_model_get_last_access_time(handle) -> u64` | `0` |
| `rust_model_get_model_id(handle) -> i32` | `-1` |
| `rust_model_get_layer_idx(handle) -> i32` | `i32::MIN` |
| `rust_model_get_model_priority(handle) -> i32` | `-1` |
| `rust_model_is_gpu_mapped(handle) -> i32` | `-1` |
| `rust_model_is_dirty(handle) -> i32` | `-1` |

**Eviction subsystem probes:**

| Symbol | Purpose |
|--------|---------|
| `rust_eviction_enabled() -> i32` | `1` if the `ai_eviction` feature is linked, else `0` |
| `rust_eviction_selftest() -> i32` | Registry round-trip selftest. `0` on success, `-1` on mismatch, `-2` when feature is off |
| `rust_eviction_run_tests() -> i32` | Comprehensive Rust-internal tests for the trait + registry + generated predictors. Returns failure count (`0` on success); `0` without running anything when feature is off. Prints `[PASS]` / `[FAIL]` per test via UART. |
| `rust_eviction_snapshot_count() -> i32` | Count of blocks the active policy would see via `snapshot_evictable_blocks` (allocated AND `ref_count ≤ 1`). Returns `-1` when the feature is off. Used by the M1 snapshot filter test. |

M3 adds classical-policy registration, M4 the ML policies, M5 CACHEUS,
M6 wires the trait into the allocator, and M7 adds the `eviction`
shell command. This document updates as each milestone lands.

### Test Coverage

`kernel/tests/test_eviction.c` registers the **Eviction Policy Tests**
Unity suite (23 tests) alongside the existing `test_suite_model_mem`:

- **Tracking-field FFI** (9 tests, always run):
  - Alloc seeds `load_time`, `last_access_time`, and `access_count = 1`
  - Touch bumps `access_count` and advances `last_access_time`
  - `set_metadata`, `set_gpu_mapped`, `set_dirty` round-trip through the
    getters; invalid handles return documented sentinels; `free()`
    clears tracking; `share()`-ref releases preserve tracking on the
    primary handle.
- **Eviction subsystem** (4 tests, skip cleanly when feature is off):
  - `rust_eviction_enabled()` returns a valid boolean
  - `rust_eviction_selftest()` returns `0`
  - `rust_eviction_run_tests()` returns `0`
  - `rust_eviction_snapshot_count()` honours the evictable-block filter
    (excludes free blocks and pinned blocks with `ref_count > 1`;
    re-admits them on ref drop; returns to baseline on free)
- **Allocator integration (M6)** (4 tests, skip cleanly when feature is off):
  - `test_alloc_evicts_when_full_weights` — eviction path doesn't
    crash when the fill does not reach pool exhaustion
  - `test_alloc_evicts_after_total_fill` — fully drain the workspace
    pool, next alloc evicts (`evictions_total += 1`, new handle valid)
  - `test_alloc_oom_when_all_pinned` — drain + share every block,
    next alloc returns a null handle (no evictable candidates)
  - `test_pool_stats_reports_evictions` — `evictions_total` field is
    readable on both pool-stats snapshots
- **Shell FFI (M7)** (4 tests, always run):
  - `test_policy_name_returns_active` — name buffer null-terminated and
    returns `"LRU"` (feature on) or `"none"` (feature off)
  - `test_policy_list_is_null_terminated` — list points at static
    storage, contains `lru` and `cacheus` tokens when feature is on
  - `test_policy_set_switches_active` — switching lfu → cacheus → lru
    round-trips; unknown name returns -1 and leaves the active policy
    unchanged; feature-off returns -2
  - `test_get_stats_populates_fields` — pool totals sane, snapshot ≥ 0,
    CACHEUS policy populates `cacheus_expert_count = 2` with uniform
    5000 bp weights
- **M8 workload stress** (2 tests, skip cleanly when feature off):
  - `test_memory_pressure_no_leak` — 4-round workspace workload that
    exceeds pool capacity several times over; final `allocated_blocks`
    is 0 and `free_blocks` returns to the pre-test baseline.
  - `test_policy_swap_mid_workload` — LRU → XGBoost → CACHEUS during
    live alloc pressure; name remains valid and no leaks.

`rust_eviction_run_tests()` itself exercises 91 internal invariants (87
when `ai_eviction_models` is off — the int8-vs-f32 cross-check and
the three Python-parity smoke cases are models-only):

- **Registry / trait** (17): default / swap / reset, `FirstCandidatePolicy`
  behaviour, `select_victim` / `score` / `update_feedback` helpers, default
  `score()` impl correctness, `with_active_policy` return values, and a
  5-swap consistency check.
- **Classical-policy parity** (14): LRU evicts oldest, LRU single-
  candidate, LRU score monotonic, LFU evicts least-accessed, LFU breaks
  ties by LRU, SLM evicts workspace first, SLM prefers inactive models,
  SLM fallback is LRU, ARC fallback is LRU, ARC initial accesses fill
  T1, ARC second access promotes to T2, ARC eviction T1→B1, ARC B1
  ghost hit grows p, ARC B2 ghost hit shrinks p, ARC reset clears all
  lists. Cases mirror the sibling parity tests candidate-for-candidate.
- **Audit-pass edge cases** (5): LFU single-candidate, SLM fallback
  with empty active table, LRU repeated-call stability, ARC ghost-list
  overflow trimming, registry `update_feedback` drives ARC's
  `notify_access` (integration through the trait).
- **Generated predictors** (11): `MODELS_AVAILABLE` matches feature,
  `xgb_predict` / `mlp_predict` finite in [0, 1] on three input shapes
  each, stubs return exactly `0.5`.
- **ML policies (M4)** (10 always + 1 gated): `extract_features` shape
  and rank-distinctness, `XGBoostPolicy` / `MlpPolicy` produce valid
  indices and finite scores in [0, 1], victim matches argmax of scores
  for both, and — only under `AI_EVICTION_MODELS=ON` — int8 MLP vs
  float32 MLP decision agreement (≥ 85% across 7 candidate groups).
- **CACHEUS + tracker (M5)** (15): initial uniform weights, weights
  sum to 1 after update, reset restores uniform, min_weight floor
  protects experts after many penalties, `ml_only` / `all_5`
  constructors produce the expected experts, default tuning
  (`lr=0.4`, `window=200`) matches Phase 5, CACHEUS installs and
  selects via the registry, tracker stores / probes / consumes / FIFO-
  evicts / drains entries correctly.
- **M8 workload + feedback** (8, with 3 gated on models): CACHEUS
  adaptation on asymmetric feedback (bad-expert weight drops),
  feedback-loop probe hit fires `update_feedback(_, true)`, probe miss
  fires nothing, window-expiry drain fires `update_feedback(_, false)`,
  rapid-swap stress leaves a valid active policy. Under
  `AI_EVICTION_MODELS=ON`, three Python-parity smoke cases exercise
  the XGBoost + MLP loaded code path.

All suites pass under `make test` on the three supported configs:
`AI_EVICTION=OFF` (default), `AI_EVICTION=ON` (stubs),
`AI_EVICTION_MODELS=ON` (trained weights).

---

## References

- Sibling reference crate: `slm-os-page-sim/slm_os_integration/` (zero-
  dependency Rust port of the Python simulator, parity-tested).
- Python simulator: `slm-os-page-sim/src/simulator/` — the ground truth
  for workload traces, feature extraction, and policy decisions.
- Phase 5 sweep: `slm-os-page-sim/data/results/RESULTS.md` — why
  `ml_only` (XGBoost + MLP, `lr=0.4`, `window=200`) is the default
  CACHEUS pool.
