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
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

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

    /* 2. Allocate a 4 KB cbuf page in the channel's GMMU. The
     *    cbuf is read by the SASS via cbuf[0] reads at offsets
     *    0x160 onward — see operator_dispatch.h::OPERATOR_CBUF0_BASE.
     *    Caller-supplied flags=0 → no RO bit (the cbuf is read-only
     *    from the GPU's perspective anyway, but leaving FLAG_RO off
     *    matches the existing MNIST cbuf which the helper allocates
     *    without RO too). */
    uint64_t cbuf_va = 0;
    uint64_t cbuf_phys = 0;
    void    *cbuf_cpu = NULL;
    rc = ga10b_gmmu_alloc(inst_block_phys, 1u, 0u,
                          &cbuf_va, &cbuf_cpu, &cbuf_phys);
    if (rc < 0) {
        uart_printf("[oplib-dispatch] cbuf alloc failed: rc=%d\n", rc);
        return -1;
    }

    /* Zero the cbuf page so any unwritten bytes stay deterministic. */
    {
        volatile uint8_t *p = (volatile uint8_t *)cbuf_cpu;
        for (size_t i = 0; i < 4096; i++) {
            p[i] = 0;
        }
    }

    /* 3. Populate cbuf via the registered builder. Builder writes
     *    at `cbuf_cpu + OPERATOR_CBUF0_BASE` for the op's arg
     *    layout. Verifies args->op_kind matches op_kind here. */
    if (args->op_kind != op_kind) {
        uart_printf("[oplib-dispatch] args->op_kind=%u != op_kind=%u\n",
                    (unsigned)args->op_kind, (unsigned)op_kind);
        return -1;
    }
    rc = operator_dispatch_build_cbuf(cbuf_cpu, args);
    if (rc != 0) {
        uart_printf("[oplib-dispatch] cbuf build failed: rc=%d\n", rc);
        return -1;
    }
    cache_clean_range(cbuf_cpu, 4096);

    /* 4. Compute the launch shape (grid/block/regs/smem/...). */
    struct operator_launch_shape shape;
    rc = operator_dispatch_launch_shape(args, &shape);
    if (rc != 0) {
        uart_printf("[oplib-dispatch] launch_shape failed: rc=%d\n", rc);
        return -1;
    }

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

#endif /* PLATFORM_JETSON_ORIN_NANO */
