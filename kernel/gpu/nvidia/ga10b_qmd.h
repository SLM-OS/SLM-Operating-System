/*
 * ga10b_qmd.h — Ampere QMDV03_00 descriptor encoder for SLM-OS.
 *
 * Authors a 256-byte Queue Meta Data descriptor that the GA10B's SKED
 * compute scheduler decodes to launch a compute kernel. Mirrors NVK's
 * `Qmd3_0` (~/slmos-ref/mesa/mesa-nak_qmd.rs:499-528) and
 * the Linux-side helper at scripts/gpu-launch-common.c — both produce
 * the same bit layout for AMPERE_COMPUTE_B.
 *
 * Bit positions are fixed by NVIDIA's auto-generated header
 * (~/slmos-ref/mesa/mesa-clc7c0qmd.h, QMDV03_00 section)
 * and are reproduced as named constants below. Each bit-range is
 * a (HI, LO) pair encoding a closed interval [LO..HI] in bit-index
 * units (bit 0 = qmd[0] LSB).
 *
 * Encoder is pure-logic and does NOT touch hardware. Wire to a real
 * dispatch via the gsp_platform cache-clean call after writing.
 *
 * Replaces the helper-baked-QMD-replay path (issue #558). See
 * docs/archive/plans/gpu-qmd-per-dispatch-plan.md for context.
 */

#ifndef GPU_NVIDIA_GA10B_QMD_H
#define GPU_NVIDIA_GA10B_QMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ga10b_channel_handoff.h"   /* struct ga10b_pipeline_op_v7 */

/* QMDV03_00 is exactly 256 bytes (64 × uint32_t). */
#define GA10B_QMD_SIZE_BYTES 256u
#define GA10B_QMD_DWORDS     (GA10B_QMD_SIZE_BYTES / 4u)

/* ---- QMDV03_00 bit positions (Ampere) ----
 *
 * Mirrored from scripts/gpu-launch-common.h:104-148 — same encoder,
 * different cache-flush primitives. Keep this list in sync with the
 * Linux-side header until the helper migrates to use this one.
 */

/* Header / version. */
#define GA10B_QMD_MAJOR_VERSION_HI                  583
#define GA10B_QMD_MAJOR_VERSION_LO                  580
#define GA10B_QMD_VERSION_HI                        579
#define GA10B_QMD_VERSION_LO                        576
#define GA10B_QMD_API_VISIBLE_CALL_LIMIT_BIT        378
#define GA10B_QMD_SAMPLER_INDEX_BIT                 382
#define GA10B_QMD_BARRIER_COUNT_HI                  767
#define GA10B_QMD_BARRIER_COUNT_LO                  763

/* Grid dimensions (CTA raster width / height / depth). */
#define GA10B_QMD_CTA_RASTER_WIDTH_HI               415
#define GA10B_QMD_CTA_RASTER_WIDTH_LO               384
#define GA10B_QMD_CTA_RASTER_HEIGHT_HI              431
#define GA10B_QMD_CTA_RASTER_HEIGHT_LO              416
#define GA10B_QMD_CTA_RASTER_DEPTH_HI               463
#define GA10B_QMD_CTA_RASTER_DEPTH_LO               448

/* Block dimensions (CTA thread dim 0/1/2). */
#define GA10B_QMD_CTA_THREAD_DIM0_HI                607
#define GA10B_QMD_CTA_THREAD_DIM0_LO                592
#define GA10B_QMD_CTA_THREAD_DIM1_HI                623
#define GA10B_QMD_CTA_THREAD_DIM1_LO                608
#define GA10B_QMD_CTA_THREAD_DIM2_HI                639
#define GA10B_QMD_CTA_THREAD_DIM2_LO                624

/* Register count (per-thread). */
#define GA10B_QMD_REGISTER_COUNT_V_HI               656
#define GA10B_QMD_REGISTER_COUNT_V_LO               648

/* Shared / local memory. */
#define GA10B_QMD_SHARED_MEMORY_SIZE_HI             561
#define GA10B_QMD_SHARED_MEMORY_SIZE_LO             544
#define GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI      759
#define GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO      736
#define GA10B_QMD_SHADER_LOCAL_MEM_HIGH_SIZE_HI     1623
#define GA10B_QMD_SHADER_LOCAL_MEM_HIGH_SIZE_LO     1600

/* Program (shader) address — Ampere uses an absolute 49-bit address
 * split across two adjacent fields. */
#define GA10B_QMD_PROGRAM_ADDRESS_LOWER_HI          1567
#define GA10B_QMD_PROGRAM_ADDRESS_LOWER_LO          1536
#define GA10B_QMD_PROGRAM_ADDRESS_UPPER_HI          1584
#define GA10B_QMD_PROGRAM_ADDRESS_UPPER_LO          1568

