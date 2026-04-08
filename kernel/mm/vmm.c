/*
 * vmm.c - Virtual Memory Manager for SLM-OS
 *
 * Manages ARM64 page tables using 4KB granule with 2MB block mappings.
 * Uses a 2-level table structure (L1 → L2) for 39-bit virtual addresses.
 */

#include "vmm.h"
#include "pmm.h"
#include "platform.h"
#include "uart.h"
#include "debug.h"
#include <stddef.h>

/*
 * Runtime flag for spinlock hardware support.
 * Before MMU enable, memory is non-cacheable and ldaxr/stxr hang.
 * Set to 1 after MMU is enabled with proper cacheable mappings.
 */
#if !defined(SPINLOCK_SKIP_LOCKING)
volatile int spinlock_hw_enabled = 0;
#endif

/*
 * MMU configuration shared with secondary CPUs.
 * Set by vmm_init() after MMU enable. Secondary CPUs read these
 * in smp_boot.S to enable their MMU with the same page tables.
 */
volatile uint64_t secondary_mmu_ttbr = 0;
volatile uint64_t secondary_mmu_mair = 0;
volatile uint64_t secondary_mmu_tcr = 0;

/*
 * ==========================================================================
 * Page Table Storage
 * ==========================================================================
 */

/*
 * L1 table: 512 entries × 1GB each = 512GB
 * For TTBR1 (kernel), we need entries for the upper VA range.
 *
 * With 39-bit VA and kernel at 0xFFFFFF80_00000000:
 * - L1 index 256-511 covers 0xFFFFFF80_00000000 to 0xFFFFFFFF_FFFFFFFF
 * - We only need entries 256+ for kernel space
 */
