# Phase AI-Sched: AI Scheduler Kernel Integration

This document tracks the integration of trained AI models (MLP, PPO, XGBoost) into the SLM-OS kernel scheduler.

**Status:** Not Started

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
| ⚠️ | Risk item requiring attention |

---

## Milestone 1: Pluggable Scheduler Interface

### Scheduler Policy Vtable
- ☐ Create `kernel/include/sched_policy.h` with vtable definition:
  ```c
  struct sched_policy_ops {
      const char *name;
      int      (*init)(void);
      void     (*shutdown)(void);
      uint32_t (*assign_cpu)(struct task *task);
      void     (*tick)(uint32_t cpu);  // may be NULL
  };
  ```
- ☐ Document interface contract in header comments
- ☐ Define `SCHED_POLICY_MAX_NAME` (16 chars)

### Extract Heuristic Policy
- ☐ Create `kernel/sched/sched_heuristic.c`
- ☐ Move `find_target_cpu()` logic from `sched.c` (~lines 487-551) to new file
- ☐ Move `find_performance_cpu()` logic
- ☐ Implement `heuristic_assign_cpu()` wrapping existing logic
- ☐ Create `sched_policy_heuristic` ops struct
- ☐ Verify extracted code compiles standalone

### Wire Vtable into sched.c
- ☐ Add `static const struct sched_policy_ops *active_policy`
- ☐ Initialize to `&sched_policy_heuristic`
- ☐ Replace inline CPU selection in `scheduler_add_task()` with `active_policy->assign_cpu(task)`
- ☐ Add `active_policy->tick(cpu_id)` call in `scheduler_tick()` (if non-NULL)
- ☐ Implement `sched_set_policy(const struct sched_policy_ops *policy)`:
  - ☐ Disable interrupts
  - ☐ Swap pointer
  - ☐ Re-enable interrupts
  - ☐ Log policy change
- ☐ Implement `sched_get_policy()` — returns current policy name
- ☐ Guard AI-specific code with `#ifdef CONFIG_AI_SCHEDULER`

### Shell Command
- ☐ Add `sched` command to shell:
  - `sched` — show current policy name
  - `sched policy` — list available policies
  - `sched policy <name>` — switch to named policy
- ☐ Implement policy registry (simple array of `sched_policy_ops*`)
- ☐ `sched_register_policy()` for dynamic registration

### Verification
- ☐ All existing scheduler tests pass unchanged
- ☐ Heuristic policy produces identical CPU assignments to old inline code
- ☐ `sched_set_policy()` swaps cleanly under load
- ☐ Shell command works correctly

---

## Milestone 2: AI Inference Engine

### Directory Structure
- ☐ Create `kernel/sched/ai/` directory
- ☐ Create `kernel/sched/ai/ai_inference.h` — public API
- ☐ Create `kernel/sched/ai/ai_inference.c` — implementation
- ☐ Create `kernel/sched/ai/ai_types.h` — shared types

### Core Math Functions
- ☐ Implement `ai_matvec()`:
  ```c
  static void ai_matvec(const float *W, const float *bias,
                        const float *in, float *out, int M, int N);
  ```
  - ☐ Row-major W[M×N] × in[N] + bias[M] → out[M]
  - ☐ Cache-friendly inner loop (stride-1 access on W)
  - ☐ No dynamic allocation
- ☐ Implement `ai_relu()`:
  ```c
  static void ai_relu(float *x, int n);
  ```
  - ☐ In-place: `if (x[i] < 0) x[i] = 0`
- ☐ Implement `ai_argmax()`:
  ```c
  static int ai_argmax(const float *x, int n);
  ```
  - ☐ Linear scan for max logit index

### ⚠️ NEON Optimization
- ☐ Verify `-mcpu=cortex-a78ae` enables NEON auto-vectorization
- ☐ If not, add explicit NEON intrinsics to `ai_matvec()`:
  ```c
  #include <arm_neon.h>
  float32x4_t acc = vdupq_n_f32(0.0f);
  // vld1q_f32, vfmaq_f32 for 4-wide accumulation
  ```
- ☐ Benchmark scalar vs NEON: target < 50µs for full inference
- ☐ Add compile-time check for NEON availability

