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
└── generated/
    ├── mod.rs       # Selects stub vs real at compile time
    ├── xgb_stub.rs  # xgb_predict(_) -> 0.5 when models are off
    ├── mlp_stub.rs  # mlp_predict(_) -> 0.5 when models are off
    # After `import_eviction_weights.sh` runs:
    ├── xgb_policy_generated.rs   # Real XGBoost if-else chain
    ├── mlp_policy_generated.rs   # Real int8-quantized MLP
    └── mlp_policy_f32.rs         # Float32 reference (test-only)
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

`mm::eviction::generated::MODELS_AVAILABLE` is a `const bool` callers
can check to decide whether to fall back to a classical policy when
the real weights are not present.

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

M3 adds classical-policy registration, M4 the ML policies, M5 CACHEUS,
M6 wires the trait into the allocator, and M7 adds the `eviction`
shell command. This document updates as each milestone lands.

### Test Coverage

`kernel/tests/test_eviction.c` registers the **Eviction Policy Tests**
Unity suite (12 tests) alongside the existing `test_suite_model_mem`:

- **Tracking-field FFI** (9 tests, always run):
  - Alloc seeds `load_time`, `last_access_time`, and `access_count = 1`
  - Touch bumps `access_count` and advances `last_access_time`
  - `set_metadata`, `set_gpu_mapped`, `set_dirty` round-trip through the
    getters; invalid handles return documented sentinels; `free()`
    clears tracking; `share()`-ref releases preserve tracking on the
    primary handle.
- **Eviction subsystem** (3 tests, skip cleanly when feature is off):
  - `rust_eviction_enabled()` returns a valid boolean
  - `rust_eviction_selftest()` returns `0`
  - `rust_eviction_run_tests()` returns `0`

`rust_eviction_run_tests()` itself exercises 48 internal invariants:

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
- **Generated predictors** (11): `MODELS_AVAILABLE` matches feature,
  `xgb_predict` / `mlp_predict` finite in [0, 1] on three input shapes
  each, stubs return exactly `0.5`.

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
