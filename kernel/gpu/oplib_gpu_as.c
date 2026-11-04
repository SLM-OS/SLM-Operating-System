/*
 * oplib_gpu_as.c - GPU address-space interface for the operator
 * library: the cold-VM backend and the backend-independent staging
 * helpers (#1092). See oplib_gpu_as.h.
 *
 * Pure code: memory and mappings are reached only through
 * struct oplib_gpu_as (and, for the cold backend, struct
 * ga10b_gpu_ops), so it builds on every platform and runs in the
 * QEMU unit tests against fake page allocators.
 */

#include "oplib_gpu_as.h"
#include "oplib_pool.h"                    /* OPLIB_POOL_* layout */

#include "nvidia/ga10b_chan.h"             /* ga10b_vm_*, ga10b_gpu_ops */
#include "nvidia/ga10b_channel_handoff.h"  /* qmd_pool_* fields */

#include "string.h"

#define PAGE_BYTES 4096u

/* ---- Cold backend ---- */

static void *cold_alloc(void *ctx, uint32_t npages, uint64_t *phys)
{
    struct oplib_cold_as_ctx *c = ctx;
    return c->ops->alloc_pages(c->ops->ctx, npages, phys);
}

static uint64_t cold_reserve(void *ctx, uint64_t bytes, uint64_t align)
{
    struct oplib_cold_as_ctx *c = ctx;
    return ga10b_vm_reserve(c->vm, bytes, align, false);
}

static int cold_map_at(void *ctx, uint64_t gpu_va, uint64_t phys, uint64_t bytes)
{
    struct oplib_cold_as_ctx *c = ctx;
    return ga10b_vm_map(c->ops, c->vm, gpu_va, phys, bytes, 0u) == GA10B_CHAN_OK
               ? OPLIB_AS_OK : OPLIB_AS_ERR_MAP;
}

static int cold_map(void *ctx, uint64_t phys, uint64_t bytes, uint64_t *gpu_va)
{
    uint64_t va = cold_reserve(ctx, bytes, PAGE_BYTES);
    if (va == 0) return OPLIB_AS_ERR_MAP;
    int rc = cold_map_at(ctx, va, phys, bytes);
    if (rc == OPLIB_AS_OK) *gpu_va = va;
    return rc;
}

static int cold_tlb_invalidate(void *ctx)
{
    struct oplib_cold_as_ctx *c = ctx;
    return ga10b_vm_tlb_invalidate(c->ops, c->vm) == GA10B_CHAN_OK
               ? OPLIB_AS_OK : OPLIB_AS_ERR_MAP;
}

static void cold_clean(void *ctx, const void *p, size_t len)
{
    struct oplib_cold_as_ctx *c = ctx;
    c->ops->cache_clean(c->ops->ctx, p, len);
}

void oplib_gpu_as_init_cold(struct oplib_gpu_as *as, struct oplib_cold_as_ctx *ctx,
                            const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm)
{
    ctx->ops = ops;
    ctx->vm = vm;
    *as = (struct oplib_gpu_as){
        .name = "cold-vm",
        .ctx = ctx,
        .alloc = cold_alloc,
        .free = NULL,           /* see header: no unmap after a failed map */
        .map = cold_map,
        .reserve = cold_reserve,
        .map_at = cold_map_at,
        .tlb_invalidate = cold_tlb_invalidate,
        .clean = cold_clean,
    };
}

/* ---- Staging helpers ---- */

static bool as_usable(const struct oplib_gpu_as *as)
{
    return as && as->alloc && as->map && as->clean;
}

/* Allocate + map `npages`; on map failure free only when the backend
 * says that is safe. */
