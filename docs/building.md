# Building SLM-OS

This document describes how to build, run, and debug SLM-OS.

---

## Prerequisites

### Required Tools

| Tool | Purpose | Installation |
|------|---------|--------------|
| `aarch64-none-elf-gcc` | ARM64 cross-compiler | [ARM GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) |
| `cmake` | Build system generator | [CMake Downloads](https://cmake.org/download/) |
| `make` | Build automation | Included with Cygwin or MinGW |
| `qemu-system-aarch64` | ARM64 emulator | [QEMU Downloads](https://www.qemu.org/download/) |
| `aarch64-none-elf-gdb` | Debugger | Included with ARM GNU Toolchain |
| `cargo` | Rust build tool (Phase 2+) | [rustup.rs](https://rustup.rs/) |

### Verify Installation

```bash
make check-tools
```

Expected output:
```
Checking required tools...
  [OK] aarch64-none-elf-gcc
  [OK] cmake
  [OK] cargo
  [OK] qemu-system-aarch64
  [OK] aarch64-none-elf-gdb
```

---

## Build Targets

### Quick Reference

| Command | Description |
|---------|-------------|
| `make` | Build kernel and runtime (default) |
| `make kernel` | Build C kernel only |
| `make run` | Build and run in QEMU |
| `make test` | Build and run automated tests |
| `make debug` | Build and run with GDB server |
| `make gdb` | Connect GDB to running QEMU |
| `make clean` | Remove all build artifacts |
| `make help` | Show all available targets |

### Build Targets

#### `make` / `make all`
Builds both the C kernel and Rust runtime (when available).

#### `make kernel`
Builds only the C kernel. This is the most common target during development.

Output:
- `build/kernel/slmos.elf` - Kernel ELF (for QEMU and debugging)
- `build/kernel/slmos.bin` - Raw binary (for real hardware)

#### `make runtime`
Builds the Rust runtime component (Phase 2+). Currently a placeholder.

### Clean Targets

#### `make clean`
Removes all build artifacts (kernel and runtime).

#### `make kernel-clean`
Removes only kernel build artifacts. Use when CMake cache needs refreshing.

#### `make kernel-rebuild`
Equivalent to `make kernel-clean && make kernel`.

### Run Targets

#### `make run`
Builds the kernel (if needed) and launches QEMU:

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -smp cores=4 \
    -m 512M \
    -nographic \
    -kernel build/kernel/slmos.elf
```

**QEMU Controls:**
- `Ctrl-A X` - Exit QEMU
- `Ctrl-A C` - Switch to QEMU monitor
- `Ctrl-A H` - Show help

#### `make debug`
Starts QEMU with GDB server enabled, waiting for debugger connection:

```bash
qemu-system-aarch64 \
    ... \
    -S \              # Start paused
    -gdb tcp::1234    # GDB server on port 1234
```

Run this in one terminal, then `make gdb` in another.

#### `make test`
Builds the kernel and runs the automated test suite in QEMU:

```bash
make test
```

Output on success:
```
PASSED - All tests passed
[INFO] VMM tests passed
[INFO] Spinlock tests passed
[INFO] SMP tests passed
[INFO] Scheduler tests passed
```

The test runs with a 60-second timeout. Test output is saved to `build/test-output.log`.
See `docs/testing.md` for details on the test framework.

#### `make gdb`
Connects GDB to a running QEMU instance:

```bash
aarch64-none-elf-gdb build/kernel/slmos.elf \
    -ex "target remote localhost:1234" \
    -ex "set confirm off"
```

**Common GDB Commands:**
| Command | Description |
|---------|-------------|
| `c` | Continue execution |
| `si` | Step one instruction |
| `n` | Step over (next line) |
| `b *0x40000000` | Breakpoint at address |
| `b kernel_main` | Breakpoint at function |
| `info registers` | Show all registers |
| `x/10i $pc` | Disassemble 10 instructions at PC |
| `x/10x $sp` | Examine 10 words at stack pointer |

### Utility Targets

#### `make info`
Shows current build configuration:

```
SLM-OS Build Information
========================
Build type:     Debug
Kernel ELF:     build/kernel/slmos.elf
Kernel binary:  build/kernel/slmos.bin
QEMU machine:   virt
QEMU CPU:       cortex-a76
QEMU memory:    512M
QEMU cores:     4
```

#### `make check-tools`
Verifies all required tools are installed and in PATH.

#### `make help`
Displays help text with all available targets.

---

## Build Configuration

### Build Types

```bash
make BUILD_TYPE=Debug    # Default: debug symbols, no optimization
make BUILD_TYPE=Release  # Optimized, no debug symbols
```

**Debug build flags:**
```
-g -O0 -DDEBUG
```

**Release build flags:**
```
-O2 -DNDEBUG
```

### Compiler Flags

Defined in `CMakeLists.txt`:

```cmake
# Common flags
-Wall -Wextra -Werror    # Strict warnings
-ffreestanding -nostdlib  # No standard library
-mcpu=cortex-a78ae       # Target CPU
-mgeneral-regs-only      # No FPU in kernel code (except context switch)
```

---

## QEMU Configuration

### Machine: `virt`

QEMU's generic ARM virtual machine with:
- PL011 UART at `0x0900_0000`
- GICv2 at `0x0800_0000` (distributor) and `0x0801_0000` (CPU interface)
- RAM starting at `0x4000_0000`
- Direct kernel loading (no bootloader)

### CPU: `cortex-a76`

Emulates ARM Cortex-A76 (same as Raspberry Pi 5). Supports:
- ARMv8.2-A architecture
- All standard ARM64 features used by SLM-OS

### Memory: `512M`

512MB RAM, sufficient for kernel development. Can be increased:

```bash
# In Makefile, change:
QEMU_MEMORY := 1G
```

### Cores: `4`

4 CPU cores. All cores are booted via PSCI and run the per-core scheduler. Tasks can be assigned to specific CPUs or migrated between them.

---

## Debugging Tips

### Common Issues

#### Kernel hangs silently
1. Run `make debug` + `make gdb`
2. Set breakpoint: `b kernel_main`
3. Continue: `c`
4. If it doesn't hit, check boot.S

#### Exception/crash
1. Look for exception output (EC, ESR, FAR, ELR)
2. Use `addr2line` to find source location:
   ```bash
   aarch64-none-elf-addr2line -e build/kernel/slmos.elf 0x40001234
   ```

#### UART not working
1. Verify UART_BASE in `platform.h` matches QEMU
2. Check PL011 initialization in `uart_pl011.c`

#### MMU-related crash
1. Disable MMU temporarily (comment out `vmm_init()`)
2. Verify page table alignment (4KB)
3. Check identity mapping covers current PC

### Useful GDB Scripts

Add to `~/.gdbinit`:

```gdb
# ARM64 helpers
define regs
    info registers x0 x1 x2 x3 x4 x5 x6 x7
    info registers x29 x30 sp pc
end

define stack
    x/20x $sp
end
```

---

## Project Structure

```
CS-496-SLM-Operating-System/
├── Makefile                 # Top-level build orchestration
├── CMakeLists.txt           # Kernel CMake configuration
├── cmake/
│   └── toolchain-aarch64-none-elf.cmake
├── kernel/
│   ├── kernel.ld            # Linker script
│   ├── include/             # Header files
│   ├── src/                 # C and assembly sources
│   └── drivers/             # Hardware drivers
├── runtime/                 # Rust runtime (Phase 2+)
├── build/                   # Build output (generated)
│   └── kernel/
│       ├── slmos.elf        # Kernel ELF
│       └── slmos.bin        # Raw binary
└── docs/                    # Documentation
```

---

## Platform-Specific Notes

### Windows (Cygwin)

The Makefile uses Windows paths for CMake compatibility:

```makefile
CMAKE := "C:/Program Files/CMake/bin/cmake.exe"
```

If using a different CMake location, update this path.

### Path with Spaces

The project is on Google Drive (`H:\My Drive\...`). This can cause issues:
- CMake handles it correctly with quoted paths
- Some tools may have problems with spaces in paths

### Build Directory on Network Drive

Google Drive syncing during builds can cause file locking issues:
- Build may fail with "file in use" errors
- Solution: Pause sync during builds, or use a local build directory

---

## Troubleshooting

### CMake configuration fails

```bash
make kernel-clean
make kernel
```

### "No rule to make target"

Ensure you're in the project root directory:
```bash
cd /path/to/CS-496-SLM-Operating-System
make kernel
```

### QEMU not found

Add QEMU to PATH or update Makefile:
```makefile
QEMU := /path/to/qemu-system-aarch64
```

### GDB connection refused

1. Ensure `make debug` is running in another terminal
2. Check port 1234 is not blocked by firewall
3. Try explicit host: `target remote 127.0.0.1:1234`

---

*Last updated: December 2025*
