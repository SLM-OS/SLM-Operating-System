/*
 * oplib_pool.c - In-kernel handle for the embedded operator-library blob (#714).
 *
 * Caches a parsed `operator_library` handle over the .incbin'ed blob
 * so dispatchers (B follow-on; #714) can look up SASS kernels by
 * (op_kind, tier, dtype) without re-validating headers on every
 * call.
 *
 * It also owns the GPU-VA staging of the SASS pool + cbuf (#718,
 * #832). Staging goes through a struct oplib_gpu_as (#1092): the
 * kexec backend below (Linux's page tables via ga10b_gmmu.c) or the
 * cold-channel backend from oplib_cold.c (SLM-OS's ga10b_vm).
 */

#include "oplib_pool.h"

#include "operator_library.h"
#include "oplib_gpu_as.h"
#include "string.h"
#include "uart.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../include/cache.h"
#include "oplib_cold.h"
#include "../include/pmm.h"  /* pmm_alloc_pages / pmm_free_pages */
#include "nvidia/ga10b_gmmu.h"
#include "nvidia/ga10b_bringup.h"          /* ga10b_bringup_handoff */
#include "nvidia/ga10b_channel_handoff.h"  /* struct ga10b_channel_handoff */
#endif

/* Linker-supplied symbols from kernel/src/oplib_embed.S. */
extern const uint8_t oplib_blob_start[];
extern const uint8_t oplib_blob_end[];

/* Cached parser handle. Zero-initialized = uninitialized; `g_init_rc`
 * captures the open() result so `oplib_pool_lookup` can return the
 * right error class without re-running the parser. */
static struct operator_library g_handle;
static int g_init_rc = OPERATOR_LIBRARY_ERR_NULL;
static bool g_initialized;

/* GPU-VA staging state (#718, A.1.5). `g_staged` flips to true after
 * the first successful `oplib_pool_stage_to_gpu()` call; subsequent
 * calls are idempotent and return `g_stage_rc`. The stub blob
 * (sass_region_len == 0) staging path is also recorded as "staged"
 * with `g_sass_pool_gpu_va == 0` so callers see a stable,
 * NOT_AVAILABLE-returning lookup contract.
 *
 * Atomic (sequentially consistent) because a post-boot re-stage
 * (`nvgpu oplib stage`, `nvgpu cold` retry) publishes the pool from the
 * shell while the W-series FFIs read it under the GPU dispatch lock on
 * other CPUs: every stage path writes the pool fields first and sets
 * g_staged last, and every reader tests g_staged first, so a reader
 * never pairs g_staged with half-written fields. A retry clears it
 * before touching them. */
static _Atomic bool g_staged;
static int g_stage_rc = OPERATOR_LIBRARY_ERR_NULL;
static uint64_t g_sass_pool_gpu_va;
static size_t g_sass_pool_n_pages;

/* #832: physical base of the pool — i.e. the CPU phys at slot 0
 * (SASS). Required by the W-series dispatch FFI for per-call
 * scratch-slot CPU↔GPU memcpys when the helper handoff doesn't
 * expose `h->shader_phys` (post-v6/v7 publish paths zero those
 * fields). Set in both fast path (from `h->shader_phys`) and slow
 * path (from `pmm_alloc_pages` return). Stays 0 if neither path
 * yields a contiguous-phys-mappable buffer. */
static uint64_t g_sass_pool_base_phys;

/* Offset of the scratch base (see oplib_pool_scratch_off) from the
 * pool base: 0 in the helper layout, more when a self-allocated pool
 * holds SASS larger than one slot (#1092). */
static uint64_t g_scratch_off;

/* #832: cbuf is a separate 4 KB page in the channel's GMMU,
 * populated per-dispatch by `slm_oplib_prepare_dispatch` with the
 * shader's cbuf[0] argument layout. Set by `oplib_pool_stage_to_gpu`
 * — fast path takes `h->cbuf_*`, slow path allocates a fresh PMM
 * page and maps it. Same v6/v7-publish-zeros story as shader_*; the
 * helper-published `h->cbuf_*` is the original design but isn't
 * populated by the current MNIST helper. */
static uint64_t g_cbuf_phys;
static uint64_t g_cbuf_gpu_va;

