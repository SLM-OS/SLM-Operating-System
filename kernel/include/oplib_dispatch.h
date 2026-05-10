/*
 * oplib_dispatch.h - Prepare a v7 op for SASS dispatch via the
 * operator library (#714, A.2 follow-on).
 *
 * Sits between the per-op metadata registry (operator_dispatch.h)
 * and the existing GA10B v7 pipeline machinery
 * (kernel/gpu/nvidia/ga10b_bringup.c). The pieces:
 *
 *   1. Look up the SASS GPU VA for the op via oplib_pool.
 *   2. Allocate a cbuf page from the inherited channel's GMMU via
 *      ga10b_gmmu_alloc.
 *   3. Populate the cbuf via the registered cbuf-args builder.
 *   4. Compute the launch shape via the registered launch-shape fn.
 *   5. Construct a v7 op struct ready to feed into the existing
 *      v7-pipeline dispatch loop.
 *
 * Step 6 (actually submitting the pushbuffer + polling the
 * semaphore) requires the v7 dispatch loop to accept an explicit
 * ops_v7 array parameter (currently it reads from
 * g_handoff.pipeline_ops_phys, which the Linux pre-kexec helper
 * stages). That refactor is the next-PR follow-on; until then,
 * `slm_oplib_prepare_dispatch` is the integration test surface
 * for everything up to "ready to fire".
 *
 * Jetson-only. On other platforms the function compiles to a stub
 * that returns -1 (no GA10B GMMU, no operator library staging).
 */

#ifndef OPLIB_DISPATCH_H
#define OPLIB_DISPATCH_H

#include "operator_dispatch.h"   /* struct operator_dispatch_args */

#include <stddef.h>
#include <stdint.h>

/* Result of `slm_oplib_prepare_dispatch`: everything needed for the
 * eventual submit step. The cbuf is GMMU-mapped and populated; the
 * v7-op fields (qmd, sema, payload, flags) that depend on the
 * dispatcher's runtime choices (which QMD slot, which sema page,
 * which payload value) are LEFT ZERO so the caller can fill them
 * when it wires the actual submit. */
struct slm_oplib_dispatch_prep {
    uint64_t shader_gpu_va;     /* SASS pool VA + entry sass_offset */
    size_t   shader_size;       /* entry sass_size */
    uint64_t cbuf_gpu_va;       /* GMMU-allocated cbuf base */
    void    *cbuf_cpu_va;       /* kernel-VA alias of cbuf */
    uint64_t cbuf_phys;         /* cbuf phys */
    uint32_t grid_x, grid_y, grid_z;
    uint32_t block_x, block_y, block_z;
    uint32_t register_count_v;
    uint32_t smem_size_bytes;
    uint32_t slm_size_bytes;
    uint32_t barrier_count;
};

/* Prepare a v7 op for SASS dispatch. Allocates a cbuf page via
 * `ga10b_gmmu_alloc` (Jetson-only), populates it from `args` via
 * the registered cbuf-args builder, looks up the SASS GPU VA, and
 * computes the launch shape. Returns 0 on success and fills `out`.
 *
 * `tier` and `dtype` follow the operator-library schema (see
 * kernel/include/gpu_handoff.h::SLM_GPU_TIER_* and
 * enum slm_gpu_dtype). For RMSNORM today, pass tier=SIMT, dtype=FP16.
 *
 * `inst_block_phys` is the GPU channel whose page tables receive
 * the cbuf mapping. Caller obtains it from
 * `ga10b_bringup_handoff()->inst_block_phys` (real handoff) or
 * `ga10b_gmmu_discover_inst_block_phys()` (FECS fallback).
 *
 * Returns:
 *   0 on success.
 *   -1 on lookup miss, GMMU alloc failure, builder mismatch, or
 *      Jetson-only-platform stub fallback.
 *
 * The cbuf allocated here is owned by the caller after success —
 * it is not auto-freed when the dispatch completes. Future
 * pooled-cbuf reuse will land alongside the submit-side wiring.
 */
int slm_oplib_prepare_dispatch(uint64_t inst_block_phys,
                                uint32_t op_kind,
                                uint32_t tier,
                                uint32_t dtype,
                                const struct operator_dispatch_args *args,
                                struct slm_oplib_dispatch_prep *out);

/* Forward decl — defined in kernel/gpu/nvidia/ga10b_bringup.h. The
 * full struct definition isn't needed by callers of `slm_oplib_dispatch`. */
struct ga10b_bringup;

/* Fire a single SASS kernel from the operator library through the
 * inherited GPU channel.
 *
 * Composes `slm_oplib_prepare_dispatch` (lookup SASS, allocate cbuf,
 * populate, compute launch shape) with
 * `ga10b_dispatch_v7_pipeline_inline` (build a 1-element v7 op,
 * submit through QMD pool + pushbuffer + semaphore, poll
 * completion). The dispatcher reuses the inherited handoff's
 * channel resources — caller must ensure the bringup state is
 * channel-open and the operator library is staged via
 * `oplib_pool_stage_to_gpu`.
 *
 * Returns 0 on dispatch + completion, -1 on any failure
 * (lookup miss, GMMU alloc failure, GPU dispatch timeout). The
 * cbuf allocated by `prepare_dispatch` stays mapped after return —
 * caller can free via the GMMU's free path or leave it for the
 * next dispatch's cbuf if reuse semantics are wanted.
 *
 * Jetson-only. On non-Jetson platforms returns -1 without side
 * effects.
 */
int slm_oplib_dispatch(struct ga10b_bringup *b,
                       uint64_t inst_block_phys,
                       uint32_t op_kind,
                       uint32_t tier,
                       uint32_t dtype,
                       const struct operator_dispatch_args *args);

#endif /* OPLIB_DISPATCH_H */