static uint64_t l1_table[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

/*
 * L2 tables: statically allocated for initial boot.
 * Each L2 table covers 1GB and contains 512 × 2MB block descriptors.
 *
 * Different platforms need different numbers of L2 tables depending
 * on how many distinct 1GB L1 regions contain devices or RAM that
 * need fine-grained (2MB) mappings.
 *
 * QEMU:   l2_kernel (RAM), l2_mmio (GIC+UART+VirtIO, all in 0x00-0x3F)
 * Jetson: l2_kernel (RAM 0x80-0xBF), l2_ram_c0 (RAM 0xC2-0xFF around OP-TEE),
 *         l2_mmio (GIC+UART+GPU+TCU, all in 0x00-0x3F), plus L1 1GB blocks for >4GB
 * Pi 5:   l2_mmio_pcie (PCIe RC/MIP at L1[64]), l2_mmio_gic (GIC/GPIO at L1[65]),
 *          l2_mmio_rp1 (RP1 UART/INTC at L1[124])
 *         (RAM uses 1GB L1 block descriptors, no L2 needed)
 */
#if defined(PLATFORM_QEMU_VIRT) || defined(PLATFORM_JETSON_ORIN_NANO)
static uint64_t l2_kernel[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_mmio[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
/* L2 table for 0xC0000000-0xFFFFFFFF: maps RAM around OP-TEE carveout */
static uint64_t l2_ram_c0[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
#endif
#if defined(PLATFORM_RASPI5)
static uint64_t l2_mmio_pcie[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_mmio_gic[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_mmio_rp1[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
/* L2 table for 4th GB: splits L1[3] so last 2MB can be non-cacheable */
static uint64_t l2_ram_gb3[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
#endif

/* VMM state */
static struct {
    bool initialized;
    uint32_t l2_tables_used;
    uint32_t blocks_mapped;
} vmm_state;

/*
 * ==========================================================================
 * Helper Functions
 * ==========================================================================
 */

/*
 * Build a block descriptor (L2 entry for 2MB block).
 */
static uint64_t make_block_desc(uint64_t pa, uint32_t flags)
{
    uint64_t desc = PTE_TYPE_BLOCK;

    /* Physical address (aligned to 2MB) */
    desc |= (pa & 0x0000FFFFFFE00000UL);

    /* Access flag must be set */
    desc |= PTE_AF;

    /* Determine memory attributes based on flags */
    if (flags & VMM_FLAG_DEVICE) {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_DEVICE_nGnRnE);
        desc |= PTE_SH_NON;     /* Device memory: non-shareable */
    } else if (flags & VMM_FLAG_NOCACHE) {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_NORMAL_NC);
        desc |= PTE_SH_INNER;   /* Normal NC: inner shareable */
    } else {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_NORMAL_WB);
        desc |= PTE_SH_INNER;   /* Normal WB: inner shareable */
    }

    /* Access permissions */
    if (flags & VMM_FLAG_USER) {
        /* User-accessible (EL0 + EL1) */
        if (!(flags & VMM_FLAG_WRITE)) {
            desc |= PTE_AP_RO_ALL;   /* Read-only at EL1 and EL0 */
        } else {
            desc |= PTE_AP_RW_ALL;   /* Read-write at EL1 and EL0 */
        }
    } else {
        /* Kernel-only (EL1) */
        if (!(flags & VMM_FLAG_WRITE)) {
            desc |= PTE_AP_RO_EL1;  /* Read-only at EL1 */
        } else {
            desc |= PTE_AP_RW_EL1;  /* Read-write at EL1 */
        }
    }

    /* Execute permissions (ARM uses "execute never" bits) */
    if (!(flags & VMM_FLAG_EXEC)) {
        desc |= PTE_PXN;        /* Privileged execute never */
    }
    if (!(flags & VMM_FLAG_USER) || !(flags & VMM_FLAG_EXEC)) {
        desc |= PTE_UXN;        /* Unprivileged execute never (unless user+exec) */
    }

    /* SLM-specific software flags */
    if (flags & VMM_FLAG_GPU_MAPPED) {
        desc |= PTE_SW_GPU_MAPPED;
    }
    if (flags & VMM_FLAG_MODEL_PAGE) {
        desc |= PTE_SW_MODEL_PAGE;
    }
    if (flags & VMM_FLAG_INFERENCE_HOT) {
        desc |= PTE_SW_INFERENCE_HOT;
    }

    return desc;
}

#if defined(PLATFORM_RASPI5)
/*
 * Build an L1 block descriptor (1GB block, used for large RAM regions).
 *
 * At L1 with 4KB granule, block descriptors map 1GB regions.
 * Output address bits are [47:30] (1GB-aligned).
 */
static uint64_t make_l1_block_desc(uint64_t pa, uint32_t flags)
{
    uint64_t desc = PTE_TYPE_BLOCK;

    /* Physical address (1GB aligned) */
    desc |= (pa & 0x0000FFFFC0000000UL);

    /* Access flag */
    desc |= PTE_AF;

    /* Memory attributes — same logic as L2 block descriptors */
    if (flags & VMM_FLAG_DEVICE) {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_DEVICE_nGnRnE);
        desc |= PTE_SH_NON;
    } else if (flags & VMM_FLAG_NOCACHE) {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_NORMAL_NC);
        desc |= PTE_SH_INNER;
    } else {
        desc |= PTE_ATTR_INDEX(MAIR_IDX_NORMAL_WB);
        desc |= PTE_SH_INNER;
    }

    /* Access permissions */
    if (flags & VMM_FLAG_USER) {
        if (!(flags & VMM_FLAG_WRITE)) {
            desc |= PTE_AP_RO_ALL;
        } else {
            desc |= PTE_AP_RW_ALL;
        }
    } else {
        if (!(flags & VMM_FLAG_WRITE)) {
            desc |= PTE_AP_RO_EL1;
        } else {
            desc |= PTE_AP_RW_EL1;
        }
    }

    /* Execute permissions */
    if (!(flags & VMM_FLAG_EXEC)) {
        desc |= PTE_PXN;
    }
    if (!(flags & VMM_FLAG_USER) || !(flags & VMM_FLAG_EXEC)) {
        desc |= PTE_UXN;
    }

    return desc;
}
#endif /* PLATFORM_RASPI5 */

/*
 * Build a table descriptor (L1 entry pointing to L2 table).
 */
static uint64_t make_table_desc(uint64_t table_pa)
{
    return PTE_TYPE_TABLE | (table_pa & PTE_ADDR_MASK);
}

/*
 * Get the L2 table for a given virtual address, or NULL if not mapped.
 */
static uint64_t *get_l2_table(uint64_t va)
{
    uint64_t l1_idx = L1_INDEX(va);
    uint64_t l1_entry = l1_table[l1_idx];

    if ((l1_entry & PTE_TYPE_MASK) != PTE_TYPE_TABLE) {
        return NULL;
    }

    /* Extract physical address of L2 table and convert to pointer */
    uint64_t l2_pa = l1_entry & PTE_ADDR_MASK;

    /*
     * Note: Before MMU is enabled, we access physical addresses directly.
     * After MMU is enabled, we need to use the kernel virtual address.
     * For simplicity, our static L2 tables are identity-mapped initially.
     */
    return (uint64_t *)l2_pa;
}

/*
 * ==========================================================================
 * Public API
 * ==========================================================================
 */

/*
 * Map a 2MB block into the kernel address space.
 */
int vmm_map_block(uint64_t virt, uint64_t phys, uint32_t flags)
{
    /* Alignment checks */
    if ((virt & (BLOCK_SIZE - 1)) != 0) {
        ERROR("vmm_map_block: virt 0x%lx not 2MB aligned", virt);
        return -1;
    }
    if ((phys & (BLOCK_SIZE - 1)) != 0) {
        ERROR("vmm_map_block: phys 0x%lx not 2MB aligned", phys);
        return -1;
    }

    uint64_t l1_idx = L1_INDEX(virt);
    uint64_t l2_idx = L2_INDEX(virt);

    /* Get L2 table (must exist) */
    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        ERROR("vmm_map_block: no L2 table for VA 0x%lx (L1[%lu])", virt, l1_idx);
        return -1;
    }

    /* Check if already mapped */
    if ((l2[l2_idx] & PTE_TYPE_MASK) != PTE_TYPE_INVALID) {
        ERROR("vmm_map_block: VA 0x%lx already mapped", virt);
        return -1;
    }

    /* Create block descriptor */
    l2[l2_idx] = make_block_desc(phys, flags);
    vmm_state.blocks_mapped++;

    /* Ensure write is visible */
    __asm__ volatile("dsb ishst" ::: "memory");

    return 0;
}

/*
 * Unmap a 2MB block.
 */
int vmm_unmap_block(uint64_t virt)
{
    if ((virt & (BLOCK_SIZE - 1)) != 0) {
        return -1;
    }

    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        return -1;
    }

    uint64_t l2_idx = L2_INDEX(virt);
    if ((l2[l2_idx] & PTE_TYPE_MASK) == PTE_TYPE_INVALID) {
        return -1;  /* Not mapped */
    }

    l2[l2_idx] = PTE_TYPE_INVALID;
    vmm_state.blocks_mapped--;

    /* TLB invalidate for this address */
    vmm_invalidate_tlb(virt);

    return 0;
}

/*
 * Map a contiguous region.
 */
int vmm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags)
{
    /* Round up to 2MB */
    size = (size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);

    for (uint64_t offset = 0; offset < size; offset += BLOCK_SIZE) {
        int ret = vmm_map_block(virt + offset, phys + offset, flags);
        if (ret != 0) {
            /* Unmap what we've done so far */
            for (uint64_t undo = 0; undo < offset; undo += BLOCK_SIZE) {
                vmm_unmap_block(virt + undo);
            }
            return ret;
        }
    }

    return 0;
}

