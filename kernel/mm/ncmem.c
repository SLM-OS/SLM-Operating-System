/*
 * ncmem.c - Non-cacheable DMA memory bump allocator
 *
 * Provides permanent, boot-time allocations from the 2MB NC region for
 * device DMA buffers (see ncmem.h). Memory is never freed.
 *
 * Works on any platform with PLATFORM_HAS_NC_REGION (Pi 5, Jetson).
 *
 * The by-VA block maintenance (#1196) is built on every ARM64 platform so
 * the QEMU test suite can drive it.
 */

#include "ncmem.h"
#include "uart.h"
#include "debug.h"
#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
 * #1196: clean and invalidate a block by VA, keeping a prefix's DRAM data
 * ========================================================================== */

#define NCMEM_MAINT_PAGE  4096U
#define NCMEM_FNV_OFFSET  0xcbf29ce484222325ULL
#define NCMEM_FNV_PRIME   0x00000100000001b3ULL

/* One page copied before the maintenance (ncmem.h: boot and tests
 * only, single caller). */
static uint64_t ncmem_maint_page[NCMEM_MAINT_PAGE / sizeof(uint64_t)];

void ncmem_dc_civac_line(uintptr_t line_addr)
{
    __asm__ volatile("dc civac, %0" :: "r"(line_addr) : "memory");
}

static inline uint64_t ncmem_fnv(uint64_t h, uint64_t v)
{
    return (h ^ v) * NCMEM_FNV_PRIME;
}

static inline uint64_t ncmem_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

int ncmem_clean_inval_range(uintptr_t base, size_t size,
                            size_t preserve_bytes, ncmem_line_op_t op,
                            ncmem_maint_stats_t *st)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    /* CTR_EL0.DminLine [19:16] = log2(words) of the smallest D line, the
     * stride of every other by-VA loop in the kernel (mmu.S, efi_stub.c). */
    uint32_t line = 4U << ((ctr >> 16) & 0xF);

    *st = (ncmem_maint_stats_t){
        .base = base,
        .size = size,
        .preserve_bytes = preserve_bytes,
        .line = line,
        .preserve_hash_before = NCMEM_FNV_OFFSET,
        .preserve_hash_after = NCMEM_FNV_OFFSET,
    };
    if (op == NULL || size == 0 || line < sizeof(uint64_t) ||
        line > NCMEM_MAINT_PAGE ||
        (base % NCMEM_MAINT_PAGE) != 0 || (size % NCMEM_MAINT_PAGE) != 0 ||
        base + size < base || preserve_bytes > size ||
        (preserve_bytes % line) != 0) {
        return -1;
    }

    const size_t words = NCMEM_MAINT_PAGE / sizeof(uint64_t);
    const size_t line_words = line / sizeof(uint64_t);
    const uintptr_t keep_end = base + preserve_bytes;
    uint64_t t0 = ncmem_cntpct();

    for (uintptr_t page = base; page < base + size; page += NCMEM_MAINT_PAGE) {
        volatile uint64_t *p = (volatile uint64_t *)page;

        /* Through the NC mapping this reads DRAM, unless the core looks
         * up its caches for non-cacheable reads (IMPLEMENTATION DEFINED
         * for mismatched attributes, Arm ARM B2.11). Then a dirty line is
         * copied as well, and nothing below counts or restores it. */
        for (size_t i = 0; i < words; i++) {
            ncmem_maint_page[i] = p[i];
        }
        for (uintptr_t a = page; a < page + NCMEM_MAINT_PAGE; a += line) {
            op(a);
            st->lines++;
        }
        __asm__ volatile("dsb sy" ::: "memory");

        bool restored_any = false;
        for (size_t w = 0; w < words; w += line_words) {
            bool differs = false;
            for (size_t k = 0; k < line_words; k++) {
                if (p[w + k] != ncmem_maint_page[w + k]) {
                    differs = true;
                }
            }
            if (!differs) {
                continue;
            }
            st->changed++;
            if (page + w * sizeof(uint64_t) < keep_end) {
                for (size_t k = 0; k < line_words; k++) {
                    p[w + k] = ncmem_maint_page[w + k];
                }
                st->restored++;
                restored_any = true;
            }
        }
        if (restored_any) {
            __asm__ volatile("dsb sy" ::: "memory");
        }

        for (size_t i = 0;
             i < words && page + i * sizeof(uint64_t) < keep_end; i++) {
            st->preserve_hash_before =
                ncmem_fnv(st->preserve_hash_before, ncmem_maint_page[i]);
            st->preserve_hash_after = ncmem_fnv(st->preserve_hash_after, p[i]);
        }
    }

    st->ticks = ncmem_cntpct() - t0;
    return 0;
}

#if defined(PLATFORM_HAS_NC_REGION)

static uintptr_t nc_next_free;
static size_t nc_total_allocated;
static ncmem_maint_stats_t ncmem_boot_maint;

void ncmem_boot_maint_stats(ncmem_maint_stats_t *out)
{
    *out = ncmem_boot_maint;
}