static int alloc_map(const struct oplib_gpu_as *as, uint32_t npages,
                     void **cpu, uint64_t *phys, uint64_t *gpu_va,
                     bool zero)
{
    uint64_t pa = 0;
    void *p = as->alloc(as->ctx, npages, &pa);
    if (!p) return OPLIB_AS_ERR_NOMEM;
    if (zero) {
        memset(p, 0, (size_t)npages * PAGE_BYTES);
        as->clean(as->ctx, p, (size_t)npages * PAGE_BYTES);
    }
    uint64_t va = 0;
    int rc = as->map(as->ctx, pa, (uint64_t)npages * PAGE_BYTES, &va);
    if (rc != OPLIB_AS_OK || va == 0) {
        if (as->free) as->free(as->ctx, p, npages);
        return OPLIB_AS_ERR_MAP;
    }
    *cpu = p;
    *phys = pa;
    *gpu_va = va;
    return OPLIB_AS_OK;
}

int oplib_gpu_as_stage_pool(const struct oplib_gpu_as *as,
                            const uint8_t *sass, size_t sass_len,
                            struct oplib_staged_pool *out)
{
    if (!as_usable(as) || !out || (sass_len != 0 && !sass)) return OPLIB_AS_ERR_ARG;
    if (sass_len > OPLIB_POOL_SASS_MAX_BYTES) return OPLIB_AS_ERR_TOO_BIG;

    const uint64_t bytes = oplib_pool_self_bytes(sass_len);
    const uint32_t npages = (uint32_t)(bytes / PAGE_BYTES);
    uint64_t pa = 0;
    uint8_t *p = as->alloc(as->ctx, npages, &pa);
    if (!p) return OPLIB_AS_ERR_NOMEM;

    /* SASS + zero padding, cleaned before the GPU can see the VA. */
    memset(p, 0, (size_t)bytes);
    if (sass_len) memcpy(p + OPLIB_POOL_OFF_SASS, sass, sass_len);
    as->clean(as->ctx, p, (size_t)bytes);

    uint64_t va = 0;
    if (as->map(as->ctx, pa, bytes, &va) != OPLIB_AS_OK || va == 0) {
        if (as->free) as->free(as->ctx, p, npages);
        return OPLIB_AS_ERR_MAP;
    }
    out->pool_cpu = p;
    out->pool_phys = pa;
    out->pool_gpu_va = va;
    out->pool_bytes = bytes;
    out->scratch_off = oplib_pool_scratch_off(sass_len);
    return OPLIB_AS_OK;
}

int oplib_gpu_as_stage_cbuf(const struct oplib_gpu_as *as,
                            struct oplib_staged_pool *out)
{
    if (!as_usable(as) || !out) return OPLIB_AS_ERR_ARG;
    return alloc_map(as, 1u, &out->cbuf_cpu, &out->cbuf_phys, &out->cbuf_gpu_va, true);
}

int oplib_gpu_as_ensure_qmd_pool(const struct oplib_gpu_as *as,
                                 struct ga10b_channel_handoff *h)
{
    if (!as_usable(as) || !h) return OPLIB_AS_ERR_ARG;
    if (h->qmd_pool_phys != 0 && h->qmd_pool_gpu_va != 0 && h->qmd_pool_n_slots != 0) {
        return 0;
    }
    const uint32_t bytes = OPLIB_QMD_POOL_SLOTS * OPLIB_QMD_POOL_SLOT_BYTES;
    void *cpu = NULL;
    uint64_t pa = 0, va = 0;
    int rc = alloc_map(as, bytes / PAGE_BYTES, &cpu, &pa, &va, true);
    if (rc != OPLIB_AS_OK) return rc;
    h->qmd_pool_phys = pa;
    h->qmd_pool_gpu_va = va;
    h->qmd_pool_size_bytes = bytes;
    h->qmd_pool_n_slots = OPLIB_QMD_POOL_SLOTS;
    return 1;
}

/* ---- Weights pool ---- */

