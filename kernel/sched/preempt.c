/*
 * preempt.c - Secondary-CPU preemption support
 *
 * Implements maybe_arm_resched_trampoline(): the C-side of the ELR
 * trampoline scheme used to defer schedule() calls from timer IRQs to
 * task context. See docs/archive/investigations/pi5-secondary-cpu-preemption.md (issue #57)
 * for the design rationale.
 *
 * The assembly side (resched_trampoline) lives in vectors.S.
 */

#include "preempt.h"

#if defined(SECONDARY_PREEMPT)

#include "ncmem.h"
#include "smp.h"
#include "trap.h"
#include "debug.h"
#include "config.h"
#include "cpu_id_asm.h"
#include "uart.h"
#include <stdint.h>

/* Tie ARM64_MAX_CPUS_LITERAL (used by the asm macro in cpu_id_asm.h)
 * to MAX_CPUS so a future change surfaces at compile time. */
_Static_assert(ARM64_MAX_CPUS_LITERAL == MAX_CPUS,
               "ARM64_MAX_CPUS_LITERAL out of sync with MAX_CPUS — "
               "update kernel/include/cpu_id_asm.h");


/* Per-CPU state, allocated from NC memory in preempt_init() so writes
 * from one CPU are immediately visible to any CPU that samples them
 * (no DSU-level coherency on Pi 5 without SMPEN). */
volatile uint32_t *reschedule_pending;
volatile uint64_t *orig_elr;
volatile uint64_t *orig_spsr;

/* PSR mode bits [3:0]. Kernel-mode sources we're willing to preempt:
 *   0x5 = EL1h (EL1 using SP_EL1) — QEMU + Jetson + x86 path
 *   0x9 = EL2h (EL2 using SP_EL2) — Pi 5 EL2/VHE path (#683)
 * EL0t (0x0) is user-mode and isn't covered here — EL0 preemption
 * doesn't need the trampoline because the kernel-stack frame on EL0
 * → EL2 trap is fresh and switch_to from there is safe. */
#define PSR_MODE_EL1H   0x5
#define PSR_MODE_EL2H   0x9
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

    /* NC allocation failure is non-recoverable: maybe_arm_resched_trampoline
     * and the asm trampoline both unconditionally dereference these
     * pointers on every timer IRQ, so a NULL would take down the first
     * preempted task with an unrecoverable fault. Fail early with a
     * clear panic message instead. */
    if (!reschedule_pending || !orig_elr || !orig_spsr) {
        panic("preempt_init: ncmem_alloc failed (pending=%p elr=%p spsr=%p, "
              "ncmem used=%lu/%lu)",
              reschedule_pending, orig_elr, orig_spsr,
              (unsigned long)ncmem_used(), (unsigned long)NC_MEM_SIZE);
    }

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        reschedule_pending[i] = 0;
        orig_elr[i]  = 0;
        orig_spsr[i] = 0;
    }
}

/*
 * Mirror of the ARM64_GET_LOGICAL_CPU asm macro
 * (kernel/include/cpu_id_asm.h) used by the resched trampoline. Both
 * resolve through cpu_logical_map[] so they agree on every platform
 * — Pi 5 Aff1, QEMU Aff0, Jetson dual-cluster (#647).
 *
 * Two layers of behavior, important to keep straight:
 *
 *   - `cpu_logical_id()` (in <smp.h>) returns -1 on miss. That's the
 *     value `preempt_check_cpu_mpidr` below relies on to detect a
 *     map-not-populated configuration error and panic loudly at boot.
 *   - This wrapper clamps -1 to 0 so its return type matches the asm
 *     macro's `dst` register convention (the trampoline has no
 *     signed-sentinel slot in its calling sequence). The asm macro's
 *     miss-returns-0 convention is documented in cpu_id_asm.h.
 *
 * Net effect: the trampoline path silently misroutes to slot 0 if it
 * ever sees an unmapped MPIDR, but the boot-time invariant check
 * forecloses that possibility on every CPU we ever schedule on. Any
 * future caller that needs to distinguish miss from "CPU 0" must call
 * `cpu_logical_id()` directly.
 */
