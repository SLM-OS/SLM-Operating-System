# CMake toolchain file for AArch64 bare-metal cross-compilation
# Target: ARM Cortex-A78AE (Jetson Orin Nano) / Cortex-A76 (Raspberry Pi 5)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross-compiler toolchain - detect OS and use appropriate paths
# Platform-specific configuration
if(WIN32)
    # Windows: Full path needed to find the compiler
    set(ARM_TOOLCHAIN_PATH "C:/Program Files (x86)/Arm/GNU Toolchain mingw-w64-i686-aarch64-none-elf/bin")
    set(CMAKE_C_COMPILER "${ARM_TOOLCHAIN_PATH}/aarch64-none-elf-gcc.exe")
    set(CMAKE_CXX_COMPILER "${ARM_TOOLCHAIN_PATH}/aarch64-none-elf-g++.exe")
    set(CMAKE_ASM_COMPILER "${ARM_TOOLCHAIN_PATH}/aarch64-none-elf-gcc.exe")
    set(CMAKE_OBJCOPY "${ARM_TOOLCHAIN_PATH}/aarch64-none-elf-objcopy.exe")
else()
    # Linux/macOS: Compiler should be in PATH (e.g., from GitHub Actions setup)
    set(CMAKE_C_COMPILER aarch64-none-elf-gcc)
    set(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
    set(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)
    set(CMAKE_OBJCOPY aarch64-none-elf-objcopy)
endif()

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
