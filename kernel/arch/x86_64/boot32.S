/*
 * boot32.S - 32-bit Multiboot2 entry point for SLM-OS x86-64
 *
 * GRUB loads this in 32-bit protected mode. We transition to 64-bit
 * and jump to the 64-bit C kernel.
 *
 * NOTE: Even though this targets x86-64, the 32-bit portion must use
 * explicit 32-bit addressing (not RIP-relative).
 */

.set MULTIBOOT2_MAGIC,     0xE85250D6
.set MULTIBOOT2_ARCH,      0
.set MULTIBOOT2_HEADER_LENGTH, multiboot_header_end - multiboot_header
.set MULTIBOOT2_CHECKSUM,  -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LENGTH)
.set MULTIBOOT2_BOOTLOADER_MAGIC, 0x36D76289

/*
 * Multiboot2 Header (must be within first 32KB)
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_HEADER_LENGTH
    .long MULTIBOOT2_CHECKSUM

    /* Framebuffer tag */
    .align 8
    .short 5            /* Type: framebuffer */
    .short 0            /* Flags */
    .long 20            /* Size */
    .long 1024          /* Width */
    .long 768           /* Height */
    .long 32            /* Depth */

    /* End tag */
    .align 8
    .short 0
    .short 0
    .long 8
multiboot_header_end:

/*
 * 32-bit entry point
 *
 * Use explicit address size overrides for 32-bit code.
 */
.section .text.boot32
.code32
.global _start
_start:
    cli

    /* Debug: 'A' to COM1 */
    movw $0x3F8, %dx
    movb $'A', %al
    outb %al, %dx

    /* Save multiboot info - use explicit absolute address */
    movl %ebx, (multiboot_info_ptr)

    /* Verify magic */
    cmpl $MULTIBOOT2_BOOTLOADER_MAGIC, %eax
    jne halt32

    /* Debug: 'B' */
    movw $0x3F8, %dx
    movb $'B', %al
    outb %al, %dx

    /* Set up stack */
    movl $stack_top, %esp

    /* Check CPUID */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    cmpl %ecx, %eax
    je halt32

    /* Debug: 'C' */
    movw $0x3F8, %dx
    movb $'C', %al
    outb %al, %dx

    /* Check long mode */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb halt32
    movl $0x80000001, %eax
    cpuid
    testl $0x20000000, %edx
    jz halt32

    /* Debug: 'D' */
    movw $0x3F8, %dx
    movb $'D', %al
    outb %al, %dx

    /* Set up page tables (identity map first 1GB with 2MB pages) */
    /* Zero PML4, PDPT, PD */
    movl $pml4, %edi
    xorl %eax, %eax
    movl $(4096*3/4), %ecx
    rep stosl

    /* PML4[0] -> PDPT */
    movl $pml4, %edi
    movl $pdpt, %eax
    orl $0x3, %eax
    movl %eax, (%edi)

    /* PDPT[0] -> PD */
    movl $pdpt, %edi
    movl $pd, %eax
    orl $0x3, %eax
    movl %eax, (%edi)

    /* PD entries: 512 x 2MB pages = 1GB */
    movl $pd, %edi
    movl $0x83, %eax         /* Present + Writable + 2MB page */
    movl $512, %ecx
.Lfill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    decl %ecx
    jnz .Lfill_pd

    /* Debug: 'E' */
    movw $0x3F8, %dx
    movb $'E', %al
    outb %al, %dx

    /* Enable PAE */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    /* Load PML4 */
    movl $pml4, %eax
    movl %eax, %cr3

    /* Enable long mode in EFER */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* Debug: 'F' */
    movw $0x3F8, %dx
    movb $'F', %al
    outb %al, %dx

    /* Enable paging */
    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0

    /* Load 64-bit GDT */
    lgdtl (gdt64_ptr)

    /* Jump to 64-bit code - use far jump with explicit encoding */
    /* ljmp $0x08, $long_mode_entry */
    .byte 0xEA                      /* Far jump opcode */
    .long long_mode_entry           /* 32-bit offset */
    .word 0x08                      /* Segment selector */

halt32:
    movw $0x3F8, %dx
    movb $'!', %al
    outb %al, %dx
    hlt
    jmp halt32

/*
 * 64-bit entry point
 */
.code64
long_mode_entry:
    /* Debug: 'G' */
    mov $0x3F8, %dx
    mov $'G', %al
    out %al, (%dx)

    /* Set up segments */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up 64-bit stack */
    movabs $stack_top, %rsp

    /* Clear BSS */
    movabs $__bss_start, %rdi
    movabs $__bss_end, %rcx
    sub %rdi, %rcx
    shr $3, %rcx
    xor %rax, %rax
    rep stosq

    /* Debug: 'H' */
    mov $0x3F8, %dx
    mov $'H', %al
    out %al, (%dx)

    /* Call kernel_main with multiboot info pointer */
    movl (multiboot_info_ptr), %edi
    call kernel_main

    /* Halt if kernel returns */
halt64:
    cli
    hlt
    jmp halt64

/*
 * Data
 */
.section .rodata
.align 16
gdt64:
    .quad 0                         /* Null */
    .quad 0x00AF9A000000FFFF        /* Code64 */
    .quad 0x00CF92000000FFFF        /* Data64 */
gdt64_end:

.align 4
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .data
.align 4
.global multiboot_info_ptr
multiboot_info_ptr:
    .long 0

.section .bss
.align 4096
pml4:
    .space 4096
pdpt:
    .space 4096
pd:
    .space 4096

.align 16
stack_bottom:
    .space 16384
.global stack_top
stack_top:

.section .note.GNU-stack, "", @progbits
