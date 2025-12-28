/**
 * setjmp.S - setjmp/longjmp for AArch64
 *
 * Minimal implementation for Lua error handling.
 * Saves and restores callee-saved registers.
 *
 * jmp_buf layout (22 x 64-bit values = 176 bytes):
 *   [0-9]   x19-x28 (callee-saved)
 *   [10]    x29 (frame pointer)
 *   [11]    x30 (link register)
 *   [12]    SP (stack pointer)
 *   [13-20] d8-d15 (callee-saved FP regs)
 *   [21]    FPCR (FP control register)
 */

.text
.global setjmp
.global longjmp

/**
 * int setjmp(jmp_buf env)
 *
 * Save current context to env.
 * Returns 0 on initial call.
 * Returns non-zero when returning via longjmp.
 */
setjmp:
    /* Save callee-saved general registers x19-x28 */
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]

    /* Save frame pointer, link register, and stack pointer */
    mov     x2, sp
    stp     x29, x30, [x0, #80]
    str     x2, [x0, #96]

    /* Save callee-saved FP registers d8-d15 */
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]

    /* Save FPCR */
    mrs     x2, fpcr
    str     x2, [x0, #168]

    /* Return 0 on initial call */
    mov     x0, #0
    ret

/**
 * void longjmp(jmp_buf env, int val)
 *
 * Restore context from env.
 * Never returns - instead, setjmp returns with val.
 * If val is 0, setjmp returns 1 instead.
 */
longjmp:
    /* If val is 0, change it to 1 */
    cmp     x1, #0
    csinc   x1, x1, xzr, ne     /* x1 = (x1 != 0) ? x1 : 1 */

    /* Restore FPCR */
    ldr     x2, [x0, #168]
    msr     fpcr, x2

    /* Restore callee-saved FP registers d8-d15 */
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]

    /* Restore stack pointer */
    ldr     x2, [x0, #96]
    mov     sp, x2

    /* Restore frame pointer and link register */
    ldp     x29, x30, [x0, #80]

    /* Restore callee-saved general registers x19-x28 */
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]

    /* Return val (in x1) as the return value of setjmp */
    mov     x0, x1
    ret
