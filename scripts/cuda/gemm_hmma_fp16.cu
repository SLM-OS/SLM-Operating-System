/*
 * gemm_hmma_fp16.cu — Parameterized HMMA tensor-core GEMM (#659)
 *
 * Reads M/K/N from cbuf at dispatch time, mirrors gemm_fp32.cu's
 * "one blob handles every matmul shape" contract. Uses the
 * nvcuda::wmma m16n16k16 C++ fragment API which compiles to
 * mma.sync.aligned.m16n8k16 PTX on Ampere (sm_87 GA10B); each
 * wmma::mma_sync emits two of those instructions to cover the
 * 16-wide output tile.
 *
 * Constraints (this version):
 *   - M, K, N must each be a multiple of 16. Edge handling for
 *     non-aligned shapes is a follow-on (host-side padding works
 *     today; in-kernel masked stores come next).
 *   - A is row-major [M × K] half (FP16).
 *   - B is column-major [K × N] half (FP16).
 *     The wmma::matrix_b col_major layout matches typical "weight
 *     matrix transposed at load time" usage in inference stacks.
 *   - C is row-major [M × N] float (FP32 accumulator).
 *
 * cbuf[0] layout (CUDA ABI, mirrors gemm_fp32.cu offsets):
 *   [0x160] a (const half *)
 *   [0x168] b (const half *)
 *   [0x170] c (float *)
 *   [0x178] M (int32)
 *   [0x17C] K (int32)
 *   [0x180] N (int32)
 *
 * Test pattern: A and B all 1.0h → C[i,j] = K for all (i, j).
 *
 * Compile + extract SASS on Jetson (jetson-nano-2 verified
 * 2026-05-06 with nvcc 12.6, sm_87, GA10B):
 *   nvcc -arch=sm_87 -o gemm_hmma_fp16 gemm_hmma_fp16.cu
 *   ./gemm_hmma_fp16                          # runs the harness
 *   cuobjdump --extract-elf all gemm_hmma_fp16
 *   readelf -SW gemm_hmma_fp16.2.sm_87.cubin | grep text
 *   # dd .text._Z14gemm_hmma_fp16... → gemm_hmma_fp16_shader.sass
 *
 * HMMA verification — `cuobjdump --dump-sass` against the
 * compiled binary contains entries like:
 *   HMMA.16816.F32 R16, R20, R6,  R16 ;
 *   HMMA.16816.F32 R12, R20, R24, R12 ;
 * .16816 is the Ampere mma.sync.m16n8k16 shape; .F32 is the FP32
 * accumulator. WMMA's m16n16k16 fragment lowers to two of these
 * per mma_sync() call.
 *
 * Tile config:
 *   - One CTA per 16×16 output tile (32 threads = one warp).
 *   - Grid = (ceil(N/16), ceil(M/16), 1).
 *   - K loop inside the kernel iterates K/16 fragments.
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

__global__ void gemm_hmma_fp16(const half *a, const half *b, float *c,
                               int M, int K, int N)
{
    int tile_row = blockIdx.y * TILE_M;
    int tile_col = blockIdx.x * TILE_N;
    if (tile_row >= M || tile_col >= N) return;

    /* m16n16k16 fragment shape. row_major A + col_major B is what
     * Ampere's HMMA path expects without an extra transpose pass. */
    wmma::fragment<wmma::matrix_a,    TILE_M, TILE_N, TILE_K,
                   half, wmma::row_major> a_frag;
    wmma::fragment<wmma::matrix_b,    TILE_M, TILE_N, TILE_K,
                   half, wmma::col_major> b_frag;
    wmma::fragment<wmma::accumulator, TILE_M, TILE_N, TILE_K, float> c_frag;

    wmma::fill_fragment(c_frag, 0.0f);

    for (int k = 0; k < K; k += TILE_K) {
        const half *a_tile = a + tile_row * K + k;        /* row-major: lda=K */
        const half *b_tile = b + tile_col * K + k;        /* col-major: ldb=K */
        wmma::load_matrix_sync(a_frag, a_tile, K);
        wmma::load_matrix_sync(b_frag, b_tile, K);
        wmma::mma_sync(c_frag, a_frag, b_frag, c_frag);
    }

    float *c_tile = c + tile_row * N + tile_col;
    wmma::store_matrix_sync(c_tile, c_frag, N, wmma::mem_row_major);
}

/* --- Host harness --- */

/* Pseudo-random fill — small range (-1, 1) so K=2048 doesn't push
 * values outside FP16's normal range. Same seed for A and B is fine
 * since the host reference and the GPU kernel both consume the same
 * arrays — only their relative content matters. */
static void fill_random(half *buf, size_t n, uint32_t *seed)
{
    for (size_t i = 0; i < n; i++) {
        *seed = (*seed) * 1664525u + 1013904223u;          /* LCG */
        float v = ((float)((*seed >> 8) & 0xFFFF) / 32768.0f) - 1.0f;
        buf[i] = __float2half(v);
    }
}

