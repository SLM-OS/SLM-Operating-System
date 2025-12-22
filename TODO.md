# Phase 3: AI Infrastructure (C kernel + Rust runtime)

This document tracks Phase 3 implementation of SLM-OS.

**Status:** In progress (Milestone 2 complete)

**Goals:**
- Model memory management (Rust)
- GPU initialization (Jetson)
- Deadline-aware scheduler (policy in Rust)
- Zero-copy buffer sharing
- Real hardware testing (Jetson Orin Nano)

**Carried from Phase 2:**
- Per-queue scheduler locks
- Load balancing
- Page fault handling
- Device tree parsing (stretch goal)

---

## Milestone 1: Model Memory Management

### Model Memory Architecture
- ✅ Design model memory region layout (see `docs/model-memory.md`)
- ✅ Define `ModelHandle` type in Rust (opaque handle with generation counter)
- ✅ Plan memory pools: weight pool (read-only) vs workspace pool (read-write)
- ✅ Document memory lifecycle: load → map → use → unmap → unload

### Model Memory Allocator (Rust)
- ✅ Implement `ModelAllocator` struct in `runtime/src/mm/model_mem.rs`
- ✅ Write `alloc_weights(size)` / `alloc_workspace(size)` — allocate 2MB-aligned blocks
- ✅ Write `free(handle)` — return block to pool (refcount-aware)
- ✅ Implement weight pool (16 MB, 8 blocks on QEMU)
- ✅ Implement workspace pool (8 MB, 4 blocks on QEMU)
- ✅ Add statistics: `weight_pool_stats()`, `workspace_pool_stats()`

### Zero-Copy Model Sharing
- ✅ Implement model reference counting (refcount in BlockMeta)
- ✅ Write `share(handle)` — increment refcount, return shared handle
- ✅ Write `unshare(handle)` — alias for free, decrements refcount
- ✅ Ensure model unload only happens when refcount reaches zero
- ✅ Test concurrent access via pool exhaust/recovery test

### GPU Memory Integration
- ✅ Implement `gpu_map(handle)` — returns physical address (stub)
- ✅ Implement `gpu_unmap(handle)` — placeholder for cache invalidate
- [ ] Handle cache coherency (flush before GPU access, invalidate after) — deferred to M3
- [ ] Use `SHM_GPU_ACCESSIBLE` flag from Phase 2 shared buffers — deferred to M3
- [ ] Test with placeholder GPU driver (actual GPU in Milestone 3) — deferred to M3

### Model Memory Testing
- ✅ Test allocation/deallocation cycles
- ✅ Test zero-copy sharing between tasks
- ✅ Verify memory statistics accuracy
- ✅ Stress test: pool exhaustion and recovery
- [ ] Test large model allocation (approach RAM limits) — deferred (needs more RAM)

---

## Milestone 2: Deadline-Aware Scheduler

### Scheduler Policy Design (Rust)
- ✅ Define priority levels for inference tasks (8 levels: 0-7, IDLE/LOW/NORMAL/HIGH/CRITICAL)
- ✅ Design deadline representation (`deadline_ns: u64` in task struct)
- ✅ Plan scheduling algorithm: Hybrid (fixed priority + deadline boost)
- ✅ Document interaction with C scheduler primitives (see `docs/scheduler.md`)

### Priority Infrastructure
- ✅ Switch FFI task API from pointer to PID-based (use existing `task->id`)
  - Changed `slm_task_create()` to return `uint32_t` task ID instead of `void*`
  - Updated Rust wrappers to use `TaskId` newtype
  - Enables safe validation via `task_get(id)` before use
- ✅ Add `priority` field to task structure (C side)
  - Added `priority`, `effective_priority` (uint8_t) and `deadline_ns` (uint64_t)
  - Added `task_create_with_priority()` function
  - Added getter/setter functions for priority and deadline
- ✅ Implement priority queue in scheduler (replace simple linked list)
  - Run queue now ordered by `effective_priority` (highest first)
  - FIFO within same priority level
- ✅ Add FFI function `slm_task_set_priority(task_id, priority)`
- ✅ Implement `slm_task_set_deadline(task_id, deadline_ns)`

### Deadline-Aware Scheduling
- ✅ Implement deadline boost in C scheduler
  - Tasks with deadlines get `effective_priority` boosted as deadline approaches
  - < 100ms: +1 boost
  - < 50ms: boost to HIGH (6)
  - < 10ms or missed: boost to CRITICAL (7)
