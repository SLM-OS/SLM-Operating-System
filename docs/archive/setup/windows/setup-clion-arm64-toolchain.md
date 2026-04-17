# Setting Up CLion for ARM64 Cross-Compilation

This guide covers installing the ARM64 bare-metal toolchain and configuring CLion for SLM-OS kernel development.

---

## Overview

Cross-compilation means compiling code on one platform (Windows x86_64) to run on another (ARM64 bare-metal). This requires:

1. A cross-compiler toolchain (`aarch64-none-elf-gcc`)
2. A CMake toolchain file to tell CMake how to use it
3. CLion configuration to use that toolchain file

---

## Prerequisites

- Windows 10/11 (or Linux/macOS)
- CLion installed (with valid license)
- Administrator access (for installation)

---

## Part 1: Install the ARM64 Bare-Metal Toolchain

### Step 1: Download the Toolchain

1. Go to ARM's official download page:
   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads

2. Find the **AArch64 bare-metal target (aarch64-none-elf)** section

3. Download the Windows installer:
   - Look for: `arm-gnu-toolchain-<version>-mingw-w64-i686-aarch64-none-elf.exe`
   - Or the `.zip` version if you prefer manual installation

### Step 2: Install the Toolchain

**Using the installer:**

1. Run the downloaded `.exe`
2. Follow the installation wizard
3. Note the installation path (e.g., `C:\Program Files\ArmGNUToolchain\13.3.rel1\aarch64-none-elf`)
4. **Important:** Check the option to add to PATH if available

**Using the ZIP:**

1. Extract to a permanent location (e.g., `C:\ArmGNUToolchain\aarch64-none-elf`)
2. Manually add to PATH (see Step 3)

### Step 3: Add to PATH (if not done automatically)

1. Press `Win + R`, type `sysdm.cpl`, press Enter
2. Go to **Advanced** tab → **Environment Variables**
3. Under **User variables** or **System variables**, find **Path**
4. Click **Edit** → **New**
5. Add the `bin` folder path:
   ```
   C:\Program Files\ArmGNUToolchain\13.3.rel1\aarch64-none-elf\bin
   ```
6. Click **OK** to close all dialogs

### Step 4: Verify Installation

Open a **new** terminal (PowerShell or Command Prompt) and run:

```powershell
aarch64-none-elf-gcc --version
```

Expected output:

```
aarch64-none-elf-gcc (Arm GNU Toolchain 13.3.Rel1) 13.3.1 20240614
Copyright (C) 2023 Free Software Foundation, Inc.
```

Also verify related tools:

```powershell
aarch64-none-elf-ld --version
aarch64-none-elf-objcopy --version
aarch64-none-elf-gdb --version
```

---

## Part 2: Toolchain Naming Explained

### Why `aarch64-none-elf` and not `aarch64-linux-gnu`?

| Toolchain | Target | Use Case |
|-----------|--------|----------|
| `aarch64-none-elf` | Bare-metal (no OS) | Building your own OS, bootloaders, firmware |
| `aarch64-linux-gnu` | Linux userspace | Apps running on Linux, or Linux kernel itself |

Since SLM-OS is a standalone operating system (not Linux-based), we use `aarch64-none-elf`.

### Toolchain Name Components

```
aarch64-none-elf
   │     │    │
   │     │    └── Executable format (ELF)
   │     └─────── Operating system (none = bare-metal)
   └───────────── Architecture (64-bit ARM)
```

---

## Part 3: CMake Toolchain File

A toolchain file tells CMake how to use the cross-compiler instead of the native compiler.

### Location

The project includes a toolchain file at:

```
cmake/toolchain-aarch64-none-elf.cmake
```

### Contents

```cmake
# CMake toolchain file for AArch64 bare-metal cross-compilation
# Target: ARM Cortex-A78AE (Jetson Orin Nano) / Cortex-A76 (Raspberry Pi 5)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler toolchain
set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
set(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
set(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)

# Bare-metal flags
set(CMAKE_C_FLAGS_INIT "-ffreestanding -nostdlib")
set(CMAKE_CXX_FLAGS_INIT "-ffreestanding -nostdlib -fno-exceptions -fno-rtti")
set(CMAKE_ASM_FLAGS_INIT "")

# Prevent CMake from trying to link a test executable (will fail without libc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search paths - don't look in host system directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Objcopy for creating binary images
set(CMAKE_OBJCOPY aarch64-none-elf-objcopy)
```

### Key Settings Explained

