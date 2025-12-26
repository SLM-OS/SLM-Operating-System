/*
 * vmm.h - Virtual Memory Manager for SLM-OS
 *
 * Manages ARM64 page tables and virtual address translation.
 * Uses 4KB granule with 2-level tables (L1 → L2 blocks) for 2MB mappings.
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>
#include "pmm.h"  /* For PAGE_SIZE, PAGE_SHIFT */

/*
 * ==========================================================================
 * Address Space Configuration
 * ==========================================================================
 */

/* Virtual address size: 39 bits = 512GB address space */
#define VA_BITS             39

/* Block sizes (PAGE_SIZE and PAGE_SHIFT come from pmm.h) */
#define BLOCK_SIZE          (2 * 1024 * 1024)   /* 2MB - L2 blocks */
#define BLOCK_SHIFT         21
#define L1_BLOCK_SIZE       (1UL * 1024 * 1024 * 1024)  /* 1GB - L1 blocks */
#define L1_BLOCK_SHIFT      30

/* Table sizes */
#define ENTRIES_PER_TABLE   512                 /* 2^9 entries per table */
#define TABLE_SIZE          (ENTRIES_PER_TABLE * sizeof(uint64_t))  /* 4KB */

/* Index extraction from virtual address */
#define L1_INDEX(va)        (((va) >> 30) & 0x1FF)  /* Bits [38:30] */
#define L2_INDEX(va)        (((va) >> 21) & 0x1FF)  /* Bits [29:21] */
#define L3_INDEX(va)        (((va) >> 12) & 0x1FF)  /* Bits [20:12] */
#define PAGE_OFFSET(va)     ((va) & 0xFFF)          /* Bits [11:0] */

/* Kernel virtual address base (TTBR1 region) */
#define KERNEL_VA_BASE      0xFFFFFF8000000000UL    /* 39-bit, top bits = 1 */

/* Convert physical to kernel virtual address */
#define PA_TO_KVA(pa)       ((pa) | KERNEL_VA_BASE)
#define KVA_TO_PA(va)       ((va) & ~KERNEL_VA_BASE)

/*
 * ==========================================================================
 * Page Table Entry Definitions
 * ==========================================================================
 */

/*
 * Descriptor types (bits [1:0])
 */
#define PTE_TYPE_INVALID    0x0
#define PTE_TYPE_BLOCK      0x1     /* L1/L2: block descriptor */
#define PTE_TYPE_TABLE      0x3     /* L1/L2: table descriptor */
#define PTE_TYPE_PAGE       0x3     /* L3: page descriptor */

#define PTE_TYPE_MASK       0x3

/*
 * Lower attributes (bits [11:2])
 */
#define PTE_ATTR_INDEX(n)   (((uint64_t)(n) & 0x7) << 2)    /* MAIR index */
#define PTE_NS              (1UL << 5)          /* Non-secure */
#define PTE_AP_RW_EL1       (0UL << 6)          /* R/W at EL1, none at EL0 */
#define PTE_AP_RW_ALL       (1UL << 6)          /* R/W at EL1 and EL0 */
#define PTE_AP_RO_EL1       (2UL << 6)          /* R/O at EL1, none at EL0 */
#define PTE_AP_RO_ALL       (3UL << 6)          /* R/O at EL1 and EL0 */
#define PTE_SH_NON          (0UL << 8)          /* Non-shareable */
#define PTE_SH_OUTER        (2UL << 8)          /* Outer shareable */
#define PTE_SH_INNER        (3UL << 8)          /* Inner shareable */
#define PTE_AF              (1UL << 10)         /* Access flag (must be 1) */

/*
 * Upper attributes (bits [63:52])
 */
#define PTE_PXN             (1UL << 53)         /* Privileged execute never */
#define PTE_UXN             (1UL << 54)         /* Unprivileged execute never */

/* Software-defined bits (bits [58:55] available for OS use) */
#define PTE_SW_GPU_MAPPED   (1UL << 55)         /* Page is mapped to GPU */
#define PTE_SW_MODEL_PAGE   (1UL << 56)         /* Contains model data */
#define PTE_SW_INFERENCE_HOT (1UL << 57)        /* Frequently accessed */

