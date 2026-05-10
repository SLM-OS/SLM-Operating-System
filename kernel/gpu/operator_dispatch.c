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

/* ============================================================================
 * ROPE (op_kind = 1)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] vec_gpu_va        uint64_t  (in/out)
 *   [0x08] positions_gpu_va  uint64_t
 *   [0x10] cos_sin_gpu_va    uint64_t
 *   [0x18] batch             uint32_t
 *   [0x1C] num_heads         uint32_t
 *   [0x20] head_dim          uint32_t
 *
 * Launch:
 *   grid  = (batch * num_heads, 1, 1)
 *   block = (head_dim/2,        1, 1)
 *
 * Element-wise per pair: one block per (token, head); one thread per
 * (pair_idx ∈ [0, head_dim/2)). No shared memory, no syncthreads,
 * no SLM. See scripts/cuda/rope_f16.cu.
 */

static int rope_build_cbuf(void *cbuf,
                            const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_ROPE) {
        return -1;
    }

    const struct operator_dispatch_args_rope *r = &args->u.rope;

    /* Same defensive shape checks as the rmsnorm builder: refuse to
     * dispatch a kernel that would compute over zero work or read
     * from a NULL pointer. The SASS doesn't validate these — it
     * just reads cbuf words and uses them as addresses. */
    if (r->batch == 0 || r->num_heads == 0 || r->head_dim == 0) {
        return -1;
    }
    /* head_dim must be even — pairs are (vec[2i], vec[2i+1]). The
     * launch_shape divides by 2 to pick block_x; an odd head_dim
     * would silently round down and skip the last element. */
    if ((r->head_dim & 1u) != 0u) {
        return -1;
    }
    if (r->vec_gpu_va == 0 || r->positions_gpu_va == 0 ||
        r->cos_sin_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + ROPE_CBUF_OFFSET_VEC,       &r->vec_gpu_va,
           sizeof(uint64_t));
    memcpy(p + ROPE_CBUF_OFFSET_POSITIONS, &r->positions_gpu_va,
           sizeof(uint64_t));
    memcpy(p + ROPE_CBUF_OFFSET_COS_SIN,   &r->cos_sin_gpu_va,
           sizeof(uint64_t));
    memcpy(p + ROPE_CBUF_OFFSET_BATCH,     &r->batch,     sizeof(uint32_t));
    memcpy(p + ROPE_CBUF_OFFSET_NUM_HEADS, &r->num_heads, sizeof(uint32_t));
    memcpy(p + ROPE_CBUF_OFFSET_HEAD_DIM,  &r->head_dim,  sizeof(uint32_t));

    return 0;
}

static int rope_launch_shape(const struct operator_dispatch_args *args,
                              struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_ROPE) {
        return -1;
    }
    const struct operator_dispatch_args_rope *r = &args->u.rope;
    if (r->batch == 0 || r->num_heads == 0 || r->head_dim == 0) {
        return -1;
    }
    if ((r->head_dim & 1u) != 0u) {
        return -1;
    }

    /* One block per (token, head). The CUDA source maps
     * `blockIdx.x = token_idx * num_heads + head_idx`. */
    out->grid_x = r->batch * r->num_heads;
    out->grid_y = 1;
    out->grid_z = 1;
    /* One thread per pair. head_dim is even (validated above) so
     * head_dim/2 is exact. */
    out->block_x = r->head_dim / 2u;
    out->block_y = 1;
    out->block_z = 1;
    /* register_count_v: 64. Same conservative pick as rmsnorm —
     * RoPE is simpler (no reduction, no shared memory) and uses
     * fewer regs in practice, but keeping the value uniform until
     * the SASS-header parser lands avoids per-op handcraft. */
    out->register_count_v = 64;
    /* No shared memory, no SLM, no barriers. RoPE has no
     * cross-thread communication: each thread reads its own
     * (cos, sin) from the LUT and rotates its own pair in-place.
     * grep'd the .cu source for `__syncthreads` / `__shared__` —
     * 0 hits. */
    out->smem_size_bytes = 0;
    out->slm_size_bytes  = 0;
    out->barrier_count   = 0;
    return 0;
}

