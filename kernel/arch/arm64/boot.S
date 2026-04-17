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
    .long   _kernel_code_size   /* SizeOfCode — code + rodata only */
    .long   _kernel_data_size   /* SizeOfInitializedData — initialized .data */
    .long   0                   /* SizeOfUninitializedData — .bss handled via .data VirtualSize delta */
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
    .long   _kernel_size        /* SizeOfImage — highest VA = _kernel_size
                                 * (headers VA 0..0x10000, .text 0x10000..data_va,
                                 *  .data data_va..data_va+data_vsz = _kernel_size). */
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
    /*
     * .text — code + rodata. RX only (no MEM_WRITE). Modern UEFI
     * enforces W^X, so anything marked executable is mapped
     * read-only regardless of MEM_WRITE; splitting .text from .data
     * lets .data actually get RW without losing execute on .text.
     */
    .ascii  ".text\0\0\0"       /* Name (8 bytes) */
    .long   _kernel_code_size   /* VirtualSize */
    .long   0x10000             /* VirtualAddress (64KB) */
    .long   _kernel_code_size   /* SizeOfRawData */
    .long   0x10000             /* PointerToRawData (64KB) */
    .long   0                   /* PointerToRelocations */
    .long   0                   /* PointerToLineNumbers */
    .short  0                   /* NumberOfRelocations */
    .short  0                   /* NumberOfLineNumbers */
    .long   0x60000020          /* Characteristics: CNT_CODE | MEM_READ | MEM_EXECUTE */

    /*
     * .data — initialized data + BSS + stack. RW, no execute.
     *
     * SizeOfRawData covers only the initialized portion
     * (__data_start..__data_end). VirtualSize extends through
     * __kernel_end so UEFI zero-fills the BSS and stack range when
     * mapping .data into memory — which both (a) preserves the
     * initial values of globals without requiring an explicit copy
     * pass, and (b) maps the BSS range as RW-NX so primary_cpu's
     * zero-fill loop doesn't fault against UEFI's W^X policy.
     */
    .ascii  ".data\0\0\0"       /* Name (8 bytes) */
    .long   _kernel_data_virtual_size  /* VirtualSize — includes BSS + stack */
    .long   _kernel_data_va     /* VirtualAddress */
    .long   _kernel_data_size   /* SizeOfRawData — initialized portion only */
    .long   _kernel_data_file   /* PointerToRawData */
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
    /* x0 = DTB pointer (or NULL). Continue with normal boot.
     *
     * Mask ALL exceptions now. SLM-OS owns the machine after
     * ExitBootServices, but UEFI's exception vectors are still
     * installed (VBAR_EL2 still points at UEFI/ArmCpuDxe). If UEFI's
     * DAIF state left interrupts unmasked and a stale timer IRQ
     * fires during the Jetson post-EBS setup below, UEFI's handler
     * runs into torn-down Boot Services and hangs silently. Masking
     * here keeps the CPU quiet until `primary_cpu` installs the
     * SLM-OS VBAR_EL1. The mask is done again at primary_cpu for
     * the kexec path, which doesn't pass through this branch.
     */
    msr     daifset, #0xF

    /*
     * No self-relocation. UEFI typically does NOT hand SLM-OS the
     * preferred `0x80000000` ImageBase on Jetson (the address is
     * occupied by UEFI's own code/data), so the earlier trampoline
     * both overwrote UEFI memory and `br`'d into a page that wasn't
     * executable from the EFI-application context. Without a real
     * `.reloc` section UEFI doesn't patch absolute references
     * either, so physically moving the image to the link address
     * didn't restore correctness — it just added a corruption pass.
     * Continue at whatever address UEFI picked. The code that
     * follows is PC-relative (ADRP+ADD, literal pools, direct
     * B/BL) and runs correctly at any load address. Absolute
     * references inside the wider kernel (function-pointer tables,
     * globals) are a separate fix — approach B of the UEFI-direct
     * plan (real `.reloc` + PIC early boot). See
     * docs/jetson-uefi-direct-result.md.
     */

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

