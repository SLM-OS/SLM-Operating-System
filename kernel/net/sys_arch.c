/**
 * lwIP System Architecture Implementation for SLM-OS
 *
 * Provides the minimal OS abstraction layer for lwIP in NO_SYS mode.
 * sys_now() for timeout handling, sys_arch_protect/unprotect for
 * critical sections, and lwip_rand_slm() for pseudo-randomness.
 */

#include "arch/cc.h"
#include "arch/sys_arch.h"
#include "lwipopts.h"   /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT for the build-time assert */
#include "timer.h"
#include "spinlock.h"

/**
 * Get current system time in milliseconds
 *
 * Used by lwIP for timeout management. Uses the platform's
 * timer_get_count()/timer_get_frequency() which works on both
 * ARM64 (CNTPCT_EL0) and x86-64 (RDTSC).
 *
 * @return Current time in milliseconds since boot
 */
uint32_t sys_now(void) {
    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();

    /* Convert to milliseconds: count * 1000 / freq */
    /* Do division first to avoid overflow on count * 1000 */
    uint64_t ms = count / (freq / 1000);

    /* Truncate to uint32_t - wraps after ~49 days, fine for timeouts */
    return (uint32_t)ms;
}

/**
 * Random number generator for lwIP
 *
 * Used for TCP initial sequence numbers, ephemeral ports, etc.
 * Uses the timer count as an entropy source combined with a
 * simple LCG for pseudo-randomness.
 *
 * NOTE: This is NOT cryptographically secure.
 *
 * @return 32-bit pseudo-random number
 */
static uint32_t rand_state = 0x12345678;

uint32_t lwip_rand_slm(void) {
    /* Mix in timer count for entropy */
    uint64_t timer = timer_get_count();
    rand_state ^= (uint32_t)timer;
    rand_state ^= (uint32_t)(timer >> 32);

    /* Linear Congruential Generator (Numerical Recipes) */
    rand_state = rand_state * 1664525u + 1013904223u;

    return rand_state;
}

/* -------------------------------------------------------------------------- */
/* Critical Section Protection                                                 */
/* -------------------------------------------------------------------------- */

/* sys_prot_t aliases irq_flags_t (both uint64_t) so the protection
 * token round-trips without truncation. Asserted at build time so a
 * future cc.h change cannot silently re-introduce the int-narrowing
 * bug it replaced. */
_Static_assert(sizeof(sys_prot_t) == sizeof(irq_flags_t),
               "sys_prot_t must hold a full irq_flags_t — see cc.h");

/* Single-CPU pinning invariant.
 *
 * `irq_save()` masks IRQs on the LOCAL CPU only. The lwIP heap
 * protection enabled by LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT=1
 * relies on every lwIP caller running on the same CPU so that
 * task-vs-task and task-vs-IRQ are both serialised.
 *
 * Today every lwIP-touching task is explicitly pinned to CPU 0:
 *   - net_pump          → kernel/src/main.c
 *   - shell             → kernel/src/shell.c
 *   - shell-tcp<N>      → kernel/src/tcp_shell_server.c
 *   - cmd_ping / cmd_telnetd / cmd_net all run on the shell task
 *
 * If a future task creates pcbs or pbufs from another CPU, this
 * protection silently degrades to nothing and the heap race comes
 * back. Add a real cross-CPU lock (or pin the new task to CPU 0)
 * before doing that. The build-time assert below documents the
 * required lwipopts.h flag. */
_Static_assert(LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT == 1,
               "lwIP heap protection requires LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT=1 — "
               "without it sys_arch_protect() is never called and the heap races. "
               "See kernel/include/lwipopts.h for the flag and the rationale.");

/**
 * Enter a critical section (protect against concurrent access).
 *
 * Disables interrupts and returns the previous interrupt state.
 * This prevents ISRs from re-entering lwIP while a network
 * operation is in progress.
 *
 * @return Protection value to pass to sys_arch_unprotect
 */
sys_prot_t sys_arch_protect(void) {
    return irq_save();
}

/**
 * Leave a critical section.
 *
 * @param pval Value returned by sys_arch_protect
 */
void sys_arch_unprotect(sys_prot_t pval) {
    irq_restore(pval);
}
