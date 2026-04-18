/*
 * kernel-x86_64-kexec.ld — SLM-OS x86-64 linker script, kexec variant.
 *
 * Identical layout to kernel-x86_64.ld, except KERNEL_PHYS is set to
 * 0x20000000 (512 MB) instead of 0x100000 (1 MB). This avoids overlap
 * with the running Ubuntu kernel's code/data when SLM-OS is loaded as
 * a kexec target — kexec-tools refuses to stage an ELF whose load
 * destination overlaps memory currently in use by Linux.
 *
 * Bare-metal UEFI/GRUB boot continues to use kernel-x86_64.ld with the
 * conventional 1 MB load address. This script is ONLY selected when
 * KEXEC_BUILD=1 is passed to CMake.
 *
 * The address 0x20000000 sits within the large 0x100000-0x35fa0fff
 * "System RAM" block on test-pc (Ubuntu 24.04, 16 GB, H610M board);
 * boards with less RAM below 4 GB may need a lower address. The
 * capstone readme for this path notes this constraint.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Kernel loads at 512 MB (kexec destination well above running Linux). */
KERNEL_PHYS = 0x20000000;

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