/* ============================================================================
 * EMBEDDING (op_kind = 2)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] table_gpu_va     uint64_t  (Q4_K table)
 *   [0x08] out_gpu_va       uint64_t  (FP16 row)
 *   [0x10] token_id         uint32_t
 *   [0x14] embedding_length uint32_t
 *   [0x18] table_row_bytes  uint32_t
 *
 * Launch:
 *   grid  = (n_blocks, 1, 1) where n_blocks = table_row_bytes / 144
 *   block = (256, 1, 1)
 *
 * One CUDA block per Q4_K super-block in the row; one thread per
 * element within the super-block. Padding tail (elem_idx >=
 * embedding_length) skipped without writing — same Q4_K decoding
 * path as q4k_dequant_f16 (#699). See scripts/cuda/embedding_q4k_f16.cu.
 */

static int embedding_build_cbuf(void *cbuf,
                                 const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_EMBEDDING) {
        return -1;
    }

    const struct operator_dispatch_args_embedding *e = &args->u.embedding;

    /* Defensive shape checks: refuse zero work, NULL pointers, and
     * a row layout that doesn't honor the Q4_K super-block geometry.
     * The SASS reads cbuf words verbatim and uses them as addresses
     * + loop bounds, so a bad table_row_bytes would either fault
     * (n_blocks=0 → no dispatch) or read past the table. */
    if (e->embedding_length == 0 || e->table_row_bytes == 0) {
        return -1;
    }
    /* table_row_bytes must be a multiple of EMBEDDING_Q4K_BLOCK_BYTES.
     * The kernel computes `n_blocks = table_row_bytes / 144` and
     * walks every block; a non-multiple would silently truncate the
     * row (drop the tail). */
    if ((e->table_row_bytes % EMBEDDING_Q4K_BLOCK_BYTES) != 0u) {
        return -1;
    }
    /* embedding_length must fit in the dequantized capacity of the
     * row. n_blocks × 256 is the max # of FP16 outputs the row can
     * produce; passing a longer embedding_length would walk past
     * the output buffer.
     *
     * Widen the multiply to uint64_t so a hostile table_row_bytes
     * near UINT32_MAX doesn't wrap n_blocks × 256 past the 32-bit
     * boundary and falsely accept an oversize embedding_length —
     * realistic SLM tables stay far below the wraparound, but this
     * is a memory-safety boundary check on attacker-controllable
     * input. */
    uint32_t n_blocks = e->table_row_bytes / EMBEDDING_Q4K_BLOCK_BYTES;
    if ((uint64_t)e->embedding_length >
        (uint64_t)n_blocks * EMBEDDING_Q4K_BLOCK_ELEMS) {
        return -1;
    }
    if (e->table_gpu_va == 0 || e->out_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + EMBEDDING_CBUF_OFFSET_TABLE,     &e->table_gpu_va,
           sizeof(uint64_t));
    memcpy(p + EMBEDDING_CBUF_OFFSET_OUT,       &e->out_gpu_va,
           sizeof(uint64_t));
    memcpy(p + EMBEDDING_CBUF_OFFSET_TOKEN_ID,  &e->token_id,
           sizeof(uint32_t));
    memcpy(p + EMBEDDING_CBUF_OFFSET_EMBED_LEN, &e->embedding_length,
           sizeof(uint32_t));
    memcpy(p + EMBEDDING_CBUF_OFFSET_ROW_BYTES, &e->table_row_bytes,
           sizeof(uint32_t));

    return 0;
}

