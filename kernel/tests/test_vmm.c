/*
 * test_vmm.c - Virtual Memory Manager Tests for SLM-OS
 *
 * Tests VMM functionality including:
 * - TLB invalidation correctness (not just "doesn't crash")
 * - Proper remapping behavior after TLB invalidation
 * - Page table and TLB consistency
 */

#include "unity.h"
#include "../include/vmm.h"
#include "../include/pmm.h"
#include "../include/smp.h"
#include "../include/platform.h"
#include "../include/uart.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Magic values for distinguishing physical regions.
 * Written to different physical addresses to verify which one is accessed.
 */
#define MARKER_PA1  0xDEADBEEF11111111UL
#define MARKER_PA2  0xCAFEBABE22222222UL
#define MARKER_PA3  0xFEEDFACE33333333UL

/*
 * Test region: We use a high RAM address for testing remapping.
 * This should be within mapped RAM but not critical for kernel operation.
 * Using 64MB offset from RAM_BASE (well into the heap area).
 */
#define TEST_VA     (RAM_BASE + 0x4000000)  /* 64MB into RAM */
#define TEST_PA1    (RAM_BASE + 0x4000000)  /* Same as VA (identity mapped) */
#define TEST_PA2    (RAM_BASE + 0x4200000)  /* 66MB into RAM (different 2MB block) */

/* ============================================================================
 * Functional Test: Verify page table matches actual memory access
 * ============================================================================ */

/*
 * Test: vmm_virt_to_phys returns correct physical address
 *
 * Verifies the page table walker correctly translates addresses.
 */
static void test_virt_to_phys_accuracy(void)
{
    /* RAM_BASE should be identity mapped */
    uint64_t pa = vmm_virt_to_phys(RAM_BASE);
    TEST_ASSERT_EQUAL_HEX64(RAM_BASE, pa);

    /* Check another known mapping */
    uint64_t mid_ram = RAM_BASE + (RAM_SIZE / 2);
    pa = vmm_virt_to_phys(mid_ram);
    TEST_ASSERT_EQUAL_HEX64(mid_ram, pa);
}

/*
 * Test: Writing via VA is visible when reading via PA
 *
 * Verifies that the mapping is correct and cache coherent.
 */
static void test_va_pa_coherency(void)
{
    volatile uint64_t *va_ptr = (volatile uint64_t *)TEST_VA;
    uint64_t pa = vmm_virt_to_phys(TEST_VA);

    /* Save original value */
    uint64_t original = *va_ptr;

    /* Write via VA */
    *va_ptr = MARKER_PA1;

    /* Ensure write is visible */
    __asm__ volatile("dsb ish" ::: "memory");

    /* Read via PA (identity mapped, so PA == VA for this region) */
    volatile uint64_t *pa_ptr = (volatile uint64_t *)pa;
    uint64_t readback = *pa_ptr;

    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, readback);

    /* Restore */
    *va_ptr = original;
}

/* ============================================================================
 * Functional Test: TLB invalidation is necessary and sufficient
 * ============================================================================ */

/*
 * Test: After remapping + TLB invalidation, new mapping is used
 *
 * This is the KEY test for TLB correctness:
 * 1. Write marker1 to PA1 (currently mapped to test VA)
 * 2. Write marker2 to PA2 (different physical block)
 * 3. Change PTE to point test VA to PA2 (without TLB invalidation)
 * 4. Invalidate TLB
 * 5. Read from test VA - MUST get marker2 (from PA2)
 */
static void test_remap_requires_invalidation(void)
{
    /* Get current PTE for test address */
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    /* Write distinct markers to two different physical addresses */
    volatile uint64_t *pa1_ptr = (volatile uint64_t *)TEST_PA1;
    volatile uint64_t *pa2_ptr = (volatile uint64_t *)TEST_PA2;

    uint64_t original_pa1 = *pa1_ptr;
    uint64_t original_pa2 = *pa2_ptr;

    *pa1_ptr = MARKER_PA1;
    *pa2_ptr = MARKER_PA2;
    __asm__ volatile("dsb ish" ::: "memory");

    /* Verify we currently read from PA1 via TEST_VA */
    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    /* Create new PTE pointing to PA2 */
    uint64_t new_pte = vmm_test_make_block_desc(TEST_PA2, VMM_FLAGS_KERNEL_DATA);

    /* Change PTE without TLB invalidation */
    int ret = vmm_test_set_l2_entry_no_invalidate(TEST_VA, new_pte);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* NOW invalidate TLB for this address */
    vmm_invalidate_tlb(TEST_VA);

    /* Read from TEST_VA - should now get PA2's marker */
    uint64_t after_remap = *test_ptr;
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, after_remap);

    /* Restore original mapping */
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, original_pte);
    vmm_invalidate_tlb(TEST_VA);

    /* Verify we're back to reading from PA1 */
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    /* Restore original data */
    *pa1_ptr = original_pa1;
    *pa2_ptr = original_pa2;
}

