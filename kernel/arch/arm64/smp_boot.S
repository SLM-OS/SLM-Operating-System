/*
 * smp_boot.S - Secondary CPU boot code for SLM-OS
 *
 * This is the entry point for secondary CPUs when woken via PSCI CPU_ON.
 * PSCI places the context_id (logical CPU ID) in x0.
 *
 * On Pi 5, TF-A drops the secondary CPU to EL2 with MMU off. This code:
 *   1. Performs EL2→EL1 transition (if needed)
 *   2. Enables the MMU using the primary CPU's page tables
 *   3. Sets up per-CPU stack and exception vectors
 *   4. Jumps to secondary_init() in C
 */

.section .text
.global secondary_entry

secondary_entry:
    /* Save logical CPU ID (passed in x0 from PSCI context_id) */
    mov     x19, x0

    /* Mask all exceptions during setup */
    msr     daifset, #0xF

    /*
     * Check if we're at EL2 (Pi 5 via TF-A) or EL1 (QEMU).
     * If EL2, do the EL2→EL1 transition first.
     */
    mrs     x0, CurrentEL
    cmp     x0, #8                  /* EL2 = 0b1000 */
    b.ne    .Lsec_in_el1

    /* --- EL2→EL1 transition (same as boot.S primary path) --- */

    /* Disable coprocessor traps to EL2 */
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    msr     hstr_el2, xzr

    /* Enable EL1 access to physical timer and counter */
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Enable FP/SIMD at EL1 */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0

    /* HCR_EL2: RW=1 (AArch64 EL1) */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /* SCTLR_EL1: safe initial value (MMU off, caches off) */
    mov     x0, #0x0800
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0

    /* Return to EL1h with exceptions masked */
    mov     x0, #0x3c4
    msr     spsr_el2, x0
    adr     x0, .Lsec_in_el1
    msr     elr_el2, x0
    eret

.Lsec_in_el1:
    /* --- Now at EL1 with MMU off --- */

    /*
     * Enable the MMU using the primary CPU's page tables.
     * The primary CPU has already set up MAIR_EL1, TCR_EL1, and
     * the L1 page table. We just need to load the same TTBR values
     * and enable SCTLR_EL1.M.
     *
     * We read the page table base from the primary CPU's TTBR0_EL1
     * saved value (stored at a known location by vmm_init).
     */
    ldr     x0, =secondary_mmu_ttbr
    ldr     x0, [x0]               /* x0 = page table physical address */
    cbz     x0, .Lsec_skip_mmu     /* If 0, MMU was never set up (shouldn't happen) */

    /* Load MAIR and TCR from saved values */
    ldr     x1, =secondary_mmu_mair
    ldr     x1, [x1]
    msr     mair_el1, x1

    ldr     x1, =secondary_mmu_tcr
    ldr     x1, [x1]
    msr     tcr_el1, x1

    /* Set TTBR0 and TTBR1 to the shared L1 table */
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    isb

    /* Enable MMU + caches in SCTLR_EL1 */
    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)      /* M: MMU enable */
    orr     x1, x1, #(1 << 2)      /* C: Data cache enable */
    orr     x1, x1, #(1 << 12)     /* I: Instruction cache enable */
    msr     sctlr_el1, x1
    isb

#if defined(PLATFORM_RASPI5)
    /*
     * Invalidate all data cache lines (L1 + L2) by set/way.
     * Pi 5 only — QEMU's coherency works without this.
     *
     * Pre-MMU reads (secondary_mmu_ttbr, _mair, _tcr above) entered
     * L1/L2 as Non-Shareable lines because the MMU was off. With MMU
     * now on, page table attributes mark memory as Inner Shareable,
     * but the existing L2 lines are NOT retroactively made coherent.
     * The DSU won't snoop these stale lines, so subsequent reads of
     * ANY address that was cached pre-MMU return stale data forever.
     *
     * Fix: invalidate all L1/L2 by set/way. This forces all subsequent
     * reads to miss and refetch through the DSU's coherency protocol.
     * Cost: ~100µs, done once per secondary CPU at boot.
     */

    /* Invalidate L1 data cache (level 0 in CSSELR) */
    mov     x0, #0                  /* Level 0 (L1), data cache */
    msr     csselr_el1, x0
    isb
    mrs     x4, ccsidr_el1          /* Read cache geometry */
    and     x1, x4, #7              /* Line size field */
    add     x1, x1, #4              /* log2(line size in bytes) */
    ubfx    x2, x4, #3, #10        /* Associativity - 1 */
    clz     w3, w2                  /* Way shift = 32 - log2(ways) */
    ubfx    x4, x4, #13, #15       /* NumSets - 1 */
.Lsec_inv_l1_set:
    mov     x5, x2                  /* x5 = way counter */
.Lsec_inv_l1_way:
    lsl     x6, x5, x3             /* Way field */
    lsl     x7, x4, x1             /* Set field */
    orr     x6, x6, x7             /* Combine way + set + level */
    dc      isw, x6                /* Invalidate by set/way */
    subs    x5, x5, #1
    b.ge    .Lsec_inv_l1_way
    subs    x4, x4, #1
    b.ge    .Lsec_inv_l1_set

    /* Invalidate L2 data cache (level 1 in CSSELR) */
    mov     x0, #2                  /* Level 1 (L2), data cache */
    msr     csselr_el1, x0
    isb
    mrs     x4, ccsidr_el1
    and     x1, x4, #7
    add     x1, x1, #4
    ubfx    x2, x4, #3, #10
    clz     w3, w2
    ubfx    x4, x4, #13, #15
.Lsec_inv_l2_set:
    mov     x5, x2
.Lsec_inv_l2_way:
    lsl     x6, x5, x3
    lsl     x7, x4, x1
    orr     x6, x6, x7
    orr     x6, x6, #2             /* Level 1 (L2) */
    dc      isw, x6
    subs    x5, x5, #1
    b.ge    .Lsec_inv_l2_way
    subs    x4, x4, #1
    b.ge    .Lsec_inv_l2_set

    dsb     sy
    isb
#endif /* PLATFORM_RASPI5 */

.Lsec_skip_mmu:
    /*
     * Set up stack for this CPU.
     * Stack = cpu_stacks[cpu_id] + STACK_SIZE
     */
    ldr     x1, =cpu_stacks
    add     x2, x19, #1            /* x2 = cpu_id + 1 */
    lsl     x2, x2, #14            /* x2 = (cpu_id + 1) * 16KB */
    add     sp, x1, x2

    /*
     * Enable FPU and SIMD at EL1 (may have been cleared by EL2→EL1).
     */
    mov     x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * Set up exception vector table (same as primary CPU).
     */
    ldr     x1, =exception_vectors
    msr     vbar_el1, x1
    isb

    /*
     * Jump to C initialization.
     * Pass logical CPU ID as argument.
     */
    mov     x0, x19
    bl      secondary_init

    /* secondary_init should not return */
1:
    wfi
    b       1b

/* External symbols */
.extern cpu_stacks
.extern exception_vectors
.extern secondary_init
.extern secondary_mmu_ttbr
.extern secondary_mmu_mair
.extern secondary_mmu_tcr
