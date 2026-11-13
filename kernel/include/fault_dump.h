/*
 * fault_dump.h - Serialized exception and panic dumps.
 *
 * A dump (panic(), the EL1 page-fault dump, the x86-64 exception dump,
 * the SLOT_TRACE bad-SP dump) runs between fault_dump_begin() and
 * fault_dump_end(). Inside that window:
 *
 *   - At most one CPU holds the dump lock. A second CPU that faults waits
 *     until the holder ends its dump, or until the holder has printed
 *     nothing for FAULT_DUMP_STALL_MS (default 50 ms, measured with the
 *     generic timer / TSC counter, not a loop count), or until
 *     FAULT_DUMP_CAP_MS in all; then it dumps anyway. A holder that
 *     faulted or hung mid-dump therefore delays another CPU's dump by
 *     about 50 ms at most and never blocks it.
 *   - Every line the dumping CPU prints starts with "[cpuN] ", so even a
 *     dump printed without the lock stays attributable line by line.
 *   - The dumping CPU's uart_puts()/uart_printf() skip the UART lock (a
 *     faulted holder of that lock cannot block the dump), and the dump
 *     holds the UART lock itself when it can get it within
 *     FAULT_DUMP_UART_WAIT_MS, so other CPUs' ordinary prints do not
 *     split dump lines.
 *
 * Callers run with IRQs masked. Before vmm_init sets spinlock_hw_enabled
 * (pre-MMU, only the boot CPU runs) the lock is skipped: exclusive
 * accesses to non-cacheable memory abort on Cortex-A78AE. With no timer
 * frequency yet, the wait is skipped too. Both cases still prefix lines.
 * See docs/testing.md "Fault dumps".
 */

#ifndef FAULT_DUMP_H
#define FAULT_DUMP_H

#include <stdbool.h>
#include <stdint.h>

#define FAULT_DUMP_STALL_MS      50u
#define FAULT_DUMP_CAP_MS        5000u
#define FAULT_DUMP_UART_WAIT_MS  20u

/* How this CPU entered the dump (fault_dump_begin's result). */
typedef enum {
    FAULT_DUMP_ACQUIRED = 0,    /* took the dump lock */
    FAULT_DUMP_NESTED   = 1,    /* already held it (fault inside a dump) */
    FAULT_DUMP_STALLED  = 2,    /* holder silent FAULT_DUMP_STALL_MS: dumped anyway */
    FAULT_DUMP_CAPPED   = 3,    /* waited FAULT_DUMP_CAP_MS in all: dumped anyway */
    FAULT_DUMP_NOLOCK   = 4,    /* pre-MMU or no timer: no lock taken */
} fault_dump_entry_t;

/* The lock. One instance serves every dump; tests use their own. */
struct fault_dump_lock {
    volatile uint32_t owner;        /* 0 = free, else CPU id + 1 */
    volatile uint64_t progress;     /* holder's counter value at its last line */
};

/* Start a dump on this CPU (IRQs already masked). Idempotent per CPU. */
fault_dump_entry_t fault_dump_begin(void);

/* End this CPU's dump: release the lock and UART lock if held, stop
 * prefixing. Call after the last line, right before halting. */
void fault_dump_end(void);

/* Number of dumps that reached fault_dump_end() since boot. */
uint32_t fault_dump_completed(void);

/* Lock primitive, exposed for tests. `me` is the caller's CPU id.
 * stall_ticks / cap_ticks are counter ticks; now() reads the counter. */
fault_dump_entry_t fault_dump_lock_acquire(struct fault_dump_lock *l,
                                           uint32_t me,
                                           uint64_t stall_ticks,
                                           uint64_t cap_ticks,
                                           uint64_t (*now)(void));
void fault_dump_lock_release(struct fault_dump_lock *l, uint32_t me);

/* Bitmask of CPUs currently inside a dump; nonzero only while one is. */
extern volatile uint32_t fault_dump_cpus;

/* Output hook for kprintf.c: prints c, adding the "[cpuN] " prefix at
 * line starts when this CPU is dumping. */
void fault_dump_putc(char c);

/* True when this CPU is inside a dump (kprintf.c skips the UART lock). */
bool fault_dump_this_cpu(void);

#endif /* FAULT_DUMP_H */