/*
 * Translate virtual to physical address.
 */
uint64_t vmm_virt_to_phys(uint64_t virt)
{
    uint64_t l1_idx = L1_INDEX(virt);
    uint64_t l1_entry = l1_table[l1_idx];

    /* Check for L1 block descriptor (1GB mapping) */
    if ((l1_entry & PTE_TYPE_MASK) == PTE_TYPE_BLOCK) {
        uint64_t block_pa = l1_entry & 0x0000FFFFC0000000UL;
        uint64_t offset = virt & (L1_BLOCK_SIZE - 1);
        return block_pa | offset;
    }

    /* Otherwise look up L2 table */
    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        return 0;
    }

    uint64_t l2_idx = L2_INDEX(virt);
    uint64_t entry = l2[l2_idx];

    if ((entry & PTE_TYPE_MASK) != PTE_TYPE_BLOCK) {
        return 0;
    }

    /* Extract physical address and add offset within block */
    uint64_t block_pa = entry & 0x0000FFFFFFE00000UL;
    uint64_t offset = virt & (BLOCK_SIZE - 1);

    return block_pa | offset;
}

/*
 * Check if address is mapped.
 */
bool vmm_is_mapped(uint64_t virt)
{
    uint64_t l1_idx = L1_INDEX(virt);
    uint64_t l1_entry = l1_table[l1_idx];

    /* L1 block descriptor = 1GB region is mapped */
    if ((l1_entry & PTE_TYPE_MASK) == PTE_TYPE_BLOCK) {
        return true;
    }

    /* Check L2 table */
    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        return false;
    }

    uint64_t l2_idx = L2_INDEX(virt);
    return (l2[l2_idx] & PTE_TYPE_MASK) != PTE_TYPE_INVALID;
}

/*
 * Invalidate TLB for a specific address.
 */
void vmm_invalidate_tlb(uint64_t virt)
{
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vaae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(virt >> 12) : "memory"
    );
}

/*
 * Invalidate entire TLB.
 */
void vmm_invalidate_tlb_all(void)
{
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vmalle1is\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory"
    );
}

/*
 * Invalidate TLB for a virtual address range.
 *
 * Strategy: For small ranges, invalidate each page individually.
 * For large ranges (> 32 pages), do a full TLB flush instead since
 * the overhead of many individual invalidations outweighs a full flush.
 */
void vmm_invalidate_tlb_range(uint64_t start, uint64_t end)
{
    /* Align start down, end up to page boundaries */
    start = start & ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Calculate number of pages */
    uint64_t num_pages = (end - start) / PAGE_SIZE;

    /*
     * Threshold for switching to full flush.
     * 32 pages is a reasonable heuristic - individual TLBI instructions
     * are fast, but 32+ iterations with barriers adds latency.
     */
    if (num_pages > 32 || end <= start) {
        vmm_invalidate_tlb_all();
        return;
    }

    /* Invalidate each page in the range */
    __asm__ volatile("dsb ishst" ::: "memory");

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        __asm__ volatile(
            "tlbi vaae1is, %0\n"
            :: "r"(va >> 12) : "memory"
        );
    }

    __asm__ volatile(
        "dsb ish\n"
        "isb\n"
        ::: "memory"
    );
}

/*
 * Invalidate TLB for a specific address and ASID.
 *
 * Uses TLBI VAE1IS which takes the VA and ASID combined:
 * Xt[63:48] = ASID, Xt[43:0] = VA[55:12] (bits [47:44] are RES0)
 */
void vmm_invalidate_tlb_asid(uint64_t virt, uint16_t asid)
{
    /*
     * Build the operand for TLBI VAE1IS:
     * - Bits [63:48]: ASID (16 bits)
     * - Bits [43:0]:  VA[55:12] shifted right by 12
     */
    uint64_t operand = ((uint64_t)asid << 48) | (virt >> 12);

    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(operand) : "memory"
    );
}

/*
 * Invalidate all TLB entries for a specific ASID.
 *
 * Uses TLBI ASIDE1IS which takes ASID in bits [63:48].
 */
void vmm_invalidate_tlb_asid_all(uint16_t asid)
{
    /*
     * Build the operand for TLBI ASIDE1IS:
     * - Bits [63:48]: ASID (16 bits)
     * - Other bits: ignored
     */
    uint64_t operand = (uint64_t)asid << 48;

    __asm__ volatile(
        "dsb ishst\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(operand) : "memory"
    );
}

/*
 * Get current TTBR1_EL1 value.
 */
uint64_t vmm_get_ttbr1(void)
{
    uint64_t ttbr1;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
    return ttbr1;
}

