/*
 * oplib_weights_pool.c — bump allocator + GMMU-walked CPU staging
 * for the W1-published weights pool. See header for design notes.
 */

#include "oplib_weights_pool.h"
#include "uart.h"

#include <stdbool.h>
#include <string.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../include/cache.h"
#include "nvidia/ga10b_bringup.h"
#include "nvidia/ga10b_channel_handoff.h"
#include "nvidia/ga10b_gmmu.h"

/* GMMU page granularity used by the pool's helper-side mapping
 * (the helper's NVGPU_AS_IOCTL_MAP_BUFFER_EX call passes
 * page_size=4096). Per-page walks are at this granularity. */
#define POOL_PAGE_SIZE   4096u
#define POOL_DEFAULT_ALIGN 256u

static uint64_t g_pool_bytes_used = 0;
/* Cached after first successful resolve so per-stage calls don't
 * re-scan DRAM. Reset when the pool resets (unload path) so a
 * post-kexec re-bringup doesn't reuse a stale value. */
static uint64_t g_inst_block_phys_cache = 0;

/* Validate a candidate inst-block phys by walking the channel
 * handoff's pushbuf gpu_va and confirming the walk lands at the
 * handoff's pushbuf phys. The pushbuf is the most reliable witness
 * pair available post-kexec — every helper-built channel maps it,
 * and its phys is recorded in the v2+ handoff prefix.
 *
 * Returns true if the inst block is usable. False if the walk
 * fails or lands at the wrong phys (FECS_CURRENT_CTX has been
 * observed to return a stale-but-non-zero value on Jetson when
 * Linux unbound the channel between the helper's last activity
 * and SLM-OS's first GMMU touch — the address-bits look plausible
 * but the PDB reads as zero or points into freed memory). */
static bool inst_block_validates(uint64_t inst)
{
    if (inst == 0) {
        return false;
    }
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL ||
        h->pushbuf_gpu_va == 0 ||
        h->pushbuf_phys == 0) {
        return false;
    }
    struct ga10b_gmmu_walk_result wr;
    if (ga10b_gmmu_walk(inst, h->pushbuf_gpu_va, &wr) != 0) {
        return false;
    }
    if (wr.status != GA10B_GMMU_WALK_OK) {
        return false;
    }
    return wr.leaf_phys == h->pushbuf_phys;
}

static uint64_t resolve_inst_block(void)
{
    if (g_inst_block_phys_cache != 0) {
        return g_inst_block_phys_cache;
    }
    /* Try the cheap FECS_CURRENT_CTX read first, then validate. */
    uint64_t inst = ga10b_gmmu_discover_inst_block_phys();
    if (inst_block_validates(inst)) {
        g_inst_block_phys_cache = inst;
        return inst;
    }

    /* Walk-based fallback: scan DRAM for a 4 KB-aligned candidate
     * whose GMMU walk for `pushbuf_gpu_va` lands at the handoff's
     * `pushbuf_phys`. Bounded by the 4 GB scan range; cost is in
     * milliseconds and only paid once per pool init. The
     * walk-discoverer rejects bogus candidates internally so its
     * return value already implies a valid PDB. */
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL ||
        h->pushbuf_gpu_va == 0 ||
        h->pushbuf_phys == 0) {
        return 0;
    }
    /* Cover the full Tegra Orin DRAM range — `phys_in_dram`'s
     * cap is 0x240000000 (9 GB). nvgpu inst-block carveouts have
     * been observed below 4 GB on some boots; weights/pushbuf
     * dmabufs cluster in 4-8 GB. The 0x80000000..0x240000000
     * span covers everything `phys_in_dram` permits. */
    inst = ga10b_gmmu_discover_inst_block_via_walk(
        h->pushbuf_gpu_va, h->pushbuf_phys,
        0x80000000ULL, 0x240000000ULL);
    if (inst != 0) {
        g_inst_block_phys_cache = inst;
    }
    return inst;
}

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

    uint64_t inst = resolve_inst_block();
    if (inst == 0) {
        return -1;
    }

    const uint8_t *src_bytes = (const uint8_t *)src;
    size_t off = 0;
    while (off < len) {
        uint64_t cur_va  = gpu_va + off;
        uint64_t page_va = cur_va & ~((uint64_t)POOL_PAGE_SIZE - 1u);
        size_t page_off  = (size_t)(cur_va - page_va);
        size_t to_copy   = POOL_PAGE_SIZE - page_off;
        if (to_copy > len - off) {
            to_copy = len - off;
        }

        struct ga10b_gmmu_walk_result r;
        if (ga10b_gmmu_walk(inst, page_va, &r) != 0 ||
            r.status != GA10B_GMMU_WALK_OK) {
            return -1;
        }
        uint8_t *dst = (uint8_t *)(uintptr_t)(r.leaf_phys + page_off);
        memcpy(dst, src_bytes + off, to_copy);
        cache_clean_range(dst, to_copy);
        off += to_copy;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

void oplib_weights_pool_reset(void)
{
    g_pool_bytes_used = 0;
    g_inst_block_phys_cache = 0;
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
