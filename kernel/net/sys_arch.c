/**
 * lwIP System Architecture Implementation for SLM-OS
 *
 * Provides the minimal OS abstraction layer for lwIP in NO_SYS mode.
 * sys_now() for timeout handling, sys_arch_protect/unprotect for
 * critical sections, and lwip_rand_slm() for pseudo-randomness.
 */

#include "arch/cc.h"
#include "arch/sys_arch.h"
#include "lwipopts.h"   /* LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT for the build-time guard */
#include "timer.h"
#include "spinlock.h"
#include "smp.h"      /* cpu_id() for the core-lock check */
#include "debug.h"    /* WARN */

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

    /* Convert to milliseconds: count * 1000 / freq.
     *
     * Precision: we divide first (`freq / 1000`) to avoid the
     * `count * 1000` overflow on large counts, but this rounds the
     * divisor down. On QEMU with cntfrq = 62.5 MHz, `freq / 1000` =
     * 62500 (exact). On x86-64 the calibrated TSC frequency may not
     * be a multiple of 1000 (e.g. 3499999000 Hz → 3499999 / 1000 =
     * 3499 — the integer division loses ~0.03% precision per ms).
     * lwIP timeouts are coarse (50-500 ms typical), so the drift is
     * negligible. If finer precision is needed for a future
     * latency-sensitive path, switch to `(count * 1000) / freq`
     * after bounding `count < UINT64_MAX / 1000` (≈5800 years at
     * 1 GHz, so the bound is benign).
     *
     * Truncate to uint32_t — wraps after ~49 days, fine for timeouts. */
    uint64_t ms = count / (freq / 1000);

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

void lwip_rand_seed(const void *bytes, uint32_t len) {
    if (!bytes || len == 0) return;
    /* Fold every byte into rand_state with a multiply-then-xor mix. The
     * underlying generator is a non-cryptographic LCG, so this isn't a
     * security boundary — the goal is just to make the boot-time state
     * depend on firmware-supplied entropy (KASLR seed, RNG seed) instead
     * of a hardcoded constant. Better than `0x12345678` for TCP ISN /
     * ephemeral-port unpredictability across reboots. */
    const uint8_t *p = (const uint8_t *)bytes;
    for (uint32_t i = 0; i < len; i++) {
        rand_state = rand_state * 1664525u + 1013904223u;
        rand_state ^= (uint32_t)p[i] << ((i & 3) * 8);
    }
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
 * Today every lwIP-touching task is explicitly pinned to CPU 0
 * via either `task_set_affinity(t, 0)` or `task->cpu_affinity = 0`
 * at task creation (net_pump, shell, shell-tcp<N>); shell-side
 * lwIP entrypoints (cmd_ping, cmd_telnetd, cmd_net) inherit the
 * pinning from the shell task. Grep for those two anchors to find
 * every site if you ever need to extend the set.
 *
 * If a future task creates pcbs or pbufs from another CPU, this
 * protection silently degrades to nothing and the heap race comes
 * back. Add a real cross-CPU lock (or pin the new task to CPU 0)
 * before doing that. The build-time guard below documents the
 * required lwipopts.h flag.
 *
 * lwIP core lock (task-vs-net_pump serialisation).
 *
 * The heap protection above covers only mem/memp allocator state.
 * TCP pcb state (unsent/unacked segment queues, listener lists,
 * timers) has no allocator-level guard, and the raw API is not
 * reentrant. Since #1047 unmasked IRQs at task entry, a timer IRQ
 * can suspend a task mid-tcp_write on every ARM64 build (QEMU, Pi 5,
 * Jetson with JETSON_HW_TICK) and dispatch net_pump, which then runs
 * tcp input/timers against the same pcb.
 *
 * Rule: every lwIP raw-API call from task context (tcp_*, udp_*,
 * raw_*, dns_*, netif_*, httpc_*) and net_poll's core-processing
 * sections must run between lwip_core_lock()/lwip_core_unlock().
 * The lock is irq_save-based, so it nests (flags round-trip) and is
 * safe under lwIP-callback recursion (sshd on_accept → tcp_write).
 * Code already inside spin_lock_irqsave holds it implicitly — IRQs
 * are masked either way. It relies on the same CPU-0 pinning
 * invariant as the heap protection and serialises NOTHING across
 * CPUs; a non-pinned lwIP caller needs a real spinlock instead.
 *
 * Do NOT hold it across anything slow or blocking from task context:
 * wolfSSH/scrypt crypto, sleep_ms, yield. Wrap only the lwIP calls
 * themselves (see wolf_io_send in kernel/net/ssh/sshd.c for the
 * pattern). Callbacks lwIP itself runs under the pump's guard
 * (e.g. net_http's file-writing recv callback) are the accepted
 * long-hold exception. */
#if !defined(LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT) || \
    (LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT != 1)
#error "lwIP heap protection requires LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT=1 in kernel/include/lwipopts.h — without it sys_arch_protect() is never called and the heap races. See the lwipopts.h comment for rationale."
#endif

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

/**
 * Acquire the coarse lwIP core lock (see the locking-rule comment
 * block above). Same mechanism as sys_arch_protect, kept as a
 * distinct named pair so core-lock sites are greppable and the two
 * protection scopes can diverge if a cross-CPU caller ever appears.
 *
 * @return Token to pass to lwip_core_unlock
 */
sys_prot_t lwip_core_lock(void) {
    return irq_save();
}

/**
 * Release the coarse lwIP core lock.
 *
 * @param token Value returned by lwip_core_lock
 */
void lwip_core_unlock(sys_prot_t token) {
    irq_restore(token);
}

/* ---- Core-lock checking (#1065) ------------------------------------ */

static volatile bool lwip_core_check_armed;
static volatile uint32_t lwip_core_violations;

#define LWIP_CORE_VIOLATION_WARN_LIMIT 4u

void lwip_core_check_arm(bool armed)
{
    lwip_core_check_armed = armed;
}

uint32_t lwip_core_violation_count(void)
{
    return lwip_core_violations;
}

static inline bool lwip_irqs_masked(void)
{
    irq_flags_t flags;
#if defined(PLATFORM_X86_64)
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    return (flags & (1u << 9)) == 0;      /* RFLAGS.IF clear */
#else
    __asm__ volatile("mrs %0, daif" : "=r"(flags) :: "memory");
    return (flags & (1u << 7)) != 0;      /* DAIF.I set */
#endif
}

/**
 * LWIP_ASSERT_CORE_LOCKED() hook. Counts, and WARNs on the first few,
 * raw-API calls made without the core lock (IRQs unmasked) or off
 * CPU 0. Deliberately not fatal: a missed call site is a race, not a
 * guaranteed crash, and the counter is what tests and `netstat`-style
 * diagnostics check.
 */
void lwip_core_assert_locked(void)
{
    if (!lwip_core_check_armed) {
        return;
    }
    uint32_t cpu = cpu_id();
    if (lwip_irqs_masked() && cpu == 0) {
        return;
    }
    uint32_t n = ++lwip_core_violations;
    if (n <= LWIP_CORE_VIOLATION_WARN_LIMIT) {
        WARN("lwIP raw API called without lwip_core_lock (cpu %u, irqs %s, caller %p)",
             (unsigned)cpu, lwip_irqs_masked() ? "masked" : "UNMASKED",
             __builtin_return_address(0));
    }
}
