# Getting Started with SLM-OS

This guide covers building, running, and testing SLM-OS from a fresh clone. The goal is to go from checkout to a running demo in approximately 15 minutes.

---

## Introduction

SLM-OS is a bare-metal operating system designed for AI inference on edge devices. It provides a cooperative scheduler, virtual filesystem, ONNX model loading, component-based application architecture, and Lua scripting -- all running without an underlying OS or hypervisor.

Supported platforms:

| Platform | Target | Status |
|----------|--------|--------|
| QEMU ARM64 | `QEMU_VIRT` (default) | Primary development target |
| Raspberry Pi 5 | `RASPI5` | Hardware target (serial console) |
| Jetson Orin Nano | `JETSON_ORIN_NANO` | Hardware target (EL2 via kexec) |
| x86-64 | `X86_64` | QEMU with GRUB multiboot2 |

---

## Prerequisites

The following tools must be installed before building:

- **ARM GNU Toolchain** -- `aarch64-none-elf-gcc` (cross-compiler for ARM64 platforms)
- **GCC for x86-64** -- `x86_64-linux-gnu-gcc` (required only for the x86-64 platform)
- **GNU Make**
- **CMake 3.20+**
- **QEMU** -- `qemu-system-aarch64` for ARM64, `qemu-system-x86_64` for x86-64
- **Rust toolchain** -- with `aarch64-unknown-none` and `x86_64-unknown-none` targets added
- **grub-mkrescue** -- required only for x86-64 ISO creation

To verify installed tools:

```bash
make check-tools
```

### Installing Rust Targets

```bash
rustup target add aarch64-unknown-none
rustup target add x86_64-unknown-none
```

---

## Building

The top-level Makefile automatically selects the correct toolchain, QEMU binary, and machine configuration based on the `PLATFORM` variable.

### ARM64 (QEMU -- default)

```bash
make kernel
```

This configures CMake, builds the Rust runtime, and compiles the C kernel into `build/kernel/slmos.elf`.

### Raspberry Pi 5

```bash
make kernel PLATFORM=RASPI5
```

The output binary is `build/kernel/slmos.bin`, which must be copied to the Pi 5 SD card as `kernel_2712.img`.

### x86-64

```bash
make kernel PLATFORM=X86_64
```

### Jetson Orin Nano

```bash
make kernel PLATFORM=JETSON_ORIN_NANO
```

### Clean Rebuilds

When switching platforms or after significant changes, a clean rebuild prevents stale object issues:

```bash
make kernel-clean && make kernel PLATFORM=RASPI5
```

---

## Running on QEMU

### Interactive Shell

```bash
make run
```

or equivalently:

```bash
make shell
```

SLM-OS boots into an interactive shell. Press `Ctrl+A` then `X` to exit QEMU.

The shell provides 30+ commands for system inspection, file management, model loading, and scripting. Type `help` at the prompt for a full command listing, or `help <command>` for detailed usage of a specific command.

### Debugging with GDB

In one terminal:

```bash
make debug
```

In a second terminal:

```bash
make gdb
```

QEMU starts paused with a GDB server on port 1234. The `make gdb` target connects `aarch64-none-elf-gdb` and loads symbols from the kernel ELF.

---

## Running on Raspberry Pi 5

### SD Card Setup

1. Prepare a micro SD card with a FAT32 boot partition containing the standard Raspberry Pi firmware files (`start4.elf`, `fixup4.dat`, etc.).
2. Build the kernel for Pi 5:
   ```bash
   make kernel PLATFORM=RASPI5
   ```
3. Copy `build/kernel/slmos.bin` to the SD card boot partition, renaming it to `kernel_2712.img`.
4. Ensure `config.txt` on the boot partition contains:
   ```
   kernel=kernel_2712.img
   arm_64bit=1
   ```
5. Insert the SD card and power on the Pi 5.

### Serial Console

SLM-OS outputs to the PL011 UART. A serial connection is required to interact with the shell:

- **GPIO header:** Connect a 3.3V USB-to-serial adapter to GPIO pins 8 (TX) and 10 (RX). Use 115200 baud, 8N1.
- **USB debug adapter:** If a Raspberry Pi Debug Probe is available, connect it to the 3-pin UART header on the Pi 5 board.

### Firmware Requirements

The Pi 5 EEPROM firmware must support loading a bare-metal kernel. Standard Raspberry Pi OS firmware files on the boot partition handle the initial hardware setup before transferring control to `kernel_2712.img`.

---

## Running Tests

### Full Test Suite

```bash
make test
```

This builds a test kernel with `ENABLE_BOOT_TESTS=ON`, launches QEMU with semihosting enabled, and runs all test suites. The test harness uses ARM semihosting (ARM64) or `isa-debug-exit` (x86-64) to signal pass/fail to the host.