size_t oplib_pool_blob_size(void)
{
    return (size_t)(oplib_blob_end - oplib_blob_start);
}

int oplib_pool_init(void)
{
    if (g_initialized) {
        return g_init_rc;
    }

    size_t len = oplib_pool_blob_size();
    g_init_rc = operator_library_open(&g_handle, oplib_blob_start, len);
    g_initialized = true;

    if (g_init_rc == 0) {
        uart_printf("[oplib] embedded blob: %zu B, op_count=%u\n",
                    len, (unsigned)g_handle.op_count);
    } else {
        uart_printf("[oplib] embedded blob parse FAILED: rc=%d "
                    "(blob_size=%zu)\n", g_init_rc, len);
    }
    return g_init_rc;
}

int oplib_pool_lookup(uint32_t op_kind, uint32_t tier, uint32_t dtype,
                      const uint8_t **out_sass, size_t *out_size)
{
    if (!g_initialized || g_init_rc != 0) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    return operator_library_lookup(&g_handle, op_kind, tier, dtype,
                                   out_sass, out_size);
}

/* `nvgpu oplib status` helper. Output is intentionally compact —
 * one summary line plus one line per entry. */
void oplib_pool_status_print(void)
{
    size_t len = oplib_pool_blob_size();

    if (!g_initialized) {
        uart_printf("[oplib] not initialized (oplib_pool_init not called)\n");
        return;
    }
    if (g_init_rc != 0) {
        uart_printf("[oplib] init failed: rc=%d, blob_size=%zu\n",
                    g_init_rc, len);
        return;
    }

    uart_printf("[oplib] blob: start=%p end=%p size=%zu\n",
                (const void *)oplib_blob_start,
                (const void *)oplib_blob_end, len);
    uart_printf("[oplib] op_count=%u, sass_region_len=%zu\n",
                (unsigned)g_handle.op_count, g_handle.sass_region_len);

    /* Walk the entries array — same 32-byte layout
     * `operator_library.h` documents. */
    for (uint32_t i = 0; i < g_handle.op_count; i++) {
        const uint8_t *e = g_handle.entries + (size_t)i *
                           OPERATOR_LIBRARY_ENTRY_LEN;
        uint32_t op_kind = (uint32_t)e[0]  | ((uint32_t)e[1]  <<  8) |
                           ((uint32_t)e[2]  << 16) | ((uint32_t)e[3]  << 24);
        uint32_t tier    = (uint32_t)e[4]  | ((uint32_t)e[5]  <<  8) |
                           ((uint32_t)e[6]  << 16) | ((uint32_t)e[7]  << 24);
        uint32_t dtype   = (uint32_t)e[8]  | ((uint32_t)e[9]  <<  8) |
                           ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint64_t off     = (uint64_t)e[16] | ((uint64_t)e[17] <<  8) |
                           ((uint64_t)e[18] << 16) | ((uint64_t)e[19] << 24) |
                           ((uint64_t)e[20] << 32) | ((uint64_t)e[21] << 40) |
                           ((uint64_t)e[22] << 48) | ((uint64_t)e[23] << 56);
        uint64_t sz      = (uint64_t)e[24] | ((uint64_t)e[25] <<  8) |
                           ((uint64_t)e[26] << 16) | ((uint64_t)e[27] << 24) |
                           ((uint64_t)e[28] << 32) | ((uint64_t)e[29] << 40) |
                           ((uint64_t)e[30] << 48) | ((uint64_t)e[31] << 56);
        uart_printf("[oplib]   [%u] op_kind=%u tier=%u dtype=%u "
                    "sass_offset=0x%llx size=%llu\n",
                    (unsigned)i, (unsigned)op_kind, (unsigned)tier,
                    (unsigned)dtype, (unsigned long long)off,
                    (unsigned long long)sz);
    }

    if (!g_staged) {
        uart_printf("[oplib] GPU staging: not staged "
                    "(call oplib_pool_stage_to_gpu)\n");
    } else if (g_stage_rc != 0) {
        uart_printf("[oplib] GPU staging: FAILED (rc=%d)\n", g_stage_rc);
    } else if (g_sass_pool_gpu_va == 0) {
        uart_printf("[oplib] GPU staging: stub (sass_region_len=0, "
                    "no allocation needed)\n");
    } else {
        uart_printf("[oplib] GPU staging: pool gpu_va=0x%llx, %zu pages "
                    "(%zu B SASS region)\n",
                    (unsigned long long)g_sass_pool_gpu_va,
                    g_sass_pool_n_pages, g_handle.sass_region_len);
    }
}

