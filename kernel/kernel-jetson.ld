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

    /* Page-align segment boundary for kexec compatibility */
    . = ALIGN(4096);

    /* Initialized data - explicitly page-aligned for segment boundary */
    .data ALIGN(4096) : {
        __data_start = .;
        *(.data .data.*)
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

    /* Stack - 16 KB, placed after BSS */
    . = ALIGN(16);
    __stack_bottom = .;
    . = . + 0x4000;             /* 16 KB stack */
    __stack_top = .;

    /* End of kernel image */
    . = ALIGN(4096);            /* Page-align end for clean memory layout */
    __kernel_end = .;

    /* Kernel virtual size (includes BSS) for PE SizeOfImage */
    _kernel_size = __kernel_end - _start;

    /* Kernel file size (excludes BSS) for ARM64 image_size */
    _kernel_file_size = __data_end - _start;

    /* Code section size in file (file size minus 64KB PE header) */
    _kernel_code_size = __data_end - _start - 0x10000;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Sanity check: ensure kernel fits in first 16 MB */
ASSERT(__kernel_end < 0x81000000, "Kernel image too large!")
