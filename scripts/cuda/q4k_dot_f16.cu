/*
 * q4k_dot_f16.cu — Q4_K-weight × FP16-activation matmul on Jetson GA10B
 * (fifth SLM-on-GPU operator from the #540 menu — the matmul that
 *  dominates the SLM decode path).
 *
 * Computes `out = W @ x` where:
 *   - x is `[K]`        FP16   (shared activation vector)
 *   - W is `[N × K]`    Q4_K-packed (row-major; each row = K/256 super-blocks of 144 B)
 *   - out is `[N]`      FP32   (matches matmul_q4k_rows in
 *                                runtime/src/inference/ops_transformer.rs)
 *
 * This is the "decode" matmul: a single activation row times a quantized
 * weight matrix to produce one output row. Used for every linear
 * projection inside the transformer (Q / K / V / O, gate / up / down,
 * LM head). For Qwen2.5-1.5B the relevant shapes are:
 *
 *   K=1536, N=1536  — Q / O projections
 *   K=1536, N=256   — KV projections (n_kv_heads * head_dim)
 *   K=1536, N=8960  — gate / up projections (FFN expand)
 *   K=8960, N=1536  — down projection (FFN contract)
 *   K=1536, N=151936 (vocab) — LM head
 *
 * `K` must be a multiple of `Q4K_BLOCK_ELEMENTS = 256` (every row is a
 * whole number of super-blocks). All Qwen2.5 / Llama / Phi shapes
 * satisfy this.
 *
 * This kernel does the "dequantize-then-FP32-dot" path the CPU
 * reference falls back to for non-Q4_K quants. The Q8_K-quantized
 * activation fast-path that `vec_dot_q4_k_q8_k` uses on CPU isn't
 * portable to the GPU without a separate Q8_K-quantize kernel; that
 * lands as a future `Q8K_QUANTIZE` operator. The dequant path is
 * within ~1e-3 of the Q8_K path numerically and gets us to a working
 * GPU forward pass first.
 *
 * Mapping (one block per output row, 256 threads per block):
 *   blockIdx.x  = output row index (0..N-1)
 *   threadIdx.x = element-within-super-block index (0..255)
 *
 * Each thread:
 *   - Walks every super-block in its row (K/256 of them).
 *   - For each super-block, decodes its 4-bit nibble + 6-bit scale/min,
 *     reconstructs the FP32 weight, multiplies by the corresponding
 *     FP16 activation, and accumulates into an FP32 register.
 *   - At the end of the row: shared-memory tree reduction over the
 *     256 partial sums, thread 0 writes to out[row].
 *
 * Q4_K element-position decoding mirrors q4k_dequant_f16.cu (#699):
 *   tid → (j ∈ [0..4), half ∈ {0,1}, l ∈ [0..32)), is = 2j+half
 *   element_idx_within_block = (j*64) + (half*32) + l
 *   nibble = (half==0) ? (qs[(j<<5)+l] & 0x0F) : (qs[(j<<5)+l] >> 4)
 *   weight = d * scale(is) * nibble - dmin * min(is)
 *
 * cbuf[0] layout (CUDA ABI for sm_87):
 *   [0x160] x       (const half *)     [K] FP16 activations
 *   [0x168] weights (const uint8_t *)  [N * K * 144/256] Q4_K-packed
 *   [0x170] out     (float *)          [N] FP32 dot products
 *   [0x178] K       (int)              input dim, multiple of 256
 *   [0x17C] N       (int)              output dim
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o q4k_dot_f16 q4k_dot_f16.cu
 *   cuobjdump --extract-elf all q4k_dot_f16
 *   readelf -SW q4k_dot_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define Q4K_BLOCK_BYTES   144
#define Q4K_BLOCK_ELEMS   256
#define BLOCK_DIM         256

/* 6-bit scale decode (mirrors q4k_dequant_f16.cu). */
__device__ __forceinline__ uint8_t q4k_scale_d(const uint8_t *s, int i)
{
    if (i < 4) return s[i] & 0x3Fu;
    return (uint8_t)((s[i + 4] & 0x0Fu) | (((s[i - 4] >> 6) & 0x3u) << 4));
}

__device__ __forceinline__ uint8_t q4k_min_d(const uint8_t *s, int i)
{
    if (i < 4) return s[i + 4] & 0x3Fu;
    return (uint8_t)(((s[i + 4] >> 4) & 0x0Fu) | (((s[i] >> 6) & 0x3u) << 4));
}

