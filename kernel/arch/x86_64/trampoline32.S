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
 *
 * No framebuffer tag — GRUB EFI cannot always set a video mode.
 * The kernel uses COM1 serial for console output.
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_HEADER_LENGTH
    .long MULTIBOOT2_CHECKSUM

    /* End tag */
    .align 8
    .short 0
    .short 0
    .long 8
multiboot_header_end:

/*
 * 32-bit entry point
 *
 * GRUB enters here in 32-bit protected mode with:
 *   EAX = Multiboot2 magic (0x36D76289) — BIOS GRUB only; UEFI GRUB may not set this
 *   EBX = pointer to Multiboot2 information structure
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

    /*
     * Skip Multiboot2 magic validation.
     * GRUB EFI does not reliably pass 0x36D76289 in EAX.
     * The multiboot info pointer in EBX is still valid.
     */

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

    /*
     * Set up page tables for identity mapping first 4GB.
     * Uses 2MB pages: 4 PD pages × 512 entries = 2048 × 2MB = 4GB.
     * vmm_init() extends mapping above 4GB from 64-bit C code.
     */

    /* Zero all page table memory (PML4 + PDPT + 20 PD pages = 22 pages) */
    mov $pml4, %edi
    xor %eax, %eax
    mov $(4096 * 22 / 4), %ecx
    rep stosl

    /* PML4[0] -> PDPT */
    mov $pml4, %edi
    mov $pdpt, %eax
    or $0x3, %eax
    mov %eax, (%edi)

    /* PDPT[0..3] -> PD[0..3] (4 entries, each covering 1GB) */
    mov $pdpt, %edi
    mov $pd, %eax
    or $0x3, %eax
    mov $4, %ecx
2:  mov %eax, (%edi)
    add $4096, %eax         /* Next PD page (4KB apart) */
    add $8, %edi            /* Next PDPT entry (8 bytes) */
    dec %ecx
    jnz 2b

    /* Fill PD[0..3] with 2048 x 2MB pages (4GB identity mapping) */
    mov $pd, %edi
    mov $0x83, %eax         /* Present + Writable + 2MB */
    mov $2048, %ecx         /* 4 × 512 entries */
1:  mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    dec %ecx
    jnz 1b

    /* Enable PAE (CR4 bit 5) + SSE FXSAVE/FXRSTOR (CR4.OSFXSR, bit 9)
     * + unmasked SSE exceptions (CR4.OSXMMEXCPT, bit 10).
     * OSFXSR + OSXMMEXCPT are required for any SSE instruction at
     * CPL=0 to execute without #UD / #NM. The inline-asm SSE kernels
     * in runtime/src/inference/ops.rs (C1 / P3-1) rely on this. */
    mov %cr4, %eax
    or $0x620, %eax
    mov %eax, %cr4

    /* Load PML4 into CR3 */
    mov $pml4, %eax
    mov %eax, %cr3

    /* Enable long mode in EFER MSR */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr

    /* Enable paging (PG bit 31), set MP (bit 1), clear EM (bit 2).
     * MP/EM control how x87 and SSE are trapped — EM=0 means don't
     * emulate (SSE executes natively), MP=1 means monitor coprocessor
     * writes (required for FXSAVE). PE (bit 0) is already set. */
    mov %cr0, %eax
    and $~0x4, %eax
    or  $0x80000003, %eax
    mov %eax, %cr0

    /*
     * Load 64-bit GDT.
     *
     * This LGDT runs in 32-bit protected mode and reads a 6-byte
     * pseudo-descriptor (2-byte limit + 4-byte base). The kernel is
     * linked at 1 MiB (see kernel-x86_64.ld), so the GDT base always
     * fits in 32 bits; the CPU zero-extends it into the 64-bit GDTR
     * when entering IA-32e mode, and the far jump below changes only
     * CS. No 64-bit-mode reload is needed. A reload would only become
     * mandatory if the kernel moved to a higher-half mapping (GDT VA
     * above 4 GiB). See docs/x86-64-port.md §GDT-and-Segments and
     * GitHub issue #73.
     */
    lgdt gdt64_ptr

    /* Far jump to 64-bit code */
    ljmp $0x08, $entry64

halt32:
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
