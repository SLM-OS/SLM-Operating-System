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

/* Kernel entry point (platform_x86.c bridges to main kernel) */
.extern kernel_main_x86

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
    call kernel_main_x86

    /* Halt if kernel returns */
halt64:
    cli
    hlt
    jmp halt64

/*
 * Page tables - in separate section, not zeroed by BSS clear.
 * trampoline32.S fills PD[0..3] (4GB). vmm_init() fills PD[4..19] (above 4GB).
 *
 * Layout: 1 PML4 + 1 PDPT + 20 PD pages = 22 pages (88 KB)
 * Each PD covers 1GB (512 × 2MB pages), so 20 PDs cover up to 20GB.
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
    .space 4096 * 20    /* PD[0]-PD[19]: 20GB coverage */

/* Number of PD pages available for vmm_init */
.global pd_count
.set pd_count, 20

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
