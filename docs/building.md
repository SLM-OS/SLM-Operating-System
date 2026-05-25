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

### Rust Setup

Rust is required for the runtime component. Install via rustup:

**Windows:**
```
Download and run: https://win.rustup.rs/x86_64
```

**After installation, add the bare-metal target:**
```bash
rustup target add aarch64-unknown-none
```

**For Cygwin/Git Bash users:** Add cargo to PATH in `~/.bash_profile`:
```bash
export PATH="/c/Users/<username>/.cargo/bin:$PATH"
```

Then `source ~/.bash_profile` to apply.

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

Verify Rust target:
```bash
rustup target list --installed | grep aarch64
# Should show: aarch64-unknown-none
```

### Codex Worktrees

If you use Codex for development, prefer launching it from a dedicated
branch worktree instead of the main checkout. This keeps `main` clean
and avoids accidental direct commits there.

Repo helper:

```bash
scripts/codex-worktree.sh <branch-name> [base-ref]
```

Example:

```bash
scripts/codex-worktree.sh feature-eviction-loader
```

That will:

- fetch `origin`
- create a sibling worktree on a new branch from `origin/main` by default
- `cd` into that worktree
- launch `codex`

---

## Build Targets

### Quick Reference

| Command | Description |
|---------|-------------|
| `make` | Build kernel and runtime (default) |
| `make kernel` | Build C kernel only |
| `make kernel-test` | Build test kernel (with ENABLE_BOOT_TESTS) |
| `make runtime` | Build Rust runtime only |
| `make run` | Build and run in QEMU |
| `make shell` | Build and run with interactive shell |
| `make test` | Build test kernel and run in QEMU (CI mode) |
| `make debug` | Build and run with GDB server |
| `make gdb` | Connect GDB to running QEMU |
| `make clean` | Remove all build artifacts |
| `make kernel-clean` | Remove kernel build artifacts |
| `make kernel-test-clean` | Remove test kernel build artifacts |
| `make runtime-clean` | Remove runtime build artifacts |
| `make kernel-rebuild` | Clean and rebuild kernel |
| `make runtime-rebuild` | Clean and rebuild runtime |
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
Builds the Rust runtime component. This produces a static library that is linked with the C kernel.

Output:
- `runtime/target/aarch64-unknown-none/debug/libslm_runtime.a` - Debug build
- `runtime/target/aarch64-unknown-none/release/libslm_runtime.a` - Release build

Note: `make kernel` automatically builds the runtime first, so explicit `make runtime` is rarely needed.

### Clean Targets

#### `make clean`
Removes all build artifacts (kernel, test kernel, and runtime).

#### `make kernel-clean`
Removes only kernel build artifacts. Use when CMake cache needs refreshing.

#### `make kernel-test-clean`
Removes only test kernel build artifacts (`build/kernel-test/`).

#### `make kernel-rebuild`
Equivalent to `make kernel-clean && make kernel`.

### Run Targets

#### `make run`
Builds the kernel (if needed) and launches QEMU. Boots directly to the interactive shell.

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -smp cores=4 \
    -m 1G \
    -nographic \
    -kernel build/kernel/slmos.elf
```

**QEMU Controls:**
- `Ctrl-A X` - Exit QEMU
- `Ctrl-A C` - Switch to QEMU monitor
- `Ctrl-A H` - Show help

#### `make shell`
Same as `make run`. Builds the kernel and launches QEMU with the interactive shell:

```
SLM-OS Shell - Type 'help' for commands
slm> help
Available commands:
  help     - Show this help message
  mem      - Show memory statistics
  tasks    - List all tasks
  ...
slm>
```

Use `Ctrl-A X` to exit QEMU when done.

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
Builds a special test kernel (with `ENABLE_BOOT_TESTS` flag) and runs it in QEMU. The test kernel runs all test suites at boot and exits via semihosting with an appropriate exit code. Designed for CI/CD pipelines.

```bash
make test                    # Run tests (uses kernel-test build)
make kernel-test             # Build test kernel only
make kernel-test-clean       # Clean test kernel build
```

The test runner:
1. Builds `build/kernel-test/slmos.elf` with `ENABLE_BOOT_TESTS=ON`
2. Launches QEMU with `-semihosting` flag
3. QEMU exits automatically when tests complete (via semihosting exit)
4. Returns exit code 0 on success, 1 on failure

Output on success:
```
Test Results:
=============
PASSED - All tests passed (exit code 0)
```

Output on failure:
```
Test Results:
=============
FAILED - Test failures detected:
  [FAIL] test_something
```

Test output is saved to `build/test-output.log`.
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

---

## Rust Runtime

The Rust runtime provides higher-level components that link with the C kernel.

### Building the Runtime

```bash
# Build runtime only (debug)
make runtime

