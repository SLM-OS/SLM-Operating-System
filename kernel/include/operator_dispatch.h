/*
 * operator_dispatch.h - Per-op dispatch metadata registry (#714, A.2).
 *
 * Each (op_kind, tier, dtype) triple registered in the operator
 * library has two pieces of dispatch metadata that the eventual
 * pushbuffer-launching dispatcher needs:
 *
 *   1. cbuf-args builder — fills the GPU constant-buffer-0 region
 *      starting at offset 0x160 with the op-specific arguments
 *      (input pointer, output pointer, scalars). The cbuf layout
 *      for each op is documented in the corresponding scripts/cuda/
 *      *.cu header comment (the "cbuf[0] layout" block).
 *
 *   2. launch-shape function — given the op's runtime parameters
 *      (e.g., n_rows for RmsNorm), returns the QMD's grid_x/y/z and
 *      block_x/y/z values. The kernels' launch-shape conventions
 *      are also documented in their .cu header comments.
 *
 * This header defines the registry interface and a lightweight
 * per-op metadata struct. The dispatcher (separate follow-on PR)
 * looks up the metadata by op_kind, calls the cbuf builder against
 * a GMMU-allocated cbuf page, calls the launch-shape function to
 * fill the QMD's grid/block fields, then submits a v7 op via the
 * existing GA10B pushbuffer machinery.
 *
 * Per-op cbuf layouts (offsets in cbuf[0]) are pinned in this
 * header as `*_CBUF_OFFSET_*` macros so a future kernel rebuild
 * that changes the offset gets caught by `_Static_assert` in
 * `kernel/tests/test_operator_dispatch.c`.
 *
 * QMD ABI quick reference (from scripts/cuda/<op>.cu headers and
 * NVK's nv_push.h documentation):
 *
 *   Constant buffer 0 begins at byte offset 0x160 in the cbuf
 *   page; the launcher passes scalars and pointers as if they were
 *   __global__ kernel arguments. 8-byte alignment per slot. Strings
 *   that begin with `[0xXXX]` in this file or the .cu headers
 *   refer to byte offsets within cbuf[0]'s start (i.e., add 0x160
 *   when computing offsets within the cbuf page).
 */

#ifndef OPERATOR_DISPATCH_H
#define OPERATOR_DISPATCH_H

#include <stddef.h>
#include <stdint.h>

/* Constant-buffer-0 base offset within the cbuf page. CUDA's
 * compiled SASS expects parameters to begin here on sm_87 — see
 * the header comment of each scripts/cuda/<op>.cu source for the
 * full ABI rationale. */
#define OPERATOR_CBUF0_BASE          0x160u

/* Per-op cbuf-arg byte offsets (relative to OPERATOR_CBUF0_BASE).
 * These are duplicates of the layout block in each .cu source — pin
 * them here so a future rebuild that changes the SASS arg layout
 * fails compilation if the kernel ABI drifts. Each `*_OFFSET_*` is
 * a byte offset within cbuf[0]. */

/* RMSNORM (scripts/cuda/rmsnorm_f16.cu):
 *   [0x160 + 0x00] x      const half *
 *   [0x160 + 0x08] gamma  const half *
 *   [0x160 + 0x10] out    half *
 *   [0x160 + 0x18] n      int       (row width, e.g. 1536 for Qwen2.5)
 *   [0x160 + 0x1C] eps    float     (1e-6 for Qwen2.5)
 * Launch shape: grid=(n_rows, 1, 1) block=(256, 1, 1). */
#define RMSNORM_CBUF_OFFSET_X        0x00u
#define RMSNORM_CBUF_OFFSET_GAMMA    0x08u
#define RMSNORM_CBUF_OFFSET_OUT      0x10u
#define RMSNORM_CBUF_OFFSET_N        0x18u
#define RMSNORM_CBUF_OFFSET_EPS      0x1Cu
#define RMSNORM_BLOCK_DIM            256u

/* ROPE (scripts/cuda/rope_f16.cu):
 *   [0x160 + 0x00] vec        half *           (in/out)
 *   [0x160 + 0x08] positions  const int *
 *   [0x160 + 0x10] cos_sin    const float *
 *   [0x160 + 0x18] batch      int
 *   [0x160 + 0x1C] num_heads  int
 *   [0x160 + 0x20] head_dim   int
 * Launch shape: grid=(batch * num_heads, 1, 1) block=(head_dim/2, 1, 1).
 * One block per (token, head); one thread per pair within head_dim.
 * Element-wise: no shared memory, no syncthreads, no SLM. */
