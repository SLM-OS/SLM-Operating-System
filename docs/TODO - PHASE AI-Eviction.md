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
- ☐ Create `runtime/src/mm/eviction_policy.rs`
- ☐ Define `pub trait EvictionPolicy { ... }` with:
  - `fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize`
  - `fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32>` (default impl: argmax of select_victim)
  - `fn update_feedback(&mut self, block_id: u32, was_fault: bool)` (no-op default)
  - `fn reset(&mut self)` (no-op default)
  - `fn name(&self) -> &'static str`
- ☐ Define `pub struct BlockMeta { ... }` mirroring sibling crate (`block_id`, `pool_type`, `model_id`, `layer_idx`, `last_access_time`, `load_time`, `access_count`, `ref_count`, `gpu_mapped`, `is_dirty`, `model_priority`)
- ☐ Define `pub enum PoolType { Weight, Workspace }`
- ☐ Define `pub type BlockFeatures = [f32; 27]`
- ☐ Document interface contract in module-level docs

### Block Metadata Tracking
- ☐ Extend `ModelHandle` storage in `model_mem.rs` to include the `BlockMeta` fields above
- ☐ Update `alloc_weights()` / `alloc_workspace()` to populate `load_time`
- ☐ On every access (FFI from kernel or Rust runtime), bump `access_count` and `last_access_time`
- ☐ Track `ref_count` against existing share/unshare
- ☐ Track `gpu_mapped` via the existing `gpu_map` / `gpu_unmap` paths