# Build runtime only (release, with LTO)
make BUILD_TYPE=Release runtime

# Clean runtime build
make runtime-clean

# Clean and rebuild runtime
make runtime-rebuild
```

Note: `make kernel` automatically builds the runtime first, so explicit `make runtime` is rarely needed.

### Direct Cargo Build

Build directly with Cargo (useful for IDE integration):

```bash
cd runtime
cargo build                    # Debug build
cargo build --release          # Release build
```

### Build Output

| Build Type | Output Path |
|------------|-------------|
| Debug | `runtime/target/aarch64-unknown-none/debug/libslm_runtime.a` |
| Release | `runtime/target/aarch64-unknown-none/release/libslm_runtime.a` |

### Runtime Structure

```
runtime/
├── Cargo.toml              # Crate configuration
├── .cargo/
│   └── config.toml         # Cross-compilation settings
└── src/
    ├── lib.rs              # Entry points (rust_init, rust_hello)
    └── kernel_ffi.rs       # FFI declarations and safe wrappers
```

### Dependencies

| Crate | Purpose |
|-------|---------|
| `linked_list_allocator` | Heap allocator for `no_std` |
| `bitflags` | Type-safe flag enums |

### Troubleshooting Rust Builds

**Target not installed:**
```bash
rustup target add aarch64-unknown-none
```

**Cargo not found (Cygwin/Git Bash):**
Add to `~/.bash_profile`:
```bash
export PATH="/c/Users/<username>/.cargo/bin:$PATH"
```

**Build fails with linker error:**
Ensure ARM GNU Toolchain is in PATH (provides `aarch64-none-elf-gcc` linker).

**Hard link warning on Google Drive:**
```
warning: hard linking files in the incremental compilation cache failed
```
This is harmless - Cargo falls back to copying. Can be ignored.

---

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

### Optional Features

```bash
make kernel AI_SCHED=ON               # AI scheduler (MLP/PPO, kernel-side, C)
make kernel                          # eviction enabled by default (LRU)
make kernel EVICTION_DEFAULT_POLICY=lfu
make kernel EVICTION_MODELS=ON        # trained XGBoost + int8 MLP policies
make kernel DISABLE_EVICTION=ON       # compile eviction out entirely
```

`EVICTION_MODELS=ON` implies eviction remains enabled. Before enabling
`EVICTION_MODELS`, run `./scripts/import_eviction_weights.sh` to
stage the generated weight files from the sibling `slm-os-page-sim`
project. See `docs/eviction.md` for the full workflow.

#### Networking + SSH flags

```bash
make kernel ENABLE_NETWORKING=ON      # default for QEMU_VIRT/X86_64/RASPI5/JETSON_ORIN_NANO
make kernel NET_TELNETD_AUTOSTART=ON  # unauthenticated telnet on port 2323 at boot
make kernel NET_SSHD=ON               # vendored wolfSSH 1.4.18 + wolfCrypt — port 2222
```

`NET_SSHD=ON` pulls in the vendored wolfSSH / wolfCrypt sources
(~205K LOC) and the SLM-OS-side glue (`kernel/net/ssh/`). It is
default ON on `RASPI5` + `JETSON_ORIN_NANO` lab images and OFF
elsewhere. `NET_SSHD_AUTOSTART` (default ON for those two platforms,
OFF elsewhere) brings the daemon up at boot; the bootstrap gate
makes that safe — the daemon refuses every login until at least one
user has been provisioned via the console-side `adduser` shell
verb. Full design + threat model: `docs/ssh.md` + `docs/security.md`.

```bash
make kernel NET_SSHD=ON NET_SSHD_AUTOSTART=ON      # opt in on QEMU
make kernel NET_SSHD=ON NET_SSHD_DEMO_ALLOW_ALL=ON # debugging: skip password auth
```

`NET_SSHD_DEMO_ALLOW_ALL` is for development only — it replaces
the scrypt-against-passwd check with a stub that accepts every
login. `sshd_autostart` prints a loud WARNING on every boot when
this is wired.

### Compiler Flags

Defined in `CMakeLists.txt`. Uses **C23 standard** with strict warnings:

```cmake
# C23 standard (for __VA_OPT__, nullptr, etc.)
-std=c23

# Strict warnings (all treated as errors)
-Wall -Wextra -Wpedantic -Werror
-Wshadow -Wcast-align -Wstrict-prototypes
-Wformat=2 -Wunused -Wunreachable-code

# Freestanding environment
-ffreestanding -nostdlib

