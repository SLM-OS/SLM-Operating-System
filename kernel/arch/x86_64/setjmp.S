/*
 * setjmp.S - setjmp/longjmp for x86-64
 *
 * Minimal implementation for Lua error handling.
 * Saves and restores callee-saved registers per System V AMD64 ABI.
 *
 * jmp_buf layout (8 x 64-bit values = 64 bytes):
 *   [0]  rbx
 *   [1]  rbp
 *   [2]  r12
 *   [3]  r13
 *   [4]  r14
 *   [5]  r15
 *   [6]  rsp (adjusted past return address)
 *   [7]  rip (return address from stack)
 */

.text
.global setjmp
.global _setjmp
.global longjmp
.type setjmp, @function
.type _setjmp, @function
.type longjmp, @function

/*
 * int setjmp(jmp_buf env)
 *
 * Save current context to env (rdi = pointer to jmp_buf).
 * Returns 0 on initial call.
 */
setjmp:
_setjmp:
    /* Save callee-saved registers */
    mov     %rbx, 0(%rdi)
    mov     %rbp, 8(%rdi)
    mov     %r12, 16(%rdi)
    mov     %r13, 24(%rdi)
    mov     %r14, 32(%rdi)
    mov     %r15, 40(%rdi)

    /* Save stack pointer (as it will be after we return — skip return addr) */
    lea     8(%rsp), %rax
    mov     %rax, 48(%rdi)

    /* Save return address (currently on top of stack) */
    mov     (%rsp), %rax
    mov     %rax, 56(%rdi)

    /* Return 0 */
    xor     %eax, %eax
    ret

.size setjmp, . - setjmp

/*
 * void longjmp(jmp_buf env, int val)
 *
 * Restore context from env. Never returns.
 * setjmp appears to return val (or 1 if val == 0).
 *
 * rdi = jmp_buf, esi = val
 */
longjmp:
    /* If val == 0, set to 1 */
    mov     %esi, %eax
    test    %eax, %eax
    jnz     1f
    mov     $1, %eax
1:

    /* Restore callee-saved registers */
    mov     0(%rdi), %rbx
    mov     8(%rdi), %rbp
    mov     16(%rdi), %r12
    mov     24(%rdi), %r13
    mov     32(%rdi), %r14
    mov     40(%rdi), %r15

    /* Restore stack pointer */
    mov     48(%rdi), %rsp

    /* Jump to saved return address (setjmp returns with val in eax) */
    jmp     *56(%rdi)

.size longjmp, . - longjmp

.section .note.GNU-stack, "", @progbits
