/*
 * x86_smp.h — x86-64 SMP bring-up state shared with the test suite.
 */
#ifndef SLMOS_ARCH_X86_64_SMP_H
#define SLMOS_ARCH_X86_64_SMP_H

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>

/* Number of APs that entered ap_entry_64 with RSP % 16 != 8 (the SysV
 * function-entry alignment). Defined in platform_x86.c and checked by
 * test_x86_boot.c as a regression test for #237. */
extern volatile uint32_t x86_ap_entry_misaligned;

#endif /* PLATFORM_X86_64 */

#endif /* SLMOS_ARCH_X86_64_SMP_H */
