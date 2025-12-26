/*
 * mmu.S - MMU enable sequence for SLM-OS
 *
 * Contains the critical code for enabling the ARM64 MMU.
 * This code must be identity-mapped to work correctly.
 */

.section .text

/*
 * mmu_enable - Enable the MMU and switch to virtual addressing
 *
 * Parameters:
 *   x0 = TTBR1_EL1 value (physical address of L1 table)
 *   x1 = TCR_EL1 value
 *   x2 = MAIR_EL1 value
 *   x3 = SCTLR_EL1 value (with M bit set)
 *
 * This function:
 * 1. Sets up MAIR, TCR, TTBR registers
 * 2. Enables MMU via SCTLR
 * 3. Returns to caller (which must be identity-mapped)
 *
 * IMPORTANT: The caller must ensure identity mapping exists for this code.
 */
.global mmu_enable
.type mmu_enable, @function
mmu_enable:
    /*
     * Disable interrupts during this critical section.
     * We'll re-enable them after MMU is stable.
     */
    msr     daifset, #0xf

    /*
     * Invalidate all TLB entries before we start.
     * This ensures no stale translations exist.
     */
    tlbi    vmalle1
    dsb     nsh
    isb

    /*
     * Invalidate instruction cache.
     * Ensures no stale instructions are cached.
     */
    ic      iallu
    dsb     nsh
    isb

    /*
     * Set MAIR_EL1 - Memory Attribute Indirection Register.
     * Defines the memory type for each attribute index.
     */
    msr     mair_el1, x2
    isb

    /*
     * Set TCR_EL1 - Translation Control Register.
     * Configures page table format, granule size, etc.
     */
    msr     tcr_el1, x1
    isb

    /*
     * Set TTBR0_EL1 and TTBR1_EL1 to the same L1 table.
     *
     * With 39-bit VA addressing:
     * - TTBR0 handles VA where bits [63:39] = 0 (identity mapping)
     * - TTBR1 handles VA where bits [63:39] = all 1s (kernel high addresses)
     * - Both use L1 index from bits [38:30], so same table works for both
     *
     * During MMU enable, we execute from identity-mapped addresses (TTBR0).
     * After enable, kernel code runs from high addresses (TTBR1).
     */
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0
    isb

    /*
     * Barrier before enabling MMU.
     * Ensures all table writes and register updates are visible.
     */
    dsb     ish
    isb

    /*
     * Read current SCTLR_EL1 and merge with requested value.
     * This preserves any bits we don't want to change.
     */
    mrs     x4, sctlr_el1

    /* Set the bits we want: M (MMU), C (data cache), I (instruction cache) */
    orr     x4, x4, x3

    /*
     * CRITICAL: Enable MMU by writing SCTLR_EL1.
     *
     * After this instruction, the next fetch will use virtual addressing.
     * The ISB ensures the CPU sees the new SCTLR before fetching.
     *
     * Because we have identity mapping, the physical address in LR
     * is also a valid virtual address, so the return works.
     */
    msr     sctlr_el1, x4
    isb

    /*
     * MMU is now enabled.
     *
     * At this point:
     * - We're still executing from the identity-mapped region
     * - Both identity mapping (low VA) and kernel mapping (high VA) exist
     * - The caller can now jump to high kernel addresses
     */

    /*
     * Final TLB maintenance after enable.
     */
    tlbi    vmalle1
    dsb     nsh
    isb

    /*
     * Re-enable interrupts.
     */
    msr     daifclr, #0xf

    ret

.size mmu_enable, . - mmu_enable

/*
 * mmu_disable - Disable the MMU (for debugging/recovery)
 *
 * WARNING: This is dangerous! Only use for debugging.
 * Caller must be identity-mapped.
 */
.global mmu_disable
.type mmu_disable, @function
mmu_disable:
    /* Disable interrupts */
    msr     daifset, #0xf

    /* Read SCTLR */
    mrs     x0, sctlr_el1

    /* Clear M, C, I bits */
    mov     x1, #0x1005         /* M | C | I bits */
    bic     x0, x0, x1

    /* Disable MMU */
    msr     sctlr_el1, x0
    isb

    /* Invalidate TLB */
    tlbi    vmalle1
    dsb     nsh
    isb

    ret

.size mmu_disable, . - mmu_disable

/*
 * Utility: Read SCTLR_EL1
 */
.global read_sctlr_el1
.type read_sctlr_el1, @function
read_sctlr_el1:
    mrs     x0, sctlr_el1
    ret
.size read_sctlr_el1, . - read_sctlr_el1

/*
 * Utility: Read TCR_EL1
 */
.global read_tcr_el1
.type read_tcr_el1, @function
read_tcr_el1:
    mrs     x0, tcr_el1
    ret
.size read_tcr_el1, . - read_tcr_el1

/*
 * Utility: Read TTBR1_EL1
 */
.global read_ttbr1_el1
.type read_ttbr1_el1, @function
read_ttbr1_el1:
    mrs     x0, ttbr1_el1
    ret
.size read_ttbr1_el1, . - read_ttbr1_el1