static int embedding_launch_shape(const struct operator_dispatch_args *args,
                                    struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_EMBEDDING) {
        return -1;
    }
    const struct operator_dispatch_args_embedding *e = &args->u.embedding;
    if (e->embedding_length == 0 || e->table_row_bytes == 0) {
        return -1;
    }
    if ((e->table_row_bytes % EMBEDDING_Q4K_BLOCK_BYTES) != 0u) {
        return -1;
    }

    /* One CUDA block per super-block in the row. The kernel
     * recomputes n_blocks from table_row_bytes itself; we mirror
     * the formula here so the QMD's grid_x stays in lock-step with
     * the kernel's loop bound. */
    out->grid_x = e->table_row_bytes / EMBEDDING_Q4K_BLOCK_BYTES;
    out->grid_y = 1;
    out->grid_z = 1;
    /* 256 threads per block — one per element within a Q4_K
     * super-block. Fixed by the format. */
    out->block_x = EMBEDDING_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    /* register_count_v: 64. Conservative pick consistent with
     * rmsnorm/rope — the precise REG count from cuobjdump is
     * available but until the SASS-header parser lands all SIMT
     * SLM ops carry the same uniform value. */
    out->register_count_v = 64;
    /* No shared memory, no SLM, no barriers. The kernel reads its
     * row + super-block scales/mins from global memory and writes
     * the corresponding FP16 element directly — no cross-thread
     * communication. grep'd the .cu source for __syncthreads /
     * __shared__: 0 hits. */
    out->smem_size_bytes = 0;
    out->slm_size_bytes  = 0;
    out->barrier_count   = 0;
    return 0;
}

/* ============================================================================
 * Q4K_DEQUANT (op_kind = 12)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] blocks_gpu_va   uint64_t  (Q4_K super-blocks, nb × 144 B)
 *   [0x08] out_gpu_va      uint64_t  (FP16 output, nb × 256)
 *   [0x10] nb              uint32_t  (number of super-blocks)
 *
 * Launch:
 *   grid  = (nb, 1, 1)
 *   block = (256, 1, 1)
 *
 * Same Q4_K decoding path as EMBEDDING but without the per-token
 * offset or padding tail — every dequantized FP16 element is
 * written. No shared memory, no syncthreads.
 * See scripts/cuda/q4k_dequant_f16.cu.
 */

static int q4k_dequant_build_cbuf(void *cbuf,
                                    const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_Q4K_DEQUANT) {
        return -1;
    }
    const struct operator_dispatch_args_q4k_dequant *q = &args->u.q4k_dequant;
    if (q->nb == 0) {
        return -1;
    }
    if (q->blocks_gpu_va == 0 || q->out_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + Q4K_DEQUANT_CBUF_OFFSET_BLOCKS, &q->blocks_gpu_va,
           sizeof(uint64_t));
    memcpy(p + Q4K_DEQUANT_CBUF_OFFSET_OUT,    &q->out_gpu_va,
           sizeof(uint64_t));
    memcpy(p + Q4K_DEQUANT_CBUF_OFFSET_NB,     &q->nb,
           sizeof(uint32_t));

    return 0;
}

static int q4k_dequant_launch_shape(const struct operator_dispatch_args *args,
                                      struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_Q4K_DEQUANT) {
        return -1;
    }
    const struct operator_dispatch_args_q4k_dequant *q = &args->u.q4k_dequant;
    if (q->nb == 0) {
        return -1;
    }

    out->grid_x = q->nb;
    out->grid_y = 1;
    out->grid_z = 1;
    out->block_x = Q4K_DEQUANT_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    out->register_count_v = 64;
    out->smem_size_bytes = 0;
    out->slm_size_bytes  = 0;
    out->barrier_count   = 0;
    return 0;
}

/* ============================================================================
 * SWIGLU (op_kind = 6)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] gate_gpu_va   uint64_t  (post-gate-projection FP16)
 *   [0x08] up_gpu_va     uint64_t  (post-up-projection FP16)
 *   [0x10] out_gpu_va    uint64_t  (silu(gate) * up FP16)
 *   [0x18] n             uint32_t  (total element count)
 *
 * Launch:
 *   grid  = (ceil(n/256), 1, 1)
 *   block = (256, 1, 1)
 *
 * Element-wise fused SiLU + multiply, the activation between the
 * gate/up matmuls and the down matmul in the SwiGLU MLP block.
 * One thread per output FP16 element. No shared memory, no
 * syncthreads. Trailing threads in the last block whose `idx >= n`
 * return without writing. See scripts/cuda/swiglu_f16.cu.
 */

