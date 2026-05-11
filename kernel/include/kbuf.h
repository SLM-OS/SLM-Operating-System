/*
 * kbuf.h — virtually-contiguous kernel buffer allocator.
 *
 * Purpose
 * -------
 * `pmm_alloc_pages(N)` returns a single physically-contiguous buddy
 * block. The buddy allocator caps at `PMM_MAX_ORDER = 19` (2 GB), and
 * after the post-kexec memory layout settles (especially on Jetson
 * with a multi-GB nvmap reservation upstream) the largest *available*
 * single block can be much smaller than the cumulative free pages —
 * fragmentation forecloses big-order requests well before the pool is
 * exhausted.
 *
 * `kbuf_alloc(bytes)` sidesteps that by producing a buffer that is
 * *virtually* contiguous but may be *physically* scattered into many
 * 2 MB (`HUGE_PAGE_SIZE`) buddy blocks. Callers who only need CPU
 * virtual-address access (the GGUF stream buffer is the motivating
 * case — #789) see a normal flat pointer; DMA-class callers that
 * require physical contiguity must keep using `pmm_alloc_pages`.
 *
 * Allocation strategy (#789)
 * --------------------------
 * 1. Fast path: try `pmm_alloc_pages(pages)` for the whole request.
 *    On success the returned pointer is already in the identity
 *    map and we just record-and-return — no VMM mapping needed.
 * 2. Fallback: split into 2 MB chunks. Each `pmm_alloc_pages(512)`
 *    returns a 2 MB-aligned physical block (`HUGE_PAGE_SIZE`).
 *    `vmm_map_block` installs each at the next 2 MB slot of a
 *    bump-allocated kernel VA region (`KBUF_VA_BASE` onwards). The
 *    resulting buffer is one contiguous VA spanning N physical
 *    chunks.
 *
 * Symmetric free: `kbuf_free(va)` looks up the recorded allocation,
 * unmaps + frees each underlying chunk (or just `pmm_free_pages` in
 * the fast-path case). `kbuf_owns_va(p)` is a cheap range check so
 * `slm_free_pages` can route correctly without knowing the kind.
 *
 * Threading
 * ---------
 * The kbuf table is guarded by an internal spinlock. `slm xload` is
 * the only producer today and runs on a single shell session, so
 * contention is zero — the lock is there for future callers (model
 * hot-swap, multi-session staging) and so the unload free path is
 * safe against concurrent allocation.
 *
 * Limits
 * ------
 * - `KBUF_MAX_REGIONS` is the static cap on simultaneous outstanding
 *   allocations. Set to a small constant (8) because typical use is
 *   one-or-two loaded models at a time; bump it if SLM_MAX_SLOTS grows.
 * - `KBUF_MAX_CHUNKS_PER_REGION` caps a single allocation's size at
 *   `KBUF_MAX_CHUNKS_PER_REGION * 2 MB`. Sized to cover a 4 GB GGUF
 *   comfortably (>2× Qwen2.5-1.5B's 1.07 GB Q4_K_M).
 */

#ifndef SLM_OS_KBUF_H
#define SLM_OS_KBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Kernel VA window the chunked-allocation path maps into. Sits well
 * above the identity-map's reach on every shipping platform (Pi 5 4 GB,
 * Jetson 8 GB, QEMU 1 GB) so installing a fresh non-identity mapping
 * here can't collide with existing kernel mappings.
 *
 * VA layout (39-bit kernel half):
 *   0xFFFFFF80_00000000  KERNEL_VA_BASE (identity-mapped RAM starts)
 *   0xFFFFFF81_00000000  +1 GB into identity map
 *   ...
 *   0xFFFFFF88_00000000  KBUF_VA_BASE (32 GB above kernel base)
 *   0xFFFFFF90_00000000  KBUF_VA_LIMIT (+32 GB of kbuf VA window)
 *
 * The window is 32 GB which covers an arbitrary number of model loads
 * before the bump pointer would need recycling. We don't recycle
 * today (allocations are rare and the VA pool is enormous), so a
 * future caller stressing the window would surface as an out-of-VA
 * error from `kbuf_alloc`.
 */
#define KBUF_VA_BASE        0xFFFFFF8800000000UL
#define KBUF_VA_LIMIT       0xFFFFFF9000000000UL

/* Chunk granularity == VMM's 2 MB block-mapping size. */
#define KBUF_CHUNK_BYTES    (2u * 1024u * 1024u)
#define KBUF_CHUNK_PAGES    (KBUF_CHUNK_BYTES / 4096u)

/*
 * Allocate a virtually-contiguous buffer of at least `bytes` bytes.
 *
 * The returned pointer is a kernel VA. Cumulatively `bytes` rounded
 * up to `KBUF_CHUNK_BYTES` is reserved. Returns NULL if no chunk can
 * be allocated or the bookkeeping table is full.
 *
 * Mapping flags: writable, cached normal memory (`VMM_FLAGS_KERNEL_DATA`).
 * The buffer is suitable for CPU memcpy + dispatch through the SLM
 * runtime's CPU-side data path. It is NOT suitable for DMA from a
 * device that requires physically contiguous memory.
 */
void *kbuf_alloc(size_t bytes);

/*
 * Free a buffer previously returned by `kbuf_alloc`. Idempotent on
 * NULL. Pointer must be exactly the value returned by `kbuf_alloc`
 * (sub-region pointers will assert / fail-fast).
 */
void  kbuf_free(void *va);

/*
 * True if `va` lies inside the kbuf-managed window. Lets a generic
 * free dispatcher (`slm_free_pages`) route correctly without tracking
 * the allocation kind at the call site.
 */
bool  kbuf_owns_va(const void *va);

/*
 * Diagnostic accessors — currently used by `kbuf` unit tests and
 * (eventually) a `mem kbuf` shell verb.
 */
size_t kbuf_total_bytes_allocated(void);
size_t kbuf_region_count(void);

#endif /* SLM_OS_KBUF_H */
