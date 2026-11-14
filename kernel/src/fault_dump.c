/*
 * fault_dump.c - Serialized exception and panic dumps.
 *
 * See fault_dump.h for the contract. Run 4b of the switch gauntlet lost
 * two Jetson boots to dumps from two CPUs interleaved character by
 * character on the UART; this keeps concurrent dumps apart and labels
 * every dump line with its CPU.
 */

#include "fault_dump.h"
#include "config.h"
#include "smp.h"
#include "spinlock.h"
#include "timer.h"
#include "uart.h"
#include <stdbool.h>
#include <stdint.h>

static_assert(MAX_CPUS <= 32, "fault_dump_cpus is a 32-bit CPU mask");

volatile uint32_t fault_dump_cpus;

static struct fault_dump_lock dump_lock;
static volatile uint32_t dumps_completed;

/* Per-CPU dump state, touched only by its own CPU. */
static bool dump_line_start[MAX_CPUS];
static bool dump_uart_held[MAX_CPUS];
static fault_dump_entry_t dump_entry[MAX_CPUS];

#if defined(FAULT_DUMP_SELFTEST)
volatile uint32_t fault_dump_selftest_stop_cpu;
static uint32_t selftest_lines;
#endif

/* kprintf.c: hold the UART lock across a dump (bounded wait). */
extern bool uart_lock_try_hold(uint64_t wait_ticks, uint64_t (*now)(void));
extern void uart_lock_hold_release(void);

/* A counter that stops moving (x86-64's PIT fallback with IRQs off)
 * would make a tick-bounded wait unbounded: after this many polls with
 * no change, give up as if the cap had expired. */
#define FAULT_DUMP_FROZEN_POLLS 1000000u

static inline void dump_cpu_relax(void)
{
#if defined(PLATFORM_X86_64)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("yield" ::: "memory");
#endif
}

static uint32_t dump_cpu(void)
{
    uint32_t me = cpu_id();
    return me < MAX_CPUS ? me : 0;
}

fault_dump_entry_t fault_dump_lock_acquire(struct fault_dump_lock *l,
                                           uint32_t me,
                                           uint64_t stall_ticks,
                                           uint64_t cap_ticks,
                                           uint64_t (*now)(void))
{
    const uint32_t want = me + 1u;
    if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) == want) {
        return FAULT_DUMP_NESTED;
    }

    const uint64_t start = now();
    uint32_t seen_owner = __atomic_load_n(&l->owner, __ATOMIC_RELAXED);
    uint64_t seen_progress = l->progress;
    uint64_t seen_at = start;
    uint64_t last_now = start;
    uint32_t frozen = 0;

    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&l->owner, &expected, want, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            l->progress = now();
            return FAULT_DUMP_ACQUIRED;
        }

        const uint64_t t = now();
        const uint64_t p = l->progress;
        /* Any sign of life restarts the silence timer: a new owner, or the
         * owner printing another line. */
        if (expected != seen_owner || p != seen_progress) {
            seen_owner = expected;
            seen_progress = p;
            seen_at = t;
        }
        if (t - seen_at >= stall_ticks) {
            return FAULT_DUMP_STALLED;
        }
        if (t - start >= cap_ticks) {
            return FAULT_DUMP_CAPPED;
        }
        if (t == last_now) {
            if (++frozen >= FAULT_DUMP_FROZEN_POLLS) {
                return FAULT_DUMP_CAPPED;
            }
        } else {
            frozen = 0;
            last_now = t;
        }
        dump_cpu_relax();
    }
}

