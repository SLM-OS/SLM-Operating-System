/*
 * oplib_dispatch.c - Prepare a v7 op for SASS dispatch via the
 * operator library (#714, A.2 follow-on).
 *
 * Composes oplib_pool's GPU-VA lookup, ga10b_gmmu's cbuf
 * allocator, and operator_dispatch's per-op metadata into a single
 * "everything needed to fire this kernel" preparation step. The
 * actual pushbuffer-firing submit lives in the next PR — that one
 * refactors `ga10b_dispatch_v7_pipeline` to accept an explicit
 * ops_v7 array, then calls it with the v7 op this file constructs.
 */

#include "oplib_dispatch.h"

#include "oplib_pool.h"
#include "operator_dispatch.h"
#include "operator_library.h"   /* OPERATOR_LIBRARY_ERR_* */
#include "uart.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../include/cache.h"
#include "nvidia/ga10b_gmmu.h"
#include "nvidia/ga10b_bringup.h"
#include "nvidia/ga10b_channel_handoff.h"
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* Populate CUDA's builtin-dim slot at cbuf[0][0x00..0x14] with the
 * blockDim and gridDim values. Mirrors the
 * gpu_write_builtin_dims() pattern documented in
 * scripts/gpu-launch-common.h:
 *
 *   [0x00] blockDim.x  uint32_t    [0x0C] gridDim.x   uint32_t
 *   [0x04] blockDim.y  uint32_t    [0x10] gridDim.y   uint32_t
 *   [0x08] blockDim.z  uint32_t    [0x14] gridDim.z   uint32_t
 *
 * CUDA-compiled SASS reads `blockDim.x` from `c[0x0][0x0]` etc.
 * via the IMAD R0, R0 (CTAID.X), c[0x0][0x0] (blockDim.x) pattern;
 * leaving these zero makes a kernel that reads them compute its
 * global thread index as if blockDim were 0 (every CTA collapses
 * onto the same range; only CTA(0,0) appears to have run). The
 * rmsnorm kernel happens not to read these slots (BLOCK_DIM is a
 * compile-time constant, threadIdx/blockIdx come from PTX
 * special registers), but populating them keeps the dispatcher
 * correct for any future operator-library entry that does. */
static void write_builtin_dims(void *cbuf_cpu,
                                const struct operator_launch_shape *shape)
{
    volatile uint32_t *bdim = (volatile uint32_t *)cbuf_cpu;
    bdim[0] = shape->block_x;   /* 0x00 blockDim.x */
    bdim[1] = shape->block_y;   /* 0x04 blockDim.y */
    bdim[2] = shape->block_z;   /* 0x08 blockDim.z */
    bdim[3] = shape->grid_x;    /* 0x0C gridDim.x  */
    bdim[4] = shape->grid_y;    /* 0x10 gridDim.y  */
    bdim[5] = shape->grid_z;    /* 0x14 gridDim.z  */
}

