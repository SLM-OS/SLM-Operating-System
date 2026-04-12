# SLM-OS Performance Benchmarks

Performance measurements across all supported platforms.

**Date:** April 2026
**Methodology:** All benchmarks run via the `bench` shell command. Each measurement is the average of multiple iterations (count noted per test). Hardware counter (CNTPCT_EL0 on ARM64, RDTSC on x86) provides nanosecond-resolution timing.

---

## Platform Summary

| Platform | CPU | Cores | Clock | RAM | Status |
|----------|-----|-------|-------|-----|--------|
| QEMU virt | Cortex-A76 (emulated) | 4 | N/A | 1 GB | Primary dev |
| Raspberry Pi 5 | BCM2712 Cortex-A76 | 4 | 2.4 GHz | 4 GB | Hardware verified |
| Jetson Orin Nano | Cortex-A78AE | 6 | 1.5 GHz | 8 GB | Blocked (kexec RAS) |
| x86-64 (QEMU) | max | 4 | N/A | 256 MB | Experimental |

---

## Kernel Benchmarks

### Context Switch Latency

Measures round-trip time for two tasks yielding back and forth on the same CPU. 99 round-trips per measurement.

| Platform | Average | Rating |
|----------|---------|--------|
| Pi 5 | **1.858 us** | Excellent (< 10 us) |
| QEMU ARM64 | ~5-15 us | Good (emulation overhead) |

**Target:** < 10 us. **Achieved on Pi 5.**

### Interrupt Latency (Timer Tick Jitter)

Measures deviation from the expected 10 ms timer tick interval using 20 consecutive samples. The measurement captures the full IRQ path: hardware interrupt assertion, GIC acknowledge, exception vector entry, handler dispatch, timer reload, and counter read. This represents the worst-case overhead added to each timer tick.

| Platform | Min | Average | Max | Jitter (max-min) |
|----------|-----|---------|-----|-------------------|
| Pi 5 | 1.445 us | 1.705 us | 2.055 us | 0.6 us |
| Pi 5 (under load) | 1.482 us | 1.782 us | 3.074 us | 1.6 us |

Timer frequency: 54 MHz (BCM2712 system counter). The "under load" measurement was taken with active tasks and shell output. The sub-2us average confirms that the GIC-to-handler path has minimal overhead on Cortex-A76.

### IPC Latency

Message queue send + receive round-trip, 100 iterations.

| Platform | Total | Average |
|----------|-------|---------|
| Pi 5 | 13.248 us | **132 ns** |

### Shared Buffer Throughput

4 KB buffer, 1000 iterations, 64-byte stride.

| Platform | Write | Read |
|----------|-------|------|
| Pi 5 | **45.8 GB/s** | **48.1 GB/s** |

### Component Benchmarks (Pi 5)

Measured via Lua REPL with `slm.uptime()` timing. Includes UART output overhead.

| Operation | Latency | Notes |
|-----------|---------|-------|
| Component load (`component_run`) | **3 ms** | Register + task create + add to scheduler |
| Component hot-swap | **11 ms** | Unregister old + load new + transfer subscriptions |
| Message publish + process | **6.39 ms** | Publish → route → subscriber receive → process → yield (100-msg avg, includes UART print per message) |
| Message publish (raw) | **~0.2 ms** | Estimated without UART overhead (IPC round-trip is 132 ns) |

### Scheduler Throughput

| Platform | Context Switches/sec | Active Tasks |
|----------|---------------------|--------------|
| Pi 5 (fresh boot, 3 tasks) | **69,353** | 3 |
| Pi 5 (after demo, 8 tasks) | **40,368** | 8 |

### Deadline-Boosted Dispatch

Task dispatch latency with deadline boost (priority escalation for urgent tasks).

| Platform | Dispatch Latency |
|----------|-----------------|
| Pi 5 | ~18 us |

### Memory Usage (Pi 5, Runtime)

| Resource | Size |
|----------|------|
| Total RAM | 4,188,256 KB (4 GB) |
| Kernel used | 395,652 KB (387 MB) |
| Free | 3,792,604 KB (3.6 GB) |
| Model weight pool | 256 MB (128 x 2 MB blocks) |
| Model workspace pool | 128 MB (64 x 2 MB blocks) |
| Rust heap | 1 MB |
| RAM disk (LittleFS) | 1 MB |

---

## Boot Performance

| Platform | Total (power to shell) | Kernel init | Firmware |
|----------|----------------------|-------------|----------|
| Pi 5 | 8.5 s | ~1.6 s | ~5-7 s |
| QEMU ARM64 | ~1 s | ~1 s | N/A |

### Boot Phase Breakdown (Pi 5)

Estimated from serial output timing at 115200 baud. Total kernel init ~1.6s (serial capture), ~3.5s (power-on including firmware).

| Phase | Estimated Time | Notes |
|-------|---------------|-------|
| DTB parse + PMM init | ~100 ms | Buddy allocator setup for 4 GB |
| VMM/MMU enable | ~50 ms | Page table construction (2054 blocks) |
| VMM validation tests | ~50 ms | 6 tests — skip in production |
| GIC + Timer init | ~20 ms | |
| SMP boot (3 secondary CPUs) | ~600 ms | Sequential PSCI CPU_ON with handshake |
| Spinlock + SMP validation | ~100 ms | 16 tests — skip in production |
| Filesystem (ramdisk + LittleFS) | ~200 ms | Format, mount, write 38 help files + demo |
| Rust runtime init | ~50 ms | Heap + FFI validation |
| Component + Model + GPU init | ~100 ms | Pool allocation (384 MB logical) |
| Scheduler + Shell start | ~50 ms | |