static int swiglu_build_cbuf(void *cbuf,
                              const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_SWIGLU) {
        return -1;
    }
    const struct operator_dispatch_args_swiglu *s = &args->u.swiglu;
    if (s->n == 0) {
        return -1;
    }
    /* Cap n so the ceil-div in `swiglu_launch_shape` can't wrap
     * past UINT32_MAX. Realistic Qwen2.5 prefill tops out at
     * ~16 rows × 8960 = 143 360, so this is purely defensive
     * against pathological / hostile inputs. */
    if (s->n > UINT32_MAX - (SWIGLU_BLOCK_DIM - 1u)) {
        return -1;
    }
    if (s->gate_gpu_va == 0 || s->up_gpu_va == 0 || s->out_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + SWIGLU_CBUF_OFFSET_GATE, &s->gate_gpu_va,
           sizeof(uint64_t));
    memcpy(p + SWIGLU_CBUF_OFFSET_UP,   &s->up_gpu_va,
           sizeof(uint64_t));
    memcpy(p + SWIGLU_CBUF_OFFSET_OUT,  &s->out_gpu_va,
           sizeof(uint64_t));
    memcpy(p + SWIGLU_CBUF_OFFSET_N,    &s->n,
           sizeof(uint32_t));

    return 0;
}

static int swiglu_launch_shape(const struct operator_dispatch_args *args,
                                struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_SWIGLU) {
        return -1;
    }
    const struct operator_dispatch_args_swiglu *s = &args->u.swiglu;
    if (s->n == 0) {
        return -1;
    }
    /* Same overflow guard as swiglu_build_cbuf — keep both paths
     * in sync so a caller that bypassed build_cbuf can't slip a
     * UINT32_MAX-adjacent n past the ceil-div. */
    if (s->n > UINT32_MAX - (SWIGLU_BLOCK_DIM - 1u)) {
        return -1;
    }

    /* One thread per element, 256 threads per block. The last block
     * may be partial — trailing threads with `idx >= n` return
     * without writing. */
    out->grid_x = (s->n + SWIGLU_BLOCK_DIM - 1u) / SWIGLU_BLOCK_DIM;
    out->grid_y = 1;
    out->grid_z = 1;
    out->block_x = SWIGLU_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    out->register_count_v = 64;
    out->smem_size_bytes = 0;
    out->slm_size_bytes  = 0;
    out->barrier_count   = 0;
    return 0;
}

/* ============================================================================
 * Q4K_DOT (op_kind = 3)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] x_gpu_va        uint64_t  (FP16 input activations [K])
 *   [0x08] weights_gpu_va  uint64_t  (Q4_K-packed weights [N × K × 144/256])
 *   [0x10] out_gpu_va      uint64_t  (FP32 output [N])
 *   [0x18] K               uint32_t  (input dim, multiple of 256)
 *   [0x1C] N               uint32_t  (output dim)
 *
 * Launch:
 *   grid  = (N, 1, 1)
 *   block = (256, 1, 1)
 *   smem  = 1024 B  (partial-sum tree reduction)
 *   barrier_count = 1  (kernel uses __syncthreads() — Volta+ BSSY/BSYNC
 *                       per #747 fix; under-allocation is a quiet
 *                       illegal-instruction hang)
 *
 * The "decode" matmul: one activation row × Q4_K-quantized weight
 * matrix → one output row. Drives Q/K/V/O projections, gate/up/down
 * matmuls, and the LM head. See scripts/cuda/q4k_dot_f16.cu.
 */

static int q4k_dot_build_cbuf(void *cbuf,
                                const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_Q4K_DOT) {
        return -1;
    }
    const struct operator_dispatch_args_q4k_dot *q = &args->u.q4k_dot;
    if (q->k == 0 || q->n == 0) {
        return -1;
    }
    /* K must be a multiple of 256 — every row is a whole number of
     * Q4_K super-blocks. The kernel walks K/256 super-blocks per
     * row; a non-multiple would leave a partial super-block at the
     * tail that the kernel can't decode (the 144 B layout requires
     * exactly 256 elements). */
    if ((q->k % EMBEDDING_Q4K_BLOCK_ELEMS) != 0u) {
        return -1;
    }
    if (q->x_gpu_va == 0 || q->weights_gpu_va == 0 ||
        q->out_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + Q4K_DOT_CBUF_OFFSET_X,       &q->x_gpu_va,
           sizeof(uint64_t));
    memcpy(p + Q4K_DOT_CBUF_OFFSET_WEIGHTS, &q->weights_gpu_va,
           sizeof(uint64_t));
    memcpy(p + Q4K_DOT_CBUF_OFFSET_OUT,     &q->out_gpu_va,
           sizeof(uint64_t));
    memcpy(p + Q4K_DOT_CBUF_OFFSET_K,       &q->k,
           sizeof(uint32_t));
    memcpy(p + Q4K_DOT_CBUF_OFFSET_N,       &q->n,
           sizeof(uint32_t));

    return 0;
}