### Action Decoding
- ☐ Define `struct ai_sched_action`:
  ```c
  struct ai_sched_action {
      uint8_t core_assignment;  // 0 to num_cores-1, or GPU
      uint8_t priority_adj;     // 0=none, 1=boost, 2=reduce
      uint8_t preempt;          // 0=no, 1=yes
  };
  ```
- ☐ Implement `ai_decode_action()`:
  ```c
  static inline void ai_decode_action(int idx, struct ai_sched_action *out) {
      out->preempt = idx % 2; idx /= 2;
      out->priority_adj = idx % 3; idx /= 3;
      out->core_assignment = idx;
  }
  ```
- ☐ Implement `ai_validate_action()` — bounds checking

### MLP Inference
- ☐ Implement `ai_schedule_mlp()`:
  ```c
  int ai_schedule_mlp(const float state[AI_STATE_DIM],
                      struct ai_sched_action *action);
  ```
  - ☐ 4-layer forward pass: (108→256→256→128→42)
  - ☐ Stack-allocated scratch buffers (256 floats = 1KB)
  - ☐ Thread-safe (no static globals)
  - ☐ Returns 0 on success, -1 on error

### PPO Inference
- ☐ Implement `ai_schedule_ppo()`:
  ```c
  int ai_schedule_ppo(const float state[AI_STATE_DIM],
                      struct ai_sched_action *action);
  ```
  - ☐ Same architecture as MLP (108→256→256→128→42)
  - ☐ Uses different weight arrays

### XGBoost Inference (Stretch)
- ⏸️ Implement tree traversal function
- ⏸️ Implement derived feature computation (5 formulas)
- ⏸️ Implement cascaded classification (core → priority → preempt)
- ⏸️ Size check: if > 1MB total, defer to later

---

## Milestone 3: State Vector Extraction

### State Dimensions
- ☐ Define constants in `ai_types.h`:
  ```c
  #define AI_STATE_DIM        108
  #define AI_NUM_CORES        6
  #define AI_FEATURES_PER_CORE 6
  #define AI_NUM_TASKS        8
  #define AI_FEATURES_PER_TASK 8
  #define AI_GLOBAL_FEATURES  8
  ```

### Core State File
- ☐ Create `kernel/sched/ai/ai_state.h` — public API
- ☐ Create `kernel/sched/ai/ai_state.c` — implementation
- ☐ Implement `ai_extract_state()`:
  ```c
  void ai_extract_state(float state[AI_STATE_DIM]);
  ```

### Per-Core Features (36 floats)
For each core c (0 to 5), features at offset `c*6`:

| Offset | Feature | Source | Notes |
|--------|---------|--------|-------|
| c×6+0 | utilization | `running_ticks / total_ticks` | New counter (see M4) |
| c×6+1 | queue_depth | `cpu_rq(c)->ready_count / 32.0f` | Existing |
| c×6+2 | cache_pressure | `0.0f` | Future: PMU integration |
| c×6+3 | core_type | `1.0f` | Homogeneous on Jetson/Pi5 |
| c×6+4 | isolated | `(sched.isolated_cores >> c) & 1` | Existing |
| c×6+5 | current_task_prio | `current->effective_priority / 7.0f` | Existing |

- ☐ Implement per-core feature extraction
- ☐ Handle core offline/invalid states gracefully

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
| t×8+6 | wait_time | `(now - arrival_time_ns) / 1e9` | New field (see M4) |
| t×8+7 | component_type | `0.0f` | Future: slm_task_info |

- ☐ Implement top-8 task collection (see M4 for caching)
- ☐ Handle < 8 tasks gracefully (zero-fill remaining slots)
- ☐ Implement per-task feature extraction

### Global Features (8 floats)
Features at offset 100:

| Offset | Feature | Source | Notes |
|--------|---------|--------|-------|
| 100 | ready_count | `sum(ready_count) / 64.0f` | Existing |
| 101 | deadline_miss_rate | Rolling window counter | New (see M4) |
| 102 | avg_latency | `cumulative_latency / completion_count / 1e7` | New (see M4) |
| 103 | weight_pool_pressure | `0.0f` or FFI query | Future: Rust runtime |
| 104 | workspace_pool_pressure | `0.0f` or FFI query | Future: Rust runtime |
| 105 | gpu_queue_depth | `gpu_pending_count() / 8.0f` | If GPU driver available |
| 106 | load_imbalance | `std_dev(utils) / mean(utils)` | Clamp [0,1] |
| 107 | episode_time | `0.0f` | Not applicable to real kernel |