/*
 * Get VMM statistics.
 */
void vmm_get_stats(struct vmm_stats *stats)
{
    if (!stats) return;

    stats->l1_tables = 1;  /* We use one L1 table */
    stats->l2_tables = vmm_state.l2_tables_used;
    stats->blocks_mapped = vmm_state.blocks_mapped;
    stats->bytes_mapped = (uint64_t)vmm_state.blocks_mapped * BLOCK_SIZE;
}

/*
 * ==========================================================================
 * VMM Validation Tests
 * ==========================================================================
 */

/*
 * Run VMM validation tests after MMU is enabled.
 * Returns 0 on success, -1 on failure.
 */
static int vmm_run_tests(void)
{
    int errors = 0;

    uart_puts("\nVMM Validation Tests:\n");

    /*
     * Test 1: vmm_virt_to_phys() for identity-mapped RAM
     * VA 0x4000_0000 should translate to PA 0x4000_0000
     */
    {
        uint64_t va = RAM_BASE;
        uint64_t expected_pa = RAM_BASE;
        uint64_t actual_pa = vmm_virt_to_phys(va);

        if (actual_pa == expected_pa) {
            uart_printf("  [PASS] virt_to_phys(0x%lx) = 0x%lx\n", va, actual_pa);
        } else {
            uart_printf("  [FAIL] virt_to_phys(0x%lx) = 0x%lx, expected 0x%lx\n",
                        va, actual_pa, expected_pa);
            errors++;
        }
    }

    /*
     * Test 2: vmm_virt_to_phys() for kernel high address (TTBR1)
     * VA 0xFFFF_FF80_4000_0000 should translate to PA 0x4000_0000
     */
    {
        uint64_t va = KERNEL_VA_BASE | RAM_BASE;
        uint64_t expected_pa = RAM_BASE;
        uint64_t actual_pa = vmm_virt_to_phys(va);

        if (actual_pa == expected_pa) {
            uart_printf("  [PASS] virt_to_phys(0x%lx) = 0x%lx\n", va, actual_pa);
        } else {
            uart_printf("  [FAIL] virt_to_phys(0x%lx) = 0x%lx, expected 0x%lx\n",
                        va, actual_pa, expected_pa);
            errors++;
        }
    }

    /*
     * Test 3: vmm_is_mapped() for mapped address
     */
    {
        uint64_t va = RAM_BASE;
        bool mapped = vmm_is_mapped(va);

        if (mapped) {
            uart_printf("  [PASS] is_mapped(0x%lx) = true\n", va);
        } else {
            uart_printf("  [FAIL] is_mapped(0x%lx) = false, expected true\n", va);
            errors++;
        }
    }

    /*
     * Test 4: vmm_is_mapped() for unmapped address
     * Pick an address beyond mapped RAM that should not be mapped.
     */
    {
        /* Use an address well beyond RAM_BASE + RAM_SIZE */
        uint64_t va = RAM_BASE + RAM_SIZE + 0x40000000UL;
        bool mapped = vmm_is_mapped(va);

        if (!mapped) {
            uart_printf("  [PASS] is_mapped(0x%lx) = false\n", va);
        } else {
            uart_printf("  [FAIL] is_mapped(0x%lx) = true, expected false\n", va);
            errors++;
        }
    }

    /*
     * Test 5: Read/write through kernel high address (TTBR1)
     * Write a magic value via high VA, read back via identity VA
     */
    {
        /* Use an address in our mapped region, after kernel code */
        volatile uint64_t *identity_ptr = (volatile uint64_t *)(RAM_BASE + 0x100000);
        volatile uint64_t *high_ptr = (volatile uint64_t *)(KERNEL_VA_BASE | RAM_BASE | 0x100000);
        uint64_t magic = 0xDEADBEEFCAFEBABEUL;
        uint64_t original = *identity_ptr;

        /* Write via high address */
        *high_ptr = magic;

        /* Read via identity address */
        uint64_t readback = *identity_ptr;

        /* Restore original value */
        *identity_ptr = original;

        if (readback == magic) {
            uart_puts("  [PASS] Write via TTBR1, read via TTBR0 works\n");
        } else {
            uart_printf("  [FAIL] Write 0x%lx via high VA, read 0x%lx via identity\n",
                        magic, readback);
            errors++;
        }
    }

    /*
     * Test 6: Dynamic mapping test
     * Map a new 2MB block, verify it's accessible, then unmap
     *
     * NOTE: This test requires physical memory beyond what's initially mapped.
     * Since vmm_init() now maps all of RAM_SIZE, this test is only meaningful
     * if we have RAM beyond what the kernel knows about, which requires
     * runtime detection (e.g., from DTB). For now, skip since all RAM is mapped.
     */
    {
        /*
         * Since we now map all of RAM_SIZE at boot, there's no unmapped region
         * within our known RAM. This test would only be meaningful with runtime
         * memory discovery (DTB parsing) that finds more RAM than RAM_SIZE.
         */
        uart_puts("  [SKIP] Dynamic map test: all RAM mapped at boot\n");
    }

    /* Summary */
    uart_puts("\n");
    if (errors == 0) {
        INFO("VMM tests passed");
    } else {
        uart_printf("[ERROR] VMM tests: %d failures\n", errors);
    }

    return (errors == 0) ? 0 : -1;
}

/*
 * Dump page table mappings for debugging.
 */
