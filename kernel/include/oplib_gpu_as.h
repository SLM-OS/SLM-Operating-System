/*
 * oplib_gpu_as.h - GPU address-space interface for the operator library
 * (#1092).
 *
 * The operator library stages SASS, a cbuf page and per-call scratch
 * slots into the GPU address space of whatever channel dispatches its
 * kernels, and (for SLM inference) a weights pool. Two kinds of
 * channel exist:
 *
 *   - kexec boot: the channel Linux's helper built. Its page tables
 *     are Linux's; SLM-OS adds mappings through ga10b_gmmu.c, keyed
 *     off the inherited instance block.
 *   - UEFI-direct boot: the channel ga10b_cold_run() built
 *     (struct ga10b_chan). Its page tables are SLM-OS's own
 *     struct ga10b_vm and must be extended with ga10b_vm_map, never
 *     with ga10b_gmmu.c, whose PDE0 writer uses the wrong entry stride
 *     for a VM laid out by ga10b_vm_* (#1090).
 *
 * struct oplib_gpu_as hides the difference: allocate contiguous pages,
 * map them at a fresh GPU VA, invalidate the TLB. The staging code
 * below takes one, so the same code runs on either channel and, with
 * fake callbacks, in the unit tests.
 *
 * Everything in this header is pure (no MMIO, no globals) and built on
 * every platform. The Jetson glue that picks a backend at run time is
 * oplib_cold.h (cold channel) and oplib_pool.c (kexec channel).
 */
#ifndef OPLIB_GPU_AS_H
#define OPLIB_GPU_AS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ga10b_gpu_ops;
struct ga10b_vm;
struct ga10b_channel_handoff;

/* ---- The interface ---- */

struct oplib_gpu_as {
    const char *name;           /* "kexec-gmmu" / "cold-vm", for logs */
    void *ctx;
    /* `npages` physically contiguous 4 KB pages. Returns the CPU
     * pointer (NULL on failure) and the physical address in *phys.
     * The pages need not be zeroed. */
    void *(*alloc)(void *ctx, uint32_t npages, uint64_t *phys);
    /* Return pages from `alloc`. NULL when a failed map may have left
     * PTEs behind (no unmap): the pages are then leaked, never reused
     * under a live PTE. */
    void (*free)(void *ctx, void *cpu, uint32_t npages);
    /* Map [phys, phys + bytes) (4 KB aligned) at a fresh GPU VA. */
    int (*map)(void *ctx, uint64_t phys, uint64_t bytes, uint64_t *gpu_va);
    /* Optional split form of `map`, needed by the weights pool (one VA
     * range backed by several physical chunks): reserve VA (into
     * *gpu_va; returns OPLIB_AS_OK or a negative code), then map a
     * physical range at a VA inside it. NULL when unsupported. */
    int (*reserve)(void *ctx, uint64_t bytes, uint64_t align, uint64_t *gpu_va);
    int (*map_at)(void *ctx, uint64_t gpu_va, uint64_t phys, uint64_t bytes);
    /* Make new mappings visible to a GPU that may already have walked
     * this address space. NULL when `map` already does it. */
    int (*tlb_invalidate)(void *ctx);
    /* Clean CPU writes to DRAM (the GPU is not cache-coherent). */
    void (*clean)(void *ctx, const void *p, size_t len);
};

/* Error codes (negative). */
#define OPLIB_AS_OK            0
#define OPLIB_AS_ERR_ARG      (-1)
#define OPLIB_AS_ERR_NOMEM    (-2)
#define OPLIB_AS_ERR_MAP      (-3)
#define OPLIB_AS_ERR_TOO_BIG  (-4)
#define OPLIB_AS_ERR_UNSUPP   (-5)
#define OPLIB_AS_ERR_BUSY     (-6)  /* the VM's writer lock stayed busy */

/* ---- Cold backend: an SLM-OS-built struct ga10b_vm ----
 *
 * alloc/free/clean go through the channel's ga10b_gpu_ops; map
 * reserves VA in the VM's user range and calls ga10b_vm_map with
 * flags 0 (GPU-uncached, like the helper's MAP_BUFFER_EX and the cold
 * MNIST pipeline). `free` is NULL: ga10b_vm_map can fail after writing
 * some PTEs and there is no unmap.
 *
 * Other code writes the same VM (the SLM GPU program, the cold
 * pipelines), so reserve, map_at and map each run under the VM's
 * writer lock, `lock` / `unlock` (#1173; on Jetson slm_prog_vm_lock).
 * map reserves and maps in one hold. A lock that stays busy fails the
 * call with OPLIB_AS_ERR_BUSY and changes nothing. Allocation and
 * cleaning happen outside the lock. NULL hooks: nothing else writes
 * this VM (tests). */
struct oplib_cold_as_ctx {
    const struct ga10b_gpu_ops *ops;
    struct ga10b_vm *vm;
    int (*lock)(void);          /* 0 when taken */
    void (*unlock)(void);
};

void oplib_gpu_as_init_cold(struct oplib_gpu_as *as, struct oplib_cold_as_ctx *ctx,
                            const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                            int (*lock)(void), void (*unlock)(void));

