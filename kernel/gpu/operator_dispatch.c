/*
 * operator_dispatch.c - Per-op dispatch metadata registry (#714, A.2).
 *
 * Owns the static table mapping op_kind to (cbuf-builder, launch-shape)
 * function pointers. Each op_kind that the dispatcher knows how to
 * fire has one row in `g_metadata`. RmsNorm is the first entry,
 * registered in this PR; the remaining six SLM kernels (EMBEDDING,
 * Q4K_DOT, ROPE, SWIGLU, GQA_ATTN, Q4K_DEQUANT) land in the B
 * follow-on (#714) once the actual pushbuffer-firing dispatcher is
 * wired up.
 *
 * The cbuf builder + launch-shape functions here are pure logic
 * (no MMIO, no PMM, no GPU touch) so they're QEMU-testable in
 * `kernel/tests/test_operator_dispatch.c`.
 */

#include "operator_dispatch.h"

#include "gpu_handoff.h"     /* SLM_GPU_OP_RMSNORM */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * RMSNORM (op_kind = 0)
 * ============================================================================
 *
 * cbuf[0] layout (relative to cbuf[0]'s start at OPERATOR_CBUF0_BASE):
 *   [0x00] x_gpu_va     uint64_t
 *   [0x08] gamma_gpu_va uint64_t
 *   [0x10] out_gpu_va   uint64_t
 *   [0x18] n            uint32_t
 *   [0x1C] eps          float
 *
 * Launch:
 *   grid  = (n_rows, 1, 1)
 *   block = (256,    1, 1)
 *
 * The kernel is one CTA per row; threads stride the row width
 * computing partial sum-of-squares, tree-reduce, then strided
 * write. See scripts/cuda/rmsnorm_f16.cu for the full algorithm.
 */

static int rmsnorm_build_cbuf(void *cbuf,
                               const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_RMSNORM) {
        return -1;
    }

    const struct operator_dispatch_args_rmsnorm *r = &args->u.rmsnorm;

    /* Pin reasonable runtime ranges so a malformed args struct
     * doesn't produce a kernel that wedges the GPU on a 0-byte
     * cbuf write. n must be > 0; n_rows must be > 0 (zero-grid
     * launches are well-defined but not useful here). */
    if (r->n == 0 || r->n_rows == 0) {
        return -1;
    }
    if (r->x_gpu_va == 0 || r->gamma_gpu_va == 0 || r->out_gpu_va == 0) {
        return -1;
    }

    /* Compute the cbuf write target. Caller guarantees `cbuf` is the
     * kernel-VA of a 4 KB cbuf page; we add OPERATOR_CBUF0_BASE
     * (= 0x160) to land on the parameter-area start. */
    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    /* Use memcpy so the writes are aligned-friendly under -O2.
     * The 8-byte fields are u64; the 4-byte n / eps are u32. */
    memcpy(p + RMSNORM_CBUF_OFFSET_X,     &r->x_gpu_va,     sizeof(uint64_t));
    memcpy(p + RMSNORM_CBUF_OFFSET_GAMMA, &r->gamma_gpu_va, sizeof(uint64_t));
    memcpy(p + RMSNORM_CBUF_OFFSET_OUT,   &r->out_gpu_va,   sizeof(uint64_t));
    memcpy(p + RMSNORM_CBUF_OFFSET_N,     &r->n,            sizeof(uint32_t));
    /* eps_bits is the IEEE 754 FP32 bit pattern; the GPU reads these
     * 4 bytes back as a `float`. */
    memcpy(p + RMSNORM_CBUF_OFFSET_EPS,   &r->eps_bits,     sizeof(uint32_t));

    return 0;
}