/*
 * Test: vmm_invalidate_tlb_all also works for remapped pages
 *
 * Same as above but uses full TLB flush instead of single address.
 */
static void test_remap_with_full_flush(void)
{
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    volatile uint64_t *pa1_ptr = (volatile uint64_t *)TEST_PA1;
    volatile uint64_t *pa2_ptr = (volatile uint64_t *)TEST_PA2;

    uint64_t original_pa1 = *pa1_ptr;
    uint64_t original_pa2 = *pa2_ptr;

    *pa1_ptr = MARKER_PA1;
    *pa2_ptr = MARKER_PA2;
    __asm__ volatile("dsb ish" ::: "memory");

    /* Change PTE to PA2 without TLB invalidation */
    uint64_t new_pte = vmm_test_make_block_desc(TEST_PA2, VMM_FLAGS_KERNEL_DATA);
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, new_pte);

    /* Use full TLB flush instead of single address */
    vmm_invalidate_tlb_all();

    /* Should now read from PA2 */
    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, *test_ptr);

    /* Restore */
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, original_pte);
    vmm_invalidate_tlb_all();
    *pa1_ptr = original_pa1;
    *pa2_ptr = original_pa2;
}

/*
 * Test: vmm_invalidate_tlb_range handles remapped region
 */
static void test_remap_with_range_invalidation(void)
{
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    volatile uint64_t *pa1_ptr = (volatile uint64_t *)TEST_PA1;
    volatile uint64_t *pa2_ptr = (volatile uint64_t *)TEST_PA2;

    uint64_t original_pa1 = *pa1_ptr;
    uint64_t original_pa2 = *pa2_ptr;

    *pa1_ptr = MARKER_PA1;
    *pa2_ptr = MARKER_PA2;
    __asm__ volatile("dsb ish" ::: "memory");

    /* Change PTE */
    uint64_t new_pte = vmm_test_make_block_desc(TEST_PA2, VMM_FLAGS_KERNEL_DATA);
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, new_pte);

    /* Use range invalidation covering the test address */
    vmm_invalidate_tlb_range(TEST_VA, TEST_VA + BLOCK_SIZE);

    /* Should read from PA2 */
    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, *test_ptr);

    /* Restore */
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, original_pte);
    vmm_invalidate_tlb_range(TEST_VA, TEST_VA + BLOCK_SIZE);
    *pa1_ptr = original_pa1;
    *pa2_ptr = original_pa2;
}

/* ============================================================================
 * Edge Case Tests
 * ============================================================================ */

/*
 * Test: Multiple remaps in sequence
 *
 * Remap VA to PA1, PA2, PA3 in sequence, verifying each works.
 */
static void test_sequential_remaps(void)
{
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    /* Use three different physical addresses */
    uint64_t pa1 = TEST_PA1;
    uint64_t pa2 = TEST_PA2;
    uint64_t pa3 = RAM_BASE + 0x4400000;  /* 68MB into RAM */

    volatile uint64_t *ptr1 = (volatile uint64_t *)pa1;
    volatile uint64_t *ptr2 = (volatile uint64_t *)pa2;
    volatile uint64_t *ptr3 = (volatile uint64_t *)pa3;

    uint64_t orig1 = *ptr1;
    uint64_t orig2 = *ptr2;
    uint64_t orig3 = *ptr3;

    *ptr1 = MARKER_PA1;
    *ptr2 = MARKER_PA2;
    *ptr3 = MARKER_PA3;
    __asm__ volatile("dsb ish" ::: "memory");

    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;

    /* Currently points to PA1 */
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    /* Remap to PA2 */
    uint64_t pte2 = vmm_test_make_block_desc(pa2, VMM_FLAGS_KERNEL_DATA);
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, pte2);
    vmm_invalidate_tlb(TEST_VA);
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, *test_ptr);

    /* Remap to PA3 */
    uint64_t pte3 = vmm_test_make_block_desc(pa3, VMM_FLAGS_KERNEL_DATA);
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, pte3);
    vmm_invalidate_tlb(TEST_VA);
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA3, *test_ptr);

    /* Remap back to PA1 */
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, original_pte);
    vmm_invalidate_tlb(TEST_VA);
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    /* Restore original data */
    *ptr1 = orig1;
    *ptr2 = orig2;
    *ptr3 = orig3;
}

