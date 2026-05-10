/*
 * oplib_weights_pool.h — bump allocator + CPU-to-GPU staging for the
 * helper-published weights pool.
 *
 * Architecture (W2 of the GPU weights pool design,
 * docs/design/gpu-weights-pool.md):
 *
 *   - The Linux gpu-channel-helper pre-allocates a 1.5 GB IOVMM pool
 *     and publishes its (gpu_va, size) in the v8 channel handoff.
 *   - SLM-OS's `slm load` calls `oplib_weights_pool_alloc` to carve
 *     a per-tensor slot out of the pool's contiguous GPU VA range,
 *     then `oplib_weights_pool_stage` to copy CPU-resident tensor
 *     bytes into the GPU-mapped DRAM backing the slot.
 *   - The pool's CPU-side physical address is unreliable (IOVMM
 *     stitches scattered phys pages via SMMU; helper publishes
 *     phys=0 sentinel). To find CPU-writable phys for a given gpu_va
 *     within the pool, the staging path walks the channel's GMMU
 *     tree with `ga10b_gmmu_walk` per 4 KB page.
 *
 * Allocator is a bump pointer with no free list — designed for
 * single-model "load once at boot, run inference until kexec" usage.
 * `slm unload` would reset the pointer; in practice today nothing
 * unloads.
 */

#ifndef OPLIB_WEIGHTS_POOL_H
#define OPLIB_WEIGHTS_POOL_H

#include <stddef.h>
#include <stdint.h>

/* Allocate `size` bytes from the helper-staged weights pool, aligned
 * to `align` bytes (must be a power of two; pass 0 to use the default
 * 256-byte alignment that matches GA10B QMD granularity).
 *
 * Returns a non-zero GPU VA on success — within the contiguous range
 * [pool_base .. pool_base + pool_size). Returns 0 on:
 *   - no v8 handoff staged (pool size == 0),
 *   - pool exhausted,
 *   - non-power-of-two `align`.
 *
 * Pure-logic — no MMIO, no GMMU touches. The returned GPU VA is
 * useable from the GPU side immediately (the helper already mapped
 * the pool into the channel's GMMU pre-kexec); SLM-OS calls
 * `oplib_weights_pool_stage` to populate the CPU-visible bytes
 * before the GPU reads them. */
uint64_t oplib_weights_pool_alloc(size_t size, size_t align);

/* Copy `len` bytes from CPU-resident `src` into the GPU-mapped
 * weights pool at `gpu_va`. The destination range
 * [gpu_va .. gpu_va + len) MUST lie inside a slot previously
 * returned by `oplib_weights_pool_alloc`.
 *
 * Walks the channel's GMMU per 4 KB page to translate gpu_va to
 * the underlying physical page (Jetson's identity-mapped DRAM lets
 * the kernel dereference the resulting phys directly). memcpys
 * handle sub-page src offsets; cache_clean_range on each touched
 * page drains the writes to PoC; a final dsb-sy fences before
 * returning so the GPU's next read sees the bytes.
 *
 * Returns 0 on success. Negative on:
 *   - null src or zero len,
 *   - gpu_va range outside the pool,
 *   - inst block discovery failure (no FECS / no walk witness),
 *   - any per-page GMMU walk failure (PDE/PTE invalid). */
int oplib_weights_pool_stage(uint64_t gpu_va,
                              const void *src,
                              size_t len);

/* Reset the bump pointer to the start of the pool. After this, all
 * GPU VAs previously returned by `oplib_weights_pool_alloc` are
 * stale (the underlying GPU bytes are unchanged but the next alloc
 * will hand out the same VAs again). Intended for unload paths and
 * unit tests; production runtime never calls this today. */
void oplib_weights_pool_reset(void);

/* Inspector: bytes consumed by the bump allocator. */
size_t oplib_weights_pool_bytes_used(void);

/* Inspector: total bytes available in the pool (i.e. the v8
 * handoff's `weights_pool_size_bytes` field). 0 when no v8 handoff
 * is staged. */
size_t oplib_weights_pool_total_size(void);

#endif /* OPLIB_WEIGHTS_POOL_H */
