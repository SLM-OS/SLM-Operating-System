/*
 * conv2d_hmma_fp16.cu — Implicit-GEMM HMMA Conv2D (#662)
 *
 * Reformulates 2D convolution as a GEMM with shapes:
 *
 *   M_gemm = K_out                    (output channels)
 *   K_gemm = C_in × R × S             (flattened input window per output cell)
 *   N_gemm = N_batch × H_out × W_out  (output spatial cells)
 *
 *   Y[k_out, n_batch, h_out, w_out]
 *     = Σ_(c_in, r, s) W[k_out, c_in, r, s] · X[n_batch, c_in, h_in, w_in]
 *     where h_in = h_out·stride + r − pad
 *           w_in = w_out·stride + s − pad
 *
 * "Implicit" GEMM: the kernel computes the index gather for the
 * im2col B matrix in-shader (no DRAM staging buffer), pays no
 * extra memory traffic vs the explicit im2col+GEMM split.
 *
 * Layout (NCHW; matches MNIST launcher today):
 *   X: [N_batch, C_in,  H_in,  W_in ] half, row-major within each plane
 *   W: [K_out,   C_in,  R,     S    ] half, contiguous = [K_out, C_in·R·S]
 *   Y: [N_batch, K_out, H_out, W_out] float
 *
 * cbuf[0] layout (CUDA ABI; mirrors gemm_hmma_fp16.cu — extends with
 * conv-specific dims):
 *   [0x160] X       (const half *)
 *   [0x168] W       (const half *)
 *   [0x170] Y       (float *)
 *   [0x178] N_batch (int32)
 *   [0x17C] C_in    (int32)
 *   [0x180] H_in    (int32)
 *   [0x184] W_in    (int32)
 *   [0x188] K_out   (int32)
 *   [0x18C] R       (int32)
 *   [0x190] S       (int32)
 *   [0x194] H_out   (int32)
 *   [0x198] W_out   (int32)
 *   [0x19C] pad     (int32)
 *   [0x1A0] stride  (int32)
 *
 * Constraints (this version — relax in follow-on per #662):
 *   - K_out          multiple of 16
 *   - C_in × R × S   multiple of 16
 *   - N_batch × H_out × W_out  multiple of 16
 *   - 1 batch dimension (N_batch = 1) for the harness; kernel is
 *     coded for any N_batch but the test loop only validates 1.
 *
 * Tile (one CTA = one warp):
 *   16 × 16 output tile in (M, N) GEMM coords →
 *   16 output channels × 16 spatial cells per CTA. Grid is
 *   (N_gemm/16, M_gemm/16, 1). K-loop is K_gemm/16 fragments.
 *
 * Compile + extract SASS on Jetson (jetson-nano-2 verified
 * 2026-05-06 with nvcc 12.6, sm_87, GA10B):
 *   nvcc -arch=sm_87 -o conv2d_hmma_fp16 conv2d_hmma_fp16.cu
 *   ./conv2d_hmma_fp16
 *   cuobjdump --extract-elf all conv2d_hmma_fp16
 *   readelf -SW conv2d_hmma_fp16.2.sm_87.cubin | grep text
 *   # dd .text._Z16conv2d_hmma_fp16... → conv2d_hmma_fp16_shader.sass
 *
 * Extracted .text section is 19,200 B (vs 3,968 B for the GEMM
 * kernel — extra size is the per-K-tile index decode + bounds
 * check inlined into the gather loop).
 *
 * HMMA verification: cuobjdump --dump-sass produces entries like
 *   HMMA.16816.F32 R8, R16.reuse, R20, R8 ;
 *   HMMA.16816.F32 R12, R16, R22, R12 ;
 * — Ampere m16n8k16 tensor-core path, FP32 accumulator. Not
 * FFMA fallback.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <cuda_fp16.h>
#include <mma.h>

using namespace nvcuda;

constexpr int TILE_M    = 16;
constexpr int TILE_N    = 16;
constexpr int TILE_K    = 16;
constexpr int WARP_SIZE = 32;          /* one warp per CTA, lane = 0..31 */

