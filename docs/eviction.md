# AI-Driven Page Eviction

The `ModelAllocator` uses a pluggable `EvictionPolicy` trait to decide
which 2 MB block to evict when the weight or workspace pool is full.
Phase AI-Eviction lands the trait, the classical policies (LRU, LFU,
ARC, SLM-Heuristic), the trained XGBoost and int8 MLP predictors, and
the CACHEUS adaptive ensemble.

This document covers build flags, the ML import flow, and the runtime
layout of the eviction subsystem. The live tracker for open work is
the [Open Issues](#open-issues) section below; the original
Phase AI-Eviction TODO has been archived under
`docs/archive/todos/TODO - PHASE AI-Eviction.md` and is no longer
maintained.

---

## Build Flags

Eviction is on by default. There are now two user-facing controls:

| Flag | Adds | Binary cost |
|------|------|-------------|
| default build | Trait, registry, classical policies, stub ML predictors (`xgb_stub`, `mlp_stub` return 0.5) | ~30 KB |
| `EVICTION_MODELS=ON` | Trained XGBoost (~1.3 MB source) + int8 MLP (~5 KB) | still ~30 KB until M4 calls them; LTO drops the unused weight tables |
| `DISABLE_EVICTION=ON` | Compiles the eviction framework out entirely | saves the eviction-framework footprint |
| `EVICTION_DEFAULT_POLICY=<name>` | Chooses the compiled-in default policy (`lru`, `lfu`, `arc`, `slm`, `cacheus`, ...) | none beyond the selected built-in policy set |

`EVICTION_MODELS=ON` requires eviction to stay enabled. CMake enforces
that dependency at configure time: setting `ENABLE_EVICTION_MODELS=ON`
with `DISABLE_EVICTION=ON` aborts with a `FATAL_ERROR`.

### Build Examples

```bash
# Default: eviction enabled, default policy = LRU
make kernel

# Pick a different compiled-in default policy
make kernel EVICTION_DEFAULT_POLICY=lfu

# Full: real trained models (requires import step first)
./scripts/import_eviction_weights.sh
make kernel EVICTION_MODELS=ON

# Compile eviction out entirely
make kernel DISABLE_EVICTION=ON

# Combined with AI scheduler
make kernel AI_SCHED=ON EVICTION_MODELS=ON
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

### Latency Benchmarks (M9)

`rust_eviction_bench_latency_ns(name, iterations)` drives each
policy's `select_victim` 1000 times against a canned 8-candidate
set and returns the average per-call latency in nanoseconds. The
`test_bench_all_policies` Unity test prints numbers for every
registered policy so the values land in each `make test` log.

**QEMU baseline** (aarch64 `virt`, cortex-a76, Release build,
re-captured 2026-05-17 alongside the Pi 5 / Jetson runs below):

| Policy | Default build | Real models (`EVICTION_MODELS=ON`) |
|--------|---------:|---------:|
| first_candidate | 60 ns | 58 ns |
| lru | 75 ns | 279 ns |
| lfu | 109 ns | 342 ns |
| arc | 3,892 ns | 3,764 ns |
| slm | 1,026 ns | 1,287 ns |
| xgboost | 23,419 ns | 240,270 ns |
| mlp | 52,223 ns | 1,877,428 ns |
| cacheus (ml_only) | 100,313 ns | 2,124,355 ns |

QEMU TCG numbers fluctuate with host load and TB-cache warmup; treat
the table as order-of-magnitude rather than tight bounds. The
re-capture is up an order of magnitude on `mlp` / `cacheus` in the
default build (stub generators got slower since the M9 capture) and
~3–4× on `lru` / `lfu` under `EVICTION_MODELS=ON` — neither move
matters for the < 1 µs target evaluation, which lives on real silicon
below.

**Pi 5 hardware** (pi-5-2, BCM2712 Cortex-A76 @ 2.4 GHz, Release
build; captured 2026-05-17 from `test_bench_all_policies` for
[#108](https://github.com/SLM-OS/SLM-Operating-System/issues/108)):

| Policy | Default build | Real models (`EVICTION_MODELS=ON`) |
|--------|---------:|---------:|
| first_candidate | 4 ns | 4 ns |
| lru | 18 ns | 49 ns |
| lfu | 25 ns | 58 ns |
| arc | 497 ns | 497 ns |
| slm | 157 ns | 167 ns |
| xgboost | 4,117 ns | 15,029 ns |
| mlp | 4,157 ns | 129,151 ns |
| cacheus (ml_only) | 8,999 ns | 145,061 ns |

**Jetson Orin Nano hardware** (jetson-nano-1, Cortex-A78AE @ 1.5 GHz
under NS-EL2/VHE, Release build; captured 2026-05-17 from
`test_bench_all_policies` for
[#109](https://github.com/SLM-OS/SLM-Operating-System/issues/109)):

| Policy | Default build | Real models (`EVICTION_MODELS=ON`) |
|--------|---------:|---------:|
| first_candidate | 4 ns | 4 ns |
| lru | 26 ns | 105 ns |
| lfu | 33 ns | 69 ns |
| arc | 621 ns | 627 ns |
| slm | 221 ns | 227 ns |
| xgboost | 6,098 ns | 21,956 ns |
| mlp | 6,170 ns | 174,248 ns |
| cacheus (ml_only) | 13,311 ns | 197,228 ns |

Cortex-A76 (Pi 5, 2.4 GHz) consistently beats Cortex-A78AE (Jetson,
1.5 GHz) — observed Jetson/Pi 5 ratios sit around 1.3–1.5×, a bit
better than the raw 1.6× clock ratio thanks to A78AE's wider issue
width. Real silicon is over an order of magnitude faster than QEMU
TCG on the heavy ML policies (Pi 5 `EVICTION_MODELS=ON` cacheus is
145 µs vs the refreshed QEMU number of 2,124 µs in the table above
— ~15×), confirming the M9 prediction that QEMU's TCG-emulated int8
MLP forward pass was not representative of hardware. The **< 1 µs Cortex-A78 target from
the Phase AI-Eviction TODO is missed on every hardware config**:
xgboost lands at 4–22 µs, mlp at 4–174 µs, cacheus at 9–197 µs. The
classical policies (`first_candidate`, `lru`, `lfu`, `arc`, `slm`) all
stay safely under 1 µs on both platforms in both build configs.
Tree-pruning the XGBoost ensemble and batching the MLP forward pass
are the two known speed-up levers carried over from the sibling
project; tracked as a perf follow-up against the M9 deliverable.

**GPU dispatch status (as of 2026-05-17) — CPU-only today on every
platform.** The numbers above are all host-CPU paths. SLM-OS's Jetson
GA10B fastpath covers MNIST/model inference (`gpu use inference on`)
and the AI scheduler MLP (`gpu use sched on`, hardware-verified on
jetson-nano-2 since PR-3 of `docs/design/gpu-policy-models.md`,
landed 2026-04-27) — the eviction MLP is NOT in that set. The
eviction trait method `EvictionPolicy::has_gpu_backend()` defaults to
`false` and no production policy overrides it; the matching dispatch
function `slm_gpu_run_eviction_inference` is named in design notes
but does not exist in tree. `gpu use eviction <on|off>` is a scaffold
toggle that prints "scaffold only — no eviction policy declares a GPU
backend yet" (see `kernel/src/gpu_consumer.c`). The originally-spec'd
GA10B work is tracked by
[#964](https://github.com/SLM-OS/SLM-Operating-System/issues/964)
(PR-5: SASS shaders + Linux producer) and
[#965](https://github.com/SLM-OS/SLM-Operating-System/issues/965)
(PR-6: SLM-OS dispatch + flip `has_gpu_backend=true`), both deferred
because the eviction MLP has no compiled-in weight source (unlike
`kernel/sched/ai/ai_weights_mlp.c` for the sched MLP). See
`docs/design/gpu-policy-models.md` §"Sequencing" for the full
roadmap. On Pi 5 there is no GPU-side path in any form: VideoCore is
intentionally stubbed and the AI HAT+ (Hailo-8L) is wedged at
boundary IN ch=2 by
[#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682)
(closed not-planned — wedge documented but work deferred), plus
there is no design pass for eviction-on-Hailo.

### Memory Overhead (M9)

Binary size (QEMU_VIRT Release `slmos.elf`):

| Config | ELF size | Δ vs eviction-disabled build |
|--------|---------:|---------:|
| `DISABLE_EVICTION=ON` | 2,444,536 B | — |
| default build | 2,580,376 B | +135,840 B (~133 KB) |
| `EVICTION_MODELS=ON` | 2,789,944 B | +345,408 B (~337 KB) |

The stripped `slmos.bin` footprint is smaller (+48 KB stubs /
+254 KB real), well under the 2 MB M9 target.

Runtime heap (order-of-magnitude):

- `CacheusSelector::ml_only`: 2 boxed experts + 2 f32 weights +
  counters + a 200-slot `VecDeque<EvictionRecord>`. Upper bound ~8 KB.
- `EvictedContentTracker` (default): 256-slot
  `VecDeque<TrackerEntry>` at 16 B/entry = ~4 KB.
- Classical policies: zero additional heap beyond the `Box`.

Total under 16 KB when CACHEUS is installed.

### Runtime Contracts

A few invariants are load-bearing but non-obvious; they're documented
here so future callers don't trip over them.

- **Task context only.** Every public entry in `mm::eviction::*`
  expects to run from a task. The model_mem spinlock is not IRQ-safe,
  and CACHEUS allocates from the global heap during `select_victim`.
  Do not call `select_victim` / `score` / `update_feedback` /
  `set_eviction_policy` from an interrupt handler.
- **FP / NEON state.** The Rust eviction path uses `f32` arithmetic
  (CACHEUS weight updates, the MLP forward pass, the feature
  extractor's `log1pf` / divisions). This is safe on ARM64 because
  `kernel/arch/arm64/context.S` eagerly saves all 32 NEON registers
  plus `FPCR` / `FPSR` on every context switch (see its
  "Save FPU/SIMD registers (eager save for SLM workloads)" comment).
  The runtime crate is compiled without `-mgeneral-regs-only`, so
  `f32` is a legal ISA-level operation. Kernel C callers that need to
  emit floats directly (e.g. the shell's stats printer) must either
  route through the AI-Sched `FP_CONTEXT_SAVE()` wrappers or use
  integer-encoded values — M7's `expert_weights_bp: [u32; 5]` is an
  example. Test coverage: every `make test` run with eviction enabled
  exercises 91 internal CACHEUS / MLP / feature-extraction checks
  while QEMU's timer preempts; no FP corruption has been observed.
- **Lock ordering.** `alloc_weights` / `alloc_workspace` release the
  allocator `LOCK` before calling `eviction::select_victim`, and
  re-acquire it to free the victim and retry. The registry's
  `REGISTRY_LOCK` is therefore never nested inside the allocator
  `LOCK`. CACHEUS's heap allocations go through
  `linked_list_allocator::LockedHeap`, a third independent lock
  domain — no ABBA cycle is possible.
- **Cross-CPU coherency.** `EVICTED_CONTENT_TRACKER` is accessed
  under the allocator `LOCK`. Its cross-CPU visibility inherits from
  the existing model memory pools, which on cacheable Pi 5 memory
  rely on the spinlock's `Acquire` / `Release` semantics (these map
  to `DMB ISH` on ARM64). The same coherency question applies to the
  whole `ModelAllocator` on Pi 5; it is tracked as part of the multi-
  CPU concurrent alloc stress test (#116), not as an eviction-
  specific concern.

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
Unity suite (24 tests) alongside the existing `test_suite_model_mem`:

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
- **M9 latency benchmarks** (1 test, skips when feature off):
  - `test_bench_all_policies` — runs `rust_eviction_bench_latency_ns`
    against every registered policy (8 total), prints the QEMU
    numbers to the test log, and asserts each stays under 10 ms per
    call (sanity cap; the < 1 µs hardware target lives in the
    benchmarks table above). Numbers surface in every `make test`
    run with eviction enabled.

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
  for both, and — only under `EVICTION_MODELS=ON` — int8 MLP vs
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
  `EVICTION_MODELS=ON`, three Python-parity smoke cases exercise
  the XGBoost + MLP loaded code path.

All suites pass under `make test` on the three supported configs:
`DISABLE_EVICTION=ON`, default build, and `EVICTION_MODELS=ON`.

---

## Open Issues

- ☐🎫 XGBoost eviction on-device vs. trainer numerical equivalence —
  [#932](https://github.com/SLM-OS/SLM-Operating-System/issues/932).
  Adds `bench xgb-equiv-evict` against the producer-side corpus
  (`evict.smb` + `test_vectors_xgb_evict.bin` + `expected_evict.bin`)
  landed in `slm-os-page-eviction` PR #3. Consumer side bundled with
  [#448](https://github.com/SLM-OS/SLM-Operating-System/issues/448).
- ☐🔗🎫 Generalize runtime policy blob formats —
  [#448](https://github.com/SLM-OS/SLM-Operating-System/issues/448).
  Reshapes the eviction-blob envelope (magic/version negotiation,
  feature-schema versioning, reserved-field forward-compat, structured
  reject path). Now absorbs the #932 consumer side: FFI
  `rust_eviction_xgb_predict`, `bench xgb-equiv-evict` shell verb,
  `eviction blob load/activate xgboost`, kernel-side mini-corpus
  regression, and `docs/benchmarks.md` entry. The envelope rework is
  done only when the bundled #932 hardware verification on pi-5-2
  passes.
- ⏸️🔗🎫 Promote XGBoost from opt-in to default eviction policy —
  [#953](https://github.com/SLM-OS/SLM-Operating-System/issues/953).
  Decision-gate ticket. Held (`blocked` label) until #932 closes with
  passing equivalence on pi-5-2. Dependencies also include
  [#110](https://github.com/SLM-OS/SLM-Operating-System/issues/110)
  (x86-64 latency capture; Pi 5 + Jetson captured 2026-05-17 — closes
  [#108](https://github.com/SLM-OS/SLM-Operating-System/issues/108) /
  [#109](https://github.com/SLM-OS/SLM-Operating-System/issues/109)),
  [#116](https://github.com/SLM-OS/SLM-Operating-System/issues/116)
  (multi-CPU stress), and the sibling-repo quality harness
  ([slm-os-page-eviction#2](https://github.com/SLM-OS/slm-os-page-eviction/issues/2)).
- ☐🎫 ML eviction policies miss the < 1 µs Cortex-A78 target —
  [#961](https://github.com/SLM-OS/SLM-Operating-System/issues/961).
  Hardware capture on pi-5-2 / jetson-nano-1 has xgboost at 4–22 µs,
  mlp at 4–174 µs, cacheus at 9–197 µs (see §"Latency Benchmarks
  (M9)" above). Levers: prune the 200-tree XGBoost ensemble and batch
  the int8 MLP forward pass — both carried over from the sibling
  project's Phase 5 work.
- ☐🎫 Continuous eviction-quality eval harness (sibling repo) —
  [slm-os-page-eviction#2](https://github.com/SLM-OS/slm-os-page-eviction/issues/2).
  Replay xgb / mlp / cacheus / arc / lru through the simulator on
  held-out trajectories; report hit-rate, eviction count, tail-latency
  proxy; CI knob for hit-rate regression. Feeds #953's decision memo.

---

## References

- Sibling reference crate: `slm-os-page-sim/slm_os_integration/` (zero-
  dependency Rust port of the Python simulator, parity-tested).
- Python simulator: `slm-os-page-sim/src/simulator/` — the ground truth
  for workload traces, feature extraction, and policy decisions.
- Phase 5 sweep: `slm-os-page-sim/data/results/RESULTS.md` — why
  `ml_only` (XGBoost + MLP, `lr=0.4`, `window=200`) is the default
  CACHEUS pool.
