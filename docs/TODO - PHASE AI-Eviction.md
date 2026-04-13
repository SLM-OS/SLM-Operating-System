# Phase AI-Eviction: AI-Driven Page Replacement for ModelAllocator

This document tracks the integration of trained AI eviction policies (XGBoost, MLP, CACHEUS) into the SLM-OS `ModelAllocator` for the weight and workspace memory pools.

**Status:** Not started — sibling project `slm-os-page-sim` has completed Phases 1-6.1 and produced a verified Rust reference crate (`slm_os_integration/`) with the trait, classical policies, CACHEUS, and end-to-end Python ↔ Rust agreement.

**Summary:** This phase makes the `ModelAllocator` choose which 2 MB block to evict using a learned policy when the weight or workspace pool is full. Today the Rust runtime exposes `alloc_weights()` / `alloc_workspace()` but has no eviction path — eviction is done implicitly via `free()`. Phase AI-Eviction adds a pluggable `EvictionPolicy` trait, ports the four classical policies plus the CACHEUS adaptive ensemble from `slm-os-page-sim`, drops in the trained XGBoost (~1.3 MB) and int8-quantized MLP (~5 KB) generated from the simulator, and wires the chosen victim into the `alloc_block()` path.

**Goals:**
- Pluggable `EvictionPolicy` trait in `runtime/src/mm/eviction_policy.rs`
- Four classical Rust policies (LRU, LFU, ARC, SLM-Heuristic) matching the simulator decision-for-decision
- Trained XGBoost + int8 MLP wired in via the generated files
- CACHEUS adaptive ensemble with the recommended `ml_only` pool (XGBoost + MLP, lr=0.4, window=200)
- Shell command `eviction` to inspect policy state, expert weights, and recent decisions
- QEMU integration test: stress alloc/free across multiple models and verify no leaks/deadlocks
- Measurable improvement over the current ad-hoc approach

**Target Performance:** < 1µs eviction-decision latency on Cortex-A78 @ 1.5 GHz (XGBoost if-else chain in the hot path; MLP/CACHEUS used only for harder cases).

**Prerequisites (Completed in Sibling Project `slm-os-page-sim`):**
- ✅ Discrete-event simulator with dual-pool memory model (Phase 1)
- ✅ 27-feature extraction with normalization (Phase 2)
- ✅ Trained XGBoost (val AUC 0.9999, mean normalized fault rate 0.215) (Phase 3)
- ✅ Trained MLP (val loss 0.0069, int8 quantization 99.6% decision agreement) (Phase 4)
- ✅ CACHEUS tuned: `ml_only` pool wins at 0.212 mean norm rate (Phase 5)
- ✅ End-to-end Rust export verification: XGBoost byte-perfect, MLP int8 95% decision agreement (Phase 6.1)
- ✅ Reference Rust crate `slm_os_integration/` with trait + LRU/LFU/SLM-Heuristic/CACHEUS + 11 parity tests (Phase 6.2 + 6.5)

**Relationship to Other Phases:**
- **Builds on:** Phase 3 (ModelAllocator weight/workspace pools), Phase AI-Sched (pluggable-vtable pattern reused for eviction policies)
- **Independent of:** Phase 5 (ONNX loader), Phase 6 (industrial demo) — can land before or after
- **Benefits:** All phases that allocate from the model pools — currently allocation can fail with `AllocError::OutOfMemory`; with eviction it will free a victim block instead

**Repository:** `CS-496-Capstone-SLM-Operating-System`

