# Installing Build Tools on Ubuntu

This guide covers installing the basic build tools required for SLM-OS development on Ubuntu/Linux.

---

## Prerequisites

- Ubuntu 22.04 LTS or newer
- `sudo` access
- Internet connection

---

## Step 1: Update Package Lists

```bash
sudo apt update
```

---

## Step 2: Install Build Essentials

The `build-essential` meta-package includes GCC, G++, make, and other essential build tools:

```bash
sudo apt install -y build-essential
```

This installs:
- `gcc` / `g++` - GNU C/C++ compilers (for host tools if needed)
- `make` - Build automation tool
- `libc6-dev` - C standard library headers
- `dpkg-dev` - Debian package development tools

---

## Step 3: Install CMake

```bash
sudo apt install -y cmake
```

---

## Step 4: Install Git

```bash
sudo apt install -y git
```

---

## Step 5: Install Additional Useful Tools

These are optional but recommended:

```bash
sudo apt install -y \
    ninja-build \
    gdb-multiarch \
    curl \
    wget
```

| Package | Purpose |
|---------|---------|
| `ninja-build` | Faster build system (alternative to make) |
| `gdb-multiarch` | GDB with multi-architecture support (for ARM debugging) |
| `curl` | HTTP client (needed for rustup) |
| `wget` | File downloader |

---

## Combined Command

Install everything at once:

```bash
sudo apt update && sudo apt install -y \
    build-essential \
    cmake \
    git \
    ninja-build \
    gdb-multiarch \
    curl \
    wget
```

---

## Verification

Verify all tools are installed:

```bash
# GCC
gcc --version

# Make
make --version

# CMake
cmake --version

# Git
git --version

# GDB (optional)
gdb-multiarch --version
```

Expected output examples:

```
gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0
GNU Make 4.3
cmake version 3.22.1
git version 2.34.1
GNU gdb (Ubuntu 12.1-0ubuntu1~22.04) 12.1
```

---

## Notes for SLM-OS Development

- The native `gcc` is only used for building host tools (if any)
- Cross-compilation uses `aarch64-none-elf-gcc` (see [ARM64 Toolchain Setup](setup-arm64-toolchain.md))
- CMake generates build files using the toolchain file
- `make` orchestrates the build process

---

*Last updated: December 2025*
