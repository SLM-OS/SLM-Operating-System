/*
 * user.ld — linker script for stand-alone EL0 ELF binaries.
 *
 * Lays out a static ARM64 ELF with three PT_LOAD segments:
 *
 *   .text   RX  at USER_TEXT_VA = 0x4000000000  (matches vmm.h)
 *   .rodata R   immediately after, page-aligned
 *   .data   RW  immediately after, page-aligned (covers .bss too)
 *
 * The kernel-side loader (elf_load_user) reads each PT_LOAD,
 * allocates physical pages, copies p_filesz bytes, zero-fills the
 * remainder up to p_memsz (for .bss), and maps the segment into the
 * per-task user L1 with the right RWX bits derived from p_flags.
 *
 * Keeping segments page-aligned is important — vmm_user_map_page
 * works one 4 KB page at a time, and a segment that crosses a page
 * boundary with the wrong flags would either fault on first access
 * or smuggle write/exec rights into a neighbouring page.
 *
 * USER_TEXT_VA is intentionally hard-coded here rather than pulled
 * from kernel/include/vmm.h: this script runs ld, not cpp, and
 * carrying a parallel kernel-style include here is not worth the
 * fragility. The build asserts the value matches vmm.h via a
 * static_assert on the kernel side (kernel/src/elf_user.c).
 */

ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);  /* R + X */
    rodata PT_LOAD FLAGS(4);  /* R     */
    data   PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
    . = 0x4000000000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    } :text

    . = ALIGN(4096);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.ARM.attributes)
    }
}
