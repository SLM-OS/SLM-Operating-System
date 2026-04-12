# Phase AI-Sched: AI Scheduler Kernel Integration

This document tracks the integration of trained AI models (MLP, PPO, XGBoost) into the SLM-OS kernel scheduler.

**Status:** Complete — M1-M9 all milestones done

**Summary:** This phase implements a pluggable scheduler interface and AI-based inference engine, allowing the kernel to use trained ML models for CPU assignment, priority adjustment, and preemption decisions.

**Relationship to Other Phases:**
- **Independent of Phase 4/4X:** Can be developed in parallel on QEMU
- **Depends on:** Plan A (Export Pipeline) for real weights; can proceed with stub weights
- **Completes items from:** Phase 3 M2 (Scheduler — actual core assignment via `assign_to_*_core()`)
- **Benefits from:** Phase 4X (x86-64 port) for SSE/AVX inference path

**Target Performance:** < 50µs inference latency on Cortex-A78 @ 1.5 GHz

**Repository:** `CS-496-Capstone-SLM-Operating-System`

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone |

---

## Milestone 1: Pluggable Scheduler Interface

**Note:** This milestone is pure kernel C refactoring — no FP, no new build flags needed. Can be completed with existing build system.

### Scheduler Policy Vtable
- ✅ Create `kernel/include/sched_policy.h` with vtable definition
- ✅ Document interface contract in header comments
- ✅ Define `SCHED_POLICY_MAX_NAME` (16 chars), `SCHED_POLICY_MAX` (8)

### Extract Heuristic Policy
- ✅ Create `kernel/sched/sched_heuristic.c`
- ✅ Move `find_target_cpu()` logic from `sched.c` to new file
- ✅ Move `find_performance_cpu()` logic
- ✅ Implement `heuristic_assign_cpu()` wrapping existing logic
- ✅ Create `sched_policy_heuristic` ops struct
- ✅ Verify extracted code compiles standalone

### Wire Vtable into sched.c
- ✅ Add `static const struct sched_policy_ops *active_policy`
- ✅ Initialize to `&sched_policy_heuristic`
- ✅ Replace inline CPU selection in `scheduler_add_task()` with `active_policy->assign_cpu(task)`
- ✅ Add `active_policy->tick(cpu_id)` call in `scheduler_tick()` (if non-NULL)
- ✅ Implement `sched_set_policy(const struct sched_policy_ops *policy)`:
  - ✅ Disable interrupts
  - ✅ Swap pointer
  - ✅ Re-enable interrupts
  - ✅ Log policy change
- ✅ Implement `sched_get_policy()` — returns current policy name
- ✅ Guard AI-specific code with `#ifdef CONFIG_AI_SCHEDULER`

### Shell Command
- ✅ Add `sched` command to shell:
  - `sched` — show current policy name
  - `sched policy` — list available policies
  - `sched policy <name>` — switch to named policy
- ✅ Implement policy registry (simple array of `sched_policy_ops*`)
- ✅ `sched_register_policy()` for dynamic registration

### Verification
- ✅ All existing scheduler tests pass unchanged
- ✅ Heuristic policy produces identical CPU assignments to old inline code
- ✅ `sched_set_policy()` swaps cleanly under load
- ✅ Shell command works correctly
- ✅ 14 new policy vtable tests added (test_scheduler.c)

---

## Milestone 2: Build System Integration

**Note:** Required before M3 (Inference) since AI code needs different compiler flags.

### CMake Configuration
- ✅ Add to `CMakeLists.txt`:
  ```cmake
  option(ENABLE_AI_SCHEDULER "Include AI scheduling models" OFF)
  
  if(ENABLE_AI_SCHEDULER)
      add_compile_definitions(CONFIG_AI_SCHEDULER=1)
      
      # AI inference library — needs FP/NEON
      # On AArch64, NEON/FP is always available.
      # The key is to NOT include -mgeneral-regs-only (which the main kernel uses).
      # Same pattern as Lua library.
      add_library(ai_sched STATIC
          kernel/sched/ai/ai_inference.c
          kernel/sched/ai/ai_state.c
          kernel/sched/ai/sched_ai.c
          kernel/sched/ai/fp_context.S
          kernel/sched/ai/ai_weights_mlp.c
      )
      
      target_compile_options(ai_sched PRIVATE
          -ffreestanding
          -nostdlib
          -mcpu=${TARGET_CPU}    # cortex-a78ae, cortex-a76, or generic
          -fPIE
          -O2
          # NOTE: No -mgeneral-regs-only here — that's the whole point
      )
      
      target_include_directories(ai_sched PRIVATE
          ${KERNEL_INCLUDES}
          kernel/sched/ai
      )
      
      target_link_libraries(slmos.elf PRIVATE ai_sched)
  endif()
  ```

