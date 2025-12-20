/*
 * kernel.ld - Linker script for SLM-OS kernel
 *
 * Memory layout for QEMU virt machine:
 *   RAM starts at 0x40000000
 *   Kernel is loaded at RAM base
 */

ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

SECTIONS
{
    /* Code section - starts at RAM base */
    .text : {
        __text_start = .;
        KEEP(*(.text.boot))     /* Boot code first */
        *(.text .text.*)        /* All other code */
        __text_end = .;
    } > RAM

    /* Read-only data */
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    } > RAM

    /* Initialized data */
    .data : {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    } > RAM

    /* Uninitialized data (BSS) */
    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    /* Stack - 16 KB, placed after BSS */
    . = ALIGN(16);
    __stack_bottom = .;
    . = . + 0x4000;             /* 16 KB stack */
    __stack_top = .;

    /* End of kernel image */
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
