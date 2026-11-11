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
 *   x4 = start of the range written with caches off (PA == VA here)
 *   x5 = end of that range (exclusive)
 *
 * This function:
 * 1. Invalidates [x4, x5) by VA to the PoC (#1197, see below)
 * 2. Sets up MAIR, TCR, TTBR registers
 * 3. Enables MMU via SCTLR
 * 4. Returns to caller (which must be identity-mapped) with the
 *    caller's DAIF restored (#1198)
 *
 * Returns (x0) the number of cache lines invalidated in step 1, so
 * test_vmm.c can check that the loop ran over the whole range.
 *
 * Caches-off invalidate (#1197). Everything the primary CPU writes
 * before this point (the BSS clear and boot stack in boot.S, the
 * globals and static page tables kernel_main and vmm_init fill in, and
 * on UEFI-direct boots the relocation copy) is written with SCTLR.C=0,
 * so the stores go straight to DRAM. Once SCTLR.C=1 the same addresses
 * are read through the Write-Back mapping, and any line for them still
 * held in this CPU's caches (left by the loader, or refilled
 * speculatively in the window before the previous owner turned its
 * caches off) would be hit instead of DRAM. Linux invalidates its
 * caches-off writes for the same reason (arch/arm64/kernel/head.S).
 * vmm_boot_caches_off_range() passes the whole image, _start to
 * __kernel_end, which holds all of those writes.
 *
 * This runs after the last caches-off store: mmu_enable is a leaf and
 * touches no memory, so no store lands between the invalidate and the
 * SCTLR write. It uses DC IVAC by VA only, never set/way (the Jetson
 * UEFI-direct rule; set/way does not reach the PoC there). On
 * Cortex-A76 and A78AE DC IVAC is executed as DC CIVAC, so a dirty
 * stale line would be written back over the caches-off data; the
 * loader-side clean (Pi 5 cold reset, the kexec segment flush, the EFI
 * stub's DC CIVAC) is what rules that out. The invalidate is skipped
 * if SCTLR.C is already set, because a true invalidate would then
 * discard live dirty data.
 *
 * IMPORTANT: The caller must ensure identity mapping exists for this code.
 *
 * PRECONDITION — pre-SMP only. Every TLBI/IC sequence below uses
 * `dsb nsh` (Non-shareable) which only synchronizes against THIS
 * core's page table walker. That is correct here because the only
 * caller, vmm_init, runs on the primary CPU before secondary CPUs
 * are brought up. If this function is ever reused for a runtime
 * remap (NC region change, kexec teardown, etc.) the `dsb nsh` is
 * insufficient — secondaries may continue to walk through their
 * own TLBs caching the previous translations. A reuse path would
 * need to upgrade to `dsb ish` and ensure all secondaries are
 * quiesced or running through the same broadcast invalidate.
 */
.global mmu_enable
.type mmu_enable, @function
mmu_enable:
    /*
     * Mask D, A, I and F during this critical section and restore the
     * caller's DAIF on exit (#1198). This used to end with
     * `msr daifclr, #0xf`, which unmasked all four on the primary
     * before gic_init and timer_init; kernel_main now unmasks them
     * explicitly (D and A after vmm_init, I and F after timer_init).
     */
    mrs     x6, daif
    msr     daifset, #0xf

    /*
     * Invalidate the caches-off range by VA (#1197, see the header).
     * Line size from CTR_EL0.DminLine (log2 of the line size in words).
     */
    mov     x11, #0                         /* lines invalidated */
    mrs     x7, sctlr_el1
    tbnz    x7, #2, .Lmmu_inval_done        /* C already set: skip */
    mrs     x8, ctr_el0
    ubfx    x8, x8, #16, #4                 /* DminLine */
    mov     x9, #4
    lsl     x9, x9, x8                      /* line size in bytes */
    sub     x10, x9, #1
    bic     x4, x4, x10                     /* align start down */
.Lmmu_inval_loop:
    cmp     x4, x5
    b.hs    .Lmmu_inval_end
    dc      ivac, x4
    add     x4, x4, x9
    add     x11, x11, #1
    b       .Lmmu_inval_loop
.Lmmu_inval_end:
    dsb     sy
.Lmmu_inval_done:

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
     * Invalidate TLB after loading new page tables.
     * If the MMU was already enabled (boot.S → vmm_init transition),
     * old TLB entries for L1[3] = 1GB cacheable block persist.
     * Without this, the NC region at 0xFFE00000 is accessed as cacheable.
     *
     * `tlbi vmalle1` is not the Inner-Shareable broadcast variant, so
     * `dsb nsh` is sufficient to order against this core's own page
     * table walker — matching the `dsb nsh` used for the earlier
     * `tlbi vmalle1` above. We are still pre-SMP here; secondary CPUs
     * are started only after vmm_init() completes.
     */
    tlbi    vmalle1
    dsb     nsh
    isb

    /*
     * Read current SCTLR_EL1 and merge with requested value.
     * This preserves any bits we don't want to change.
     */
    mrs     x7, sctlr_el1

    /* Set the bits we want: M (MMU), C (data cache), I (instruction cache) */
    orr     x7, x7, x3

    /*
     * CRITICAL: Enable MMU by writing SCTLR_EL1.
     *
     * After this instruction, the next fetch will use virtual addressing.
     * The ISB ensures the CPU sees the new SCTLR before fetching.
     *
     * Because we have identity mapping, the physical address in LR
     * is also a valid virtual address, so the return works.
     */
    msr     sctlr_el1, x7
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
     * Restore the caller's DAIF (#1198).
     */
    msr     daif, x6

    mov     x0, x11                         /* lines invalidated */
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

    /* Clear SCTLR_EL1.{M, C, I} = bits {0, 2, 12}.
     * 0x1005 = (1<<12) | (1<<2) | (1<<0) — spell it out so a future
     * reader doesn't have to decode the hex. */
    mov     x1, #((1<<0) | (1<<2) | (1<<12))    /* M | C | I */
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