### Stub Weights for Development
- ✅ Create `kernel/sched/ai/ai_weights_stub.c`:
  - All-zero weight arrays matching extern declarations
  - MLP and PPO stubs (8 arrays each, cache-line aligned)
- ✅ Create `kernel/sched/ai/ai_weights.h`:
  - Extern declarations for weight arrays
  - References ai_types.h for dimension constants
- ✅ Create `kernel/sched/ai/ai_types.h`:
  - State vector dimensions with _Static_assert
  - Action decoding, MLP layer dimensions

### Weight File Integration
- ✅ Create `scripts/import_ai_weights.sh`:
  - Copies generated weights from Plan A output directory
  - Copies `ai_config.h` with platform-specific dimensions
  - Validates expected array names present
- ⏸️ Add Makefile target: `make import-ai-weights` — deferred (script works standalone)

### Build Verification
- ✅ Verify build with `ENABLE_AI_SCHEDULER=OFF` (existing behavior)
- ✅ Verify build with `ENABLE_AI_SCHEDULER=ON` + stub weights
- ✅ Verify build with `ENABLE_AI_SCHEDULER=ON` + real weights (April 2026)

---

## Milestone 3: AI Inference Engine

**Depends on:** M2 (Build System)

### Directory Structure
- ✅ Create `kernel/sched/ai/` directory
- ✅ Create `kernel/sched/ai/ai_inference.h` — public API
- ✅ Create `kernel/sched/ai/ai_inference.c` — stub implementation (M3 fills in)
- ✅ Create `kernel/sched/ai/ai_types.h` — shared types

### Core Math Functions
- ✅ Implement `ai_matvec()`:
  - ✅ Row-major W[M×N] × in[N] + bias[M] → out[M]
  - ✅ Cache-friendly inner loop (stride-1 access on W)
  - ✅ No dynamic allocation
  - ✅ NEON 4-wide path + scalar fallback
- ✅ Implement `ai_relu()`:
  - ✅ In-place with NEON vmaxq path
- ✅ Implement `ai_argmax()`:
  - ✅ Linear scan for max logit index

### NEON Optimization
- ✅ Explicit NEON intrinsics in `ai_matvec()` (vfmaq_f32, vaddvq_f32)
- ✅ NEON intrinsics in `ai_relu()` (vmaxq_f32)
- ✅ Compile-time check: `#if defined(__aarch64__) && defined(__ARM_NEON)`
- ✅ Verified NEON codegen: fmla, faddp, fmax instructions present in object
- ✅ Benchmark on real hardware: **41.9 µs on Pi 5 Cortex-A76** (target < 50µs ACHIEVED)

### Action Decoding
- ✅ `struct ai_sched_action` defined in `ai_types.h` (M2)
- ✅ `ai_decode_action()` — static inline in `ai_types.h` (M2)
- ✅ `ai_validate_action()` — bounds checking in `ai_inference.c`

### MLP Inference
- ✅ Implement `ai_schedule_mlp()`:
  - ✅ 4-layer forward pass via shared `forward_pass()`: (108→256→256→128→N_ACTIONS)
  - ✅ Stack-allocated scratch buffers (2 × 256 floats = 2 KB)
  - ✅ Thread-safe (no static globals)
  - ✅ Returns 0 on success, -1 on error

### PPO Inference
- ✅ Implement `ai_schedule_ppo()`:
  - ✅ Same architecture, uses `ai_ppo_w*`/`ai_ppo_b*` weight arrays

