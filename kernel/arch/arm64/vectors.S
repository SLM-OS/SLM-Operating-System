/*
 * vectors.S - Exception vector table for SLM-OS (ARM64)
 *
 * Handles exceptions and interrupts at EL1.
 */

.section .text

/*
 * Exception vector table.
 *
 * ARM64 requires vectors aligned to 2KB (0x800).
 * Each entry is 0x80 (128) bytes.
 *
 * There are 4 groups of 4 vectors:
 *   - Current EL with SP0 (not used, we use SPx)
 *   - Current EL with SPx (kernel exceptions/IRQs)
 *   - Lower EL using AArch64 (userspace, future)
 *   - Lower EL using AArch32 (not supported)
 */

.balign 0x800
.global exception_vectors
exception_vectors:

    /* ============================================================
     * Current EL with SP0 (EL1t) - not used
     * ============================================================ */
    .balign 0x80
    b       el1_sync_sp0        /* Synchronous */
    .balign 0x80
    b       el1_irq_sp0         /* IRQ */
    .balign 0x80
    b       el1_fiq_sp0         /* FIQ */
    .balign 0x80
    b       el1_serror_sp0      /* SError */

    /* ============================================================
     * Current EL with SPx (EL1h) - kernel mode
     * ============================================================ */
    .balign 0x80
    b       el1_sync            /* Synchronous */
    .balign 0x80
    b       el1_irq             /* IRQ - timer interrupts come here */
    .balign 0x80
    b       el1_fiq             /* FIQ */
    .balign 0x80
    b       el1_serror          /* SError */

    /* ============================================================
     * Lower EL using AArch64 (EL0) - userspace (future)
     * ============================================================ */
    .balign 0x80
    b       el0_sync            /* Synchronous */
    .balign 0x80
    b       el0_irq             /* IRQ */
    .balign 0x80
    b       el0_fiq             /* FIQ */
    .balign 0x80
    b       el0_serror          /* SError */

    /* ============================================================
     * Lower EL using AArch32 - not supported
     * ============================================================ */
    .balign 0x80
    b       hang                /* Synchronous */
    .balign 0x80
    b       hang                /* IRQ */
    .balign 0x80
    b       hang                /* FIQ */
    .balign 0x80
    b       hang                /* SError */


/* ============================================================
 * Exception handlers
 * ============================================================ */

/*
 * Macro to save all general-purpose registers.
 * Creates a trap frame on the stack.
 */
.macro save_regs
    sub     sp, sp, #272            /* Space for x0-x30, sp, elr, spsr */
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]

    /* Save ELR and SPSR */
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #248]
.endm

/*
 * Phase 1b: per-CPU, per-vector exception counter bump.
 *
 * Runs as the first instructions of every EL1/EL0 vector entry, before
 * save_regs. Preserves x16-x17 across the bump so the subsequent
 * save_regs captures the task's original register state.
 *
 * Layout (see kernel/include/diag_pi5.h):
 *   base = NC_MEM_BASE + 0xFF60
 *   per-CPU block = 32 bytes at base + cpu*0x20
 *   offsets: sync=0x00, irq=0x08, fiq=0x10, serror=0x18
 *
 * cpu_id computed inline with the MPIDR hack used throughout the
 * PI5_* paths: `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` — correct for
 * Pi 5 (Aff1=cpu) and QEMU (Aff0=cpu). Not used on Jetson builds.
 */