/* Per-cbuf descriptor fields. The QMD has 8 cbuf slots; each slot is
 * 64 bits wide and starts at base + idx * 64. */
#define GA10B_QMD_CBUF_ADDR_LO_BASE                 1024
#define GA10B_QMD_CBUF_ADDR_HI_BASE                 1056
#define GA10B_QMD_CBUF_SIZE_SHIFTED4_BASE           1075
#define GA10B_QMD_CBUF_VALID_BASE                   640
#define GA10B_QMD_CBUF_PREFETCH_POST_BASE           1073
#define GA10B_QMD_CBUF_INVALIDATE_BASE              1074

/* Cache invalidate flags. Set to 1 to ask the SM to drop the named
 * cache before this kernel runs. */
#define GA10B_QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT   186
#define GA10B_QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT  187
#define GA10B_QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT     188
#define GA10B_QMD_INVALIDATE_SHADER_DATA_CACHE_BIT      189
#define GA10B_QMD_INVALIDATE_INSTRUCTION_CACHE_BIT      190
#define GA10B_QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT  191

/* CTA Workload Dispatcher membar — runs before this QMD's CTAs
 * launch. NV reference: ~/slmos-ref/mesa/mesa-clc7c0qmd.h
 * NVC7C0_QMDV02_03_CWD_MEMBAR_TYPE at MW(369:368), 2-bit field with:
 *   L1_NONE      = 0  (no membar — default; can leave stale L1/L2
 *                       lines visible to a kernel that just had its
 *                       inputs rewritten by the CPU)
 *   L1_SYSMEMBAR = 1  (system memory barrier; forces this CTA's
 *                       loads to observe any pending writes through
 *                       to sysmem — the option needed to fix the
 *                       iter-1 stale-input race in #596)
 *   L1_MEMBAR    = 3  (GPU-only memory barrier, weaker than
 *                       SYSMEMBAR; ordering only within GPU caches) */
#define GA10B_QMD_CWD_MEMBAR_TYPE_LO_BIT                368
#define GA10B_QMD_CWD_MEMBAR_TYPE_HI_BIT                369
#define GA10B_QMD_CWD_MEMBAR_TYPE_L1_NONE                 0u
#define GA10B_QMD_CWD_MEMBAR_TYPE_L1_SYSMEMBAR            1u
#define GA10B_QMD_CWD_MEMBAR_TYPE_L1_MEMBAR               3u

/* Caching enable flags. */
#define GA10B_QMD_SM_GLOBAL_CACHING_ENABLE_BIT          134

/* Default cbuf[0] size — matches the Linux helper's
 * GPU_LAUNCH_CBUF_SIZE_B (512 B reserves 32 B for CUDA built-in dim
 * vars at offsets 0x00..0x14 and leaves the rest for kernel params). */
#define GA10B_QMD_CBUF0_SIZE_BYTES                  512u

/* ---- Bit-range setter ----
 *
 * Pack `val` into bits [hi:lo] of a 256-byte QMD stored as
 * `uint32_t qmd[64]`. Pure-logic; safe to call without any platform
 * ops installed. Mirrors gpu_qmd_set_bits in scripts/gpu-qmd-bits.h.
 */
void ga10b_qmd_set_bits(uint32_t *qmd, unsigned hi, unsigned lo,
                        uint64_t val);

/* ---- High-level encoder ----
 *
 * Populate a 256-byte QMD buffer for a compute kernel launch. The
 * caller is responsible for the buffer being 256-byte aligned, GMMU-
 * mapped, and cache-flushed after the call (we don't know about
 * gsp_platform here — keep this layer pure-logic so host tests can
 * exercise it without a vtable).
 *
 * Args:
 *   qmd               — destination, must be 64 × uint32_t (256 B)
 *   shader_gpu_va     — GPU virtual address of the SASS. QMDV03_00
 *                       PROGRAM_ADDRESS spans 49 bits (32 + 17);
 *                       addresses above 2^49 are silently truncated
 *                       to the low 49 bits — same behaviour as the
 *                       Linux nvgpu helper. Today's nvgpu allocator
 *                       returns GPU VAs comfortably under 2^40 so
 *                       this is a future-proofing note, not a hot
 *                       hazard.
 *   cbuf_gpu_va       — GPU VA of cbuf[0] (CUDA param area). Same
 *                       49-bit cap as `shader_gpu_va`.
 *   register_count_v  — per-thread register usage from the SASS header
 *   grid_x/y/z        — number of CTAs in each dimension
 *   block_x/y/z       — threads per CTA in each dimension
 *
 * Sets all the cache-invalidate bits and SM_GLOBAL_CACHING_ENABLE,
 * mirroring the helper's defaults. Other fields (smem_size, slm_size,
 * barrier_count) default to 0; the caller can override them via
 * ga10b_qmd_set_bits if needed.
 *
 * Pure-logic; host-testable. Does NOT issue any cache_clean — the
 * caller is responsible for visibility to the GPU after this returns.
 */
