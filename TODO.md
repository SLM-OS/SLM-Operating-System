# Phase 3: AI Infrastructure (C kernel + Rust runtime)

This document tracks Phase 3 implementation of SLM-OS.

**Status:** In progress (M1-3 complete, M4 DTB parser done, M5 shell complete, M6 complete, M4 hardware testing pending)

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
- 🔗 Handle cache coherency (flush before GPU access, invalidate after) — blocked by M3
- 🔗 Use `SHM_GPU_ACCESSIBLE` flag from Phase 2 shared buffers — blocked by M3
- 🔗 Test with placeholder GPU driver (actual GPU in Milestone 3) — blocked by M3

### Model Memory Testing
- ✅ Test allocation/deallocation cycles
- ✅ Test zero-copy sharing between tasks
- ✅ Verify memory statistics accuracy
- ✅ Stress test: pool exhaustion and recovery
- [ ] Test large model allocation (approach RAM limits) — needs more RAM

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

### Per-Queue Scheduler Locks (Carried from Phase 2)
- ✅ Replace global `sched.lock` with per-CPU run queue locks
  - Each `cpu_runqueue` now has its own `spinlock_t lock`
  - Single-queue operations use per-queue lock
  - Cross-queue operations (migration) lock both queues in CPU ID order to prevent deadlock
  - Global statistics are racy but acceptable for diagnostics
- ⏸️ Implement lock-free task stealing between queues (optional) — deferred
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
- ✅ Study Jetson Orin Nano GPU architecture (Ampere, 1024 cores)
  - Researched GSP (GPU System Processor) requirement for modern NVIDIA GPUs
  - GSP is RISC-V core that handles GPU init; requires firmware from filesystem
  - Full bare-metal GPU compute impractical without GSP firmware loading
- ✅ Document MMIO register map for GPU control
  - Documented Host1x DMA engine (push buffers, syncpoints)
  - See `docs/gpu.md` for architecture details
- ✅ Review NVIDIA open-source kernel driver for reference
  - Reviewed open-gpu-kernel-modules and drm/tegra
  - Confirmed GSP dependency for Ampere architecture
- ✅ Identify minimal initialization sequence
  - Memory allocation + cache coherency is achievable
  - Full GPU init requires GSP firmware (deferred to Phase 5)
- ✅ Plan DMA buffer allocation for GPU data transfer
  - GPU buffer API with cpu_addr/gpu_addr for unified memory

### Platform Abstraction
- ✅ Create `kernel/gpu/` directory structure
  - `gpu.h` — Public API and driver interface
  - `gpu.c` — Driver registration and dispatch
  - `cache.c` — ARM64 cache maintenance
  - `gpu_stub.c` — QEMU stub driver
- ✅ Define GPU driver interface (`struct gpu_driver`)
  - init/shutdown, alloc/free, sync_for_gpu/sync_for_cpu
  - Extensible for future submit/wait operations
- ✅ Implement QEMU stub driver (no-op, for testing without GPU)
  - Allocates from PMM, exercises cache coherency code path
  - Reports GPU_CAP_NONE but allows API usage
- ✅ Add platform detection to select correct driver
  - `gpu_register_driver()` called from main.c with appropriate driver

### Jetson GPU Driver (C)
- 🔗 Write `jetson_gpu_init()` — power on, clock enable, reset sequence — requires M4
- 🔗 Write `jetson_gpu_alloc(size)` — allocate GPU-accessible memory — requires M4
- 🔗 Write `jetson_gpu_free(addr)` — free GPU memory — requires M4
- 🔗 Write `jetson_gpu_submit(cmd_buffer)` — submit work to GPU — requires M4
- 🔗 Write `jetson_gpu_wait()` — wait for GPU completion — requires M4
- 🔗 Implement basic fence/sync mechanism — requires M4

### Memory Coherency
- ✅ Implement cache flush before GPU access (`dc cvac`, `dc civac`)
  - `cache_clean_range()` — write back dirty lines
  - `cache_flush_range()` — clean + invalidate
- ✅ Implement cache invalidate after GPU write (`dc ivac`)
  - `cache_invalidate_range()` — discard stale cache
- ✅ Test with simple GPU memory copy operation
  - Stub driver exercises cache ops; verified in test suite
- ✅ Document coherency requirements in `docs/gpu.md`

### GPU Testing
- 🔗 Test GPU initialization on real Jetson hardware — requires M4
- ✅ Test memory allocation and mapping — stub driver works
- 🔗 Test simple compute operation (if possible without CUDA) — requires M4
- 🔗 Verify CPU can read GPU-written data correctly — requires M4
- ✅ Fallback: Defer actual GPU compute to Phase 5 (SLM Integration)
  - Decision documented in Outstanding Decisions section

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

