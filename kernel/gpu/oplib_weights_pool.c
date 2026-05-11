/*
 * oplib_weights_pool.c — bump allocator + GA10B Copy Engine staging
 * for the W1-published weights pool. See header for design notes
 * and docs/design/gpu-weights-pool.md for the architecture trail.
 *
 * Staging mechanism: CPU writes the source bytes into a helper-staged
 * bounce buffer (the SASS pool's SCRATCH0 slot — contiguous, known
 * phys, identity-mapped on Jetson), then asks the GPU's Copy Engine
 * to memcpy bounce_gpu_va → pool_slot_gpu_va over the channel's
 * inherited GMMU mapping. This sidesteps the post-kexec GMMU-walk-
 * discovery failure described in PR #769: SLM-OS never needs the
 * channel's inst-block phys.
 */

#include "oplib_weights_pool.h"
#include "oplib_pool.h"          /* OPLIB_POOL_SLOT_BYTES + slot offsets */
#include "uart.h"

#include <stdbool.h>
#include <string.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../include/cache.h"
#include "nvidia/ga10b_bringup.h"
#include "nvidia/ga10b_ce.h"
#include "nvidia/ga10b_channel_handoff.h"

#define POOL_DEFAULT_ALIGN 256u

/* Chunk size for the staging loop — one CE memcpy moves up to this
 * many bytes from the SASS pool's SCRATCH0 bounce slot into the
 * weights pool. Equal to OPLIB_POOL_SLOT_BYTES (64 KB) so a single
 * memcpy fills the bounce before each CE dispatch. CE itself can
 * move much more per LAUNCH_DMA (up to GA10B_CE_MAX_BYTES_PER_LAUNCH
 * = 16 MB), but the bounce-slot ceiling is the binding constraint. */
#define POOL_STAGE_CHUNK_BYTES  OPLIB_POOL_SLOT_BYTES

static uint64_t g_pool_bytes_used = 0;

uint64_t oplib_weights_pool_alloc(size_t size, size_t align)
{
    if (size == 0) {
        return 0;
    }
    if (align == 0) {
        align = POOL_DEFAULT_ALIGN;
    }
    if ((align & (align - 1u)) != 0u) {
        return 0;
    }

    uint64_t pool_base = ga10b_weights_pool_gpu_va();
    uint64_t pool_size = ga10b_weights_pool_size_bytes();
    if (pool_base == 0 || pool_size == 0) {
        return 0;
    }

    uint64_t aligned_offset =
        (g_pool_bytes_used + (uint64_t)align - 1u) &
        ~((uint64_t)align - 1u);

    /* Guard the addition against overflow before comparing. */
    if (size > pool_size) {
        return 0;
    }
    if (aligned_offset > pool_size - size) {
        return 0;
    }

    g_pool_bytes_used = aligned_offset + size;
    return pool_base + aligned_offset;
}

int oplib_weights_pool_stage(uint64_t gpu_va,
                              const void *src,
                              size_t len)
{
    if (src == NULL || len == 0) {
        return -1;
    }

    uint64_t pool_base = ga10b_weights_pool_gpu_va();
    uint64_t pool_size = ga10b_weights_pool_size_bytes();
    if (pool_base == 0 || pool_size == 0) {
        return -1;
    }
    if (gpu_va < pool_base) {
        return -1;
    }
    if ((uint64_t)len > pool_size) {
        return -1;
    }
    if (gpu_va - pool_base > pool_size - (uint64_t)len) {
        return -1;
    }

    struct ga10b_bringup *b = ga10b_bringup_state();
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (b == NULL || h == NULL ||
        h->shader_phys == 0 || h->shader_gpu_va == 0 ||
        h->shader_size < OPLIB_POOL_MIN_BYTES) {
        /* No helper-staged bounce buffer means we have nowhere to
         * land CPU bytes before the CE picks them up. Return -1 so
         * the Rust caller falls back to "GPU staging unavailable". */
        return -1;
    }

    /* Bounce slot in the SASS pool's SCRATCH0 region. The CE dispatch
     * path is single-threaded (caller holds g_gpu_dispatch_lock), so
     * even though SCRATCH0 is shared with the per-op dispatchers
     * (W4-W7), staging completes before inference fires and the slot
     * is free again. */
    uint64_t bounce_phys = h->shader_phys + OPLIB_POOL_OFF_SCRATCH0;
    uint64_t bounce_gva  = h->shader_gpu_va + OPLIB_POOL_OFF_SCRATCH0;

    const uint8_t *src_bytes = (const uint8_t *)src;
    size_t off = 0;
    while (off < len) {
        size_t to_copy = POOL_STAGE_CHUNK_BYTES;
        if (to_copy > len - off) {
            to_copy = len - off;
        }

        /* CPU writes the chunk into the bounce slot. Identity-mapped
         * phys works here because the SASS pool is small-and-
         * contiguous carveout (unlike the GB-scale IOVMM weights
         * pool whose CPU-side phys is unreliable). cache_clean_range
         * drains the writes to PoC; CE reads through DRAM. */
        memcpy((void *)(uintptr_t)bounce_phys, src_bytes + off, to_copy);
        cache_clean_range((void *)(uintptr_t)bounce_phys,
                          (size_t)((to_copy + 4095u) & ~4095ull));

        /* CE memcpy bounce_gva → destination chunk inside the pool.
         * Both VAs resolve through the channel's inherited GMMU,
         * which the GPU side never needs SLM-OS to re-walk. */
        int rc = ga10b_ce_memcpy(b, bounce_gva, gpu_va + off,
                                  (uint32_t)to_copy);
        if (rc != 0) {
            return rc;
        }
        off += to_copy;
    }
    return 0;
}

void oplib_weights_pool_reset(void)
{
    g_pool_bytes_used = 0;
}

size_t oplib_weights_pool_bytes_used(void)
{
    return (size_t)g_pool_bytes_used;
}

size_t oplib_weights_pool_total_size(void)
{
    return (size_t)ga10b_weights_pool_size_bytes();
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* Stubs for non-Jetson platforms (QEMU, Pi 5, x86-64). The weights
 * pool is GA10B-channel-specific; on every other platform the
 * accessors return 0 and the pool is permanently empty. The Rust
 * `LoadedSlm::stage_tensor_to_gpu` wrapper treats a zero return
 * from `slm_runtime_stage_weight` as "not available, use CPU path",
 * so these stubs need only return failure. */

uint64_t oplib_weights_pool_alloc(size_t size, size_t align)
{
    (void)size;
    (void)align;
    return 0;
}

int oplib_weights_pool_stage(uint64_t gpu_va,
                              const void *src,
                              size_t len)
{
    (void)gpu_va;
    (void)src;
    (void)len;
    return -1;
}

void oplib_weights_pool_reset(void) { }

size_t oplib_weights_pool_bytes_used(void) { return 0; }
size_t oplib_weights_pool_total_size(void) { return 0; }

#endif