### Policy Registry
- ☐ Add `static ACTIVE_POLICY: Mutex<Box<dyn EvictionPolicy + Send>>` (or atomic-pointer variant for IRQ safety)
- ☐ Initialize to `LruPolicy::new()` (matches today's implicit FIFO behavior closely)
- ☐ Implement `pub fn set_eviction_policy(policy: Box<dyn EvictionPolicy + Send>)`
- ☐ Implement `pub fn get_eviction_policy_name() -> &'static str`
- ☐ Guard AI-specific policies with `#[cfg(feature = "ai_eviction")]`

### Verification
- ☐ Existing alloc/free tests pass unchanged with `LruPolicy` as default
- ☐ Policy swap is safe under concurrent allocation (stress-test with ≥4 threads)
- ☐ `BlockMeta` accessors return expected values across alloc/access/free cycles

---

## Milestone 2: Build System Integration

**Note:** Required before M3 (ML inference) since the generated XGBoost code is ~1.3 MB and not always desired. Mirrors the AI-Sched feature-gate pattern.

### Cargo Feature Configuration
- ☐ Add to `runtime/Cargo.toml`:
  ```toml
  [features]
  default = []
  ai_eviction = []          # Brings in pluggable trait + classical Rust policies
  ai_eviction_models = ["ai_eviction"]  # Also pulls in generated XGBoost + MLP
  ```
- ☐ Wire `ai_eviction` cfg into `runtime/src/mm/mod.rs`

### CMake / Top-Level Integration
- ☐ Add to `CMakeLists.txt`:
  ```cmake
  option(ENABLE_AI_EVICTION "Include AI eviction policies" OFF)
  option(ENABLE_AI_EVICTION_MODELS "Include trained XGBoost + MLP weights" OFF)

  if(ENABLE_AI_EVICTION_MODELS AND NOT ENABLE_AI_EVICTION)
      message(FATAL_ERROR "ENABLE_AI_EVICTION_MODELS requires ENABLE_AI_EVICTION")
  endif()
  ```
- ☐ Pass cargo features through to the runtime build
- ☐ Document the two-tier feature flag in `docs/build.md`

### Stub Models for Development
- ☐ Create `runtime/src/mm/eviction/generated/stub.rs` with all-zero weight tables matching the real export's shape
- ☐ Create `runtime/src/mm/eviction/generated/mod.rs` selecting stub vs real at compile time
- ☐ Verify the stub compiles standalone (`cargo build -p runtime --features ai_eviction`)

### Weight File Import
- ☐ Create `scripts/import_eviction_weights.sh`:
  - Copies `xgb_policy_generated.rs`, `mlp_policy_generated.rs`, `mlp_policy_f32.rs` from sibling project's `data/export/` into `runtime/src/mm/eviction/generated/`
  - Copies feature-name list into `eviction_features.rs` for runtime introspection
  - Validates expected `pub fn xgb_predict` / `pub fn mlp_predict` symbols are present
- ☐ Document the import flow in `docs/eviction.md`

### Build Verification
- ☐ Build with `ENABLE_AI_EVICTION=OFF` (existing behavior, no AI code)
- ☐ Build with `ENABLE_AI_EVICTION=ON` + stub models
- ☐ Build with `ENABLE_AI_EVICTION_MODELS=ON` + real models from sibling export
- ☐ Verify final binary size impact: target < 2 MB additional when models enabled

---

## Milestone 3: Classical Policies in Rust

**Depends on:** M1 (Trait), M2 (Build)

**Note:** All four implementations exist in the sibling crate `slm_os_integration/src/{lru,lfu,slm_heuristic,arc}.rs` and pass parity tests against the Python reference. This milestone is largely a copy-with-light-renaming.

### LRU
- ☐ Port `slm_os_integration/src/lru.rs` to `runtime/src/mm/eviction/lru.rs`
- ☐ Verify `select_victim` returns the candidate with the smallest `last_access_time`
- ☐ Verify `score` produces inverse-recency in `[0, 1]`

### LFU
- ☐ Port `slm_os_integration/src/lfu.rs` to `runtime/src/mm/eviction/lfu.rs`
- ☐ Verify ties broken by older `last_access_time` (LRU within tied set)

### ARC
- ☐ Port the Python `ARCPolicy` (`src/policies/arc.py` in the sibling project) to Rust
  - **Note:** ARC is not yet in the sibling Rust crate. Use the Python implementation as the reference.
- ☐ Implement the four LRU lists (T1, T2, B1, B2) as `VecDeque`s
- ☐ Implement adaptive parameter `p` and the `replace()` and `notify_access()` / `notify_eviction()` callbacks
- ☐ Wire `notify_access` and `notify_eviction` into `alloc_block()` / `free()` (M5)

### SLM-Heuristic
- ☐ Port `slm_os_integration/src/slm_heuristic.rs` to `runtime/src/mm/eviction/slm_heuristic.rs`
- ☐ Maintain the priority cascade: workspace before weights → inactive models → LRU
- ☐ Wire active-inference table from the scheduler (Phase AI-Sched provides per-task model_id)

### Parity Tests
- ☐ Port `slm_os_integration/tests/parity.rs` to `runtime/tests/eviction_parity.rs`
- ☐ Add the same hand-crafted candidate sets (≥11 cases) and assert identical decisions
- ☐ All tests pass under `cargo test --features ai_eviction`

---

## Milestone 4: XGBoost and MLP Wiring

**Depends on:** M1 (Trait), M2 (Build with `ai_eviction_models`), Plan A (sibling export)

### XGBoost Policy
- ☐ Create `runtime/src/mm/eviction/xgboost.rs` with:
  - `pub struct XGBoostPolicy;`
  - `EvictionPolicy` impl that calls `generated::xgb_predict(features)` for each candidate
- ☐ Implement feature-vector extraction from `BlockMeta` + `GlobalState` (matches `FeatureExtractor.extract_candidate_features` in Python)
- ☐ Use the 27-feature vector by default (with `predicted_reuse_dist` heuristic)
- ☐ Verify: for 1,000 random feature vectors, predictions match Python within 1e-3 (sibling project already showed byte-perfect agreement)
- ☐ Benchmark inference latency on Cortex-A78 — target < 1 µs per candidate

### MLP Policy
- ☐ Create `runtime/src/mm/eviction/mlp.rs` with:
  - `pub struct MlpPolicy;`
  - `EvictionPolicy` impl that calls `generated::mlp_predict(features)` (int8-quantized version)
- ☐ Verify: 500 random vectors, decision agreement ≥ 95% on synthetic 8-candidate groups (matches sibling Python ↔ Rust verification)
- ☐ Benchmark inference latency on Cortex-A78 — target < 1 µs per candidate
- ☐ Confirm int8 model size is < 5 KB on disk (sibling reports 4.4 KB)

### Float32 Verification Build
- ☐ Add `#[cfg(test)]` build that pulls in `mlp_policy_f32.rs` for cross-check tests
- ☐ Test: int8 vs float32 MLP agreement on 1,000 vectors (target ≥ 99% within tolerance 0.05)

---

## Milestone 5: CACHEUS in Rust

**Depends on:** M1 (Trait), M3 (Classical), M4 (ML)

**Note:** Full implementation already exists in `slm_os_integration/src/cacheus.rs` with the corrected weight-update rule from the sibling project's Phase 5 work. Port directly.

### CacheusSelector
- ☐ Port `slm_os_integration/src/cacheus.rs` to `runtime/src/mm/eviction/cacheus.rs`
- ☐ Use `Vec<Box<dyn EvictionPolicy + Send>>` for the expert pool
- ☐ Multiplicative weight update with `f32` arithmetic (acceptable outside the hot path)
- ☐ Circular feedback buffer via `VecDeque<EvictionRecord>` (default `window_size=200`)
- ☐ `EvictionRecord` stores the **ensemble's actual choice** (not the last expert's choice — that was the original bug fixed in the sibling)
- ☐ Weight floor at `min_weight=0.01` then renormalize after each update

