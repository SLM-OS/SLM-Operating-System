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
     * Raw size = .data content rounded up to FileAlignment (512 bytes).
     * Virtual size covers through __kernel_end so UEFI zero-fills the
     * BSS + stack portion when mapping .data into memory. This is what
     * lets the W^X-enforcing UEFI firmware map the whole region as
     * writable + non-executable, unblocking boot.S's BSS clear. */
    _kernel_data_va = __data_start - _start;
    _kernel_data_size = __data_end - __data_start;
    _kernel_data_file = __data_start - _start;
    _kernel_data_virtual_size = __kernel_end - __data_start;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}

/* Sanity checks */
ASSERT(__kernel_end < 0x81000000, "Kernel image too large!")

/* PE/COFF invariants (SectionAlignment=0x10000, FileAlignment=0x200).
 * These must hold for UEFI to accept the PE/COFF image built from
 * this layout. Each assert would catch a specific regression in the
 * PE header or in the linker-emitted section boundaries. */

/* .data's VirtualAddress must be SectionAlignment-aligned. */
ASSERT((__data_start - _start) % 0x10000 == 0,
    "PE error: .data VA not aligned to SectionAlignment (64KB)")

/* .data's SizeOfRawData must be FileAlignment-aligned. */
ASSERT((__data_end - __data_start) % 0x200 == 0,
    "PE error: .data SizeOfRawData not aligned to FileAlignment (512)")

/* .text's SizeOfRawData must be FileAlignment-aligned. Holds today
 * because _kernel_code_size = __data_start - _start - 0x10000 and
 * __data_start is SectionAlignment-aligned (0x10000 ≫ 0x200), but
 * assert explicitly so a future linker change that moves .data onto
 * a smaller alignment breaks the build instead of producing an
 * invalid PE. */
ASSERT(_kernel_code_size % 0x200 == 0,
    "PE error: .text SizeOfRawData not aligned to FileAlignment (512)")

/* .data's PointerToRawData must be FileAlignment-aligned — same
 * reasoning as SizeOfRawData. `_kernel_data_file` is defined above
 * as `__data_start - _start`, so this tracks the section's file
 * placement. */
ASSERT(_kernel_data_file % 0x200 == 0,
    "PE error: .data PointerToRawData not aligned to FileAlignment (512)")

/* SizeOfImage must be SectionAlignment-aligned. `_kernel_size` is
 * already `__kernel_end - _start` where __kernel_end is forced
 * 0x10000-aligned above, so this is a belt-and-braces check in case
 * someone moves the ALIGN. */
ASSERT(_kernel_size % 0x10000 == 0,
    "PE error: SizeOfImage not aligned to SectionAlignment (64KB)")

/* .text and .data must not overlap. .text ends at VA 0x10000 +
 * _kernel_code_size; .data starts at VA _kernel_data_va. Would
 * catch the QEMU-linker bug where _kernel_code_size incorrectly
 * included the .data range. */
ASSERT(_kernel_data_va >= 0x10000 + _kernel_code_size,
    "PE error: .text and .data sections overlap")

/* .data's end VA (VirtualAddress + VirtualSize) must equal
 * SizeOfImage, i.e. no gap between .data and the end of the mapped
 * image. Would catch a _kernel_data_virtual_size miscalculation. */
ASSERT(_kernel_data_va + _kernel_data_virtual_size == _kernel_size,
    "PE error: .data end VA != SizeOfImage (layout inconsistency)")

/* Early EL2 VBAR (UEFI-direct diagnostic) invariants.
 *
 * The `jetson_early_vbar_el2` table in boot.S is loaded into
 * VBAR_EL2 by efi_stub_entry post-EBS. ARM64 requires VBAR to be
 * 0x800 (2048-byte) aligned — an unaligned table silently
 * misroutes every exception to garbage. The `.balign 0x800`
 * directive should take care of this, but assert explicitly so a
 * future edit (moving the table, wrapping it in an extra section,
 * inserting code before it) breaks the build instead of producing
 * a quietly-broken binary. */
ASSERT((jetson_early_vbar_el2 & 0x7FF) == 0,
    "Jetson early VBAR_EL2 table is not 2KB-aligned (ARM64 VBAR requirement)")

/* The handler writes 56 bytes (7 × uint64_t: magic, ESR, ELR, FAR,
 * SPSR, HCR, CurrentEL) into `jetson_early_fault_slot`. BSS gives
 * us 64 bytes; if someone shrinks the `.skip` the handler will run
 * off the end of the slot and stomp whatever BSS variable follows.
 * Pin the size via the next-symbol check. The first declared BSS
 * symbol after the slot is `__bss_end` if the slot is last in BSS,
 * but boot.S places the slot early in BSS so we can't rely on
 * that; instead use a paired marker symbol, defined below. */
ASSERT(jetson_early_fault_slot_end - jetson_early_fault_slot >= 64,
    "jetson_early_fault_slot too small (handler writes 56 bytes, need ≥ 64)")
