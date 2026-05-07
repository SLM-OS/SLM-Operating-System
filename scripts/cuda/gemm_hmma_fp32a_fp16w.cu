/*
 * gemm_hmma_fp32a_fp16w.cu — Mixed-precision HMMA GEMM
 * (#661 enabler — needed because MNIST's pool2 output is FP32,
 * not FP16. Casts FP32 activations to FP16 on the shared-memory
 * load path before the MMA fragment.)
 *
 * Inputs:
 *   a (const float *)  row-major [M × K]  FP32 activations
 *   b (const half  *)  row-major [K × N]  FP16 weights (from #660 path).
 *                                          Row-major matches the
 *                                          on-disk W_fc.bin layout
 *                                          produced by
 *                                          mnist-extract-weights.py
 *                                          --dtype fp16; no transpose
 *                                          required on the launcher.
 * Output:
 *   c (float *)        row-major [M × N]  FP32
 *
 * cbuf[0] layout: same scalar offsets as gemm_fp32.cu and
 * gemm_hmma_fp16.cu (0x160/0x168/0x170 pointers; 0x178/0x17C/0x180
 * dims). The launcher just swaps the shader and the b dtype.
 *
 * Shapes:
 *   - Tile-aligned (M/K/N % 16 == 0): all bounds checks pass
 *     trivially; HMMA fires every K-step.
 *   - Non-aligned: per-element bounds checks during the load loops
 *     write zeros to the unused fragment slots, and the per-cell
 *     output store guards `m < M && n < N`. MNIST's M=1, K=256,
 *     N=10 GEMM lands here — most of the 16×16 output tile is
 *     "wasted" but the row 0 cells 0..9 are correct.
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o gemm_hmma_fp32a_fp16w gemm_hmma_fp32a_fp16w.cu
 *   ./gemm_hmma_fp32a_fp16w
 *
 * HMMA verification: cuobjdump --dump-sass should contain
 * HMMA.16816.F32 instructions, same as the FP16-input variant.
 * The extra FP32→FP16 cast emits HFMA2 / I2FP / F2I conversion
 * instructions in the load path but the MMA core is identical.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <cuda_fp16.h>
#include <mma.h>

using namespace nvcuda;

constexpr int TILE_M = 16;
constexpr int TILE_N = 16;
constexpr int TILE_K = 16;

__global__ void gemm_hmma_fp32a_fp16w(const float *a, const half *b, float *c,
                                       int M, int K, int N)
{
    const int tile_row = blockIdx.y * TILE_M;
    const int tile_col = blockIdx.x * TILE_N;
    if (tile_row >= M || tile_col >= N) return;

    const int lane = threadIdx.x;

    __shared__ __align__(16) half  a_smem[TILE_M * TILE_K];
    __shared__ __align__(16) half  b_smem[TILE_K * TILE_N];
    __shared__ __align__(16) float c_smem[TILE_M * TILE_N];

    wmma::fragment<wmma::matrix_a,    TILE_M, TILE_N, TILE_K,
                   half, wmma::row_major> a_frag;
    wmma::fragment<wmma::matrix_b,    TILE_M, TILE_N, TILE_K,
                   half, wmma::row_major> b_frag;
    wmma::fragment<wmma::accumulator, TILE_M, TILE_N, TILE_K, float> c_frag;

    wmma::fill_fragment(c_frag, 0.0f);

    const int K_tiles = (K + TILE_K - 1) / TILE_K;
    for (int kt = 0; kt < K_tiles; kt++) {
        const int k_base = kt * TILE_K;

        /* A tile: FP32 → FP16 cast on shared-memory load. */
        for (int i = lane; i < TILE_M * TILE_K; i += 32) {
            const int m_local = i / TILE_K;
            const int k_local = i % TILE_K;
            const int m = tile_row + m_local;
            const int k = k_base + k_local;
            half v = __float2half(0.0f);
            if (m < M && k < K) {
                v = __float2half(a[m * K + k]);
            }
            a_smem[m_local * TILE_K + k_local] = v;
        }

        /* B tile: already FP16, row-major K×N (matches on-disk
         * W_fc.bin from --dtype fp16). */
        for (int i = lane; i < TILE_K * TILE_N; i += 32) {
            const int k_local = i / TILE_N;
            const int n_local = i % TILE_N;
            const int n = tile_col + n_local;
            const int k = k_base + k_local;
            half v = __float2half(0.0f);
            if (n < N && k < K) {
                v = b[k * N + n];   /* row-major: b[k, n] = b[k·N + n] */
            }
            b_smem[k_local * TILE_N + n_local] = v;
        }

        __syncthreads();
        wmma::load_matrix_sync(a_frag, a_smem, TILE_K);
        wmma::load_matrix_sync(b_frag, b_smem, TILE_N);
        wmma::mma_sync(c_frag, a_frag, b_frag, c_frag);
        __syncthreads();
    }

    wmma::store_matrix_sync(c_smem, c_frag, TILE_N, wmma::mem_row_major);
    __syncthreads();

    for (int i = lane; i < TILE_M * TILE_N; i += 32) {
        const int m_local = i / TILE_N;
        const int n_local = i % TILE_N;
        const int m = tile_row + m_local;
        const int n = tile_col + n_local;
        if (m < M && n < N) {
            c[m * N + n] = c_smem[m_local * TILE_N + n_local];
        }
    }
}

