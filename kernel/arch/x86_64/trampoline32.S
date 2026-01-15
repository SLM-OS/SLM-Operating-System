/*
 * trampoline32.S - Pure 32-bit Multiboot2 entry point
 *
 * This is compiled as 32-bit and handles the transition to 64-bit long mode.
 * It then jumps to entry64 (defined elsewhere) which is in 64-bit code.
 *
 * Compile with: gcc -m32 -c trampoline32.S -o trampoline32.o
 */

.set MULTIBOOT2_MAGIC,     0xE85250D6
.set MULTIBOOT2_ARCH,      0
.set MULTIBOOT2_HEADER_LENGTH, multiboot_header_end - multiboot_header
.set MULTIBOOT2_CHECKSUM,  -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LENGTH)
.set MULTIBOOT2_BOOTLOADER_MAGIC, 0x36D76289

/* Page tables and stack - defined in entry64.S */
.extern pml4
.extern pdpt
.extern pd
.extern stack_top

/* 64-bit entry point - defined in entry64.S */
.extern entry64

/*
 * Multiboot2 Header
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
 */
.section .text.boot32
.code32
.global _start
_start:
    cli

    /* Save multiboot info pointer for later */
    mov %ebx, multiboot_ptr

    /* Set up 32-bit stack */
    mov $stack_top, %esp

    /* Verify Multiboot2 magic */
    cmp $MULTIBOOT2_BOOTLOADER_MAGIC, %eax
    jne halt32

    /* Check CPUID support */
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %ecx, %eax
    je halt32

    /* Check long mode support */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb halt32
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx
    jz halt32

    /* Set up page tables for identity mapping first 1GB */
    /* Zero all page table memory */
    mov $pml4, %edi
    xor %eax, %eax
    mov $(4096 * 3 / 4), %ecx
    rep stosl

    /* PML4[0] -> PDPT */
    mov $pml4, %edi
    mov $pdpt, %eax
    or $0x3, %eax
    mov %eax, (%edi)

    /* PDPT[0] -> PD */
    mov $pdpt, %edi
    mov $pd, %eax
    or $0x3, %eax
    mov %eax, (%edi)

    /* Fill PD with 512 x 2MB pages */
    mov $pd, %edi
    mov $0x83, %eax         /* Present + Writable + 2MB */
    mov $512, %ecx
1:  mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    dec %ecx
    jnz 1b

    /* Enable PAE */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    /* Load PML4 into CR3 */
    mov $pml4, %eax
    mov %eax, %cr3

    /* Enable long mode in EFER MSR */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    /* Enable paging (activates long mode) */
    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0

    /* Load 64-bit GDT */
    lgdt gdt64_ptr

    /* Far jump to 64-bit code */
    ljmp $0x08, $entry64

halt32:
    mov $0x3F8, %dx
    mov $'!', %al
    out %al, %dx
    hlt
    jmp halt32

/*
 * 64-bit GDT
 */
.section .rodata.gdt
.align 16
gdt64:
    .quad 0                         /* Null */
    .quad 0x00AF9A000000FFFF        /* Code64: base=0, limit=0xFFFFF, DPL=0, Code, 64-bit */
    .quad 0x00CF92000000FFFF        /* Data64: base=0, limit=0xFFFFF, DPL=0, Data */
gdt64_end:

.align 4
.global gdt64_ptr
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

/*
 * Multiboot info pointer (accessible from 64-bit code)
 */
.section .data
.global multiboot_ptr
multiboot_ptr:
    .long 0

.section .note.GNU-stack, "", @progbits