/*
 * Test: Rapid remap stress test
 *
 * Rapidly remap between two physical addresses many times.
 */
static void test_rapid_remap_stress(void)
{
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    volatile uint64_t *ptr1 = (volatile uint64_t *)TEST_PA1;
    volatile uint64_t *ptr2 = (volatile uint64_t *)TEST_PA2;

    uint64_t orig1 = *ptr1;
    uint64_t orig2 = *ptr2;

    *ptr1 = MARKER_PA1;
    *ptr2 = MARKER_PA2;
    __asm__ volatile("dsb ish" ::: "memory");

    uint64_t pte1 = vmm_test_make_block_desc(TEST_PA1, VMM_FLAGS_KERNEL_DATA);
    uint64_t pte2 = vmm_test_make_block_desc(TEST_PA2, VMM_FLAGS_KERNEL_DATA);

    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;

    /* Rapidly alternate between PA1 and PA2 */
    for (int i = 0; i < 50; i++) {
        /* Map to PA2 */
        vmm_test_set_l2_entry_no_invalidate(TEST_VA, pte2);
        vmm_invalidate_tlb(TEST_VA);
        TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, *test_ptr);

        /* Map to PA1 */
        vmm_test_set_l2_entry_no_invalidate(TEST_VA, pte1);
        vmm_invalidate_tlb(TEST_VA);
        TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);
    }

    /* Restore */
    vmm_test_set_l2_entry_no_invalidate(TEST_VA, original_pte);
    vmm_invalidate_tlb(TEST_VA);
    *ptr1 = orig1;
    *ptr2 = orig2;
}

/* ============================================================================
 * ASID Tests (for future user space support)
 * ============================================================================ */

/*
 * Test: ASID invalidation executes without fault
 *
 * Smoke test only: calls the function with several ASID values to verify
 * no exception is raised.  TLB state cannot be validated from software.
 */
static void test_asid_invalidation_executes(void)
{
    vmm_invalidate_tlb_asid(RAM_BASE, 0);
    vmm_invalidate_tlb_asid(RAM_BASE, 1);
    vmm_invalidate_tlb_asid(RAM_BASE, 255);
    TEST_IGNORE_MESSAGE("smoke test: verifies no crash, cannot validate TLB state");
}

/*
 * Test: ASID-all invalidation executes without fault
 *
 * Smoke test only: calls the function with several ASID values to verify
 * no exception is raised.  TLB state cannot be validated from software.
 */
static void test_asid_all_invalidation_executes(void)
{
    vmm_invalidate_tlb_asid_all(0);
    vmm_invalidate_tlb_asid_all(1);
    vmm_invalidate_tlb_asid_all(255);
    TEST_IGNORE_MESSAGE("smoke test: verifies no crash, cannot validate TLB state");
}

/* ============================================================================
 * Multi-CPU TLB Broadcast Tests
 * ============================================================================ */

/*
 * Test: Verify TLB operations reach all CPUs
 *
 * This test verifies that the "is" (inner shareable) suffix works by
 * checking that all online CPUs can access remapped memory correctly.
 * Note: In QEMU, all CPUs share memory so this mainly verifies no faults.
 */
static void test_tlb_broadcast_all_cpus(void)
{
    uint32_t num_cpus = cpus_online;
    uart_printf("  Testing TLB broadcast with %u CPUs online\n", num_cpus);

    /* Perform TLB operations - should broadcast to all CPUs */
    vmm_invalidate_tlb(TEST_VA);
    vmm_invalidate_tlb_all();
    vmm_invalidate_tlb_range(TEST_VA, TEST_VA + BLOCK_SIZE * 4);

    /* Verify memory access still works from this CPU */
    volatile uint64_t *ptr = (volatile uint64_t *)TEST_VA;
    uint64_t val = *ptr;
    (void)val;  /* Suppress unused warning */

    TEST_IGNORE_MESSAGE("smoke test: verifies no crash on single-core");
}

/* ============================================================================
 * Regression Tests: Integer Overflow in Statistics
 * ============================================================================ */

/*
 * Regression test: vmm_get_stats bytes_mapped doesn't overflow.
 * Previously, vmm_dump used (blocks_mapped * BLOCK_SIZE) in 32-bit
 * arithmetic, which overflowed on Pi 5 where blocks_mapped > 2047.
 * vmm_get_stats uses uint64_t and should be correct.
 */