/* ============================================================================
 * GPU-VA staging (#718, A.1.5)
 * ============================================================================ */

uint64_t oplib_pool_gpu_va_base(void)
{
    return g_sass_pool_gpu_va;
}

uint64_t oplib_pool_base_phys(void)
{
    return g_sass_pool_base_phys;
}

uint64_t oplib_pool_cbuf_phys(void)
{
    return g_cbuf_phys;
}

uint64_t oplib_pool_cbuf_gpu_va(void)
{
    return g_cbuf_gpu_va;
}

/* ---- Backend-independent staging (#1092) ----
 *
 * Stage the SASS pool + cbuf through `as` and publish the result in
 * the g_* accessors. Caller has checked/reset g_staged. Records the
 * outcome in g_staged / g_stage_rc. */
static int stage_common(const struct oplib_gpu_as *as)
{
    if (!g_initialized || g_init_rc != 0) {
        g_staged = true;
        g_stage_rc = OPERATOR_LIBRARY_ERR_NULL;
        return g_stage_rc;
    }
    /* Stub blob (op_count=0, sass_region_len=0): nothing to map. Mark
     * staged so subsequent lookups return NOT_AVAILABLE cleanly via
     * g_sass_pool_gpu_va == 0. */
    if (g_handle.sass_region_len == 0) {
        g_staged = true;
        g_stage_rc = 0;
        uart_printf("[oplib] stage_to_gpu: stub blob, no SASS region "
                    "(op_count=%u)\n", (unsigned)g_handle.op_count);
        return 0;
    }

    /* The W-series dispatch FFIs compute their per-call scratch slots
     * as `pool_base + OPLIB_POOL_OFF_SCRATCH<N>`, so the pool is one
     * contiguous physical buffer (SASS in slot 0, 64 KB). */
    size_t sass_len = g_handle.sass_region_len;
    struct oplib_staged_pool sp;
    memset(&sp, 0, sizeof(sp));
    int rc = oplib_gpu_as_stage_pool(as, g_handle.sass_region, sass_len, &sp);
    if (rc != OPLIB_AS_OK) {
        g_staged = true;
        g_stage_rc = -1;
        if (rc == OPLIB_AS_ERR_TOO_BIG) {
            uart_printf("[oplib] stage_to_gpu: SASS region %zu B exceeds "
                        "the %u B self-allocated pool limit "
                        "(OPLIB_POOL_SASS_MAX_BYTES)\n",
                        sass_len, (unsigned)OPLIB_POOL_SASS_MAX_BYTES);
        } else {
            uart_printf("[oplib] stage_to_gpu: %s: %llu KB pool alloc/map "
                        "failed rc=%d\n", as->name,
                        (unsigned long long)(oplib_pool_self_bytes(sass_len) >> 10), rc);
        }
        return -1;
    }

    /* Cbuf gets its own 4 KB page (helper-published h->cbuf_* are used
     * instead when present; kexec fast path only). A cbuf failure makes
     * the W-series FFIs fall back to CPU but leaves the pool usable. */
    if (g_cbuf_phys == 0 || g_cbuf_gpu_va == 0) {
        if (oplib_gpu_as_stage_cbuf(as, &sp) == OPLIB_AS_OK) {
            g_cbuf_phys   = sp.cbuf_phys;
            g_cbuf_gpu_va = sp.cbuf_gpu_va;
        } else {
            uart_printf("[oplib] stage_cbuf: %s: cbuf alloc/map failed — "
                        "W-series dispatches will fall back to CPU\n",
                        as->name);
        }
    }

    g_sass_pool_gpu_va    = sp.pool_gpu_va + OPLIB_POOL_OFF_SASS;
    g_sass_pool_n_pages   = (sass_len + 4095) / 4096;
    g_sass_pool_base_phys = sp.pool_phys;
    g_scratch_off         = sp.scratch_off;
    g_stage_rc = 0;
    g_staged   = true;

    /* An address space the GPU may already have walked (the cold
     * channel's, on a `nvgpu oplib stage` retry after boot) must drop
     * stale translations before the new pool is used. The kexec mapper
     * has no invalidate (NULL). */
    if (as->tlb_invalidate != NULL && as->tlb_invalidate(as->ctx) != OPLIB_AS_OK) {
        uart_printf("[oplib] stage_to_gpu: %s: TLB invalidate timed out — "
                    "the pool may not be visible to the GPU yet\n", as->name);
    }

    uart_printf("[oplib] stage_to_gpu: %zu B SASS staged at gpu_va=0x%llx "
                "(%s, self-allocated %llu KB pool, base phys=0x%llx, "
                "cbuf gpu_va=0x%llx)\n",
                sass_len, (unsigned long long)g_sass_pool_gpu_va, as->name,
                (unsigned long long)(sp.pool_bytes >> 10),
                (unsigned long long)sp.pool_phys,
                (unsigned long long)g_cbuf_gpu_va);
    return 0;
}

