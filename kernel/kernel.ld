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

    /* Initialized data — aligned to the PE SectionAlignment (64 KB) so
     * the PE .data section starts on a section boundary. UEFI firmware
     * applies W^X per PE section (`make test-qemu-uefi` boots this image
     * under QEMU's AArch64 UEFI): with .data at a 4 KB boundary its
     * first page was still mapped read-only with .text and the first
     * .data store faulted. Also satisfies kexec page alignment. */
    .data ALIGN(0x10000) : {
        __data_start = .;
        *(.data .data.*)
        /* Pad to PE FileAlignment (512 bytes) for SizeOfRawData */
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

    /* End of kernel image — aligned to SectionAlignment for PE SizeOfImage */
    . = ALIGN(0x10000);
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
     * makes UEFI map that range RW-NX. `make test` loads the raw ELF
     * and ignores the PE header; `make test-qemu-uefi` boots
     * slmos.bin through QEMU's UEFI firmware, which parses it. */
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

/* PE/COFF invariants (SectionAlignment=0x10000, FileAlignment=0x200),
 * same as kernel-jetson.ld. The PE header boot.S emits is consumed by
 * real UEFI firmware in `make test-qemu-uefi`, so it must be valid PE,
 * not just link. */
ASSERT((__data_start - _start) % 0x10000 == 0,
    "PE error: .data VA not aligned to SectionAlignment (64KB)")
ASSERT((__data_end - __data_start) % 0x200 == 0,
    "PE error: .data SizeOfRawData not aligned to FileAlignment (512)")
ASSERT(_kernel_code_size % 0x200 == 0,
    "PE error: .text SizeOfRawData not aligned to FileAlignment (512)")
ASSERT(_kernel_size % 0x10000 == 0,
    "PE error: SizeOfImage not aligned to SectionAlignment (64KB)")
/* .text and .data must not describe overlapping VA ranges. Would catch
 * a regression of the `__data_end` vs `__data_start` bug this file
 * used to have in `_kernel_code_size`. */
ASSERT(_kernel_data_va >= 0x10000 + _kernel_code_size,
    "PE error: .text and .data sections overlap")
ASSERT(_kernel_data_va + _kernel_data_virtual_size == _kernel_size,
    "PE error: .data end VA != SizeOfImage (layout inconsistency)")

/* Sanity check: 64 MB ceiling. The original 16 MB ceiling
 * (0x41000000) was a defensive cap chosen long before the AI
 * scheduler weight blobs (~6 MB across MLP + PPO) and the
 * accumulated platform driver code pushed the bare-metal image
 * past 13 MB; with AI_SCHED=ON the link now needs ~14-18 MB.
 * It was raised to 32 MB, and then to 64 MB, the cap Pi 5 and Jetson
 * use, once the GA10B host-tested modules' fake-GPU page pools
 * (#1088/#1092; test sources link into every image) took the
 * AI_SCHED=ON test kernel to ~39 MB, almost all BSS. Nothing on QEMU
 * virt is placed below 0x44000000 (PMM starts at __kernel_end), and
 * the 1 GB of guest RAM leaves ample room. */
ASSERT(__kernel_end < 0x44000000, "Kernel image too large!")
