/*
 * ga10b_qmd.h — Ampere QMDV03_00 descriptor encoder for SLM-OS.
 *
 * Authors a 256-byte Queue Meta Data descriptor that the GA10B's SKED
 * compute scheduler decodes to launch a compute kernel. Mirrors NVK's
 * `Qmd3_0` (../slmos-reference-cache/mesa/mesa-nak_qmd.rs:499-528) and
 * the Linux-side helper at scripts/gpu-launch-common.c — both produce
 * the same bit layout for AMPERE_COMPUTE_B.
 *
 * Bit positions are fixed by NVIDIA's auto-generated header
 * (../slmos-reference-cache/mesa/mesa-clc7c0qmd.h, QMDV03_00 section)
 * and are reproduced as named constants below. Each bit-range is
 * a (HI, LO) pair encoding a closed interval [LO..HI] in bit-index
 * units (bit 0 = qmd[0] LSB).
 *
 * Encoder is pure-logic and does NOT touch hardware. Wire to a real
 * dispatch via the gsp_platform cache-clean call after writing.
 *
 * Replaces the helper-baked-QMD-replay path (issue #558). See
 * docs/gpu-qmd-per-dispatch-plan.md for context.
 */

#ifndef GPU_NVIDIA_GA10B_QMD_H
#define GPU_NVIDIA_GA10B_QMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 *   shader_gpu_va     — full 49-bit GPU virtual address of the SASS
 *   cbuf_gpu_va       — GPU VA of cbuf[0] (CUDA param area)
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

#endif /* GPU_NVIDIA_GA10B_QMD_H */
