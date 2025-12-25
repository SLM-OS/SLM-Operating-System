/*
 * boot.S - SLM-OS entry point for ARM64
 *
 * This is the first code executed when the kernel is loaded.
 *
 * For QEMU: Loads at 0x40000000 with x0 = DTB address
 * For Jetson: Loaded by UEFI at runtime-determined address with x0 = DTB address
 *
 * Entry conditions:
 *   - x0 = DTB address (preserved and passed to kernel_main)
 *   - EL1 or EL2 exception level
 *   - MMU disabled
 *   - Caches disabled
 *   - SP undefined
 */

.section .text.boot
.global _start
.global real_start

/*
 * ARM64 Linux Image Header with PE/COFF stub
 *
 * This header is required for UEFI boot on ARM64.
 * UEFI requires PE/COFF format, so we include a minimal PE header.
 * Format follows Linux kernel's arch/arm64/kernel/efi-header.S
 *
 * PE/COFF requires "MZ" at offset 0, and PE header pointer at offset 0x3C.
 * UEFI reads AddressOfEntryPoint from PE header to find the actual start.
 *
 * For direct boot (QEMU -kernel binary), the first instruction must be
 * executable. The ccmp instruction encodes to bytes "MZ.." satisfying both
 * PE/COFF magic requirement and ARM64 executable requirement.
 */
_start:
    /*
     * This instruction encodes to 0xfa405a4d = bytes "MZ@." in little-endian.
     * It's a conditional compare that does nothing useful but is harmless.
     * This satisfies PE/COFF "MZ" signature while being valid ARM64 code.
     */
    ccmp    x18, #0, #0xd, pl   /* code0: PE/COFF "MZ" magic + harmless NOP */
    b       real_start          /* code1: branch to actual entry point */
    .quad   0                   /* text_offset: load offset (0 = any 2MB aligned) */
    .quad   _kernel_file_size   /* image_size: actual file size (excludes BSS) */
    .quad   0x0a                /* flags: LE kernel, 4KB pages, anywhere in RAM */
    .quad   0                   /* res2: reserved */
    .quad   0                   /* res3: reserved */
    .quad   0                   /* res4: reserved */
    .ascii  "ARM\x64"           /* magic: ARM64 image magic */
    .long   pe_header - _start  /* PE header offset */

    /*
     * Minimal PE/COFF header for UEFI
     */
    .align  3
pe_header:
    .ascii  "PE\0\0"            /* PE signature */
    .short  0xaa64              /* Machine: ARM64 */
    .short  2                   /* NumberOfSections */
    .long   0                   /* TimeDateStamp */
    .long   0                   /* PointerToSymbolTable */
    .long   0                   /* NumberOfSymbols */
    .short  section_table - optional_header  /* SizeOfOptionalHeader */
    .short  0x206               /* Characteristics: EXECUTABLE_IMAGE | LINE_NUMS_STRIPPED */

optional_header:
    .short  0x20b               /* PE32+ magic */
    .byte   0x02                /* MajorLinkerVersion */
    .byte   0x14                /* MinorLinkerVersion */
    .long   _kernel_size        /* SizeOfCode */
    .long   0                   /* SizeOfInitializedData */
    .long   0                   /* SizeOfUninitializedData */
    .long   0x10000             /* AddressOfEntryPoint (64KB, where real_start will be) */
    .long   0x10000             /* BaseOfCode (64KB) */

    .quad   0                   /* ImageBase */
    .long   0x10000             /* SectionAlignment (64KB, like Linux) */
    .long   0x200               /* FileAlignment (512 bytes) */
    .short  0                   /* MajorOperatingSystemVersion */
    .short  0                   /* MinorOperatingSystemVersion */
    .short  1                   /* MajorImageVersion */
    .short  0                   /* MinorImageVersion */
    .short  0                   /* MajorSubsystemVersion */
    .short  0                   /* MinorSubsystemVersion */
    .long   0                   /* Win32VersionValue */
    .long   _kernel_size + 0x10000  /* SizeOfImage (code + 64KB header) */
    .long   0x10000             /* SizeOfHeaders (64KB, like Linux) */
    .long   0                   /* CheckSum */
    .short  0xa                 /* Subsystem: EFI_APPLICATION */
    .short  0                   /* DllCharacteristics */
    .quad   0                   /* SizeOfStackReserve */
    .quad   0                   /* SizeOfStackCommit */
    .quad   0                   /* SizeOfHeapReserve */
    .quad   0                   /* SizeOfHeapCommit */
    .long   0                   /* LoaderFlags */
    .long   6                   /* NumberOfRvaAndSizes */

    /* Data directories (6 entries, all zero) */
    .quad   0                   /* Export Table */
    .quad   0                   /* Import Table */
    .quad   0                   /* Resource Table */
    .quad   0                   /* Exception Table */
    .quad   0                   /* Certificate Table */
    .quad   0                   /* Base Relocation Table */