int oplib_pool_stage_with_as(const struct oplib_gpu_as *as)
{
    if (as == NULL) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    if (g_staged && g_stage_rc == 0) {
        return 0;
    }
    /* A previous attempt failed (e.g. `nvgpu oplib stage` run before
     * the cold channel existed): retry from scratch. */
    g_staged = false;
    g_stage_rc = OPERATOR_LIBRARY_ERR_NULL;
    g_sass_pool_gpu_va = 0;
    g_sass_pool_n_pages = 0;
    g_sass_pool_base_phys = 0;
    g_scratch_off = 0;
    return stage_common(as);
}

void oplib_pool_unstage(void)
{
    g_staged = false;
    g_stage_rc = OPERATOR_LIBRARY_ERR_NULL;
    g_sass_pool_gpu_va = 0;
    g_sass_pool_n_pages = 0;
    g_sass_pool_base_phys = 0;
    g_scratch_off = 0;
    g_cbuf_phys = 0;
    g_cbuf_gpu_va = 0;
}

size_t oplib_pool_sass_region_len(void)
{
    return (g_initialized && g_init_rc == 0) ? g_handle.sass_region_len : 0;
}

int oplib_pool_scratch_base(uint64_t *out_phys, uint64_t *out_gpu_va)
{
    if (out_phys == NULL || out_gpu_va == NULL) {
        return -1;
    }
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Helper-published v4 shader region (kexec, old helpers). Never
     * present on a locally built handoff. */
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h != NULL && h->shader_phys != 0 && h->shader_gpu_va != 0 &&
        h->shader_size >= OPLIB_POOL_MIN_BYTES) {
        *out_phys   = h->shader_phys;
        *out_gpu_va = h->shader_gpu_va;
        return 0;
    }