# Target CPU
-mcpu=cortex-a78ae       # ARM Cortex-A78AE (Jetson Orin Nano)
-mgeneral-regs-only      # No FPU in kernel code (except context switch)
```

The codebase compiles with zero warnings at maximum strictness.

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

### Memory: `1G`

1GB RAM (scaled for model testing). The VMM maps all RAM at boot, supporting:
- 256MB weight pool + 128MB workspace pool = 384MB for model memory
- Remaining RAM for kernel, tasks, and test allocations

Can be adjusted in `Makefile`:

```bash
QEMU_MEMORY := 512M  # Smaller for faster tests
QEMU_MEMORY := 2G    # Larger if needed
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
├── version.txt              # Single source of truth for SLMOS_VERSION
├── cmake/
│   ├── toolchain-aarch64-none-elf.cmake
│   └── gen_build_info.cmake # Generates build_info.h every build
├── kernel/
│   ├── kernel.ld            # Linker script
│   ├── include/             # Header files
│   │   └── slm_ffi.h        # FFI declarations for Rust
│   ├── src/                 # C and assembly sources
│   │   └── slm_ffi.c        # FFI implementations
│   └── drivers/             # Hardware drivers
├── runtime/                 # Rust runtime
│   ├── Cargo.toml           # Rust crate configuration
│   ├── .cargo/
│   │   └── config.toml      # Cross-compilation settings
│   └── src/
│       ├── lib.rs           # Runtime entry points
│       └── kernel_ffi.rs    # Rust FFI bindings
├── build/                   # Build output (generated)
│   ├── kernel/              # Normal kernel build
│   │   ├── include/
│   │   │   └── build_info.h # Generated: VERSION + UTC stamp + git SHA
│   │   ├── slmos.elf        # Kernel ELF (boots to shell)
│   │   └── slmos.bin        # Raw binary
│   ├── kernel-test/         # Test kernel build (ENABLE_BOOT_TESTS)
│   │   └── slmos.elf        # Test kernel ELF (runs tests, exits)
│   └── test-output.log      # Test output from last `make test`
└── docs/                    # Documentation
    └── ffi.md               # FFI documentation
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

## Building for Jetson Orin Nano

For deployment to real Jetson hardware, additional steps are required.

### Prerequisites

- Jetson Orin Nano Developer Kit
- SD card (32GB+ recommended) with JetPack flashed
- USB-to-UART adapter (3.3V TTL, connected to J14 header)
- Serial terminal (PuTTY, minicom, or screen)

### Build for Jetson

The same kernel binary works for both QEMU and Jetson:

```bash
make kernel
```

Output files:
- `build/kernel/slmos.elf` - For debugging (with symbols)
- `build/kernel/slmos.bin` - Raw binary (alternative format)

### Deploy to Jetson

1. **Mount the Jetson SD card** on a host machine

2. **Copy kernel to boot partition:**
   ```bash
   sudo cp build/kernel/slmos.elf /media/user/boot/boot/slmos.elf
   ```

3. **Edit extlinux.conf:**
   ```bash
   sudo nano /media/user/boot/boot/extlinux/extlinux.conf
   ```

   Add an entry:
   ```
   LABEL slmos
       MENU LABEL SLM-OS
       LINUX /boot/slmos.elf
       FDT /boot/tegra234-p3768-0000+p3767-0000.dtb
       APPEND console=ttyTCU0,115200
   ```

4. **Boot the Jetson** and select SLM-OS from the boot menu

### Serial Console Setup

Connect to the Jetson debug UART:

| Jetson J14 Pin | USB-UART Adapter |
|----------------|------------------|
| Pin 6 (GND)    | GND              |
| Pin 8 (TX)     | RX               |
| Pin 10 (RX)    | TX               |

Serial settings: **115200 baud, 8N1**

```bash
# Linux/macOS
screen /dev/ttyUSB0 115200

# Windows (PuTTY)
# Select COM port, 115200 baud
```

### Platform Differences

| Feature | QEMU virt | Jetson Orin Nano |
|---------|-----------|------------------|
| UART | PL011 (0x09000000) | UARTA NS16550 (0x03100000)† |
| Timer | ARM Generic Timer | ARM Generic Timer |
| GIC | GIC-400 (v2) | GICv3 |
| CPUs | 4 × Cortex-A76 | 6 × Cortex-A78AE |
| RAM | 1GB (default) | 8GB |
| GPU | None (stub) | Ampere (1024 CUDA cores) |

**†Note:** The USB-C debug port (TCU/ttyTCU0) does not work after kexec or for bare-metal code. Use UARTA via the 40-pin GPIO header (pins 6/8/10) for SLM-OS serial output.

See:
- `docs/jetson-boot.md` — Detailed boot process documentation
- `docs/platform-abstraction.md` — Platform differences and abstraction strategy
- `docs/jetson-tcu.md` — Why TCU doesn't work for bare-metal

---

*Last updated: December 2025*
