# SLM-OS Development Environment Setup Guide

This master guide lists all setup documentation in the order they should be performed.

---

## Prerequisites

Before starting, ensure you have:

- Windows 10/11
- Administrator access
- Internet connection
- CLion installed (with valid license)

---

## Setup Order

Complete these guides in order:

### 1. ARM Cross-Compilation Toolchain and CLion

**File:** [setup-clion-arm64-toolchain.md](setup-clion-arm64-toolchain.md)

Installs:
- `aarch64-none-elf-gcc` (ARM bare-metal cross-compiler)
- CMake toolchain file configuration
- CLion CMake profile setup

### 2. Windows CMake for Cygwin

**File:** [setup-cmake-windows.md](setup-cmake-windows.md)

Installs:
- Windows-native CMake
- Cygwin PATH configuration to use Windows CMake

**Why:** Cygwin's CMake passes Cygwin-style paths that Windows compilers don't understand.

### 3. Cygwin Terminal for CLion

**File:** [setup-clion-cygwin.md](setup-clion-cygwin.md)

Configures:
- Cygwin as CLion's terminal
- Bash profile with PATH for all tools
- Project auto-navigation

**Why:** Provides Unix-like environment with `make` and bash.

### 4. Rust Toolchain

**File:** [setup-rust-toolchain.md](setup-rust-toolchain.md)

Installs:
- Rust via rustup
- `aarch64-unknown-none` target (bare-metal ARM64)
- `rust-src` component

### 5. QEMU for ARM64 Emulation

**File:** [setup-qemu-arm64.md](setup-qemu-arm64.md)

Installs:
- QEMU ARM64 system emulator
- Configuration for the `virt` machine

### 6. Claude Code Git Integration (Optional)

**File:** [setup-claude-code-git.md](setup-claude-code-git.md)

Configures:
- Claude Code permissions for Git operations
- Controlled commit workflow

---

## Verification Checklist

After completing all guides, verify your setup:

```bash
# ARM toolchain
aarch64-none-elf-gcc --version

# CMake (should be Windows version)
which cmake
cmake --version

# Rust
rustc --version
rustup target list --installed | grep aarch64

# QEMU
qemu-system-aarch64 --version

# Make
make --version
```

---

## Build Test

From the project directory:

```bash
# Show available targets
make help

# Check all tools are found
make check-tools

# Build the kernel (will fail until source files are added)
make kernel
```

---

## Tool Summary

| Tool | Source | Purpose |
|------|--------|---------|
| `aarch64-none-elf-gcc` | ARM (Windows) | Cross-compiler for ARM64 |
| `cmake` | Kitware (Windows) | Build system generator |
| `make` | Cygwin | Build orchestration |
| `bash` | Cygwin | Shell environment |
| `rustc` / `cargo` | rustup (Windows) | Rust compiler and package manager |
| `qemu-system-aarch64` | QEMU (Windows) | ARM64 emulator |

---

## Directory Structure After Setup

```
CS-496-SLM-Operating-System/
├── .claude/
│   ├── settings.json          # Shared Claude Code settings
│   └── settings.local.json    # Personal Claude Code settings
├── cmake/
│   └── toolchain-aarch64-none-elf.cmake
├── docs/
│   └── setup/                 # Setup guides
│       ├── SETUP-GUIDE.md     # This file
│       ├── setup-clion-arm64-toolchain.md
│       ├── setup-cmake-windows.md
│       ├── setup-clion-cygwin.md
│       ├── setup-rust-toolchain.md
│       ├── setup-qemu-arm64.md
│       └── setup-claude-code-git.md
├── runtime/
│   ├── Cargo.toml
│   ├── .cargo/
│   │   └── config.toml
│   └── src/
│       └── lib.rs
├── CMakeLists.txt
├── Makefile
├── README.md
└── TODO.md
```

---

## Next Steps

After environment setup is complete, proceed to Month 1 development tasks in `TODO.md`:

1. Boot process research
2. Write `boot.S` (entry point)
3. Create linker script
4. Implement UART driver
5. Physical memory management
6. Simple scheduler

---

*Last updated: December 2025*
