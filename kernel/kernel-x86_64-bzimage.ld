/*
 * kernel-x86_64-bzimage.ld — linker script for the Linux bzImage build.
 *
 * Same memory layout as kernel-x86_64-kexec.ld (load at 0x20000000 —
 * clear of the running Ubuntu kernel at kexec time) but places a
 * new section `.bzimage_entry` at a fixed 0x200 offset from the load
 * base. kexec-tools' bzImage64 loader jumps to `load_addr + 0x200`
 * after copying the payload, so the bzImage entry stub MUST sit
 * exactly there — an alignment or a stray extra byte in .multiboot
 * would desynchronise it.
 *
 * Selected when CMake is configured with BZIMAGE_BUILD=1 (and, by
 * implication, KEXEC_BUILD=1 or compatible load address). Neither the
 * bare-metal UEFI path (kernel-x86_64.ld) nor the Multiboot2 kexec
 * path (kernel-x86_64-kexec.ld) is affected.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start_bzimage)

/* Same as kexec: load high to clear running-Linux memory. */
KERNEL_PHYS = 0x20000000;

SECTIONS
{
    . = KERNEL_PHYS;

    /* Multiboot header — unused by kexec (it reads the bzImage
     * setup_header instead), but harmless to leave so the same
     * image also boots via GRUB if ever needed. Small: ~40 bytes. */
    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    /* Force the bzImage entry stub to exactly KERNEL_PHYS + 0x200.
     * If .multiboot extends past 0x200 this linker invocation will
     * fail loudly, catching any regression where MB headers grow
     * large enough to collide with the bzImage entry. */
    . = KERNEL_PHYS + 0x200;
    .bzimage_entry :
    {
        KEEP(*(.bzimage_entry))
    }

    /* Rest of the kernel — same structure as the other X86_64
     * linker scripts. .text gets aligned to a 4 KiB boundary, which
     * puts trampoline32's _start at KERNEL_PHYS + 0x1000 just like
     * the kexec / bare-metal builds. */
    .text : ALIGN(4096)
    {
        __text_start = .;
        *(.text.boot)
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(4096)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(4096)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    .page_tables (NOLOAD) : ALIGN(4096)
    {
        *(.page_tables)
    }

    . = ALIGN(4096);
    __kernel_end = .;

    __kernel_size       = __kernel_end - KERNEL_PHYS;
    __kernel_code_size  = __rodata_start - __text_start;
    __kernel_file_size  = __bss_start - KERNEL_PHYS;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