__global__ void q4k_dot_f16(const half *x, const uint8_t *weights,
                             float *out, int K, int N)
{
    int row = blockIdx.x;
    int tid = threadIdx.x;
    if (row >= N) return;

    int n_blocks  = K >> 8;                    /* K / 256 */
    int row_bytes = n_blocks * Q4K_BLOCK_BYTES;
    const uint8_t *w_row = weights + (size_t)row * row_bytes;

    /* Decode tid → (j, half, l) once. The same tid handles the same
     * element offset within every super-block. */
    int j        = tid >> 6;          /* 0..3 */
    int hi       = (tid >> 5) & 1;    /* 0 = low nibble, 1 = high */
    int l        = tid & 31;          /* 0..31 */
    int is       = (j << 1) | hi;     /* 0..7 */
    int qs_off   = (j << 5) + l;      /* 0..127 */
    int elem_off = (j << 6) + (hi << 5) + l;  /* 0..255 within block */

    float local_acc = 0.0f;

    for (int b = 0; b < n_blocks; b++) {
        const uint8_t *blk    = w_row + b * Q4K_BLOCK_BYTES;
        half d_h              = *reinterpret_cast<const half *>(blk + 0);
        half dmin_h           = *reinterpret_cast<const half *>(blk + 2);
        const uint8_t *scales = blk + 4;
        const uint8_t *qs     = blk + 16;

        float d    = __half2float(d_h);
        float dmin = __half2float(dmin_h);

        uint8_t qpack = qs[qs_off];
        uint8_t nib   = hi ? (qpack >> 4) : (qpack & 0x0Fu);

        float sc = (float)q4k_scale_d(scales, is);
        float mn = (float)q4k_min_d(scales, is);
        float w  = d * sc * (float)nib - dmin * mn;

        int x_idx = (b << 8) + elem_off;       /* b*256 + elem_off */
        float xv  = __half2float(x[x_idx]);
        local_acc += xv * w;
    }

    /* Block tree reduction. */
    __shared__ float smem[BLOCK_DIM];
    smem[tid] = local_acc;
    __syncthreads();

    for (int s = BLOCK_DIM / 2; s > 0; s >>= 1) {
        if (tid < s) smem[tid] += smem[tid + s];
        __syncthreads();
    }

    if (tid == 0) out[row] = smem[0];
}

/* ---- CPU reference: dequantize + FP32 dot product ---- */

static uint8_t cpu_scale(const uint8_t *s, int i)
{
    if (i < 4) return s[i] & 0x3F;
    return (uint8_t)((s[i + 4] & 0x0F) | ((s[i - 4] >> 6) << 4));
}

static uint8_t cpu_min(const uint8_t *s, int i)
{
    if (i < 4) return s[i + 4] & 0x3F;
    return (uint8_t)((s[i + 4] >> 4) | ((s[i] >> 6) << 4));
}

static void dequant_block(const uint8_t *blk, float *out_256)
{
    half d_h              = *reinterpret_cast<const half *>(blk + 0);
    half dmin_h           = *reinterpret_cast<const half *>(blk + 2);
    const uint8_t *scales = blk + 4;
    const uint8_t *qs     = blk + 16;
    float d    = __half2float(d_h);
    float dmin = __half2float(dmin_h);

    for (int j = 0; j < 4; j++) {
        int is1 = j * 2;
        int is2 = j * 2 + 1;
        float sc1 = (float)cpu_scale(scales, is1);
        float mn1 = (float)cpu_min(scales, is1);
        float sc2 = (float)cpu_scale(scales, is2);
        float mn2 = (float)cpu_min(scales, is2);
        for (int l = 0; l < 32; l++) {
            uint8_t q = qs[j * 32 + l];
            out_256[j * 64 + l]      = d * sc1 * (float)(q & 0x0F) - dmin * mn1;
            out_256[j * 64 + 32 + l] = d * sc2 * (float)(q >> 4)   - dmin * mn2;
        }
    }
}

static void cpu_reference(const half *x, const uint8_t *weights,
                          float *out, int K, int N)
{
    int n_blocks  = K / 256;
    int row_bytes = n_blocks * Q4K_BLOCK_BYTES;
    float dq[256];
    for (int row = 0; row < N; row++) {
        const uint8_t *w_row = weights + (size_t)row * row_bytes;
        float acc = 0.0f;
        for (int b = 0; b < n_blocks; b++) {
            dequant_block(w_row + b * Q4K_BLOCK_BYTES, dq);
            int x_base = b * 256;
            for (int e = 0; e < 256; e++) {
                acc += __half2float(x[x_base + e]) * dq[e];
            }
        }
        out[row] = acc;
    }
}

/* ---- Fixture builder + harness ---- */

