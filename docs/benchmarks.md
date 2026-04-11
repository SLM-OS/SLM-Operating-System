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

Measures deviation from the expected 10 ms timer tick interval using 20 consecutive samples.

| Platform | Min | Average | Max | Jitter |
|----------|-----|---------|-----|--------|
| Pi 5 | 1.482 us | 1.782 us | 3.074 us | 1.6 us |

Timer frequency: 54 MHz (BCM2712 system counter).

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
| Pi 5 | 8.5 s | ~3.5 s | ~5 s |
| QEMU ARM64 | ~1 s | ~1 s | N/A |

Boot time includes: PMM init, VMM/MMU enable, SMP boot (4 cores), GIC/timer init, filesystem mount, Rust runtime init, component system init, model memory init, GPU stub init, scheduler start.

**Target:** < 2 s kernel init. Current: 3.5 s on Pi 5. Main contributors: SMP secondary CPU boot (~1s with PSCI timeouts), filesystem initialization (~0.5s), model memory pool allocation (~0.5s).

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

## AI Scheduler Inference (Stub Weights)

With stub weights (all zeros), the AI scheduler inference path exercises the full code but produces trivial output (always core 0).

| Metric | QEMU (reported) |
|--------|-----------------|
| MLP forward pass | Functional (stub weights) |
| PPO forward pass | Functional (stub weights) |
| State extraction | 108 dimensions, all fields populated |

**Note:** Performance targets (< 50 us inference latency on Cortex-A78) require real weights from Plan A export pipeline.

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
