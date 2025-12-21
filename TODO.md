# Phase 3: AI Infrastructure (C kernel + Rust runtime)

This document tracks Phase 3 implementation of SLM-OS.

**Status:** Not started

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
- [ ] Design model memory region layout (see design doc section 2.1.2)
- [ ] Define `ModelHandle` type in Rust (opaque handle to C-allocated memory)
- [ ] Plan memory pools: weight pool (read-only) vs workspace pool (read-write)
- [ ] Document memory lifecycle: load → map → use → unmap → unload

### Model Memory Allocator (Rust)
- [ ] Implement `ModelAllocator` struct in `runtime/src/mm/model_mem.rs`
- [ ] Write `alloc_model_region(size, flags)` — allocate contiguous 2MB-aligned region
- [ ] Write `free_model_region(handle)` — return region to pool
- [ ] Implement weight pool with read-only enforcement
- [ ] Implement workspace pool for inference scratch space
- [ ] Add statistics: total model memory, allocated, largest free block

### Zero-Copy Model Sharing
- [ ] Implement model reference counting (multiple components can use same model)
- [ ] Write `model_share(handle, target_component)` — share model with another component
- [ ] Write `model_unshare(handle, component)` — release component's reference
- [ ] Ensure model unload only happens when refcount reaches zero
- [ ] Test concurrent access from multiple tasks

### GPU Memory Integration
- [ ] Implement `gpu_map_model(handle)` — make model accessible to GPU
- [ ] Implement `gpu_unmap_model(handle)` — remove GPU mapping
- [ ] Handle cache coherency (flush before GPU access, invalidate after)
- [ ] Use `SHM_GPU_ACCESSIBLE` flag from Phase 2 shared buffers
- [ ] Test with placeholder GPU driver (actual GPU in Milestone 3)

### Model Memory Testing
- [ ] Test allocation/deallocation cycles
- [ ] Test zero-copy sharing between tasks
- [ ] Verify memory statistics accuracy
- [ ] Stress test with many small allocations
- [ ] Test large model allocation (approach RAM limits)

---

## Milestone 2: Deadline-Aware Scheduler

### Scheduler Policy Design (Rust)
- [ ] Define priority levels for inference tasks
- [ ] Design deadline representation (`deadline_ns: u64`)
- [ ] Plan scheduling algorithm (EDF vs fixed priority vs hybrid)
- [ ] Document interaction with C scheduler primitives

### Priority Infrastructure
- [ ] Switch FFI task API from pointer to PID-based (use existing `task->id`)
  - Change `slm_task_create()` to return `uint32_t` task ID instead of `void*`
  - Update Rust wrappers to use `TaskId` newtype
  - Enables safe validation via `task_get(id)` before use
- [ ] Add `priority` field to task structure (C side)
- [ ] Implement priority queue in scheduler (replace simple linked list)
- [ ] Add FFI function `slm_task_set_priority(task_id, priority)`
- [ ] Implement `slm_task_set_deadline(task_id, deadline_ns)`

### Deadline-Aware Scheduling (Rust)
- [ ] Implement `schedule_slm_task()` in `runtime/src/sched/deadline.rs`
- [ ] Urgent task detection (deadline < threshold → performance core)
- [ ] Working set size heuristics (small model → efficiency core OK)
- [ ] Implement `assign_to_performance_core()` / `assign_to_efficiency_core()`
- [ ] Load balancing across cores based on deadline pressure

### Per-Queue Scheduler Locks (Deferred from Phase 2)
- [ ] Replace global `sched.lock` with per-CPU run queue locks
- [ ] Implement lock-free task stealing between queues (optional)
- [ ] Reduce contention for cross-core operations
- [ ] Benchmark improvement over global lock

### Priority Inversion Prevention
- [ ] Implement priority inheritance for spinlocks
- [ ] Detect and log priority inversion events
- [ ] Test with high-priority task waiting on low-priority lock holder

### Scheduler Testing
- [ ] Test priority ordering (high priority runs first)
- [ ] Test deadline-aware scheduling (urgent tasks preempt)
- [ ] Verify no starvation of low-priority tasks
- [ ] Stress test with mixed priorities and deadlines
- [ ] Benchmark scheduling overhead

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

## Milestone 5: Deferred Items and Polish

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

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Pool Strategy** | Single pool vs separate weight/workspace | Separate pools enable read-only weights, but more complexity | Before starting M1 |
| **Alignment** | 2MB (huge page) vs 4KB | 2MB reduces TLB pressure for large models; 4KB more flexible | Before starting M1 |
| **Rust vs C Implementation** | All Rust vs Rust policy + C mechanism | Rust for safety; C if performance critical | Before starting M1 |

### Milestone 2 — Scheduler

| Decision | Options | Considerations | Deadline |
|----------|---------|----------------|----------|
| **Algorithm** | EDF vs fixed priority vs hybrid | EDF is optimal but complex; fixed priority is simpler | Before starting M2 |
| **Priority Levels** | 4 vs 8 vs 16 vs 32 | More levels = finer control but more overhead | Before priority infrastructure |
| **Lock-Free Stealing** | Implement vs defer | Performance benefit vs complexity; may not be needed for 4-6 cores | During per-queue locks |

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

### Dependencies Between Milestones

```
M1 (Model Memory) ─────────> M3 (GPU) ──> GPU needs model memory for DMA buffers
                    │
                    └──────> M5 (Rust Runtime) ──> ModelLoader uses model memory

M2 (Scheduler) ────────────> Independent, can parallel with M1

M3 (GPU) ──────────────────> M4 (Hardware) ──> GPU driver only testable on Jetson

M4 (Hardware) ─────────────> Should start early for serial console setup
```

Recommended order:
1. M4 (Hardware) — Get serial working ASAP for debugging
2. M1 (Model Memory) — Foundation for GPU and inference
3. M2 (Scheduler) — Can parallel with M1
4. M3 (GPU) — Requires M1 and M4
5. M5 (Polish) — Throughout

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