#if defined(PI5_IRQ_DIAG)
    /*
     * Phase 1a: EL2 register snapshot for issue #99.
     *
     * Writes diagnostic values to NC region at 0xFFE0FF00..+0x50. MMU
     * is not yet enabled here, so the CPU uses nGnRnE attributes and
     * these writes hit DRAM directly; after MMU enable, the NC mapping
     * exposes them back. Layout must match `struct diag_el2_snapshot`
     * in kernel/include/diag_pi5.h.
     *
     * Scratch: x15 (snapshot base), x16-x17 (values). Preserve x10
     * which holds CurrentEL from the mrs above.
     */
    mov     x15, #0xFF00
    movk    x15, #0xFFE0, lsl #16           /* x15 = 0xFFE0FF00 */

    str     x10, [x15, #0x18]               /* [+0x18] = CurrentEL at entry */

    mrs     x16, midr_el1
    str     x16, [x15, #0x20]               /* [+0x20] = MIDR_EL1 */
    mrs     x16, id_aa64pfr0_el1
    str     x16, [x15, #0x28]               /* [+0x28] = ID_AA64PFR0_EL1 */
#endif

    cmp     x10, #8                 /* EL2 = 0b1000 (CurrentEL bits [3:2] = 2) */
    b.ne    .Lpi5_in_el1            /* Already in EL1, skip transition */

    /* We're in EL2 - configure and transition to EL1 */

#if defined(PI5_IRQ_DIAG)
    /* HCR_EL2 pre-our-write (still in EL2 here, so mrs is legal). */
    mrs     x16, hcr_el2
    str     x16, [x15, #0x00]               /* [+0x00] = HCR_EL2 pre */
#endif

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

#if defined(PI5_IRQ_DIAG)
    str     x10, [x15, #0x10]               /* [+0x10] = CNTHCTL_EL2 as written */
#endif

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

#if defined(PI5_IRQ_DIAG)
    /* Snapshot GICD_IGROUPR[0] pre-our-write. */
    ldr     w16, [x10, #0x080]
    str     x16, [x15, #0x48]               /* [+0x48] = IGROUPR[0] pre */
    /* Snapshot GICD_CTLR pre-our-write (saved again into w14 below for
     * legacy reasons; keep both paths so the existing code is untouched). */
    ldr     w16, [x10, #0x000]
    str     x16, [x15, #0x30]               /* [+0x30] = GICD_CTLR pre */
#endif

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

#if defined(PI5_IRQ_DIAG)
    /*
     * Load-bearing readback: on GICv2 with Security Extensions,
     * non-secure writes to GICD_IGROUPR cannot promote interrupts
     * from Group 0 to Group 1. If this readback returns 0 (or anything
     * other than 0xFFFFFFFF), the writes we just did were no-ops and
     * the FIQ hypothesis for #99 is confirmed.
     */
    dsb     sy
    ldr     w16, [x10, #0x080]
    str     x16, [x15, #0x40]               /* [+0x40] = IGROUPR[0] post-readback */
#endif

    /* Read GICD_CTLR before we modify it (to see what TF-A left) */
    ldr     w14, [x10, #0x000]      /* Save original GICD_CTLR in w14 */

    /* Enable Group 0 and Group 1 in distributor */
    mov     w11, #3                 /* EnableGrp0 | EnableGrp1 (non-secure: only bit 0 effective) */
    str     w11, [x10, #0x000]      /* GICD_CTLR */

    /* Configure CPU interface */
    ldr     x10, =0x107FFFA000      /* GICC base */

#if defined(PI5_IRQ_DIAG)
    /* Snapshot GICC_CTLR pre-our-write. */
    ldr     w16, [x10, #0x000]
    str     x16, [x15, #0x38]               /* [+0x38] = GICC_CTLR pre */
#endif

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

    /* RC BAR3 → MIP0 routing (BAR1 is used by firmware for RP1 MMIO).
     * Linux uses BAR3 for MSI-X inbound capture — confirmed by register dump.
     * PCI address 0xFF_FFFFF000 (device tree value, NOT Circle's 0x0F). */
    ldr     x10, =0x100012403C     /* BAR3_CONFIG_LO */
    ldr     w11, =0xFFFFF01C       /* addr=0xFFFFF000, size=4KB */
    str     w11, [x10]
    ldr     x10, =0x1000124040     /* BAR3_CONFIG_HI */
    mov     w11, #0xFF             /* PCI addr hi = 0xFF (from DT, confirmed by Linux) */
    str     w11, [x10]
    ldr     x10, =0x10001240BC     /* UBUS_BAR3_REMAP */
    ldr     w11, =0x00130001       /* → MIP0 at 0x00130000, access_en=1 */
    str     w11, [x10]
    ldr     x10, =0x10001240C0     /* UBUS_BAR3_REMAP_HI */
    mov     w11, #0x10             /* → 0x10_00130000 */
    str     w11, [x10]
    dsb     sy

    /* Set MISC_CTRL bits that Linux sets (may be needed for BAR3 matching) */
    ldr     x10, =0x1000124008     /* MISC_CTRL */
    ldr     w11, [x10]             /* Read current value */
    orr     w11, w11, #(1 << 12)   /* SCB_ACCESS_EN */
    orr     w11, w11, #(1 << 13)   /* CFG_READ_UR_MODE */
    orr     w11, w11, #(1 << 17)   /* RCB_MPS */
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

#if defined(PI5_IRQ_DIAG)
    /* Snapshot HCR_EL2 after the write. If firmware had IMO/FMO/AMO
     * bits set and our mov-then-msr didn't clear them explicitly, this
     * readback will show it. */
    isb
    mrs     x16, hcr_el2
    str     x16, [x15, #0x08]               /* [+0x08] = HCR_EL2 post */

    /* Mark the snapshot valid so the shell can distinguish "never ran
     * EL2 block" from "ran but all zeros". */
    mov     x16, #0x5EE1
    movk    x16, #0x5EE1, lsl #16
    movk    x16, #0x5EE1, lsl #32
    movk    x16, #0x5EE1, lsl #48
    str     x16, [x15, #0x50]               /* [+0x50] = DIAG_EL2_MAGIC */

    /*
     * Zero the per-CPU exception counters (0xFF60..0xFFDF, 128 bytes)
     * and the FIQ trace (0xFFE0..0xFFEF, 16 bytes). NC memory at a
     * fixed physical address is not cleared on power cycle, so without
     * this the counters read as whatever DRAM happened to hold.
     * Primary CPU only — secondary CPUs must not wipe the primary's
     * collected counts.
     */
    mov     x16, #0xFF60
    movk    x16, #0xFFE0, lsl #16           /* x16 = 0xFFE0FF60 */
    mov     x17, #18                         /* 18 × 8 = 144 bytes */
.Ldiag_zero_loop:
    str     xzr, [x16], #8
    subs    x17, x17, #1
    b.ne    .Ldiag_zero_loop
    dsb     sy
#endif

    /* SCTLR_EL1 - safe initial value (from Circle) */
    mov     x10, #0x0800
    movk    x10, #0x30d0, lsl #16
    msr     sctlr_el1, x10

    /* Set up return to EL1h with interrupts masked */
    mov     x10, #0x3c4             /* EL1h, DAIF masked */
    msr     spsr_el2, x10
    /*
     * PCIe RC BAR3 → MIP0 routing was configured above (must be done from EL2).
     * PCIe RC register writes hang from EL1 — the hardware restricts
     * write access to EL2+. BAR3 routes RP1 MSI-X writes (PCIe addr
     * 0xFF_FFFFF000) to MIP0 (phys 0x10_00130000) for interrupt delivery.
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
    /*
     * ISB required between ELR_EL2/SPSR_EL2 writes and ERET
     * (ARM ARM D.1.21.1) — ensures the exception-return context is
     * observable before the exception return executes.
     */
    isb
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
     *
     * Gated on `CurrentEL == EL2`. Kexec-from-Linux enters at EL2 from
     * TF-A, and UEFI-direct boot on Jetson also enters at EL2+VHE
     * (contrary to PR #226's EL1 conclusion — that was misled by an
     * ArmCpuDxe PC-reporting quirk and DAIF.D masking brk probes; see
     * docs/jetson-uefi-direct-result.md §§5/5a/5b). Both paths take
     * the fall-through branch here.
     *
     * CurrentEL[3:2] encodes the EL: 0b00=EL0, 0b01=EL1, 0b10=EL2,
     * 0b11=EL3. Value 0b1000 = 8 means EL2.
     *
     * HCR_EL2 sequence (Path-2 P3 fix, 2026-04-17):
     * Read-modify-write instead of unconditional overwrite.
     *
     *   Why not overwrite: UEFI at this point has TGE|RW set and
     *   potentially other bits (HCD, TRVM, firmware-specific
     *   virtualization traps). Clobbering those mid-flight while
     *   UEFI's MMU is still configured for the prior regime is
     *   what hung PR #219 / #226 / #239's UEFI-direct probes
     *   (docs/jetson-uefi-direct-result.md §5c).
     *
     *   Why this is safe here: efi_disable_mmu ran just before the
     *   return to boot.S and now actually disables the active EL2
     *   MMU (it's E2H-aware since P3; pre-P3 it wrote the dormant
     *   SCTLR_EL1 when E2H=0 and left the real EL2 MMU running —
     *   that was the silent latent bug). With MMU off, flipping
     *   HCR_EL2.E2H 0→1 no longer tears down an active translation
     *   regime.
     *
     *   What the ORR targets: OR in (E2H|RW|TGE) to guarantee VHE
     *   is on, EL1 is AArch64, and general exceptions route to
     *   EL2. Pre-existing bits (API, APK, AMO, IMO, FMO, HCD, VM,
     *   firmware-specific) are preserved.
     */
    mrs     x10, CurrentEL
    cmp     x10, #8
    b.ne    .Ljetson_post_setup            /* not EL2 → skip EL2 code */

    mrs     x10, hcr_el2
    ldr     x11, =((1 << 34) | (1 << 31) | (1 << 27))
    orr     x10, x10, x11
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

.Ljetson_post_setup:
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
     * Mask ALL exceptions before any stack/BSS/FPU setup.
     *
     * After kexec on Jetson, stale Linux interrupts (timers, peripherals)
     * can fire at any time. Without a valid VBAR, any exception causes an
     * unpredictable crash. This MUST be the first instruction after the
     * primary_cpu: label, before stack/FPU/BSS setup.
     *
     * Note: on non-Pi5 platforms, some code at real_start runs before we
     * reach primary_cpu (EFI detection, UART debug). That code does not
     * allocate a stack or touch BSS, so a spurious exception would only
     * corrupt scratch GPRs — survivable. After this instruction, DAIF is
     * masked until vectors_init() installs a handler.
     *
     * DAIF bits: D=Debug, A=SError, I=IRQ, F=FIQ
     */
    msr     daifset, #0xF

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

/*
 * Note: Exception vector table is now in vectors.S
 */

#if defined(PLATFORM_JETSON_ORIN_NANO)
/*
 * =========================================================================
 * Early EL2 exception vector table — UEFI-direct diagnostic
 * =========================================================================
 *
 * Installed by efi_stub_entry() immediately after a successful
 * ExitBootServices so any synchronous fault during the UEFI→SLM-OS
 * transition (post-EBS efi_prints, efi_disable_mmu, the return to
 * boot.S, the Jetson EL2 block's HCR_EL2 write, UARTC probe,
 * timer disables) lands in this handler instead of bouncing to
 * UEFI's VBAR_EL2 — which on post-EBS firmware points into
 * ArmCpuDxe memory that may have been freed or may depend on Boot
 * Services state that no longer exists. A fault into that handler
 * is the current silent-hang failure mode on the UEFI-direct path
 * (docs/jetson-uefi-direct-result.md §5c).
 *
 * At fault time the handler:
 *   1. Disables EL2 MMU so subsequent MMIO/DRAM accesses don't
 *      recurse through the (potentially broken) translation that
 *      faulted in the first place.
 *   2. Records ESR_EL2, ELR_EL2, FAR_EL2, SPSR_EL2, HCR_EL2,
 *      CurrentEL and a magic signature to `jetson_early_fault_slot`
 *      (BSS, PC-reachable via adrp+add) so a future debugger /
 *      warm-reset recovery path can identify what happened.
 *   3. Emits "!FAULT\r\n" over UARTC (0x0C280000) as a real-time
 *      signal that the handler fired. One attempt per register;
 *      if UARTC MMIO itself faults, SError stays masked by the
 *      automatic PSTATE.DAIF set on exception entry so the async
 *      abort doesn't recurse.
 *   4. WFE-loops forever.
 *
 * Design notes:
 *   - Uses only x10-x12. No stack required — safe even if SP is
 *     corrupted or unmapped.
 *   - VBAR_EL2 requires 0x800 (2KB) alignment; each of the 16
 *     vectors is 0x80 (128) bytes. Total table size: 2048 bytes.
 *   - All 16 vectors funnel to the same handler — we don't care
 *     which class of exception fired, only that one did.
 *   - The scratch slot is in BSS so UEFI's VirtualSize zero-fill
 *     guarantees it starts at zero; the magic signature lets a
 *     reader distinguish "handler fired" from "slot untouched".
 *   - No explicit cache maintenance on the slot writes: the
 *     handler disables EL2 MMU + caches (SCTLR_EL2.M/C/I clear)
 *     BEFORE storing, so writes go through Device-nGnRnE and land
 *     in DRAM directly. A follow-up reader (kexec-from-Linux,
 *     warm-reset dump) sees them without dc civac. If the
 *     MMU-disable step is ever moved AFTER the slot writes, add
 *     `dc civac` + `dsb sy` per store.
 */
.balign 0x800
.global jetson_early_vbar_el2
jetson_early_vbar_el2:
    /* Current EL with SP0 (EL2t) */
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    /* Current EL with SPx (EL2h) — normal case */
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    /* Lower EL using AArch64 */
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    /* Lower EL using AArch32 — not supported */
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault
    .balign 0x80
    b       .Ljetson_early_fault

    .balign 0x80
.Ljetson_early_fault:
    /* Disable EL2 MMU + caches so subsequent accesses bypass
     * translation. Clearing M prevents nested translation faults
     * from the handler's own MMIO/DRAM accesses. Clearing I+C
     * avoids pulling stale cache state. */
    mrs     x10, sctlr_el2
    bic     x10, x10, #(1 << 0)           /* M  */
    bic     x10, x10, #(1 << 2)           /* C  */
    bic     x10, x10, #(1 << 12)          /* I  */
    msr     sctlr_el2, x10
    dsb     sy
    isb

    /* Scratch slot base: adrp+add gives a full 32-bit PC-relative
     * address, reaching BSS regardless of .data/.text distance. */
    adrp    x10, jetson_early_fault_slot
    add     x10, x10, :lo12:jetson_early_fault_slot

    /* Magic "__EL2FAT" — stored so a hexdump / strings(1) on the
     * slot produces the readable string "__EL2FAT" starting at
     * offset 0. Memory layout: addr+0..7 = {'_','_','E','L','2',
     * 'F','A','T'}. For a little-endian STR of a uint64, each
     * halfword in x11 becomes two bytes at addr+(N/8) and
     * addr+(N/8)+1, where N is the low bit of the halfword. The
     * low byte of each halfword lands at the lower address. */
    movz    x11, #0x5F5F                  /* addr+0..1: "__" */
    movk    x11, #0x4C45, lsl #16         /* addr+2..3: "EL" */
    movk    x11, #0x4632, lsl #32         /* addr+4..5: "2F" */
    movk    x11, #0x5441, lsl #48         /* addr+6..7: "AT" */
    str     x11, [x10, #0x00]

    mrs     x11, esr_el2
    str     x11, [x10, #0x08]
    mrs     x11, elr_el2
    str     x11, [x10, #0x10]
    mrs     x11, far_el2
    str     x11, [x10, #0x18]
    mrs     x11, spsr_el2
    str     x11, [x10, #0x20]
    mrs     x11, hcr_el2
    str     x11, [x10, #0x28]
    mrs     x11, CurrentEL
    str     x11, [x10, #0x30]
    dsb     sy

    /* No explicit daifset here: on EL2 exception entry, ARMv8
     * hardware sets PSTATE.DAIF (D, A, I, F all masked). SError is
     * already held, so if the UARTC writes below raise an async
     * abort (CBB firewall, translation), it stays pending for the
     * duration of the WFE loop rather than recursing through this
     * handler. */

    /* Emit "!FAULT\r\n" over UARTC. NS16550 THR at offset 0,
     * 32-bit stride. Single-shot writes — if the wire eats them
     * we get nothing; if the wire accepts them we get a
     * human-visible marker on the serial console. */
    ldr     x11, =0x0C280000
    mov     w12, #'!'
    str     w12, [x11]
    mov     w12, #'F'
    str     w12, [x11]
    mov     w12, #'A'
    str     w12, [x11]
    mov     w12, #'U'
    str     w12, [x11]
    mov     w12, #'L'
    str     w12, [x11]
    mov     w12, #'T'
    str     w12, [x11]
    mov     w12, #'\r'
    str     w12, [x11]
    mov     w12, #'\n'
    str     w12, [x11]
    dsb     sy

.Ljetson_early_fault_hang:
    wfe
    b       .Ljetson_early_fault_hang

.section .bss
.balign 64
.global jetson_early_fault_slot
jetson_early_fault_slot:
    .skip 64                              /* 8 × uint64_t */
.global jetson_early_fault_slot_end
jetson_early_fault_slot_end:

/* Restore the original section the file opened in (`.text.boot`)
 * so any code added below this block lands in the boot text
 * section rather than plain `.text`. Without this, a future edit
 * that appends routines after `#endif` would silently land in the
 * wrong section and potentially outside the kernel's first
 * segment. */
.section .text.boot
#endif /* PLATFORM_JETSON_ORIN_NANO */