#define ROPE_CBUF_OFFSET_VEC         0x00u
#define ROPE_CBUF_OFFSET_POSITIONS   0x08u
#define ROPE_CBUF_OFFSET_COS_SIN     0x10u
#define ROPE_CBUF_OFFSET_BATCH       0x18u
#define ROPE_CBUF_OFFSET_NUM_HEADS   0x1Cu
#define ROPE_CBUF_OFFSET_HEAD_DIM    0x20u

/* EMBEDDING (scripts/cuda/embedding_q4k_f16.cu):
 *   [0x160 + 0x00] table_bytes      const uint8_t *  [n_tokens × table_row_bytes]
 *   [0x160 + 0x08] out              half *           [embedding_length]
 *   [0x160 + 0x10] token_id         int              [0, n_tokens)
 *   [0x160 + 0x14] embedding_length int              FP16 outputs to write
 *   [0x160 + 0x18] table_row_bytes  int              bytes per token row
 * Launch shape: grid=(n_blocks, 1, 1) block=(256, 1, 1) where
 *               n_blocks = table_row_bytes / EMBEDDING_Q4K_BLOCK_BYTES.
 * One CUDA block per Q4_K super-block in the row; one thread per element
 * within the super-block. Padding tail (elem_idx >= embedding_length)
 * skipped without writing. No shared memory, no syncthreads, no SLM. */
#define EMBEDDING_CBUF_OFFSET_TABLE        0x00u
#define EMBEDDING_CBUF_OFFSET_OUT          0x08u
#define EMBEDDING_CBUF_OFFSET_TOKEN_ID     0x10u
#define EMBEDDING_CBUF_OFFSET_EMBED_LEN    0x14u
#define EMBEDDING_CBUF_OFFSET_ROW_BYTES    0x18u
#define EMBEDDING_BLOCK_DIM                256u
/* Q4_K super-block geometry — fixed by the GGUF Q4_K format
 * (see runtime/src/inference/quant.rs::Q4K_BLOCK_SIZE). */
#define EMBEDDING_Q4K_BLOCK_BYTES          144u
#define EMBEDDING_Q4K_BLOCK_ELEMS          256u

/* Q4K_DEQUANT (scripts/cuda/q4k_dequant_f16.cu):
 *   [0x160 + 0x00] blocks  const uint8_t *  Q4_K super-blocks
 *   [0x160 + 0x08] out     half *           [256 × nb] FP16 output
 *   [0x160 + 0x10] nb      int              number of super-blocks
 * Launch shape: grid=(nb, 1, 1) block=(256, 1, 1). One CUDA block per
 * super-block; same Q4_K decode path as EMBEDDING but without the
 * per-token offset / padding tail. No shared memory, no syncthreads. */
#define Q4K_DEQUANT_CBUF_OFFSET_BLOCKS     0x00u
#define Q4K_DEQUANT_CBUF_OFFSET_OUT        0x08u
#define Q4K_DEQUANT_CBUF_OFFSET_NB         0x10u
#define Q4K_DEQUANT_BLOCK_DIM              256u

/* SWIGLU (scripts/cuda/swiglu_f16.cu):
 *   [0x160 + 0x00] gate const half *  [n] post-gate-projection FP16
 *   [0x160 + 0x08] up   const half *  [n] post-up-projection FP16
 *   [0x160 + 0x10] out  half *        [n] silu(gate) * up
 *   [0x160 + 0x18] n    int           total element count (rows × intermediate)
 * Launch shape: grid=(ceil(n/256), 1, 1) block=(256, 1, 1). One thread
 * per output FP16 element. Element-wise pure; no shared, no syncthreads. */
#define SWIGLU_CBUF_OFFSET_GATE            0x00u
#define SWIGLU_CBUF_OFFSET_UP              0x08u
#define SWIGLU_CBUF_OFFSET_OUT             0x10u
#define SWIGLU_CBUF_OFFSET_N               0x18u
#define SWIGLU_BLOCK_DIM                   256u