/* ---- SASS pool + cbuf staging ----
 *
 * Layout from oplib_pool.h: one physically contiguous pool of
 * oplib_pool_self_bytes(sass_len) — SASS from offset 0 over
 * oplib_pool_sass_slots() 64 KB slots, then three per-call scratch
 * slots — plus a separate 4 KB cbuf page. For SASS up to one slot
 * this is the 256 KB helper layout. */
struct oplib_staged_pool {
    void *pool_cpu;
    uint64_t pool_phys;
    uint64_t pool_gpu_va;       /* offset 0 = SASS */
    uint64_t pool_bytes;
    uint64_t scratch_off;       /* scratch base = pool + scratch_off */
    void *cbuf_cpu;
    uint64_t cbuf_phys;
    uint64_t cbuf_gpu_va;
};

/* Allocate the pool, copy `sass` to offset 0 and zero the rest of the
 * pool (instruction prefetch past the last kernel reads zeros, as in
 * the helper's 64 KB zero-padded shader buffers), clean, map. Returns
 * OPLIB_AS_OK, OPLIB_AS_ERR_TOO_BIG when `sass_len` exceeds
 * OPLIB_POOL_SASS_MAX_BYTES (nothing allocated), or another negative
 * code. `sass_len` may be 0.
 * Does not invalidate the TLB (callers batch that). */
int oplib_gpu_as_stage_pool(const struct oplib_gpu_as *as,
                            const uint8_t *sass, size_t sass_len,
                            struct oplib_staged_pool *out);

/* Allocate, zero, clean and map one cbuf page into out->cbuf_*. */
int oplib_gpu_as_stage_cbuf(const struct oplib_gpu_as *as,
                            struct oplib_staged_pool *out);

/* ---- QMD pool for a channel-only handoff ----
 *
 * slm_oplib_dispatch fires through ga10b_dispatch_v7_pipeline_inline,
 * which takes a QMD slot from the active handoff's qmd_pool_*. The
 * cold MNIST step publishes one; a channel-only (v2) handoff has none.
 * If `h` has no QMD pool, allocate a zeroed one (GA10B_MNIST-sized:
 * 1024 x 256 B), map it, and fill h->qmd_pool_*. Returns 1 when `h`
 * was changed (caller re-installs it), 0 when it already had a pool,
 * negative on failure. */
#define OPLIB_QMD_POOL_SLOTS      1024u
#define OPLIB_QMD_POOL_SLOT_BYTES 256u
int oplib_gpu_as_ensure_qmd_pool(const struct oplib_gpu_as *as,
                                 struct ga10b_channel_handoff *h);

/* ---- Weights pool on SLM-OS-owned memory ----
 *
 * One contiguous GPU VA range (what oplib_weights_pool's bump
 * allocator hands out) backed by `n_chunks` physically contiguous
 * chunks. Unlike the kexec pool (IOVMM-stitched, phys unknown, filled
 * by the copy engine) every chunk is CPU-reachable, so staging is a
 * memcpy + clean. */
#define OPLIB_WPOOL_MAX_CHUNKS      64u
#define OPLIB_WPOOL_CHUNK_BYTES     (64ull << 20)   /* 64 MB, a PMM power of two */

struct oplib_wpool_chunk {
    void *cpu;
    uint64_t phys;
};

struct oplib_wpool {
    uint64_t gpu_va;            /* base of the range */
    uint64_t size;              /* bytes mapped = n_chunks * chunk_bytes */
    uint64_t chunk_bytes;
    uint32_t n_chunks;
    struct oplib_wpool_chunk chunk[OPLIB_WPOOL_MAX_CHUNKS];
};

/* Reserve VA for `request` bytes (rounded up to whole chunks, capped
 * at OPLIB_WPOOL_MAX_CHUNKS) and back it chunk by chunk. Allocation
 * stops at the first chunk the allocator cannot provide; the pool is
 * then smaller than requested (out->size says how big). Returns
 * OPLIB_AS_OK when at least one chunk was mapped, OPLIB_AS_ERR_NOMEM
 * when none could be allocated, OPLIB_AS_ERR_UNSUPP when `as` lacks
 * reserve/map_at, OPLIB_AS_ERR_BUSY when the VM's writer lock stayed
 * busy before the first chunk was mapped, or another negative code. `chunk_bytes` must be a
 * multiple of 4 KB (0 = OPLIB_WPOOL_CHUNK_BYTES). Chunks are not
 * zeroed here (the allocator may zero them). Does not invalidate the
 * TLB. */
int oplib_wpool_build(const struct oplib_gpu_as *as, uint64_t request,
                      uint64_t chunk_bytes, struct oplib_wpool *out);

/* CPU pointer for GPU VA `gpu_va` inside the pool, and how many bytes
 * from there stay inside the same chunk (*run). NULL when `gpu_va` is
 * outside the pool. */
void *oplib_wpool_translate(const struct oplib_wpool *w, uint64_t gpu_va,
                            uint64_t *run);

/* Copy `len` bytes to GPU VA `gpu_va` (split at chunk boundaries) and
 * clean them. The whole range must lie inside the pool. Returns
 * OPLIB_AS_OK or OPLIB_AS_ERR_ARG. */
int oplib_wpool_write(const struct oplib_wpool *w, const struct oplib_gpu_as *as,
                      uint64_t gpu_va, const void *src, size_t len);

#endif /* OPLIB_GPU_AS_H */
