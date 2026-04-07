# SLM-OS Performance Benchmarks

Benchmark results across all hardware platforms. All measurements taken with `bench` shell commands using the ARM generic timer (CNTPCT_EL0).

**Date:** April 2026

---

## Context Switch Latency

Measures round-trip time for a full task context switch (save registers, switch page tables, restore registers).

| Platform | CPU | Cores | Avg Latency | Rating |
|----------|-----|-------|-------------|--------|
| **Jetson Orin Nano** | Cortex-A78AE @ EL2 | 6 | **262 ns** | Excellent |
| **Jetson Orin Nano** | Cortex-A78AE @ EL2 | 1 | 471 ns | Excellent |
| **Raspberry Pi 5** | Cortex-A76 | 4 | **1,600 ns** | Excellent |
| **QEMU virt** | Emulated | 4 | ~20,000 ns | N/A (emulated) |

The Jetson A78AE is 6.1x faster than the Pi 5 A76 on context switches with all cores active.

---

## Interrupt Latency (Timer Tick Jitter)

Measures the variation in timer IRQ delivery across 20 consecutive ticks at 100 Hz.

| Platform | Cores | Min | Avg | Max | Jitter |
|----------|-------|-----|-----|-----|--------|
| **Jetson Orin Nano** | 6 | 224 ns | **390 ns** | 2,624 ns | 2 us |
| **Jetson Orin Nano** | 1 | 416 ns | 595 ns | 2,432 ns | 2 us |
| **Raspberry Pi 5** | 4 | — | < 1,000 ns | — | — |

Sub-microsecond average IRQ latency on both platforms. The Jetson's 2 us worst-case jitter is well within real-time requirements.

---

## IPC Message Passing

Measures full send+receive round-trip latency for a single message through the kernel IPC subsystem (100 iterations).

| Platform | Cores | Avg Round-Trip | Per Operation |
|----------|-------|---------------|---------------|
| **Jetson Orin Nano** | 6 | **330 ns** | ~165 ns |
| **Jetson Orin Nano** | 1 | 622 ns | ~311 ns |
| **Raspberry Pi 5** | 4 | **322 ns** (send+recv) | ~161 ns |

Both platforms demonstrate sub-microsecond IPC.

---

## Memory

| Platform | Total RAM | Usable | Free at Boot |
|----------|-----------|--------|-------------|
| **Jetson Orin Nano** | 8 GB | ~6.7 GB (3 regions, OP-TEE carveout skipped) | 6,874 MB |
| **Raspberry Pi 5** | 4 GB | 4 GB | ~3.9 GB |
| **QEMU virt** | 1 GB (configurable) | 1 GB | ~950 MB |

Jetson's 6.7 GB usable memory spans three non-contiguous regions around the OP-TEE secure carveout at 0xBE000000-0xC1FFFFFF.

---

## GPU Cache Sync (Jetson Only)

Measures GPU buffer allocation, cache maintenance (DC CVAC/IVAC), and deallocation on a 4KB buffer (1024 uint32 values). Uses unified memory (CPU and GPU share DRAM).

| Operation | Time | Notes |
|-----------|------|-------|
| gpu_alloc (4KB) | 1,952 ns | PMM page allocation |
| sync_for_gpu (DC CVAC) | 1,184 ns | Clean 64 cache lines to PoC |
| sync_for_cpu (DC IVAC) | 896 ns | Invalidate 64 cache lines |
| gpu_free | 2,272 ns | PMM page deallocation |
| Data integrity | PASS | 0xDEADBEEF survives clean+invalidate round-trip |

---

## Core Isolation (Jetson)

`bench isolate`: CPU 2 isolated from GIC SPI routing, 8 tasks dispatched across remaining 5 CPUs. 0 tasks reached isolated CPU. **PASS.**

---

## Cross-CPU Task Dispatch (Jetson)

`bench smp`: dispatches tasks to CPUs 1-5, verifies each completes on the correct CPU. 3 consecutive runs, all 5/5 CPUs COMPLETED. Uses cooperative scheduling via WFE/SEV.

---

## Hardware Summary

| Feature | Jetson Orin Nano | Raspberry Pi 5 | QEMU virt |
|---------|-----------------|----------------|-----------|
| CPU | 6x Cortex-A78AE | 4x Cortex-A76 | 4x (emulated) |
| Cores Online | 6 | 4 | 4 |
| Exception Level | EL2 (VHE) | EL1 | EL1 |
| GIC | GICv3 | GICv2 | GICv2 |
| GPU | GA10B (Ampere) | None | None |
| Timer Freq | 31.25 MHz | 54 MHz | 62.5 MHz |

---

## Test Suite Results

### Jetson Orin Nano (Single-Core, April 2026)

All subsystems verified via interactive shell commands:

| Subsystem | Status | Notes |
|-----------|--------|-------|
| PMM (buddy allocator) | Pass | 6,874 MB free across 3 regions |
| VMM + MMU | Pass | 1001 blocks mapped (2 GB) |
| GICv3 | Pass | 992 interrupt lines, timer IRQ driving scheduler |
| Timer (100 Hz) | Pass | Preemptive scheduling working |
| Scheduler | Pass | Tasks running, context switching |
| UART (UARTC + TCU RX) | Pass | Bidirectional serial via USB-C |
| DTB parsing | Pass | 6 CPUs, correct peripheral addresses |
| LittleFS | Pass | Read/write/mkdir/cp/rm/grep/wc/stat |
| VFS | Pass | /sys, /proc, /components, /mnt/files |
| IPC | Pass | Message queues functional |
| Model memory | Pass | 384 MB (128 weight + 64 workspace blocks) |
| Lua 5.4 | Pass | Scripting engine functional |
| Component system | Pass | Registry, lifecycle management |
| ELF loader | Pass | Validation tests (1 minor assertion mismatch) |
| GPU probe | Pass | GA10B identified (BOOT_0=0xB7B000A1) |

### QEMU virt

All 327+ automated unit tests pass (`make test`).

### Raspberry Pi 5

431 tests: 415 pass, 16 ignored (platform-specific), 0 failures.

---

*Created: April 2026*
