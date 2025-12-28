# Installing Windows CMake for Cygwin Development

This guide covers installing Windows-native CMake and configuring it to work with Cygwin.

---

## Why Windows CMake?

When using Cygwin as your shell, you might assume Cygwin's CMake (`/usr/bin/cmake`) would work. However:

| CMake Version | Path Style | Works with Windows ARM Compiler? |
|---------------|------------|----------------------------------|
| Cygwin CMake | `/cygdrive/c/...` | No - compiler can't find files |
| Windows CMake | `C:\...` | Yes |

The ARM cross-compiler is a native Windows executable that doesn't understand Cygwin paths. Windows CMake passes Windows-style paths that the compiler understands.

---

## Prerequisites

- Cygwin installed and configured as CLion terminal
- ARM toolchain installed

---

## Step 1: Install Windows CMake

### Option A: Using winget (Recommended)

**Important:** Run this from PowerShell or CMD, **not** from Cygwin. `winget` is a native Windows tool that hangs or fails in Cygwin.

1. Open a **PowerShell** window (Windows key → type "PowerShell")
2. Run:
   ```powershell
   winget install Kitware.CMake
   ```
3. Return to Cygwin after installation completes

### Option B: Download Installer

1. Go to https://cmake.org/download/
2. Download the Windows x64 Installer (`.msi`)
3. Run the installer
4. **Important:** Select "Add CMake to the system PATH for all users" during installation

### Option C: Using Chocolatey

```powershell
choco install cmake
```

---

## Step 2: Verify Windows Installation

Open a **new** PowerShell window and run:

```powershell
cmake --version
```

Expected output:

```
cmake version 3.28.0 (or similar)
```

Note the installation path, typically:

```
C:\Program Files\CMake\bin\cmake.exe
```

---

## Step 3: Configure Cygwin to Use Windows CMake

Edit your Cygwin `~/.bash_profile`:

```bash
nano ~/.bash_profile
```

Add this line **before** any Cygwin paths:

```bash
# Use Windows CMake (required for ARM cross-compilation)
export PATH="/cygdrive/c/Program Files/CMake/bin:$PATH"
```

Your PATH exports should be ordered with Windows tools first:

```bash
# Windows tools (must come before Cygwin to override)
export PATH="/cygdrive/c/Program Files/CMake/bin:$PATH"
export PATH="/cygdrive/c/Program Files/ArmGNUToolchain/13.3.rel1/aarch64-none-elf/bin:$PATH"
export PATH="/cygdrive/c/Program Files/qemu:$PATH"

# Project auto-navigation
if [ -d "${PROJECT_DIR}" ]; then
    cd "$PROJECT_DIR"
fi
```

---

## Step 4: Reload and Verify

Reload your profile:

```bash
source ~/.bash_profile
```

Verify you're using Windows CMake:

```bash
which cmake
```

Expected output:

```
/cygdrive/c/Program Files/CMake/bin/cmake
```

**Not** `/usr/bin/cmake` (that's Cygwin's CMake).

Verify version:

```bash
cmake --version
```

---

## Step 5: Test the Build

Clean any previous failed attempts:

```bash
make clean
```

Or manually remove the build directory:

```bash
rm -rf build/kernel
```

Run the build:

```bash
make kernel
```

You should see CMake configure successfully without path errors.

---

## How It Works

The build chain now looks like this:

```
Cygwin bash (your shell)
    │
    └── Cygwin make (from Makefile)
            │
            └── Windows cmake (generates build files)
                    │
                    └── Windows aarch64-none-elf-gcc (compiles)
```

- **Cygwin make** can invoke Windows executables and translates paths as needed
- **Windows cmake** receives and passes Windows-style paths
- **Windows gcc** receives Windows paths it understands

---

## Troubleshooting

### Still seeing `/cygdrive/` path errors

Verify which cmake is being used:

```bash
which cmake
```

If it shows `/usr/bin/cmake`, your PATH order is wrong. Windows CMake must come before Cygwin paths.

### "cmake: command not found"

1. Verify CMake is installed: Open PowerShell and run `cmake --version`
2. Check the installation path matches what's in your `.bash_profile`
3. Reload profile: `source ~/.bash_profile`

### CMake runs but compiler still fails

Check that the ARM toolchain is also Windows-native and in PATH:

```bash
which aarch64-none-elf-gcc
```

Should show a `/cygdrive/c/...` path to the Windows installation.

### Cygwin CMake was uninstalled but still being used

Check if cmake is cached in Cygwin:

```bash
hash -r  # Clear command hash table
which cmake
```

---

## Optional: Remove Cygwin CMake

If you have Cygwin CMake installed and want to avoid confusion:

1. Run Cygwin Setup (`setup-x86_64.exe`)
2. Search for `cmake`
3. Change from "Keep" to "Uninstall"
4. Complete the setup

This ensures only Windows CMake is available.

---

## Summary

| Item | Value |
|------|-------|
| Installation | `winget install Kitware.CMake` |
| Typical path | `C:\Program Files\CMake\bin\cmake.exe` |
| Cygwin PATH | `/cygdrive/c/Program Files/CMake/bin` |
| Verify | `which cmake` should show Windows path |

---

*Last updated: December 2025*
