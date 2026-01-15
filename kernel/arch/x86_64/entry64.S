/*
 * entry64.S - 64-bit entry point for SLM-OS x86-64
 *
 * This is jumped to from the 32-bit trampoline after long mode is enabled.
 * Compile with: gcc -m64 -c entry64.S -o entry64.o
 */

.section .text
.code64
.global entry64

/* Imports from trampoline32.S */
.extern multiboot_ptr

/* Kernel entry point */
.extern kernel_main

/* BSS boundaries from linker */
.extern __bss_start
.extern __bss_end

entry64:
    /* Set up data segments */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up 64-bit stack */
    lea stack_top(%rip), %rsp

    /* Clear BSS */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    xor %rax, %rax
    rep stosq

    /* Load multiboot info pointer and call kernel */
    mov multiboot_ptr(%rip), %edi
    call kernel_main

    /* Halt if kernel returns */
halt64:
    cli
    hlt
    jmp halt64

/*
 * Page tables - in separate section, not zeroed by BSS clear
 * These are set up by trampoline32.S and must remain intact.
 */
.section .page_tables, "aw", @nobits
.align 4096

.global pml4
pml4:
    .space 4096

.global pdpt
pdpt:
    .space 4096

.global pd
pd:
    .space 4096

/*
 * Stack (in BSS)
 */
.section .bss
.align 16
stack_bottom:
    .space 16384
.global stack_top
stack_top:

.section .note.GNU-stack, "", @progbits