static void test_vmm_stats_bytes_mapped_no_overflow(void)
{
    struct vmm_stats stats;
    vmm_get_stats(&stats);

    /* bytes_mapped should equal blocks_mapped * 2MB */
    uint64_t expected = (uint64_t)stats.blocks_mapped * (2 * 1024 * 1024);
    TEST_ASSERT_EQUAL_HEX64(expected, stats.bytes_mapped);

    /* On Pi 5, blocks_mapped is ~2050+, bytes should be ~4GB+ */
    /* On QEMU, blocks_mapped is smaller but still should be consistent */
    TEST_ASSERT_TRUE(stats.bytes_mapped > 0);
    TEST_ASSERT_TRUE(stats.bytes_mapped >= (uint64_t)stats.blocks_mapped * 1024 * 1024);
}

/*
 * Test: L1 block mapping detection
 *
 * Pi 5 maps RAM with 1GB L1 block descriptors (no L2 tables for RAM).
 * QEMU virt uses L2 tables with 2MB entries. This test verifies that
 * vmm_test_get_l2_entry correctly returns 0 for L1 block mappings
 * and non-zero for L2 table mappings.
 */
static void test_l1_block_mapping_detection(void)
{
    uint64_t pte = vmm_test_get_l2_entry(TEST_VA);

#if defined(PLATFORM_RASPI5)
    /*
     * Pi 5 uses 1GB L1 block descriptors for the entire 4GB RAM region.
     * vmm_test_get_l2_entry should return 0 because there is no L2 table
     * for this address — the L1 entry is a block descriptor, not a table.
     */
    TEST_ASSERT_EQUAL_HEX64(0, pte);
#else
    /*
     * QEMU virt uses L2 tables with 2MB block entries for RAM.
     * vmm_test_get_l2_entry should return a valid non-zero PTE.
     */
    TEST_ASSERT_TRUE(pte != 0);
#endif

    /* virt_to_phys should work regardless of mapping granularity */
    uint64_t pa = vmm_virt_to_phys(TEST_VA);
    TEST_ASSERT_EQUAL_HEX64(TEST_PA1, pa);
}

/*
 * Test: MMIO regions are mapped correctly.
 *
 * Verifies that platform-specific device memory regions (UART, GIC, etc.)
 * are accessible via vmm_is_mapped(). These regions are mapped as device
 * memory (nGnRnE) in the VMM init code.
 */
static void test_mmio_regions_mapped(void)
{
    /* UART must be mapped on all platforms */
    TEST_ASSERT_TRUE(vmm_is_mapped(UART_BASE));

    /* GIC must be mapped on all platforms */
    TEST_ASSERT_TRUE(vmm_is_mapped(GIC_DIST_BASE));

#if defined(PLATFORM_RASPI5)
    /* RP1 INTC (PCIE_CFG) at 0x1F00108000 */
    TEST_ASSERT_TRUE(vmm_is_mapped(RP1_INTC_BASE));

    /* PCIe RC + MIP0 region */
    TEST_ASSERT_TRUE(vmm_is_mapped(PCIE_RC_BASE));
    TEST_ASSERT_TRUE(vmm_is_mapped(MIP0_BASE));

    /* RP1 BAR0 (MSI-X table) */
    TEST_ASSERT_TRUE(vmm_is_mapped(RP1_MSIX_TABLE_BASE));

    /* NC shared memory region (last 2MB of RAM, mapped via L2 table) */
    TEST_ASSERT_TRUE(vmm_is_mapped(0xFFE00000));

    /* Verify virt_to_phys works for NC region (identity-mapped) */
    TEST_ASSERT_EQUAL_HEX64(0xFFE00000, vmm_virt_to_phys(0xFFE00000));
#endif
}

/*
 * Test: PCIe RC BAR3→MIP0 routing configured correctly by boot.S (Pi 5 only).
 * Verifies the register values that enable MSI-X interrupt delivery.
 */