uint32_t preempt_trampoline_cpu_for_mpidr(uint64_t mpidr)
{
    int cpu = cpu_logical_id(mpidr);
    return (cpu >= 0) ? (uint32_t)cpu : 0u;
}

/*
 * #137: sanity-check the per-CPU invariant
 * "trampoline_cpu_id(MPIDR) == logical_cpu_id" at boot. With the
 * cpu_logical_map[]-based lookup (#647) this can fail only when the
 * map is not populated for this CPU — a configuration bug, not the
 * old fold-collision class.
 */
void preempt_check_cpu_mpidr(uint32_t this_cpu)
{
    uint64_t mpidr = cpu_get_mpidr();
    int looked_up = cpu_logical_id(mpidr);

    if (looked_up < 0) {
        panic("SECONDARY_PREEMPT: cpu_logical_map[] has no entry for "
              "MPIDR=0x%lx (logical cpu %u). cpu_logical_map must be "
              "populated for every CPU before SECONDARY_PREEMPT can run.",
              (unsigned long)mpidr, (unsigned)this_cpu);
    }
    if ((uint32_t)looked_up != this_cpu) {
        panic("SECONDARY_PREEMPT: cpu_logical_map[%d] = MPIDR=0x%lx but "
              "logical cpu reported as %u — map and bring-up sequence "
              "disagree.",
              looked_up, (unsigned long)mpidr, (unsigned)this_cpu);
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
    /* Pre-preempt_init bail (#750).
     *
     * preempt_init() — which allocates reschedule_pending / orig_elr /
     * orig_spsr from NC memory — runs inside scheduler_init(). Under
     * JETSON_HW_TICK=ON + SECONDARY_PREEMPT=ON, IRQs are unmasked by
     * the `msr daifclr, #0xf` at the tail of mmu_enable (kernel/arch/
     * arm64/mmu.S), which is reached from vmm_init() — long before
     * scheduler_init() runs. Any IRQ delivered in that window (a
     * pending xudc IRQ 198 inherited from Linux is the canonical
     * trigger on Jetson) takes the el1_irq vector path, which under
     * SECONDARY_PREEMPT calls `bl maybe_arm_resched_trampoline`.
     * Without this guard, the original `if (!reschedule_pending[cpu])`
     * dereffed a NULL pointer; the resulting page-table walk for VA 0
     * hit DFSC=0x17 (synchronous external abort, level-3 TTW) which
     * BL31's RAS handler catches and powers off the core.
     *
     * The window is real on every ARM64 platform that takes IRQs
     * before scheduler_init — Jetson is the one that exercises it
     * because Linux leaves xudc enabled across kexec. Pi 5 boots
     * via VC firmware and clears the GIC distributor, so it never
     * hits this case in practice; the guard is still a no-cost
     * structural fix everywhere.
     *
     * No reschedule can be pending pre-preempt_init by construction,
     * so the bail is correct. The one-shot uart_printf surfaces the
     * fact that IRQs are firing pre-init (useful diagnostic for any
     * future regression in the boot sequence) without flooding the
     * console under an IRQ storm. */
    if (!reschedule_pending) {
        static volatile int warned = 0;
        if (!__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED)) {
            uart_printf("[preempt] IRQ taken pre-preempt_init "
                        "(elr=0x%lx spsr=0x%lx) — bail\n",
                        (unsigned long)tf->elr, (unsigned long)tf->spsr);
        }
        return;
    }

    uint32_t cpu = cpu_id();

    if (!reschedule_pending[cpu])
        return;
    if (preempt_disabled[cpu])
        return;
    /* Accept either EL1h or EL2h — kernel-mode source. EL0t and
     * other unexpected modes fall through and are not preempted. */
    uint64_t mode = tf->spsr & PSR_MODE_MASK;
    if (mode != PSR_MODE_EL1H && mode != PSR_MODE_EL2H)
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

#endif /* SECONDARY_PREEMPT */
