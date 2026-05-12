/*
 * kbuf.c — virtually-contiguous kernel buffer allocator.
 *
 * See kbuf.h for the design rationale (#789 — slm xload OOM on
 * Qwen-1.5B GGUF when the helper weights pool ≥1.5 GB fragments
 * PMM beyond the point where a single order-18 buddy block is
 * available).
 *
 * Two-mode allocation:
 *   - Fast path: one `pmm_alloc_pages(total_pages)` for the whole
 *     request. The returned VA is already in the identity-map; no
 *     VMM mapping needed. Recorded with `n_chunks = 0` so the free
 *     path knows to call `pmm_free_pages` on the single block.
 *   - Slow path: split into 2 MB chunks. Each chunk is an
 *     independent buddy-block allocation. They are mapped into a
 *     bump-allocated kernel VA window (`KBUF_VA_BASE` onwards)
 *     using `vmm_map_block`. The free path unmaps each block and
 *     calls `pmm_free_pages(chunk, KBUF_CHUNK_PAGES)` per chunk.
 *
 * No VA recycling: the bump pointer advances forever within the
 * 32 GB window. At ~1 GB per typical use that's 32 model loads,
 * far more than any plausible operator workflow before reboot.
 * Recycling can be added when a real consumer needs it.
 *
 * Single allocation: a static `g_regions` table backs the bookkeeping
 * with a fixed `KBUF_MAX_REGIONS` cap. Sized for "a few simultaneous
 * loaded models"; matches `SLM_MAX_SLOTS = 4` with headroom.
 */

#include "kbuf.h"

#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "uart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KBUF_MAX_REGIONS              8
/* 4 GB / 2 MB = 2048 chunks max per single allocation. Sized to
 * cover ~2× the largest model we expect to load. */
#define KBUF_MAX_CHUNKS_PER_REGION    2048u

/*
 * Failure reasons captured by `try_slow_path` so `kbuf_alloc` can
 * print the diagnostic AFTER releasing g_lock. Keeps `uart_printf`
 * out of the critical section — important because on QEMU /
 * non-NC-memory platforms the UART driver takes its own spinlock,
 * and we'd otherwise extend the global lock-order chain.
 *
 * Single-writer (try_slow_path under g_lock), single-reader
 * (kbuf_alloc immediately after the unlock). Storage doesn't need
 * separate synchronization since both ends sit under the same
 * lock-acquire/release pair.
 */
enum kbuf_slow_err {
    KBUF_SLOW_OK = 0,
    KBUF_SLOW_OUT_OF_VA,
    KBUF_SLOW_PMM_EXHAUSTION,
    KBUF_SLOW_VMM_MAP_FAIL,
};

struct kbuf_slow_diag {
    enum kbuf_slow_err err;
    uint32_t failed_chunk;
    uint32_t total_chunks;
    uint64_t failed_va;
    uint64_t failed_phys;
    int      vmm_rc;
};

struct kbuf_region {
    /* `va_base == 0` means slot free. */
    uint64_t va_base;
    uint64_t va_end;        /* exclusive */
    size_t   total_bytes;   /* rounded up to KBUF_CHUNK_BYTES granularity */

    /* Allocation kind:
     *   n_chunks == 0  → fast path; `phys_single` holds one PMM
     *                     allocation whose VA equals `va_base`.
     *   n_chunks > 0   → slow path; `phys_chunks[0..n_chunks)` are the
     *                     2 MB blocks mapped into [va_base, va_end). */
    uint32_t n_chunks;
    union {
        void *phys_single;
        uint64_t phys_chunks[KBUF_MAX_CHUNKS_PER_REGION];
    };
};

/*
 * Static table. 8 regions × ~16 KB each = 128 KB BSS — fine.
 * `phys_chunks` is the dominant cost; even with `n_chunks == 0`
 * (fast path) the array is still resident but only the first
 * `phys_single` slot is used.
 */
static struct kbuf_region g_regions[KBUF_MAX_REGIONS];

/* Bump-allocator for the kbuf VA window. Advances forever; no recycling. */
static uint64_t g_next_va = KBUF_VA_BASE;