| Setting | Purpose |
|---------|---------|
| `CMAKE_SYSTEM_NAME Generic` | Tells CMake this is bare-metal (no OS) |
| `CMAKE_SYSTEM_PROCESSOR aarch64` | Target architecture |
| `-ffreestanding` | Don't assume hosted environment |
| `-nostdlib` | Don't link standard library |
| `-fno-exceptions` | Disable C++ exceptions (need runtime support) |
| `-fno-rtti` | Disable C++ RTTI (need runtime support) |
| `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` | Prevents compiler test from failing |

### Using Absolute Paths (Alternative)

If you don't want to modify PATH, use absolute paths in the toolchain file:

```cmake
set(CMAKE_C_COMPILER "C:/Program Files/ArmGNUToolchain/13.3.rel1/aarch64-none-elf/bin/aarch64-none-elf-gcc.exe")
set(CMAKE_CXX_COMPILER "C:/Program Files/ArmGNUToolchain/13.3.rel1/aarch64-none-elf/bin/aarch64-none-elf-g++.exe")
set(CMAKE_ASM_COMPILER "C:/Program Files/ArmGNUToolchain/13.3.rel1/aarch64-none-elf/bin/aarch64-none-elf-gcc.exe")
```

---

## Part 4: Configure CLion

### Step 1: Open the Project

1. Launch CLion
2. **File → Open**
3. Select the project folder containing `CMakeLists.txt`
4. CLion will detect the CMake project

### Step 2: Configure CMake Profile

1. Go to **File → Settings** (or `Ctrl+Alt+S`)
2. Navigate to **Build, Execution, Deployment → CMake**
3. You'll see a default profile (usually "Debug")

Configure the profile:

| Setting | Value |
|---------|-------|
| **Name** | `Debug-ARM64` (or any name you prefer) |
| **Build type** | `Debug` |
| **CMake options** | `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake` |
| **Build directory** | `build/debug-arm64` (optional, keeps builds organized) |

4. Click **Apply**

### Step 3: Create Release Profile (Optional)

1. In the same CMake settings, click **+** to add a profile
2. Configure:

| Setting | Value |
|---------|-------|
| **Name** | `Release-ARM64` |
| **Build type** | `Release` |
| **CMake options** | `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake` |
| **Build directory** | `build/release-arm64` |

3. Click **Apply** then **OK**

### Step 4: Reload CMake Project

1. In the **CMake** tool window (usually at the bottom), click the **Reload** button
2. Or right-click `CMakeLists.txt` → **Reload CMake Project**

### Step 5: Verify Configuration

Check the CMake output in the **CMake** tool window. You should see:

```
-- SLM-OS Build Configuration
--   Version:    0.1.0
--   C Compiler: C:/Program Files/ArmGNUToolchain/.../aarch64-none-elf-gcc.exe
--   Build Type: Debug
```

If you see errors about the compiler not being found, verify:

1. The toolchain is installed correctly
2. The PATH is set (restart CLion after PATH changes)
3. The toolchain file path is correct

---

## Part 5: Building

### From CLion

1. Select your CMake profile from the dropdown in the toolbar
2. **Build → Build Project** (or `Ctrl+F9`)

### From Command Line

```powershell
cd "H:\My Drive\Capstone\CS-496-SLM-Operating-System"
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake
cmake --build build
```

---

## Troubleshooting

### "CMAKE_C_COMPILER is not a full path and was not found in the PATH"

The toolchain is not installed or not in PATH.

1. Verify installation: `aarch64-none-elf-gcc --version`
2. If command not found, check PATH environment variable
3. Restart CLion after modifying PATH (full restart, not just Invalidate Caches)

### "No CMAKE_C_COMPILER could be found"

Same as above. Ensure the toolchain `bin` directory is in PATH.

### CLion doesn't pick up PATH changes

CLion inherits PATH from its launch environment. On Windows:

1. Close CLion completely
2. Open a new terminal and verify: `aarch64-none-elf-gcc --version`
3. Launch CLion from that same terminal, or restart Windows Explorer

### CMake configuration fails with "compiler test failed"

The toolchain file should have:

```cmake
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

This prevents CMake from trying to link a test executable (which fails without libc).

### "cannot find -lgcc"

This usually means the toolchain installation is incomplete. Re-download and reinstall.

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

---

## Additional Resources

- [ARM GNU Toolchain Documentation](https://developer.arm.com/documentation/102433/latest/)
- [CMake Toolchains Documentation](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)
- [CLion CMake Documentation](https://www.jetbrains.com/help/clion/cmake-support.html)

---

*Last updated: December 2025*
