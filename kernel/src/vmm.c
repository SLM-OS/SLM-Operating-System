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
 * L2 tables: allocated dynamically from PMM as needed.
 * Each L2 table covers 1GB and contains 512 × 2MB block descriptors.
 *
 * For initial boot, we statically allocate a few L2 tables:
 * - One for kernel code/data (covers first 1GB of physical RAM)
 * - One for MMIO (covers GIC, UART region)
 */
static uint64_t l2_kernel[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2_mmio[ENTRIES_PER_TABLE] __attribute__((aligned(PAGE_SIZE)));

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
    if (!(flags & VMM_FLAG_WRITE)) {
        desc |= PTE_AP_RO_EL1;  /* Read-only at EL1 */
    } else {
        desc |= PTE_AP_RW_EL1;  /* Read-write at EL1 */
    }

    /* Execute permissions (ARM uses "execute never" bits) */
    if (!(flags & VMM_FLAG_EXEC)) {
        desc |= PTE_PXN;        /* Privileged execute never */
    }
    desc |= PTE_UXN;            /* Always block EL0 execute for kernel pages */

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
     * Address 0x8000_0000 (2GB) should not be mapped
     */
    {
        uint64_t va = 0x80000000UL;
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
     * On QEMU with only 128MB RAM (0x40000000-0x48000000), all RAM is mapped
     * at init, so we skip this test. On systems with more RAM, we test at
     * 128MB offset which should be unmapped but valid physical memory.
     */
    {
        /* Find an unmapped region - use 128MB offset */
        uint64_t test_pa = RAM_BASE + (128 * 1024 * 1024);  /* 0x4800_0000 */
        uint64_t test_va = test_pa;  /* Identity map for simplicity */

        /* Check if this address is beyond available RAM */
        if (test_pa >= RAM_BASE + RAM_SIZE) {
            uart_puts("  [SKIP] Dynamic map test: no unmapped RAM available\n");
        } else if (vmm_is_mapped(test_va)) {
            uart_printf("  [FAIL] VA 0x%lx already mapped before test\n", test_va);
            errors++;
        } else {
            /* Map it */
            int ret = vmm_map_block(test_va, test_pa,
                                    VMM_FLAG_READ | VMM_FLAG_WRITE);
            if (ret != 0) {
                uart_printf("  [FAIL] vmm_map_block() returned %d\n", ret);
                errors++;
            } else {
                /* Verify it's now mapped */
                if (!vmm_is_mapped(test_va)) {
                    uart_puts("  [FAIL] Block not mapped after vmm_map_block()\n");
                    errors++;
                } else {
                    /* Write and read back */
                    volatile uint64_t *ptr = (volatile uint64_t *)test_va;
                    uint64_t magic = 0x1234567890ABCDEFULL;
                    *ptr = magic;
                    uint64_t readback = *ptr;

                    if (readback != magic) {
                        uart_printf("  [FAIL] Dynamic map: wrote 0x%lx, read 0x%lx\n",
                                    magic, readback);
                        errors++;
                    } else {
                        /* Unmap it */
                        ret = vmm_unmap_block(test_va);
                        if (ret != 0) {
                            uart_printf("  [FAIL] vmm_unmap_block() returned %d\n", ret);
                            errors++;
                        } else if (vmm_is_mapped(test_va)) {
                            uart_puts("  [FAIL] Block still mapped after unmap\n");
                            errors++;
                        } else {
                            uart_puts("  [PASS] Dynamic map/unmap works\n");
                        }
                    }
                }
            }
        }
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
                (vmm_state.blocks_mapped * BLOCK_SIZE) / (1024 * 1024));

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
 * Set up initial page tables and enable MMU.
 *
 * With 39-bit VA and shared L1 table (TTBR0 = TTBR1), each mapping
 * serves both identity (TTBR0) and kernel high (TTBR1) addresses:
 *
 * Physical:          TTBR0 (identity):     TTBR1 (kernel):
 * 0x0800_0000 (GIC)  0x0800_0000       ->  0xFFFF_FF80_0800_0000
 * 0x0900_0000 (UART) 0x0900_0000       ->  0xFFFF_FF80_0900_0000
 * 0x4000_0000 (RAM)  0x4000_0000       ->  0xFFFF_FF80_4000_0000
 *
 * During MMU enable, code executes via TTBR0 (identity mapping).
 * After enable, kernel code runs via TTBR1 (high addresses).
 */
void vmm_init(void)
{
    INFO("Initializing VMM...");

    /* Clear tables */
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        l1_table[i] = 0;
        l2_kernel[i] = 0;
        l2_mmio[i] = 0;
    }

    /*
     * Set up L1 table entries.
     *
     * With 39-bit VA, both TTBR0 (identity) and TTBR1 (kernel) use the
     * same L1 table. The index is determined by VA[38:30]:
     * - L1[0] covers 0x0000_0000 - 0x3FFF_FFFF (MMIO region)
     * - L1[1] covers 0x4000_0000 - 0x7FFF_FFFF (RAM region)
     *
     * For TTBR0 access (identity): VA 0x4000_0000 → L1[1]
     * For TTBR1 access (kernel):   VA 0xFFFF_FF80_4000_0000 → L1[1]
     * Same index, different TTBR selection based on high bits.
     */

    /* L1[0] for MMIO region (0x0000_0000 - 0x3FFF_FFFF) */
    l1_table[0] = make_table_desc((uint64_t)l2_mmio);
    vmm_state.l2_tables_used++;

    /* L1[1] for RAM region (0x4000_0000 - 0x7FFF_FFFF) */
    l1_table[RAM_BASE >> 30] = make_table_desc((uint64_t)l2_kernel);
    vmm_state.l2_tables_used++;

    /*
     * Populate L2 tables with block mappings.
     */

    /*
     * Map kernel RAM (first 128MB for now).
     * L2 index for 0x4000_0000 within its 1GB region = 0.
     *
     * Since TTBR0 and TTBR1 share the same L1 table, this mapping
     * serves both identity (VA = PA) and kernel high addresses
     * (VA = PA | KERNEL_VA_BASE).
     *
     * Note: Linker script now separates .text (RX) from .data/.bss (RW)
     * in the ELF segments. However, the MMU uses 2MB blocks which are
     * too coarse for per-section permissions. Fine-grained RX/RW mapping
     * requires 4KB pages or 2MB-aligned sections (Phase 3 work).
     */
    uint32_t kernel_flags = VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_EXEC;
    for (int i = 0; i < 64; i++) {  /* 64 × 2MB = 128MB */
        uint64_t pa = RAM_BASE + (i * BLOCK_SIZE);
        l2_kernel[i] = make_block_desc(pa, kernel_flags);
        vmm_state.blocks_mapped++;
    }

    /*
     * Map MMIO devices.
     * GIC is at 0x0800_0000, UART at 0x0900_0000
     * L2 index = PA / 2MB = PA >> 21
     * 0x0800_0000 >> 21 = 64
     * 0x0900_0000 >> 21 = 72
     */
    uint64_t gic_l2_idx = (GIC_DIST_BASE >> BLOCK_SHIFT) & 0x1FF;
    uint64_t uart_l2_idx = (UART_BASE >> BLOCK_SHIFT) & 0x1FF;

    /*
     * MMIO mappings - same table serves both identity and kernel mappings.
     */
    l2_mmio[gic_l2_idx] = make_block_desc(GIC_DIST_BASE & ~(BLOCK_SIZE - 1),
                                           VMM_FLAGS_DEVICE);
    l2_mmio[uart_l2_idx] = make_block_desc(UART_BASE & ~(BLOCK_SIZE - 1),
                                            VMM_FLAGS_DEVICE);
    vmm_state.blocks_mapped += 2;

    DEBUG_PRINT("  L1 table at PA: 0x%lx", (uint64_t)l1_table);
    DEBUG_PRINT("  L2 kernel at PA: 0x%lx", (uint64_t)l2_kernel);
    DEBUG_PRINT("  L2 MMIO at PA: 0x%lx", (uint64_t)l2_mmio);
    DEBUG_PRINT("  TTBR0 = TTBR1 = 0x%lx (shared L1 table)", (uint64_t)l1_table);
    DEBUG_PRINT("  Mapped %u blocks (%lu MB)",
                vmm_state.blocks_mapped,
                (vmm_state.blocks_mapped * BLOCK_SIZE) / (1024 * 1024));

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
    vmm_dump();

    /* Run validation tests */
    vmm_run_tests();
}
