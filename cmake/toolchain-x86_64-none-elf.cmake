# CMake toolchain file for x86-64 bare-metal
#
# Uses the system GCC compiler with freestanding flags.
# Unlike ARM64 which needs a cross-compiler, x86-64 can use
# the host GCC with appropriate flags.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Use the system GCC compiler
# On x86-64 Linux, we can compile for bare-metal using -ffreestanding
set(CMAKE_C_COMPILER gcc)
set(CMAKE_ASM_COMPILER gcc)
set(CMAKE_OBJCOPY objcopy)
set(CMAKE_OBJDUMP objdump)

# Don't try to compile test programs (we're targeting bare-metal)
set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Force static linking only
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# x86-64 specific flags. Ubuntu/Debian host GCC enables
# -D_FORTIFY_SOURCE=2 by default at -O1+, which rewrites
# snprintf/memcpy/etc. into __snprintf_chk/__memcpy_chk calls. Those
# symbols live in glibc, which we don't link against
# (-ffreestanding -nostdlib). Force fortify off so the freestanding
# build doesn't accidentally pull in host libc shims.
set(CMAKE_C_FLAGS_INIT "-m64 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0")
set(CMAKE_ASM_FLAGS_INIT "-m64")