- ☐ Implement global feature extraction
- ☐ Implement load imbalance calculation (std_dev / mean)

---

## Milestone 4: Scheduler Counters & Task Fields

### New Task Fields
- ☐ Add to `struct task` (guarded by `CONFIG_AI_SCHEDULER`):
  ```c
  #ifdef CONFIG_AI_SCHEDULER
      uint64_t arrival_time_ns;      // Set in scheduler_add_task()
      uint64_t completion_time_ns;   // Set on task exit
  #endif
  ```
- ☐ Initialize `arrival_time_ns` in `scheduler_add_task()`
- ☐ Set `completion_time_ns` in task cleanup path

### Per-CPU Utilization Tracking
- ☐ Add to scheduler state (per-CPU):
  ```c
  struct cpu_runqueue {
      // ... existing fields ...
  #ifdef CONFIG_AI_SCHEDULER
      uint64_t running_ticks;   // Ticks where current != idle
      uint64_t total_ticks;     // Total ticks since boot
  #endif
  };
  ```
- ☐ Increment `running_ticks` in `scheduler_tick()` when current task is not idle
- ☐ Increment `total_ticks` always
- ☐ Utilization = `running_ticks / total_ticks`

### Deadline Miss Tracking
- ☐ Add deadline miss counter:
  ```c
  #ifdef CONFIG_AI_SCHEDULER
  static struct {
      uint32_t miss_count;
      uint32_t total_count;
      uint32_t window_misses[100];  // Rolling window
      uint32_t window_idx;
  } deadline_stats;
  #endif
  ```
- ☐ Update on task completion: check if `completion_time_ns > deadline_ns`
- ☐ Implement rolling window miss rate calculation

### Completion Latency Tracking
- ☐ Add latency accumulator:
  ```c
  #ifdef CONFIG_AI_SCHEDULER
  static struct {
      uint64_t cumulative_latency_ns;
      uint32_t completion_count;
  } latency_stats;
  #endif
  ```
- ☐ Update on task completion: `latency = completion_time_ns - arrival_time_ns`
- ☐ Implement average latency calculation

### Top-8 Task Cache
- ☐ Add cached task list:
  ```c
  #ifdef CONFIG_AI_SCHEDULER
  static struct {
      struct task *tasks[8];
      uint32_t count;
      uint64_t last_update_tick;
  } top_tasks_cache;
  #endif
  ```
- ☐ Update cache in `scheduler_tick()` (every N ticks, not every tick)
- ☐ Implement `ai_get_top_tasks()` that returns cached list
- ☐ Tune update frequency (every 10 ticks = 100ms seems reasonable)

---

## Milestone 5: AI Policy Implementation

### Policy File
- ☐ Create `kernel/sched/ai/sched_ai.c`
- ☐ Create `kernel/sched/ai/sched_ai.h`

### MLP Policy Ops
- ☐ Implement `ai_mlp_init()`:
  - ☐ Verify weights are loaded (not all zeros)
  - ☐ Run self-test inference
  - ☐ Return 0 on success
- ☐ Implement `ai_mlp_shutdown()`:
  - ☐ Log statistics (decisions made, fallbacks, avg latency)
- ☐ Implement `ai_mlp_assign_cpu()`:
  ```c
  uint32_t ai_mlp_assign_cpu(struct task *task) {
      float state[AI_STATE_DIM];
      struct ai_sched_action action;
      
      // FP state save (see M6)
      fp_save();
      
      ai_extract_state(state);
      
      if (ai_schedule_mlp(state, &action) < 0) {
          fp_restore();
          return heuristic_assign_cpu(task);  // Fallback
      }
      
      // Validate action
      if (action.core_assignment >= cpu_count() ||
          (sched_is_core_isolated(action.core_assignment) &&
           task->cpu_affinity == CPU_AFFINITY_ANY)) {
          fp_restore();
          return heuristic_assign_cpu(task);  // Fallback
      }
      
      // Apply priority adjustment
      if (action.priority_adj == 1) {
          task->effective_priority = min(task->effective_priority + 1, PRIORITY_CRITICAL);
      } else if (action.priority_adj == 2) {
          task->effective_priority = max(task->effective_priority - 1, PRIORITY_IDLE);
      }
      
      // Preempt flag affects pick_next_task() via priority
      if (action.preempt && task->effective_priority < PRIORITY_CRITICAL) {
          task->effective_priority++;
      }
      
      fp_restore();
      return action.core_assignment;
  }
  ```
