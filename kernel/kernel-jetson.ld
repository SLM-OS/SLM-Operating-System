/*
 * kernel-jetson.ld - Linker script for SLM-OS on Jetson Orin Nano
 *
 * Memory layout for Jetson Orin Nano (Tegra234):
 *   RAM starts at 0x80000000 (standard ARM64 layout)
 *   Kernel is loaded at RAM base
 *
 * Segment layout:
 *   .text segment (RX) - code and read-only data
 *   .data segment (RW) - initialized and uninitialized data
 */

ENTRY(_start)

/*
 * Program headers define ELF segments with proper permissions.
 * This eliminates the RWX warning by separating code from data.
 */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X = 0x5 (read + execute) */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W = 0x6 (read + write) */
}

MEMORY
{
    /* Jetson Orin Nano: 8GB RAM starting at 0x80000000 */
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1024M
}

SECTIONS
{
    /* ================================================================
     * TEXT SEGMENT (RX) - Code and read-only data
     * ================================================================ */

    /* Code section - starts at RAM base */
    .text : {
        __text_start = .;
        KEEP(*(.text.boot))     /* Boot code first */
        *(.text .text.*)        /* All other code */
        __text_end = .;
    } > RAM :text

    /* Read-only data - same segment as code */
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } > RAM :text

    /* ================================================================
     * DATA SEGMENT (RW) - Initialized and uninitialized data
     * ================================================================ */

    /* Initialized data — aligned to PE SectionAlignment (64KB) for proper
     * PE/COFF .data section layout. Also satisfies kexec page-alignment. */
    .data ALIGN(0x10000) : {
        __data_start = .;
        *(.data .data.*)
        /* Pad to PE FileAlignment (512 bytes) for SizeOfRawData compliance */
        . = ALIGN(0x200);
        __data_end = .;
    } > RAM :data

    /* Uninitialized data (BSS) */
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM :data

    /*
     * Stack - 16 KB, placed after BSS
     *
     * IMPORTANT: The stack must be part of the data segment so that kexec
     * allocates memory for it. Previously the stack was just symbols outside
     * any section, which meant kexec's segment allocation didn't include it.
     */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        __stack_bottom = .;
        . = . + 0x4000;         /* 16 KB stack */
        __stack_top = .;
    } > RAM :data

    /* End of kernel image — align to SectionAlignment for PE SizeOfImage */
    . = ALIGN(0x10000);
    __kernel_end = .;

    /* Kernel virtual size (includes BSS) for PE SizeOfImage */
    _kernel_size = __kernel_end - _start;

    /* Kernel file size (excludes BSS) for ARM64 image_size */
    _kernel_file_size = __data_end - _start;

    /* PE .text section: code + rodata (everything before .data).
     * With .data at 64KB alignment, this is automatically section-aligned. */
    _kernel_code_size = __data_start - _start - 0x10000;

    /* PE .data section: initialized data.
     * VA = file offset of .data (relative to image base).
     * Size = .data content rounded up to FileAlignment (512 bytes). */
    _kernel_data_va = __data_start - _start;
    _kernel_data_size = __data_end - __data_start;
    _kernel_data_file = __data_start - _start;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Sanity checks */
ASSERT(__kernel_end < 0x81000000, "Kernel image too large!")

/* PE/COFF alignment constraints (SectionAlignment=0x10000, FileAlignment=0x200).
 * These must hold for UEFI to accept the PE/COFF image on Jetson. */
ASSERT((__data_start - _start) % 0x10000 == 0,
    "PE error: .data VA not aligned to SectionAlignment (64KB)")
ASSERT((__data_end - __data_start) % 0x200 == 0,
    "PE error: .data SizeOfRawData not aligned to FileAlignment (512)")
ASSERT((_kernel_size + 0x10000) % 0x10000 == 0,
    "PE error: SizeOfImage not aligned to SectionAlignment (64KB)")