int oplib_wpool_build(const struct oplib_gpu_as *as, uint64_t request,
                      uint64_t chunk_bytes, struct oplib_wpool *out)
{
    if (!as_usable(as) || !out || request == 0) return OPLIB_AS_ERR_ARG;
    if (!as->reserve || !as->map_at) return OPLIB_AS_ERR_UNSUPP;
    if (chunk_bytes == 0) chunk_bytes = OPLIB_WPOOL_CHUNK_BYTES;
    if (chunk_bytes % PAGE_BYTES != 0 || chunk_bytes / PAGE_BYTES > UINT32_MAX) {
        return OPLIB_AS_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    uint64_t want = request / chunk_bytes + (request % chunk_bytes != 0);
    if (want > OPLIB_WPOOL_MAX_CHUNKS) want = OPLIB_WPOOL_MAX_CHUNKS;

    /* One VA range for the whole request; chunks the allocator cannot
     * provide leave its tail unmapped (and unused: size covers only the
     * mapped chunks). Chunk-aligned so a chunk never straddles a PDE0
     * boundary more than it must. */
    uint64_t align = PAGE_BYTES;
    while (align < chunk_bytes && align < (2ull << 20)) align <<= 1;
    uint64_t base = as->reserve(as->ctx, want * chunk_bytes, align);
    if (base == 0) return OPLIB_AS_ERR_MAP;

    const uint32_t npages = (uint32_t)(chunk_bytes / PAGE_BYTES);
    uint32_t n = 0;
    for (; n < want; n++) {
        uint64_t pa = 0;
        void *p = as->alloc(as->ctx, npages, &pa);
        if (!p) break;
        if (as->map_at(as->ctx, base + (uint64_t)n * chunk_bytes, pa, chunk_bytes)
                != OPLIB_AS_OK) {
            /* Possibly half-mapped: leak `p` unless the backend can
             * free safely, and stop growing. */
            if (as->free) as->free(as->ctx, p, npages);
            if (n == 0) return OPLIB_AS_ERR_MAP;
            break;
        }
        out->chunk[n].cpu = p;
        out->chunk[n].phys = pa;
    }
    if (n == 0) return OPLIB_AS_ERR_NOMEM;
    out->gpu_va = base;
    out->chunk_bytes = chunk_bytes;
    out->n_chunks = n;
    out->size = (uint64_t)n * chunk_bytes;
    return OPLIB_AS_OK;
}

void *oplib_wpool_translate(const struct oplib_wpool *w, uint64_t gpu_va,
                            uint64_t *run)
{
    if (!w || w->n_chunks == 0 || gpu_va < w->gpu_va) return NULL;
    uint64_t off = gpu_va - w->gpu_va;
    if (off >= w->size) return NULL;
    uint64_t idx = off / w->chunk_bytes;
    uint64_t in = off % w->chunk_bytes;
    if (run) *run = w->chunk_bytes - in;
    return (uint8_t *)w->chunk[idx].cpu + in;
}

int oplib_wpool_write(const struct oplib_wpool *w, const struct oplib_gpu_as *as,
                      uint64_t gpu_va, const void *src, size_t len)
{
    if (!w || !as || !as->clean || !src || len == 0) return OPLIB_AS_ERR_ARG;
    if (gpu_va < w->gpu_va) return OPLIB_AS_ERR_ARG;
    uint64_t off = gpu_va - w->gpu_va;
    if (off > w->size || (uint64_t)len > w->size - off) return OPLIB_AS_ERR_ARG;

    const uint8_t *s = src;
    size_t done = 0;
    while (done < len) {
        uint64_t run = 0;
        uint8_t *dst = oplib_wpool_translate(w, gpu_va + done, &run);
        if (!dst) return OPLIB_AS_ERR_ARG;   /* unreachable after the range check */
        size_t n = len - done;
        if ((uint64_t)n > run) n = (size_t)run;
        memcpy(dst, s + done, n);
        as->clean(as->ctx, dst, n);
        done += n;
    }
    return OPLIB_AS_OK;
}