/* Q4K_DOT (scripts/cuda/q4k_dot_f16.cu):
 *   [0x160 + 0x00] x       const half *     [K] FP16 activations
 *   [0x160 + 0x08] weights const uint8_t *  [N × K × 144/256] Q4_K-packed
 *   [0x160 + 0x10] out     float *          [N] FP32 dot products
 *   [0x160 + 0x18] K       int              input dim, multiple of 256
 *   [0x160 + 0x1C] N       int              output dim
 * Launch shape: grid=(N, 1, 1) block=(256, 1, 1). One CUDA block per
 * output row; 256 threads per block, each walking K/256 super-blocks
 * of the row. Uses 1024 B shared memory + a tree reduction with
 * `__syncthreads()` — barrier_count = 1 (Volta+ ITS-aware barriers,
 * #747 fix). */
#define Q4K_DOT_CBUF_OFFSET_X              0x00u
#define Q4K_DOT_CBUF_OFFSET_WEIGHTS        0x08u
#define Q4K_DOT_CBUF_OFFSET_OUT            0x10u
#define Q4K_DOT_CBUF_OFFSET_K              0x18u
#define Q4K_DOT_CBUF_OFFSET_N              0x1Cu
#define Q4K_DOT_BLOCK_DIM                  256u
/* Shared memory: BLOCK_DIM × sizeof(float) = 1024 B for the
 * partial-sum tree reduction. */
#define Q4K_DOT_SMEM_BYTES                 1024u
/* K must be a multiple of EMBEDDING_Q4K_BLOCK_ELEMS (256) — every
 * row is a whole number of Q4_K super-blocks. */

/* GQA_ATTN (scripts/cuda/gqa_attn_f16.cu):
 *   [0x160 + 0x00] q         const half *   [n_head_q  × head_dim]
 *   [0x160 + 0x08] k         const half *   [seq_len × n_head_kv × head_dim]
 *   [0x160 + 0x10] v         const half *   [seq_len × n_head_kv × head_dim]
 *   [0x160 + 0x18] out       half *         [n_head_q × head_dim]
 *   [0x160 + 0x20] n_head_q  int
 *   [0x160 + 0x24] n_head_kv int
 *   [0x160 + 0x28] head_dim  int
 *   [0x160 + 0x2C] seq_len   int
 * Launch shape: grid=(n_head_q, 1, 1) block=(128, 1, 1). One CUDA
 * block per query head; threads cooperate over (seq_len, head_dim).
 * Static shared memory: q_cache[256] + logits[4096] + reduce_buf[128]
 * + 2 broadcast scalars = 17928 B. Three-phase fused logits/softmax/
 * weighted-sum kernel; uses `__syncthreads()` between phases →
 * barrier_count = 1. */
#define GQA_ATTN_CBUF_OFFSET_Q             0x00u
#define GQA_ATTN_CBUF_OFFSET_K             0x08u
#define GQA_ATTN_CBUF_OFFSET_V             0x10u
#define GQA_ATTN_CBUF_OFFSET_OUT           0x18u
#define GQA_ATTN_CBUF_OFFSET_N_HEAD_Q      0x20u
#define GQA_ATTN_CBUF_OFFSET_N_HEAD_KV     0x24u
#define GQA_ATTN_CBUF_OFFSET_HEAD_DIM      0x28u
#define GQA_ATTN_CBUF_OFFSET_SEQ_LEN       0x2Cu
#define GQA_ATTN_BLOCK_DIM                 128u
/* Static shared-memory caps from the kernel source. Exceeding these
 * would overrun the static __shared__ arrays (q_cache[MAX_HEAD_DIM],
 * logits[MAX_SEQ_LEN]) and read past valid scratch. */
#define GQA_ATTN_MAX_HEAD_DIM              256u
#define GQA_ATTN_MAX_SEQ_LEN               4096u
/* q_cache[256]×4 + logits[4096]×4 + reduce_buf[128]×4 + 2 floats. */
#define GQA_ATTN_SMEM_BYTES                17928u
/* Pin the smem footprint formula at compile time so a future tweak
 * to MAX_HEAD_DIM / MAX_SEQ_LEN / BLOCK_DIM that doesn't update
 * GQA_ATTN_SMEM_BYTES breaks the build instead of silently under-
 * allocating shared memory in the QMD. */
_Static_assert(GQA_ATTN_SMEM_BYTES ==
               (GQA_ATTN_MAX_HEAD_DIM * 4u) +
               (GQA_ATTN_MAX_SEQ_LEN  * 4u) +
               (GQA_ATTN_BLOCK_DIM    * 4u) +
               (2u * 4u),
               "GQA_ATTN_SMEM_BYTES must equal the sum of the kernel's "
               "static __shared__ arrays (q_cache + logits + "
               "reduce_buf + 2 broadcast scalars)");

