/*
 * oplib_pool.c - In-kernel handle for the embedded operator-library blob (#714).
 *
 * Caches a parsed `operator_library` handle over the .incbin'ed blob
 * so dispatchers (B follow-on; #714) can look up SASS kernels by
 * (op_kind, tier, dtype) without re-validating headers on every
 * call.
 *
 * GPU-VA staging is intentionally out of scope here — that's the
 * dispatcher's job and lands once #666 GMMU work merges to main.
 * This file is just the parser-handle owner.
 */

#include "oplib_pool.h"

#include "operator_library.h"
#include "uart.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../include/cache.h"
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
 * NOT_AVAILABLE-returning lookup contract. */
static bool g_staged;
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

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* #832: ensure a cbuf page is mapped into the channel's GMMU and
 * recorded in g_cbuf_phys / g_cbuf_gpu_va. Called by both fast and
 * slow paths of `oplib_pool_stage_to_gpu` after SASS staging — the
 * helper may have published cbuf_* (h->cbuf_*) but the current
 * MNIST helper's v6/v7 paths zero those fields, so we fall back to
 * a SLM-OS-allocated 4 KB page.
 *
 * Idempotent: if g_cbuf_phys is already non-zero (set earlier by
 * the fast path's helper-cbuf check), returns 0 without allocating.
 *
 * Returns 0 on success or a negative error. On error, the caller
 * still treats SASS staging as successful — cbuf failure makes the
 * W-series dispatch FFI return -1 (CPU fallback) but doesn't
 * invalidate the SASS pool itself. */
static int stage_cbuf(uint64_t inst_block_phys)
{
    if (g_cbuf_phys != 0 && g_cbuf_gpu_va != 0) {
        return 0;  /* helper-published path; nothing to do */
    }

    void *cbuf_cpu = pmm_alloc_pages(1u);
    if (cbuf_cpu == NULL) {
        uart_puts("[oplib] stage_cbuf: pmm_alloc_pages(1) failed — "
                  "W-series dispatches will fall back to CPU\n");
        return -1;
    }
    uint64_t cbuf_phys = (uint64_t)(uintptr_t)cbuf_cpu;

    uint64_t cbuf_gva = 0;
    int rc = ga10b_gmmu_map(inst_block_phys, cbuf_phys, 1u, 0u, &cbuf_gva);
    if (rc < 0) {
        pmm_free_pages(cbuf_cpu, 1u);
        uart_printf("[oplib] stage_cbuf: ga10b_gmmu_map(phys=0x%llx) "
                    "failed rc=%d\n",
                    (unsigned long long)cbuf_phys, rc);
        return rc;
    }

    /* Zero the cbuf — each dispatch writes a fresh layout, but
     * keeping uninitialized bytes deterministic helps diagnostic
     * dumps. */
    {
        volatile uint8_t *p = (volatile uint8_t *)cbuf_cpu;
        for (size_t i = 0; i < 4096; i++) {
            p[i] = 0;
        }
        cache_clean_range(cbuf_cpu, 4096);
        __asm__ volatile("dsb sy" ::: "memory");
    }

    g_cbuf_phys   = cbuf_phys;
    g_cbuf_gpu_va = cbuf_gva;
    uart_printf("[oplib] stage_cbuf: allocated 4 KB cbuf at "
                "gpu_va=0x%llx (phys=0x%llx)\n",
                (unsigned long long)cbuf_gva,
                (unsigned long long)cbuf_phys);
    return 0;
}

int oplib_pool_stage_to_gpu(uint64_t inst_block_phys)
{
    if (g_staged) {
        return g_stage_rc;
    }
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
        /* Capacity check uses the page-rounded length (matches
         * what the slow path's ga10b_gmmu_alloc(n_pages) would
         * map). A handoff with shader_size between sass_len and
         * round_up(sass_len, 4 KB) would otherwise pass the raw
         * sass_len check but leave the last 4 KB page partially
         * outside the helper's mapping, which the
         * `g_sass_pool_n_pages` claim below would lie about. The
         * helper today always allocates a multiple of 4 KB so the
         * extra strictness is a no-op in practice; this guards
         * against any future producer that hands SLM-OS an
         * unaligned shader_size. */
        size_t sass_pages_bytes = ((sass_len + 4095u) / 4096u) * 4096u;
        if (h != NULL && h->shader_gpu_va != 0 &&
            h->shader_phys != 0 && h->shader_size >= sass_pages_bytes) {
            /* The SASS bytes occupy slot 0 of the helper-staged
             * pool (see `OPLIB_POOL_OFF_SASS` in oplib_pool.h). The
             * `+ OPLIB_POOL_OFF_SASS` is a no-op today because the
             * offset is 0, but adding it explicitly keeps the slot
             * mapping symmetric with the SCRATCH slots that the
             * dispatch verbs in shell_sys.c carve out. */
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
            /* #832: record pool base so W-series dispatch FFIs can
             * compute scratch slot phys without re-reading
             * `h->shader_phys`. SASS is slot 0 (OPLIB_POOL_OFF_SASS == 0),
             * so the pool base IS the SASS base. */
            g_sass_pool_base_phys = h->shader_phys;
            /* If the helper also published cbuf_*, record it.
             * Otherwise fall through to the cbuf-allocation step
             * below — the v6/v7 publish paths zero cbuf_* the same
             * way they zero shader_*. */
            if (h->cbuf_phys != 0 && h->cbuf_gpu_va != 0) {
                g_cbuf_phys   = h->cbuf_phys;
                g_cbuf_gpu_va = h->cbuf_gpu_va;
            }
            /* Ensure cbuf is mapped. Returns 0 if helper published
             * h->cbuf_*, otherwise allocates a fresh 4 KB page. */
            (void)stage_cbuf(inst_block_phys);
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
     * `h->shader_*`, so there's no pre-staged scratch region for
     * the W-series dispatch FFI to use. Allocate the FULL pool
     * (SASS + 3 scratch slots = OPLIB_POOL_MIN_BYTES) as one
     * contiguous physical buffer via PMM and map it into the
     * inherited channel's GMMU via the new `ga10b_gmmu_map`.
     *
     * Contiguous-phys is load-bearing: each dispatch FFI does
     * `pool_base_phys + OPLIB_POOL_OFF_SCRATCH<N>` to find its
     * per-call CPU↔GPU staging slot. The prior slow path that used
     * `ga10b_gmmu_alloc` got per-page-discontig PMM and couldn't
     * satisfy that math.
     *
     * SASS must fit in slot 0 (64 KB). All seven W-series shipped
     * kernels combine to ~40 KB today — fits comfortably. If a
     * future operator library exceeds this, the assertion below
     * fires before any GMMU mapping is attempted. */
    if (sass_len > OPLIB_POOL_SLOT_BYTES) {
        g_staged = true;
        g_stage_rc = -1;
        uart_printf("[oplib] stage_to_gpu: SASS region %zu B exceeds "
                    "slot 0 capacity %u B — pool layout assumes "
                    "SASS fits in slot 0. Rework slot sizing or "
                    "split the library.\n",
                    sass_len, (unsigned)OPLIB_POOL_SLOT_BYTES);
        return -1;
    }

    /* OPLIB_POOL_MIN_BYTES = 256 KB rounds to 64 pages → order 6
     * in the buddy allocator. Plenty of headroom on Jetson's
     * ~6.7 GB usable. */
    const size_t pool_pages = OPLIB_POOL_MIN_BYTES / 4096u;
    void *pool_cpu = pmm_alloc_pages(pool_pages);
    if (pool_cpu == NULL) {
        g_staged = true;
        g_stage_rc = -1;
        uart_printf("[oplib] stage_to_gpu: pmm_alloc_pages(%zu) for "
                    "256 KB scratch pool failed\n", pool_pages);
        return -1;
    }
    uint64_t pool_phys = (uint64_t)(uintptr_t)pool_cpu;

    /* Map the whole pool into the channel's GMMU at a fresh GPU VA.
     * Read-write because per-call staging writes activations into
     * SCRATCH0/1/2; the SASS slot is also writable today (no
     * separate RO flag per slot — fix once we add slot-granular
     * mapping). */
    uint64_t gpu_va = 0;
    int rc = ga10b_gmmu_map(inst_block_phys, pool_phys,
                             (uint32_t)pool_pages,
                             /* flags */ 0,
                             &gpu_va);
    if (rc < 0) {
        /* Free the PMM pages — they'd otherwise leak until reboot. */
        pmm_free_pages(pool_cpu, pool_pages);
        g_staged = true;
        g_stage_rc = rc;
        uart_printf("[oplib] stage_to_gpu: ga10b_gmmu_map(phys=0x%llx, "
                    "n_pages=%zu) failed: rc=%d\n",
                    (unsigned long long)pool_phys, pool_pages, rc);
        return rc;
    }

    /* Copy SASS into slot 0. The pool is physically contiguous so
     * we can write through the kernel-VA alias (`pool_cpu`) in one
     * pass, and a single cache_clean_range covers slot 0. */
    {
        const uint8_t *src = g_handle.sass_region;
        volatile uint8_t *dst =
            (volatile uint8_t *)pool_cpu + OPLIB_POOL_OFF_SASS;
        for (size_t i = 0; i < sass_len; i++) {
            dst[i] = src[i];
        }
        cache_clean_range((void *)((uintptr_t)pool_cpu + OPLIB_POOL_OFF_SASS),
                          sass_len);
    }

    /* DSB SY so the GPU sees the populated SASS pages from PoC on
     * its first dispatch. ga10b_gmmu_map already issued one for the
     * page-table publication; this one orders the SASS data writes. */
    __asm__ volatile("dsb sy" ::: "memory");

    g_sass_pool_gpu_va    = gpu_va + OPLIB_POOL_OFF_SASS;
    g_sass_pool_n_pages   = (sass_len + 4095) / 4096;
    g_sass_pool_base_phys = pool_phys;

    /* Cbuf gets its own 4 KB page, separately allocated and mapped.
     * Same v6/v7-publish-zeros story as shader_*. */
    (void)stage_cbuf(inst_block_phys);

    g_stage_rc = 0;
    g_staged   = true;

    uart_printf("[oplib] stage_to_gpu: %zu B SASS staged at gpu_va=0x%llx "
                "(self-allocated 256 KB pool, base phys=0x%llx, "
                "scratch slots mapped)\n",
                sass_len, (unsigned long long)g_sass_pool_gpu_va,
                (unsigned long long)pool_phys);
    return 0;
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
