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
 * Per-task user VA range (#697 PR-2).
 *
 * SLM-OS keeps the kernel running at low VA in a shared TTBR0=TTBR1
 * mapping. Per-task L1 tables (allocated by vmm_create_user_l1)
 * mirror the boot L1's entries for L1[0..USER_L1_FIRST-1] (kernel
 * mappings, shared via L2-table pointer copies) and have their own
 * entries for L1[USER_L1_FIRST..USER_L1_LIMIT-1] (per-task user
 * mappings, populated by task_create_user in PR-3).
 *
 * USER_L1_FIRST = 256 was chosen because every current platform's
 * kernel mappings stay below L1[256] (Pi 5 uses up to L1[124] for
 * MMIO; QEMU/Jetson use the low 8). 256 GB of user VA is far more
 * than any user task needs.
 */
#define USER_L1_FIRST       256                  /* First L1 index for user mappings */
#define USER_L1_LIMIT       512                  /* One past last L1 index for user */
#define USER_VA_BASE        ((uint64_t)USER_L1_FIRST * L1_BLOCK_SIZE)
#define USER_VA_LIMIT       ((uint64_t)USER_L1_LIMIT * L1_BLOCK_SIZE)

/*
 * #697 PR-4 — fixed user-VA layout for the smoke EL0 task.
 *
 * Two 4 KB pages at the bottom of the user window:
 *   USER_TEXT_VA       — first page, RX (.text.user maps here).
 *   USER_STACK_PAGE_VA — second page, RW (per-task stack page).
 *   USER_STACK_TOP     — one past the stack page; SP_EL0 starts here
 *                        and grows down into USER_STACK_PAGE_VA.
 *
 * A future ELF loader will replace these constants with per-task
 * layout. For the smoke we hard-code the addresses since both pages
 * are sized to the smoke's needs (one code page, one stack page).
 */
#define USER_TEXT_VA        USER_VA_BASE
#define USER_STACK_PAGE_VA  (USER_VA_BASE + PAGE_SIZE)
#define USER_STACK_TOP      (USER_STACK_PAGE_VA + PAGE_SIZE)

/*
 * Bump-allocator window for sys_mmap. Starts 1 MB into the user
 * window — well clear of .text.user (1 page) and the stack (1 page),
 * with plenty of headroom for either to grow before they collide.
 *
 * Allocator never reuses VA, so a fresh allocation never aliases
 * stale TLB entries from a prior munmap of the same range.
 */
#define USER_MMAP_VA_START  (USER_VA_BASE + 0x100000)

/*
 * Stack layout for EL0 ELF tasks (task_create_user_elf).
 *
 * The smoke task in task_create_user uses USER_STACK_PAGE_VA at
 * USER_VA_BASE + PAGE_SIZE because its .text.user is exactly one
 * page. ELF binaries are multi-page (text + rodata + data + bss),
 * so their stack has to live somewhere that won't collide with the
 * loaded segments. Placing the stack one page below the mmap window
 * gives ELF segments up to 1 MB − 4 KB of contiguous space before
 * they would touch the stack — comfortable for static demo binaries
 * and the limit the user-VA loader enforces today.
 */
#define USER_ELF_STACK_PAGE_VA  (USER_MMAP_VA_START - PAGE_SIZE)
#define USER_ELF_STACK_TOP      (USER_MMAP_VA_START)

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
#define PTE_NG              (1UL << 11)         /* Not Global (ASID-tagged) */

/*
 * Upper attributes (bits [63:52])
 */
#define PTE_PXN             (1UL << 53)         /* Privileged execute never */
#define PTE_UXN             (1UL << 54)         /* Unprivileged execute never */

/* Software-defined bits (bits [58:55] available for OS use) */
#define PTE_SW_GPU_MAPPED   (1UL << 55)         /* Page is mapped to GPU */
#define PTE_SW_MODEL_PAGE   (1UL << 56)         /* Contains model data */
#define PTE_SW_INFERENCE_HOT (1UL << 57)        /* Frequently accessed */
#define PTE_SW_PMM_OWNED    (1UL << 58)         /* PMM-backed user page; free on user L1 destroy */

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
#define TCR_AS              (1UL << 36)             /* 16-bit ASIDs (vs 8-bit) */

/* TCR value for SLM-OS: 39-bit VA, 4KB granule, both TTBR0 and TTBR1 enabled,
 * 16-bit ASIDs. */
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
                             TCR_IPS_40BIT    | \
                             TCR_AS)          /* Note: EPD0 NOT set - need TTBR0 for identity mapping */

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
#define SCTLR_SPAN          (1UL << 23)     /* Set PAN on exception (clear = don't auto-set PAN) */

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

/* User mode flag (Phase 5 M4) */
#define VMM_FLAG_USER           (1U << 5)   /* EL0 accessible */