### Recommended Default Pool
- ☐ Wire the **`ml_only` pool** (XGBoost + MLP, `lr=0.4`, `window=200`) as the default `CACHEUS` instance — sibling Phase 5 sweep showed this beats `all_5` (0.212 vs 0.427 mean norm rate). Adding classical experts dilutes the ensemble.
- ☐ Provide `cacheus_all5()` constructor for ablation experiments

### Eviction Feedback Plumbing
- ☐ When `alloc_block()` evicts a block, record the `(model_id, layer_idx, pool_type)` content key in a small `EvictedContentTracker`
- ☐ When the next miss tries to load a block whose content key was recently evicted (within `EVICTION_FEEDBACK_WINDOW=200` ticks), call `policy.update_feedback(evicted_block_id, true)`
- ☐ Periodically (or on tracker overflow) flush stale records as `update_feedback(_, false)` — these were good evictions
- ☐ Mirror the simulator's `_evicted_content` design (`src/simulator/core.py:175-186`)

### Trajectory Recording (Optional)
- ☐ Behind a separate `cacheus_trace` feature flag, append `(tick, weights)` to a bounded ring buffer for shell inspection
- ☐ Expose via `eviction trajectory` shell subcommand (M7)

---

## Milestone 6: Wire into ModelAllocator

**Depends on:** M1-M5

**Note:** This is the actual integration point — making `alloc_weights` / `alloc_workspace` consult the active policy.

### Allocation Path Changes
- ☐ Modify `runtime/src/mm/model_mem.rs:alloc_weights()`:
  - When the weight pool has no free block, build the candidate list from currently-allocated blocks with `ref_count == 0`
  - If `candidates.is_empty()`: return `AllocError::OutOfMemory` (all blocks pinned)
  - Otherwise, call `ACTIVE_POLICY.lock().select_victim(&candidates)`
  - Free the chosen block (call existing `free()` internals), then allocate
  - Record the eviction in the `EvictedContentTracker` for CACHEUS feedback (M5)
- ☐ Same change to `alloc_workspace()`
- ☐ Update `PoolStats` to include `evictions_total` counter

### Access Tracking
- ☐ Add `pub fn touch(handle: ModelHandle)` that bumps `access_count` and updates `last_access_time`
- ☐ Call `touch()` from any FFI path that reads/writes block contents (kernel side via `mm_touch_block` syscall)
- ☐ Document the contract: callers MUST `touch()` for the policy to learn

### Verification
- ☐ Existing tests for `alloc_weights` / `alloc_workspace` still pass with default `LruPolicy`
- ☐ New test: fill the pool, free nothing, allocate one more — verify the LRU candidate is evicted
- ☐ New test: pin every block (`ref_count > 0`), allocate one more — verify `OutOfMemory`
- ☐ New test: switch to `XGBoostPolicy`, repeat the fill-and-allocate test, verify eviction happens

---

## Milestone 7: Shell Command

**Depends on:** M6

### `eviction` Subcommand
- ☐ Add to the existing shell:
  - `eviction` — show current policy name, total evictions, current pool utilizations
  - `eviction policy` — list available policies (`lru`, `lfu`, `arc`, `slm`, `xgboost`, `mlp`, `cacheus`)
  - `eviction policy <name>` — switch policy at runtime
  - `eviction stats` — per-policy decision count, expert weights (CACHEUS), recent fault rate
  - `eviction trajectory` — last N (cap 100) `(tick, weights)` records (CACHEUS only, gated on `cacheus_trace`)
- ☐ Mirror the AI-Sched `sched` command structure

### Stats Tracking
- ☐ Per-policy: total decisions, total fallbacks, average decision latency
- ☐ CACHEUS-specific: per-expert decision count, per-expert weight, per-expert fault rate
- ☐ Per-pool: total evictions, evictions/sec rolling average

---

## Milestone 8: Testing

### Unit Tests
- ☐ Port the 11 parity tests from `slm_os_integration/tests/parity.rs` (M3 covers most)
- ☐ Add `test_eviction_policy_swap` — switch under concurrent alloc/free, no panics
- ☐ Add `test_xgb_decision_matches_python` — load 100 known feature vectors with expected outputs (export from sibling project)
- ☐ Add `test_mlp_decision_matches_python` — same idea with int8 tolerance
- ☐ Add `test_cacheus_adapts_to_workload` — run a synthetic workload and verify weights skew toward the better expert