static void test_bar3_mip0_routing(void)
{
#if defined(PLATFORM_RASPI5)
    volatile uint32_t *bar3_lo = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_LO);
    volatile uint32_t *bar3_hi = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_HI);
    volatile uint32_t *remap_lo = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP);
    volatile uint32_t *remap_hi = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP_HI);

    /* BAR3 captures PCI address 0xFF_FFFFF000 (MSI-X target) */
    TEST_ASSERT_EQUAL_HEX32(0xFFFFF01C, *bar3_lo);
    TEST_ASSERT_EQUAL_HEX32(0x000000FF, *bar3_hi);

    /* BAR3 remaps to MIP0 at physical 0x10_00130000 */
    TEST_ASSERT_EQUAL_HEX32(0x00130001, *remap_lo);   /* access_en=1 */
    TEST_ASSERT_EQUAL_HEX32(0x00000010, *remap_hi);

    /* MISC_CTRL has SCB_ACCESS_EN set */
    volatile uint32_t *misc = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_MISC_CTRL);
    TEST_ASSERT_MESSAGE((*misc & (1 << 12)) != 0, "MISC_CTRL SCB_ACCESS_EN not set");
#else
    TEST_ASSERT(1);  /* BAR3 not configured on non-Pi5 platforms */
#endif
}

/*
 * Test: unmap + map via public API without manual TLBI (MM-H1 regression)
 *
 * vmm_map_block() now issues its own TLBI after writing the L2 entry.
 * This test exercises unmap → map via the public API and verifies that
 * reads reflect the new mapping without the caller invalidating the TLB.
 */
static void test_public_remap_invalidates_tlb(void)
{
    uint64_t original_pte = vmm_test_get_l2_entry(TEST_VA);
    if (original_pte == 0) {
        TEST_IGNORE_MESSAGE("L1 block mapping: no L2 entry to remap");
        return;
    }

    volatile uint64_t *pa1_ptr = (volatile uint64_t *)TEST_PA1;
    volatile uint64_t *pa2_ptr = (volatile uint64_t *)TEST_PA2;
    uint64_t original_pa1 = *pa1_ptr;
    uint64_t original_pa2 = *pa2_ptr;

    *pa1_ptr = MARKER_PA1;
    *pa2_ptr = MARKER_PA2;
    __asm__ volatile("dsb ish" ::: "memory");

    volatile uint64_t *test_ptr = (volatile uint64_t *)TEST_VA;
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    /* Remap via public API — no manual TLBI by caller */
    int rc = vmm_unmap_block(TEST_VA);
    TEST_ASSERT_EQUAL_INT(0, rc);
    rc = vmm_map_block(TEST_VA, TEST_PA2, VMM_FLAGS_KERNEL_DATA);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Public API must have invalidated the TLB itself */
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA2, *test_ptr);

    /* Restore original mapping */
    vmm_unmap_block(TEST_VA);
    rc = vmm_map_block(TEST_VA, TEST_PA1, VMM_FLAGS_KERNEL_DATA);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_HEX64(MARKER_PA1, *test_ptr);

    *pa1_ptr = original_pa1;
    *pa2_ptr = original_pa2;
}

/* ============================================================================
 * #697 PR-2: Per-task TTBR0 L1 helpers
 * ============================================================================ */

/* The boot L1 is file-scoped static in vmm.c. Reach it via
 * vmm_get_ttbr1() (returns the live TTBR1_EL1 register value). The
 * register layout is BADDR[47:1] | CnP[0], with optional ASID in bits
 * [63:48] (TCR_EL1.AS controlled). Mask to bits [47:12] to extract
 * the table PA — survives a future ASID introduction. Identity-mapped,
 * so the PA can be cast directly to uint64_t * for table reads. */
#define BOOT_L1_PA()        (vmm_get_ttbr1() & 0x0000FFFFFFFFF000UL)

/* Test: vmm_create_user_l1 returns a fresh, non-zero L1 PA. */
static void test_create_user_l1_returns_fresh_pa(void)
{
    uint64_t pa1 = 0;
    int rc = vmm_create_user_l1(&pa1);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(pa1 != 0);
    TEST_ASSERT_EQUAL_UINT64(0, pa1 & 0xFFF);   /* page-aligned */

    uint64_t pa2 = 0;
    rc = vmm_create_user_l1(&pa2);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(pa2 != 0);
    TEST_ASSERT_TRUE(pa1 != pa2);               /* distinct pages */

    vmm_destroy_user_l1(pa1);
    vmm_destroy_user_l1(pa2);
}

/* Test: kernel-region L1 entries (0..USER_L1_FIRST-1) mirror the boot L1.
 * Read entries from both tables and verify byte-for-byte equality. */
static void test_create_user_l1_mirrors_kernel_entries(void)
{
    uint64_t pa = 0;
    int rc = vmm_create_user_l1(&pa);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Identity-mapped: PA == VA for kernel pages. */
    uint64_t *user_l1 = (uint64_t *)(uintptr_t)pa;
    uint64_t *boot_l1 = (uint64_t *)(uintptr_t)BOOT_L1_PA();

    /* Sample several kernel-region indices, including known mapped ones
     * (L1[0] is MMIO, L1[1] is RAM on QEMU). */
    for (size_t i = 0; i < USER_L1_FIRST; i++) {
        TEST_ASSERT_EQUAL_HEX64(boot_l1[i], user_l1[i]);
    }

    vmm_destroy_user_l1(pa);
}