/* Aggregate counters for diagnostics. Read via accessors. */
static size_t g_total_bytes_allocated;
static size_t g_region_count;

/*
 * Lock guards the table + bump pointer + counters. Allocations
 * are rare (one per `slm xload`) so a single coarse lock is fine.
 * Acquired with `spin_lock_irqsave` so allocator paths can be called
 * with IRQs in any state — current callers all have IRQs on, but
 * the IRQ-save form is the bare-metal-safe default and the overhead
 * is negligible vs. the actual page-table writes.
 */
static spinlock_t g_lock = SPINLOCK_INIT;

/*
 * Test-only knob: when set, `try_fast_path` returns NULL
 * unconditionally so `kbuf_alloc` always falls through to the
 * chunked VMM-remap path. Lets the QEMU unit tests exercise the
 * slow path without needing the post-kexec PMM-fragmentation that
 * triggers it in production. Default OFF.
 */
static bool g_force_slow_path = false;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static inline size_t round_up_chunk(size_t bytes)
{
    return (bytes + KBUF_CHUNK_BYTES - 1u) & ~((size_t)KBUF_CHUNK_BYTES - 1u);
}

static struct kbuf_region *find_free_slot_locked(void)
{
    for (uint32_t i = 0; i < KBUF_MAX_REGIONS; i++) {
        if (g_regions[i].va_base == 0) {
            return &g_regions[i];
        }
    }
    return NULL;
}

static struct kbuf_region *find_region_by_va_locked(uint64_t va)
{
    for (uint32_t i = 0; i < KBUF_MAX_REGIONS; i++) {
        if (g_regions[i].va_base == va) {
            return &g_regions[i];
        }
    }
    return NULL;
}

/*
 * Try the contiguous fast path. Returns the PMM pointer on success
 * (already kernel-VA), or NULL if the buddy allocator can't satisfy
 * the request as a single block. Either way the table slot pointer
 * is the same one the caller already reserved.
 *
 * On success: r->n_chunks stays 0, phys_single is set, va_base and
 * va_end mirror the identity-mapped VA range.
 *
 * If the test knob `g_force_slow_path` is set, returns NULL without
 * even calling PMM — used by `test_kbuf.c` to exercise the chunked
 * path under QEMU where fragmentation pressure never naturally
 * triggers it.
 */
static void *try_fast_path(struct kbuf_region *r, size_t total_bytes)
{
    if (g_force_slow_path) {
        return NULL;
    }
    size_t total_pages = total_bytes / PAGE_SIZE;
    void *p = pmm_alloc_pages(total_pages);
    if (p == NULL) {
        return NULL;
    }
    r->va_base     = (uint64_t)(uintptr_t)p;
    r->va_end      = r->va_base + total_bytes;
    r->total_bytes = total_bytes;
    r->n_chunks    = 0;
    r->phys_single = p;
    return p;
}

/*
 * Slow path: allocate N × 2 MB chunks, map them into the kbuf VA
 * window. Cleans up partial state on any failure (unmaps installed
 * blocks, frees allocated chunks).
 *
 * Returns the base VA on success, NULL otherwise. On failure the
 * caller's table slot is left zeroed so it's reusable. The reason
 * is written into `*diag` so the caller can `uart_printf` it AFTER
 * releasing g_lock (avoids `g_lock → uart_lock` lock-order edge on
 * non-NC platforms).
 */
static void *try_slow_path(struct kbuf_region *r, size_t total_bytes,
                            struct kbuf_slow_diag *diag)
{
    uint32_t n_chunks = (uint32_t)(total_bytes / KBUF_CHUNK_BYTES);
    if (n_chunks == 0 || n_chunks > KBUF_MAX_CHUNKS_PER_REGION) {
        return NULL;
    }
    /* Reserve VA range. The bump-allocator never recycles, so we
     * just advance once we commit to this request. */
    if (g_next_va + total_bytes > KBUF_VA_LIMIT) {
        diag->err          = KBUF_SLOW_OUT_OF_VA;
        diag->total_chunks = n_chunks;
        return NULL;
    }
    uint64_t va_base = g_next_va;

