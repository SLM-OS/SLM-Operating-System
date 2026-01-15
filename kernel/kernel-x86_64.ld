/*
 * kernel-x86_64.ld - Linker script for SLM-OS x86-64
 *
 * This linker script is designed for Multiboot2 boot with GRUB.
 * The kernel is loaded at 1MB (0x100000), which is the standard
 * location for Multiboot kernels.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Kernel loads at 1MB */
KERNEL_PHYS = 0x100000;

SECTIONS
{
    . = KERNEL_PHYS;

    /* Multiboot header must be within first 32KB */
    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    /* Code section */
    .text : ALIGN(4096)
    {
        __text_start = .;
        *(.text.boot)
        *(.text .text.*)
        __text_end = .;
    }

    /* Read-only data */
    .rodata : ALIGN(4096)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    /* Read-write data */
    .data : ALIGN(4096)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    /* Uninitialized data (BSS) */
    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Page tables - separate from BSS to avoid being zeroed during boot */
    .page_tables (NOLOAD) : ALIGN(4096)
    {
        *(.page_tables)
    }

    /* End of kernel image */
    . = ALIGN(4096);
    __kernel_end = .;

    /* Calculate sizes for Multiboot */
    __kernel_size = __kernel_end - KERNEL_PHYS;
    __kernel_code_size = __rodata_start - __text_start;
    __kernel_file_size = __bss_start - KERNEL_PHYS;

    /* Discard unwanted sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
