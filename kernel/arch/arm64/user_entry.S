/*
 * user_entry.S - EL0 Entry Trampoline for SLM-OS
 *
 * Transitions from EL1 (kernel) to EL0 (user) via ERET.
 * Called from user_task_wrapper() in task.c after context switch
 * restores this task for the first time.
 */

.section .text

/*
 * user_task_enter(entry, stack_top, arg)
 *
 * Transitions to EL0 by setting up the return state and issuing ERET.
 *
 * Arguments:
 *   x0 = user entry point address (function pointer)
 *   x1 = user stack pointer (top of allocated stack)
 *   x2 = argument to pass to user function (in x0 at EL0)
 *
 * This function does not return — ERET transfers to EL0.
 * When the EL0 function wants to exit, it issues SVC #0 with x8=SYS_EXIT.
 */
.global user_task_enter
.type user_task_enter, %function
user_task_enter:
    /* Set up EL0 return state */
    msr     elr_el1, x0         /* Entry point -> ELR (where ERET will jump) */
    mov     x3, #0x0            /* SPSR = EL0t: EL0, SP_EL0, interrupts enabled */
    msr     spsr_el1, x3
    msr     sp_el0, x1          /* User stack pointer */

    /* Pass argument in x0 for the user function */
    mov     x0, x2

    /* Clear all other registers to prevent kernel info leak */
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0

    /* Transition to EL0 */
    eret
.size user_task_enter, . - user_task_enter