/* --- Host harness --- */

static void fill_random_f32(float *buf, size_t n, uint32_t *seed)
{
    for (size_t i = 0; i < n; i++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        buf[i] = ((float)((*seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
    }
}

static void fill_random_h(half *buf, size_t n, uint32_t *seed)
{
    for (size_t i = 0; i < n; i++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        float v = ((float)((*seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
        buf[i] = __float2half(v);
    }
}

static void cpu_reference(const float *a, const half *b, float *c,
                          int M, int K, int N)
{
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < K; k++) {
                /* Match the GPU's FP32→FP16 cast on the activation
                 * load path so the host reference reflects the
                 * precision the kernel actually computes. */
                float av = __half2float(__float2half(a[i * K + k]));
                float bv = __half2float(b[k * N + j]);
                s += av * bv;
            }
            c[i * N + j] = s;
        }
    }
}

static int run_one(int M, int K, int N, bool verbose)
{
    size_t a_n = (size_t)M * K;
    size_t b_n = (size_t)K * N;
    size_t c_n = (size_t)M * N;

    float *h_a   = (float *)malloc(a_n * sizeof(float));
    half  *h_b   = (half  *)malloc(b_n * sizeof(half));
    float *h_gpu = (float *)malloc(c_n * sizeof(float));
    float *h_ref = (float *)malloc(c_n * sizeof(float));

    uint32_t seed = 0xC0FFEEu + (uint32_t)(M * 31 + K * 17 + N);
    fill_random_f32(h_a, a_n, &seed);
    fill_random_h  (h_b, b_n, &seed);

    cpu_reference(h_a, h_b, h_ref, M, K, N);

    float *d_a; cudaMalloc(&d_a, a_n * sizeof(float));
    half  *d_b; cudaMalloc(&d_b, b_n * sizeof(half));
    float *d_c; cudaMalloc(&d_c, c_n * sizeof(float));
    cudaMemcpy(d_a, h_a, a_n * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, b_n * sizeof(half),  cudaMemcpyHostToDevice);
    cudaMemset(d_c, 0, c_n * sizeof(float));

    dim3 block(32, 1, 1);
    dim3 grid((N + TILE_N - 1) / TILE_N,
              (M + TILE_M - 1) / TILE_M, 1);
    gemm_hmma_fp32a_fp16w<<<grid, block>>>(d_a, d_b, d_c, M, K, N);
    cudaDeviceSynchronize();

    cudaMemcpy(h_gpu, d_c, c_n * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    float worst = 0.0f;
    for (size_t i = 0; i < c_n; i++) {
        float e = h_gpu[i] - h_ref[i];
        if (e < 0) e = -e;
        if (e > worst) worst = e;
        if (e > 1e-2f) {
            if (ok) {
                fprintf(stderr,
                        "[%dx%dx%d] MISMATCH C[%zu] gpu=%f ref=%f err=%f\n",
                        M, K, N, i, (double)h_gpu[i], (double)h_ref[i],
                        (double)e);
            }
            ok = 0;
        }
    }
    if (ok) {
        printf("[%dx%dx%d] OK (worst err %.6e)\n",
               M, K, N, (double)worst);
        if (verbose) {
            printf("    sample: C[0]=%f ref=%f\n",
                   (double)h_gpu[0], (double)h_ref[0]);
        }
    }

    free(h_a); free(h_b); free(h_gpu); free(h_ref);
    cudaFree(d_a); cudaFree(d_b); cudaFree(d_c);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    bool verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    int rc = 0;
    /* MNIST's GEMM shape — M=1 underfills the tile but bounds
     * checks make it work. */
    rc |= run_one(   1, 256,  10, verbose);
    /* Tile-aligned shapes for sanity. */
    rc |= run_one(  16,  16,  16, verbose);
    rc |= run_one(  16, 128,  16, verbose);
    rc |= run_one( 256, 256, 128, verbose);
    /* Mixed alignment. */
    rc |= run_one(   8,  25, 784, verbose);   /* MNIST Conv1 lifted */
    rc |= run_one(  16, 200, 196, verbose);   /* MNIST Conv2 lifted */
    return rc;
}