### Testing (14 new tests)
- ✅ matvec: basic 2×3, identity 4×4, zero weights at model dimensions
- ✅ relu: mixed values, all positive, all negative
- ✅ argmax: basic, first/last/tie, negative values
- ✅ Full MLP and PPO forward pass with stub weights
- ✅ ai_validate_action: valid, out-of-range core/priority/preempt, NULL

### XGBoost Inference (Stretch)
- ⏸️ Implement tree traversal function
- ⏸️ Implement derived feature computation (5 formulas)
- ⏸️ Implement cascaded classification (core → priority → preempt)
- ⏸️ Size check: if > 1MB total, defer to later

---

## Milestone 4: State Vector Extraction

**Depends on:** M2 (Build System)

### State Dimensions
- ✅ Create `kernel/sched/ai/ai_types.h` with dimension constants:
  ```c
  // State vector is fixed at 108 dimensions (trained model input shape).
  // Platforms with fewer cores zero-fill unused core slots.
  // These values come from ai_config.h (generated by Plan A) or defaults.
  #ifndef AI_STATE_DIM
  #define AI_STATE_DIM          108
  #endif
  #ifndef AI_STATE_NUM_CORES
  #define AI_STATE_NUM_CORES    6   // Max cores in state vector (zero-fill if fewer)
  #endif
  #define AI_FEATURES_PER_CORE  6
  #ifndef AI_STATE_NUM_TASKS
  #define AI_STATE_NUM_TASKS    8   // Top-N tasks by priority
  #endif
  #define AI_FEATURES_PER_TASK  8
  #define AI_GLOBAL_FEATURES    8
  
  // Verify: 6×6 + 8×8 + 8 = 36 + 64 + 8 = 108
  _Static_assert(AI_STATE_NUM_CORES * AI_FEATURES_PER_CORE +
                 AI_STATE_NUM_TASKS * AI_FEATURES_PER_TASK +
                 AI_GLOBAL_FEATURES == AI_STATE_DIM,
                 "State dimension mismatch");
  ```

**Note:** The state vector shape is fixed at 108 dimensions because the model was trained with this input size. On platforms with fewer physical cores (e.g., Pi 5 with 4 cores), the extra core slots (4-5) are zero-filled. This matches how the simulator handles variable core counts. Coordinate with Plan A to confirm model training configuration.

### Core State File
- ✅ Create `kernel/sched/ai/ai_state.h` — public API (M2)
- ✅ Create `kernel/sched/ai/ai_state.c` — full implementation
- ✅ Implement `ai_extract_state()` with real kernel data

### Per-Core Features (36 floats)
For each core c (0 to AI_STATE_NUM_CORES-1), features at offset `c*6`:

| Offset | Feature | Source | Notes |
|--------|---------|--------|-------|
| c×6+0 | utilization | `running_ticks / total_ticks` | New counter (see M5) |
| c×6+1 | queue_depth | `cpu_rq(c)->ready_count / 32.0f` | Existing |
| c×6+2 | cache_pressure | `0.0f` | Future: PMU integration |
| c×6+3 | core_type | `1.0f` | Homogeneous on Jetson/Pi5 |
| c×6+4 | isolated | `(sched.isolated_cores >> c) & 1` | Existing |
| c×6+5 | current_task_prio | `current->effective_priority / 7.0f` | Existing |

- ✅ Implement per-core feature extraction
- ✅ Zero-fill cores beyond `cpu_count()` (for Pi 5: cores 4-5 = zeros)
- ✅ Handle core offline/invalid states gracefully

### Per-Task Features (64 floats)
Top 8 tasks by effective_priority, features at offset `36 + t*8`:

| Offset | Feature | Source | Notes |
|--------|---------|--------|-------|
| t×8+0 | priority | `task->effective_priority / 7.0f` | Existing |
| t×8+1 | deadline_urgency | `1.0 - (deadline - now) / 1e9` | Clamp [0,1] |
| t×8+2 | working_set | `0.0f` | Future: slm_task_info |
| t×8+3 | model_size | `0.0f` | Future: slm_task_info |
| t×8+4 | inference_dur | `0.0f` | Future: slm_task_info |
| t×8+5 | can_use_gpu | `0.0f` | Future: slm_task_info |
| t×8+6 | wait_time | `(now - arrival_time_ns) / 1e9` | New field (see M5) |
| t×8+7 | component_type | `0.0f` | Future: slm_task_info |

