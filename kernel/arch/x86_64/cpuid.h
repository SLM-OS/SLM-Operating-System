/*
 * cpuid.h — shared x86-64 CPUID inline helper.
 *
 * Both lapic.c and timer_x86.c need CPUID for their post-kexec
 * calibration paths (leaf 0x15 for LAPIC timer bus frequency, leaves
 * 0x15 + 0x16 for TSC frequency). Rather than duplicate the inline
 * asm in each translation unit, define it once here.
 *
 * No public C prototype is needed — callers just `#include "cpuid.h"`
 * and invoke `x86_cpuid(leaf, &eax, &ebx, &ecx, &edx)`.
 *
 * ECX is passed as 0 (subleaf 0). Callers that need subleaves should
 * not use this helper; the three leaves we currently care about
 * (0x01, 0x15, 0x16) don't have subleaves.
 */
#ifndef SLMOS_ARCH_X86_64_CPUID_H
#define SLMOS_ARCH_X86_64_CPUID_H

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>

static inline void x86_cpuid(uint32_t leaf,
                             uint32_t *eax, uint32_t *ebx,
                             uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

#endif  /* PLATFORM_X86_64 */

#endif  /* SLMOS_ARCH_X86_64_CPUID_H */
