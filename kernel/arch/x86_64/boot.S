/*
 * boot.S - SLM-OS entry point for x86-64
 *
 * This is the first code executed when the kernel is loaded by GRUB.
 *
 * Multiboot2 entry conditions (32-bit protected mode):
 *   - EAX = 0x36D76289 (Multiboot2 magic)
 *   - EBX = physical address of Multiboot2 information structure
 *   - CS = 32-bit read/execute code segment
 *   - DS/ES/FS/GS/SS = 32-bit read/write data segment
 *   - A20 gate enabled
 *   - CR0.PG = 0 (paging disabled)
 *   - EFLAGS.IF = 0 (interrupts disabled)
 *
 * This code transitions from 32-bit protected mode to 64-bit long mode
 * and then jumps to the C kernel entry point.
 */

.set MULTIBOOT2_MAGIC,     0xE85250D6
.set MULTIBOOT2_ARCH,      0           /* i386 protected mode */
.set MULTIBOOT2_HEADER_LENGTH, multiboot_header_end - multiboot_header
.set MULTIBOOT2_CHECKSUM,  -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LENGTH)

/* Multiboot2 bootloader magic (for verification) */
.set MULTIBOOT2_BOOTLOADER_MAGIC, 0x36D76289

/*
 * Multiboot2 Header
 * Must be within the first 32KB of the kernel image, 8-byte aligned
 */
.section .multiboot, "a"
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCH
    .long MULTIBOOT2_HEADER_LENGTH
    .long MULTIBOOT2_CHECKSUM

    /* Framebuffer tag - request a framebuffer from GRUB */
    .align 8
framebuffer_tag_start:
    .short 5                    /* Type: framebuffer */
    .short 0                    /* Flags: not optional */
    .long framebuffer_tag_end - framebuffer_tag_start
    .long 1024                  /* Preferred width */
    .long 768                   /* Preferred height */
    .long 32                    /* Preferred depth (32-bit color) */
framebuffer_tag_end:

    /* End tag */
    .align 8
    .short 0                    /* Type: end */
    .short 0                    /* Flags */
    .long 8                     /* Size */
multiboot_header_end:

/*
 * 32-bit entry point (from Multiboot2)
 */
.section .text.boot
.code32
.global _start
_start:
    /* Disable interrupts */
    cli

    /* Early debug: Write 'A' to COM1 to confirm we reached _start */
    mov $0x3F8, %dx
    mov $'A', %al
    out %al, (%dx)

    /* Save Multiboot2 info pointer (EBX) - we'll pass this to kernel_main */
    mov %ebx, (multiboot_info_ptr)

    /* Verify Multiboot2 magic */
    cmp $MULTIBOOT2_BOOTLOADER_MAGIC, %eax
    jne .Lhalt32

    /* Debug: 'B' = magic OK */
    mov $0x3F8, %dx
    mov $'B', %al
    out %al, (%dx)

    /* Set up a temporary stack for 32-bit mode */
    mov $stack_top, %esp

    /* Check for CPUID support */
    call check_cpuid
    test %eax, %eax
    jz .Lhalt32

    /* Debug: 'C' = CPUID OK */
    mov $0x3F8, %dx
    mov $'C', %al
    out %al, (%dx)

    /* Check for long mode (64-bit) support */
    call check_long_mode
    test %eax, %eax
    jz .Lhalt32

    /* Debug: 'D' = Long mode OK */
    mov $0x3F8, %dx
    mov $'D', %al
    out %al, (%dx)

    /* Set up identity-mapped page tables for the first 2GB */
    call setup_page_tables

    /* Debug: 'E' = Page tables OK */
    mov $0x3F8, %dx
    mov $'E', %al
    out %al, (%dx)

    /* Enable PAE (Physical Address Extension) */
    mov %cr4, %eax
    or $0x20, %eax              /* CR4.PAE = 1 */
    mov %eax, %cr4

    /* Load PML4 address into CR3 */
    mov $pml4_table, %eax
    mov %eax, %cr3

    /* Enable long mode in EFER MSR */
    mov $0xC0000080, %ecx       /* EFER MSR */
    rdmsr
    or $0x100, %eax             /* EFER.LME = 1 (Long Mode Enable) */
    wrmsr

    /* Debug: 'F' = before paging enable */
    mov $0x3F8, %dx
    mov $'F', %al
    out %al, (%dx)

    /* Enable paging (and thus activate long mode) */
    mov %cr0, %eax
    or $0x80000001, %eax        /* CR0.PG = 1, CR0.PE = 1 */
    mov %eax, %cr0

    /* Load 64-bit GDT */
    lgdt (gdt64_ptr)

    /* Far jump to 64-bit code segment */
    ljmp $0x08, $.Llong_mode_entry