**Sibling Repository:** `slm-os-page-sim` (https://github.com/johnjezl/slm-os-page-sim)

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone |

---

## Milestone 1: Pluggable Eviction Policy Trait

**Note:** Pure Rust refactoring — no FP, no new build flags needed. Mirrors the sibling crate `slm_os_integration/src/eviction_policy.rs` exactly so policies can be lifted unchanged.

### EvictionPolicy Trait
- ✅ Create `runtime/src/mm/eviction/policy.rs` (nested module; `eviction/` hosts trait, types, and registry)
- ✅ Define `pub trait EvictionPolicy { ... }` with:
  - `fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize`
  - `fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32>` (default impl: argmax of select_victim)
  - `fn update_feedback(&mut self, block_id: u32, was_fault: bool)` (no-op default)
  - `fn reset(&mut self)` (no-op default)
  - `fn name(&self) -> &'static str`
- ✅ Define `pub struct BlockMeta { ... }` mirroring sibling crate (`block_id`, `pool_type`, `model_id`, `layer_idx`, `last_access_time`, `load_time`, `access_count`, `ref_count`, `gpu_mapped`, `is_dirty`, `model_priority`)
- ✅ Define `pub enum PoolType { Weight, Workspace }`
- ✅ Define `pub type BlockFeatures = [f32; 27]`
- ✅ Document interface contract in module-level docs

### Block Metadata Tracking
- ✅ Rename internal `BlockMeta` → `BlockSlot` in `model_mem.rs` and add tracking fields (`load_time`, `last_access_time`, `access_count`, `model_id`, `layer_idx`, `gpu_mapped`, `is_dirty`, `model_priority`). Fields always on (~12 KB total) so baseline and AI builds don't diverge.
- ✅ `alloc_weights()` / `alloc_workspace()` populate `load_time` and seed `last_access_time` / `access_count = 1`
- ✅ `pub fn touch(handle)` bumps `access_count` and `last_access_time`; exposed as FFI `rust_model_touch`. Kernel-side callers wired in M6.
- ✅ `ref_count` surfaced via the snapshot helper (derived from the existing `BlockSlot::refcount`)
- ✅ `gpu_map` / `gpu_unmap` flip `gpu_mapped` through `set_gpu_mapped`
- ⏸️🎫 `pub fn set_dirty` exists; caller wire-up waits until something in the runtime actually writes to blocks — #118

### Policy Registry
- ✅ Add `static ACTIVE_POLICY: Option<Box<dyn EvictionPolicy + Send>>` behind a SpinGuard (no `std::sync::Mutex` in no_std; matches the `model_mem` lock style)
- ✅ Default is a placeholder `FirstCandidatePolicy` (M3 will swap in the ported `LruPolicy`)
- ✅ Implement `pub fn set_eviction_policy(Box<dyn EvictionPolicy + Send>)`
- ✅ Implement `pub fn get_eviction_policy_name() -> &'static str`
- ✅ Implement `with_active_policy`, `select_victim`, `score`, `update_feedback`, `reset_to_default` helpers for M3–M6 use
- ✅ Guard `pub mod eviction` in `mm::mod.rs` with `#[cfg(feature = "ai_eviction")]`; Cargo.toml declares `ai_eviction` and `ai_eviction_models` features

### Verification
- ✅ Existing alloc/free tests pass unchanged — `make test` green on OFF / `AI_EVICTION=ON` / `AI_EVICTION_MODELS=ON`
- ✅ FFI selftest `rust_eviction_selftest` exercises init → swap → swap → reset_to_default (returns -2 when feature off, 0 on success)
- ✅ Comprehensive Rust-internal tests in `rust_eviction_run_tests` (27 invariants: trait + registry + FirstCandidatePolicy + default-score impl + `with_active_policy` + generated predictor smoke)
- ✅ Kernel-side Unity suite `kernel/tests/test_eviction.c` (12 tests) covers the FFI contract end-to-end: alloc seeds tracking, touch bumps access_count, set_metadata / set_gpu_mapped / set_dirty round-trip through the new getters, invalid-handle sentinels, free clears tracking, shared-block preservation
- ⏸️🎫 Concurrent ≥4-thread stress test — multi-CPU alloc/eviction under policy swap not exercised; M8 covers single-thread paths only — #116
- ✅ `BlockMeta` snapshot round-trips fields across alloc / touch / free via `mm::snapshot_evictable_blocks`

---

## Milestone 2: Build System Integration

**Note:** Required before M3 (ML inference) since the generated XGBoost code is ~1.3 MB and not always desired. Mirrors the AI-Sched feature-gate pattern.

### Cargo Feature Configuration
- ✅ Declared in `runtime/Cargo.toml` (M1):
  ```toml
  [features]
  default = []
  ai_eviction = []
  ai_eviction_models = ["ai_eviction"]
  ```
- ✅ `pub mod eviction` in `runtime/src/mm/mod.rs` is `#[cfg(feature = "ai_eviction")]`

### CMake / Top-Level Integration
- ✅ `CMakeLists.txt` declares `ENABLE_AI_EVICTION` and `ENABLE_AI_EVICTION_MODELS` options; a `FATAL_ERROR` fires if `MODELS` is set without `EVICTION`. Sets `CONFIG_AI_EVICTION` / `CONFIG_AI_EVICTION_MODELS` compile definitions and a status-summary line.
- ✅ Makefile threads `AI_EVICTION` / `AI_EVICTION_MODELS` through as `-D` CMake flags and as `--features ai_eviction` / `--features ai_eviction_models` on cargo. `AI_EVICTION_MODELS=ON` auto-implies `AI_EVICTION=ON`.
- ✅ Documented the flags in `docs/eviction.md` and cross-linked from `docs/building.md`

### Stub Models for Development
- ✅ `runtime/src/mm/eviction/generated/xgb_stub.rs` and `mlp_stub.rs` ship `pub fn xgb_predict` / `mlp_predict` that return `0.5`. Exposes `MODELS_AVAILABLE` constant so callers can detect stub vs real at runtime.
- ✅ `runtime/src/mm/eviction/generated/mod.rs` selects stub vs real via `#[cfg(feature = "ai_eviction_models")]` so the same import path (`generated::xgb_predict`) works either way.
- ✅ Stub build verified: `make kernel AI_EVICTION=ON` builds clean.

### Weight File Import
- ✅ `scripts/import_eviction_weights.sh` copies `xgb_policy_generated.rs`, `mlp_policy_generated.rs`, `mlp_policy_f32.rs` from `slm-os-page-sim/data/export/`, rewrites the `use` path (`eviction_policy` → `eviction::policy`) and `.exp()` (→ `libm::expf`) so the generated files build in `no_std`, and validates the expected public symbols.
- ⏸️🎫 Feature-name list (`eviction_features.rs`) for runtime introspection — sibling's `docs/features.md` is the interim reference — #112
- ✅ Import flow documented in `docs/eviction.md`

### Build Verification
- ✅ `AI_EVICTION=OFF`: existing behaviour, no AI code (2423648-byte ELF)
- ✅ `AI_EVICTION=ON`: stub compiles (2452000-byte ELF, +28 KB)
- ✅ `AI_EVICTION_MODELS=ON`: real models compile (2452000-byte ELF; XGBoost + MLP tables dead-code-eliminated by LTO until M4 references them)
- ✅ Binary-size impact well under the 2 MB budget on all three configs
- ✅ CMake `FATAL_ERROR` fires when `-DENABLE_AI_EVICTION_MODELS=ON -DENABLE_AI_EVICTION=OFF` is passed directly to `cmake`
- ✅ `make test` passes on default and on `AI_EVICTION=ON`

---

## Milestone 3: Classical Policies in Rust

**Depends on:** M1 (Trait), M2 (Build)

**Note:** All four implementations exist in the sibling crate `slm_os_integration/src/{lru,lfu,slm_heuristic,arc}.rs` and pass parity tests against the Python reference. This milestone is largely a copy-with-light-renaming.

### LRU
- ✅ Ported `slm_os_integration/src/lru.rs` to `runtime/src/mm/eviction/lru.rs`
- ✅ `select_victim` picks the candidate with the smallest `last_access_time` (parity test `lru_evicts_oldest`)
- ✅ `score` produces inverse-recency in `[0, 1]` (parity test `lru_score_monotonic`)

### LFU
- ✅ Ported `slm_os_integration/src/lfu.rs` to `runtime/src/mm/eviction/lfu.rs`
- ✅ Ties broken by older `last_access_time` — parity test `lfu_breaks_ties_by_lru`

### ARC
- ✅ Ported the Python `ARCPolicy` (`src/policies/arc.py`) to `runtime/src/mm/eviction/arc.rs`. Sibling Rust crate still doesn't ship ARC — this file is the canonical Rust translation.
- ✅ Four LRU lists (T1, T2, B1, B2) backed by `VecDeque<(u32, u8)>`; O(n) `move_to_end` is acceptable because the lists are bounded at `ARC_DEFAULT_GHOST_SIZE = 256`.
- ✅ Adaptive parameter `p`, `notify_access()` / `notify_eviction()` callbacks, and a generic `EvictionPolicy::update_feedback` bridge so the registry's feedback hook drives ghost-hit adaptation without bespoke FFI.
- ⏸️🎫 Direct `notify_eviction` wire-up from `alloc_block()` — `notify_access` is reachable via the M6 tracker → `update_feedback(_, true)` path; the proactive ghost-list update from the textbook ARC algorithm is missing — #114

### SLM-Heuristic
- ✅ Ported `slm_os_integration/src/slm_heuristic.rs` to `runtime/src/mm/eviction/slm_heuristic.rs`
- ✅ Priority cascade preserved: workspace before weights → inactive models → LRU (parity tests `slm_evicts_workspace_first`, `slm_evicts_inactive_models_before_active`, `slm_fallback_is_lru`)
- ✅ `set_active_inferences` + `bump_active` provide the update API for the future scheduler feed (Phase AI-Sched)
- ⏸️🎫 Wire active-inference feed from the scheduler into `SlmHeuristicPolicy::set_active_inferences` — needs Phase AI-Sched integration; until then the inactive-models tier always fires — #113

### Default Policy Swap
- ✅ Registry default is now `LruPolicy` instead of the M1 placeholder `FirstCandidatePolicy`. The placeholder is still exported (`mm::eviction::FirstCandidatePolicy`) for tests that need a deterministic trivial policy.

### Parity Tests
- ✅ 14 classical-policy parity cases + 7 ARC invariants added to `rust_eviction_run_tests()`. Runs under `make test` on all three configs.
  - Note: `cargo test --features ai_eviction` is not a viable target in this repo — the runtime crate has pre-existing `cargo test` compile errors unrelated to eviction. All Rust-internal tests run via the kernel test harness instead.

---

## Milestone 4: XGBoost and MLP Wiring

**Depends on:** M1 (Trait), M2 (Build with `ai_eviction_models`), Plan A (sibling export)

### XGBoost Policy
- ✅ `runtime/src/mm/eviction/xgboost.rs` ships `XGBoostPolicy` — calls `generated::xgb_predict` per candidate, returns argmax.
- ✅ Feature-vector extraction lives in `runtime/src/mm/eviction/features.rs` (27-feature layout matching `FeatureExtractor.extract_candidate_features` / `FeatureNormalizer.normalize`).
- ✅ `predicted_reuse_dist` heuristic included (Sequential branch — `BlockMeta` doesn't carry `access_pattern` yet; documented approximation).
- ✅ Sibling export verified byte-perfect Python↔Rust agreement for `xgb_predict`; runtime tests smoke-check finite-in-[0, 1] and argmax-matches-score consistency.
- ⏸️🎫 Inference-latency benchmark on Cortex-A78 (Pi 5) — bench framework lives in M9; hardware run pending — #108

### MLP Policy
- ✅ `runtime/src/mm/eviction/mlp.rs` ships `MlpPolicy` — calls `generated::mlp_predict` (int8) per candidate, returns argmax.
- ✅ Int8 vs float32 decision-agreement test in `rust_eviction_run_tests` — 7 synthetic candidate groups; target ≥ 85% agreement; passes under `AI_EVICTION_MODELS=ON`. Broader 1000-vector verification stays in the sibling's `scripts/verify_rust_export.py`.
- ⏸️🎫 Cortex-A78 latency benchmark (MLP) — same Pi 5 hardware run as the XGBoost row above — #108
- ✅ int8 model size from sibling export: 20 KB compiled (`mlp_policy_generated.rs`, includes predict fn + weight tables). Stub is 0.6 KB. Well under the 5 KB target for the int8 weights alone (4.4 KB constants).

### Float32 Verification Build
- ✅ `mlp_policy_f32` module is gated on `ai_eviction_models` (not `#[cfg(test)]`) so the runtime test harness can pull it in. Exposes `mlp_predict_f32`.
- ✅ Int8 vs float32 cross-check runs under `rust_eviction_run_tests` with real models. Uses `extract_features`-produced inputs (post-normalisation) so the int8 quantiser stays within its designed dynamic range.

---

## Milestone 5: CACHEUS in Rust

**Depends on:** M1 (Trait), M3 (Classical), M4 (ML)

**Note:** Full implementation already exists in `slm_os_integration/src/cacheus.rs` with the corrected weight-update rule from the sibling project's Phase 5 work. Port directly.

### CacheusSelector
- ✅ Ported `slm_os_integration/src/cacheus.rs` to `runtime/src/mm/eviction/cacheus.rs`
- ✅ `Vec<Box<dyn EvictionPolicy + Send>>` expert pool with multiplicative f32 weight updates
- ✅ Circular feedback buffer via `VecDeque<EvictionRecord>` (default `window_size=200`)
- ✅ `EvictionRecord` stores the ensemble's actual choice (Phase 5 fix preserved)
- ✅ Weight floor at `min_weight=0.01` with renormalisation after each update

### Recommended Default Pool
- ✅ `CacheusSelector::ml_only()` constructor — XGBoost + MLP, `lr=0.4`, `window=200`. Matches the sibling Phase 5 winning configuration.
- ✅ `CacheusSelector::all_5()` constructor — LRU + LFU + SLM-Heuristic + XGBoost + MLP. Available for ablation experiments.

### Eviction Feedback Plumbing
- ✅ `runtime/src/mm/eviction/tracker.rs` ships `EvictedContentTracker` — FIFO of `(ContentKey, evicted_block_id, evicted_at_ns)` with configurable capacity (default 256) and feedback window (default `EVICTION_FEEDBACK_WINDOW_NS = 200 ms`). Mirrors the simulator's `_evicted_content`.
- ✅ `probe_on_alloc(key, now)` returns the block_id of a matching recent eviction and consumes the entry; `drain_expired(now)` returns block_ids of entries past the window.
- ✅ `probe_and_report_fault` / `drain_and_report_good` convenience wrappers drive the installed policy's `update_feedback` hook.
- ✅ Wired `record_eviction` / `probe_on_alloc` into `alloc_weights` / `alloc_workspace` — landed in M6 (`evict_and_retry` records, `set_metadata` probes)

### Trajectory Recording (Optional)
- ⏸️🎫 `cacheus_trace` feature flag + `(tick, weights)` ring buffer — runtime introspection available via `CacheusSelector::weights` / `expert_faults` / `expert_decisions` accessors; trace history pairs with `eviction trajectory` shell subcommand — #111

---

## Milestone 6: Wire into ModelAllocator

**Depends on:** M1-M5

**Note:** This is the actual integration point — making `alloc_weights` / `alloc_workspace` consult the active policy.

### Allocation Path Changes
- ✅ `alloc_weights` and `alloc_workspace` centralise through `alloc_with_eviction(pool_id)`. First attempt hits the pool fast-path; on `OutOfMemory` with `ai_eviction` on, `evict_and_retry` is invoked.
- ✅ `evict_and_retry` snapshots the pool's non-pinned candidates (`snapshot_evictable_blocks` filtered by `PoolType`), consults `ACTIVE_POLICY.select_victim`, frees the victim, records the eviction into `EVICTED_CONTENT_TRACKER`, and retries the allocation. `StaleHandle` on the victim-free is treated as a benign race (the retry still fires).
- ✅ `candidates.is_empty()` (every block pinned) returns `AllocError::OutOfMemory` — matches the TODO's intent.
- ✅ Expired-tracker entries drain at the top of `evict_and_retry` and drive `update_feedback(_, false)` on the installed policy (good-eviction credit).
- ✅ `set_metadata` probes `EVICTED_CONTENT_TRACKER` after writing the content key; a hit drives `update_feedback(_, true)` — this is the CACHEUS fault signal that couldn't be produced from the alloc path alone (the content key isn't known until the caller labels the block).
- ✅ `PoolStats` gains `evictions_total`; `RustPoolStats` in `slm_ffi.h` mirrors the Rust layout.

### Access Tracking
- ✅ `touch(handle)` lands in M1; M6 keeps it unchanged. `rust_model_touch` is the FFI the kernel uses when it reads/writes block contents.
- ⏸️🎫 Kernel-side syscall wire-up (`mm_touch_block`) — existing tests exercise `rust_model_touch` directly; user-mode callers will need a syscall once they start mutating block contents — #123

### Verification
- ✅ Existing `alloc_weights` / `alloc_workspace` tests (`test_suite_model_mem`) pass on all three configs.
- ✅ `test_pool_exhaustion` updated to branch on `rust_eviction_enabled()`: under AI_EVICTION the pool never truly exhausts (eviction keeps it usable); under classic the old OOM behaviour is preserved.
- ✅ New `test_alloc_evicts_when_full_weights`, `test_alloc_evicts_after_total_fill`, `test_alloc_oom_when_all_pinned`, and `test_pool_stats_reports_evictions` added to `test_suite_eviction`.
- ✅ Switching to `XGBoostPolicy` mid-test and repeating — `test_policy_swap_mid_workload` in M8 cycles LRU → XGBoost → CACHEUS during live alloc pressure.

---

## Milestone 7: Shell Command

**Depends on:** M6

### `eviction` Subcommand
- ✅ `eviction` — summary: current policy, pool utilisations (weight + workspace), eviction counts, evictable-candidate snapshot
- ✅ `eviction policy` — lists registered policies (`lru`, `lfu`, `arc`, `slm`, `xgboost`, `mlp`, `cacheus`, `cacheus_all5`, `first_candidate`) with the active one tagged
- ✅ `eviction policy <name>` — installs the named policy at runtime via `rust_eviction_policy_set`
- ✅ `eviction stats` — summary plus per-expert CACHEUS weights (basis points to avoid float math in `-mgeneral-regs-only` C)
- ⏸️🎫 `eviction trajectory` — pairs with the M5 `cacheus_trace` deferral; current introspection is live-state only — #111
- ✅ Mirrors the AI-Sched `sched` command structure (subcommand dispatch, "Unknown policy" error message, no-arg summary).

### Stats Tracking
- ✅ Per-pool: `evictions_total` counter and pool utilisations surfaced via `RustEvictionStats`.
- ✅ CACHEUS: per-expert weights via `EvictionPolicy::ensemble_weights` (default `None`, overridden by `CacheusSelector`). Weight snapshots flow through the stats struct as basis points.
- ⏸️🎫 Generic per-policy decision count / fallback count / average latency — live CACHEUS-specific counters exist but the generic trait wrapper is not in place — #115

---

## Milestone 8: Testing

### Unit Tests
- ✅ Parity tests ported — M3 landed LRU/LFU/SLM-Heuristic/ARC parity cases (14 + ARC-specific) plus 5 audit-pass edge cases, all running under `rust_eviction_run_tests`.
- ✅ `policy_swap_stress_final_state_valid` — 20-iteration rapid-swap stress with an alloc between each swap; final state is a valid policy.
- ✅ `xgb_python_parity_*` and `mlp_python_parity_*` — sample fixtures with hand-checked expected bounds. Under `AI_EVICTION_MODELS=ON` both predictors are byte-perfect against the sibling's export-time verification (`scripts/verify_rust_export.py`); the 3 hard-coded cases in the runner exercise the loaded code path. Full 100-vector fixtures remain in the sibling's verification harness.
- ✅ `cacheus_adapts_weights_toward_better_expert` — 2-expert pool (FirstCandidate vs LRU) driven through 30 rounds of asymmetric feedback; LRU's weight exceeds FirstCandidate's at the end.

### Integration Tests
- ✅ `test_alloc_evicts_when_full_weights` + `test_alloc_evicts_after_total_fill` — M6.
- ✅ `test_alloc_oom_when_all_pinned` — M6.
- ✅ `feedback_loop_probe_hit_reports_fault` — drives `probe_and_report_fault` end-to-end and verifies the installed Recorder policy received `update_feedback(_, true)`.
- ✅ `feedback_loop_probe_miss_no_fault` — probing with a non-matching key fires no callback.
- ✅ `feedback_loop_window_expiry_signals_good` — drain past window returns 1 block_id and delivers `update_feedback(_, false)`.
- ✅ `test_memory_pressure_no_leak` — 4-round workspace workload that allocates/holds/frees more than the pool capacity; ends with `allocated_blocks == 0` and `free_blocks == pre_test_baseline`.
- ✅ `test_policy_swap_mid_workload` — LRU → XGBoost → CACHEUS during live alloc pressure; name remains valid and no leaks.

### Performance Tests
- ✅ `bench_xgb_inference_latency`, `bench_mlp_inference_latency`,
  `bench_cacheus_inference_latency`, `bench_lru_inference_latency` —
  landed in M9 as `test_bench_all_policies` (reports avg ns / call
  for all eight policies, prints to the test log on every `make test`
  run).

### QEMU Validation
- ✅ Boot with `AI_EVICTION_MODELS=ON` — every M8 test run exercises this path via `make test AI_EVICTION_MODELS=ON`.
- ✅ `eviction` shell command lists policies and switches them — covered by `test_policy_list_is_null_terminated` + `test_policy_set_switches_active`.
- ✅ Synthetic memory-pressure workload — `test_memory_pressure_no_leak` exceeds pool capacity 4×; pool stats verify zero leaks at the end.
- ⏸️🎫 End-to-end fault rate `cacheus` vs `lru` (50%+ reduction target) — requires the simulator's workload-replay infrastructure in the kernel; CACHEUS adaptation is already verified qualitatively by `cacheus_adapts_weights_toward_better_expert` — #117

---

## Milestone 9: Performance Validation

**Depends on:** M8

**Note:** Mirrors Phase AI-Sched M9. The sibling project measured Python latencies (XGBoost ~9 µs, MLP ~2 µs per candidate at batch=64) but those include Python overhead. Native Rust if-else chains should be sub-microsecond.

### Benchmark Framework
- ✅ `rust_eviction_bench_latency_ns(name, iterations)` FFI runs the
  given policy against a fixed 8-candidate set; returns average
  nanoseconds per `select_victim` call.
- ✅ `test_bench_all_policies` (`test_suite_eviction`) reports numbers
  for all policies under each build config. Runs as a Unity test so
  the numbers appear in every `make test` log.

### Hardware Benchmarks
- ✅ QEMU baseline numbers (aarch64 `virt`, cortex-a76, 1000 iterations per call,
  stats from `make test AI_EVICTION=ON`):

  | Policy | Stub build | Real models |
  |--------|------------|-------------|
  | first_candidate | 55 ns | 58 ns |
  | lru | 71 ns | 72 ns |
  | lfu | 90 ns | 91 ns |
  | arc | 3989 ns | 3132 ns |
  | slm | 1749 ns | 881 ns |
  | xgboost | 18780 ns | 190725 ns |
  | mlp | 18596 ns | 1583126 ns |
  | cacheus (ml_only) | 46381 ns | 1734300 ns |

  The MLP number under `AI_EVICTION_MODELS=ON` is dominated by QEMU's
  slow int8 emulation — real silicon should be orders of magnitude
  faster (the sibling's Python reference is ~2 µs at batch=64).

- ⏸️🎫 Cortex-A78 (Pi 5) — requires Pi 5 hardware deploy via
  `labctl sdwire_update` and a dedicated `eviction bench` shell run.
  The framework is in place; final numbers are a capstone-demo task — #108
- ⏸️🎫 Jetson Orin Nano — same shape as Pi 5, different silicon — #109
- ⏸️🎫 x86-64 — informational only — #110

### End-to-End Workload Comparison
- ⏸️🎫 7-scenario simulator replay (single_inference, multi_model,
  hot_swap, burst_load, mixed_priority, gpu_contention, adversarial) —
  requires porting the sibling's `simulator/core.py` trace driver into
  the kernel and is a multi-week effort. The qualitative adaptation
  signal is verified by `cacheus_adapts_weights_toward_better_expert`;
  quantitative comparison to LRU remains in the sibling project
  (`data/results/policy_scenario_matrix.csv`) — #117

### Memory Overhead
- ✅ Binary size delta (QEMU_VIRT Release build; `stat slmos.elf`):

  | Config | ELF size | Delta vs OFF |
  |--------|---------:|-------------:|
  | OFF | 2,444,536 B | — |
  | AI_EVICTION=ON (stubs) | 2,580,376 B | +135,840 B (~133 KB) |
  | AI_EVICTION_MODELS=ON (real) | 2,789,944 B | +345,408 B (~337 KB) |

  `slmos.bin` (the stripped flash footprint) deltas are smaller:
  +48 KB for stubs, +254 KB for full models. All well under the
  2 MB budget.

- ✅ Runtime overhead (sized against the M5 design):
  - `CacheusSelector` (ml_only): 2 × `Box<dyn EvictionPolicy>` + 2 f32
    weights + 2 u32 fault counters + 2 u32 decision counters +
    200-slot `VecDeque<EvictionRecord>`, each record is an id + two
    `usize` (small Vec on the heap). Upper bound: ~8 KB.
  - `EvictedContentTracker` (default): 256-slot
    `VecDeque<TrackerEntry>`, each entry `{ContentKey, u32, u64}` =
    16 B → 4 KB + overhead.
  - Classical policies: zero additional heap beyond the Box.
  - **Total**: < 16 KB on the heap when CACHEUS is installed.

- ✅ Overhead documented in `docs/eviction.md` (Memory Overhead
  section alongside the table).

---

## Phase AI-Eviction Completion Checklist

### Deliverables
- ✅ Pluggable `EvictionPolicy` trait integrated into `ModelAllocator` (M1 + M6)
- ✅ Four classical Rust policies (LRU, LFU, ARC, SLM-Heuristic) ported from sibling crate (M3)
- ✅ XGBoost and int8 MLP wired in via generated files; predictions agree with Python reference byte-perfect for XGBoost and within documented int8 quantisation tolerance for MLP (M4)
- ✅ CACHEUS adaptive selector with `ml_only` default pool constructor (M5)
- ✅ `eviction` shell command for runtime introspection and policy switching (M7)
- ✅ Eviction decisions trigger on pool exhaustion (M6 — replaces the old `OutOfMemory` failure when `AI_EVICTION=ON`)
- ⏸️🎫 Inference latency < 1 µs on target hardware — bench framework is in place (M9), QEMU numbers are captured, Pi 5 / Jetson runs need hardware deploy — #108, #109
- ✅ All tests pass with `AI_EVICTION=OFF` / `ON` / `MODELS=ON`.

### Demo
The functionality required for the demo script is in place and exercised
by the M7/M8 Unity tests on every `make test` run. The actual demo
performance happens during the capstone presentation; nothing in this
repo blocks it.

- ✅ Boot SLM-OS in QEMU with AI eviction (`make run AI_EVICTION_MODELS=ON`)
- ✅ `eviction policy` lists registered policies (covered by `test_policy_list_is_null_terminated`)
- ✅ `eviction policy cacheus` switches to the adaptive ensemble (`test_policy_set_switches_active`)
- ✅ Load 4 models that exceed pool capacity; verify successful allocation through eviction (`test_memory_pressure_no_leak` exceeds capacity 4×)
- ✅ `eviction stats` shows expert weights (covered by `test_get_stats_populates_fields`; live adaptation requires the workload-replay infrastructure tracked by #117)
- ✅ Switch back to `lru` (`test_policy_swap_mid_workload` exercises LRU → XGBoost → CACHEUS → LRU)

### Documentation
- ✅ `docs/eviction.md` — pluggable policy, trait, classical policies, ML policies, CACHEUS, shell commands, latency benchmarks, memory overhead all covered
- ✅ Feature vector layout (27 features) — documented in `docs/eviction.md` (ML Policies section) with the simulator's normalisation rules and the runtime's deviations
- ✅ Performance targets documented — `docs/eviction.md` Latency Benchmarks + Memory Overhead sections; the < 1 µs hardware target stays as the criterion for closing #108 / #109

---

## Outstanding Decisions

### Milestone 1 — Trait Surface

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Concurrency model** | `Mutex<Box<dyn>>` vs lock-free swap | **Mutex** initially — eviction is slow path, contention is rare |
| **`BlockMeta` ownership** | Borrow from `ModelHandle` vs copy out | **Copy** at decision time — avoids lifetime headaches with the policy holding references |

### Milestone 2 — Build

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Feature granularity** | One flag vs two-tier | **Two-tier** (`ai_eviction` enables trait + classical, `ai_eviction_models` adds the 1.3 MB XGBoost) — lets QEMU smoke tests skip the big binary |
| **Generated file format** | Generated `.rs` vs binary blob + loader | **Generated `.rs`** — already verified end-to-end in sibling, no parser to maintain |

### Milestone 3 — ARC Port

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **List representation** | `VecDeque` vs intrusive linked list | **`VecDeque`** for first cut — intrusive list is a separate optimization |

### Milestone 5 — CACHEUS

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Default pool** | `all_5` vs `ml_only` | **`ml_only`** — sibling Phase 5 showed `all_5` is 2× worse (0.427 vs 0.212) |
| **Feedback window** | 100 vs 200 vs 500 ticks | **200** — matches sibling tuning, matches the simulator's `_eviction_feedback_window` |
| **Trajectory recording** | Always on vs feature-gated | **Feature-gated** (`cacheus_trace`) — debug-only, not worth the always-on memory |

### Milestone 6 — Allocation Path

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Evictable-block filter** | `ref_count == 0` vs more complex | **`ref_count == 0`** for first cut; revisit if too restrictive |
| **GPU-mapped blocks** | Always evictable vs never | **Never** in first cut (matches simulator) — GPU unmap is expensive |

### Milestone 8 — Testing

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Python parity vectors** | Hardcoded in tests vs generated CSV | **Generated CSV** (export from sibling) — keeps tests in sync with model retrains |

---

## Risk Mitigation

### Identified Risks

1. **⚠️ Memory Cost of XGBoost If-Else Chain**
   - Risk: Generated XGBoost is ~1.3 MB of `.rs` source — bloats the kernel image
   - Mitigation: Two-tier feature flag (`ai_eviction_models` opt-in)
   - Mitigation: Document tree-pruning options if size becomes painful (sibling supports `--max-depth` / `--num-rounds` reductions during training)
   - Fallback: Use only the MLP (4.4 KB) for memory-constrained builds

2. **⚠️ Block Metadata Tracking Cost**
   - Risk: Updating `last_access_time` / `access_count` on every block touch adds overhead to the hot path
   - Mitigation: Only update via explicit `touch()` calls — let callers batch updates if profitable
   - Mitigation: Use atomic counters where possible to avoid locking
   - Fallback: Sample-based tracking (e.g., bump every 16th access)

3. **⚠️ CACHEUS Weight-Update Overhead**
   - Risk: Multiplicative weight update on every eviction adds latency
   - Mitigation: Updates happen only on the slow path (eviction triggered) — not the common alloc path
   - Mitigation: Defer feedback processing if the allocation path is contended
   - Verification: Bench `cacheus_inference_latency` separately from update cost

4. **Feature-Vector Mismatch with Sibling**
   - Risk: Feature ordering or normalization drifts between Python and Rust → predictions diverge
   - Mitigation: Sibling project's `scripts/verify_rust_export.py` already cross-checks; rerun after every model retrain
   - Mitigation: Generate `eviction_features.rs` with the exact feature names + indices from `FeatureConfig.feature_names`
   - Fallback: Run the sibling's verification harness on the kernel-built models too

5. **Eviction Storm Under High Pressure**
   - Risk: Adversarial workload triggers continuous eviction → policy thrashes
   - Mitigation: Already exercised by `adversarial` scenario in the sibling sim — all policies handle it
   - Mitigation: Add a per-tick eviction-rate cap if pathological cases appear
   - Fallback: Fall back to LRU when eviction rate exceeds threshold (similar to AI-Sched fallback path)

6. **Pinned-Block Starvation**
   - Risk: All evictable candidates have `ref_count > 0` → `OutOfMemory` even though policy could pick any of them
   - Mitigation: Rely on existing `unshare()` to drop references promptly; document the contract
   - Mitigation: Consider "soft-pin" semantics (preferable but evictable if no choice) in a future phase
   - Fallback: Existing `OutOfMemory` is the same behavior as today

---

## Dependencies

### External Dependencies
- **Sibling project `slm-os-page-sim`:** Generates `xgb_policy_generated.rs`, `mlp_policy_generated.rs`, `mlp_policy_f32.rs`, `eviction_features.rs`
  - Can proceed with stub weights until sibling ships
  - Reference Rust crate at `slm_os_integration/` is already a working template
- **Phase AI-Sched:** Provides per-task `model_id` for the SLM-Heuristic policy's active-inference table
- **Rust toolchain:** rustc (already in build); no additional crates required (sibling crate has zero dependencies)

### Internal Dependencies
- **Phase 3 ModelAllocator:** Provides the weight/workspace pools and the `alloc_*` API that this phase extends
- **Phase 4 GPU memory:** `gpu_mapped` flag on `BlockMeta` requires the existing `gpu_map`/`gpu_unmap` paths

### Optional Enhancements (Post-Capstone)
- ⏸️🎫 Online retraining: ship the simulator with the OS and let it produce updated weights from real workload traces — #119
- ⏸️🎫 Per-pool policy: different policies for weight vs workspace pools (current design uses one policy for both) — #120
- ⏸️🎫 Multi-feature predicted-reuse: extend the 27-feature vector with kernel-side signals not available to the simulator — #122
