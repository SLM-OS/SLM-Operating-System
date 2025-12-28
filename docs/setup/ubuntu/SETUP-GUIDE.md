# SLM-OS Development Environment Setup Guide (Ubuntu/Linux)

This master guide lists all setup documentation in the order they should be performed.

---

## Prerequisites

Before starting, ensure the following:

- Ubuntu 22.04 LTS or newer (or compatible Linux distribution)
- `sudo` access
- Internet connection

---

## Setup Order

Complete these guides in order:

### 1. Build Essentials and CMake

**File:** [setup-build-tools.md](setup-build-tools.md)

Installs:
- `build-essential` (gcc, make, etc.)
- `cmake`
- `git`

### 2. ARM Cross-Compilation Toolchain

**File:** [setup-arm64-toolchain.md](setup-arm64-toolchain.md)

Installs:
- `aarch64-none-elf-gcc` (ARM bare-metal cross-compiler)
- CMake toolchain file configuration

### 3. Rust Toolchain

**File:** [setup-rust-toolchain.md](setup-rust-toolchain.md)

Installs:
- Rust via rustup
- `aarch64-unknown-none` target (bare-metal ARM64)
- `rust-src` component

### 4. QEMU for ARM64 Emulation

**File:** [setup-qemu-arm64.md](setup-qemu-arm64.md)

Installs:
- QEMU ARM64 system emulator
- Configuration for the `virt` machine

### 5. Claude Code Git Integration (Optional)

**File:** [../setup-claude-code-git.md](../setup-claude-code-git.md)

Configures:
- Claude Code permissions for Git operations
- Controlled commit workflow

---

## Quick Install Summary

For users who want to install everything quickly, here are all the commands:

```bash
# 1. Build essentials and CMake
sudo apt update
sudo apt install -y build-essential cmake git ninja-build gdb-multiarch curl wget

# 2. QEMU
sudo apt install -y qemu-system-arm

# 3. ARM toolchain (download from ARM website)
cd ~/Downloads
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz
sudo mkdir -p /opt/arm-gnu-toolchain
sudo tar -xf arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1

# 4. Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
rustup target add aarch64-unknown-none
rustup component add rust-src

# 5. Add both toolchains to PATH permanently
echo 'export PATH="/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

---

## Verification Checklist

After completing all guides, verify the setup:

```bash
# Build tools
make --version
cmake --version
git --version

# ARM toolchain
aarch64-none-elf-gcc --version

# Rust
rustc --version
rustup target list --installed | grep aarch64

# QEMU
qemu-system-aarch64 --version
```

---

## Build Test

From the project directory:

```bash
# Show available targets
make help

# Check all tools are found
make check-tools

# Build the kernel
make kernel

# Run in QEMU
make run
```

---

## Tool Summary

| Tool | Source | Purpose |
|------|--------|---------|
| `aarch64-none-elf-gcc` | ARM (Linux) | Cross-compiler for ARM64 |
| `cmake` | apt | Build system generator |
| `make` | apt | Build orchestration |
| `bash` | System | Shell environment |
| `rustc` / `cargo` | rustup | Rust compiler and package manager |
| `qemu-system-aarch64` | apt | ARM64 emulator |

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
│   └── setup/
│       ├── setup-claude-code-git.md  # Shared (platform-independent)
│       ├── ubuntu/                    # Ubuntu/Linux guides
│       │   ├── SETUP-GUIDE.md
│       │   ├── setup-build-tools.md
│       │   ├── setup-arm64-toolchain.md
│       │   ├── setup-rust-toolchain.md
│       │   └── setup-qemu-arm64.md
│       └── windows/                   # Windows guides
│           ├── SETUP-GUIDE.md
│           └── ...
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

After environment setup is complete, proceed to development tasks in `TODO.md`.

---

*Last updated: December 2025*