void ga10b_qmd_populate(uint32_t *qmd,
                        uint64_t shader_gpu_va,
                        uint64_t cbuf_gpu_va,
                        uint32_t register_count_v,
                        uint32_t grid_x, uint32_t grid_y, uint32_t grid_z,
                        uint32_t block_x, uint32_t block_y, uint32_t block_z);

/* ---- Pool-slot dispatch helper ----
 *
 * Pick the next slot in a QMD pool, populate it from a v7 op's
 * QMD-construction inputs, advance the slot counter (mod
 * `pool_n_slots`), and return enough handles to the populated slot
 * for the dispatch path to:
 *   - cache_clean the slot bytes (uses `cpu_va`)
 *   - feed the GPU VA into `SEND_PCAS_A` (uses `gpu_va`)
 *
 * Pure-logic. The caller is responsible for:
 *
 *   - Pre-validating that the v7 handoff carries a non-zero pool
 *     (`qmd_pool_n_slots > 0` and `qmd_pool_gpu_va != 0`).
 *   - Calling `gsp_platform->cache_clean(result.cpu_va, 256)` and
 *     `gsp_platform->mb()` after this function returns, so the GPU
 *     sees the populated bytes once the dispatch fires.
 *
 * Args:
 *   pool_va         CPU virtual address of slot 0 (CPU-side identity-
 *                   mapped DRAM on Jetson; host tests pass a heap
 *                   buffer). Must be at least 4-byte aligned because
 *                   the encoder casts each 256-byte slot to
 *                   `uint32_t *` for word-granular writes. The
 *                   helper-side allocator returns page-aligned
 *                   memory, so this falls out automatically; it's
 *                   only a constraint for synthesised pools in
 *                   tests.
 *   pool_gpu_va     GPU virtual address of slot 0 (from the v7
 *                   handoff's `qmd_pool_gpu_va`).
 *   pool_n_slots    number of 256-byte slots in the pool (from the
 *                   v7 handoff's `qmd_pool_n_slots`).
 *   slot_inout      [in/out] the next-slot counter. Read at entry,
 *                   advanced (mod `pool_n_slots`) on exit.
 *   op              the v7 op whose QMD inputs (shader_gpu_va,
 *                   cbuf_gpu_va, register_count_v, grid/block dims)
 *                   become the encoded QMD content.
 *
 * Returns a `ga10b_qmd_pool_slot`. On invalid input (NULL pointers
 * or `pool_n_slots == 0`), the returned struct has all-zero fields.
 *
 * smem_size_bytes, slm_size_bytes, and barrier_count from the v7 op
 * are written unconditionally over whatever `ga10b_qmd_populate` left
 * in those fields — the v7 op is authoritative. SIMT kernels (the
 * MNIST conv + pool + addrelu chain) send 0 in all three, which
 * encodes the same bits as populate's defaults but isolates the v7
 * path from future changes to those defaults. HMMA / WMMA kernels
 * (the FP32A×FP16W tensor-core GEMM at MNIST op 6) need
 * SHARED_MEMORY_SIZE = 2048 and BARRIER_COUNT = 3 — gpu-kernel-mnist
 * sets the v7 fields when invoked with `--gemm-tier hmma`. Without
 * the propagation the WMMA chain stalls op[N+1] silently (observed
 * empirically on Jetson GA10B).
 *
 * SLM is encoded into the LOW half only (24-bit field, ~16 MB
 * ceiling). The HIGH half stays at populate's zero default; no
 * realistic GA10B kernel needs > 16 MB of shader-local memory per
 * thread. Update this contract if a future workload hits the limit.
 */
struct ga10b_qmd_pool_slot {
    uint64_t gpu_va;        /* slot's GPU VA — feeds SEND_PCAS_A */
    uint8_t *cpu_va;        /* slot's CPU VA — for cache_clean */
    uint32_t index;         /* slot index used (for diagnostics) */
};

struct ga10b_qmd_pool_slot
ga10b_qmd_pool_prepare(uint8_t *pool_va,
                       uint64_t pool_gpu_va,
                       uint32_t pool_n_slots,
                       uint32_t *slot_inout,
                       const struct ga10b_pipeline_op_v7 *op);

#endif /* GPU_NVIDIA_GA10B_QMD_H */