/* Output address mask (bits [47:12] for 4KB, [47:21] for 2MB) */
#define PTE_ADDR_MASK       0x0000FFFFFFFFF000UL

/*
 * ==========================================================================
 * MAIR Configuration
 * ==========================================================================
 */

/* Memory attribute encodings */
#define MAIR_DEVICE_nGnRnE  0x00UL  /* Device: no Gather, no Reorder, no Early ack */
#define MAIR_DEVICE_nGnRE   0x04UL  /* Device: no Gather, no Reorder, Early ack */
#define MAIR_NORMAL_NC      0x44UL  /* Normal: Non-cacheable */
#define MAIR_NORMAL_WB      0xFFUL  /* Normal: Write-back, read/write allocate */

/* MAIR index assignments */
#define MAIR_IDX_DEVICE_nGnRnE  0
#define MAIR_IDX_DEVICE_nGnRE   1
#define MAIR_IDX_NORMAL_NC      2
#define MAIR_IDX_NORMAL_WB      3

/* Complete MAIR_EL1 value */
#define MAIR_EL1_VALUE      ((MAIR_DEVICE_nGnRnE << (8 * MAIR_IDX_DEVICE_nGnRnE)) | \
                             (MAIR_DEVICE_nGnRE  << (8 * MAIR_IDX_DEVICE_nGnRE))  | \
                             (MAIR_NORMAL_NC     << (8 * MAIR_IDX_NORMAL_NC))     | \
                             (MAIR_NORMAL_WB     << (8 * MAIR_IDX_NORMAL_WB)))

/*
 * ==========================================================================
 * TCR Configuration
 * ==========================================================================
 */

#define TCR_T0SZ(n)         ((64UL - (n)) << 0)     /* TTBR0 region size */
#define TCR_T1SZ(n)         ((64UL - (n)) << 16)    /* TTBR1 region size */
#define TCR_TG0_4KB         (0UL << 14)             /* TTBR0 granule: 4KB */
#define TCR_TG1_4KB         (2UL << 30)             /* TTBR1 granule: 4KB */
#define TCR_SH0_INNER       (3UL << 12)             /* TTBR0 inner shareable */
#define TCR_SH1_INNER       (3UL << 28)             /* TTBR1 inner shareable */
#define TCR_ORGN0_WB_WA     (1UL << 10)             /* TTBR0 outer WB, write-alloc */
#define TCR_ORGN1_WB_WA     (1UL << 26)             /* TTBR1 outer WB, write-alloc */
#define TCR_IRGN0_WB_WA     (1UL << 8)              /* TTBR0 inner WB, write-alloc */
#define TCR_IRGN1_WB_WA     (1UL << 24)             /* TTBR1 inner WB, write-alloc */
#define TCR_EPD0            (1UL << 7)              /* Disable TTBR0 walks */
#define TCR_EPD1            (1UL << 23)             /* Disable TTBR1 walks */
#define TCR_IPS_40BIT       (2UL << 32)             /* 40-bit physical addresses */

/* TCR value for SLM-OS: 39-bit VA, 4KB granule, both TTBR0 and TTBR1 enabled */
#define TCR_EL1_VALUE       (TCR_T0SZ(39)     | \
                             TCR_T1SZ(39)     | \
                             TCR_TG0_4KB      | \
                             TCR_TG1_4KB      | \
                             TCR_SH0_INNER    | \
                             TCR_SH1_INNER    | \
                             TCR_ORGN0_WB_WA  | \
                             TCR_ORGN1_WB_WA  | \
                             TCR_IRGN0_WB_WA  | \
                             TCR_IRGN1_WB_WA  | \
                             TCR_IPS_40BIT)   /* Note: EPD0 NOT set - need TTBR0 for identity mapping */

/*
 * ==========================================================================
 * SCTLR Configuration
 * ==========================================================================
 */

