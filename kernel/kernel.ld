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

/*
 * Entry point for ELF loaders (QEMU, kexec).
 * Note: _start contains PE/COFF header for UEFI compatibility.
 * real_start is the actual executable code at offset 0x10000.
 * UEFI ignores ELF entry point and uses PE header's AddressOfEntryPoint.
 */
ENTRY(real_start)

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

    /* Code section - starts at RAM base.
     * EXCLUDE_FILE keeps user_smoke.c's .text.user / .rodata.user
     * out of the main code section so the per-task L1 can map exactly
     * the .text.user range below with VMM_FLAG_USER (#697 PR-4). The
     * `.text.*` wildcard would otherwise greedily absorb `.text.user`. */
    .text : {
        __text_start = .;
        KEEP(*(.text.boot))     /* Boot code first */
        *(EXCLUDE_FILE(*user_smoke.c.obj) .text)
        *(EXCLUDE_FILE(*user_smoke.c.obj) .text.*)
        __text_end = .;
    } > RAM :text

    /* Read-only data - same segment as code */
    .rodata : {
        __rodata_start = .;
        *(EXCLUDE_FILE(*user_smoke.c.obj) .rodata)
        *(EXCLUDE_FILE(*user_smoke.c.obj) .rodata.*)
        __rodata_end = .;
    } > RAM :text

    /* User-mode smoke section (#697 PR-4) — page-aligned + page-padded
     * so the per-task L1 can map exactly this range with VMM_FLAG_USER
     * without leaking adjacent kernel text/rodata. The `.text.user` and
     * `.rodata.user` content from user_smoke.c lands here via the
     * explicit section attributes; everything else from that .obj is
     * EXCLUDE_FILEd above (in practice user_smoke.c emits nothing else
     * because every symbol carries the section attribute). */
    .text.user ALIGN(4096) : {
        __text_user_start = .;
        *(.text.user .text.user.*)
        *(.rodata.user .rodata.user.*)
        . = ALIGN(4096);
        __text_user_end = .;
    } > RAM :text

    /* ================================================================
     * DATA SEGMENT (RW) - Initialized and uninitialized data
     * ================================================================ */

    /* Page-align segment boundary for kexec compatibility */
    . = ALIGN(4096);

    /* Initialized data */
    .data : {
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

    /* End of kernel image */
    . = ALIGN(4096);            /* Page-align end for clean memory layout */
    __kernel_end = .;

    /* Kernel virtual size (includes BSS) for PE SizeOfImage */
    _kernel_size = __kernel_end - _start;

    /* Kernel file size (excludes BSS) for ARM64 image_size */
    _kernel_file_size = __data_end - _start;

    /* Code section size in file (file size minus 64KB PE header).
     * Stops at __data_start — the PE .text and .data sections are
     * distinct under the split-section layout, and `_kernel_code_size`
     * sizes `.text` only. Previously used `__data_end` here, which
     * silently overlapped `.text` and `.data` ranges; inert for QEMU
     * (QEMU loads the raw ELF and ignores the PE header) but invalid
     * PE per UEFI's loader. Kept consistent with kernel-jetson.ld. */
    _kernel_code_size = __data_start - _start - 0x10000;

    /* PE .data section symbols (used by PE/COFF header in boot.S).
     * `_kernel_data_virtual_size` covers through __kernel_end so UEFI
     * zero-fills the BSS region when mapping .data, which in turn
     * makes UEFI map that range RW-NX — boot.S's BSS clear then
     * doesn't fault against UEFI's W^X policy. The PE header in
     * boot.S is inert under QEMU (QEMU loads the raw ELF directly
     * and doesn't parse the MZ/PE stub), but the symbol still has
     * to resolve at link time. */
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

/* PE/COFF section non-overlap invariant.
 *
 * The PE header boot.S emits is inert under QEMU (QEMU loads the raw
 * ELF and ignores the MZ/PE stub), so the PE alignment invariants
 * Jetson cares about (SectionAlignment, FileAlignment, SizeOfImage)
 * don't need to hold here — QEMU's linker aligns .data to 4KB, not
 * 64KB, and the PE header just absorbs that. But .text and .data
 * must not describe overlapping VA ranges regardless of whether the
 * header is consumed, otherwise `_kernel_code_size` is wrong in a
 * structural way. Would catch a regression of the `__data_end` vs.
 * `__data_start` bug that this file used to have. */
ASSERT(_kernel_data_va >= 0x10000 + _kernel_code_size,
    "PE error: .text and .data sections overlap")

/* Sanity check: 32 MB ceiling. The original 16 MB ceiling
 * (0x41000000) was a defensive cap chosen long before the AI
 * scheduler weight blobs (~6 MB across MLP + PPO) and the
 * accumulated platform driver code pushed the bare-metal image
 * past 13 MB; with AI_SCHED=ON the link now needs ~14-18 MB.
 * 32 MB matches the headroom Pi 5 already has (its cap is at
 * 64 MB) and stays well below the QEMU virt RAM start
 * + 1 GB allocation. */
ASSERT(__kernel_end < 0x42000000, "Kernel image too large!")