static int q4k_dot_launch_shape(const struct operator_dispatch_args *args,
                                  struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_Q4K_DOT) {
        return -1;
    }
    const struct operator_dispatch_args_q4k_dot *q = &args->u.q4k_dot;
    if (q->k == 0 || q->n == 0) {
        return -1;
    }
    if ((q->k % EMBEDDING_Q4K_BLOCK_ELEMS) != 0u) {
        return -1;
    }

    out->grid_x = q->n;
    out->grid_y = 1;
    out->grid_z = 1;
    out->block_x = Q4K_DOT_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    out->register_count_v = 64;
    out->smem_size_bytes = Q4K_DOT_SMEM_BYTES;
    out->slm_size_bytes  = 0;
    /* `__syncthreads()` in the tree reduction — needs a barrier
     * slot. See PR #747: BARRIER_COUNT < 1 lets BSSY hit
     * illegal-instruction at the first sync. */
    out->barrier_count   = 1;
    return 0;
}

/* ============================================================================
 * GQA_ATTN (op_kind = 5)
 * ============================================================================
 *
 * cbuf[0] layout (relative to OPERATOR_CBUF0_BASE):
 *   [0x00] q_gpu_va     uint64_t  ([n_head_q × head_dim] FP16 queries)
 *   [0x08] k_gpu_va     uint64_t  ([seq_len × n_head_kv × head_dim] FP16 K)
 *   [0x10] v_gpu_va     uint64_t  ([seq_len × n_head_kv × head_dim] FP16 V)
 *   [0x18] out_gpu_va   uint64_t  ([n_head_q × head_dim] FP16 output)
 *   [0x20] n_head_q     uint32_t
 *   [0x24] n_head_kv    uint32_t
 *   [0x28] head_dim     uint32_t
 *   [0x2C] seq_len      uint32_t
 *
 * Launch:
 *   grid  = (n_head_q, 1, 1)
 *   block = (128, 1, 1)
 *   smem  = 17928 B  (q_cache + logits + reduce_buf + 2 broadcast scalars)
 *   barrier_count = 1  (kernel uses __syncthreads() between phases)
 *
 * Three-phase fused decode-step attention: logits → softmax →
 * weighted V-sum. Causal mask is implicit — `seq_len` is the count
 * of past+current positions; future positions aren't passed in.
 * GQA pairing: `group = n_head_q / n_head_kv` query heads share one
 * KV head; n_head_q must be a multiple of n_head_kv. Mirrors
 * `runtime/src/inference/ops_transformer.rs::gqa_decode_step`.
 * See scripts/cuda/gqa_attn_f16.cu.
 */

