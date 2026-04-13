/*
 * preempt.c - Secondary-CPU preemption support (Pi 5)
 *
 * Implements maybe_arm_resched_trampoline(): the C-side of the ELR
 * trampoline scheme used to defer schedule() calls from timer IRQs to
 * task context. See docs/pi5-secondary-cpu-preemption.md (issue #57)
 * for the design rationale.
 *
 * The assembly side (resched_trampoline) lives in vectors.S.
 */

#include "preempt.h"

#if defined(PI5_SECONDARY_PREEMPT)

#include "ncmem.h"
#include "smp.h"
#include "trap.h"
#include "config.h"
#include <stdint.h>


/* Per-CPU state, allocated from NC memory in preempt_init() so writes
 * from one CPU are immediately visible to any CPU that samples them
 * (no DSU-level coherency on Pi 5 without SMPEN). */
volatile uint32_t *reschedule_pending;
volatile uint64_t *orig_elr;
volatile uint64_t *orig_spsr;

/* PSR mode bits [3:0]. 0x5 == EL1h (EL1 using SP_EL1). */
#define PSR_MODE_EL1H   0x5
#define PSR_MODE_MASK   0xF

/* PSR.DAIF.I bit — IRQ mask. Set in the SPSR we hand back to `eret` so
 * the trampoline starts with IRQs masked, preventing a nested timer
 * from re-arming over the same task's exception state. The trampoline
 * unmasks IRQs itself after saving caller-clobbered registers. */
#define PSR_I           (1UL << 7)

extern volatile int preempt_disabled[MAX_CPUS];

void preempt_init(void)
{
    reschedule_pending = ncmem_alloc(MAX_CPUS * sizeof(uint32_t), 64);
    orig_elr  = ncmem_alloc(MAX_CPUS * sizeof(uint64_t), 64);
    orig_spsr = ncmem_alloc(MAX_CPUS * sizeof(uint64_t), 64);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        reschedule_pending[i] = 0;
        orig_elr[i]  = 0;
        orig_spsr[i] = 0;
    }
}

/*
 * Called from el1_irq after el1_irq_handler returns, before restore_regs.
 *
 * If a reschedule is pending on this CPU, rewrite the trap frame's ELR
 * so the upcoming `eret` lands in resched_trampoline instead of the
 * originally interrupted PC. resched_trampoline runs in task context
 * (post-eret), calls schedule(), then synthesizes a second eret back
 * to the original PC using orig_elr/orig_spsr.
 *
 * Bail out silently if:
 *   - no reschedule pending
 *   - a context switch is already in progress on this CPU
 *     (preempt_disabled would otherwise let us clobber ELR mid-switch)
 *   - the interrupted context was not EL1h (we don't preempt EL0
 *     userspace components here yet)
 */
void maybe_arm_resched_trampoline(struct trap_frame *tf)
{
    uint32_t cpu = cpu_id();

    if (!reschedule_pending[cpu])
        return;
    if (preempt_disabled[cpu])
        return;
    if ((tf->spsr & PSR_MODE_MASK) != PSR_MODE_EL1H)
        return;

    /* Consume the flag and stash the original exception return state
     * for resched_trampoline to replay. orig_spsr is the interrupted
     * context's PSTATE (which may have had IRQs unmasked — e.g. the
     * idle task does `daifclr + wfi`). */
    reschedule_pending[cpu] = 0;
    orig_elr[cpu]  = tf->elr;
    orig_spsr[cpu] = tf->spsr;

    /* Redirect eret into the trampoline and force DAIF.I=1 in the
     * SPSR that `eret` will install. If we left tf->spsr unchanged,
     * the trampoline would start running with IRQs unmasked (since the
     * interrupted idle task had them unmasked) — a nested timer IRQ
     * before the trampoline's register-save prologue would corrupt
     * x0-x18 and re-arm the trampoline over our saved ELR/SPSR.
     *
     * The trampoline unmasks IRQs itself after it has saved registers
     * and set preempt_disabled[cpu]=1. */
    tf->elr  = (uint64_t)&resched_trampoline;
    tf->spsr |= PSR_I;
}

#endif /* PI5_SECONDARY_PREEMPT */