/* CPU reference: compute C = A @ B^T (because B is col-major K×N,
 * which is row-major N×K aliased) in FP32. The FP16 inputs are
 * cast to FP32 before the multiply-add, mirroring the HMMA
 * accumulator's behavior. Tolerance below is set assuming this
 * exact reference. */
static void cpu_reference(const half *a, const half *b, float *c,
                          int M, int K, int N)
{
    /* a: row-major [M, K], lda = K
     * b: col-major [K, N], ldb = K (i.e. element (k, n) = b[n * K + k])
     * c: row-major [M, N], ldc = N */
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            for (int k = 0; k < K; k++) {
                float av = __half2float(a[i * K + k]);
                float bv = __half2float(b[j * K + k]);
                s += av * bv;
            }
            c[i * N + j] = s;
        }
    }
}

static int run_random(int M, int K, int N, bool verbose)
{
    if (M % TILE_M || K % TILE_K || N % TILE_N) return 0;

    size_t a_n = (size_t)M * K;
    size_t b_n = (size_t)K * N;
    size_t c_n = (size_t)M * N;

    half  *h_a    = (half  *)malloc(a_n * sizeof(half));
    half  *h_b    = (half  *)malloc(b_n * sizeof(half));
    float *h_gpu  = (float *)malloc(c_n * sizeof(float));
    float *h_ref  = (float *)malloc(c_n * sizeof(float));
    if (!h_a || !h_b || !h_gpu || !h_ref) { fprintf(stderr, "oom\n"); return 1; }

    uint32_t seed = 0x1234u + (uint32_t)(M * 31 + K * 17 + N);
    fill_random(h_a, a_n, &seed);
    fill_random(h_b, b_n, &seed);

    cpu_reference(h_a, h_b, h_ref, M, K, N);

    half  *d_a; cudaMalloc(&d_a, a_n * sizeof(half));
    half  *d_b; cudaMalloc(&d_b, b_n * sizeof(half));
    float *d_c; cudaMalloc(&d_c, c_n * sizeof(float));
    cudaMemcpy(d_a, h_a, a_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, b_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemset(d_c, 0, c_n * sizeof(float));

    dim3 block(32, 1, 1);
    dim3 grid((N + TILE_N - 1) / TILE_N,
              (M + TILE_M - 1) / TILE_M,
              1);
    gemm_hmma_fp16<<<grid, block>>>(d_a, d_b, d_c, M, K, N);
    cudaDeviceSynchronize();

    cudaMemcpy(h_gpu, d_c, c_n * sizeof(float), cudaMemcpyDeviceToHost);

    /* Tolerance: HMMA accumulates FP32 internally but operands are
     * FP16, so per-multiply error is bounded by FP16 ULP of the
     * inputs (~5e-4 for values near 1.0). Across K accumulations the
     * error can grow ~sqrt(K) under typical input distributions.
     * For K up to 2048, 1e-2 abs tolerance gives margin without
     * masking real bugs. Tighten to 1e-3 once we know HMMA + col-
     * major B layout is correct on this hardware. */
    int ok = 1;
    int worst_idx = 0;
    float worst_err = 0.0f;
    for (size_t i = 0; i < c_n; i++) {
        float err = h_gpu[i] - h_ref[i];
        if (err < 0) err = -err;
        if (err > worst_err) { worst_err = err; worst_idx = (int)i; }
        if (err > 1e-2f) {
            if (ok) {
                fprintf(stderr,
                        "[rand %dx%dx%d] MISMATCH C[%zu] gpu=%f ref=%f err=%f\n",
                        M, K, N, i, (double)h_gpu[i], (double)h_ref[i],
                        (double)err);
            }
            ok = 0;
        }
    }
    if (ok) {
        printf("[rand %dx%dx%d] OK (worst err %.6e at idx %d, gpu=%.6f ref=%.6f)\n",
               M, K, N, (double)worst_err, worst_idx,
               (double)h_gpu[worst_idx], (double)h_ref[worst_idx]);
    }
    if (verbose && ok && c_n >= 4) {
        printf("    sample: C[0]=%f ref=%f, C[%zu]=%f ref=%f\n",
               (double)h_gpu[0], (double)h_ref[0],
               c_n - 1, (double)h_gpu[c_n - 1], (double)h_ref[c_n - 1]);
    }

    free(h_a); free(h_b); free(h_gpu); free(h_ref);
    cudaFree(d_a); cudaFree(d_b); cudaFree(d_c);
    return ok ? 0 : 5;
}

static int run_one(int M, int K, int N, bool verbose)
{
    if (M % TILE_M || K % TILE_K || N % TILE_N) {
        fprintf(stderr,
                "[%dx%dx%d] SKIPPED — this kernel requires M/K/N "
                "multiples of %d (TILE_M=TILE_K=TILE_N).\n",
                M, K, N, TILE_M);
        return 0;  /* Not an error — caller responsibility today. */
    }

    size_t a_n = (size_t)M * K;
    size_t b_n = (size_t)K * N;
    size_t c_n = (size_t)M * N;

    half  *h_a = (half  *)malloc(a_n * sizeof(half));
    half  *h_b = (half  *)malloc(b_n * sizeof(half));
    float *h_c = (float *)malloc(c_n * sizeof(float));
    if (!h_a || !h_b || !h_c) { fprintf(stderr, "oom\n"); return 1; }

    /* All-ones smoke pattern. Same as gemm_fp32.cu so the per-cell
     * sentinel (= K) is comparable across kernels. */
    half one_h = __float2half(1.0f);
    for (size_t i = 0; i < a_n; i++) h_a[i] = one_h;
    /* B is col-major [K × N]: layout in memory is N columns of K
     * elements each, so all-ones works without thinking about it. */
    for (size_t i = 0; i < b_n; i++) h_b[i] = one_h;
    memset(h_c, 0, c_n * sizeof(float));

    half  *d_a; cudaMalloc(&d_a, a_n * sizeof(half));
    half  *d_b; cudaMalloc(&d_b, b_n * sizeof(half));
    float *d_c; cudaMalloc(&d_c, c_n * sizeof(float));
    cudaMemcpy(d_a, h_a, a_n * sizeof(half),  cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, b_n * sizeof(half),  cudaMemcpyHostToDevice);
    cudaMemset(d_c, 0, c_n * sizeof(float));

    dim3 block(32, 1, 1);                          /* one warp = 32 threads */
    dim3 grid((N + TILE_N - 1) / TILE_N,
              (M + TILE_M - 1) / TILE_M,
              1);
    gemm_hmma_fp16<<<grid, block>>>(d_a, d_b, d_c, M, K, N);
    cudaError_t kerr = cudaGetLastError();
    if (kerr != cudaSuccess) {
        fprintf(stderr, "[%dx%dx%d] kernel launch failed: %s\n",
                M, K, N, cudaGetErrorString(kerr));
        return 2;
    }
    cudaDeviceSynchronize();
    kerr = cudaGetLastError();
    if (kerr != cudaSuccess) {
        fprintf(stderr, "[%dx%dx%d] kernel run failed: %s\n",
                M, K, N, cudaGetErrorString(kerr));
        return 3;
    }

    cudaMemcpy(h_c, d_c, c_n * sizeof(float), cudaMemcpyDeviceToHost);

    /* Expected: K (because A and B are all 1.0). FP16 accumulated
     * into FP32 with K=16 should be exact; larger K may show tiny
     * deviation due to FP16 rep error if any element wasn't 1.0,
     * but with all-1.0 the result is exact through K=2048+. */
    int ok = 1;
    float expected = (float)K;
    for (size_t i = 0; i < c_n; i++) {
        if (h_c[i] != expected) {
            if (ok) {
                fprintf(stderr,
                        "[%dx%dx%d] MISMATCH C[%zu] = %f (want %f)\n",
                        M, K, N, i, (double)h_c[i], (double)expected);
            }
            ok = 0;
        }
    }

    if (ok && verbose) {
        printf("[%dx%dx%d] OK — all %zu cells = %f\n",
               M, K, N, c_n, (double)expected);
    } else if (ok) {
        printf("[%dx%dx%d] OK\n", M, K, N);
    }

    free(h_a); free(h_b); free(h_c);
    cudaFree(d_a); cudaFree(d_b); cudaFree(d_c);
    return ok ? 0 : 4;
}

int main(int argc, char **argv)
{
    bool verbose = false;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) verbose = true;

    int rc = 0;
    /* Smoke: all-1s, exact integer expected value. Bit-exact when
     * inputs are 1.0h. Catches launch / dispatch failures. */
    printf("=== smoke (all-ones) ===\n");
    rc |= run_one(  16,  16,  16, verbose);
    rc |= run_one(  16, 128,  16, verbose);
    rc |= run_one( 256, 256, 128, verbose);
    rc |= run_one(1024, 256, 512, verbose);

    /* Random: catches stride / layout / sign bugs that all-ones
     * hides. Tolerance accounts for FP16 input precision. */
    printf("=== random ===\n");
    rc |= run_random(  16,  16,  16, verbose);
    rc |= run_random(  16, 128,  16, verbose);
    rc |= run_random( 256, 256, 128, verbose);
    rc |= run_random(1024, 256, 512, verbose);

    /* MNIST-shaped (M=1, N=10 below tile size) — skipped by the
     * alignment check today. The launcher in #661 will pad M and
     * N to multiples of 16 before dispatch. */
    rc |= run_one(   1, 256,  10, verbose);
    return rc;
}