/* Per-op runtime arguments. Different op_kinds have different
 * argument shapes; the registry's cbuf-builder dispatches on
 * op_kind to pick which sub-struct to read. */
struct operator_dispatch_args_rmsnorm {
    uint64_t x_gpu_va;        /* input  [n_rows × n] FP16 */
    uint64_t gamma_gpu_va;    /* gamma  [n] FP16 */
    uint64_t out_gpu_va;      /* output [n_rows × n] FP16 */
    uint32_t n_rows;          /* number of rows */
    uint32_t n;               /* row width (must be reasonable for the BLOCK_DIM-strided reduction) */
    /* IEEE 754 FP32 bit pattern for the normalization epsilon. The
     * dispatcher writes these 4 bytes verbatim into cbuf[0] at offset
     * RMSNORM_CBUF_OFFSET_EPS; the SASS reads them back as an `float`
     * argument. The kernel build forbids FP types
     * (-mgeneral-regs-only), so callers either:
     *   - precompute the bit pattern at compile time
     *     (e.g. 1e-6f → 0x358637BD), or
     *   - convert from a float at the runtime/inference layer where
     *     FP is allowed and pass the resulting u32 in. */
    uint32_t eps_bits;
};

struct operator_dispatch_args_rope {
    uint64_t vec_gpu_va;      /* in/out  [batch × num_heads × head_dim] FP16 */
    uint64_t positions_gpu_va;/* [batch] int32 — token position per row */
    uint64_t cos_sin_gpu_va;  /* [max_pos × head_dim] FP32 — interleaved
                               * (cos, sin, cos, sin, ...) precomputed table */
    uint32_t batch;           /* number of tokens */
    uint32_t num_heads;       /* heads per token (n_head_q on Q, n_head_kv on K) */
    uint32_t head_dim;        /* must be even — pairs are (vec[2i], vec[2i+1]) */
};

struct operator_dispatch_args_embedding {
    uint64_t table_gpu_va;    /* Q4_K embedding table [n_tokens × table_row_bytes] */
    uint64_t out_gpu_va;      /* output FP16 row [embedding_length] */
    uint32_t token_id;        /* row index — must be < n_tokens (caller checks) */
    uint32_t embedding_length;/* width of the dequantized row (e.g. 1536 for Qwen2.5).
                               * May be < table_row_bytes/144*256 when the row was
                               * padded up to a Q4_K-block boundary (e.g. SmolLM2 576). */
    uint32_t table_row_bytes; /* bytes per token row — must be a multiple of
                               * EMBEDDING_Q4K_BLOCK_BYTES (144). For Qwen2.5
                               * with embedding_length=1536, this is
                               * (1536/256)*144 = 864. */
};

struct operator_dispatch_args_q4k_dequant {
    uint64_t blocks_gpu_va;   /* Q4_K-packed input [nb × 144 B] */
    uint64_t out_gpu_va;      /* FP16 output [nb × 256] */
    uint32_t nb;              /* number of Q4_K super-blocks */
};

struct operator_dispatch_args_swiglu {
    uint64_t gate_gpu_va;     /* [n] post-gate-projection FP16 */
    uint64_t up_gpu_va;       /* [n] post-up-projection FP16 */
    uint64_t out_gpu_va;      /* [n] silu(gate) * up — FP16 */
    uint32_t n;               /* total element count (rows × intermediate_size).
                               * For Qwen2.5-1.5B intermediate=8960; per-token
                               * decode → n=8960, 16-row prefill → n=143360. */
};

struct operator_dispatch_args_q4k_dot {
    uint64_t x_gpu_va;        /* [K] FP16 input activations */
    uint64_t weights_gpu_va;  /* [N × K × 144/256] Q4_K-packed weights */
    uint64_t out_gpu_va;      /* [N] FP32 output dot products */
    uint32_t k;               /* input dim — must be a multiple of 256 (every
                               * row is a whole number of Q4_K super-blocks). */
    uint32_t n;               /* output dim — number of rows in W. For Qwen2.5
                               * Q/O proj: 1536, KV proj: 256, gate/up: 8960,
                               * down: 1536, lm_head: 151936. */
};