- ☐ Implement `ai_mlp_tick()` (optional, for periodic rebalancing)

### PPO Policy Ops
- ☐ Implement `sched_policy_ai_ppo` struct (same pattern as MLP)
- ☐ Uses `ai_schedule_ppo()` instead of `ai_schedule_mlp()`

### XGBoost Policy Ops (Stretch)
- ⏸️ Implement `sched_policy_ai_xgboost` struct
- ⏸️ Uses cascaded tree inference

### Policy Registration
- ☐ Register policies in `sched_ai_init()`:
  ```c
  void sched_ai_init(void) {
      sched_register_policy(&sched_policy_ai_mlp);
      sched_register_policy(&sched_policy_ai_ppo);
  }
  ```
- ☐ Call from `kernel_main()` if `CONFIG_AI_SCHEDULER` defined

### Statistics
- ☐ Track per-policy stats:
  - ☐ Total decisions made
  - ☐ Fallback count
  - ☐ Average inference latency
  - ☐ Action distribution histogram
- ☐ Add `sched stats` shell command to display

---

## Milestone 6: FP State Handling

### ⚠️ Problem Analysis
- ☐ Document all paths where `ai_mlp_assign_cpu()` can be called
- ☐ Identify which paths are interrupt context:
  - `scheduler_add_task()` from IRQ (e.g., timer waking sleeping task)
  - `scheduler_add_task()` from syscall (non-IRQ)
- ☐ Verify current FP save/restore only happens on context switch

### FP Save/Restore Macros
- ☐ Create `kernel/include/fp_context.h`:
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
- ☐ Implement `fp_save()` in assembly (stp q0-q31, mrs fpcr/fpsr)
- ☐ Implement `fp_restore()` in assembly (ldp q0-q31, msr fpcr/fpsr)

### Integration
- ☐ Wrap AI inference calls with FP save/restore
- ☐ Measure overhead of FP save/restore (~50-100 cycles expected)
- ☐ Verify no FP register corruption under stress test

### Alternative: Deferred Inference
- ⏸️ If FP save/restore overhead is too high:
  - Queue scheduling decisions for deferred processing
  - Process queue in non-IRQ context
  - More complex but avoids FP save on every decision

---

## Milestone 7: Build System Integration

### CMake Configuration
- ☐ Add to `CMakeLists.txt`:
  ```cmake
  option(ENABLE_AI_SCHEDULER "Include AI scheduling models" OFF)
  
  if(ENABLE_AI_SCHEDULER)
      add_compile_definitions(CONFIG_AI_SCHEDULER=1)
      
      # AI inference library - needs FP/NEON
      add_library(ai_sched STATIC
          kernel/sched/ai/ai_inference.c
          kernel/sched/ai/ai_state.c
          kernel/sched/ai/sched_ai.c
          kernel/sched/ai/fp_context.S
          kernel/sched/ai/ai_weights_mlp.c
      )
      
      # Explicit FP/NEON flags - NOT -mgeneral-regs-only
      target_compile_options(ai_sched PRIVATE
          -ffreestanding
          -nostdlib
          -mcpu=cortex-a78ae
          -mfpu=neon-fp-armv8
          -mfloat-abi=hard
          -fPIE
          -O2
      )
      
      target_include_directories(ai_sched PRIVATE
          ${KERNEL_INCLUDES}
          kernel/sched/ai
      )
      
      target_link_libraries(slmos.elf PRIVATE ai_sched)
  endif()
  ```

### Stub Weights for Development
- ☐ Create `kernel/sched/ai/ai_weights_stub.c`:
  - All-zero weight arrays matching extern declarations
  - Allows compilation without Plan A deliverables
- ☐ Create `kernel/sched/ai/ai_weights_stub.h`:
  - Extern declarations for weight arrays
  - Dimension constants

### Weight File Integration
- ☐ Create `scripts/import_ai_weights.sh`:
  - Copies generated weights from Plan A output directory
  - Updates include paths
  - Validates dimensions match