#endif
    if (g_staged && g_stage_rc == 0 && g_sass_pool_base_phys != 0 &&
        g_sass_pool_gpu_va != 0) {
        *out_phys   = g_sass_pool_base_phys + g_scratch_off;
        *out_gpu_va = g_sass_pool_gpu_va - OPLIB_POOL_OFF_SASS + g_scratch_off;
        return 0;
    }
    return -1;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* ---- kexec backend: Linux's page tables through ga10b_gmmu.c ----
 *
 * Only for a channel Linux's helper created. ga10b_gmmu_map writes
 * 8-byte-stride dual PDE0 entries (#1090), so it must never touch a
 * VM laid out by ga10b_vm_* (the cold channel; see oplib_cold.c). */
static void *kexec_as_alloc(void *ctx, uint32_t npages, uint64_t *phys)
{
    (void)ctx;
    void *p = pmm_alloc_pages(npages);
    if (p != NULL) {
        *phys = (uint64_t)(uintptr_t)p;   /* identity map */
    }
    return p;
}

static void kexec_as_free(void *ctx, void *cpu, uint32_t npages)
{
    (void)ctx;
    pmm_free_pages(cpu, npages);
}

static int kexec_as_map(void *ctx, uint64_t phys, uint64_t bytes,
                        uint64_t *gpu_va)
{
    uint64_t inst = *(const uint64_t *)ctx;
    int rc = ga10b_gmmu_map(inst, phys, (uint32_t)(bytes / 4096u), 0u, gpu_va);
    if (rc < 0) {
        uart_printf("[oplib] ga10b_gmmu_map(inst=0x%llx, phys=0x%llx, "
                    "%llu B) failed rc=%d\n", (unsigned long long)inst,
                    (unsigned long long)phys, (unsigned long long)bytes, rc);
        return OPLIB_AS_ERR_MAP;
    }
    return OPLIB_AS_OK;
}

static void kexec_as_clean(void *ctx, const void *p, size_t len)
{
    (void)ctx;
    cache_clean_range(p, len);
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ga10b_gmmu_map orders its own PTE writes (dsb sy) and the inherited
 * channel must not see a dispatch-time all-VA invalidate (#844), so
 * no tlb_invalidate / reserve / map_at. The pages are freed after a
 * failed map, as the pre-#1092 slow path did. */
static void kexec_as_init(struct oplib_gpu_as *as, uint64_t *inst)
{
    *as = (struct oplib_gpu_as){
        .name = "kexec-gmmu",
        .ctx = inst,
        .alloc = kexec_as_alloc,
        .free = kexec_as_free,
        .map = kexec_as_map,
        .clean = kexec_as_clean,
    };
}

int oplib_pool_stage_to_gpu(uint64_t inst_block_phys)
{
    /* UEFI-direct boot: the channel is SLM-OS's own; stage into its VM
     * (inst_block_phys is ignored — the cold channel is not reachable
     * through ga10b_gmmu.c). */
    if (oplib_cold_active()) {
        return oplib_pool_stage_with_as(oplib_cold_as());
    }
    if (g_staged) {
        return g_stage_rc;
    }
    if (ga10b_bringup_handoff_is_local()) {
        /* A local handoff without oplib_cold_attach: refusing beats
         * writing Linux-format PDEs into SLM-OS's page tables. */
        g_staged = true;
        g_stage_rc = -1;
        uart_puts("[oplib] stage_to_gpu: active channel was built by "
                  "SLM-OS but the operator library is not attached to "
                  "it (oplib_cold_attach) — not staging\n");
        return -1;
    }
    if (!g_initialized || g_init_rc != 0 || g_handle.sass_region_len == 0) {
        return stage_common(NULL);   /* records the init/stub outcome */
    }

    uint64_t inst = inst_block_phys;
    struct oplib_gpu_as kx;
    kexec_as_init(&kx, &inst);
    size_t sass_len = g_handle.sass_region_len;

    /* Fast path: if the inherited channel handoff exposes a pre-
     * staged SASS region (helper allocated `shader_*` fields in
     * the channel's GMMU pre-kexec), copy the embedded SASS bytes
     * into it and use the helper's GPU VA directly. Skips the
     * post-kexec ga10b_gmmu_alloc path entirely — that path
     * requires discovering the channel's inst_block_phys, which
     * is unreliable on a kexec'd Linux box (FECS_CURRENT_CTX
     * stale, walk-discovery blocked on big-page support). */
    {
        const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
        /* Capacity check uses the page-rounded length so the last
         * 4 KB page is fully inside the helper's mapping. */
        size_t sass_pages_bytes = ((sass_len + 4095u) / 4096u) * 4096u;
        /* Only when the SASS fits slot 0: the helper's slots 1-3 are
         * the scratch buffers, and a larger library would overwrite
         * them. Larger libraries take the self-allocated pool below. */
        if (h != NULL && h->shader_gpu_va != 0 &&
            h->shader_phys != 0 && h->shader_size >= sass_pages_bytes &&
            sass_len <= OPLIB_POOL_SLOT_BYTES) {
            /* SASS occupies slot 0 of the helper-staged pool. */
            uint64_t sass_phys = h->shader_phys + OPLIB_POOL_OFF_SASS;
            uint64_t sass_gva  = h->shader_gpu_va + OPLIB_POOL_OFF_SASS;
            volatile uint8_t *dst =
                (volatile uint8_t *)(uintptr_t)sass_phys;
            const uint8_t *src = g_handle.sass_region;
            for (size_t i = 0; i < sass_len; i++) {
                dst[i] = src[i];
            }
            cache_clean_range((void *)(uintptr_t)sass_phys, sass_len);
            __asm__ volatile("dsb sy" ::: "memory");
            g_sass_pool_gpu_va = sass_gva;
            g_sass_pool_n_pages = (sass_len + 4095) / 4096;
            /* #832: pool base for the W-series scratch-slot math. */
            g_sass_pool_base_phys = h->shader_phys;
            g_scratch_off = 0;
            if (h->cbuf_phys != 0 && h->cbuf_gpu_va != 0) {
                g_cbuf_phys   = h->cbuf_phys;
                g_cbuf_gpu_va = h->cbuf_gpu_va;
            } else {
                /* v6/v7 publish paths zero cbuf_*: own 4 KB page. */
                struct oplib_staged_pool sp;
                memset(&sp, 0, sizeof(sp));
                if (oplib_gpu_as_stage_cbuf(&kx, &sp) == OPLIB_AS_OK) {
                    g_cbuf_phys   = sp.cbuf_phys;
                    g_cbuf_gpu_va = sp.cbuf_gpu_va;
                    uart_printf("[oplib] stage_cbuf: allocated 4 KB cbuf at "
                                "gpu_va=0x%llx (phys=0x%llx)\n",
                                (unsigned long long)sp.cbuf_gpu_va,
                                (unsigned long long)sp.cbuf_phys);
                } else {
                    uart_puts("[oplib] stage_cbuf: cbuf alloc/map failed — "
                              "W-series dispatches will fall back to CPU\n");
                }
            }
            g_stage_rc = 0;
            g_staged = true;
            uart_printf("[oplib] stage_to_gpu: %zu B copied into helper-"
                        "staged SASS region at gpu_va=0x%llx (phys=0x%llx, "
                        "capacity %u B)\n",
                        sass_len,
                        (unsigned long long)sass_gva,
                        (unsigned long long)sass_phys,
                        (unsigned)h->shader_size);
            return 0;
        }
    }

    /* Slow path (#832): the helper's v6/v7 publish path zeros
     * `h->shader_*`. Allocate the full pool (SASS + 3 scratch slots)
     * as one contiguous PMM buffer and map it into the inherited
     * channel's GMMU through the kexec address space. */
    return stage_common(&kx);
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int oplib_pool_stage_to_gpu(uint64_t inst_block_phys)
{
    (void)inst_block_phys;
    /* Non-Jetson platforms have no GA10B GMMU. Staging is a no-op
     * stub; `oplib_pool_get_sass_gpu_va` will always return
     * NOT_AVAILABLE because g_sass_pool_gpu_va stays 0. */
    g_staged = true;
    g_stage_rc = OPERATOR_LIBRARY_ERR_NULL;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */

int oplib_pool_get_sass_gpu_va(uint32_t op_kind, uint32_t tier, uint32_t dtype,
                                uint64_t *out_gpu_va, size_t *out_size)
{
    if (out_gpu_va == NULL || out_size == NULL) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    if (!g_initialized || g_init_rc != 0) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    if (!g_staged || g_stage_rc != 0 || g_sass_pool_gpu_va == 0) {
        return OPERATOR_LIBRARY_ERR_NULL;
    }
    const uint8_t *sass = NULL;
    size_t size = 0;
    int rc = operator_library_lookup(&g_handle, op_kind, tier, dtype,
                                     &sass, &size);
    if (rc != 0) {
        return rc;
    }
    /* Defensive bound check: the parser guarantees `sass` lies inside
     * `[sass_region, sass_region + sass_region_len)` and `size`
     * doesn't run past the end. But a corrupted blob (e.g. one that
     * passed the outer-header checksum but had an entry table tampered
     * with after parsing) could produce out-of-range pointers that
     * yield wild GPU VAs. Re-check here so the dispatcher never sees
     * a VA outside the staged pool. */
    if (sass < g_handle.sass_region ||
        size > g_handle.sass_region_len ||
        (size_t)(sass - g_handle.sass_region) >
            g_handle.sass_region_len - size) {
        return OPERATOR_LIBRARY_ERR_LAYOUT;
    }
    /* Translate CPU-side pointer into the embedded blob's SASS region
     * to GPU VA inside the staged pool. */
    size_t offset = (size_t)(sass - g_handle.sass_region);
    *out_gpu_va = g_sass_pool_gpu_va + offset;
    *out_size   = size;
    return 0;
}
