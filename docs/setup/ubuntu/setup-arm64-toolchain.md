# Installing ARM64 Bare-Metal Toolchain on Ubuntu

This guide covers installing the ARM64 bare-metal cross-compiler toolchain for SLM-OS kernel development on Ubuntu/Linux.

---

## Prerequisites

- Ubuntu 22.04 LTS or newer
- Build tools installed (see [setup-build-tools.md](setup-build-tools.md))
- Internet connection

---

## Why `aarch64-none-elf` and not `aarch64-linux-gnu`?

| Toolchain | Target | Use Case |
|-----------|--------|----------|
| `aarch64-none-elf` | Bare-metal (no OS) | Building an OS, bootloaders, firmware |
| `aarch64-linux-gnu` | Linux userspace | Apps running on Linux, or Linux kernel |

Since SLM-OS is a standalone operating system (not Linux-based), the bare-metal toolchain is required.

**Note:** Ubuntu's repositories include `gcc-aarch64-linux-gnu`, but this is NOT suitable for bare-metal development. The ARM GNU Toolchain from ARM's website must be used instead.

---

## Step 1: Download the Toolchain

### Option A: Download from ARM Website (Recommended)

1. Go to ARM's official download page:
   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads

2. Find the **AArch64 bare-metal target (aarch64-none-elf)** section

3. Download the Linux x86_64 tarball:
   - Look for: `arm-gnu-toolchain-<version>-x86_64-aarch64-none-elf.tar.xz`
   - Example: `arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz`

### Option B: Direct Download via Command Line

```bash
# Check the ARM website for the latest version URL
# Example for version 13.3.rel1:
cd ~/Downloads
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz
```

---

## Step 2: Extract the Toolchain

Extract to `/opt` (recommended system-wide location):

```bash
sudo mkdir -p /opt/arm-gnu-toolchain
sudo tar -xf arm-gnu-toolchain-*-x86_64-aarch64-none-elf.tar.xz -C /opt/arm-gnu-toolchain --strip-components=1
```

Or extract to a versioned directory for multiple versions:

```bash
sudo tar -xf arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf.tar.xz -C /opt/
# This creates: /opt/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf/
```

---

## Step 3: Add to PATH

Add the toolchain to the system PATH. Choose one method:

### Option A: System-wide (all users)

Create a profile script:

```bash
sudo tee /etc/profile.d/arm-toolchain.sh > /dev/null << 'EOF'
# ARM GNU Toolchain for bare-metal development
export PATH="/opt/arm-gnu-toolchain/bin:$PATH"
EOF
```

Then reload:

```bash
source /etc/profile.d/arm-toolchain.sh
```

### Option B: Current user only (Recommended)

Add to `~/.bashrc` along with Rust/Cargo:

```bash
echo 'export PATH="/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

This single line adds both the ARM toolchain and Rust toolchain to the PATH.

Or add to `~/.profile` for login shells:

```bash
echo 'export PATH="/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH"' >> ~/.profile
source ~/.profile
```

---

## Step 4: Verify Installation

```bash
aarch64-none-elf-gcc --version
```

Expected output:

```
aarch64-none-elf-gcc (Arm GNU Toolchain 13.3.Rel1 (Build arm-13.24)) 13.3.1 20240614
Copyright (C) 2023 Free Software Foundation, Inc.
```

Verify related tools:

```bash
aarch64-none-elf-ld --version
aarch64-none-elf-objcopy --version
aarch64-none-elf-gdb --version
```

---

## Step 5: Create Symbolic Link (Optional)

For easier version management, create a symbolic link:

```bash
# If you extracted to a versioned directory:
sudo ln -sf /opt/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-elf /opt/arm-gnu-toolchain
```

This allows updating the toolchain by changing only the symlink.

---

## CMake Toolchain File

The project includes a toolchain file at:

```
cmake/toolchain-aarch64-none-elf.cmake
```

This file tells CMake how to use the cross-compiler. Key settings:

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
set(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
set(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)

set(CMAKE_C_FLAGS_INIT "-ffreestanding -nostdlib")
set(CMAKE_CXX_FLAGS_INIT "-ffreestanding -nostdlib -fno-exceptions -fno-rtti")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

No changes are needed if the toolchain is in PATH.

---

## Toolchain Name Components

```
aarch64-none-elf
   │     │    │
   │     │    └── Executable format (ELF)
   │     └─────── Operating system (none = bare-metal)
   └───────────── Architecture (64-bit ARM)
```

---

## Toolchain Components Reference

| Tool | Purpose |
|------|---------|
| `aarch64-none-elf-gcc` | C compiler |
| `aarch64-none-elf-g++` | C++ compiler |
| `aarch64-none-elf-as` | Assembler |
| `aarch64-none-elf-ld` | Linker |
| `aarch64-none-elf-objcopy` | Convert ELF to binary |
| `aarch64-none-elf-objdump` | Disassembler |
| `aarch64-none-elf-nm` | List symbols |
| `aarch64-none-elf-size` | Show section sizes |
| `aarch64-none-elf-gdb` | Debugger |
| `aarch64-none-elf-readelf` | ELF file analyzer |

---

## Troubleshooting

### "aarch64-none-elf-gcc: command not found"

The toolchain is not in PATH.

1. Verify the installation location:
   ```bash
   ls /opt/arm-gnu-toolchain/bin/aarch64-none-elf-gcc
   ```

2. Check PATH:
   ```bash
   echo $PATH | tr ':' '\n' | grep arm
   ```

3. Reload shell configuration:
   ```bash
   source ~/.bashrc
   # or start a new terminal
   ```

### "cannot execute binary file: Exec format error"

Downloaded the wrong architecture. Ensure the x86_64 Linux version is used (not AArch64 host or Windows).

### Missing shared libraries

Install required 32-bit libraries if using an older toolchain:

```bash
sudo apt install -y lib32z1 lib32ncurses6
```

For newer toolchains, these may be needed:

```bash
sudo apt install -y libncurses5
```

### Permission denied

Ensure the toolchain binaries are executable:

```bash
sudo chmod +x /opt/arm-gnu-toolchain/bin/*
```

---

## Building the Project

Once the toolchain is installed:

```bash
cd /path/to/CS-496-SLM-Operating-System

# Build the kernel
make kernel

# Build and run in QEMU
make run
```

---

## Additional Resources

- [ARM GNU Toolchain Documentation](https://developer.arm.com/documentation/102433/latest/)
- [ARM GNU Toolchain Downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
- [CMake Toolchains Documentation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)

---

*Last updated: December 2025*