/* SLM-specific flags */
#define VMM_FLAG_GPU_MAPPED     (1U << 8)   /* Mapped to GPU */
#define VMM_FLAG_MODEL_PAGE     (1U << 9)   /* Contains model data */
#define VMM_FLAG_INFERENCE_HOT  (1U << 10)  /* Hot inference page */

/*
 * Mark a user-mode mapping as backed by a PMM page that the VMM
 * should reclaim on tear-down. Without this flag, vmm_destroy_user_l1
 * leaves the leaf PA alone (the caller owns it — used today for
 * `.text.user` mappings whose PA is in the kernel image).
 *
 * Set by mmap-style callers and by the per-task EL0 stack mapping.
 */
#define VMM_FLAG_PMM_OWNED      (1U << 11)  /* Free leaf page on user-L1 destroy */

/* Common flag combinations */
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_READ | VMM_FLAG_EXEC)
#define VMM_FLAGS_KERNEL_DATA   (VMM_FLAG_READ | VMM_FLAG_WRITE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_READ)
#define VMM_FLAGS_DEVICE        (VMM_FLAG_DEVICE | VMM_FLAG_READ | VMM_FLAG_WRITE)
#define VMM_FLAGS_DMA           (VMM_FLAG_NOCACHE | VMM_FLAG_READ | VMM_FLAG_WRITE)
#define VMM_FLAGS_USER_CODE     (VMM_FLAG_READ | VMM_FLAG_EXEC | VMM_FLAG_USER)
#define VMM_FLAGS_USER_DATA     (VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER)

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
 * Allocate and initialise a per-task L1 table for TTBR0_EL1 (#697 PR-2).
 *
 * The new L1 mirrors the boot L1's entries for L1[0..USER_L1_FIRST-1]
 * (kernel mappings — pointers to shared L2 tables) and zeroes
 * L1[USER_L1_FIRST..USER_L1_LIMIT-1] (user mappings — populated by
 * task_create_user in PR-3).
 *
 * Mirroring is done at L1 granularity by COPYING L1 entries (which
 * contain L2 table pointers) — the underlying L2 tables are shared
 * with the kernel boot L1 and other per-task L1s. This means any
 * kernel-side mapping change must go through L2-level edits to
 * propagate to existing per-task L1s; direct boot-L1 edits do NOT
 * propagate. This invariant is documented in
 * docs/pi5-el0-execution-plan.md "Risks" section.
 *
 * Concurrency precondition: caller must hold the kernel's VMM
 * serialisation contract. SLM-OS does not maintain a vmm-wide lock;
 * the existing convention is that mutations of l1_table happen
 * single-threaded (boot, or under per-subsystem locks for runtime
 * mappings like PCIe BAR setup). vmm_create_user_l1 reads l1_table,
 * so it inherits that contract — torn reads if a concurrent writer
 * mutates l1_table mid-copy. PR-3 callers must respect this when
 * deciding when to call task_create_user.
 *
 * @out_pa: Output — physical address of the new L1 page (caller-owned).
 *          Pass to vmm_destroy_user_l1 when done.
 *
 * Returns 0 on success, -1 on failure (PMM exhausted or NULL out_pa).
 */
int vmm_create_user_l1(uint64_t *out_pa);

/*
 * Free a per-task L1 table previously returned by vmm_create_user_l1.
 *
 * Frees only the page-table STRUCTURE — the L1 page itself plus any
 * per-task L2 / L3 sub-tables hanging off user-region L1 entries
 * (USER_L1_FIRST..USER_L1_LIMIT-1). Kernel-region L1 entries point
 * to shared L2s and are NOT freed.
 *
 * Caller responsibility: free user backing pages BEFORE calling this.
 * The data pages mapped by 2 MB block / 4 KB page descriptors are
 * NOT freed automatically — destroy doesn't know whether a backing
 * PA is a PMM-allocated user page (needs free) or an alias of a
 * kernel page (e.g. .text.user in PR-4, must NOT be freed). The
 * caller (PR-3+ task_destroy path) tracks page lifetime separately
 * and frees backings before invoking this.
 *
 * Safe to call with l1_pa == 0 (no-op).
 *
 * @l1_pa: Physical address of the L1 page to free.
 */
void vmm_destroy_user_l1(uint64_t l1_pa);

/*
 * ASID reserved for the kernel / boot L1. User PTEs carry nG=1 so the
 * hardware tags them with the active ASID; kernel PTEs are global
 * (nG=0) and apply across all ASIDs. Reverting TTBR0_EL1 to the boot
 * L1 (no user mappings) uses ASID 0 by convention.
 */
#define VMM_KERNEL_ASID     0U

/*
 * Allocate a fresh ASID for a user task.
 *
 * Returns a value in [1, VMM_USER_ASID_MAX]; 0 is reserved for the
 * kernel/boot path. Returns 0 if the pool is exhausted (recoverable
 * via task_destroy → vmm_free_asid; non-recoverable in this PR if
 * the live task count truly exceeds the pool).
 *
 * If the returned ASID was previously in use (recycled after a free),
 * the allocator broadcasts `tlbi aside1is` for that ASID before
 * returning it, so the next user-task swap cannot read stale entries
 * from the prior holder.
 *
 * Allocator state is global; call site is the task-create path, so
 * concurrency is bounded by the task-table spinlock at the caller.
 */
#define VMM_USER_ASID_MAX   255U
uint16_t vmm_alloc_asid(void);

/*
 * Release a user ASID back to the pool.
 *
 * `asid == 0` is a no-op (the kernel ASID is never on the pool).
 * Future allocations may recycle this slot; the broadcast TLB flush
 * happens at the next vmm_alloc_asid that hands it out, not here, so
 * a hot alloc/free loop pays the flush only on the second recycle.
 */
void vmm_free_asid(uint16_t asid);

/*
 * Switch the EL0 address space (TTBR0_EL1) to a per-task L1 table.
 *
 * Composes (asid << 48) | l1_pa into TTBR0_EL1, then ISBs. No TLB
 * invalidation: user PTEs are tagged with `asid` (nG=1) so the
 * hardware disambiguates them from any other ASID's entries that
 * may still be cached. Kernel mappings (nG=0) remain valid through
 * the swap.
 *
 * For the user→kernel revert, pass (vmm_boot_l1_pa(),
 * VMM_KERNEL_ASID) — it composes the same TTBR0 the boot path
 * installed, with no entries that could conflict with kernel
 * translations.
 *
 * Concurrency: TTBR0_EL1 is per-CPU, so no cross-CPU race on the
 * register itself. The caller is expected to hold IRQs disabled
 * across this + the immediately-following switch_to (today:
 * scheduler holds rq_lock_irqsave through both).
 *
 * @l1_pa: PA of the per-task L1 table (or boot L1 for kernel revert).
 *         Must be 4 KB aligned.
 * @asid:  16-bit ASID from vmm_alloc_asid (or VMM_KERNEL_ASID for
 *         the kernel revert).
 */
void vmm_user_addrspace_switch(uint64_t l1_pa, uint16_t asid);

/*
 * Map a single 4 KB page into a per-task L1 (#697 PR-4).
 *
 * Walks the per-task L1 → L2 → L3 chain at `va`, allocating fresh L2
 * and L3 sub-tables from PMM as needed, then installs an L3 page
 * descriptor for `pa` with the requested permissions.
 *
 * `va` must be in the user window [USER_VA_BASE, USER_VA_LIMIT) and
 * page-aligned; `pa` must also be page-aligned. Pass `flags` from the
 * VMM_FLAG_* / VMM_FLAGS_* set — VMM_FLAG_USER controls the AP[1]
 * (EL0-accessible) bit; without it the entry is kernel-only and the
 * mapping is useless to a user task.
 *
 * Sub-tables created here become owned by the L1 — vmm_destroy_user_l1
 * frees them when the task is destroyed.
 *
 * No TLB invalidation is emitted: the caller is expected to either
 * (a) populate mappings before the L1 is loaded into TTBR0_EL1 (the
 * vmm_user_addrspace_switch on schedule() then flushes), or (b) issue
 * a manual flush after a runtime mapping change. PR-4 takes path (a).
 *
 * Returns 0 on success, -1 on validation failure or PMM exhaustion or
 * if the target slot is already mapped (caller is expected not to
 * re-map an active VA).
 */
int vmm_user_map_page(uint64_t l1_pa, uint64_t va, uint64_t pa, uint32_t flags);

/*
 * Tear down a single 4 KB user-mapped page (#697 follow-up: mmap).
 *
 * Walks the per-task L1 → L2 → L3 chain at `va`, clears the L3 entry,
 * and — if the page was mapped with VMM_FLAG_PMM_OWNED — frees the
 * underlying PMM page back to the buddy allocator. Sub-tables (L2, L3
 * page-table pages) are NOT freed here even if they become empty;
 * vmm_destroy_user_l1 reclaims them at task tear-down.
 *
 * No TLB invalidation is emitted. Callers that may have a live TLB
 * entry for `va` (i.e. the L1 is in some CPU's TTBR0_EL1 right now)
 * must follow this with `tlbi vae1is, <va>>>12` + dsb ish + isb.
 *
 * Returns 0 on success, -1 if `va` is out of the user window, not
 * page-aligned, or not currently mapped.
 */
int vmm_user_unmap_page(uint64_t l1_pa, uint64_t va);

/*
 * PA of the boot (kernel) L1 table — the L1 that vmm_init populated
 * at boot, mirrored into every per-task L1 by vmm_create_user_l1.
 *
 * Used by schedule() to restore TTBR0_EL1 when switching from a user
 * task back to a kernel task (#697 PR-4): without the restore, TTBR0
 * keeps pointing at the user task's L1 even after the task is gone,
 * and a subsequent task_destroy frees that L1 while it's still the
 * walker's active root.
 */
uint64_t vmm_boot_l1_pa(void);

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
