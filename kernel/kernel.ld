/*
 * kernel.ld - Linker script for SLM-OS kernel
 *
 * Memory layout for QEMU virt machine:
 *   RAM starts at 0x40000000
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
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 1024M
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

    /* Initialized data */
    .data : {
        . = ALIGN(4096);        /* Page-align for MMU protection */
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

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Sanity check: ensure kernel fits in first 16 MB */
ASSERT(__kernel_end < 0x41000000, "Kernel image too large!")