section_table:
    /* .text section */
    .ascii  ".text\0\0\0"       /* Name (8 bytes) */
    .long   _kernel_size        /* VirtualSize */
    .long   0x10000             /* VirtualAddress (64KB) */
    .long   _kernel_code_size   /* SizeOfRawData (code size, excludes 64KB header) */
    .long   0x10000             /* PointerToRawData (64KB) */
    .long   0                   /* PointerToRelocations */
    .long   0                   /* PointerToLineNumbers */
    .short  0                   /* NumberOfRelocations */
    .short  0                   /* NumberOfLineNumbers */
    .long   0x60000020          /* Characteristics: CNT_CODE | MEM_READ | MEM_EXECUTE */

    /* .data section (dummy, required) */
    .ascii  ".data\0\0\0"       /* Name (8 bytes) */
    .long   0                   /* VirtualSize */
    .long   0                   /* VirtualAddress */
    .long   0                   /* SizeOfRawData */
    .long   0                   /* PointerToRawData */
    .long   0                   /* PointerToRelocations */
    .long   0                   /* PointerToLineNumbers */
    .short  0                   /* NumberOfRelocations */
    .short  0                   /* NumberOfLineNumbers */
    .long   0xc0000040          /* Characteristics: CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE */

    /*
     * Pad to 64KB (0x10000) to match SizeOfHeaders.
     * This ensures real_start is at file offset 0x10000.
     */
    .align  16
    .space  0x10000 - (. - _start)

real_start:
    /*
     * Save DTB address (x0) to callee-saved register.
     * QEMU passes DTB pointer in x0, which we preserve for kernel_main.
     */
    mov     x19, x0

    /*
     * Ensure we're running on the primary CPU (core 0).
     * Secondary cores will spin in a wait loop.
     * MPIDR_EL1 bits [7:0] contain the CPU ID (Aff0).
     */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, primary_cpu
    b       secondary_spin

primary_cpu:
    /*
     * Set up the stack pointer.
     * Stack grows downward, so point to top of stack area.
     */
    ldr     x1, =__stack_top
    mov     sp, x1

    /*
     * Enable FPU and SIMD (NEON).
     * CPACR_EL1 bits [21:20] control access to SIMD/FP:
     *   00 = trap all access
     *   01 = trap EL0 access only
     *   10 = trap all access
     *   11 = no trapping
     * We set bits [21:20] = 11 to enable full access.
     */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * Clear the BSS section.
     * BSS contains uninitialized global/static variables.
     */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
bss_clear:
    cmp     x1, x2
    b.ge    bss_done
    str     xzr, [x1], #8
    b       bss_clear
bss_done:

    /*
     * Disable interrupts while we initialize.
     * DAIF bits: D=Debug, A=SError, I=IRQ, F=FIQ
     * Setting these bits masks (disables) the corresponding exceptions.
     */
    msr     daifset, #0xF

    /*
     * Set up exception vector table.
     * VBAR_EL1 holds the base address of the vector table.
     */
    ldr     x1, =exception_vectors
    msr     vbar_el1, x1
    isb

    /*
     * Jump to C code.
     * Pass DTB address as first argument (x0).
     * kernel_main() should not return, but if it does,
     * we fall through to the halt loop.
     */
    mov     x0, x19             /* DTB address from saved register */
    bl      kernel_main

    /* Fall through to halt if kernel_main returns */

halt:
    /*
     * Halt the CPU.
     * WFI = Wait For Interrupt, puts CPU in low-power state.
     */
    wfi
    b       halt

secondary_spin:
    /*
     * Secondary CPUs wait here.
     * In Phase 2, we'll implement proper multi-core bring-up.
     */
    wfe                         /* Wait For Event */
    b       secondary_spin

/*
 * Note: Exception vector table is now in vectors.S
 */
