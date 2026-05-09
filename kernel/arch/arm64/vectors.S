/*
 * vectors.S - Exception vector table for SLM-OS (ARM64)
 *
 * Handles exceptions at the EL the kernel runs at: EL1h on QEMU,
 * EL2h with VHE on PLATFORM_RASPI5 / PLATFORM_JETSON_ORIN_NANO
 * (#683). Vector table layout is the same for both — only the
 * ELR/SPSR register operands differ (no VHE redirect for those —
 * see el_regs.h).
 */

#include "el_regs.h"
#include "cpu_id_asm.h"

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
 * Macro to save all general-purpose registers AND FP/SIMD state.
 * Creates a trap frame on the stack.
 *
 * Frame layout (800 bytes, 16-byte aligned):
 *   [sp, #0..239]    x0-x29 (15 stp pairs)
 *   [sp, #240]       x30
 *   [sp, #248..263]  ELR, SPSR
 *   [sp, #264..271]  padding (keeps q-region 16-byte aligned)
 *   [sp, #272..783]  q0-q31 (16 stp pairs of q,q at stride 0x20)
 *   [sp, #784..791]  FPCR
 *   [sp, #792..799]  FPSR
 *
 * AAPCS reserves x18 as the "platform register" — toolchains that
 * generate code for a runtime which uses x18 as a TLS / shadow-call
 * pointer expect every async exception to preserve it transparently.
 * SLM-OS is bare-metal and the kernel itself never uses x18, so the
 * trap-frame save/restore at offset 144 (paired with restore_regs
 * below) is sufficient: any task-side use of x18 is preserved across
 * the trap frame, and the kernel handler runs with x18 reloaded from
 * the saved slot. If the kernel is ever linked against compiler-rt /
 * LLVM objects compiled with `-ffixed-x18`, the IRQ entry must NOT
 * be allowed to clobber x18 between the save and the C handler call;
 * today the macro stores it before any kernel-side mutation, so the
 * AAPCS contract is preserved by accident. Document explicitly so a
 * future maintainer doesn't hoist the x18-touching code above the
 * stp at offset 144.
 *
 * FP/SIMD save (q0-q31, FPCR, FPSR) is required by the SECONDARY_PREEMPT
 * trampoline path and any future user-mode preemption work. Without it:
 *
 *   1. Task is interrupted with FPU state in q0-q31.
 *   2. el1_irq_handler / maybe_arm_resched_trampoline are AAPCS64 C
 *      callees and may freely clobber q0-q7 + q16-q31 (caller-saved).
 *   3. eret to resched_trampoline leaves the live q-regs in their
 *      post-C state — NOT the interrupted task's state.
 *   4. Trampoline calls schedule() → switch_to(old, new) which saves
 *      the LIVE q-regs as old->context — i.e. saves the clobbered
 *      values, not the interrupted task's actual FPU state.
 *   5. When old task is later resumed via switch_to(other, old),
 *      the corrupted q-regs are restored. Old task continues with
 *      garbage FPU state.
 *
 * Same shape as switch_to in context.S (eager save of full FP/SIMD
 * file). Cost: 16 stp pairs + 2 mrs + 1 stp = ~20 instructions +
 * 528 bytes of stack per IRQ on every ARM64 path. Acceptable
 * universal cost; there is no FPU-trap mechanism in this kernel
 * that would let us defer the save to first-use.
 */
.macro save_regs
    sub     sp, sp, #800            /* GPRs + ELR/SPSR + pad + q-regs + FPCR/FPSR */
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

    /* Save ELR and SPSR (KERN_ELR / KERN_SPSR resolve to elr_el2 /
     * spsr_el2 on PLATFORM_RASPI5, elr_el1 / spsr_el1 elsewhere). */
    mrs     x0, KERN_ELR
    mrs     x1, KERN_SPSR
    stp     x0, x1, [sp, #248]

    /* Save FP/SIMD register file (q0-q31). 16-byte aligned at offset
     * 272 since stp q,q requires that alignment. Stride 0x20 (32B) per
     * pair; covers the whole 512-byte register file. */
    stp     q0,  q1,  [sp, #272]
    stp     q2,  q3,  [sp, #304]
    stp     q4,  q5,  [sp, #336]
    stp     q6,  q7,  [sp, #368]
    stp     q8,  q9,  [sp, #400]
    stp     q10, q11, [sp, #432]
    stp     q12, q13, [sp, #464]
    stp     q14, q15, [sp, #496]
    stp     q16, q17, [sp, #528]
    stp     q18, q19, [sp, #560]
    stp     q20, q21, [sp, #592]
    stp     q22, q23, [sp, #624]
    stp     q24, q25, [sp, #656]
    stp     q26, q27, [sp, #688]
    stp     q28, q29, [sp, #720]
    stp     q30, q31, [sp, #752]

    /* Save FPCR / FPSR. mrs reads each into a 64-bit GPR slot; the
     * upper half is RES0 in both registers. Use individual str (not
     * stp) because offsets 784/792 are outside the stp x,x signed
     * 7-bit *8 immediate range (-512..504); str has a 12-bit unsigned
     * scaled immediate that comfortably reaches the end of this frame. */
    mrs     x0, fpcr
    str     x0, [sp, #784]
    mrs     x0, fpsr
    str     x0, [sp, #792]
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
 * cpu_id computed via ARM64_GET_LOGICAL_CPU (cpu_logical_map[] lookup),
 * so the macro is correct on every platform — Pi 5 (Aff1 encoding),
 * QEMU (Aff0), Jetson (dual-cluster). #647.
 */
.macro DIAG_BUMP_VEC off:req
#if defined(PI5_IRQ_DIAG)
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    ARM64_GET_LOGICAL_CPU x14, x15, x16, x17    /* x14 = logical cpu */
    mov     x17, #0xFF60
    movk    x17, #0xFFE0, lsl #16
    add     x17, x17, x14, lsl #5           /* x17 = per-CPU block base */
    ldr     x16, [x17, #\off]
    add     x16, x16, #1
    str     x16, [x17, #\off]
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
#endif
.endm

/*
 * Macro to restore all general-purpose registers AND FP/SIMD state.
 * Mirrors save_regs above — see that block for the frame layout and
 * rationale for saving the full FP/SIMD file on every IRQ entry.
 */
.macro restore_regs
    /* Restore FPCR / FPSR before the q-regs so they govern any
     * subsequent FP operation (here just the q-reg loads, which don't
     * actually exercise FPCR/FPSR — but keep the program-state-first,
     * data-second ordering consistent with switch_to in context.S).
     * Individual ldr (not ldp) for the same imm-range reason as the
     * save side. */
    ldr     x0, [sp, #784]
    msr     fpcr, x0
    ldr     x0, [sp, #792]
    msr     fpsr, x0

    /* Restore FP/SIMD register file. Stride matches save_regs above. */
    ldp     q0,  q1,  [sp, #272]
    ldp     q2,  q3,  [sp, #304]
    ldp     q4,  q5,  [sp, #336]
    ldp     q6,  q7,  [sp, #368]
    ldp     q8,  q9,  [sp, #400]
    ldp     q10, q11, [sp, #432]
    ldp     q12, q13, [sp, #464]
    ldp     q14, q15, [sp, #496]
    ldp     q16, q17, [sp, #528]
    ldp     q18, q19, [sp, #560]
    ldp     q20, q21, [sp, #592]
    ldp     q22, q23, [sp, #624]
    ldp     q24, q25, [sp, #656]
    ldp     q26, q27, [sp, #688]
    ldp     q28, q29, [sp, #720]
    ldp     q30, q31, [sp, #752]

    /* Restore ELR and SPSR (see KERN_ELR / KERN_SPSR in el_regs.h). */
    ldp     x0, x1, [sp, #248]
    msr     KERN_ELR, x0
    msr     KERN_SPSR, x1

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
    add     sp, sp, #800
.endm

/*
 * Stage 2.5 trace marker macro — writes a single ASCII char to the
 * Pi 5 RP1 PL011 DR register at 0x1f00030000 (mirrors
 * IRQTEST_UART_DR in kernel/src/shell_sys.c — keep both in sync if
 * the RP1 mapping ever changes). Bypasses the kernel UART driver
 * entirely (no lock, no FIFO check). Saves/restores x16/x17 around
 * the write so it's safe before save_regs.
 *
 * Pi-5-specific. Compiled in only when STAGE25_TRACE_PI5 is defined.
 */
#if defined(STAGE25_TRACE_PI5)
/* Address constructed via movz/movk so we don't depend on a reachable
 * literal pool — the macro is invoked from inside the exception
 * vectors which are in the .text section but the literal pool
 * placement is up to the linker.
 *
 * 0x1f00030000 = 0x001f_0003_0000_0000:
 *   movz #0x0000          → bits  0..15
 *   movk #0x0003, lsl #16 → bits 16..31
 *   movk #0x001f, lsl #32 → bits 32..47 */
.macro STAGE25_TRACE_CHAR c
    stp     x16, x17, [sp, #-16]!
    movz    x16, #0x0000
    movk    x16, #0x0003, lsl #16
    movk    x16, #0x001f, lsl #32
    mov     w17, #\c
    str     w17, [x16]
    ldp     x16, x17, [sp], #16
.endm
#else
.macro STAGE25_TRACE_CHAR c
.endm
#endif

/*
 * Hang - unhandled exceptions
 */
hang:
    STAGE25_TRACE_CHAR 'h'              /* Trace: hang vector reached (#672 probe) */
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
    STAGE25_TRACE_CHAR 'S'              /* Trace: sync exception (#672 probe) */
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
    STAGE25_TRACE_CHAR 'V'              /* Trace: vector entered */
    DIAG_BUMP_VEC 0x08
    save_regs
    bl      el1_irq_handler     /* Call C handler */
    STAGE25_TRACE_CHAR 'H'              /* Trace: irq_handler returned */
#if defined(SECONDARY_PREEMPT)
    /* After the IRQ body runs, check for a pending reschedule. When set,
     * maybe_arm_resched_trampoline() rewrites the saved ELR_EL1 in the
     * trap frame so the upcoming `eret` lands in resched_trampoline
     * rather than the originally interrupted PC. That trampoline calls
     * schedule() in task context (not exception context), avoiding the
     * Pi 5 hang caused by switch_to inside an exception handler. */
    mov     x0, sp
    bl      maybe_arm_resched_trampoline
    STAGE25_TRACE_CHAR 'M'              /* Trace: maybe_arm returned */
#endif
    restore_regs
    STAGE25_TRACE_CHAR 'E'              /* Trace: about to eret */
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
    STAGE25_TRACE_CHAR 'F'              /* Trace: FIQ entered (#672 probe) */
    DIAG_BUMP_VEC 0x10
    save_regs
    mov     x0, sp
    bl      el1_fiq_handler
#if defined(SECONDARY_PREEMPT)
    /* Same trampoline-arming hook as el1_irq: when timer_handler has
     * set reschedule_pending for this CPU, rewrite the saved ELR so
     * eret lands in resched_trampoline and schedule() runs in task
     * context (not exception context — fatal on Pi 5 otherwise). */
    mov     x0, sp
    bl      maybe_arm_resched_trampoline
#endif
    restore_regs
    eret

/*
 * EL1 SError (kernel)
 */
el1_serror:
    STAGE25_TRACE_CHAR 'X'              /* Trace: SError (#672 probe) */
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

/* Exposed for the #683 PR-5 vector-layout regression test in
 * kernel/tests/test_syscall.c — the test decodes the `b el0_sync`
 * branch at exception_vectors + 0x400 and asserts the target. */
.global el0_sync
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

#if defined(SECONDARY_PREEMPT)
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
    STAGE25_TRACE_CHAR 'T'              /* Trace: trampoline entry */
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

    /* Compute logical cpu_id via the shared ARM64_GET_LOGICAL_CPU macro
     * (kernel/include/cpu_id_asm.h). Scans cpu_logical_map[] so the
     * lookup is correct on every platform — Pi 5 Aff1 encoding, QEMU
     * Aff0, Jetson dual-cluster (#647). x0..x18 have already been
     * stp'd to the trampoline frame above, so clobbering x0..x3 here
     * is free. */
    ARM64_GET_LOGICAL_CPU x0, x1, x2, x3   /* x0 = cpu */

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

    STAGE25_TRACE_CHAR 'B'              /* Trace: trampoline before schedule */
    /* Unmask IRQs and call schedule(). If schedule() picks a different
     * task, switch_to() swaps stacks. When this task is eventually
     * switched back in, execution resumes just after the bl below. */
    msr     daifclr, #2
    isb
    bl      schedule
    STAGE25_TRACE_CHAR 'C'              /* Trace: trampoline after schedule */
    msr     daifset, #2
    isb

    /* schedule() clobbered x0-x18 and the flag-clear state. Recompute
     * cpu_id (cpu_logical_map[] lookup, #647) and clear
     * preempt_disabled[cpu]. */
    ARM64_GET_LOGICAL_CPU x0, x1, x2, x3
    adrp    x5, preempt_disabled
    add     x5, x5, #:lo12:preempt_disabled
    str     wzr, [x5, x0, lsl #2]

    /* Restore orig_elr/orig_spsr into the system registers. */
    ldp     x3, x4, [sp, #168]
    msr     KERN_ELR, x3
    msr     KERN_SPSR, x4

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

    STAGE25_TRACE_CHAR 'D'              /* Trace: trampoline before final eret */
    /* eret works outside of an exception handler: it atomically
     * restores PSTATE from SPSR_EL1 and branches to ELR_EL1. This is
     * the canonical way to jump-with-PSTATE-restore in kernel code. */
    eret

.size resched_trampoline, . - resched_trampoline
#endif /* SECONDARY_PREEMPT */