The test suite contains 600+ tests across all subsystems:

- Physical memory manager (buddy allocator)
- Virtual memory manager and TLB invalidation
- Scheduler (priority, deadline, isolation, benchmarks)
- IPC (message queues, shared buffers)
- Priority inheritance mutexes
- GPU subsystem
- Component system
- Virtual filesystem and LittleFS
- Shell command parsing and path resolution
- Networking (QEMU only)
- Lua scripting integration
- Multi-core integration

### QEMU Safeguards

The test runner enforces resource limits to prevent runaway tests from consuming host resources:

- **Memory:** `systemd-run` enforces a 3 GB RSS limit via cgroups
- **Timeout:** Tests must complete within 120 seconds or QEMU is killed
- **CPU:** CPU quota is capped at 200%

### x86-64 Tests

```bash
make test PLATFORM=X86_64
```

The x86-64 test target creates a GRUB ISO and uses the `isa-debug-exit` device for clean test termination. An exit code of 1 from QEMU indicates all tests passed (the `isa-debug-exit` device maps value 0 to exit code 1).

---

## Running the Demo

SLM-OS includes a Lua demo script that exercises the AI inference pipeline.

### Steps

1. Boot SLM-OS in QEMU:
   ```bash
   make run
   ```

2. At the shell prompt, run the demo script:
   ```
   slmos> lua /mnt/files/demo.lua
   ```

### What the Demo Shows

The industrial IoT demo exercises the major runtime subsystems:

- **Component lifecycle** -- Starts and manages the sensor_monitor component
- **Message routing** -- Publishes sensor readings to `/sensors/data` via pub/sub
- **Anomaly detection** -- Threshold monitoring triggers alerts for values above 50
- **Live hot-swap** -- Replaces the monitoring component with zero downtime
- **Component lifecycle** -- Demonstrates start, stop, and hot-swap of running components

---

## Key Shell Commands

### System Information

| Command | Description |
|---------|-------------|
| `mem` | Physical memory statistics (total pages, free, allocated) |
| `tasks` | List all tasks with ID, state, CPU affinity, and priority |
| `cpu` | Per-core status (online state, current task) |
| `uptime` | System uptime in seconds and milliseconds |
| `vmm` | Virtual memory statistics (page tables, mapped regions) |

### Performance Benchmarks

| Command | Description |
|---------|-------------|
| `bench context` | Context switch latency |
| `bench irq` | Interrupt handling latency |
| `bench ipc` | IPC throughput |
| `bench stats` | Scheduler statistics |
| `bench all` | Run all benchmarks |

### AI Model Management

| Command | Description |
|---------|-------------|
| `model load <path>` | Load an ONNX model from VFS |
| `model list` | List loaded models with parameter counts |
| `model infer <name>` | Run inference with zero input |
| `model bench <name> [N]` | Benchmark inference latency (default: 10 iterations) |
| `model gpu` | Show GPU status and inference backend |
| `model pools` | Show weight and workspace memory pool stats |

### Component System

| Command | Description |
|---------|-------------|
| `component list` | List registered components |
| `component run <name>` | Start a component |
| `component swap <old> <new>` | Hot-swap a running component |
| `component status` | Show component states |

### Scheduler

| Command | Description |
|---------|-------------|
| `sched` | Show current scheduling policy |
| `sched policy` | List all registered policies |
| `sched policy <name>` | Switch scheduling policy at runtime |
| `sched stats` | Show per-CPU utilization and scheduler statistics |

### Lua Scripting

| Command | Description |
|---------|-------------|
| `lua` | Enter interactive Lua REPL |
| `lua -e "code"` | Execute a Lua expression |
| `lua <file>` | Run a Lua script from the filesystem |

### Filesystem

The shell provides standard file operations: `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `stat`, `tree`, `find`, `grep`, `hexdump`, `wc`, `df`, `write`, `append`, and `truncate`.

---

## Project Structure

```
SLM-OS/
  kernel/           C kernel source (boot, scheduler, memory, drivers, shell)
  runtime/          Rust runtime (model loading, ONNX inference, allocator)
  cmake/            CMake toolchain files
  scripts/          Demo and utility scripts (Lua)
  docs/             Documentation
  Makefile          Top-level build orchestration
```

---

## Further Reading

- `docs/architecture.md` -- System architecture overview
- `docs/shell.md` -- Complete shell command reference
- `docs/testing.md` -- Test framework details and writing new tests
- `docs/mmu.md` -- Memory management and virtual memory
- `docs/scheduler.md` -- Scheduler policies and AI-driven scheduling
- `docs/components.md` -- Component system design
- `docs/onnx-support.md` -- ONNX model format and inference pipeline
- `docs/lua.md` -- Lua scripting API reference