void vmm_dump(void)
{
    uart_puts("\nVMM Page Table Dump:\n");
    uart_printf("  L1 table at: %p\n", l1_table);
    uart_printf("  Blocks mapped: %u\n", vmm_state.blocks_mapped);
    uart_printf("  Bytes mapped: %lu MB\n",
                ((uint64_t)vmm_state.blocks_mapped * BLOCK_SIZE) / (1024 * 1024));

    /* Dump non-empty L1 entries */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (l1_table[i] != 0) {
            uint64_t va_base = (i < 256) ?
                ((uint64_t)i << 30) :
                (KERNEL_VA_BASE | ((uint64_t)(i - 256) << 30));
            uart_printf("  L1[%d]: 0x%lx (covers VA 0x%lx)\n",
                        i, l1_table[i], va_base);
        }
    }
}

/*
 * ==========================================================================
 * VMM Initialization
 * ==========================================================================
 */

/*
 * ==========================================================================
 * Platform-Specific Page Table Setup
 *
 * Each platform has a different physical memory layout and device address
 * space. The L1 table maps 512 × 1GB regions (39-bit VA). Each platform
 * populates the L1 entries differently:
 *
 * QEMU virt:
 *   L1[0]  → L2 table for MMIO (GIC 0x08M, UART 0x09M, VirtIO 0x0AM)
 *   L1[1]  → L2 table for RAM  (0x40000000, 1GB)
 *
 * Jetson Orin Nano:
 *   L1[0]  → L2 table for MMIO (GIC 0x0F4M, UART 0x031M)
 *   L1[2]  → L2 table for RAM  (0x80000000, 1GB of 8GB mapped)
 *
 * Raspberry Pi 5:
 *   L1[0-3] → 1GB block descs for RAM (0x00000000, 4GB)
 *   L1[65]  → L2 table for GIC/GPIO   (0x107FFF9000, 0x107D517C04)
 *   L1[124] → L2 table for RP1/UART   (0x1F00030000, 0x1F000D0000)
 *
 * x86-64 has its own VMM (4-level page tables, not shared with this code).
 * ==========================================================================
 */

#if defined(PLATFORM_QEMU_VIRT) || defined(PLATFORM_JETSON_ORIN_NANO)
/*
 * QEMU and Jetson: RAM and MMIO are in separate 1GB L1 regions.
 * RAM gets an L2 table for 2MB block mappings.
 * MMIO gets an L2 table with sparse device mappings.
 */