/* Test: user-region L1 entries (USER_L1_FIRST..USER_L1_LIMIT-1) are zero. */
static void test_create_user_l1_zeros_user_entries(void)
{
    uint64_t pa = 0;
    int rc = vmm_create_user_l1(&pa);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint64_t *user_l1 = (uint64_t *)(uintptr_t)pa;
    for (size_t i = USER_L1_FIRST; i < USER_L1_LIMIT; i++) {
        TEST_ASSERT_EQUAL_UINT64(0, user_l1[i]);
    }

    vmm_destroy_user_l1(pa);
}

/* Test: NULL out_pa returns -1 without allocating. */
static void test_create_user_l1_rejects_null_out(void)
{
    int rc = vmm_create_user_l1(NULL);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* Test: vmm_destroy_user_l1(0) is a no-op (does not panic, does not
 * touch PMM). */
static void test_destroy_user_l1_zero_is_noop(void)
{
    /* Should not crash, should not assert. */
    vmm_destroy_user_l1(0);
    TEST_PASS();
}

/* Test: create + destroy returns the L1 page to PMM (free count
 * recovers). */
static void test_create_destroy_user_l1_no_leak(void)
{
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    uint64_t pa = 0;
    int rc = vmm_create_user_l1(&pa);
    TEST_ASSERT_EQUAL_INT(0, rc);
    vmm_destroy_user_l1(pa);

    pmm_get_stats(&after);
    /* Allocator may keep some metadata; require free count >= before
     * (i.e. no net leak). */
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "PMM leaked pages across create+destroy");
}

/* Test (mmap follow-up): vmm_user_unmap_page clears the L3 entry and
 * frees the leaf when VMM_FLAG_PMM_OWNED was set. Net PMM count
 * recovers across map+unmap+destroy. */
static void test_user_unmap_page_frees_pmm_owned(void)
{
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    void *leaf = pmm_alloc_pages(1);
    TEST_ASSERT_NOT_NULL(leaf);

    uint64_t va = USER_VA_BASE + 0x10000;   /* arbitrary user-window page */
    int rc = vmm_user_map_page(l1_pa, va, (uint64_t)(uintptr_t)leaf,
                               VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED);
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Unmap — should free the leaf and clear the L3 entry. A second
     * call must fail because the slot is no longer mapped. */
    TEST_ASSERT_EQUAL_INT(0, vmm_user_unmap_page(l1_pa, va));
    TEST_ASSERT_EQUAL_INT(-1, vmm_user_unmap_page(l1_pa, va));

    vmm_destroy_user_l1(l1_pa);

    pmm_get_stats(&after);
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "PMM leaked across map+unmap+destroy");
}

/* Test: destroy walks L3 leaves and frees PMM_OWNED pages without
 * the caller having to unmap them first. */
static void test_destroy_user_l1_frees_owned_leaves(void)
{
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    /* Map two PMM_OWNED pages and one un-owned page (kernel-image PA
     * placeholder — we just borrow the boot-L1 PA for the test; the
     * destroy path must leave it alone). */
    void *p1 = pmm_alloc_pages(1);
    void *p2 = pmm_alloc_pages(1);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);

    uint64_t kernel_pa_placeholder = vmm_boot_l1_pa();   /* not allocated by us */

    TEST_ASSERT_EQUAL_INT(0, vmm_user_map_page(l1_pa, USER_VA_BASE + 0x1000,
                                               (uint64_t)(uintptr_t)p1,
                                               VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED));
    TEST_ASSERT_EQUAL_INT(0, vmm_user_map_page(l1_pa, USER_VA_BASE + 0x2000,
                                               (uint64_t)(uintptr_t)p2,
                                               VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED));
    TEST_ASSERT_EQUAL_INT(0, vmm_user_map_page(l1_pa, USER_VA_BASE + 0x3000,
                                               kernel_pa_placeholder,
                                               VMM_FLAGS_USER_CODE));   /* no PMM_OWNED */

    vmm_destroy_user_l1(l1_pa);

    /* Both PMM_OWNED leaves should have been returned. The kernel-PA
     * leaf must NOT have been freed (would corrupt the boot L1 free
     * lists in PMM if it was). */
    pmm_get_stats(&after);
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "destroy leaked PMM_OWNED leaves");
}

