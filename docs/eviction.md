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
├── registry.rs      # ACTIVE_POLICY, set/get/with_active_policy,
│                    # select_victim / score / update_feedback helpers
└── generated/
    ├── mod.rs       # Selects stub vs real at compile time
    ├── xgb_stub.rs  # xgb_predict(_) -> 0.5 when models are off
    ├── mlp_stub.rs  # mlp_predict(_) -> 0.5 when models are off
    # After `import_eviction_weights.sh` runs:
    ├── xgb_policy_generated.rs   # Real XGBoost if-else chain
    ├── mlp_policy_generated.rs   # Real int8-quantized MLP
    └── mlp_policy_f32.rs         # Float32 reference (test-only)
```

`mm::eviction::generated::MODELS_AVAILABLE` is a `const bool` callers
can check to decide whether to fall back to a classical policy when
the real weights are not present.

### Public FFI (Phase AI-Eviction M1)

The C kernel calls Rust via four entry points so far:

| Symbol | Purpose |
|--------|---------|
| `rust_model_touch(handle)` | Bump access tracking on a block (eviction input) |
| `rust_model_set_metadata(handle, model_id, layer_idx, priority)` | Attach identity to a block |
| `rust_eviction_enabled() -> i32` | `1` if the `ai_eviction` feature is linked |
| `rust_eviction_selftest() -> i32` | `0` on success, `-1` on mismatch, `-2` when the feature is off |

M3 adds classical-policy registration, M4 the ML policies, M5 CACHEUS,
M6 wires the trait into the allocator, and M7 adds the `eviction`
shell command. This document updates as each milestone lands.

---

## References

- Sibling reference crate: `slm-os-page-sim/slm_os_integration/` (zero-
  dependency Rust port of the Python simulator, parity-tested).
- Python simulator: `slm-os-page-sim/src/simulator/` — the ground truth
  for workload traces, feature extraction, and policy decisions.
- Phase 5 sweep: `slm-os-page-sim/data/results/RESULTS.md` — why
  `ml_only` (XGBoost + MLP, `lr=0.4`, `window=200`) is the default
  CACHEUS pool.