- ✅ Implement `schedule_slm_task()` in `runtime/src/sched/deadline.rs`
  - Created `sched` module with `deadline.rs` submodule
  - Provides `schedule_slm_task()` for SLM inference scheduling hints
  - Exports `Priority`, `CoreType`, `SchedulingHint`, `TaskDeadline`, `SlmTaskInfo`
- ✅ Urgent task detection (deadline < threshold → performance core)
  - Tasks with deadline < 50ms get `CoreType::Performance` hint
  - Tasks with deadline < 10ms get pinned to performance core
- ✅ Working set size heuristics (small model → efficiency core OK)
  - Models with working set < 8MB can use `CoreType::Efficiency`
  - Larger models get `CoreType::Performance`
- ✅ Implement `suggest_core_affinity()` (returns CoreType enum)
  - Actual core assignment (`assign_to_*_core()`) deferred to Phase 4 (big.LITTLE)
- ✅ Load balancing across cores based on deadline pressure
  - `calculate_deadline_pressure()` scores CPUs based on deadline-constrained tasks
  - `find_target_cpu()` uses combined metric: ready_count + deadline_pressure
  - Spreads deadline tasks across cores to reduce contention

### Per-Queue Scheduler Locks (Deferred from Phase 2)
- ✅ Replace global `sched.lock` with per-CPU run queue locks
  - Each `cpu_runqueue` now has its own `spinlock_t lock`
  - Single-queue operations use per-queue lock
  - Cross-queue operations (migration) lock both queues in CPU ID order to prevent deadlock
  - Global statistics are racy but acceptable for diagnostics
- Deferred: Implement lock-free task stealing between queues (optional)
- ✅ Reduce contention for cross-core operations
- ✅ Benchmark queue operations
  - `test_benchmark_queue_operations` measures add/remove time
  - Results logged during test run

### Priority Inversion Prevention
- ✅ Implement priority inheritance mutex (`pi_mutex.h`, `pi_mutex.c`)
  - Priority-inheriting mutex that boosts lock holder's priority when high-pri waits on low-pri
  - Tracks original priority and restores on unlock
  - Logs all priority inversions with INFO message
- ✅ Detect and log priority inversion events
  - `pi_mutex_inversion_count()` returns total inversions detected
  - Each boost logs: "PI: Boosting task 'X' (pri N->M) for waiter 'Y' (pri M)"
- ✅ Test with high-priority task waiting on low-priority lock holder
  - `test_priority_inheritance_basic` in `test_pi_mutex.c`
  - All 7 PI mutex tests pass

### Real-Time Core Pinning
- ✅ Use existing `cpu_affinity` field for deadline-critical tasks
  - Added `sched_set_task_affinity(task, cpu)` function
  - Tasks can be pinned to specific CPU or use `CPU_AFFINITY_ANY`
- ✅ Implement `sched_isolate_core(cpu_id)` — exclude core from general scheduling
  - `sched_isolate_core()`, `sched_unisolate_core()`, `sched_is_core_isolated()`
  - `sched_get_isolated_cores()` returns bitmask of isolated CPUs
  - CPU 0 cannot be isolated (boot CPU)
- ✅ Update `scheduler_add_task()` to skip isolated cores for `CPU_AFFINITY_ANY` tasks
  - New `find_target_cpu()` function picks least-loaded non-isolated CPU
  - Pinned tasks still run on isolated cores
- ✅ Policy: tasks with `deadline_ns > 0` get pinned to performance core
  - `find_performance_cpu()` prefers CPU 1+ (non-boot CPUs) for deadline tasks
  - Keeps CPU 0 available for system tasks, reduces interference
- ✅ Route timer IRQ only; keep other IRQs off isolated cores (GIC affinity)
  - `gic_set_affinity()`, `gic_get_affinity()` for SPI target control
  - `gic_exclude_cpu_from_spis()` routes SPIs away from isolated core
  - `gic_include_cpu_in_spis()` restores SPI routing
  - Timer IRQs (PPIs) unaffected — each CPU keeps its own timer for preemption
- ✅ Test: pinned inference task shows consistent latency vs unpinned
  - `test_isolated_core_latency` compares wake-to-run latency
  - Measures 10 samples on isolated and non-isolated cores
  - Logs average, range, min/max for both configurations

### Scheduler Testing
- ✅ Test priority ordering (high priority runs first)
  - `test_high_priority_runs_first`, `test_priority_ordering_multiple_levels`