.macro DIAG_BUMP_VEC off:req
#if defined(PI5_IRQ_DIAG)
    stp     x16, x17, [sp, #-16]!
    mrs     x16, mpidr_el1
    ubfx    x17, x16, #8, #8
    and     x16, x16, #0xFF
    orr     x16, x16, x17                   /* x16 = logical cpu */
    mov     x17, #0xFF60
    movk    x17, #0xFFE0, lsl #16
    add     x17, x17, x16, lsl #5           /* x17 = per-CPU block base */
    ldr     x16, [x17, #\off]
    add     x16, x16, #1
    str     x16, [x17, #\off]
    ldp     x16, x17, [sp], #16
#endif
.endm

/*
 * Macro to restore all general-purpose registers.
 */
.macro restore_regs
    /* Restore ELR and SPSR */
    ldp     x0, x1, [sp, #248]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #272
.endm

/*
 * Hang - unhandled exceptions
 */
hang:
    wfi
    b       hang

/*
 * SP0 handlers (not used - we use SPx)
 */
el1_sync_sp0:
el1_irq_sp0:
el1_fiq_sp0:
el1_serror_sp0:
    b       hang

/*
 * EL1 Synchronous exception (kernel)
 * Could be: SVC, data abort, instruction abort, etc.
 */
el1_sync:
    DIAG_BUMP_VEC 0x00
    save_regs
    mov     x0, sp              /* Pass trap frame pointer */
    bl      el1_sync_handler    /* Call C handler */
    restore_regs
    eret

/*
 * EL1 IRQ (kernel) - Timer interrupts come here
 */
el1_irq:
    DIAG_BUMP_VEC 0x08
    save_regs
    bl      el1_irq_handler     /* Call C handler */
#if defined(PI5_SECONDARY_PREEMPT)
    /* After the IRQ body runs, check for a pending reschedule. When set,
     * maybe_arm_resched_trampoline() rewrites the saved ELR_EL1 in the
     * trap frame so the upcoming `eret` lands in resched_trampoline
     * rather than the originally interrupted PC. That trampoline calls
     * schedule() in task context (not exception context), avoiding the
     * Pi 5 hang caused by switch_to inside an exception handler. */
    mov     x0, sp
    bl      maybe_arm_resched_trampoline
#endif
    restore_regs
    eret

/*
 * EL1 FIQ (kernel)
 *
 * Previously `b hang` — a silent black hole. Now a real handler so we
 * can see FIQ deliveries (issue #99 Phase 1). On GICv2 with Security
 * Extensions, non-secure IGROUPR writes cannot promote Group 0 → Group 1;
 * if timer PPIs remain in Group 0, they arrive here as FIQ rather than
 * through el1_irq. The handler reads GICC_AIAR (Group 0 ACK) and
 * records the IRQ number, then EOIs.
 */
el1_fiq:
    DIAG_BUMP_VEC 0x10
    save_regs
    mov     x0, sp
    bl      el1_fiq_handler
    restore_regs
    eret

/*
 * EL1 SError (kernel)
 */
el1_serror:
    DIAG_BUMP_VEC 0x18
    save_regs
    mov     x0, sp
    bl      el1_serror_handler
    restore_regs
    eret

/*
 * EL0 handlers (userspace - Phase 5 M4)
 *
 * When an exception occurs at EL0, the CPU switches from SP_EL0 to SP_EL1.
 * SP_EL1 was set before ERET and points to the task's kernel stack.
 * save_regs saves the EL0 return state (ELR_EL1/SPSR_EL1 contain the
 * EL0 PC and PSTATE), and restore_regs + eret returns to EL0.
 */

el0_sync:
    DIAG_BUMP_VEC 0x00
    save_regs
    mov     x0, sp              /* Pass trap frame pointer */
    bl      el0_sync_handler    /* C handler: SVC dispatch or fault */
    restore_regs
    eret

el0_irq:
    DIAG_BUMP_VEC 0x08
    save_regs
    bl      el1_irq_handler     /* Reuse EL1 IRQ handler (timer preemption) */
    restore_regs
    eret

el0_fiq:
    DIAG_BUMP_VEC 0x10
    save_regs
    mov     x0, sp
    bl      el1_fiq_handler     /* Reuse EL1 FIQ handler (diag) */
    restore_regs
    eret

el0_serror:
    DIAG_BUMP_VEC 0x18
    save_regs
    mov     x0, sp
    bl      el0_serror_handler  /* C handler: terminate component */
    restore_regs
    eret

#if defined(PI5_SECONDARY_PREEMPT)
/*
 * resched_trampoline - Deferred-scheduling trampoline (Pi 5, issue #57).
 *
 * Invoked by `eret` from el1_irq when maybe_arm_resched_trampoline() has
 * rewritten the trap frame's ELR_EL1 to point here. Runs in task
 * context on the interrupted task's stack.
 *
 * Entry invariants (set by the trap-frame rewrite):
 *   - PC = resched_trampoline (via eret)
 *   - SPSR restored from orig_spsr, so DAIF.I=1 (IRQs masked) and
 *     M[3:0]=0x5 (EL1h)
 *   - SP = interrupted task's SP (SP_EL1 is not swapped on EL1->EL1)
 *   - x0-x30 = task's values at interruption (restore_regs ran before
 *     the first eret)
 *
 * Responsibilities:
 *   1. Save all caller-saved GPRs (x0-x18) and FP/LR (x29-x30) on the
 *      task's stack. x19-x28 are preserved across schedule() by the
 *      AAPCS64 call convention.
 *   2. Load orig_elr[cpu] / orig_spsr[cpu] from NC memory into stack
 *      locals so a nested preemption can't race them.
 *   3. Set preempt_disabled[cpu]=1 so maybe_arm_resched_trampoline()
 *      bails on any nested timer IRQ rather than re-arming over us.
 *   4. Unmask IRQs and bl schedule. schedule() may or may not call
 *      switch_to. If it does, we resume here on this task's stack when
 *      switched back in the future.
 *   5. Re-mask IRQs, clear preempt_disabled[cpu], restore the saved
 *      original ELR/SPSR into the system registers, restore GPRs, and
 *      `eret` to the originally interrupted PC.
 *
 * Stack frame layout (192 bytes, 16-byte aligned):
 *   [sp, #0..143]   x0-x17 (9 pairs)
 *   [sp, #144]      x18
 *   [sp, #152]      x29 (fp)
 *   [sp, #160]      x30 (lr)
 *   [sp, #168]      saved orig_elr
 *   [sp, #176]      saved orig_spsr
 *   [sp, #184]      padding
 *
 * Stack-size impact: 192 bytes per trampoline activation, on top of the
 * ~160-byte interrupt trap frame already on the task's kernel stack at
 * the point of preemption. Task stacks are `STACK_SIZE = 64 KB`
 * (kernel/include/config.h), so the trampoline consumes ~0.3% of the
 * stack — safely within budget. schedule() itself adds a small amount
 * for its own locals + rq_lock_irqsave frames, bounded and audited.
 */
.global resched_trampoline
.type resched_trampoline, @function
resched_trampoline:
    /* Save caller-clobbered registers. IRQs are still masked so we
     * cannot be re-entered here. */
    sub     sp, sp, #192
    stp     x0, x1,   [sp, #0]
    stp     x2, x3,   [sp, #16]
    stp     x4, x5,   [sp, #32]
    stp     x6, x7,   [sp, #48]
    stp     x8, x9,   [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    str     x18,      [sp, #144]
    stp     x29, x30, [sp, #152]

    /* Compute logical cpu_id inline rather than calling the C `cpu_id()`
     * helper. Reasons:
     *   1. We have not yet saved x19-x28 or set up a valid frame pointer
     *      for a C call; a bl here would need extra register-save paperwork.
     *   2. The MPIDR hack `(mpidr & 0xFF) | ((mpidr >> 8) & 0xFF)` is
     *      the same one used by every other PI5_SECONDARY_PREEMPT code
     *      site — task_entry_trampoline (task.c), NC trace points
     *      (sched.c, exceptions.c, smp.c) — and is correct for Pi 5
     *      (Aff1 holds CPU index) and QEMU (Aff0 holds it). The two
     *      affinity fields are never both non-zero on these platforms.
     *   3. Jetson uses a dual-cluster layout where this would be wrong,
     *      but the whole trampoline is gated on PI5_SECONDARY_PREEMPT
     *      and is only defined for PLATFORM=RASPI5 in CMakeLists.
     *
     * If the kernel is ever ported to a CPU with more complex affinity
     * encoding while reusing this infrastructure, convert this into a
     * shared asm macro alongside the existing NC-trace uses — don't
     * silently skew one call site from the rest. */
    mrs     x0, mpidr_el1
    and     x1, x0, #0xFF
    ubfx    x2, x0, #8, #8
    orr     x0, x1, x2          /* x0 = cpu */

    /* Snapshot orig_elr[cpu] / orig_spsr[cpu] to stack locals.
     * `orig_elr` and `orig_spsr` are global pointers into NC memory
     * (assigned by preempt_init()); `adrp + ldr` loads the pointer
     * value, then we index by cpu. */
    adrp    x1, orig_elr
    ldr     x1, [x1, #:lo12:orig_elr]
    ldr     x3, [x1, x0, lsl #3]    /* x3 = orig_elr[cpu] */
    adrp    x2, orig_spsr
    ldr     x2, [x2, #:lo12:orig_spsr]
    ldr     x4, [x2, x0, lsl #3]    /* x4 = orig_spsr[cpu] */
    stp     x3, x4, [sp, #168]

    /* Mark preempt_disabled[cpu] = 1 before unmasking IRQs so any
     * nested timer IRQ's scheduler_tick/maybe_arm check aborts and
     * does not overwrite orig_elr[cpu]/orig_spsr[cpu]. */
    adrp    x5, preempt_disabled
    add     x5, x5, #:lo12:preempt_disabled
    mov     w6, #1
    str     w6, [x5, x0, lsl #2]

    /* Unmask IRQs and call schedule(). If schedule() picks a different
     * task, switch_to() swaps stacks. When this task is eventually
     * switched back in, execution resumes just after the bl below. */
    msr     daifclr, #2
    isb
    bl      schedule
    msr     daifset, #2
    isb

    /* schedule() clobbered x0-x18 and the flag-clear state. Recompute
     * cpu_id and clear preempt_disabled[cpu]. */
    mrs     x0, mpidr_el1
    and     x1, x0, #0xFF
    ubfx    x2, x0, #8, #8
    orr     x0, x1, x2
    adrp    x5, preempt_disabled
    add     x5, x5, #:lo12:preempt_disabled
    str     wzr, [x5, x0, lsl #2]

    /* Restore orig_elr/orig_spsr into the system registers. */
    ldp     x3, x4, [sp, #168]
    msr     elr_el1, x3
    msr     spsr_el1, x4

    /* Restore task's GPRs and unwind the trampoline frame. */
    ldp     x0, x1,   [sp, #0]
    ldp     x2, x3,   [sp, #16]
    ldp     x4, x5,   [sp, #32]
    ldp     x6, x7,   [sp, #48]
    ldp     x8, x9,   [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldr     x18,      [sp, #144]
    ldp     x29, x30, [sp, #152]
    add     sp, sp, #192

    /* eret works outside of an exception handler: it atomically
     * restores PSTATE from SPSR_EL1 and branches to ELR_EL1. This is
     * the canonical way to jump-with-PSTATE-restore in kernel code. */
    eret

.size resched_trampoline, . - resched_trampoline
#endif /* PI5_SECONDARY_PREEMPT */