#define SCTLR_M             (1UL << 0)      /* MMU enable */
#define SCTLR_A             (1UL << 1)      /* Alignment check enable */
#define SCTLR_C             (1UL << 2)      /* Data cache enable */
#define SCTLR_SA            (1UL << 3)      /* SP alignment check */
#define SCTLR_I             (1UL << 12)     /* Instruction cache enable */
#define SCTLR_WXN           (1UL << 19)     /* Write implies XN */

/*
 * ==========================================================================
 * Memory Flags for vmm_map functions
 * ==========================================================================
 */

/* Memory type flags */
#define VMM_FLAG_DEVICE         (1U << 0)   /* Device memory (nGnRnE) */
#define VMM_FLAG_NORMAL         (0U << 0)   /* Normal memory (default) */
#define VMM_FLAG_NOCACHE        (1U << 1)   /* Non-cacheable */
#define VMM_FLAG_CACHED         (0U << 1)   /* Write-back cached (default) */

/* Access flags */
#define VMM_FLAG_READ           (1U << 2)   /* Readable */
#define VMM_FLAG_WRITE          (1U << 3)   /* Writable */
#define VMM_FLAG_EXEC           (1U << 4)   /* Executable */

/* SLM-specific flags */
#define VMM_FLAG_GPU_MAPPED     (1U << 8)   /* Mapped to GPU */
#define VMM_FLAG_MODEL_PAGE     (1U << 9)   /* Contains model data */
#define VMM_FLAG_INFERENCE_HOT  (1U << 10)  /* Hot inference page */

/* Common flag combinations */
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_READ | VMM_FLAG_EXEC)
#define VMM_FLAGS_KERNEL_DATA   (VMM_FLAG_READ | VMM_FLAG_WRITE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_READ)
#define VMM_FLAGS_DEVICE        (VMM_FLAG_DEVICE | VMM_FLAG_READ | VMM_FLAG_WRITE)
#define VMM_FLAGS_DMA           (VMM_FLAG_NOCACHE | VMM_FLAG_READ | VMM_FLAG_WRITE)

/*
 * ==========================================================================
 * VMM API
 * ==========================================================================
 */

/*
 * Initialize the virtual memory manager.
 *
 * Creates initial kernel page tables and enables the MMU.
 * After this call, all kernel code runs at virtual addresses.
 *
 * This function:
 * 1. Allocates L1 and L2 tables from physical memory
 * 2. Creates identity mapping for current code (required for MMU enable)
 * 3. Creates kernel high mapping (KERNEL_VA_BASE + PA)
 * 4. Maps MMIO regions (UART, GIC)
 * 5. Enables MMU and jumps to high kernel address
 */
void vmm_init(void);

/*
 * Map a 2MB block into the kernel address space.
 *
 * @virt:  Virtual address (must be 2MB aligned)
 * @phys:  Physical address (must be 2MB aligned)
 * @flags: VMM_FLAG_* flags
 *
 * Returns 0 on success, -1 on error.
 */
int vmm_map_block(uint64_t virt, uint64_t phys, uint32_t flags);

/*
 * Unmap a 2MB block from the kernel address space.
 *
 * @virt: Virtual address (must be 2MB aligned)
 *
 * Returns 0 on success, -1 if not mapped.
 */
int vmm_unmap_block(uint64_t virt);

/*
 * Map a contiguous region into the kernel address space.
 *
 * @virt:  Virtual address (must be 2MB aligned)
 * @phys:  Physical address (must be 2MB aligned)
 * @size:  Size in bytes (will be rounded up to 2MB)
 * @flags: VMM_FLAG_* flags
 *
 * Returns 0 on success, -1 on error.
 */
int vmm_map_region(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags);

/*
 * Translate a virtual address to physical.
 *
 * @virt: Virtual address to translate
 *
 * Returns physical address, or 0 if not mapped (note: PA 0 is valid,
 * so check vmm_is_mapped() first if needed).
 */
uint64_t vmm_virt_to_phys(uint64_t virt);

/*
 * Check if a virtual address is mapped.
 *
 * @virt: Virtual address to check
 *
 * Returns true if mapped, false otherwise.
 */
bool vmm_is_mapped(uint64_t virt);