static void vmm_setup_platform(void)
{
    /* Note: VMM_FLAG_USER not set here yet — EL0 memory access requires
     * further investigation on QEMU virt platform. Components currently
     * run at EL1 with syscall infrastructure ready for EL0 transition. */
    uint32_t kernel_flags = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_EXEC;

    /* L1 entry for MMIO region (GIC, UART, etc. all below 0x40000000) */
    l1_table[0] = make_table_desc((uint64_t)l2_mmio);
    vmm_state.l2_tables_used++;

    /* L1[2]: RAM 0x80000000-0xBFFFFFFF via L2 table */
    l1_table[RAM_BASE >> 30] = make_table_desc((uint64_t)l2_kernel);
    vmm_state.l2_tables_used++;

    /* Populate RAM L2 table with 2MB blocks.
     * On Jetson, stop before OP-TEE carveout at 0xBE000000 (L2 entry 496).
     * On QEMU, map up to 1GB. */
#if defined(PLATFORM_JETSON_ORIN_NANO)
    #define KERNEL_L2_LIMIT 495  /* 495 * 2MB = 990 MB → 0x80000000-0xBDDFFFFF (WB) */
#else
    #define KERNEL_L2_LIMIT 512
#endif
    uint32_t num_blocks = RAM_SIZE / BLOCK_SIZE;
    if (num_blocks > KERNEL_L2_LIMIT) num_blocks = KERNEL_L2_LIMIT;
    for (uint32_t i = 0; i < num_blocks; i++) {
        uint64_t pa = RAM_BASE + (i * BLOCK_SIZE);
        l2_kernel[i] = make_block_desc(pa, kernel_flags);
        vmm_state.blocks_mapped++;
    }

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Entry 495: NC 2MB block at 0xBDE00000 for cross-CPU shared data.
     * This is the last 2MB of region 1 (before OP-TEE carveout at 0xBE000000).
     * Mapped as Normal Non-Cacheable, Inner Shareable (MAIR index 2). */
    {
        uint32_t nc_flags = VMM_FLAG_NOCACHE | VMM_FLAG_READ | VMM_FLAG_WRITE;
        l2_kernel[495] = make_block_desc(0xBDE00000UL, nc_flags);
        vmm_state.blocks_mapped++;
        DEBUG_PRINT("  L2[495]: NC block at 0xBDE00000 (cross-CPU shared memory)");
    }
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * Jetson memory expansion: map RAM above OP-TEE carveout.
     *
     * Memory map (from /proc/iomem):
     *   80000000-BDFFFFFF : System RAM (~990 MB) — mapped above in l2_kernel
     *   BE000000-C1FFFFFF : OP-TEE carveout (64 MB) — DO NOT MAP
     *   C2000000-FFFDFFFF : System RAM (~958 MB)
     *   100000000-25E244FFF : System RAM (~5.5 GB)
     *
     * L1[3] (0xC0000000-0xFFFFFFFF): L2 table skipping OP-TEE at entries 0-15
     * L1[4]-L1[8] (0x100000000-0x23FFFFFFF): 1GB block descriptors
     */

    /* L1[3]: 0xC0000000-0xFFFFFFFF via L2 table (skip carveout) */
    l1_table[3] = make_table_desc((uint64_t)l2_ram_c0);
    vmm_state.l2_tables_used++;

    /* Map 0xC2000000-0xFFFFFFFF as 2MB blocks (skip entries 0-15 = OP-TEE) */
    for (uint32_t i = 16; i < 512; i++) {  /* Entry 16 = 0xC2000000 */
        uint64_t pa = 0xC0000000UL + (i * BLOCK_SIZE);
        l2_ram_c0[i] = make_block_desc(pa, kernel_flags);
        vmm_state.blocks_mapped++;
    }

    /* L1[4]-L1[8]: 1GB block descriptors for 0x100000000-0x23FFFFFFF (5 GB)
     * Using L1-level 1GB blocks — same descriptor format as L2 2MB blocks
     * but placed directly in L1 with 1GB-aligned addresses. */
    for (uint32_t idx = 4; idx <= 8; idx++) {
        uint64_t pa = (uint64_t)idx << 30;  /* idx * 1GB */
        l1_table[idx] = make_block_desc(pa, kernel_flags);
        vmm_state.blocks_mapped++;
    }
#endif

    /* Map MMIO devices into l2_mmio */
    uint64_t gic_l2_idx = (GIC_DIST_BASE >> BLOCK_SHIFT) & 0x1FF;
    uint64_t uart_l2_idx = (UART_BASE >> BLOCK_SHIFT) & 0x1FF;

    l2_mmio[gic_l2_idx] = make_block_desc(GIC_DIST_BASE & ~(BLOCK_SIZE - 1),
                                           VMM_FLAGS_DEVICE);
    l2_mmio[uart_l2_idx] = make_block_desc(UART_BASE & ~(BLOCK_SIZE - 1),
                                            VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped += 2;

#if defined(TCU_RX_MBOX)
    /* TCU RX mailbox (HSP shared mailbox for serial input on Jetson) */
    uint64_t tcu_l2_idx = (TCU_RX_MBOX >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio[tcu_l2_idx] = make_block_desc(TCU_RX_MBOX & ~(BLOCK_SIZE - 1),
                                           VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(GPU_BASE)
    /* GPU MMIO region (0x17000000, maps into L1[0] l2_mmio) */
    uint64_t gpu_l2_idx = (GPU_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio[gpu_l2_idx] = make_block_desc(GPU_BASE & ~(BLOCK_SIZE - 1),
                                           VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;
#endif

#if defined(PLATFORM_QEMU_VIRT)
    /* VirtIO MMIO region for virtio-net at 0x0A000000 */
    uint64_t virtio_l2_idx = (0x0a000000 >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio[virtio_l2_idx] = make_block_desc(0x0a000000, VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;
#endif
}
#endif /* QEMU || JETSON */

#if defined(PLATFORM_RASPI5)
/*
 * Raspberry Pi 5: RAM starts at 0x0 and devices are at high addresses.
 *
 * Physical memory layout:
 *   0x0000_0000 - 0x0FFF_FFFF : RAM (4GB)          → L1[0]-L1[3]
 *   0x1040_0000_0000 region   : GIC-400, GPIO2      → L1[65]
 *   0x1F00_0000_0000 region   : RP1 (UART, GPIO)    → L1[124]
 *
 * RAM uses 1GB L1 block descriptors (no L2 tables needed).
 * MMIO uses L2 tables for fine-grained 2MB device mappings.
 */
static void vmm_setup_platform(void)
{
    uint32_t ram_flags = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_EXEC;

    /*
     * Map RAM using 1GB L1 block descriptors.
     * Pi 5 has 4GB (or 8GB) starting at PA 0x0.
     *
     * L1[0-2]: 1GB block descriptors (write-back cacheable).
     * L1[3]:   L2 table — 511 WB 2MB blocks + 1 NC 2MB block at the top.
     *          The NC block (PA 0xFFE00000) is used for cross-CPU shared data
     *          that must bypass L1/L2 caches (see ncmem.h).
     */
    uint32_t ram_gb = RAM_SIZE / L1_BLOCK_SIZE;
    if (ram_gb > 512) ram_gb = 512;

    /* Map first N-1 GBs as L1 block descriptors */
    uint32_t l1_blocks = (ram_gb > 1) ? ram_gb - 1 : ram_gb;
    for (uint32_t i = 0; i < l1_blocks; i++) {
        l1_table[i] = make_l1_block_desc(i * L1_BLOCK_SIZE, ram_flags);
        vmm_state.blocks_mapped += 512;
    }

    /* Split the last GB into L2 2MB entries for NC region at top */
    if (ram_gb > 1) {
        uint64_t gb3_base = (uint64_t)(ram_gb - 1) * L1_BLOCK_SIZE;
        uint32_t nc_flags = VMM_FLAG_NOCACHE | VMM_FLAG_READ | VMM_FLAG_WRITE;

        for (int i = 0; i < ENTRIES_PER_TABLE; i++)
            l2_ram_gb3[i] = 0;

        /* L2[0-510]: WB cacheable 2MB blocks (same as original L1 block) */
        for (int i = 0; i < ENTRIES_PER_TABLE - 1; i++) {
            l2_ram_gb3[i] = make_block_desc(gb3_base + (uint64_t)i * BLOCK_SIZE,
                                            ram_flags);
        }
        /* L2[511]: Non-cacheable 2MB block for cross-CPU shared memory */
        l2_ram_gb3[ENTRIES_PER_TABLE - 1] = make_block_desc(
            gb3_base + (uint64_t)(ENTRIES_PER_TABLE - 1) * BLOCK_SIZE, nc_flags);

        l1_table[ram_gb - 1] = make_table_desc((uint64_t)l2_ram_gb3);
        vmm_state.l2_tables_used++;
        vmm_state.blocks_mapped += ENTRIES_PER_TABLE;

        DEBUG_PRINT("  L1[%u]: L2 table (511 WB + 1 NC at 0x%lx)",
                    ram_gb - 1,
                    (unsigned long)(gb3_base + (uint64_t)(ENTRIES_PER_TABLE - 1) * BLOCK_SIZE));
    }

    /*
     * Map PCIe RC + MIP0: L1[64] covers 0x1000000000-0x103FFFFFFF
     *
     * PCIe RC (pcie2): 0x1000120000  → L2 index = (0x1000120000 >> 21) & 0x1FF
     * MIP0:            0x1000130000  → same 2MB block as PCIe RC
     */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++)
        l2_mmio_pcie[i] = 0;

    uint64_t pcie_l1_idx = PCIE_RC_BASE >> 30;
    l1_table[pcie_l1_idx] = make_table_desc((uint64_t)l2_mmio_pcie);
    vmm_state.l2_tables_used++;

    /* PCIe RC and MIP0 are in the same 2MB block (0x1000100000-0x10002FFFFF) */
    uint64_t pcie_l2_idx = (PCIE_RC_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio_pcie[pcie_l2_idx] = make_block_desc(PCIE_RC_BASE & ~(BLOCK_SIZE - 1),
                                                  VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;

    /*
     * Map GIC and GPIO2 region: L1[65] covers 0x1040000000-0x107FFFFFFF
     *
     * GIC distributor: 0x107FFF9000  → L2 index = (0x107FFF9000 >> 21) & 0x1FF
     * GIC CPU iface:   0x107FFFA000  → same 2MB block as distributor
     * GPIO2 (ACT LED): 0x107D517C04  → L2 index = (0x107D517C04 >> 21) & 0x1FF
     */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++)
        l2_mmio_gic[i] = 0;

    uint64_t gic_l1_idx = GIC_DIST_BASE >> 30;
    l1_table[gic_l1_idx] = make_table_desc((uint64_t)l2_mmio_gic);
    vmm_state.l2_tables_used++;

    /* GIC distributor + CPU interface (same 2MB block) */
    uint64_t gic_l2_idx = (GIC_DIST_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio_gic[gic_l2_idx] = make_block_desc(GIC_DIST_BASE & ~(BLOCK_SIZE - 1),
                                                VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;

    /* GPIO2 for ACT LED at 0x107D517C04 */
    uint64_t gpio2_addr = 0x107D517C04ULL;
    uint64_t gpio2_l2_idx = (gpio2_addr >> BLOCK_SHIFT) & 0x1FF;
    if (gpio2_l2_idx != gic_l2_idx) {
        l2_mmio_gic[gpio2_l2_idx] = make_block_desc(gpio2_addr & ~(BLOCK_SIZE - 1),
                                                      VMM_FLAGS_DEVICE);
        vmm_state.blocks_mapped++;
    }

    /*
     * Map RP1 peripherals: L1[124] covers 0x1F00000000-0x1F3FFFFFFF
     *
     * UART0:  0x1F00030000  → L2 index = (0x1F00030000 >> 21) & 0x1FF
     * GPIO:   0x1F000D0000  → same 2MB block as UART0 (both in first 2MB)
     * RIO:    0x1F000E0000  → same 2MB block
     * PADS:   0x1F000F0000  → same 2MB block
     */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++)
        l2_mmio_rp1[i] = 0;

    uint64_t rp1_l1_idx = UART_BASE >> 30;
    l1_table[rp1_l1_idx] = make_table_desc((uint64_t)l2_mmio_rp1);
    vmm_state.l2_tables_used++;

    /* RP1 first 2MB block covers UART0, GPIO, RIO, PADS */
    uint64_t rp1_l2_idx = (UART_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio_rp1[rp1_l2_idx] = make_block_desc(UART_BASE & ~(BLOCK_SIZE - 1),
                                                VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;

    /* RP1 INTC (PCIE_CFG) at 0x1F00108000 — different 2MB block */
    uint64_t rp1_intc_l2_idx = (RP1_INTC_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio_rp1[rp1_intc_l2_idx] = make_block_desc(RP1_INTC_BASE & ~(BLOCK_SIZE - 1),
                                                     VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;

    /* RP1 BAR0 (MSI-X table) at 0x1F00410000 — another 2MB block */
    uint64_t rp1_bar0_l2_idx = (RP1_MSIX_TABLE_BASE >> BLOCK_SHIFT) & 0x1FF;
    l2_mmio_rp1[rp1_bar0_l2_idx] = make_block_desc(RP1_MSIX_TABLE_BASE & ~(BLOCK_SIZE - 1),
                                                     VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped++;
}
#endif /* PLATFORM_RASPI5 */

/*
 * Set up initial page tables and enable MMU.
 *
 * With 39-bit VA and shared L1 table (TTBR0 = TTBR1), each mapping
 * serves both identity (TTBR0) and kernel high (TTBR1) addresses.
 * During MMU enable, code executes via TTBR0 (identity mapping).
 */
void vmm_init(void)
{
    INFO("Initializing VMM...");

    /* Clear L1 table */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        l1_table[i] = 0;
    }

    /* Platform-specific page table setup */
    vmm_setup_platform();

    /*
     * Flush page tables to DRAM before MMU enable.
     * On real hardware (Pi 5, Jetson), secondary CPUs' page table walkers
     * read from DRAM, not from CPU 0's L1/L2 cache. Without this flush,
     * secondary CPUs read stale L2 entries (0 = unmapped), making the NC
     * region invisible. Uses raw DC CVAC since cache.h wrappers may not
     * be initialized yet (MMU is still off). */
    {
        const char *p = (const char *)l1_table;
        const char *end = p + sizeof(l1_table);
        for (; p < end; p += 64)
            __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {
        const char *p = (const char *)l2_kernel;
        const char *end = p + ENTRIES_PER_TABLE * sizeof(uint64_t);
        for (; p < end; p += 64)
            __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
    {
        const char *p = (const char *)l2_ram_c0;
        const char *end = p + ENTRIES_PER_TABLE * sizeof(uint64_t);
        for (; p < end; p += 64)
            __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
    }
#endif
    __asm__ volatile("dsb sy" ::: "memory");

    DEBUG_PRINT("  L1 table at PA: 0x%lx", (uint64_t)l1_table);
    DEBUG_PRINT("  TTBR0 = TTBR1 = 0x%lx (shared L1 table)", (uint64_t)l1_table);
    DEBUG_PRINT("  L2 tables used: %u", vmm_state.l2_tables_used);
    DEBUG_PRINT("  Blocks mapped: %u (%lu MB)",
                vmm_state.blocks_mapped,
                ((uint64_t)vmm_state.blocks_mapped * BLOCK_SIZE) / (1024 * 1024));

    /*
     * Enable MMU.
     *
     * IMPORTANT: mmu_enable() must be called with:
     * 1. Identity mapping active (current PC is valid after enable)
     * 2. TTBR1 pointing to our L1 table
     * 3. Proper TCR and MAIR values
     */
    INFO("Enabling MMU...");

    uint64_t ttbr1 = (uint64_t)l1_table;
    uint64_t sctlr = SCTLR_M | SCTLR_C | SCTLR_I;  /* MMU + caches */

    mmu_enable(ttbr1, TCR_EL1_VALUE, MAIR_EL1_VALUE, sctlr);

    /* If we get here, MMU is enabled and we're running at virtual addresses */
    vmm_state.initialized = true;

    INFO("MMU enabled successfully");

    /* Save MMU configuration for secondary CPUs (smp_boot.S reads these) */
    secondary_mmu_ttbr = (uint64_t)l1_table;
    secondary_mmu_mair = MAIR_EL1_VALUE;
    secondary_mmu_tcr = TCR_EL1_VALUE;
    __asm__ volatile("dmb ish" ::: "memory");

    /* Enable hardware spinlocks now that memory is cacheable.
     * The exclusive monitor (ldaxr/stxr) requires cacheable, shareable
     * memory to function. Before MMU enable, all memory is non-cacheable
     * and exclusive operations hang on Cortex-A76. */
#if !defined(SPINLOCK_SKIP_LOCKING)
    spinlock_hw_enabled = 1;
    __asm__ volatile("dmb ish" ::: "memory");  /* Ensure flag is visible */
    INFO("Spinlock hardware enabled (exclusive monitor active)");
#endif

    vmm_dump();

    /* Run validation tests */
    vmm_run_tests();
}

/*
 * ==========================================================================
 * Test Helpers (for unit tests only)
 * ==========================================================================
 *
 * These functions expose low-level page table manipulation for testing
 * TLB invalidation behavior. They should NOT be used in production code.
 */

/*
 * Get the L2 page table entry for a virtual address.
 * Returns the raw PTE value, or 0 if no L2 table exists for this VA.
 */
uint64_t vmm_test_get_l2_entry(uint64_t virt)
{
    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        return 0;
    }
    return l2[L2_INDEX(virt)];
}

/*
 * Set the L2 page table entry WITHOUT invalidating TLB.
 *
 * This is intentionally dangerous - it creates an inconsistency between
 * the page tables and TLB. Use only for testing that TLB invalidation
 * is actually necessary and working.
 *
 * Returns 0 on success, -1 if no L2 table exists for this VA.
 */
int vmm_test_set_l2_entry_no_invalidate(uint64_t virt, uint64_t pte)
{
    uint64_t *l2 = get_l2_table(virt);
    if (!l2) {
        return -1;
    }

    /* Ensure prior stores are visible before PTE update */
    __asm__ volatile("dsb ishst" ::: "memory");

    l2[L2_INDEX(virt)] = pte;

    /* Ensure PTE update is visible (but do NOT invalidate TLB) */
    __asm__ volatile("dsb ish" ::: "memory");

    return 0;
}

/*
 * Build a block descriptor for testing.
 * Wrapper around internal make_block_desc for test use.
 */
uint64_t vmm_test_make_block_desc(uint64_t phys, uint32_t flags)
{
    return make_block_desc(phys, flags);
}