static int gqa_attn_build_cbuf(void *cbuf,
                                 const struct operator_dispatch_args *args)
{
    if (cbuf == NULL || args == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_GQA_ATTN) {
        return -1;
    }
    const struct operator_dispatch_args_gqa_attn *g = &args->u.gqa_attn;

    if (g->n_head_q == 0 || g->n_head_kv == 0 ||
        g->head_dim == 0 || g->seq_len == 0) {
        return -1;
    }
    /* GQA grouping: every `group = n_head_q / n_head_kv` query heads
     * share one KV head. The kernel computes `hkv = hq / group`, so
     * a non-divisible ratio would map some Q heads to the wrong KV
     * head silently. */
    if ((g->n_head_q % g->n_head_kv) != 0u) {
        return -1;
    }
    /* Static smem caps from the kernel. Exceeding either would
     * overrun q_cache[256] or logits[4096] and read garbage from
     * outside the static __shared__ region. */
    if (g->head_dim > GQA_ATTN_MAX_HEAD_DIM) {
        return -1;
    }
    if (g->seq_len > GQA_ATTN_MAX_SEQ_LEN) {
        return -1;
    }
    if (g->q_gpu_va == 0 || g->k_gpu_va == 0 ||
        g->v_gpu_va == 0 || g->out_gpu_va == 0) {
        return -1;
    }

    uint8_t *p = (uint8_t *)cbuf + OPERATOR_CBUF0_BASE;

    memcpy(p + GQA_ATTN_CBUF_OFFSET_Q,         &g->q_gpu_va,
           sizeof(uint64_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_K,         &g->k_gpu_va,
           sizeof(uint64_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_V,         &g->v_gpu_va,
           sizeof(uint64_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_OUT,       &g->out_gpu_va,
           sizeof(uint64_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_N_HEAD_Q,  &g->n_head_q,
           sizeof(uint32_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_N_HEAD_KV, &g->n_head_kv,
           sizeof(uint32_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_HEAD_DIM,  &g->head_dim,
           sizeof(uint32_t));
    memcpy(p + GQA_ATTN_CBUF_OFFSET_SEQ_LEN,   &g->seq_len,
           sizeof(uint32_t));

    return 0;
}

static int gqa_attn_launch_shape(const struct operator_dispatch_args *args,
                                   struct operator_launch_shape *out)
{
    if (args == NULL || out == NULL) {
        return -1;
    }
    if (args->op_kind != SLM_GPU_OP_GQA_ATTN) {
        return -1;
    }
    const struct operator_dispatch_args_gqa_attn *g = &args->u.gqa_attn;
    if (g->n_head_q == 0 || g->n_head_kv == 0 ||
        g->head_dim == 0 || g->seq_len == 0) {
        return -1;
    }
    if ((g->n_head_q % g->n_head_kv) != 0u) {
        return -1;
    }
    if (g->head_dim > GQA_ATTN_MAX_HEAD_DIM ||
        g->seq_len > GQA_ATTN_MAX_SEQ_LEN) {
        return -1;
    }

    out->grid_x = g->n_head_q;
    out->grid_y = 1;
    out->grid_z = 1;
    out->block_x = GQA_ATTN_BLOCK_DIM;
    out->block_y = 1;
    out->block_z = 1;
    out->register_count_v = 64;
    /* Static smem footprint: independent of runtime args because the
     * kernel declares fixed-size __shared__ arrays sized to MAX_HEAD_DIM
     * and MAX_SEQ_LEN. The actual Q head's working set is smaller for
     * shorter contexts but the QMD must reserve the full static size. */
    out->smem_size_bytes = GQA_ATTN_SMEM_BYTES;
    out->slm_size_bytes  = 0;
    /* Multiple `__syncthreads()` between phases — barrier slot
     * required (#747). */
    out->barrier_count   = 1;
    return 0;
}

/* Per-op metadata registry. Linear scan in
 * `operator_dispatch_get_metadata` — N is small (seven entries; the
 * full SLM-on-GPU op set per #714). A direct-indexed table is
 * overkill at this size. */
static const struct operator_dispatch_metadata g_metadata[] = {
    {
        .op_kind      = SLM_GPU_OP_RMSNORM,
        .build_cbuf   = rmsnorm_build_cbuf,
        .launch_shape = rmsnorm_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_ROPE,
        .build_cbuf   = rope_build_cbuf,
        .launch_shape = rope_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_EMBEDDING,
        .build_cbuf   = embedding_build_cbuf,
        .launch_shape = embedding_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_Q4K_DEQUANT,
        .build_cbuf   = q4k_dequant_build_cbuf,
        .launch_shape = q4k_dequant_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_SWIGLU,
        .build_cbuf   = swiglu_build_cbuf,
        .launch_shape = swiglu_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_Q4K_DOT,
        .build_cbuf   = q4k_dot_build_cbuf,
        .launch_shape = q4k_dot_launch_shape,
    },
    {
        .op_kind      = SLM_GPU_OP_GQA_ATTN,
        .build_cbuf   = gqa_attn_build_cbuf,
        .launch_shape = gqa_attn_launch_shape,
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