/*
 * ==========================================================================
 * TLB Shootdown API
 * ==========================================================================
 *
 * ARM64 TLB invalidation with the "is" (inner shareable) suffix broadcasts
 * to all CPUs in the inner shareable domain automatically. This provides
 * hardware-assisted TLB shootdown without requiring explicit IPIs.
 *
 * All functions use DSB (Data Synchronization Barrier) to ensure:
 * - DSB ISHST before: all prior stores are visible before invalidation
 * - DSB ISH after: invalidation is complete on all CPUs before continuing
 * - ISB: instruction stream is synchronized with TLB state
 *
 * Thread Safety: These functions are safe to call from any CPU. The hardware
 * broadcast mechanism ensures coherency across all cores.
 */

/*
 * Invalidate TLB for a specific address (all CPUs).
 *
 * Uses TLBI VAAE1IS (VA, All ASIDs, EL1, Inner Shareable).
 * Broadcasts to all CPUs in the inner shareable domain.
 *
 * @virt: Virtual address to invalidate (any alignment)
 */
void vmm_invalidate_tlb(uint64_t virt);

/*
 * Invalidate TLB for a virtual address range (all CPUs).
 *
 * Efficiently invalidates all TLB entries covering the given range.
 * For large ranges (> 32 pages), falls back to full TLB flush.
 *
 * @start: Start virtual address (will be page-aligned)
 * @end:   End virtual address (exclusive)
 */
void vmm_invalidate_tlb_range(uint64_t start, uint64_t end);

/*
 * Invalidate TLB for a specific address and ASID (all CPUs).
 *
 * Uses TLBI VAE1IS (VA, specified ASID, EL1, Inner Shareable).
 * Only invalidates entries matching both the VA and ASID.
 * Useful for per-process address space management.
 *
 * @virt: Virtual address to invalidate
 * @asid: Address Space Identifier (0-65535)
 */
void vmm_invalidate_tlb_asid(uint64_t virt, uint16_t asid);

/*
 * Invalidate all TLB entries for a specific ASID (all CPUs).
 *
 * Uses TLBI ASIDE1IS (All addresses for ASID, Inner Shareable).
 * Useful for process exit or address space destruction.
 *
 * @asid: Address Space Identifier (0-65535)
 */
void vmm_invalidate_tlb_asid_all(uint16_t asid);

/*
 * Invalidate entire TLB (all CPUs).
 *
 * Uses TLBI VMALLE1IS (VM All EL1, Inner Shareable).
 * Broadcasts to all CPUs in the inner shareable domain.
 * Use sparingly - invalidates all cached translations.
 */
void vmm_invalidate_tlb_all(void);

/*
 * Dump page table mappings for debugging.
 */
void vmm_dump(void);

/*
 * Get VMM statistics.
 */
struct vmm_stats {
    uint32_t l1_tables;         /* Number of L1 tables */
    uint32_t l2_tables;         /* Number of L2 tables allocated */
    uint32_t blocks_mapped;     /* Number of 2MB blocks mapped */
    uint64_t bytes_mapped;      /* Total bytes mapped */
};

void vmm_get_stats(struct vmm_stats *stats);

/*
 * ==========================================================================
 * Test Helpers (for unit tests only - do NOT use in production)
 * ==========================================================================
 */

/*
 * Get the raw L2 PTE for a virtual address.
 */
uint64_t vmm_test_get_l2_entry(uint64_t virt);

/*
 * Set the L2 PTE WITHOUT invalidating TLB.
 * Creates intentional TLB/page-table inconsistency for testing.
 */
int vmm_test_set_l2_entry_no_invalidate(uint64_t virt, uint64_t pte);

/*
 * Build a block descriptor for testing.
 */
uint64_t vmm_test_make_block_desc(uint64_t phys, uint32_t flags);

/*
 * ==========================================================================
 * Low-level helpers (for mmu.S)
 * ==========================================================================
 */

/* Enable MMU (implemented in assembly) */
extern void mmu_enable(uint64_t ttbr1, uint64_t tcr, uint64_t mair, uint64_t sctlr);

/* Get current page table base */
uint64_t vmm_get_ttbr1(void);

#endif /* VMM_H */
