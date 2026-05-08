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

/* Tagged-union arg holder. The dispatcher fills the right sub-struct
 * based on the op_kind, then hands a pointer-to-union to the cbuf
 * builder which already knows which member to read. Keeping this
 * union explicit (rather than `void *`) buys static-shape checks
 * inside the test suite. */
struct operator_dispatch_args {
    uint32_t op_kind;
    union {
        struct operator_dispatch_args_rmsnorm rmsnorm;
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
