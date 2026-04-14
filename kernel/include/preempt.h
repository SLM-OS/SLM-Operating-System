/*
 * preempt.h - Secondary-CPU preemption support
 *
 * Exposes the per-CPU state used by the deferred-scheduling exception
 * return path. scheduler_tick() sets reschedule_pending[cpu] instead of
 * calling schedule() directly; vectors.S checks the flag on IRQ return
 * and, when set, points ELR_EL1 at resched_trampoline so schedule()
 * runs in task context rather than inside the exception handler.
 *
 * Only active when SECONDARY_PREEMPT is defined (ARM64 hardware where
 * schedule-from-ISR corrupts the exception frame — Pi 5 today, Jetson
 * once P1.0 / P3 enable it). On QEMU the old direct-schedule-from-ISR
 * path still works and no trampoline is needed.
 */

#ifndef PREEMPT_H
#define PREEMPT_H

#include <stdint.h>
#include "config.h"

#if defined(SECONDARY_PREEMPT)

/*
 * Per-CPU "reschedule pending" flag. Set by scheduler_tick() when the
 * timer IRQ decides a context switch is warranted. Cleared by the
 * trampoline-arming path in maybe_arm_resched_trampoline(), and by
 * schedule() after a voluntary call (so a stale flag from a prior tick
 * does not spuriously arm the trampoline on the next IRQ).
 *
 * Allocated from NC memory in scheduler_init() for cross-CPU visibility.
 */
extern volatile uint32_t *reschedule_pending;

/*
 * Per-CPU saved ELR_EL1 / SPSR_EL1 at the point the trampoline was
 * armed. resched_trampoline reloads these when synthesizing its final
 * eret to the originally interrupted PC.
 *
 * Single writer per CPU (the CPU being preempted); single reader on the
 * same CPU (the trampoline). No cross-CPU race, but placed in NC memory
 * for simplicity and debuggability.
 */
extern volatile uint64_t *orig_elr;
extern volatile uint64_t *orig_spsr;

/* Assembly entry point for the deferred-scheduling trampoline. */
void resched_trampoline(void);

/*
 * C helper called from el1_irq (after el1_irq_handler). Inspects
 * reschedule_pending[cpu] and, if set, rewrites tf->elr to point at
 * resched_trampoline so the upcoming eret lands in the trampoline.
 */
struct trap_frame;
void maybe_arm_resched_trampoline(struct trap_frame *tf);

/* Initialize per-CPU preemption state. Called from scheduler_init(). */
void preempt_init(void);

#else /* !SECONDARY_PREEMPT */

static inline void preempt_init(void) {}

#endif

#endif /* PREEMPT_H */