### Optimization Opportunities

| Optimization | Estimated Savings |
|-------------|-------------------|
| Skip boot-time validation tests | ~150 ms |
| Parallel SMP boot (simultaneous PSCI) | ~400 ms |
| Lazy help/demo file writing | ~100 ms |
| **Total potential** | **~650 ms** |

**Target:** < 2 s kernel init. Current: ~1.6 s on Pi 5 (measured from first serial output to shell prompt). The 3.5s figure from earlier measurements included serial capture connection latency. With the optimizations above, sub-1s kernel init is achievable.

---

## Binary Size

| Platform | Binary (.bin) | ELF text | ELF data | ELF bss |
|----------|--------------|----------|----------|---------|
| QEMU ARM64 | 973 KB | 912 KB | 62 KB | 1.9 MB |
| Pi 5 | 824 KB | 762 KB | 61 KB | 2.6 MB |
| Jetson | 893 KB | 818 KB | 61 KB | 3.6 MB |
| x86-64 | 610 KB | 544 KB | 59 KB | 5.1 MB |

BSS varies by platform due to: task table size (NC vs BSS), per-CPU data, platform-specific driver buffers. The kernel binary includes: C kernel, Lua 5.4 interpreter, embedded help files, and demo script.

---

## Platform Comparison (QEMU)

Emulated benchmarks on the same host. QEMU ARM64 emulates Cortex-A76; QEMU x86-64 uses host CPU passthrough. Numbers reflect emulation overhead, not native hardware performance.

| Metric | QEMU ARM64 | QEMU x86-64 | Pi 5 (native) |
|--------|-----------|-------------|---------------|
| Context switch | ~807 ns | ~1,332 ns | **1,858 ns** |
| IPC round-trip | ~1,709 ns | ~761 ns | **132 ns** |
| Buffer write | 9.7 GB/s | 21.0 GB/s | **45.8 GB/s** |
| Buffer read | 8.2 GB/s | 14.1 GB/s | **48.1 GB/s** |
| Binary size | 973 KB | 610 KB | 824 KB |
| Tests passing | 620+ (all) | 426/434 | 615+/620+ |

QEMU numbers vary between runs due to host load and emulation non-determinism. Pi 5 native numbers are the authoritative measurements for capstone evaluation.

---

## Memory Usage

### Model Memory Pools

| Pool | Block Size | Blocks | Total |
|------|-----------|--------|-------|
| Weight (read-only) | 2 MB | 128 | 256 MB |
| Workspace (scratch) | 2 MB | 64 | 128 MB |
| **Total** | | **192** | **384 MB** |

### Kernel Memory (Pi 5)

| Region | Size |
|--------|------|
| Kernel code + data | ~1 MB |
| PMM heap | 4,076 MB (of 4 GB RAM) |
| NC shared memory | 2 MB |
| Rust heap | 1 MB |
| Model pools | 384 MB |
| RAM disk (LittleFS) | 1 MB |
| Task stacks | 16 KB each (max 32 tasks) |

---

## AI Scheduler Inference

Real trained MLP and PPO weights from the Plan A export pipeline (108→256→256→128→42 architecture, ~3 MB per model).

| Metric | Pi 5 (native) | Target |
|--------|--------------|--------|
| MLP inference latency | **41.9 us** | < 50 us |
| State extraction | 108 dimensions | — |
| Action space | 42 actions (7 cores x 3 priority x 2 preempt) | — |
| Weight size (MLP) | 3.0 MB | — |
| Weight size (PPO) | 3.0 MB | — |
| Kernel binary with AI | 1.9 MB (vs 824 KB without) | — |

**Target achieved:** 41.9 us < 50 us on Cortex-A76 @ 2.4 GHz.

The latency includes: FP context save, state vector extraction (108 floats from kernel data), 4-layer forward pass (NEON-optimized matvec), action decode and validation, FP context restore. Measured via `test_ai_inference_latency` (100 iterations, average reported by the test).

---

## Test Suite Performance

| Platform | Tests | Pass | Fail | Time |
|----------|-------|------|------|------|
| QEMU ARM64 | 620+ | All | 0 | ~15 s |
| Pi 5 | 620+ | 615+ | 5 | ~24 s |
| x86-64 | 434 | 426 | 8 | ~10 s |

Pi 5 failures: 5 multi-core integration tests (secondary CPU preemption limitation).
x86-64 failures: 8 platform-specific tests (PIC, PCI, GPU commands not implemented).

---

## Comparison Notes

Direct comparison with Linux is planned but not yet completed. Key advantages of SLM-OS:

- **Context switch:** 1.858 us (Linux RT kernel typical: 3-10 us)
- **IPC:** 132 ns (Linux pipe: ~2 us, shared memory: ~200 ns)
- **Boot time:** 3.5 s kernel (Linux minimal: 1-3 s, full Ubuntu: 20+ s)
- **Binary size:** < 1 MB (Linux kernel: 10-30 MB)
- **Memory overhead:** < 2 MB kernel (Linux minimum: ~50 MB)

These advantages come from the bare-metal design: no system call overhead, no virtual memory TLB faults, no process isolation overhead, and purpose-built AI memory management.

---

*Measured: April 2026 on Raspberry Pi 5 (BCM2712, 4x Cortex-A76 @ 2.4 GHz, 4 GB LPDDR4X)*