- ☐ Add Makefile target: `make import-ai-weights`

### Build Verification
- ☐ Verify build with `ENABLE_AI_SCHEDULER=OFF` (existing behavior)
- ☐ Verify build with `ENABLE_AI_SCHEDULER=ON` + stub weights
- ☐ Verify build with `ENABLE_AI_SCHEDULER=ON` + real weights (after Plan A)

---

## Milestone 8: Testing

### Unit Tests

#### Inference Tests
- ☐ `test_ai_matvec_basic` — small known matrix, verify output
- ☐ `test_ai_matvec_large` — 256×256 matrix, verify dimensions
- ☐ `test_ai_relu_positive` — positive values unchanged
- ☐ `test_ai_relu_negative` — negative values zeroed
- ☐ `test_ai_argmax` — verify max index found correctly
- ☐ `test_ai_decode_action` — verify encoding/decoding roundtrip
- ☐ `test_ai_schedule_mlp_valid` — returns valid action struct
- ☐ `test_ai_schedule_mlp_bounds` — action values in valid ranges

#### State Extraction Tests
- ☐ `test_ai_extract_state_dimensions` — output is 108 floats
- ☐ `test_ai_extract_state_normalized` — values in expected ranges [0,1] or similar
- ☐ `test_ai_extract_state_cores` — per-core features match kernel state
- ☐ `test_ai_extract_state_tasks` — per-task features match known task values

#### Policy Tests
- ☐ `test_sched_policy_switch` — switch from heuristic to AI and back
- ☐ `test_ai_policy_fallback` — invalid action triggers fallback
- ☐ `test_ai_policy_respects_isolation` — isolated cores avoided

### Performance Tests
- ☐ `test_ai_inference_latency`:
  - Measure `slm_get_time_ns()` before/after `ai_schedule_mlp()`
  - Run 1000 iterations
  - Report min/max/avg
  - **Assert: avg < 50µs**
- ☐ `test_ai_state_extraction_latency`:
  - Measure `ai_extract_state()` time
  - Report min/max/avg
- ☐ `test_fp_save_restore_latency`:
  - Measure FP context save/restore overhead

### Integration Tests
- ☐ `test_ai_scheduler_stress`:
  - Create 20 tasks with varying priorities/deadlines
  - Switch to AI policy
  - Run for 10 seconds
  - Verify all tasks complete
  - Verify no panics, no deadlocks
- ☐ `test_ai_scheduler_mixed_policy`:
  - Switch between heuristic and AI policies under load
  - Verify smooth transitions

### QEMU Validation
- ☐ Boot with AI scheduler enabled
- ☐ Use shell to switch policies
- ☐ Run standard workload, compare behavior
- ☐ Verify `sched stats` shows reasonable numbers

---

## Milestone 9: x86-64 Support (Phase 4X Integration)

### SSE/AVX Implementation
- ☐ Create `kernel/sched/ai/ai_inference_x86.c`:
  - SSE intrinsics version of `ai_matvec()`
  - `_mm_load_ps`, `_mm_fmadd_ps` for 4-wide
- ☐ Add `#ifdef __x86_64__` / `#ifdef __aarch64__` guards
- ☐ Verify performance target on x86-64 (< 50µs)

### Build System
- ☐ Update CMake for x86-64 AI scheduler build
- ☐ Add appropriate SSE/AVX flags

### x86-64 FP Context
- ☐ Implement `fp_save()` / `fp_restore()` for x86-64 (FXSAVE/FXRSTOR)

---

## Phase AI-Sched Completion Checklist

### Deliverables
- ☐ Pluggable scheduler interface working
- ☐ Heuristic policy extracted and functionally identical
- ☐ AI inference engine compiles and runs (with stub weights)
- ☐ State vector extraction matches simulator specification
- ☐ MLP policy makes scheduling decisions
- ☐ FP state properly saved/restored in interrupt context
- ☐ Shell commands for policy switching working
- ☐ Inference latency < 50µs on target hardware
- ☐ All tests pass

### Demo
- ☐ Boot SLM-OS in QEMU
- ☐ Show `sched` command listing policies
- ☐ Switch to `ai_mlp` policy via shell
- ☐ Show tasks being scheduled by AI
- ☐ Show `sched stats` with inference statistics
- ☐ Switch back to `heuristic`, verify smooth transition