- ✅ Test deadline-aware scheduling (urgent tasks preempt)
  - `test_deadline_boost_affects_order` and deadline boost unit tests
- ✅ Test core isolation and affinity
  - `test_isolate_core_marks_isolated`, `test_cannot_isolate_cpu0`, `test_isolate_invalid_cpu`
  - `test_affinity_any_avoids_isolated`, `test_pinned_task_runs_on_isolated`
  - `test_set_affinity_updates_field`, `test_set_affinity_invalid_cpu`, `test_multiple_cores_isolated`
- ✅ Verify no starvation of low-priority tasks
  - `test_no_starvation` — HIGH and LOW priority tasks both complete all iterations
- ✅ Stress test with mixed priorities and deadlines
  - `test_stress_mixed_priorities` — 5 tasks with IDLE/LOW/NORMAL/HIGH/CRITICAL priorities
  - One LOW task has urgent deadline (5ms) and gets boosted to CRITICAL
  - Verifies exact execution order matches priority ranking
- ✅ Benchmark scheduling overhead
  - `test_benchmark_context_switch` measures 100 context switches between two tasks
  - `test_benchmark_queue_operations` measures add/remove latency
  - Results logged during test run with assessment (Excellent/Good/Acceptable)

---

## Milestone 3: GPU Initialization (Jetson)

### Jetson Hardware Research
- [ ] Study Jetson Orin Nano GPU architecture (Ampere, 1024 cores)
- [ ] Document MMIO register map for GPU control
- [ ] Review NVIDIA open-source kernel driver for reference
- [ ] Identify minimal initialization sequence
- [ ] Plan DMA buffer allocation for GPU data transfer

### Platform Abstraction
- [ ] Create `kernel/gpu/` directory structure
- [ ] Define GPU driver interface (`struct gpu_driver`)
- [ ] Implement QEMU stub driver (no-op, for testing without GPU)
- [ ] Add platform detection to select correct driver

### Jetson GPU Driver (C)
- [ ] Write `jetson_gpu_init()` — power on, clock enable, reset sequence
- [ ] Write `jetson_gpu_alloc(size)` — allocate GPU-accessible memory
- [ ] Write `jetson_gpu_free(addr)` — free GPU memory
- [ ] Write `jetson_gpu_submit(cmd_buffer)` — submit work to GPU
- [ ] Write `jetson_gpu_wait()` — wait for GPU completion
- [ ] Implement basic fence/sync mechanism

### Memory Coherency
- [ ] Implement cache flush before GPU access (`dc civac`)
- [ ] Implement cache invalidate after GPU write (`dc ivac`)
- [ ] Test with simple GPU memory copy operation
- [ ] Document coherency requirements in `docs/gpu.md`

### GPU Testing
- [ ] Test GPU initialization on real Jetson hardware
- [ ] Test memory allocation and mapping
- [ ] Test simple compute operation (if possible without CUDA)
- [ ] Verify CPU can read GPU-written data correctly
- [ ] Fallback: Defer actual GPU compute to Phase 5 (SLM Integration)

---

## Milestone 4: Real Hardware Bring-Up

### Jetson Orin Nano Preparation
- [ ] Set up SD card with bootable image
- [ ] Configure U-Boot to chainload SLM-OS
- [ ] Set up serial console for debug output
- [ ] Document boot process in `docs/jetson-boot.md`

### Platform-Specific Drivers
- [ ] Implement Tegra UART driver (NS16550-compatible)
- [ ] Verify GIC configuration for Jetson (may differ from QEMU)
- [ ] Implement Jetson-specific timer if needed
- [ ] Test GPIO driver on real pins (LED blink test)