__global__ void conv2d_hmma_fp16(const half *X, const half *W, float *Y,
                                 int N_batch, int C_in, int H_in, int W_in,
                                 int K_out,   int R,    int S,
                                 int H_out,   int W_out,
                                 int pad,     int stride)
{
    const int M_gemm = K_out;
    const int K_gemm = C_in * R * S;
    const int N_gemm = N_batch * H_out * W_out;

    const int tile_m = blockIdx.y * TILE_M;
    const int tile_n = blockIdx.x * TILE_N;
    if (tile_m >= M_gemm || tile_n >= N_gemm) return;

    const int lane = threadIdx.x;  /* 0..31, one warp per CTA */

    __shared__ __align__(16) half  a_smem[TILE_M * TILE_K];
    __shared__ __align__(16) half  b_smem[TILE_K * TILE_N];
    __shared__ __align__(16) float c_smem[TILE_M * TILE_N];

    wmma::fragment<wmma::matrix_a,    TILE_M, TILE_N, TILE_K,
                   half, wmma::row_major> a_frag;
    wmma::fragment<wmma::matrix_b,    TILE_M, TILE_N, TILE_K,
                   half, wmma::col_major> b_frag;
    wmma::fragment<wmma::accumulator, TILE_M, TILE_N, TILE_K, float> c_frag;

    wmma::fill_fragment(c_frag, 0.0f);

    const int K_tiles = (K_gemm + TILE_K - 1) / TILE_K;
    for (int kt = 0; kt < K_tiles; kt++) {
        const int k_base = kt * TILE_K;

        /* --- Stage A tile (weights). A is row-major [M, K]; element
         * (m_local, k_local) = W[tile_m + m_local, k_base + k_local].
         * W is stored [K_out, C_in·R·S] = contiguous reshape of
         * W[K_out, C_in, R, S], so the index is just a linear read. */
        for (int i = lane; i < TILE_M * TILE_K; i += WARP_SIZE) {
            const int m_local = i / TILE_K;
            const int k_local = i % TILE_K;
            const int m = tile_m + m_local;
            const int k = k_base + k_local;
            half v = __float2half(0.0f);
            if (m < M_gemm && k < K_gemm) {
                v = W[m * K_gemm + k];
            }
            a_smem[m_local * TILE_K + k_local] = v;
        }

        /* --- Stage B tile (input gather). B is col-major [K, N];
         * element (k_local, n_local) at b_smem[n_local·TILE_K + k_local].
         *
         * k decodes to (c_in, r, s); n decodes to (n_batch, h_out, w_out).
         * Ordering: k = c_in·R·S + r·S + s — must MATCH W storage so
         * the row-major W load `W[m·K_gemm + k]` lands on
         * W[k_out, c_in, r, s] without a transpose. (Earlier attempt
         * with c_in innermost was inconsistent with NCHW weight
         * storage and produced wrong outputs on every 3×3 shape.)
         *
         * Out-of-bounds (h_in, w_in) → 0 (zero padding). */
        for (int i = lane; i < TILE_K * TILE_N; i += WARP_SIZE) {
            const int n_local = i / TILE_K;
            const int k_local = i % TILE_K;
            const int n = tile_n + n_local;
            const int k = k_base + k_local;
            half v = __float2half(0.0f);
            if (n < N_gemm && k < K_gemm) {
                /* Decode k → (c_in, r, s) per [C_in, R, S] layout. */
                const int rs   = k % (R * S);
                const int c_in = k / (R * S);
                const int r    = rs / S;
                const int s    = rs - r * S;

                /* Decode n → (n_batch, h_out, w_out) */
                const int hw   = H_out * W_out;
                const int nb   = n / hw;
                const int rem  = n - nb * hw;
                const int h_out = rem / W_out;
                const int w_out = rem - h_out * W_out;

                const int h_in = h_out * stride + r - pad;
                const int w_in = w_out * stride + s - pad;

                if (h_in >= 0 && h_in < H_in && w_in >= 0 && w_in < W_in) {
                    /* X is [N_batch, C_in, H_in, W_in], NCHW */
                    const long long x_idx = ((long long)nb * C_in + c_in)
                                          * H_in * W_in
                                          + (long long)h_in * W_in + w_in;
                    v = X[x_idx];
                }
            }
            b_smem[n_local * TILE_K + k_local] = v;
        }

        __syncthreads();

        wmma::load_matrix_sync(a_frag, a_smem, TILE_K);
        wmma::load_matrix_sync(b_frag, b_smem, TILE_K);
        wmma::mma_sync(c_frag, a_frag, b_frag, c_frag);

        __syncthreads();
    }

    /* --- Store output. C_frag holds 16×16 accumulators; cells map
     * to Y[k_out, n_batch, h_out, w_out] via the same (m, n)
     * decomposition used during the gather. The 16 N-cells in a
     * tile may not be contiguous in Y (they may cross h_out
     * boundaries), so stage through shared mem and write per-cell. */
    wmma::store_matrix_sync(c_smem, c_frag, TILE_N, wmma::mem_row_major);
    __syncthreads();

    for (int i = lane; i < TILE_M * TILE_N; i += WARP_SIZE) {
        const int m_local = i / TILE_N;
        const int n_local = i % TILE_N;
        const int m = tile_m + m_local;
        const int n = tile_n + n_local;
        if (m < M_gemm && n < N_gemm) {
            const int k_out = m;
            const int hw    = H_out * W_out;
            const int nb    = n / hw;
            const int rem   = n - nb * hw;
            const int h_out = rem / W_out;
            const int w_out = rem - h_out * W_out;
            /* Y is [N_batch, K_out, H_out, W_out], NCHW */
            const long long y_idx = ((long long)nb * K_out + k_out)
                                  * H_out * W_out
                                  + (long long)h_out * W_out + w_out;
            Y[y_idx] = c_smem[m_local * TILE_N + n_local];
        }
    }
}

