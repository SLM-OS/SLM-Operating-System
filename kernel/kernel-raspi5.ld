/*
 * kernel-raspi5.ld - Linker script for SLM-OS on Raspberry Pi 5
 *
 * Memory layout for Raspberry Pi 5 (BCM2712):
 *   RAM starts at 0x00000000
 *   Kernel is loaded at 0x80000 by firmware (64-bit mode)
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
    /*
     * Raspberry Pi 5 4GB model
     * Kernel loaded at 0x80000 by firmware
     * Reserve first 512KB for firmware/bootloader use
     */
    RAM (rwx) : ORIGIN = 0x80000, LENGTH = 1024M - 512K
}

SECTIONS
{
    /* ================================================================
     * TEXT SEGMENT (RX) - Code and read-only data
     * ================================================================ */

    /* Code section - starts at load address (0x80000) */
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

    /* Page-align segment boundary */
    . = ALIGN(4096);

    /* Initialized data */
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

    /*
     * Stack - 16 KB, placed after BSS
     */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        __stack_bottom = .;
        . = . + 0x4000;         /* 16 KB stack */
        __stack_top = .;
    } > RAM :data

    /* End of kernel image */
    . = ALIGN(4096);            /* Page-align end for clean memory layout */
    __kernel_end = .;

    /* Kernel size calculations */
    _kernel_size = __kernel_end - _start;
    _kernel_file_size = __data_end - _start;

    /* Code section size in file (for PE header compatibility with boot.S) */
    _kernel_code_size = __data_end - _start - 0x10000;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Sanity check: ensure kernel fits in first 16 MB */
ASSERT(__kernel_end < 0x01080000, "Kernel image too large!")