void fault_dump_lock_release(struct fault_dump_lock *l, uint32_t me)
{
    uint32_t want = me + 1u;
    (void)__atomic_compare_exchange_n(&l->owner, &want, 0u, false,
                                      __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static uint64_t dump_now(void)
{
    return timer_get_count();
}

fault_dump_entry_t fault_dump_begin(void)
{
    const uint32_t me = dump_cpu();
    const uint32_t bit = 1u << me;

    if (fault_dump_cpus & bit) {
        return FAULT_DUMP_NESTED;
    }

    const bool hw = spinlock_hw_enabled != 0;
    const uint64_t freq = timer_get_frequency();
    fault_dump_entry_t entry;
    uint32_t holder = 0;

    if (!hw || freq == 0) {
        entry = FAULT_DUMP_NOLOCK;
    } else {
        holder = __atomic_load_n(&dump_lock.owner, __ATOMIC_RELAXED);
        entry = fault_dump_lock_acquire(&dump_lock, me,
                                        freq * FAULT_DUMP_STALL_MS / 1000u,
                                        freq * FAULT_DUMP_CAP_MS / 1000u,
                                        dump_now);
        if (entry != FAULT_DUMP_ACQUIRED) {
            uint32_t h = __atomic_load_n(&dump_lock.owner, __ATOMIC_RELAXED);
            if (h != 0) {
                holder = h;
            }
        }
    }

    dump_entry[me] = entry;
    dump_line_start[me] = true;
    dump_uart_held[me] = false;
    if (hw) {
        __atomic_fetch_or(&fault_dump_cpus, bit, __ATOMIC_ACQ_REL);
    } else {
        fault_dump_cpus |= bit;     /* pre-MMU: only the boot CPU runs */
    }

    /* Hold the UART lock so other CPUs' ordinary prints wait until the
     * dump ends. A CPU that faulted while holding it (or a faulted
     * holder elsewhere) costs FAULT_DUMP_UART_WAIT_MS, then the dump
     * prints without it. */
    if (entry == FAULT_DUMP_ACQUIRED) {
        dump_uart_held[me] = uart_lock_try_hold(
            freq * FAULT_DUMP_UART_WAIT_MS / 1000u, dump_now);
    }

    switch (entry) {
    case FAULT_DUMP_STALLED:
        uart_printf_unlocked("fault_dump: cpu%u holds the dump lock and printed "
                             "nothing for %u ms; dumping without it\n",
                             holder ? holder - 1u : 0u, FAULT_DUMP_STALL_MS);
        break;
    case FAULT_DUMP_CAPPED:
        uart_printf_unlocked("fault_dump: waited %u ms for the dump lock "
                             "(holder cpu%u); dumping without it\n",
                             FAULT_DUMP_CAP_MS, holder ? holder - 1u : 0u);
        break;
    case FAULT_DUMP_NOLOCK:
        uart_puts_unlocked("fault_dump: no dump lock (MMU off or no timer)\n");
        break;
    default:
        break;
    }
    return entry;
}

void fault_dump_end(void)
{
    const uint32_t me = dump_cpu();
    const uint32_t bit = 1u << me;

    if (!(fault_dump_cpus & bit)) {
        return;
    }
    /* End the last line, then mark the end of this CPU's dump in the log
     * (still prefixed) before the locks go. */
    if (!dump_line_start[me]) {
        uart_puts_unlocked("\n");
    }
    uart_puts_unlocked("fault_dump: end, releasing the dump lock\n");
    if (dump_uart_held[me]) {
        dump_uart_held[me] = false;
        uart_lock_hold_release();
    }

    const bool hw = spinlock_hw_enabled != 0;
    if (dump_entry[me] == FAULT_DUMP_ACQUIRED) {
        fault_dump_lock_release(&dump_lock, me);
    }
    if (hw) {
        __atomic_fetch_and(&fault_dump_cpus, ~bit, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&dumps_completed, 1u, __ATOMIC_RELEASE);
    } else {
        fault_dump_cpus &= ~bit;
        dumps_completed++;
    }
}

uint32_t fault_dump_completed(void)
{
    return __atomic_load_n(&dumps_completed, __ATOMIC_ACQUIRE);
}

bool fault_dump_lock_silent(const struct fault_dump_lock *l, uint64_t now,
                            uint64_t stall_ticks)
{
    if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) == 0) {
        return false;
    }
    const uint64_t progress = __atomic_load_n(&l->progress, __ATOMIC_ACQUIRE);
    /* The holder may stamp a counter value later than the caller's `now`
     * (another CPU's line ended in between): that is progress, not an
     * unsigned wrap to "silent for ages". */
    if (now < progress) {
        return false;
    }
    return now - progress >= stall_ticks;
}

bool fault_dump_holder_silent(void)
{
    const uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        return true;        /* no counter: never wait on a dump holder */
    }
    if (__atomic_load_n(&dump_lock.owner, __ATOMIC_ACQUIRE) == 0) {
        return false;
    }
    /* Read the holder's stamp before the counter, so a stamp written
     * after this point cannot be newer than `now`. */
    const uint64_t progress = __atomic_load_n(&dump_lock.progress, __ATOMIC_ACQUIRE);
    const uint64_t now = timer_get_count();
    if (now < progress) {
        return false;
    }
    return now - progress >= freq * FAULT_DUMP_STALL_MS / 1000u;
}

bool fault_dump_this_cpu(void)
{
    uint32_t mask = fault_dump_cpus;
    return mask != 0 && (mask & (1u << dump_cpu())) != 0;
}

static void dump_put_prefix(uint32_t me)
{
    uart_putc('[');
    uart_putc('c');
    uart_putc('p');
    uart_putc('u');
    if (me >= 10) {
        uart_putc((char)('0' + me / 10));
    }
    uart_putc((char)('0' + me % 10));
    uart_putc(']');
    uart_putc(' ');
}

void fault_dump_putc(char c)
{
    const uint32_t me = dump_cpu();
    if ((fault_dump_cpus & (1u << me)) == 0) {
        uart_putc(c);
        return;
    }
    if (dump_line_start[me]) {
        dump_put_prefix(me);
        dump_line_start[me] = false;
    }
    uart_putc(c);
    if (c == '\n') {
        dump_line_start[me] = true;
        /* Heartbeat for CPUs waiting on the lock. */
        if (dump_entry[me] == FAULT_DUMP_ACQUIRED) {
            dump_lock.progress = timer_get_count();
        }
#if defined(FAULT_DUMP_SELFTEST)
        if (fault_dump_selftest_stop_cpu == me + 1u &&
            ++selftest_lines >= FAULT_DUMP_SELFTEST_STOP_LINES) {
            for (;;) {
                __asm__ volatile("wfe");
            }
        }
#endif
    }
}