### Device Tree Support (Required)
- ✅ Implement minimal DTB parser (`kernel/src/dtb.c`, `kernel/include/dtb.h`)
  - Parses FDT header, validates magic and version
  - Walks structure block extracting properties
  - Extracts: RAM, UART, GIC, CPU count, timer IRQ
  - Falls back to `platform.h` defaults if parsing fails
- ✅ Add `dtb` shell command to display parsed/default values
- ✅ Preserve DTB pointer in boot.S (x0 → x19 → kernel_main)
- [ ] Extract memory regions from device tree (parser ready, QEMU ELF boot doesn't pass DTB)
- [ ] Extract interrupt configuration from device tree
- [ ] Remove hardcoded addresses from `platform.h` (after hardware testing confirms DTB works)
- [ ] Test on both Jetson and Pi 5 with platform-specific DTBs

### Hardware Testing
- [ ] Full test suite passes on Jetson
- [ ] Multi-core stress test on real hardware
- [ ] IPC stress test on real hardware
- [ ] Measure actual context switch time (< 10 µs target)
- [ ] Measure actual interrupt latency

---

## Milestone 5: Debug Shell & ELF Execution

### Custom Shell Implementation ✅
- ✅ Create `shell.h` API header with command registration interface
- ✅ Implement shell task running on CPU 0 at low priority
- ✅ Implement `shell_init()` — initialize shell subsystem
- ✅ Implement `shell_run()` — main loop with line editing (backspace, Ctrl+C)
- ✅ Implement `shell_execute()` — execute command string directly
- ✅ Implement `shell_register_command()` — register external commands
- ✅ Integrate into `kernel_main()` with `shell_start()`
- ✅ Verify prompt and echo working in QEMU

### Built-in Commands (System Inspection)
- ✅ `help` — list available commands
- ✅ `mem` — PMM statistics (total, free, allocated pages)
- ✅ `tasks` — list tasks (PID, state, CPU, name, priority, switches)
- ✅ `cpu` — per-core status (online, isolated, current task)
- ✅ `uptime` — system uptime (hours:minutes:seconds)
- ✅ `clear` — clear screen (ANSI escape codes)
- ✅ `reboot` — system restart (via PSCI)
- ✅ `vmm` — virtual memory regions and flags
- ✅ `ipc` — message queue and shared buffer stats
- ✅ `model` — model memory pool status

### Virtual Filesystem Structure ⏸️
- ⏸️ Mount root `/` with command nodes — deferred to Phase 4
- ⏸️ Mount `/sys/` for system info (read-only virtual files) — deferred to Phase 4
- ⏸️ Mount `/proc/` for per-task info (stretch goal) — deferred to Phase 4
- ⏸️ Design `/components/` mount point for Phase 4 — deferred to Phase 4

### Basic ELF Loader ⏸️
- ⏸️ Implement minimal ELF64 parser (`elf_loader.c`) — deferred
    - Parse ELF header, program headers
    - Support `PT_LOAD` segments only
    - Validate for ARM64 architecture
- ⏸️ `elf_load(buffer, size)` — load ELF from memory buffer — deferred
    - Allocate pages for code/data segments
    - Map into address space (component region)
    - Return entry point address
- ⏸️ Create task from ELF entry point — deferred
- ⏸️ Test with minimal "hello world" ELF (prints to UART and exits) — deferred

### Shell ELF Execution Command ⏸️
- ⏸️ `run <name>` — load and execute ELF from built-in table (initially) — deferred
- ⏸️ Pass argc/argv to loaded program (simple stack setup) — deferred
- ⏸️ Handle task exit and cleanup — deferred
- ⏸️ `kill <pid>` — terminate running task — deferred

### Testing
- ✅ Shell responds to commands in QEMU
- ✅ Built-in commands work correctly
- ⏸️ Load and run trivial ELF executable — deferred to Phase 4 (requires ELF loader)
- ⏸️ Task exits cleanly, memory reclaimed — deferred to Phase 4 (requires ELF loader)
- 🔗 Test on Jetson — requires M4

---

## Milestone 6: Deferred Items and Polish

### Page Fault Handling (Carried from Phase 2) ✅
- ✅ Implement basic page fault handler (panic with useful info)
- ✅ Log faulting address, access type, task ID
- ✅ Add fault address to panic register dump
- ⏸️ Future: demand paging for model memory — deferred to Phase 5+

### IPC Improvements (Carried from Phase 2) ✅
- ✅ Implement proper timeout handling in `msg_recv()` and `msg_send()`
  - Uses timer_get_count() for elapsed time tracking
  - Timed waits use polling with yield() for simplicity
  - Infinite waits use blocking (BLOCKED state)
- ✅ Queue stress test (high throughput single-threaded)
  - 100 rounds of 10 send / 8 recv = 856+ messages
  - Verifies all messages accounted for
- ✅ Memory leak verification after IPC teardown
  - Tests queue create/destroy and buffer map/unmap cycles
  - Compares free pages before/after
- ✅ Add IPC statistics (messages sent, queue high-water mark)
  - Per-queue: msgs_sent, msgs_recv, high_water
  - Global: queue_count, buffer_count, total_msgs_sent/recv
  - `msg_queue_stats()` and `ipc_get_stats()` API

### Rust Runtime Expansion ✅
- ✅ Implement `ModelLoader` struct skeleton (full implementation Phase 5)
  - `runtime/src/mm/model_loader.rs` with ModelFormat, ModelMetadata, LoadedModel
  - ModelLoader with detect_format() and load_from_buffer() skeleton
- ✅ Implement `InferenceScheduler` struct skeleton
  - `runtime/src/sched/inference.rs` with RequestState, InferenceConfig
  - InferenceScheduler with submit(), cancel(), get_result() skeleton
- ✅ Add logging infrastructure in Rust (via FFI to UART)
  - `runtime/src/log.rs` with LogLevel, log_debug/info/warn/error
  - Numeric formatting (decimal and hex) without std
  - FFI exports for C code to use Rust logging
- ✅ Create `runtime/src/sched/heterogeneous.rs` for big.LITTLE awareness
  - CpuTopology, CoreInfo, ClusterInfo for CPU topology
  - TaskPlacement policy for core selection
  - LoadBalancer with load-aware core selection

### Documentation ✅
- ✅ Update architecture doc with Phase 3 learnings
  - Created `docs/architecture.md` with system overview and Phase 3 learnings
- ✅ Document GPU driver interface
  - `docs/gpu.md` already comprehensive (GPU HAL, driver interface, cache ops)
- ✅ Document model memory API
  - `docs/model-memory.md` updated with ModelLoader section
- ✅ Update build instructions for Jetson target
  - Added "Building for Jetson Orin Nano" section to `docs/building.md`
- ✅ Create troubleshooting guide for hardware issues
  - Created `docs/troubleshooting.md` covering build, QEMU, Jetson, and runtime issues


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

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **GPU Scope** | Init only vs basic compute vs CUDA | **Memory + coherency** | Full GPU compute requires GSP firmware (RISC-V on GPU) which needs Linux-like environment. We provide GPU-accessible memory allocation and cache coherency. Actual compute deferred to Phase 5 with TensorRT/CUDA runtime. |
| **Driver Model** | Kernel driver vs user-space | **Kernel driver** | Simpler for bare-metal. `struct gpu_driver` interface with platform-specific implementations. |
| **Fallback** | CPU-only inference | **Stub driver** | QEMU uses `gpu_stub_driver` which allocates regular memory and exercises cache coherency code path. SLM-OS code uses GPU API uniformly. |

### Milestone 4 — Hardware

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Boot Method** | U-Boot vs UEFI | **UEFI (extlinux.conf)** | Jetson Orin series uses UEFI, not U-Boot. UEFI reads /boot/extlinux/extlinux.conf from rootfs. Pi 5 may need different approach. |
| **Device Tree** | Full parsing vs minimal vs hardcoded | **Hardcoded initially, DTB required later** | Get booting first with hardcoded addresses in `platform.h`. DTB parsing is NOT optional — will be implemented before Phase 4 completion. |
| **Pi 5 Support** | Now vs later vs never | **Later** | Focus on Jetson first; Pi 5 as fallback if Jetson stalls. May need U-Boot or native bootloader (different from Jetson's UEFI). |

### Milestone 5 — Debug Shell

| Decision | Options | **Choice** | Rationale |
|----------|---------|------------|-----------|
| **Shell implementation** | MicroShell library vs Custom minimal | **Custom minimal** | No external dependencies, simpler, full control. ~500 lines C. |
| **Shell task priority** | High vs normal vs low vs idle | **IDLE (0)** | Shell shouldn't preempt tests or real work. Runs only when system is otherwise idle. |
| **ELF source** | Embedded in kernel vs filesystem vs network | **Deferred** | ELF loader deferred. Shell extensible via `shell_register_command()` for now. |
| **Address space** | Shared with kernel vs isolated | **Shared** for Phase 3 — isolation in Phase 4+ |


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