.Lhalt32:
    hlt
    jmp .Lhalt32

/*
 * Check if CPUID is supported by flipping ID bit in EFLAGS
 */
check_cpuid:
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax         /* Flip ID bit (bit 21) */
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %ecx, %eax
    je .Lno_cpuid
    mov $1, %eax
    ret
.Lno_cpuid:
    xor %eax, %eax
    ret

/*
 * Check if CPU supports 64-bit long mode
 */
check_long_mode:
    /* Check if extended CPUID functions are available */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb .Lno_long_mode

    /* Check for long mode support */
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx      /* LM bit (bit 29) */
    jz .Lno_long_mode
    mov $1, %eax
    ret
.Lno_long_mode:
    xor %eax, %eax
    ret

/*
 * Set up initial page tables (identity mapping first 2GB)
 *
 * Page table structure for x86-64:
 *   PML4 -> PDPT -> PD -> PT (or 2MB pages)
 *
 * We use 2MB pages to simplify setup:
 *   - 1 PML4 entry -> PDPT
 *   - 1 PDPT entry -> PD
 *   - 1024 PD entries -> 2MB pages (covers 2GB)
 */
setup_page_tables:
    /* Zero out page table memory */
    mov $pml4_table, %edi
    xor %eax, %eax
    mov $4096, %ecx             /* 4KB for each table */
    rep stosl

    /* PML4[0] -> PDPT */
    mov $pml4_table, %edi
    mov $pdpt_table, %eax
    or $0x3, %eax               /* Present + Writable */
    mov %eax, (%edi)

    /* PDPT[0] -> PD */
    mov $pdpt_table, %edi
    mov $pd_table, %eax
    or $0x3, %eax               /* Present + Writable */
    mov %eax, (%edi)

    /* Identity-map the first 1 GiB with 2 MB pages (512 PD entries).
     * VMM extends the mapping to cover additional PDs once C code runs. */
    mov $pd_table, %edi
    mov $0x83, %eax             /* Present + Writable + 2MB page */
    mov $512, %ecx              /* 512 entries × 2 MB = 1 GiB */
.Lsetup_pd_loop:
    mov %eax, (%edi)
    add $0x200000, %eax         /* Next 2MB */
    add $8, %edi                /* Next PD entry */
    dec %ecx
    jnz .Lsetup_pd_loop

    ret

/*
 * 64-bit entry point
 */
.code64
.Llong_mode_entry:
    /* Reload segment registers for 64-bit mode */
    mov $0x10, %ax              /* Data segment selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up the 64-bit stack */
    mov $stack_top, %rsp

    /* Clear BSS section */
    mov $__bss_start, %rdi
    mov $__bss_end, %rcx
    sub %rdi, %rcx
    shr $3, %rcx                /* Divide by 8 (qwords) */
    xor %rax, %rax
    rep stosq

    /* Load Multiboot2 info pointer as first argument */
    mov (multiboot_info_ptr), %edi
    /* Zero-extend to 64-bit (Multiboot2 struct is in low memory) */
    mov %edi, %edi

    /* Call kernel main */
    call kernel_main

    /* If kernel_main returns, halt */
.Lhalt64:
    cli
    hlt
    jmp .Lhalt64

/*
 * 64-bit GDT (Global Descriptor Table)
 */
.section .rodata
.align 16
gdt64:
    .quad 0                     /* Null descriptor */
gdt64_code:
    .quad 0x00AF9A000000FFFF    /* 64-bit code segment: base=0, limit=0xFFFFF, DPL=0 */
gdt64_data:
    .quad 0x00CF92000000FFFF    /* 64-bit data segment: base=0, limit=0xFFFFF, DPL=0 */
gdt64_end:

.align 4
gdt64_ptr:
    .word gdt64_end - gdt64 - 1 /* Limit */
    .quad gdt64                 /* Base */

/*
 * Data section
 */
.section .data
.align 4
multiboot_info_ptr:
    .long 0

/*
 * BSS section - page tables and stack
 */
.section .bss
.align 4096
pml4_table:
    .space 4096
pdpt_table:
    .space 4096
pd_table:
    .space 4096

.align 16
stack_bottom:
    .space 16384                /* 16KB stack */
stack_top:

/* Mark stack as non-executable (prevents linker warning) */
.section .note.GNU-stack, "", @progbits