### Hardware Differences
- [ ] Document all QEMU vs Jetson differences discovered
- [ ] Update `platform.h` with Jetson-specific addresses
- [ ] Test MMU with Jetson's actual memory map
- [ ] Verify interrupt handling on real hardware
- [ ] Test multi-core boot on Jetson (6 cores vs QEMU's 4)

### Device Tree Support (Stretch Goal)
- [ ] Implement minimal DTB parser
- [ ] Extract memory regions from device tree
- [ ] Extract interrupt configuration from device tree
- [ ] Remove hardcoded addresses where possible

### Hardware Testing
- [ ] Full test suite passes on Jetson
- [ ] Multi-core stress test on real hardware
- [ ] IPC stress test on real hardware
- [ ] Measure actual context switch time (< 10 µs target)
- [ ] Measure actual interrupt latency

---

## Milestone 5: Debug Shell & ELF Execution

### MicroShell Integration
- [ ] Clone MicroShell into `kernel/lib/microshell/` (or as git submodule)
- [ ] Create UART I/O interface (`shell_io.c`) — `read`/`write` callbacks
- [ ] Initialize shell in `kernel_main()` after scheduler starts
- [ ] Run `ush_service()` from a dedicated shell task
- [ ] Verify basic prompt and echo working in QEMU

### Built-in Commands (System Inspection)
- [ ] `help` — list available commands (MicroShell builtin)
- [ ] `mem` — PMM statistics (total, free, allocated pages)
- [ ] `vmm` — virtual memory regions and flags
- [ ] `tasks` — list tasks (PID, state, CPU, name, priority)
- [ ] `cpu` — per-core status (frequency, load, current task)
- [ ] `ipc` — message queue and shared buffer stats
- [ ] `model` — model memory pool status (if M1 complete)
- [ ] `reboot` — system restart

### Virtual Filesystem Structure
- [ ] Mount root `/` with command nodes
- [ ] Mount `/sys/` for system info (read-only virtual files)
- [ ] Mount `/proc/` for per-task info (stretch goal)
- [ ] Design `/components/` mount point for Phase 4

### Basic ELF Loader
- [ ] Implement minimal ELF64 parser (`elf_loader.c`)
    - Parse ELF header, program headers
    - Support `PT_LOAD` segments only
    - Validate for ARM64 architecture
- [ ] `elf_load(buffer, size)` — load ELF from memory buffer
    - Allocate pages for code/data segments
    - Map into address space (component region)
    - Return entry point address
- [ ] Create task from ELF entry point
- [ ] Test with minimal "hello world" ELF (prints to UART and exits)

### Shell ELF Execution Command
- [ ] `run <name>` — load and execute ELF from built-in table (initially)
- [ ] Pass argc/argv to loaded program (simple stack setup)
- [ ] Handle task exit and cleanup
- [ ] `kill <pid>` — terminate running task

### Testing
- [ ] Shell responds to commands in QEMU
- [ ] All inspection commands show accurate data
- [ ] Load and run trivial ELF executable
- [ ] Task exits cleanly, memory reclaimed
- [ ] Test on Jetson if M4 complete

### Debug Monitor (Fallback if MicroShell Integration fails completely)
See `docs/shell.md` for design details.
- [ ] Implement `shell_init()` — spawn shell task on CPU 0
- [ ] Implement `shell_getline()` — read line from UART (blocking)
- [ ] Implement command dispatch (strcmp-based, no parsing)
- [ ] Commands: `help`, `mem`, `tasks`, `cpu`, `reboot`
- [ ] Optional: `gpio <n>` for LED toggle on real hardware

---

## Milestone 6: Deferred Items and Polish

### Page Fault Handling (Deferred from Phase 2)
- [ ] Implement basic page fault handler (panic with useful info)
- [ ] Log faulting address, access type, task ID
- [ ] Add fault address to panic register dump
- [ ] Future: demand paging for model memory (Phase 5+)

### IPC Improvements (Deferred from Phase 2)
- [ ] Implement proper timeout handling in `msg_recv()`
- [ ] Multi-task producer/consumer stress test
- [ ] Memory leak verification after IPC teardown
- [ ] Add IPC statistics (messages sent, queue high-water mark)

### Rust Runtime Expansion
- [ ] Implement `ModelLoader` struct skeleton (full implementation Phase 5)
- [ ] Implement `InferenceScheduler` struct skeleton
- [ ] Add logging infrastructure in Rust (via FFI to UART)
- [ ] Create `runtime/src/sched/heterogeneous.rs` for big.LITTLE awareness

### Documentation
- [ ] Update architecture doc with Phase 3 learnings
- [ ] Document GPU driver interface
- [ ] Document model memory API
- [ ] Update build instructions for Jetson target
- [ ] Create troubleshooting guide for hardware issues


---

## Phase 3 Completion Checklist

### Deliverables
- [ ] Model memory allocator functional (Rust)
- [ ] Deadline-aware scheduling working
- [ ] GPU initialized on Jetson (basic functionality)
- [ ] Kernel boots and runs on real Jetson hardware
- [ ] All Phase 2 tests still pass
- [ ] New tests for Phase 3 features
- [ ] Documentation updated

### Demo
- [ ] Boot on Jetson Orin Nano via serial console
- [ ] Show model memory allocation and sharing
- [ ] Show deadline-aware task scheduling
- [ ] Show GPU memory mapping (compute deferred to Phase 5)
- [ ] Compare performance: QEMU vs real hardware

---

## Outstanding Decisions

### Milestone 1 — Model Memory

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Pool Strategy** | Single pool vs separate weight/workspace | **Separate pools** | Weight pool (read-only, shareable) vs workspace pool (read-write, per-task). Enables zero-copy model sharing. |
| **Alignment** | 2MB (huge page) vs 4KB | **2MB blocks** | Reduces TLB pressure for large models. Simple block allocator without fragmentation concerns. |
| **Rust vs C** | All Rust vs Rust policy + C mechanism | **All Rust** | Model memory allocator entirely in Rust (`runtime/src/mm/`). FFI only for underlying page allocation from C kernel. |

### Milestone 2 — Scheduler

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Algorithm** | EDF vs fixed priority vs hybrid | **Hybrid** | Fixed priority base + deadline boost. Simpler than full EDF, sufficient for 4-6 cores |
| **Priority Levels** | 4 vs 8 vs 16 vs 32 | **8 levels** | Enough granularity without complexity. Maps to 3 bits |
| **Lock-Free Stealing** | Implement vs defer | **Defer** | Global lock is fine for now. Profile first to confirm contention is an issue |

### Milestone 3 — GPU

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **GPU Scope** | Init only vs basic compute vs CUDA | Init only is safest; CUDA requires proprietary libs | Before starting M3 |
| **Driver Model** | Kernel driver vs user-space | Kernel is simpler for bare-metal; user-space more modular | Before starting M3 |
| **Fallback** | CPU-only inference | Must work without GPU for QEMU testing | Before Phase 5 |

### Milestone 4 — Hardware

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Boot Method** | U-Boot vs UEFI direct | U-Boot is documented; UEFI may be cleaner | Before hardware bring-up |
| **Device Tree** | Full parsing vs minimal vs hardcoded | Full is flexible; hardcoded is faster to implement | Stretch goal, can defer |
| **Pi 5 Support** | Now vs later vs never | Different GPU, simpler platform; good fallback if Jetson stalls | Defer to Phase 4+ |

### Milestone 5 — Debug Shell

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Shell task priority** | High vs normal | **Normal** — shell shouldn't preempt real work |
| **ELF source** | Embedded in kernel vs filesystem vs network | **Embedded initially** — array of `{name, data, size}` |
| **Address space** | Shared with kernel vs isolated | **Shared** for Phase 3 — isolation in Phase 4+ |
| **Argument passing** | Stack-based vs register | **Stack** — matches ARM64 ABI for `main(argc, argv)` |


### Deferred to Phase 4

| Item | Notes |
|------|-------|
| Component hot-swap | Phase 4: "Hot-swap mechanism" |
| Component isolation | Phase 4: "Component isolation" |
| Message routing | Phase 4: "Message routing" |
| Full model loader | Phase 5: "ONNX model loader" |
| Inference engine | Phase 5: "TensorRT Lite integration" |

---

## Risk Mitigation

### Phase 2 Risks (Resolved) ✅

1. **MMU Enable Sequence** — ✅ Resolved
    - Risk: Many ways to triple-fault or hang silently
    - What worked: Identity mapping with shared L1 table for TTBR0/TTBR1, extensive debug output, 2MB block granularity for simplicity
    - Result: MMU enables cleanly, kernel runs at high addresses

2. **Multi-Core Synchronization** — ✅ Resolved
    - Risk: Race conditions are hard to reproduce and debug
    - What worked: Global scheduler lock (simple but sufficient for 4 cores), ARM exclusive load/store for atomics, comprehensive stress tests (6 tasks across 3 CPUs, lock contention test with 150 increments)
    - Result: No race conditions detected, all tests pass consistently

3. **Rust/C Linking** — ✅ Resolved
    - Risk: Symbol visibility, name mangling, ABI mismatches
    - What worked: `extern "C"` and `#[no_mangle]` on all FFI functions, `#[repr(C)]` on shared structs, compile-time size assertions, runtime FFI validation
    - Result: Rust runtime links and runs correctly, FFI tests pass

4. **Rust Borrow Checker vs Kernel Patterns** — ✅ Resolved (for Phase 2 scope)
    - Risk: Kernel data structures often have complex ownership
    - What worked: Keep Rust code simple for Phase 2, use `unsafe` only at FFI boundary, wrap unsafe in safe abstractions
    - Result: No borrow checker issues; pattern scales for Phase 3

### Phase 3 Risks

1. **GPU Integration**
    - Risk: NVIDIA's GPU is complex; documentation may be incomplete
    - Mitigation: Start with just initialization and memory mapping
    - Mitigation: Defer actual compute to Phase 5 with TensorRT
    - Fallback: CPU-only inference is viable for small models

2. **Real Hardware Bring-Up**
    - Risk: Silent failures, different behavior from QEMU
    - Mitigation: Have working serial console before anything else
    - Mitigation: Start with minimal boot, add features incrementally
    - Mitigation: Keep QEMU as primary development target

3. **Deadline Scheduler Correctness**
    - Risk: Priority inversion, missed deadlines, starvation
    - Mitigation: Start with simple fixed priorities
    - Mitigation: Add EDF only if fixed priority is insufficient
    - Fallback: Round-robin with affinity works (Phase 2 baseline)

4. **Model Memory Pressure**
    - Risk: Large models exceed available RAM
    - Mitigation: Start with small test models (< 100MB)
    - Mitigation: Implement memory statistics early for visibility
    - Fallback: Target 1-3B parameter models, not 7B+

5. **Rust in Performance-Critical Paths**
    - Risk: Abstraction overhead, borrow checker friction
    - Mitigation: Profile early, optimize hot paths
    - Mitigation: Keep FFI boundary thin
    - Fallback: Move hot paths to C if needed

6. **MicroShell ELF Load and Execute**
    - **MicroShell integration** — Low risk, well-documented library
    - **ELF loader** — Medium risk, but ELF64 is simpler than ELF32; ARM64 is straightforward
    - **Task creation from ELF** — Already have `task_create()`, just need entry point wiring

### Dependencies Between Milestones

```
M1 (Model Memory) ─────────> M3 (GPU) ──> GPU needs model memory for DMA buffers
                    │
                    └──────> M6 (Polish) ──> ModelLoader skeleton uses model memory

M2 (Scheduler) ────────────> Independent, can parallel with M1

M3 (GPU) ──────────────────> M4 (Hardware) ──> GPU driver only testable on Jetson

M4 (Hardware) ─────────────> Should start early for serial console setup

M5 (Shell) ────────────────> Useful for debugging, can start after M4
```

Recommended order:
1. M1 (Model Memory) — ✅ Complete
2. M2 (Scheduler) — In progress
3. M4 (Hardware) — Get serial working for debugging
4. M3 (GPU) — Requires M1 and M4
5. M5 (Shell) — Useful for hardware debugging
6. M6 (Polish) — Throughout

---

## Resources

### GPU
- NVIDIA Jetson Orin Nano Developer Guide
- NVIDIA Open GPU Kernel Modules (reference for register interface)
- Linux kernel `drivers/gpu/drm/nouveau/` (open-source NVIDIA driver)

### Scheduling
- "Deadline Scheduling in Linux" (LWN.net articles)
- "Operating Systems: Three Easy Pieces" — Chapter on MLFQ and scheduling
- RTEMS documentation (real-time scheduling in embedded OS)

### Model Memory
- "Memory Management for Machine Learning Inference" (various papers)
- TensorFlow Lite memory allocation strategy
- ONNX Runtime memory arena design

### Jetson Hardware
- Jetson Orin Nano Developer Kit User Guide
- Tegra234 Technical Reference Manual (if available)
- NVIDIA L4T (Linux for Tegra) source code for driver reference

---

## Lessons from Phase 1 & 2

### What Worked Well
- Starting with QEMU before real hardware
- Global scheduler lock (simplicity over optimization)
- Eager FPU save (predictable, no lazy-save bugs)
- Rust FFI with safe wrappers around unsafe calls
- Compile-time assertions for struct sizes

### What To Do Differently
- Set up serial console on real hardware earlier
- Profile before optimizing (per-queue locks may not be needed)
- Document all QEMU assumptions in one place
- Add more stress tests earlier in development

### Patterns to Preserve
- Phase documents with checkboxes track progress visibly
- "Deferred to Phase N" is explicit — nothing lost
- Risk mitigation has concrete fallbacks
- Decisions table forces explicit choices

---

*Created: December 2025*
*Target: Complete before Phase 4 (Component System)*