static void pack_scale(uint8_t *s, int i, uint8_t v)
{
    v &= 0x3F;
    if (i < 4) {
        s[i] = (uint8_t)((s[i] & 0xC0) | v);
    } else {
        s[i + 4] = (uint8_t)((s[i + 4] & 0xF0) | (v & 0x0F));
        s[i - 4] = (uint8_t)((s[i - 4] & 0x3F) | (((v >> 4) & 0x3) << 6));
    }
}

static void pack_min(uint8_t *s, int i, uint8_t v)
{
    v &= 0x3F;
    if (i < 4) {
        s[i + 4] = (uint8_t)((s[i + 4] & 0xC0) | v);
    } else {
        s[i + 4] = (uint8_t)((s[i + 4] & 0x0F) | ((v & 0x0F) << 4));
        s[i] = (uint8_t)((s[i] & 0x3F) | (((v >> 4) & 0x3) << 6));
    }
}

/* Build one fixture super-block. Same shape as q4k_dequant_f16.cu:
 * d = 0.125, dmin = 0.0625 (exact in FP16); scales = 1..8, mins = 0..7;
 * qs = pseudo-random nibble pairs from a tiny LCG seeded by `seed`. */
static void build_fixture_block(uint8_t *blk, uint32_t *seed)
{
    memset(blk, 0, Q4K_BLOCK_BYTES);
    half d_h    = __float2half(0.125f);
    half dmin_h = __float2half(0.0625f);
    memcpy(blk + 0, &d_h, 2);
    memcpy(blk + 2, &dmin_h, 2);
    uint8_t *scales = blk + 4;
    uint8_t *qs     = blk + 16;
    for (int i = 0; i < 8; i++) {
        pack_scale(scales, i, (uint8_t)(i + 1));
        pack_min(scales, i, (uint8_t)i);
    }
    for (int k = 0; k < 128; k++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        qs[k] = (uint8_t)((*seed) & 0xFF);
    }
}

static int run_one(int K, int N, uint32_t seed_in, const char *label,
                   float rel_tol)
{
    if (K % Q4K_BLOCK_ELEMS != 0) {
        fprintf(stderr, "[%s] K=%d not multiple of 256\n", label, K);
        return 0;
    }
    int n_blocks_per_row = K / Q4K_BLOCK_ELEMS;
    size_t weight_bytes  = (size_t)N * n_blocks_per_row * Q4K_BLOCK_BYTES;

    half  *h_x   = (half  *)malloc((size_t)K * sizeof(half));
    uint8_t *h_w = (uint8_t *)malloc(weight_bytes);
    float *h_gpu = (float *)malloc((size_t)N * sizeof(float));
    float *h_cpu = (float *)malloc((size_t)N * sizeof(float));
    if (!h_x || !h_w || !h_gpu || !h_cpu) { fprintf(stderr, "oom\n"); return 0; }

    /* x ~ uniform(-0.25, 0.25). Keeps the dot product within FP32 range
     * comfortably for the largest K we run. */
    uint32_t seed = seed_in;
    for (int i = 0; i < K; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) * 0.5f - 0.25f;
        h_x[i] = __float2half(v);
    }
    /* Build N rows of K/256 super-blocks each. */
    for (int r = 0; r < N; r++) {
        uint8_t *row = h_w + (size_t)r * n_blocks_per_row * Q4K_BLOCK_BYTES;
        for (int b = 0; b < n_blocks_per_row; b++) {
            build_fixture_block(row + b * Q4K_BLOCK_BYTES, &seed);
        }
    }

    cpu_reference(h_x, h_w, h_cpu, K, N);

    half *d_x; uint8_t *d_w; float *d_out;
    cudaMalloc(&d_x, (size_t)K * sizeof(half));
    cudaMalloc(&d_w, weight_bytes);
    cudaMalloc(&d_out, (size_t)N * sizeof(float));
    cudaMemcpy(d_x, h_x, (size_t)K * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_w, h_w, weight_bytes,             cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(N, 1, 1);
    q4k_dot_f16<<<grid, block>>>(d_x, d_w, d_out, K, N);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_x); cudaFree(d_w); cudaFree(d_out);
        free(h_x); free(h_w); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, (size_t)N * sizeof(float), cudaMemcpyDeviceToHost);

    /* The GPU and CPU sum 256 × n_blocks FP32 terms in different orders
     * (parallel tree vs sequential), so the result agrees to relative
     * tolerance, not bit-exactly. With x ~ U(-0.25, 0.25) and dequantized
     * weights at O(1), per-output magnitude scales as ~K/512; the tree
     * vs sequential reorder typically shows up at ~1e-5 relative. */
    int ok = 1;
    float worst_rel = 0.0f;
    int reported = 0;
    for (int i = 0; i < N; i++) {
        float g = h_gpu[i];
        float c = h_cpu[i];
        float diff = g - c;
        if (diff < 0) diff = -diff;
        float scale = c < 0 ? -c : c;
        if (scale < 1e-6f) scale = 1e-6f;
        float rel = diff / scale;
        if (rel > worst_rel) worst_rel = rel;
        if (rel > rel_tol) {
            if (reported < 4) {
                fprintf(stderr,
                        "[%s] REL MISMATCH out[%d] gpu=%f cpu=%f "
                        "(rel=%e tol=%e)\n",
                        label, i, (double)g, (double)c, (double)rel,
                        (double)rel_tol);
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[%s] OK — %d outputs within %.0e rel of CPU "
               "(worst=%.2e)\n", label, N, (double)rel_tol,
               (double)worst_rel);
    } else {
        fprintf(stderr,
                "[%s] FAILED — worst rel %.2e (allowed %.0e)\n",
                label, (double)worst_rel, (double)rel_tol);
    }

    cudaFree(d_x); cudaFree(d_w); cudaFree(d_out);
    free(h_x); free(h_w); free(h_gpu); free(h_cpu);
    return ok;
}