static int rmsnorm_launch_shape(const struct operator_dispatch_args *args,
                                 struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_RMSNORM) {
        return -1;
    }
    const struct operator_dispatch_args_rmsnorm *r = &args->u.rmsnorm;
    if (r->n_rows == 0) {
        return -1;
    }

    out->grid_x = r->n_rows;
    out->grid_y = 1;
    out->grid_z = 1;
    out->block_x = RMSNORM_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    /* register_count_v: 64.
     *
     * History: 32 → 64 (PR #744) made dispatch #1 fire
     *          end-to-end; 64 → 128 (this branch) was tested as
     *          a candidate fix for the repeat-dispatch hang and
     *          made no difference. The SM's `illegal_instr_param`
     *          trap fires on multiple warps regardless of
     *          register count; the actual SASS's register usage
     *          is in its `.nv.info` section, but the trap is
     *          rooted in something we're feeding the QMD/launch
     *          setup that the SM rejects, not a too-low reg
     *          count. Reverting to 64 as the known-working
     *          baseline pending a SASS-header parser and a
     *          systematic comparison against a working CUDA
     *          invocation's QMD bytes. */
    out->register_count_v = 64;
    /* RmsNorm uses BLOCK_DIM (256) × float for the partial-sum
     * reduction buffer (1024 B) plus a single float for the
     * broadcast rms_inv (4 B). Round up to 2 KB to leave headroom
     * for any future minor change to the kernel's shared layout
     * without re-deriving the size. SASS-header-driven smem sizing
     * is tracked in the same #714 follow-on as register_count_v. */
    out->smem_size_bytes = 2048;
    out->slm_size_bytes  = 0;
    /* The rmsnorm kernel uses __syncthreads() between the
     * partial-sum write and the tree-reduction read. nvcc on
     * Ampere compiles __syncthreads() to BSSY/BSYNC (the
     * Volta+ ITS-aware barrier intrinsic, replacing pre-Volta
     * BAR.SYNC), which requires the QMD's BARRIER_COUNT field
     * to reserve at least one barrier slot. With
     * BARRIER_COUNT=0 the SM traps the BSSY with
     * `illegal_instr_param` (warp_esr error 0x0b) on every warp
     * that reaches the sync — the trap latches sticky exception
     * state in gr_exception.gpc that blocks all subsequent
     * COMPUTE_B method submissions on the channel. Diagnosed
     * via the per-GPC exception probe: trapping PC was
     * SASS_base + 0xb80, where the SASS bytes start with
     * `1d 7b 00 00` matching the Ampere BSYNC opcode encoding.
     *
     * Each `__syncthreads()` requires one barrier slot. The
     * rmsnorm kernel has a single sync point, so 1 suffices.
     * gpu-kernel-mnist.c sets 3 for its HMMA WMMA shaders
     * (load + mma_sync + store barriers). */
    out->barrier_count   = 1;
    return 0;
}

/* Per-op metadata registry. Linear scan in
 * `operator_dispatch_get_metadata` — N is small (one entry today,
 * grows to seven once #714 lands all the SLM kernels), so a
 * direct-indexed table is overkill. */
static const struct operator_dispatch_metadata g_metadata[] = {
    {
        .op_kind      = SLM_GPU_OP_RMSNORM,
        .build_cbuf   = rmsnorm_build_cbuf,
        .launch_shape = rmsnorm_launch_shape,
    },
};

#define G_METADATA_COUNT \
    (sizeof(g_metadata) / sizeof(g_metadata[0]))

const struct operator_dispatch_metadata *
operator_dispatch_get_metadata(uint32_t op_kind)
{
    for (size_t i = 0; i < G_METADATA_COUNT; i++) {
        if (g_metadata[i].op_kind == op_kind) {
            return &g_metadata[i];
        }
    }
    return NULL;
}

int operator_dispatch_build_cbuf(void *cbuf,
                                  const struct operator_dispatch_args *args)
{
    if (args == NULL) {
        return -1;
    }
    const struct operator_dispatch_metadata *meta =
        operator_dispatch_get_metadata(args->op_kind);
    if (meta == NULL || meta->build_cbuf == NULL) {
        return -1;
    }
    return meta->build_cbuf(cbuf, args);
}

int operator_dispatch_launch_shape(const struct operator_dispatch_args *args,
                                    struct operator_launch_shape *out)
{
    if (args == NULL) {
        return -1;
    }
    const struct operator_dispatch_metadata *meta =
        operator_dispatch_get_metadata(args->op_kind);
    if (meta == NULL || meta->launch_shape == NULL) {
        return -1;
    }
    return meta->launch_shape(args, out);
}