### Integration Tests
- ☐ `test_alloc_evicts_when_full` — pool full, all `ref_count=0`, allocate one more → eviction happens, allocation succeeds
- ☐ `test_alloc_returns_oom_when_all_pinned` — pool full, all `ref_count>0`, allocate one more → `OutOfMemory`
- ☐ `test_eviction_feedback_loop` — evict, re-access evicted content key within window, verify `update_feedback(_, true)` was called
- ☐ `test_eviction_window_expiry_signals_good` — evict, run past `EVICTION_FEEDBACK_WINDOW` ticks, verify `update_feedback(_, false)` was called

### Performance Tests
- ☐ `bench_xgb_inference_latency` — 1,000 calls, report avg/p50/p99; assert avg < 1 µs on real hardware
- ☐ `bench_mlp_inference_latency` — same; assert avg < 1 µs
- ☐ `bench_cacheus_inference_latency` — sums per-expert times; report breakdown
- ☐ `bench_lru_inference_latency` — baseline (target < 100 ns)

### QEMU Validation
- ☐ Boot SLM-OS with `ENABLE_AI_EVICTION_MODELS=ON`
- ☐ `eviction` shell command lists policies and switches them
- ☐ Run a synthetic memory-pressure workload (load 4 models > total pool size) and verify no leaks
- ☐ Compare end-to-end fault rate of `cacheus` vs `lru` (target: 50%+ reduction matching sibling simulator results)

---

## Milestone 9: Performance Validation

**Depends on:** M8

**Note:** Mirrors Phase AI-Sched M9. The sibling project measured Python latencies (XGBoost ~9 µs, MLP ~2 µs per candidate at batch=64) but those include Python overhead. Native Rust if-else chains should be sub-microsecond.

### Hardware Benchmarks
- ☐ Run latency benches on Cortex-A78 (Pi 5) — target < 1 µs avg
- ☐ Run on Jetson Orin Nano — target < 1 µs avg
- ☐ Run on x86-64 — informational only, not a target platform

### End-to-End Workload Comparison
- ☐ Reproduce sibling project's 7 workload scenarios as kernel test programs (single_inference, multi_model, hot_swap, burst_load, mixed_priority, gpu_contention, adversarial)
- ☐ Measure fault count and total latency under each policy
- ☐ Generate a comparison table matching `data/results/policy_scenario_matrix.csv` from the sibling

### Memory Overhead
- ☐ Measure: total binary size delta with `ENABLE_AI_EVICTION_MODELS=ON` vs OFF — target < 2 MB
- ☐ Measure: runtime memory overhead (CACHEUS state, eviction tracker) — target < 16 KB
- ☐ Document overhead in `docs/eviction.md`

---

## Phase AI-Eviction Completion Checklist

### Deliverables
- ☐ Pluggable `EvictionPolicy` trait integrated into `ModelAllocator`
- ☐ Four classical Rust policies (LRU, LFU, ARC, SLM-Heuristic) ported from sibling crate
- ☐ XGBoost and int8 MLP wired in via generated files; predictions agree with Python reference
- ☐ CACHEUS adaptive selector with `ml_only` default pool
- ☐ `eviction` shell command for runtime introspection and policy switching
- ☐ Eviction decisions trigger on pool exhaustion (replaces today's `OutOfMemory` failure)
- ☐ Inference latency < 1 µs on target hardware
- ☐ All tests pass with `ENABLE_AI_EVICTION` ON and OFF

### Demo
- ☐ Boot SLM-OS in QEMU with AI eviction
- ☐ `eviction policy` lists registered policies
- ☐ `eviction policy cacheus` switches to the adaptive ensemble
- ☐ Load 4 models that exceed pool capacity; verify successful allocation through eviction
- ☐ `eviction stats` shows expert weights adapting to the workload
- ☐ Switch back to `lru`; verify smooth transition

### Documentation
- ☐ `docs/eviction.md` — pluggable policy, trait, classical policies, ML policies, CACHEUS, shell commands
- ☐ Feature vector layout (27 features) reproduced from sibling `docs/features.md`
- ☐ Performance targets documented

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
- ⏸️ Online retraining: ship the simulator with the OS and let it produce updated weights from real workload traces
- ⏸️ Per-pool policy: different policies for weight vs workspace pools (current design uses one policy for both)
- ⏸️ Multi-feature predicted-reuse: extend the 27-feature vector with kernel-side signals not available to the simulator
