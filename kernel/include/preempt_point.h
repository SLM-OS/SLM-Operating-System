/*
 * preempt_point.h - Voluntary preemption points for COOP_PREEMPT mode
 *
 * On platforms where the hardware timer IRQ does not deliver to the
 * kernel (Pi 5, Jetson — see issues #99 and #134), preemptive
 * scheduling is approximated by two cooperating mechanisms:
 *
 *   1. coop_preempt_maybe_tick() — runs from the top of schedule(),
 *      synthesizes a scheduler_tick() call when ≥10 ms has elapsed
 *      on the current CPU. Drives the AI policy, deadline boosts,
 *      and migration decisions.
 *
 *   2. slm_preempt_point() — call sites in long-running CPU-bound
 *      loops that don't otherwise hit a yielding primitive. Calls
 *      schedule() when the local quantum has expired (= same 10 ms
 *      threshold). Cheap fast-path: a CNTPCT load and one compare
 *      when the quantum has not expired.
 *
 * Together these convert SLM-OS's nominally cooperative scheduler
 * into one that can pre-empt any in-tree code path that calls
 * slm_preempt_point() at its loop back-edge. Pure busy-wait loops
 * with no preempt-point call still monopolize their CPU; in-tree
 * loops are expected to follow the policy documented in
 * docs/scheduler.md §Preemption Points.
 *
 * **DO NOT call this macro from inside a spinlock-held region.**
 * SLM-OS's `spin_lock` / `spin_lock_irqsave` (kernel/include/spinlock.h)
 * do not bump `preempt_disabled[]`; the macro would fall into
 * `schedule()` with the lock still held, and any contending task on
 * the same CPU would spin forever. Restrict call sites to lock-free
 * busy loops (computation, polling that does not run under a lock,
 * test helpers).
 *
 * No-op on platforms where COOP_PREEMPT is not defined (QEMU,
 * x86-64). Stays compile-clean there so call sites can be
 * unconditional.
 *
 * Distinct from preempt.h, which gates the SECONDARY_PREEMPT
 * ELR-trampoline infrastructure used when hardware timer IRQ
 * delivery works.
 */

#ifndef PREEMPT_POINT_H
#define PREEMPT_POINT_H

#include "config.h"

#if defined(COOP_PREEMPT)

#include <stdint.h>

/*
 * Public entry point used by the slm_preempt_point() macro. Reads
 * CNTPCT_EL0, compares against the per-CPU coop tick deadline, and
 * calls schedule() only when ≥10 ms (one TIMER_HZ period) has elapsed.
 *
 * Cost on the not-due path: ~5–8 instructions (cpu_id, freq read,
 * counter read, subtract, compare, branch). Cost on the due path:
 * full schedule() invocation, which itself runs
 * coop_preempt_maybe_tick() and possibly switch_to().
 *
 * Caller must NOT hold a spinlock — see header block above. The
 * preempt_disabled[cpu] early-out only short-circuits recursion when
 * the scheduler is already mid-context-switch; it does not legitimize
 * lock-held callers, since SLM-OS spinlocks do not bump that flag.
 */
void slm_preempt_check_and_yield(void);

/*
 * Diagnostic counters maintained by slm_preempt_check_and_yield.
 * Useful for the Track A soak test (kernel/tests/test_coop_preempt.c)
 * and for runtime diagnostics — observing both lets a test distinguish
 * "macro called many times, never fell into schedule()" (period math
 * wrong, or quantum never expired) from "macro never called at all"
 * (linker dropped the call site, COOP_PREEMPT unset, etc.).
 *
 * `slm_preempt_point_calls` increments on every call, fast or slow.
 * `slm_preempt_point_schedule_calls` increments only when the slow
 * path (full schedule()) ran. Both are plain uint64_t; cross-CPU
 * tearing is acceptable for a diagnostic counter and avoids the
 * cost of an atomic on the fast path.
 */
extern volatile uint64_t slm_preempt_point_calls;
extern volatile uint64_t slm_preempt_point_schedule_calls;

#define slm_preempt_point() slm_preempt_check_and_yield()

#else  /* !COOP_PREEMPT */

/*
 * Compile-time no-op on platforms where hardware timer IRQs deliver
 * preemption directly. The macro expansion is a void cast so call
 * sites pass strict-prototype checks even when there are no scheduler
 * symbols linked yet (e.g. early boot helpers).
 */
#define slm_preempt_point() ((void)0)

#endif  /* COOP_PREEMPT */

#endif  /* PREEMPT_POINT_H */