int slm_oplib_prepare_dispatch(uint64_t inst_block_phys,
                                uint32_t op_kind,
                                uint32_t tier,
                                uint32_t dtype,
                                const struct operator_dispatch_args *args,
                                struct slm_oplib_dispatch_prep *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }

    /* Zero the output up front so partial-failure paths leave
     * well-defined fields. */
    {
        uint8_t *p = (uint8_t *)out;
        for (size_t i = 0; i < sizeof(*out); i++) {
            p[i] = 0;
        }
    }

    /* 1. Look up the SASS GPU VA + size from the staged pool.
     *    Pre-condition: caller ran `oplib_pool_stage_to_gpu` first
     *    (typically via `nvgpu oplib stage`). If the pool isn't
     *    staged the lookup returns ERR_NULL and we bail. */
    uint64_t sass_va = 0;
    size_t   sass_size = 0;
    int rc = oplib_pool_get_sass_gpu_va(op_kind, tier, dtype,
                                         &sass_va, &sass_size);
    if (rc != 0) {
        uart_printf("[oplib-dispatch] SASS lookup miss: op_kind=%u "
                    "tier=%u dtype=%u rc=%d\n",
                    (unsigned)op_kind, (unsigned)tier,
                    (unsigned)dtype, rc);
        return -1;
    }

    /* 2. Get a cbuf page in the channel's GMMU. The cbuf is read by
     *    the SASS via cbuf[0] reads at offsets 0x160 onward — see
     *    operator_dispatch.h::OPERATOR_CBUF0_BASE.
     *
     *    Fast path: if the inherited handoff has a pre-staged cbuf
     *    (helper allocated `cbuf_*` fields in the channel's GMMU
     *    pre-kexec), use it directly. The cbuf is reused serially
     *    across dispatches — slm_oplib_dispatch polls the trailing
     *    semaphore before returning, so two dispatches can't race
     *    on the cbuf bytes.
     *
     *    Slow path: post-kexec ga10b_gmmu_alloc, which requires a
     *    correct inst_block_phys (currently unreliable on Jetson —
     *    see oplib_pool.c::oplib_pool_stage_to_gpu for the same
     *    architectural concern). */
    uint64_t cbuf_va = 0;
    uint64_t cbuf_phys = 0;
    void    *cbuf_cpu = NULL;
    {
        const struct ga10b_channel_handoff *hp = ga10b_bringup_handoff();
        if (hp != NULL && hp->cbuf_gpu_va != 0 && hp->cbuf_phys != 0) {
            cbuf_va  = hp->cbuf_gpu_va;
            cbuf_phys = hp->cbuf_phys;
            cbuf_cpu = (void *)(uintptr_t)hp->cbuf_phys;
        } else {
            rc = ga10b_gmmu_alloc(inst_block_phys, 1u, 0u,
                                  &cbuf_va, &cbuf_cpu, &cbuf_phys);
            if (rc < 0) {
                uart_printf("[oplib-dispatch] cbuf alloc failed: rc=%d\n", rc);
                return -1;
            }
        }
    }

    /* Zero the cbuf page so any unwritten bytes stay deterministic. */
    {
        volatile uint8_t *p = (volatile uint8_t *)cbuf_cpu;
        for (size_t i = 0; i < 4096; i++) {
            p[i] = 0;
        }
    }

    /* 3. Compute the launch shape (grid/block/regs/smem/...). The
     *    shape is needed before the cbuf write so the CUDA builtin-
     *    dim slot (cbuf[0][0x00..0x14]) can carry the right values
     *    — see step 4. */
    struct operator_launch_shape shape;
    rc = operator_dispatch_launch_shape(args, &shape);
    if (rc != 0) {
        uart_printf("[oplib-dispatch] launch_shape failed: rc=%d\n", rc);
        return -1;
    }

    /* 4. Populate cbuf. The CUDA builtin-dim slot at
     *    cbuf[0][0x00..0x14] gets blockDim/gridDim via
     *    write_builtin_dims (see helper above). The per-op
     *    builder then writes its kernel args at
     *    OPERATOR_CBUF0_BASE (0x160), beyond the builtin-dim
     *    region. */
    if (args->op_kind != op_kind) {
        uart_printf("[oplib-dispatch] args->op_kind=%u != op_kind=%u\n",
                    (unsigned)args->op_kind, (unsigned)op_kind);
        return -1;
    }
    write_builtin_dims(cbuf_cpu, &shape);
    rc = operator_dispatch_build_cbuf(cbuf_cpu, args);
    if (rc != 0) {
        uart_printf("[oplib-dispatch] cbuf build failed: rc=%d\n", rc);
        return -1;
    }
    cache_clean_range(cbuf_cpu, 4096);

    /* DSB SY so the cbuf bytes + page-table publication that
     * ga10b_gmmu_alloc already issued are both visible to the GPU
     * before any submit consumes the cbuf. The submit-side helper
     * (next PR) issues its own dsb sy too — this one orders the
     * data writes relative to the page-table writes. */
    __asm__ volatile("dsb sy" ::: "memory");

    out->shader_gpu_va    = sass_va;
    out->shader_size      = sass_size;
    out->cbuf_gpu_va      = cbuf_va;
    out->cbuf_cpu_va      = cbuf_cpu;
    out->cbuf_phys        = cbuf_phys;
    out->grid_x           = shape.grid_x;
    out->grid_y           = shape.grid_y;
    out->grid_z           = shape.grid_z;
    out->block_x          = shape.block_x;
    out->block_y          = shape.block_y;
    out->block_z          = shape.block_z;
    out->register_count_v = shape.register_count_v;
    out->smem_size_bytes  = shape.smem_size_bytes;
    out->slm_size_bytes   = shape.slm_size_bytes;
    out->barrier_count    = shape.barrier_count;

    uart_printf("[oplib-dispatch] prepared op_kind=%u: "
                "shader_va=0x%llx (%zu B), cbuf_va=0x%llx (cpu=%p phys=0x%llx), "
                "grid=(%u,%u,%u) block=(%u,%u,%u) regs=%u smem=%u\n",
                (unsigned)op_kind,
                (unsigned long long)sass_va, sass_size,
                (unsigned long long)cbuf_va,
                cbuf_cpu, (unsigned long long)cbuf_phys,
                (unsigned)shape.grid_x, (unsigned)shape.grid_y,
                (unsigned)shape.grid_z,
                (unsigned)shape.block_x, (unsigned)shape.block_y,
                (unsigned)shape.block_z,
                (unsigned)shape.register_count_v,
                (unsigned)shape.smem_size_bytes);
    return 0;
}