void ncmem_init(void)
{
    /*
     * Reserve the first NC pages for Linux -> SLM-OS handoff/state:
     *   +0x0000 handoff block
     *   +0x1000 retained slot-3 devctx mirror
     *   +0x2000 retained slot-3 input ctx
     *   +0x3000 retained slot-3 EP0 ring alias window (2 pages)
     */
    nc_next_free = NC_MEM_BASE + NC_MEM_RESERVED_BYTES;
    nc_total_allocated = 0;

    /* Don't zero entire 2MB region — callers zero their allocations.
     * NC memory writes bypass cache and are slow for large fills. */

    INFO("NC memory: 0x%lx - 0x%lx (%lu KB)",
         (unsigned long)NC_MEM_BASE,
         (unsigned long)(NC_MEM_BASE + NC_MEM_SIZE),
         (unsigned long)(NC_MEM_SIZE / 1024));

    /*
     * #1196: clean and invalidate the whole block by VA before anything
     * is allocated from it. On Jetson CPU 0 is not reset across kexec or
     * the UEFI hand-off, so its caches may hold Write-Back lines for this
     * block from Linux's linear map or UEFI's identity map. SLM-OS maps
     * it Normal NC, so such a line is a mismatched-attribute alias: if
     * dirty, its eviction at any later time would write the previous
     * owner's data over DMA rings and buffers or the diagnostic slots
     * (Arm ARM B2.11). Maintenance by VA reaches the PoC whatever the
     * cacheability of the mapping used.
     *
     * Nothing has written the block through the NC mapping yet (vmm_init
     * to here touches page tables, PMM metadata and the GPU reserve), and
     * IRQ and FIQ are still masked (#1198), so no handler writes the
     * diagnostic slots meanwhile. On UEFI-direct boots the EFI stub has
     * already cleaned the block with UEFI's mapping live (efi_stub.c);
     * this pass is the one that covers kexec.
     *
     * The handoff pages (NC_MEM_RESERVED_BYTES) keep their DRAM contents.
     * Linux writes them only through ioremap_wc, which arm64 refuses for
     * memory in its linear map (arch/arm64/mm/ioremap.c), so a Write-Back
     * line for them is never newer than DRAM. A line there that the
     * maintenance changes is written back from the copy taken just before.
     *
     * Pi 5 does not need this: its cores are cold-reset and nothing runs
     * through the A76 caches in this range before SLM-OS (the firmware
     * runs on the VideoCore, BL31 in low memory). It runs anyway, one code
     * path for both boards, and its count is a negative control.
     */
    int rc = ncmem_clean_inval_range(NC_MEM_BASE, NC_MEM_SIZE,
                                     NC_MEM_RESERVED_BYTES,
                                     ncmem_dc_civac_line, &ncmem_boot_maint);
    if (rc != 0) {
        WARN("NC memory: clean+invalidate rejected its arguments (#1196)");
        return;
    }

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long us = freq != 0
        ? (unsigned long)(ncmem_boot_maint.ticks * 1000000ULL / freq)
        : 0;
    INFO("NC memory: clean+invalidate by VA, %u lines of %u B, %lu us; "
         "%u changed, %u restored in the first %lu KB "
         "(hash 0x%lx -> 0x%lx)",
         ncmem_boot_maint.lines, ncmem_boot_maint.line, us,
         ncmem_boot_maint.changed, ncmem_boot_maint.restored,
         (unsigned long)(NC_MEM_RESERVED_BYTES / 1024),
         (unsigned long)ncmem_boot_maint.preserve_hash_before,
         (unsigned long)ncmem_boot_maint.preserve_hash_after);
    if (ncmem_boot_maint.changed != 0) {
        WARN("NC memory: %u lines were written back from stale dirty "
             "cache lines (#1196)", ncmem_boot_maint.changed);
    }
}

void *ncmem_alloc(size_t size, size_t align)
{
    if (size == 0 || align == 0) {
        return NULL;
    }

    /* Round up to alignment */
    uintptr_t aligned = (nc_next_free + align - 1) & ~(align - 1);

    /* Rounding wrapped — treat as out of memory */
    if (aligned < nc_next_free) {
        return NULL;
    }

    /* size addition wrapped — treat as out of memory */
    if (aligned + size < aligned) {
        return NULL;
    }

    if (aligned + size > NC_MEM_BASE + NC_MEM_SIZE) {
        WARN("ncmem_alloc: out of NC memory (requested %zu, used %zu/%lu)",
             size, nc_total_allocated, (unsigned long)NC_MEM_SIZE);
        return NULL;
    }

    nc_next_free = aligned + size;
    nc_total_allocated = nc_next_free - NC_MEM_BASE;

    return (void *)aligned;
}

size_t ncmem_used(void)
{
    return nc_total_allocated;
}

#else /* no NC block */

void ncmem_boot_maint_stats(ncmem_maint_stats_t *out)
{
    *out = (ncmem_maint_stats_t){ 0 };
}

#endif /* PLATFORM_HAS_NC_REGION */