    /* Ensure every L1 entry the request spans has an L2 table
     * installed. vmm_init only pre-populates the identity-map
     * L1 slots; the kbuf VA window sits above that and would
     * otherwise hit "no L2 table for VA..." on the first chunk.
     * Idempotent — repeat allocations into the same 1 GB span
     * skip the alloc. */
    for (uint64_t off = 0; off < total_bytes;
         off += (1UL << 30) /* 1 GB L1 stride */) {
        if (vmm_ensure_kernel_l2_table(va_base + off) != 0) {
            diag->err          = KBUF_SLOW_VMM_MAP_FAIL;
            diag->failed_chunk = 0;
            diag->total_chunks = n_chunks;
            diag->failed_va    = va_base + off;
            diag->failed_phys  = 0;
            diag->vmm_rc       = -1;
            return NULL;
        }
    }

    /* Allocate + map every chunk. On any failure, unmap + free
     * what's been installed so far. */
    for (uint32_t i = 0; i < n_chunks; i++) {
        void *phys = pmm_alloc_pages(KBUF_CHUNK_PAGES);
        if (phys == NULL) {
            diag->err          = KBUF_SLOW_PMM_EXHAUSTION;
            diag->failed_chunk = i;
            diag->total_chunks = n_chunks;
            for (uint32_t j = 0; j < i; j++) {
                (void)vmm_unmap_block(va_base + (uint64_t)j * KBUF_CHUNK_BYTES);
                pmm_free_pages((void *)(uintptr_t)r->phys_chunks[j],
                               KBUF_CHUNK_PAGES);
            }
            return NULL;
        }
        uint64_t chunk_va = va_base + (uint64_t)i * KBUF_CHUNK_BYTES;
        int rc = vmm_map_block(chunk_va, (uint64_t)(uintptr_t)phys,
                               VMM_FLAGS_KERNEL_DATA);
        if (rc != 0) {
            diag->err          = KBUF_SLOW_VMM_MAP_FAIL;
            diag->failed_chunk = i;
            diag->total_chunks = n_chunks;
            diag->failed_va    = chunk_va;
            diag->failed_phys  = (uint64_t)(uintptr_t)phys;
            diag->vmm_rc       = rc;
            pmm_free_pages(phys, KBUF_CHUNK_PAGES);
            for (uint32_t j = 0; j < i; j++) {
                (void)vmm_unmap_block(va_base + (uint64_t)j * KBUF_CHUNK_BYTES);
                pmm_free_pages((void *)(uintptr_t)r->phys_chunks[j],
                               KBUF_CHUNK_PAGES);
            }
            return NULL;
        }
        r->phys_chunks[i] = (uint64_t)(uintptr_t)phys;
    }