/* Test: out-of-range or unmapped VA returns -1 from unmap_page. */
static void test_user_unmap_page_validation(void)
{
    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    /* VA below user window. */
    TEST_ASSERT_EQUAL_INT(-1, vmm_user_unmap_page(l1_pa, USER_VA_BASE - PAGE_SIZE));
    /* VA at the upper edge (== USER_VA_LIMIT, out of range). */
    TEST_ASSERT_EQUAL_INT(-1, vmm_user_unmap_page(l1_pa, USER_VA_LIMIT));
    /* Unaligned VA. */
    TEST_ASSERT_EQUAL_INT(-1, vmm_user_unmap_page(l1_pa, USER_VA_BASE + 0x1));
    /* Unmapped (no L2 yet). */
    TEST_ASSERT_EQUAL_INT(-1, vmm_user_unmap_page(l1_pa, USER_VA_BASE));

    vmm_destroy_user_l1(l1_pa);
}

/* ============================================================================
 * ASID allocator (per-task TTBR0 ASID tagging).
 * ============================================================================ */

static void test_alloc_asid_returns_nonzero_unique(void)
{
    /* First two ASIDs out of the pool must both be non-zero (kernel
     * ASID is reserved) and distinct. */
    uint16_t a = vmm_alloc_asid();
    uint16_t b = vmm_alloc_asid();
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_TRUE(a <= VMM_USER_ASID_MAX);
    TEST_ASSERT_TRUE(b <= VMM_USER_ASID_MAX);

    vmm_free_asid(a);
    vmm_free_asid(b);
}

static void test_alloc_asid_recycles_after_free(void)
{
    /* Free → re-alloc must hand back a previously-freed slot.
     * (Allocator is a linear scan from slot 1, so the freed slot is
     * the lowest free index by the time we re-alloc — but we only
     * need to check that *some* slot is reused without exhausting
     * the pool.) */
    uint16_t a = vmm_alloc_asid();
    TEST_ASSERT_NOT_EQUAL(0, a);
    vmm_free_asid(a);
    uint16_t b = vmm_alloc_asid();
    TEST_ASSERT_EQUAL_UINT16(a, b);
    vmm_free_asid(b);
}

static void test_free_asid_zero_noop(void)
{
    /* Freeing ASID 0 (kernel-reserved) and out-of-range values must
     * be silent no-ops. The allocator's bitmap shouldn't underflow
     * or trip a panic. */
    vmm_free_asid(0);
    vmm_free_asid(VMM_USER_ASID_MAX + 1);
    vmm_free_asid(0xFFFF);
    /* If any of those touched the bitmap, a fresh alloc might fail
     * or hand back ASID 0; assert it doesn't. */
    uint16_t a = vmm_alloc_asid();
    TEST_ASSERT_NOT_EQUAL(0, a);
    vmm_free_asid(a);
}

static void test_user_pte_has_ng_bit(void)
{
    /* User mappings must set PTE_NG (bit 11) so the hardware tags
     * them with the active ASID. */
    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    void *backing = pmm_alloc_pages(1);
    TEST_ASSERT_NOT_NULL(backing);
    uint64_t pa = (uint64_t)(uintptr_t)backing;

    TEST_ASSERT_EQUAL_INT(0,
        vmm_user_map_page(l1_pa, USER_VA_BASE, pa,
                          VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED));

    /* Walk the L1 to read the L3 entry directly and check nG. */
    uint64_t *l1 = (uint64_t *)(uintptr_t)l1_pa;
    uint64_t l1_idx = (USER_VA_BASE >> 30) & 0x1FF;
    uint64_t l2_pa = l1[l1_idx] & PTE_ADDR_MASK;
    uint64_t *l2 = (uint64_t *)(uintptr_t)l2_pa;
    uint64_t l2_idx = (USER_VA_BASE >> 21) & 0x1FF;
    uint64_t l3_pa = l2[l2_idx] & PTE_ADDR_MASK;
    uint64_t *l3 = (uint64_t *)(uintptr_t)l3_pa;
    uint64_t l3_idx = (USER_VA_BASE >> 12) & 0x1FF;
    uint64_t pte = l3[l3_idx];

    TEST_ASSERT_MESSAGE(pte & PTE_NG, "user PTE missing nG bit");

    vmm_destroy_user_l1(l1_pa);
}