/* --- Host harness --- */

/* Harness exit codes — same scheme as scripts/cuda/gemm_hmma_fp16.cu so a
 * future CI wrapper can keyed on identical numbers across both kernels. */
#define HARNESS_RC_OK              0
#define HARNESS_RC_OOM             1
#define HARNESS_RC_LAUNCH_FAIL     2  /* cudaGetLastError after <<<...>>> */
#define HARNESS_RC_RUN_FAIL        3  /* error during cudaDeviceSynchronize */
#define HARNESS_RC_MISMATCH        5  /* result above tolerance */

/* Bail loudly on the first CUDA error so launch / sync failures surface
 * with a distinct HARNESS_RC_* instead of being mis-attributed by a
 * comparison loop reading uninitialized device memory. */
#define CUDA_OK_OR(rc, label)                                                  \
    do {                                                                       \
        cudaError_t _e = cudaGetLastError();                                   \
        if (_e != cudaSuccess) {                                               \
            fprintf(stderr, "%s:%d: cuda error: %s\n",                         \
                    __FILE__, __LINE__, cudaGetErrorString(_e));               \
            ret = (rc);                                                        \
            goto label;                                                        \
        }                                                                      \
    } while (0)

static void fill_random(half *buf, size_t n, uint32_t *seed)
{
    for (size_t i = 0; i < n; i++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        float v = ((float)((*seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
        buf[i] = __float2half(v);
    }
}

/* CPU reference. NCHW layout matches the kernel; FP32 throughout
 * with FP16 inputs cast on each multiply, mirroring the HMMA
 * accumulator. */
static void cpu_reference(const half *X, const half *W, float *Y,
                          int N_batch, int C_in, int H_in, int W_in,
                          int K_out,   int R,    int S,
                          int H_out,   int W_out,
                          int pad,     int stride)
{
    for (int nb = 0; nb < N_batch; nb++) {
        for (int k = 0; k < K_out; k++) {
            for (int h = 0; h < H_out; h++) {
                for (int w = 0; w < W_out; w++) {
                    float sum = 0.0f;
                    for (int c = 0; c < C_in; c++) {
                        for (int r = 0; r < R; r++) {
                            for (int sx = 0; sx < S; sx++) {
                                int hi = h * stride + r - pad;
                                int wi = w * stride + sx - pad;
                                if (hi < 0 || hi >= H_in
                                 || wi < 0 || wi >= W_in) {
                                    continue;
                                }
                                long long xi = ((long long)nb * C_in + c)
                                             * H_in * W_in
                                             + (long long)hi * W_in + wi;
                                long long wi_ = ((long long)k * C_in + c)
                                              * R * S
                                              + (long long)r * S + sx;
                                sum += __half2float(X[xi])
                                     * __half2float(W[wi_]);
                            }
                        }
                    }
                    long long yi = ((long long)nb * K_out + k)
                                 * H_out * W_out
                                 + (long long)h * W_out + w;
                    Y[yi] = sum;
                }
            }
        }
    }
}

static int run_one(int N_batch, int C_in, int H_in, int W_in,
                   int K_out,   int R,    int S,
                   int pad,     int stride,
                   bool verbose)
{
    const int H_out = (H_in + 2 * pad - R) / stride + 1;
    const int W_out = (W_in + 2 * pad - S) / stride + 1;

    const int M_gemm = K_out;
    const int K_gemm = C_in * R * S;
    const int N_gemm = N_batch * H_out * W_out;

    if (M_gemm % TILE_M || K_gemm % TILE_K || N_gemm % TILE_N) {
        fprintf(stderr,
                "[N=%d C=%d %dx%d K=%d %dx%d pad=%d stride=%d "
                "→ M=%d K=%d N=%d] SKIPPED (not tile-aligned).\n",
                N_batch, C_in, H_in, W_in, K_out, R, S, pad, stride,
                M_gemm, K_gemm, N_gemm);
        return HARNESS_RC_OK;
    }

    size_t x_n = (size_t)N_batch * C_in * H_in * W_in;
    size_t w_n = (size_t)K_out * C_in * R * S;
    size_t y_n = (size_t)N_batch * K_out * H_out * W_out;

    int    ret   = HARNESS_RC_OK;
    half  *h_x   = (half  *)malloc(x_n * sizeof(half));
    half  *h_w   = (half  *)malloc(w_n * sizeof(half));
    float *h_gpu = (float *)malloc(y_n * sizeof(float));
    float *h_ref = (float *)malloc(y_n * sizeof(float));
    half  *d_x   = nullptr;
    half  *d_w   = nullptr;
    float *d_y   = nullptr;
    if (!h_x || !h_w || !h_gpu || !h_ref) {
        fprintf(stderr, "oom\n");
        ret = HARNESS_RC_OOM;
        goto cleanup;
    }

    /* Scoped block: see the matching note in gemm_hmma_fp16.cu —
     * goto cleanup must not cross initializer-bearing locals. */
    {
        uint32_t seed = 0x9E3779B9u
                      ^ (uint32_t)(C_in * 31 + K_out * 17
                                   + R * 11 + pad * 7 + stride);
        fill_random(h_x, x_n, &seed);
        fill_random(h_w, w_n, &seed);
    }

    cpu_reference(h_x, h_w, h_ref,
                  N_batch, C_in, H_in, W_in,
                  K_out, R, S, H_out, W_out,
                  pad, stride);

    cudaMalloc(&d_x, x_n * sizeof(half));
    cudaMalloc(&d_w, w_n * sizeof(half));
    cudaMalloc(&d_y, y_n * sizeof(float));
    /* Catch device-side OOM here so it surfaces as HARNESS_RC_OOM
     * rather than being mis-attributed to the kernel launch below. */
    CUDA_OK_OR(HARNESS_RC_OOM, cleanup);
    cudaMemcpy(d_x, h_x, x_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_w, h_w, w_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemset(d_y, 0, y_n * sizeof(float));

    {
        dim3 block(WARP_SIZE, 1, 1);
        dim3 grid(N_gemm / TILE_N, M_gemm / TILE_M, 1);
        conv2d_hmma_fp16<<<grid, block>>>(d_x, d_w, d_y,
                                          N_batch, C_in, H_in, W_in,
                                          K_out, R, S, H_out, W_out,
                                          pad, stride);
    }
    CUDA_OK_OR(HARNESS_RC_LAUNCH_FAIL, cleanup);
    cudaDeviceSynchronize();
    CUDA_OK_OR(HARNESS_RC_RUN_FAIL, cleanup);

    cudaMemcpy(h_gpu, d_y, y_n * sizeof(float), cudaMemcpyDeviceToHost);

    /* Tolerance: see scripts/cuda/gemm_hmma_fp16.cu for the derivation
     * — HMMA accumulates FP32 from FP16 operands, per-multiply error
     * is ~FP16 ULP (~5e-4 near 1.0), and across K accumulations it
     * grows ~sqrt(K). For the largest case here (K_gemm = 288), 1e-2
     * abs is comfortably above the noise floor without masking real
     * bugs. */
    {
        int ok = 1;
        float worst_err = 0.0f;
        for (size_t i = 0; i < y_n; i++) {
            float err = h_gpu[i] - h_ref[i];
            if (err < 0) err = -err;
            if (err > worst_err) worst_err = err;
            if (err > 1e-2f) {
                if (ok) {
                    fprintf(stderr,
                            "[%dx%dx%dx%d K%d %dx%d pad%d s%d] MISMATCH "
                            "Y[%zu] gpu=%f ref=%f err=%f\n",
                            N_batch, C_in, H_in, W_in, K_out, R, S, pad,
                            stride, i, (double)h_gpu[i], (double)h_ref[i],
                            (double)err);
                }
                ok = 0;
            }
        }

        if (ok) {
            printf("[N=%d C=%d %dx%d K=%d %dx%d pad=%d s=%d "
                   "→ M=%d K=%d N=%d] OK (worst err %.6e)\n",
                   N_batch, C_in, H_in, W_in, K_out, R, S, pad, stride,
                   M_gemm, K_gemm, N_gemm, (double)worst_err);
            if (verbose && y_n >= 4) {
                printf("    sample: Y[0]=%f ref=%f, Y[%zu]=%f ref=%f\n",
                       (double)h_gpu[0], (double)h_ref[0],
                       y_n - 1, (double)h_gpu[y_n - 1],
                       (double)h_ref[y_n - 1]);
            }
        } else {
            ret = HARNESS_RC_MISMATCH;
        }
    }

cleanup:
    free(h_x);
    free(h_w);
    free(h_gpu);
    free(h_ref);
    if (d_x) cudaFree(d_x);
    if (d_w) cudaFree(d_w);
    if (d_y) cudaFree(d_y);
    return ret;
}

int main(int argc, char **argv)
{
    bool verbose = false;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) verbose = true;

    int rc = 0;

    /* Shape 1: 1×1 conv (degenerate, K=C_in only). Sanity check
     * the GEMM path absent any spatial gather. */
    rc |= run_one(/*N=*/1, /*C=*/16, /*H=*/16, /*W=*/16,
                  /*K=*/16, /*R=*/1, /*S=*/1,
                  /*pad=*/0, /*stride=*/1, verbose);

    /* Shape 2: 3×3, pad=0, stride=1 — basic conv. */
    rc |= run_one(1, 16, 18, 18, 16, 3, 3, 0, 1, verbose);

    /* Shape 3: 3×3, pad=1, stride=1 — exercise zero padding at
     * boundaries. Output spatial == input spatial. */
    rc |= run_one(1, 16, 16, 16, 16, 3, 3, 1, 1, verbose);

    /* Shape 4: 3×3, stride=2 — exercise stride. */
    rc |= run_one(1, 16, 33, 33, 16, 3, 3, 0, 2, verbose);

    /* Shape 5: more channels + larger K. */
    rc |= run_one(1, 32, 18, 18, 32, 3, 3, 0, 1, verbose);

    /* Shape 6: ImageNet-ish (ResNet-18 conv1 needs C=3 which isn't
     * div 16, so use C=16 to stay aligned). Worth a real sized
     * exercise of memory throughput. */
    rc |= run_one(1, 16, 64, 64, 64, 3, 3, 1, 1, verbose);

    return rc;
}
