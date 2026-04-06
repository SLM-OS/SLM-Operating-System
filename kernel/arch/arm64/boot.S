/*
 * boot.S - SLM-OS entry point for ARM64
 *
 * This is the first code executed when the kernel is loaded.
 *
 * For QEMU: Loads at 0x40000000 with x0 = DTB address
 * For Jetson: Loaded by UEFI at runtime-determined address with x0 = DTB address
 * For Pi 5: Loaded at 0x80000 by firmware with x0 = DTB address
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

#if !defined(PLATFORM_RASPI5)
/*
 * ARM64 Linux Image Header with PE/COFF stub
 *
 * This header is required for UEFI boot on ARM64 (Jetson, etc.).
 * UEFI requires PE/COFF format, so we include a minimal PE header.
 * Format follows Linux kernel's arch/arm64/kernel/efi-header.S
 *
 * PE/COFF requires "MZ" at offset 0, and PE header pointer at offset 0x3C.
 * UEFI reads AddressOfEntryPoint from PE header to find the actual start.
 *
 * For direct boot (QEMU -kernel binary), the first instruction must be
 * executable. The ccmp instruction encodes to bytes "MZ.." satisfying both
 * PE/COFF magic requirement and ARM64 executable requirement.
 *
 * NOTE: Pi 5 does not use this header - see PLATFORM_RASPI5 section below.
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

    .quad   0x80000000          /* ImageBase - preferred load address (Jetson RAM base) */
    .long   0x10000             /* SectionAlignment (64KB, like Linux) */
    .long   0x200               /* FileAlignment (512 bytes) */
    .short  0                   /* MajorOperatingSystemVersion */
    .short  0                   /* MinorOperatingSystemVersion */
    .short  1                   /* MajorImageVersion */
    .short  0                   /* MinorImageVersion */
    .short  0                   /* MajorSubsystemVersion */
    .short  0                   /* MinorSubsystemVersion */
    .long   0                   /* Win32VersionValue */
    .long   _kernel_size + 0x10000  /* SizeOfImage (64KB header + full virtual size) */
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
    /* .text section — all code and data */
    .ascii  ".text\0\0\0"       /* Name (8 bytes) */
    .long   _kernel_size        /* VirtualSize */
    .long   0x10000             /* VirtualAddress (64KB) */
    .long   _kernel_code_size   /* SizeOfRawData (file size, excludes 64KB header) */
    .long   0x10000             /* PointerToRawData (64KB) */
    .long   0                   /* PointerToRelocations */
    .long   0                   /* PointerToLineNumbers */
    .short  0                   /* NumberOfRelocations */
    .short  0                   /* NumberOfLineNumbers */
    .long   0xe0000020          /* Characteristics: CNT_CODE | MEM_READ | MEM_WRITE | MEM_EXECUTE */

    /* .data section (dummy, required by PE/COFF but not actually used) */
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

#else /* PLATFORM_RASPI5 */

/*
 * Pi 5 Raw Binary Entry Point
 *
 * Pi 5 uses a raw binary format loaded at 0x80000 by the firmware.
 * No PE/COFF header is needed - the firmware jumps directly to the
 * first byte of the kernel image.
 *
 * By making _start = real_start (same address), all symbol offset
 * calculations work correctly. The literal pool uses offsets relative
 * to _start, which now points to the actual start of the executable code.
 *
 * This approach follows Circle's proven raw binary format for Pi 5.
 */
_start:
real_start:

#endif /* PLATFORM_RASPI5 */


