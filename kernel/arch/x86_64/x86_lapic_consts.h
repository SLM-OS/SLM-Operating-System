/*
 * x86_lapic_consts.h — LAPIC + LAPIC-timer constants shared between
 * the LAPIC driver, the timer wrapper that drives it, and the test
 * suite that asserts invariants on both. Kept in arch/x86_64 because
 * none of these values mean anything outside x86; included from
 * kernel/tests/ via the per-platform include path added in
 * CMakeLists.txt for PLATFORM=X86_64.
 *
 * Add new shared LAPIC constants here rather than re-declaring them
 * locally and chasing drift with "keep in sync" comments — the whole
 * point of this file is to make a sync requirement a compile-time
 * fact rather than a documentation hope.
 */
#ifndef SLMOS_ARCH_X86_64_LAPIC_CONSTS_H
#define SLMOS_ARCH_X86_64_LAPIC_CONSTS_H

#include "platform.h"

#if defined(PLATFORM_X86_64)

/* Bounded busy-wait budget for PIT calibration paths. ~200 ms at
 * 3 GHz; comfortably longer than the legitimate ~10 ms PIT window
 * but short enough that a kexec-disabled PIT can't hang boot. Used
 * by lapic_timer_calibrate (lapic.c) and timer_init's TSC fallback
 * (timer_x86.c). */
#define TSC_PIT_TIMEOUT_CYCLES   600000000ULL

/* LAPIC timer interrupt vector — must not collide with IOAPIC
 * vectors (32-47). timer_x86.c programs the LVT to deliver here;
 * test_x86_boot.c asserts the LVT carries this exact value
 * post-init. Re-numbering here is the single edit needed across
 * the producer + consumer + tests. */
#define LAPIC_TIMER_VECTOR       48

#endif  /* PLATFORM_X86_64 */

#endif  /* SLMOS_ARCH_X86_64_LAPIC_CONSTS_H */