int slm_oplib_dispatch(struct ga10b_bringup *b,
                       uint64_t inst_block_phys,
                       uint32_t op_kind,
                       uint32_t tier,
                       uint32_t dtype,
                       const struct operator_dispatch_args *args)
{
    if (b == NULL || args == NULL) {
        return -1;
    }

    struct slm_oplib_dispatch_prep prep;
    int rc = slm_oplib_prepare_dispatch(inst_block_phys, op_kind,
                                         tier, dtype, args, &prep);
    if (rc < 0) {
        return rc;
    }

    /* Build a single-element v7 ops array on the stack. The
     * dispatcher's per-op validity check
     * (`ga10b_pipeline_op_is_valid`) requires `qmd_gpu_va != 0` and
     * `output_phys != 0` — those fields are MNIST helper-published
     * sentinels that the v7 dispatch loop body doesn't actually use
     * (the QMD slot is picked by `ga10b_qmd_pool_prepare`; the
     * trailing semaphore is the only completion signal). Set them
     * to a non-zero placeholder that the validity check passes.
     * The QMD pool's GPU VA is always non-zero in v7 mode and is
     * conveniently available — using it makes the placeholder
     * obvious in dispatch logs ("qmd matches pool base"). */
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->qmd_pool_gpu_va == 0) {
        /* Defensive: the inline dispatcher would still fail later
         * on a zero pool VA. Surface here for a clearer error. */
        uart_puts("[oplib-dispatch] qmd_pool_gpu_va == 0 — "
                  "channel handoff missing v7 pool resources\n");
        return -1;
    }
    uint64_t placeholder = h->qmd_pool_gpu_va;

    struct ga10b_pipeline_op_v7 op = {
        .qmd_gpu_va       = placeholder,
        .output_phys      = placeholder,
        .expected_payload = 0,
        .flags            = 0,
        .shader_gpu_va    = prep.shader_gpu_va,
        .cbuf_gpu_va      = prep.cbuf_gpu_va,
        .register_count_v = prep.register_count_v,
        .grid_x           = prep.grid_x,
        .grid_y           = prep.grid_y,
        .grid_z           = prep.grid_z,
        .block_x          = prep.block_x,
        .block_y          = prep.block_y,
        .block_z          = prep.block_z,
        .smem_size_bytes  = prep.smem_size_bytes,
        .slm_size_bytes   = prep.slm_size_bytes,
        .barrier_count    = prep.barrier_count,
    };

    rc = ga10b_dispatch_v7_pipeline_inline(b, &op, 1);
    if (rc < 0) {
        uart_printf("[oplib-dispatch] inline dispatch failed: rc=%d\n", rc);
        return rc;
    }
    uart_printf("[oplib-dispatch] op_kind=%u dispatched + completed\n",
                (unsigned)op_kind);
    return 0;
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

int slm_oplib_prepare_dispatch(uint64_t inst_block_phys,
                                uint32_t op_kind,
                                uint32_t tier,
                                uint32_t dtype,
                                const struct operator_dispatch_args *args,
                                struct slm_oplib_dispatch_prep *out)
{
    (void)inst_block_phys;
    (void)op_kind;
    (void)tier;
    (void)dtype;
    (void)args;
    (void)out;
    /* Non-Jetson platforms have no GA10B GMMU and no SASS pool
     * staging. The dispatcher prep is a no-op stub. */
    return -1;
}

int slm_oplib_dispatch(struct ga10b_bringup *b,
                       uint64_t inst_block_phys,
                       uint32_t op_kind,
                       uint32_t tier,
                       uint32_t dtype,
                       const struct operator_dispatch_args *args)
{
    (void)b;
    (void)inst_block_phys;
    (void)op_kind;
    (void)tier;
    (void)dtype;
    (void)args;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