#if !defined(PLATFORM_RASPI5)
    /*
     * EFI Boot Detection
     *
     * When launched as a UEFI application, UEFI calls this entry with:
     *   x0 = EFI_HANDLE (image handle)
     *   x1 = EFI_SYSTEM_TABLE* (system table pointer)
     *
     * When launched via kexec:
     *   x0 = DTB pointer
     *   x1 = 0 or undefined
     *
     * Detect EFI by checking if x1 points to a valid EFI_SYSTEM_TABLE
     * (signature "IBI SYST" = 0x5453595320494249 at the start).
     * If EFI: call efi_stub_entry() which handles ExitBootServices,
     *         disables MMU, and returns the DTB pointer in x0.
     * If kexec: x0 already contains the DTB pointer, continue normally.
     */
    cbz     x1, .Lnot_efi              /* x1 == NULL → definitely not EFI */
    ldr     x10, =0x5453595320494249    /* EFI_SYSTEM_TABLE_SIGNATURE */
    ldr     x11, [x1]                   /* Read first 8 bytes at x1 */
    cmp     x10, x11
    b.ne    .Lnot_efi                   /* Signature mismatch → kexec boot */

    /*
     * EFI boot detected. x0=handle, x1=system_table.
     * UEFI provides a valid stack, so we can call C code directly.
     * efi_stub_entry() will:
     *   1. Find DTB in the EFI configuration table
     *   2. Call ExitBootServices to take over the machine
     *   3. Disable MMU and clean caches
     *   4. Return the DTB pointer (or NULL) in x0
     */
    bl      efi_stub_entry
    /* x0 now contains DTB pointer (or NULL). Continue with normal boot. */

    /*
     * Self-Relocating Trampoline
     *
     * UEFI loads the PE/COFF image at an arbitrary address, but the
     * kernel is linked at 0x80000000. GOT entries and all absolute
     * addresses in C code assume the link address. Copy the entire
     * image to 0x80000000 and jump there so everything works.
     *
     * Preconditions (established by efi_stub_entry):
     *   - ExitBootServices completed, we own all memory
     *   - MMU disabled, caches cleaned and disabled
     *   - x0 = DTB pointer (or NULL)
     *
     * The kexec path never reaches here (cbz x1 skips to .Lnot_efi).
     */
    mov     x20, x0                /* Save DTB pointer */

    adr     x5, _start             /* x5 = actual load address of _start */
    ldr     x4, .Llink_address     /* x4 = 0x80000000 (link address) */
    cmp     x5, x4
    b.eq    .Lreloc_done           /* Already at link address, skip copy */

    /* Copy image from UEFI load address to link address */
    ldr     x8, .Limage_size       /* x8 = _kernel_file_size (text+rodata+data) */
    mov     x2, x5                 /* x2 = source pointer */
    mov     x1, x4                 /* x1 = destination pointer */
    add     x3, x8, #15
    and     x3, x3, #~15          /* x3 = size rounded up to 16-byte alignment */

.Lreloc_copy:
    cbz     x3, .Lreloc_copy_done
    ldp     x6, x7, [x2], #16
    stp     x6, x7, [x1], #16
    sub     x3, x3, #16
    b       .Lreloc_copy

.Lreloc_copy_done:
    /*
     * Invalidate instruction cache. With MMU and D-cache off
     * (SCTLR_EL2.C=0, I=0 from efi_disable_mmu), the stp writes went
     * directly to memory. But stale I-cache lines from UEFI's previous
     * execution at 0x80000000 must be flushed before we jump there.
     */
    ic      ialluis
    dsb     nsh
    isb

    /*
     * Jump to the copy at 0x80000000.
     * Compute: target = link_address + (.Lpost_reloc - _start)
     */
    adr     x9, .Lpost_reloc
    sub     x9, x9, x5            /* offset of .Lpost_reloc from _start */
    add     x10, x4, x9           /* address in the copy */
    br      x10                    /* absolute branch to relocated code */

.Lpost_reloc:
    /* Now running at link address. All GOT/absolute addresses are correct. */

.Lreloc_done:
    mov     x0, x20                /* Restore DTB pointer */

.Lnot_efi:
#endif /* !PLATFORM_RASPI5 */

    /*
     * Early debug: Write characters to UART to confirm we reached real_start.
     *
     * NOTE: On Jetson after kexec, early UART access causes RAS errors because
     * peripheral clocks/power are not configured. Skip early debug for Jetson
     * and rely on C code UART initialization instead.
     *
     * QEMU PL011: 0x09000000 - works immediately, no clock setup needed.
     * Pi 5 PL011: 0x1F00030000 - via RP1, should work after firmware init.
     */
#if defined(PLATFORM_QEMU_VIRT)
    ldr     x10, =0x09000000        /* QEMU PL011 base */
    mov     w11, #'S'               /* 'S' for Start */
    str     w11, [x10]              /* Write to THR */
    mov     w11, #'\n'
    str     w11, [x10]