/* Boundary case: weights = all-zero super-blocks (d = 0, dmin = 0,
 * scales = 0, mins = 0, qs = 0) → every dequantized weight is 0 →
 * every output is 0 regardless of x. */
static int run_zero_weights(int K, int N)
{
    int n_blocks_per_row = K / Q4K_BLOCK_ELEMS;
    size_t weight_bytes  = (size_t)N * n_blocks_per_row * Q4K_BLOCK_BYTES;
    half  *h_x   = (half  *)malloc((size_t)K * sizeof(half));
    uint8_t *h_w = (uint8_t *)calloc(1, weight_bytes);
    float *h_gpu = (float *)malloc((size_t)N * sizeof(float));
    if (!h_x || !h_w || !h_gpu) { fprintf(stderr, "oom\n"); return 0; }

    /* x = arbitrary non-zero. */
    for (int i = 0; i < K; i++) h_x[i] = __float2half(1.0f);

    half *d_x; uint8_t *d_w; float *d_out;
    cudaMalloc(&d_x, (size_t)K * sizeof(half));
    cudaMalloc(&d_w, weight_bytes);
    cudaMalloc(&d_out, (size_t)N * sizeof(float));
    cudaMemcpy(d_x, h_x, (size_t)K * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_w, h_w, weight_bytes,             cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(N, 1, 1);
    q4k_dot_f16<<<grid, block>>>(d_x, d_w, d_out, K, N);
    cudaDeviceSynchronize();
    cudaMemcpy(h_gpu, d_out, (size_t)N * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (h_gpu[i] != 0.0f) {
            fprintf(stderr,
                    "[zero-w K=%d N=%d] out[%d]=%f, expected 0.0\n",
                    K, N, i, (double)h_gpu[i]);
            ok = 0;
            break;
        }
    }
    if (ok) {
        printf("[zero-w K=%d N=%d] OK — all outputs are 0.0\n", K, N);
    }

    cudaFree(d_x); cudaFree(d_w); cudaFree(d_out);
    free(h_x); free(h_w); free(h_gpu);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 5e-4 relative tolerance: the FP32 sum-of-products reorders
     * differently between the GPU's tree reduction and the CPU's
     * sequential accumulation. Empirically this lands at ~1e-5 for
     * Qwen2.5-1.5B shapes; the tolerance leaves headroom for the
     * widest K (8960) where small magnitudes near zero amplify the
     * relative error. */
    const float REL_TOL = 5e-4f;

    /* SLM forward shapes for Qwen2.5-1.5B. Skip the LM head (K=1536,
     * N=151936) — same kernel, just bigger grid; harness time grows
     * linearly with N. The shapes below cover every distinct (K, N)
     * combination the SLM forward path actually dispatches. */
    all_ok &= run_one(1536, 1536, 0xC0FFEE01u, "qwen-Q   ", REL_TOL);
    all_ok &= run_one(1536, 256,  0xC0FFEE02u, "qwen-KV  ", REL_TOL);
    all_ok &= run_one(1536, 8960, 0xC0FFEE03u, "qwen-up  ", REL_TOL);
    all_ok &= run_one(8960, 1536, 0xC0FFEE04u, "qwen-down", REL_TOL);

    all_ok &= run_zero_weights(1536, 16);
    all_ok &= run_zero_weights(8960, 16);

    return all_ok ? 0 : 1;
}
