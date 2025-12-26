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
    save_regs
    mov     x0, sp              /* Pass trap frame pointer */
    bl      el1_sync_handler    /* Call C handler */
    restore_regs
    eret

/*
 * EL1 IRQ (kernel) - Timer interrupts come here
 */
el1_irq:
    save_regs
    bl      el1_irq_handler     /* Call C handler */
    restore_regs
    eret

/*
 * EL1 FIQ (kernel)
 */
el1_fiq:
    b       hang                /* Not used */

/*
 * EL1 SError (kernel)
 */
el1_serror:
    save_regs
    mov     x0, sp
    bl      el1_serror_handler
    restore_regs
    eret

/*
 * EL0 handlers (userspace - future)
 */
el0_sync:
el0_irq:
el0_fiq:
el0_serror:
    b       hang                /* Not implemented yet */