#elif defined(PLATFORM_RASPI5)
    /*
     * Pi 5 enters from firmware in EL2. We must transition to EL1 before
     * peripheral access will work. This follows Circle's startup64.S approach.
     */

    /* Check current exception level */
    mrs     x10, CurrentEL
    cmp     x10, #8                 /* EL2 = 0b1000 (CurrentEL bits [3:2] = 2) */
    b.ne    .Lpi5_in_el1            /* Already in EL1, skip transition */

    /* We're in EL2 - configure and transition to EL1 */

    /* Disable coprocessor traps to EL2 */
    mov     x10, #0x33ff
    msr     cptr_el2, x10
    msr     hstr_el2, xzr

    /* Enable EL1 access to physical timer and counter registers.
     * CNTHCTL_EL2: bit 0 (EL1PCTEN) = allow CNTPCT_EL0 reads
     *              bit 1 (EL1PCEN)   = allow CNTP_* timer access
     * Without this, any physical timer access from EL1 traps to EL2
     * and hangs (no EL2 exception handler installed). */
    mov     x10, #3
    msr     cnthctl_el2, x10
    msr     cntvoff_el2, xzr        /* No virtual offset */

    /*
     * Configure GIC-400 interrupt groups from EL2.
     *
     * The Pi 5 firmware loads the kernel without running Circle's armstub8.S,
     * so the GIC group registers (GICD_IGROUPR) may default to 0 (Group 0).
     * Group 0 interrupts signal as FIQ, not IRQ. EL1 code unmasks IRQ (DAIF-I)
     * but not FIQ, so interrupts are never delivered.
     *
     * Fix: set all interrupts to Group 1 (signaled as IRQ to non-secure EL1).
     * Also enable both groups in the distributor and configure the CPU interface
     * to deliver Group 1 as IRQ. This mirrors what armstub8.S does from EL3.
     *
     * GIC-400 base addresses (from device tree):
     *   GICD: 0x107FFF9000
     *   GICC: 0x107FFFA000
     */
    ldr     x10, =0x107FFF9000      /* GICD base */

    /* Read GICD_TYPER to get number of interrupt lines */
    ldr     w11, [x10, #0x004]      /* GICD_TYPER */
    and     w11, w11, #0x1F         /* ITLinesNumber field */
    add     w11, w11, #1            /* Number of 32-IRQ groups */

    /* Set all GICD_IGROUPR registers to 0xFFFFFFFF (all Group 1) */
    mov     x12, x10
    add     x12, x12, #0x080       /* GICD_IGROUPR0 offset */
    mov     w13, #0
.Lgroup_loop:
    mov     w14, #0xFFFFFFFF
    str     w14, [x12, x13, lsl #2]
    add     w13, w13, #1
    cmp     w13, w11
    b.lt    .Lgroup_loop

    /* Read GICD_CTLR before we modify it (to see what TF-A left) */
    ldr     w14, [x10, #0x000]      /* Save original GICD_CTLR in w14 */

    /* Enable Group 0 and Group 1 in distributor */
    mov     w11, #3                 /* EnableGrp0 | EnableGrp1 (non-secure: only bit 0 effective) */
    str     w11, [x10, #0x000]      /* GICD_CTLR */

    /* Configure CPU interface */
    ldr     x10, =0x107FFFA000      /* GICC base */
    mov     w11, #0xFF
    str     w11, [x10, #0x004]      /* GICC_PMR = 0xFF (allow all) */
    /* Enable Group 0+1 with bypass disable bits.
     * AckCtl (bit 2) removed — it interferes with TF-A's Group 0 handling. */
    ldr     w11, =0x1E3             /* EnableGrp0+1, bypass disables, no AckCtl */
    str     w11, [x10, #0x000]      /* GICC_CTLR */

    /*
     * Configure PCIe→MIP→GIC interrupt path from EL2 (writes to RC
     * and MIP registers require EL2 on BCM2712).
     */

    /* MSI-X enable in RP1 PCIe config (via RC EXT_CFG at +0x8000) */
    ldr     x10, =0x1000129000     /* EXT_CFG_INDEX */
    str     wzr, [x10]             /* bus=0, devfn=0 */
    dsb     sy
    ldr     x10, =0x10001280B0     /* EXT_CFG_DATA + 0xB0 (MSI-X cap) */
    ldr     w11, [x10]
    orr     w11, w11, #(1 << 31)   /* Enable (bit 15 of upper 16) */
    bic     w11, w11, #(1 << 30)   /* Clear Function Mask */
    str     w11, [x10]
    dsb     sy

    /* RC BAR1 → MIP0 routing */
    ldr     x10, =0x100012402C
    ldr     w11, =0xFFFFF01C
    str     w11, [x10]
    ldr     x10, =0x1000124030
    mov     w11, #0x0F             /* BAR1 PCIe addr hi (Circle: 0x0F, not 0xFF) */
    str     w11, [x10]
    ldr     x10, =0x10001240AC
    ldr     w11, =0x00130001
    str     w11, [x10]
    ldr     x10, =0x10001240B0
    mov     w11, #0x10
    str     w11, [x10]
    dsb     sy

    /* MIP0: unmask UART0 (vec 25), mask VPU, edge-triggered */
    mvn     w11, wzr
    ldr     x10, =0x1000130040     /* MASKL_HOST */
    mov     w12, w11
    bic     w12, w12, #(1 << 25)
    str     w12, [x10]
    ldr     x10, =0x1000130050     /* MASKH_HOST */
    str     w11, [x10]
    ldr     x10, =0x1000130060     /* MASKL_VPU */
    str     w11, [x10]
    ldr     x10, =0x1000130070     /* MASKH_VPU */
    str     w11, [x10]
    ldr     x10, =0x1000130020     /* CFGL_HOST */
    str     w11, [x10]
    ldr     x10, =0x1000130030     /* CFGH_HOST */
    str     w11, [x10]
    dsb     sy

    /* Enable FP/SIMD at EL1 */
    mov     x10, #(3 << 20)
    msr     cpacr_el1, x10

    /* Initialize HCR_EL2 for 64-bit EL1 */
    mov     x10, #(1 << 31)         /* RW bit = 1 for AArch64 */
    msr     hcr_el2, x10

    /* SCTLR_EL1 - safe initial value (from Circle) */
    mov     x10, #0x0800
    movk    x10, #0x30d0, lsl #16
    msr     sctlr_el1, x10

    /* Set up return to EL1h with interrupts masked */
    mov     x10, #0x3c4             /* EL1h, DAIF masked */
    msr     spsr_el2, x10
    /*
     * Configure PCIe RC BAR1 → MIP0 routing (must be done from EL2).
     * PCIe RC register writes hang from EL1 — the hardware restricts
     * write access to EL2+. This routes RP1 MSI-X writes (PCIe addr
     * 0xFF_FFFFF000) to MIP0 (phys 0x10_00130000) for interrupt delivery.
     *
     * RC base: 0x10_00120000 (pcie2 for RP1)
     */
    /*
     * Enable MSI-X in RP1 PCIe config space (requires EL2 for writes).
     * EXT_CFG: write bus/devfn to RC+0x9000, data at RC+0x8000+offset.
     * MSI-X cap is at offset 0xB0. Message Control is at 0xB0+2 (in
     * the upper 16 bits of the 32-bit word at 0xB0).
     * Set Enable (bit 15) but NOT Function Mask (bit 14).
     */
    /* PCIe INTA approach for RP1 interrupts (no MSI-X/BAR1/MIP needed).
     * RP1 MSIX_CFG and GIC configuration done from EL1 in uart_irq_init(). */

    adr     x10, .Lpi5_in_el1
    msr     elr_el2, x10
    eret

.Lpi5_in_el1:
    /* Now in EL1 - synchronize before continuing */
    dsb     sy
    isb

#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * EL2 UART PROBE: Test peripheral access from EL2.
     *
     * Previous tests confirmed:
     *   - SLM-OS enters at EL2 after kexec (via PSCI SYSTEM_OFF probe)
     *   - UARTA (0x03100000) write from EL2 triggers CBB error
     *
     * This test tries UARTC (0x0C280000) — the peripheral a forum user
     * successfully accessed from EL2 on Jetson Orin. Different UARTs
     * may be in different CBB firewall domains.
     *
     * Also tests UARTD (0x031D0000) as a third option.
     *
     * NS16550 THR at offset 0, reg-shift=2 (4-byte spacing).
     * UART clocks: UARTA needs /dev/ttyTHS1 open before kexec.
     *              UARTC clock status unknown (may need BPMP enablement).
     *
     * If any write succeeds, characters appear on whatever serial adapter
     * is monitoring that UART. If CBB blocks, EL3 logs the RAS error.
     */

    /*
     * Enable VHE (Virtual Host Extensions) at EL2.
     *
     * With VHE, the CPU transparently redirects EL1 register names to EL2:
     *   SCTLR_EL1 → SCTLR_EL2,  VBAR_EL1 → VBAR_EL2
     *   TTBR0_EL1 → TTBR0_EL2,  TTBR1_EL1 → TTBR1_EL2
     *   TCR_EL1 → TCR_EL2,      MAIR_EL1 → MAIR_EL2
     *   CPACR_EL1 → CPTR_EL2    (with CPACR-compatible format)
     *
     * This means the entire existing kernel code works unmodified at EL2.
     * Exceptions are taken to EL2 (TGE=1), not EL1.
     *
     * HCR_EL2 bits:
     *   E2H (bit 34): Enable EL1→EL2 register redirection
     *   TGE (bit 27): Trap General Exceptions to EL2
     *   RW  (bit 31): EL1 is AArch64
     */
    ldr     x10, =((1 << 34) | (1 << 31) | (1 << 27))
    msr     hcr_el2, x10
    isb

    /*
     * Disable ARM generic timer to prevent stale Linux timer IRQs.
     * After kexec, the timer compare value from Linux may fire soon,
     * causing a pending interrupt that cascades through the GIC and
     * triggers CBB/RAS errors. Disable all timer controls early.
     *
     * With VHE (E2H=1), CNTP_CTL_EL0 and CNTV_CTL_EL0 control the
     * host physical and virtual timers respectively.
     */
    msr     cntp_ctl_el0, xzr      /* Disable physical timer */
    msr     cntv_ctl_el0, xzr      /* Disable virtual timer */
    msr     cnthp_ctl_el2, xzr     /* Disable hypervisor physical timer */
    isb

    /* Quick UARTC confirmation — write "EL2\r\n" to verify serial works */
    ldr     x11, =0x0C280000       /* UARTC base (accessible from EL2) */
    mov     w12, #'E'
    str     w12, [x11]
    mov     w12, #'L'
    str     w12, [x11]
    mov     w12, #'2'
    str     w12, [x11]
    mov     w12, #'\r'
    str     w12, [x11]
    mov     w12, #'\n'
    str     w12, [x11]
#endif

    /*
     * Save DTB address (x0) to callee-saved register.
     * QEMU passes DTB pointer in x0, which we preserve for kernel_main.
     */
    mov     x19, x0

    /*
     * NOTE: After kexec, we may not be on CPU 0. For testing, skip the
     * MPIDR check and continue on whatever CPU we landed on.
     * TODO: Properly handle SMP bring-up after kexec.
     */
#if defined(PLATFORM_RASPI5)
    /* Pi 5 BCM2712: CPU ID is in MPIDR Aff1 (bits 15:8), not Aff0.
     * MPIDR values: CPU0=0x000, CPU1=0x100, CPU2=0x200, CPU3=0x300 */
    mrs     x1, mpidr_el1
    lsr     x1, x1, #8
    and     x1, x1, #0xFF
    cbz     x1, primary_cpu
    b       secondary_spin
#elif defined(PLATFORM_QEMU_VIRT)
    /* QEMU virt: CPU ID is in MPIDR Aff0 (bits 7:0) */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, primary_cpu
    b       secondary_spin
#endif
    /* On Jetson, skip MPIDR check - may not be on CPU 0 after kexec */

primary_cpu:
    /*
     * Set up the stack pointer.
     * Stack grows downward, so point to top of stack area.
     *
     * UEFI loads us at an arbitrary address. We must calculate addresses
     * relative to our actual load address, not the linker's assumed base.
     * Use adr to get the actual address of _start, then add known offsets.
     */
    adr     x0, _start                      /* x0 = actual runtime address of _start */
    ldr     x1, .Lstack_offset              /* x1 = offset from _start to __stack_top */
    add     x1, x0, x1                      /* x1 = actual __stack_top address */
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
     * Calculate addresses relative to actual _start location.
     */
    adr     x0, _start                      /* x0 = actual runtime address of _start */
    ldr     x1, .Lbss_start_offset          /* x1 = offset to __bss_start */
    ldr     x2, .Lbss_end_offset            /* x2 = offset to __bss_end */
    add     x1, x0, x1                      /* x1 = actual __bss_start */
    add     x2, x0, x2                      /* x2 = actual __bss_end */
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
     * Calculate address relative to actual _start location.
     */
    adr     x0, _start                          /* x0 = actual runtime address of _start */
    ldr     x1, .Lvectors_offset                /* x1 = offset to exception_vectors */
    add     x1, x0, x1                          /* x1 = actual exception_vectors address */
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
 * Literal pool for position-independent addressing.
 * These offsets are calculated by the linker and used to compute
 * actual runtime addresses when UEFI loads us at an arbitrary location.
 */
    .align  3
.Lstack_offset:
    .quad   __stack_top - _start
.Lbss_start_offset:
    .quad   __bss_start - _start
.Lbss_end_offset:
    .quad   __bss_end - _start
.Lvectors_offset:
    .quad   exception_vectors - _start
.Llink_address:
    .quad   0x80000000
.Limage_size:
    .quad   _kernel_file_size

/*
 * Note: Exception vector table is now in vectors.S
 */

