/*
 * start.S — _start trampoline for the embedded EL0 hello binary.
 *
 * The kernel sets SP_EL0 + ELR + SPSR before ERET (see
 * kernel/arch/arm64/user_entry.S), so by the time _start runs the
 * stack is already valid and we just call into C `main` and `svc`
 * out via SYS_EXIT (= 0). The exit code is whatever `main` returned
 * in w0; passing it through unchanged matches the user_syscall.h
 * `sys_exit(int)` ABI.
 */

    .section .text._start, "ax", %progbits
    .globl _start
    .type  _start, %function
_start:
    bl  main
    /* main's return value is in w0; SYS_EXIT = 0 goes in x8. */
    mov x8, #0
    svc #0
1:  b   1b
    .size _start, . - _start