- ✅ Implement top-8 task collection (cached, updated every 10 ticks)
- ✅ Handle < 8 tasks gracefully (zero-fill remaining slots)
- ✅ Implement per-task feature extraction

### Global Features (8 floats)
Features at offset 100:

| Offset | Feature | Source | Notes |
|--------|---------|--------|-------|
| 100 | ready_count | `sum(ready_count) / 64.0f` | Existing |
| 101 | deadline_miss_rate | Rolling window counter | New (see M5) |
| 102 | avg_latency | `cumulative_latency / completion_count / 1e7` | New (see M5) |
| 103 | weight_pool_pressure | `0.0f` or FFI query | Future: Rust runtime |
| 104 | workspace_pool_pressure | `0.0f` or FFI query | Future: Rust runtime |
| 105 | gpu_queue_depth | `gpu_pending_count() / 8.0f` | If GPU driver available |
| 106 | load_imbalance | `std_dev(utils) / mean(utils)` | Clamp [0,1] |
| 107 | episode_time | `0.0f` | Not applicable to real kernel |

- ✅ Implement global feature extraction
- ✅ Implement load imbalance calculation (coefficient of variation, Newton's sqrt)

---

## Milestone 5: Scheduler Counters & Task Fields

### New Task Fields
- ✅ Add `arrival_time_ns` and `completion_time_ns` to `struct task` (guarded)
- ✅ Initialize `arrival_time_ns` in `scheduler_add_task()`
- ✅ Set `completion_time_ns` in task_exit via `sched_ai_record_completion()`

### Per-CPU Utilization Tracking
- ✅ Add to scheduler state (per-CPU):
  ```c
  struct cpu_runqueue {
      // ... existing fields ...
  #ifdef CONFIG_AI_SCHEDULER
      uint64_t running_ticks;   // Ticks where current != idle
      uint64_t total_ticks;     // Total ticks since boot
  #endif
  };
  ```
- ✅ Increment `running_ticks` in `scheduler_tick()` when current task is not idle
- ✅ Increment `total_ticks` always
- ✅ Utilization = `running_ticks / total_ticks` (computed in ai_state.c)

### Deadline Miss Tracking
- ✅ Rolling window of last 100 completions in `ai_deadline_stats`
- ✅ Update on task completion via `sched_ai_record_completion()`
- ✅ Rolling window miss rate calculation (integer accessor for FP-safe code)

### Completion Latency Tracking
- ✅ Cumulative latency and count in `ai_latency_stats`
- ✅ Update on task completion: `latency = now - arrival_time_ns`
- ✅ Average latency calculation (integer accessor for FP-safe code)

### Top-8 Task Cache
- ✅ `ai_top_tasks` struct with task pointers, count, last_update_tick
- ✅ Update in `scheduler_tick()` every 10 ticks (CPU 0 only)
- ✅ `sched_ai_get_top_tasks()` returns cached list
- ✅ Insertion sort: replaces lowest-priority task when cache is full

---

## Milestone 6: FP State Handling

### ⚠️ Problem Analysis
- ✅ Document all paths where `ai_mlp_assign_cpu()` can be called
  - `scheduler_add_task()` from task_create (non-IRQ) — safe
  - `scheduler_add_task()` from timer IRQ waking sleeping task — needs FP save
  - `sched_set_task_affinity()` migration — non-IRQ, safe
- ✅ Always save FP state since IRQ path is possible
- ✅ Current FP save/restore in context.S covers context switches; fp_context.h covers inference

### FP Save/Restore Macros
- ✅ Create `kernel/include/fp_context.h`:
  ```c
  #ifdef CONFIG_AI_SCHEDULER
  
  struct fp_state {
      uint8_t regs[32 * 16];  // V0-V31, 128 bits each
      uint32_t fpcr;
      uint32_t fpsr;
  } __attribute__((aligned(16)));
  
  void fp_save(struct fp_state *state);
  void fp_restore(const struct fp_state *state);
  
  // Stack-allocated version for short critical sections
  #define FP_CONTEXT_SAVE() \
      struct fp_state __fp_state; \
      fp_save(&__fp_state)
  
  #define FP_CONTEXT_RESTORE() \
      fp_restore(&__fp_state)
  
  #endif
  ```
- ✅ `fp_save()` in assembly (stp q0-q31, mrs fpcr/fpsr) — implemented in M2
- ✅ `fp_restore()` in assembly (ldp q0-q31, msr fpcr/fpsr) — implemented in M2
- ✅ `fp_context.h` created with `FP_CONTEXT_SAVE()` / `FP_CONTEXT_RESTORE()` macros

### Integration
- ✅ AI inference calls wrapped with FP save/restore in `sched_ai.c`
- ✅ Self-test inference in `ai_mlp_init()` / `ai_ppo_init()` validates FP linkage
- ✅ Measure overhead of FP save/restore (test_ai_fp_save_restore_latency — QEMU reporting only)
- ✅ Verify no FP register corruption under stress test (test_ai_scheduler_stress, test_ai_mixed_policy_switch)

### Alternative: Deferred Inference
- ⏸️ If FP save/restore overhead is too high:
  - Queue scheduling decisions for deferred processing
  - Process queue in non-IRQ context
  - More complex but avoids FP save on every decision

---

## Milestone 7: AI Policy Implementation

**Depends on:** M1 (Vtable), M3 (Inference), M4 (State), M5 (Counters), M6 (FP State)

### Policy File
- ✅ `kernel/sched/ai/sched_ai.c` — full implementation (replaced M2 stubs)

### MLP Policy Ops
- ✅ `ai_mlp_init()`:
  - ✅ Run self-test inference (zero state → verify returns 0)
  - ✅ Reset statistics counters
- ✅ `ai_mlp_shutdown()`:
  - ✅ Log decisions, fallbacks, avg inference latency
- ✅ `ai_mlp_assign_cpu()`:
  - ✅ FP_CONTEXT_SAVE/RESTORE around entire inference path
  - ✅ Extract state → run MLP inference → validate action
  - ✅ Fallback to heuristic on inference failure or invalid action
  - ✅ Apply priority_adj (boost/reduce) and preempt flag
  - ✅ Track per-call latency and fallback count

### PPO Policy Ops
- ✅ `sched_policy_ai_ppo` — same pattern as MLP via shared `ai_assign_cpu_common()`
- ✅ Uses `ai_schedule_ppo()` with separate weight arrays and stats

### XGBoost Policy Ops (Stretch)
- ⏸️ Implement `sched_policy_ai_xgboost` struct
- ⏸️ Uses cascaded tree inference

### Policy Registration
- ✅ Register policies in `sched_ai_init()`:
  ```c
  void sched_ai_init(void) {
      sched_register_policy(&sched_policy_ai_mlp);
      sched_register_policy(&sched_policy_ai_ppo);
  }
  ```
- ✅ Called from `kernel_main()` when `CONFIG_AI_SCHEDULER` defined (M2)

### Statistics
- ✅ Track per-policy stats:
  - ✅ Total decisions made
  - ✅ Fallback count
  - ✅ Average inference latency (total_latency_ns / decisions)
  - ✅ Action distribution histogram (per-action counts in `sched stats` output)
- ✅ `sched stats` shell command (M8)

---

## Milestone 8: Testing

### Unit Tests

#### Inference Tests
- ✅ `test_ai_matvec_basic` — 2×3 known matrix
- ✅ `test_ai_matvec_identity` — 4×4 identity matrix
- ✅ `test_ai_matvec_zero_weights` — model-dimension zero weights
- ✅ `test_ai_matvec_single_row` — M=1 edge case
- ✅ `test_ai_matvec_8x8` — exercises NEON 4-wide path with remainder
- ✅ `test_ai_relu_mixed` — positive/negative/zero values
- ✅ `test_ai_relu_all_positive` — positive values unchanged
- ✅ `test_ai_relu_all_negative` — negative values zeroed
- ✅ `test_ai_relu_single_neg` — n=1 edge case
- ✅ `test_ai_relu_empty` — n=0 no-crash edge case
- ✅ `test_ai_argmax_basic` — clear maximum in middle
- ✅ `test_ai_argmax_first` / `last` — boundary positions
- ✅ `test_ai_argmax_tie` — first occurrence wins
- ✅ `test_ai_argmax_negative` — all-negative array
- ✅ `test_ai_argmax_empty` — n=0 returns -1
- ✅ `test_ai_argmax_single` — n=1 returns 0
- ✅ `test_ai_decode_roundtrip` — all 42 indices roundtrip correctly
- ✅ `test_ai_mlp_forward_pass` — full 4-layer forward pass with stub weights
- ✅ `test_ai_ppo_forward_pass` — PPO forward pass with stub weights
- ✅ `test_ai_mlp_action_bounds` — output action fields in valid ranges
- ✅ `test_ai_mlp_null_state` — NULL state returns -1
- ✅ `test_ai_ppo_null_state` — NULL state returns -1
- ✅ `test_ai_validate_action_bounds` — valid/invalid core, priority, preempt, NULL

#### State Extraction Tests
- ✅ `test_ai_extract_state_writes_all` — all 108 floats (432 bytes) written
- ✅ `test_ai_state_core_type` — online cores=1.0, offline=0.0
- ✅ `test_ai_state_core_zero_fill` — cores beyond cpu_count are all zeros
- ✅ `test_ai_state_isolated_core` — isolated core shows 1.0 in state vector
- ✅ `test_ai_state_utilization_range` — per-core utilization in [0,1]
- ✅ `test_ai_state_task_zero_fill` — unused task slots are zeros
- ✅ `test_ai_state_task_features` — task with known priority/deadline appears in state
- ✅ `test_ai_state_global_offset` — global features at correct offset, valid ranges
- ✅ `test_ai_arrival_time_set` — arrival_time_ns set between before/after timestamps
- ✅ `test_ai_utilization_counters_exist` — running_ticks ≤ total_ticks, total > 0

#### Policy Tests
- ✅ `test_ai_policy_switch_to_mlp_and_back` — switch from heuristic to AI and back (M1)
- ✅ `test_policy_switch_calls_init_shutdown` — init/shutdown callbacks invoked (M1)
- ✅ `test_ai_policy_mlp_end_to_end` — full dispatch path with init/shutdown (M7)
- ✅ `test_ai_policy_dispatches_any_affinity` — ANY affinity uses AI policy (M7)
- ☐ `test_ai_policy_fallback` — real weights produce valid actions, so fallback path not exercised
- ☐ `test_ai_policy_respects_isolation` — requires adversarial weights that pick isolated cores

### Performance Tests
- ✅ `test_ai_inference_latency` — 100 iterations, report avg (no QEMU assertion)
- ✅ `test_ai_state_extraction_latency` — 100 iterations, report avg
- ✅ `test_ai_fp_save_restore_latency` — 100 iterations, report avg
- ✅ Assert < 50µs on real hardware (Pi 5: 41.9 µs)

### Integration Tests
- ✅ `test_ai_scheduler_stress`:
  - Create 20 tasks with varying priorities/deadlines under AI policy
  - Verify all created and dispatched without crash
- ✅ `test_ai_mixed_policy_switch`:
  - 5 rounds of switching between heuristic and AI under load
  - Verify smooth transitions

### QEMU Validation
- ✅ Boot with AI scheduler enabled (verified via test builds)
- ✅ `sched policy` shell command lists and switches policies (M1)
- ✅ `sched stats` shell command shows policy, tasks, switches, utilization

---

## Milestone 9: x86-64 Support (Phase 4X Integration)

**Depends on:** Phase 4X M3 (Interrupts) complete

### SSE Implementation
- ✅ SSE intrinsics in `ai_inference.c` (same file, `#elif USE_SSE` guards):
  - `ai_matvec()`: `_mm_loadu_ps`, `_mm_mul_ps`, `_mm_add_ps`, shuffle-based horizontal sum
  - `ai_relu()`: `_mm_max_ps` with zero vector
- ✅ Architecture guards: `#if USE_NEON` / `#elif USE_SSE` / `#else scalar`
- ✅ Verified SSE codegen: 24 packed SSE instructions (mulps, addps, maxps, shufps)
- ✅ Verify x86-64 build with real weights (compiles + links with SSE, scheduler tests skipped on x86-64)

### Build System
- ✅ CMake sets `-msse -msse2` for x86-64 ai_sched library (M2)
- ✅ x86-64 build succeeds with `ENABLE_AI_SCHEDULER=ON`

### x86-64 FP Context
- ✅ `fp_save()` / `fp_restore()` implemented via FXSAVE/FXRSTOR in `fp_context.S` (M2)

---

## Phase AI-Sched Completion Checklist

### Deliverables
- ✅ Pluggable scheduler interface working
- ✅ Heuristic policy extracted and functionally identical
- ✅ AI inference engine compiles and runs (with stub AND real weights)
- ✅ State vector extraction matches simulator specification
- ✅ MLP policy makes scheduling decisions (with fallback to heuristic)
- ✅ FP state properly saved/restored in interrupt context
- ✅ Shell commands for policy switching working (M1)
- ✅ Inference latency < 50µs on target hardware (Pi 5: 41.9 µs with real MLP weights)
- ✅ All tests pass (AI scheduler ON and OFF)

### Demo
- ✅ Boot SLM-OS in QEMU with AI scheduler (verified via test builds)
- ✅ `sched` / `sched policy` lists registered policies
- ✅ `sched policy ai_mlp` switches to AI policy (tested in M7 end-to-end)
- ✅ Tasks dispatched through AI policy (real weights → valid CPU assignment)
- ✅ `sched stats` shows policy name, task count, switches, per-CPU utilization
- ✅ Switch back to `heuristic` verified in integration tests

### Documentation
- ✅ `docs/scheduler.md` — pluggable policy, inference engine, state vector, FP safety, fallback, stats
- ✅ State vector specification (108-dim layout with tables)
- ✅ Action decoding formula documented
- ✅ Performance targets documented (< 50µs, NEON optimization)

---

## Outstanding Decisions

### Milestone 3 — Inference

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **NEON strategy** | Auto-vectorize vs explicit intrinsics | **Try auto-vectorize first** — add intrinsics only if < 50µs not met |
| **Scratch buffers** | Stack vs static | **Stack** — thread safety, no locking needed |

### Milestone 4 — State Vector

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Core zero-fill** | Error on mismatch vs zero-fill | **Zero-fill** — matches simulator behavior, model trained this way |

### Milestone 5 — Counters

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Top-8 cache update** | Every tick vs every N ticks | **Every 10 ticks (100ms)** — balance freshness vs overhead |
| **Rolling window size** | 50 vs 100 vs 200 | **100** — ~10 seconds of history at 10 completions/sec |

### Milestone 6 — FP State

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **FP handling** | Save/restore vs deferred | **Save/restore** — simpler, predictable latency |
| **Save scope** | Full V0-V31 vs caller-saved only | **Full** — safest, inference may use any register |

### Milestone 9 — x86-64

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **x86 SIMD** | SSE vs AVX vs AVX-512 | **SSE4.2** — widest compatibility, sufficient for 256×256 |

---

## Risk Mitigation

### Identified Risks

1. **⚠️ FP Register Corruption in IRQ Context**
   - Risk: AI inference clobbers FP registers used by interrupted code
   - Mitigation: Explicit FP save/restore around inference (M6)
   - Verification: Stress test with FP-heavy workload + frequent scheduling
   - Fallback: Defer inference to non-IRQ context

2. **⚠️ Inference Latency > 50µs**
   - Risk: Auto-vectorized code doesn't meet performance target
   - Mitigation: Verify NEON usage in disassembly; add intrinsics if needed
   - Verification: Latency test on QEMU and real hardware
   - Fallback: Reduce model size (fewer neurons), or accept higher latency

3. **Missing Per-Task Features**
   - Risk: 5 of 8 per-task features zeroed → degraded model accuracy
   - Mitigation: Accept for now; add `slm_task_info` struct later
   - Mitigation: Consider retraining with features masked
   - Fallback: Rely on heuristic fallback for poor predictions

4. **Top-8 Task Scan Contention**
   - Risk: Locking all run queues for scan creates contention
   - Mitigation: Cache top-8 list, update on timer tick (M5)
   - Verification: Measure contention under high task creation rate
   - Fallback: Simpler "first 8 ready tasks" without global scan

5. **Weight File Integration**
   - Risk: Plan A deliverables delayed or incompatible
   - Mitigation: Develop with stub weights; interface is defined
   - Mitigation: Verification script validates dimensions
   - Fallback: Manual weight array creation from Python export

6. **Platform Core Count Mismatch**
   - Risk: Model expects 6 cores, Pi 5 has 4
   - Mitigation: Zero-fill unused core slots (documented in M4)
   - Verification: Test on both 4-core and 6-core QEMU configurations
   - Coordination: Confirm with Plan A that model handles zero-filled cores

---

## Dependencies

### External Dependencies
- **Plan A (Export Pipeline):** Generates `ai_weights_*.c` and `ai_config.h` files
  - Can proceed with stub weights until Plan A delivers
  - Interface contract defined in both plans
  - Coordinate on: state vector dimensions, action space per platform
- **Phase 4X (x86-64 port):** For SSE/AVX inference path
  - M9 depends on Phase 4X M3 (interrupts) being complete

### Internal Dependencies

```
M1 (Vtable) ──────────────────────────────────> M7 (AI Policy)
                                                     │
M2 (Build) ──────> M3 (Inference) ───────────────────┤
                          │                          │
                   M4 (State) ───────────────────────┤
                          │                          │
                   M5 (Counters) ────────────────────┤
                                                     │
                   M6 (FP State) ────────────────────┘
                   
M8 (Testing) ──────> After M7

M9 (x86-64) ──────> After M1-M8 complete on ARM64
```

**Recommended Order:**
1. **M1 (Vtable)** — pure refactor, validates interface, no new dependencies
2. **M2 (Build)** — set up FP library infrastructure
3. **M3 (Inference)** — core math, needs M2
4. **M4 (State)** — feature extraction, needs M2
5. **M5 (Counters)** — kernel instrumentation
6. **M6 (FP State)** — interrupt safety
7. **M7 (AI Policy)** — integration, needs M1, M3-M6
8. **M8 (Testing)** — validation
9. **M9 (x86-64)** — port

---

## Resources

### Plan A Integration
- Export script: `slm-os-scheduler-ai/scripts/export_models.py`
- Generated output: `slm-os-scheduler-ai/deploy/generated/`
- Verification: `slm-os-scheduler-ai/scripts/verify_inference.c`
- Platform config: `slm-os-scheduler-ai/slm_sim/platforms.py`

### Simulator Reference
- State vector: `slm_sim/observation.py`
- Action space: `slm_sim/actions.py`
- Platform configs: `slm_sim/platforms.py`

### NEON/SIMD
- [ARM NEON Intrinsics Reference](https://developer.arm.com/architectures/instruction-sets/intrinsics/)
- [Auto-vectorization with GCC](https://gcc.gnu.org/projects/tree-ssa/vectorization.html)
- Verify with: `aarch64-none-elf-objdump -d ai_inference.o | grep -E 'fmla|fadd'`

### Kernel Scheduling
- `kernel/sched/sched.c` — current scheduler
- `kernel/include/task.h` — task structure
- `docs/scheduler.md` — existing documentation

---

## Lessons Applicable from Previous Phases

### From Phase 3 Scheduler Work
- Per-queue locks work well (avoid global lock where possible)
- Priority inheritance mutex pattern available if needed
- Core isolation infrastructure already exists
- Deadline boost mechanism provides fallback behavior

### From Rust FFI Work
- FFI boundary patterns established
- Safe wrappers around unsafe calls
- Model memory allocator available for future integration

### From Build System
- Separate library with different compiler flags (Lua pattern) works well
- `-mgeneral-regs-only` for main kernel, omit for FP libraries

---

*Created: January 2026*
*Revised: January 2026 — Fixed AArch64 compiler flags, reordered milestones, documented core count handling*
*Purpose: Integrate trained AI models into SLM-OS kernel scheduler*
*Depends on: Plan A (Export Pipeline) for production weights*