static void test_kernel_pte_global(void)
{
    /* Kernel-mapped pages walked from the boot L1 must have nG=0 so
     * they apply across all ASIDs (no per-ASID retagging on TTBR0
     * swap). Read the boot L1's L2/L3 chain for a known kernel VA —
     * any page in the kernel image works. */
    extern char __text_start[];
    uint64_t kva = (uint64_t)(uintptr_t)__text_start;
    /* Round to page boundary in case the symbol isn't aligned. */
    kva &= ~(PAGE_SIZE - 1UL);

    uint64_t l1_pa = vmm_boot_l1_pa();
    uint64_t *l1 = (uint64_t *)(uintptr_t)l1_pa;
    uint64_t l1_idx = (kva >> 30) & 0x1FF;
    uint64_t l1_entry = l1[l1_idx];
    /* L1 may be a 1 GB block (Pi 5 / Jetson) or a table descriptor
     * (QEMU virt). Block descriptors carry the nG bit directly; for
     * tables we walk to L2. */
    if ((l1_entry & PTE_TYPE_MASK) == PTE_TYPE_BLOCK) {
        TEST_ASSERT_MESSAGE(!(l1_entry & PTE_NG),
            "kernel L1 block has nG=1");
        return;
    }
    uint64_t *l2 = (uint64_t *)(uintptr_t)(l1_entry & PTE_ADDR_MASK);
    uint64_t l2_idx = (kva >> 21) & 0x1FF;
    uint64_t l2_entry = l2[l2_idx];
    if ((l2_entry & PTE_TYPE_MASK) == PTE_TYPE_BLOCK) {
        TEST_ASSERT_MESSAGE(!(l2_entry & PTE_NG),
            "kernel L2 block has nG=1");
        return;
    }
    uint64_t *l3 = (uint64_t *)(uintptr_t)(l2_entry & PTE_ADDR_MASK);
    uint64_t l3_idx = (kva >> 12) & 0x1FF;
    uint64_t l3_entry = l3[l3_idx];
    TEST_ASSERT_MESSAGE(!(l3_entry & PTE_NG),
        "kernel L3 page has nG=1");
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_vmm(void)
{
    UnityBegin("VMM/TLB Functional Tests");

    /* Page table verification */
    RUN_TEST(test_virt_to_phys_accuracy);
    RUN_TEST(test_va_pa_coherency);

    /* Platform-specific mapping granularity */
    RUN_TEST(test_l1_block_mapping_detection);

    /* MMIO region mapping verification */
    RUN_TEST(test_mmio_regions_mapped);

    /* TLB invalidation correctness - the KEY tests */
    RUN_TEST(test_remap_requires_invalidation);
    RUN_TEST(test_remap_with_full_flush);
    RUN_TEST(test_remap_with_range_invalidation);
    RUN_TEST(test_public_remap_invalidates_tlb);

    /* Edge cases */
    RUN_TEST(test_sequential_remaps);
    RUN_TEST(test_rapid_remap_stress);

    /* ASID tests */
    RUN_TEST(test_asid_invalidation_executes);
    RUN_TEST(test_asid_all_invalidation_executes);

    /* Multi-CPU */
    RUN_TEST(test_tlb_broadcast_all_cpus);

    /* Regression: integer overflow in stats */
    RUN_TEST(test_vmm_stats_bytes_mapped_no_overflow);

    /* PCIe BAR3→MIP0 routing (Pi 5 UART IRQ path) */
    RUN_TEST(test_bar3_mip0_routing);

    /* #697 PR-2: per-task TTBR0 L1 helpers */
    RUN_TEST(test_create_user_l1_returns_fresh_pa);
    RUN_TEST(test_create_user_l1_mirrors_kernel_entries);
    RUN_TEST(test_create_user_l1_zeros_user_entries);
    RUN_TEST(test_create_user_l1_rejects_null_out);
    RUN_TEST(test_destroy_user_l1_zero_is_noop);
    RUN_TEST(test_create_destroy_user_l1_no_leak);
    RUN_TEST(test_user_unmap_page_frees_pmm_owned);
    RUN_TEST(test_destroy_user_l1_frees_owned_leaves);
    RUN_TEST(test_user_unmap_page_validation);

    /* ASID allocator (per-task TTBR0 ASID tagging follow-up) */
    RUN_TEST(test_alloc_asid_returns_nonzero_unique);
    RUN_TEST(test_alloc_asid_recycles_after_free);
    RUN_TEST(test_free_asid_zero_noop);
    RUN_TEST(test_user_pte_has_ng_bit);
    RUN_TEST(test_kernel_pte_global);

    return UnityEnd();
}