    r->va_base     = va_base;
    r->va_end      = va_base + total_bytes;
    r->total_bytes = total_bytes;
    r->n_chunks    = n_chunks;
    g_next_va += total_bytes;
    return (void *)(uintptr_t)va_base;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void *kbuf_alloc(size_t bytes)
{
    if (bytes == 0) {
        return NULL;
    }
    size_t total_bytes = round_up_chunk(bytes);
    struct kbuf_slow_diag diag = { .err = KBUF_SLOW_OK };

    irq_flags_t flags = spin_lock_irqsave(&g_lock);

    struct kbuf_region *r = find_free_slot_locked();
    if (r == NULL) {
        spin_unlock_irqrestore(&g_lock, flags);
        uart_puts("[kbuf] region table full\n");
        return NULL;
    }
    memset(r, 0, sizeof(*r));

    /* Fast path first — preserves physical contiguity for the
     * common case where the buddy allocator can satisfy. */
    void *p = try_fast_path(r, total_bytes);
    if (p == NULL) {
        p = try_slow_path(r, total_bytes, &diag);
    }
    if (p != NULL) {
        g_total_bytes_allocated += total_bytes;
        g_region_count++;
    } else {
        /* Both paths failed — leave slot zero. */
        memset(r, 0, sizeof(*r));
    }

    spin_unlock_irqrestore(&g_lock, flags);

    /* Now that g_lock is released, surface any deferred diagnostic
     * from try_slow_path. Logging here keeps `uart_printf` out of
     * the critical section (matters on platforms where the UART
     * driver has its own spinlock). */
    if (p == NULL) {
        switch (diag.err) {
        case KBUF_SLOW_OUT_OF_VA:
            uart_puts("[kbuf] out of VA window — slow path refused\n");
            break;
        case KBUF_SLOW_PMM_EXHAUSTION:
            uart_printf("[kbuf] slow-path PMM exhaustion at chunk %u/%u\n",
                        (unsigned)diag.failed_chunk,
                        (unsigned)diag.total_chunks);
            break;
        case KBUF_SLOW_VMM_MAP_FAIL:
            uart_printf("[kbuf] vmm_map_block(0x%lx -> 0x%lx) rc=%d "
                        "(chunk %u/%u)\n",
                        (unsigned long)diag.failed_va,
                        (unsigned long)diag.failed_phys,
                        diag.vmm_rc,
                        (unsigned)diag.failed_chunk,
                        (unsigned)diag.total_chunks);
            break;
        case KBUF_SLOW_OK:
            /* Reached only when `try_fast_path` returned NULL and
             * `try_slow_path` was never called (e.g., total_bytes
             * misaligned past KBUF_MAX_CHUNKS_PER_REGION). The
             * fast-path failure is informationally sufficient on
             * its own; nothing more to print. */
            break;
        }
    }
    return p;
}

void kbuf_free(void *va)
{
    if (va == NULL) {
        return;
    }
    uint64_t addr = (uint64_t)(uintptr_t)va;

    irq_flags_t flags = spin_lock_irqsave(&g_lock);

    struct kbuf_region *r = find_region_by_va_locked(addr);
    if (r == NULL) {
        spin_unlock_irqrestore(&g_lock, flags);
        uart_printf("[kbuf] kbuf_free(%p) — no region match\n", va);
        return;
    }

    if (r->n_chunks == 0) {
        /* Fast-path region: single PMM allocation. */
        pmm_free_pages(r->phys_single, r->total_bytes / PAGE_SIZE);
    } else {
        for (uint32_t i = 0; i < r->n_chunks; i++) {
            (void)vmm_unmap_block(r->va_base + (uint64_t)i * KBUF_CHUNK_BYTES);
            pmm_free_pages((void *)(uintptr_t)r->phys_chunks[i],
                           KBUF_CHUNK_PAGES);
        }
    }

    g_total_bytes_allocated -= r->total_bytes;
    g_region_count--;
    memset(r, 0, sizeof(*r));

    spin_unlock_irqrestore(&g_lock, flags);
}

bool kbuf_owns_va(const void *va)
{
    /* NULL is never owned — and explicitly handling it here avoids
     * a false positive against a free table slot, which is
     * sentinel-marked with `va_base == 0`. */
    if (va == NULL) {
        return false;
    }
    uint64_t addr = (uint64_t)(uintptr_t)va;
    /* Fast-path allocations live in the identity map — they have a
     * VA below KBUF_VA_BASE — but they're still tracked in our
     * table. To keep `slm_free_pages` routing simple, check the
     * table rather than a range. */
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    bool owned = (find_region_by_va_locked(addr) != NULL);
    spin_unlock_irqrestore(&g_lock, flags);
    return owned;
}

size_t kbuf_total_bytes_allocated(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    size_t v = g_total_bytes_allocated;
    spin_unlock_irqrestore(&g_lock, flags);
    return v;
}

size_t kbuf_region_count(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    size_t v = g_region_count;
    spin_unlock_irqrestore(&g_lock, flags);
    return v;
}

/*
 * Test-only knob (declared in kbuf.h). Flips `g_force_slow_path`,
 * which makes `try_fast_path` short-circuit to NULL so every
 * `kbuf_alloc` falls through to the chunked VMM-remap path. Used
 * by `test_kbuf.c` to exercise the slow path under QEMU. Not
 * wired to any production caller.
 */
void kbuf_test_set_force_slow_path(bool on)
{
    irq_flags_t flags = spin_lock_irqsave(&g_lock);
    g_force_slow_path = on;
    spin_unlock_irqrestore(&g_lock, flags);
}