### Documentation
- ☐ Update `docs/scheduler.md` with AI scheduler section
- ☐ Document state vector specification
- ☐ Document action decoding
- ☐ Document performance requirements and measurements

---

## Outstanding Decisions

### Milestone 2 — Inference

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **NEON strategy** | Auto-vectorize vs explicit intrinsics | **Try auto-vectorize first** — add intrinsics only if needed |
| **Scratch buffers** | Stack vs static | **Stack** — thread safety, no locking needed |

### Milestone 4 — Counters

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
| **x86 SIMD** | SSE vs AVX vs AVX-512 | **SSE** — widest compatibility, sufficient for 256×256 |

---

## Risk Mitigation

### Identified Risks

1. **⚠️ FP Register Corruption in IRQ Context**
   - Risk: AI inference clobbers FP registers used by interrupted code
   - Mitigation: Explicit FP save/restore around inference
   - Verification: Stress test with FP-heavy workload + frequent scheduling
   - Fallback: Defer inference to non-IRQ context

2. **⚠️ Inference Latency > 50µs**
   - Risk: Scalar FP code is too slow (~87µs estimated)
   - Mitigation: Verify NEON auto-vectorization; add intrinsics if needed
   - Verification: Latency test on QEMU and real hardware
   - Fallback: Reduce model size (fewer neurons), or accept higher latency

3. **Missing Per-Task Features**
   - Risk: 5 of 8 per-task features zeroed → degraded model accuracy
   - Mitigation: Accept for now; add `slm_task_info` struct later
   - Mitigation: Consider retraining with features masked
   - Fallback: Rely on heuristic fallback for poor predictions

4. **Top-8 Task Scan Contention**
   - Risk: Locking all run queues for scan creates contention
   - Mitigation: Cache top-8 list, update on timer tick (not every decision)
   - Verification: Measure contention under high task creation rate
   - Fallback: Simpler "first 8 ready tasks" without global scan

5. **Weight File Integration**
   - Risk: Plan A deliverables delayed or incompatible
   - Mitigation: Develop with stub weights; interface is defined
   - Mitigation: Verification script validates dimensions
   - Fallback: Manual weight array creation from Python export

---

## Dependencies

### External Dependencies
- **Plan A (Export Pipeline):** Generates `ai_weights_*.c` files
  - Can proceed with stub weights until Plan A delivers
  - Interface contract defined in both plans
- **Phase 4X (x86-64 port):** For SSE/AVX inference path
  - M9 depends on Phase 4X M3 (interrupts) being complete

### Internal Dependencies

```
M1 (Vtable) ──────> M5 (AI Policy) ──────> M8 (Testing)
                          │
M2 (Inference) ───────────┤
                          │
M3 (State) ───────────────┤
                          │
M4 (Counters) ────────────┘
      │
M6 (FP State) ──────> M5 (AI Policy)

M7 (Build) ──────> Independent, start early

M9 (x86-64) ──────> After M1-M6 complete on ARM64
```

**Recommended Order:**
1. M7 (Build) — set up infrastructure
2. M1 (Vtable) — foundation
3. M2 (Inference) — core math
4. M3 (State) — feature extraction
5. M4 (Counters) — kernel instrumentation
6. M6 (FP State) — interrupt safety
7. M5 (AI Policy) — integration
8. M8 (Testing) — validation
9. M9 (x86-64) — port

---

## Resources

### Plan A Integration
- Export script: `slm-os-scheduler-ai/scripts/export_models.py`
- Generated output: `slm-os-scheduler-ai/deploy/generated/`
- Verification: `slm-os-scheduler-ai/scripts/verify_inference.c`

### Simulator Reference
- State vector: `slm_sim/observation.py`
- Action space: `slm_sim/actions.py`
- Platform configs: `slm_sim/platforms.py`

### NEON/SIMD
- [ARM NEON Intrinsics Reference](https://developer.arm.com/architectures/instruction-sets/intrinsics/)
- [Auto-vectorization with GCC](https://gcc.gnu.org/projects/tree-ssa/vectorization.html)

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

---

*Created: January 2026*
*Purpose: Integrate trained AI models into SLM-OS kernel scheduler*
*Depends on: Plan A (Export Pipeline) for production weights*