struct operator_dispatch_args_gqa_attn {
    uint64_t q_gpu_va;        /* [n_head_q × head_dim] FP16 query rows */
    uint64_t k_gpu_va;        /* [seq_len × n_head_kv × head_dim] FP16 key cache */
    uint64_t v_gpu_va;        /* [seq_len × n_head_kv × head_dim] FP16 value cache */
    uint64_t out_gpu_va;      /* [n_head_q × head_dim] FP16 attention output */
    uint32_t n_head_q;        /* number of query heads (12 for Qwen2.5-1.5B) */
    uint32_t n_head_kv;       /* number of KV heads (2 for Qwen2.5-1.5B).
                               * n_head_q must be a multiple of n_head_kv —
                               * group = n_head_q / n_head_kv. */
    uint32_t head_dim;        /* dim per head (128 for Qwen2.5-1.5B). Must be
                               * ≤ GQA_ATTN_MAX_HEAD_DIM (256). */
    uint32_t seq_len;         /* count of past+current positions to attend to.
                               * Causal mask is implicit (future positions
                               * aren't passed in). Must be ≤
                               * GQA_ATTN_MAX_SEQ_LEN (4096). */
};

/* Tagged-union arg holder. The dispatcher fills the right sub-struct
 * based on the op_kind, then hands a pointer-to-union to the cbuf
 * builder which already knows which member to read. Keeping this
 * union explicit (rather than `void *`) buys static-shape checks
 * inside the test suite. */
struct operator_dispatch_args {
    uint32_t op_kind;
    union {
        struct operator_dispatch_args_rmsnorm     rmsnorm;
        struct operator_dispatch_args_rope        rope;
        struct operator_dispatch_args_embedding   embedding;
        struct operator_dispatch_args_q4k_dequant q4k_dequant;
        struct operator_dispatch_args_swiglu      swiglu;
        struct operator_dispatch_args_q4k_dot     q4k_dot;
        struct operator_dispatch_args_gqa_attn    gqa_attn;
    } u;
};

/* Launch shape: 3-D grid + 3-D block dims, matching CUDA / GA10B
 * QMD CTA_RASTER + CTA_THREAD_DIM. */
struct operator_launch_shape {
    uint32_t grid_x, grid_y, grid_z;
    uint32_t block_x, block_y, block_z;
    /* QMD ancillary fields, per ga10b_pipeline_op_v7. Most ops zero
     * these (SIMT kernels with no shared memory, no SLM, no
     * barriers); HMMA tensor-core kernels need non-zero values. */
    uint32_t register_count_v;   /* per-thread register usage */
    uint32_t smem_size_bytes;
    uint32_t slm_size_bytes;
    uint32_t barrier_count;
};

/* Fill the cbuf at offset OPERATOR_CBUF0_BASE with op-specific
 * arguments. `cbuf` is the kernel-side VA of the cbuf page; the
 * caller already cleared/cache_clean'd the page and will issue the
 * cache_clean + dsb sy after the builder returns.
 *
 * Returns 0 on success, negative on:
 *   - mismatch between args->op_kind and the dispatched builder
 *   - out-of-range scalar values (e.g. block dim too large)
 *
 * Builder is read-only on `args` and writes only inside cbuf[0]
 * (offsets [0x160, 0x160 + max_offset] for the op). Pure-logic;
 * unit-testable on QEMU. */
typedef int (*operator_cbuf_builder_fn)(void *cbuf,
                                         const struct operator_dispatch_args *args);

/* Compute the launch shape for an op given its runtime args. Pure
 * logic; no MMIO. Returns 0 on success, negative if args are out
 * of supported range (e.g. n_rows == 0 or n_rows > MAX). */
typedef int (*operator_launch_shape_fn)(const struct operator_dispatch_args *args,
                                         struct operator_launch_shape *out);

/* Per-op metadata. Looked up by op_kind via
 * `operator_dispatch_get_metadata`. */
struct operator_dispatch_metadata {
    uint32_t op_kind;
    operator_cbuf_builder_fn  build_cbuf;
    operator_launch_shape_fn  launch_shape;
};

/* Look up the dispatch metadata for an op_kind. Returns NULL if
 * the op_kind has no registered metadata (the dispatcher should
 * fall back to CPU). */
const struct operator_dispatch_metadata *
operator_dispatch_get_metadata(uint32_t op_kind);

/* Convenience entry points: dispatch to the registered builder
 * after looking up by op_kind. Returns 0 on success or
 * propagates the builder/lookup error. */
int operator_dispatch_build_cbuf(void *cbuf,
                                  const struct operator_dispatch_args *args);
int operator_dispatch_launch_shape(const struct operator_dispatch_args *args,
                                    struct operator_launch_shape *out);

#endif /* OPERATOR_DISPATCH_H */
