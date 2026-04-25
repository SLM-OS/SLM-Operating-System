/*
 * gemm_fp32.cu — Parameterized fp32 GEMM on Jetson GA10B
 * (M2 of the MNIST-on-GPU plan).
 *
 * First kernel in the SLM-OS tree to read its problem shape from
 * cbuf at dispatch time rather than baking M/K/N into the SASS.
 * One compiled blob handles every matmul shape MNIST and future
 * models will need.
 *
 * Mapping:
 *   global_row i = blockIdx.y * blockDim.y + threadIdx.y
 *   global_col j = blockIdx.x * blockDim.x + threadIdx.x
 *   if (i >= M || j >= N) return;       (edge handling)
 *   c[i,j] = sum_{k=0..K-1} a[i,k] * b[k,j]
 *
 * Launcher uses a 16×16-thread CTA and a ceil(N/16) × ceil(M/16)
 * grid, which gives a "thread per output cell" tiling that handles
 * any M×K×N up to ~65k×K×65k (CTA grid limit). For the MNIST FC
 * layer (M=1, K=256, N=10) most threads are idle but the kernel
 * still produces correct output — perf concerns are deferred to
 * future tile-cooperation work.
 *
 * cbuf[0] layout (CUDA ABI):
 *   [0x160] a (const float *)
 *   [0x168] b (const float *)
 *   [0x170] c (float *)
 *   [0x178] M (int32)
 *   [0x17C] K (int32)
 *   [0x180] N (int32)
 *
 * Test pattern in host main: A and B all 1.0f → C[i,j] = K for all
 * (i, j). Sentinel C[0][0] = K.0f bit pattern. Every shape tested
 * uses this pattern so the same launcher binary handles every
 * shape with one expected output table.
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o gemm_fp32 gemm_fp32.cu
 *   cuobjdump --extract-elf all gemm_fp32
 *   readelf -SW gemm_fp32.2.sm_87.cubin | grep text
 *   # dd .text._Z9gemm_fp32PKfS0_Pfiii into gemm_fp32_shader.sass
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

__global__ void gemm_fp32(const float *a, const float *b, float *c,
                          int M, int K, int N)
{
    int i = blockIdx.y * blockDim.y + threadIdx.y;
    int j = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= M || j >= N) return;

    float sum = 0.0f;
    for (int k = 0; k < K; k++) {
        sum += a[i * K + k] * b[k * N + j];
    }
    c[i * N + j] = sum;
}

static int run_one(int M, int K, int N)
{
    size_t a_n = (size_t)M * K;
    size_t b_n = (size_t)K * N;
    size_t c_n = (size_t)M * N;

    float *h_a = (float *)malloc(a_n * sizeof(float));
    float *h_b = (float *)malloc(b_n * sizeof(float));
    float *h_c = (float *)malloc(c_n * sizeof(float));
    if (!h_a || !h_b || !h_c) { fprintf(stderr, "oom\n"); return 1; }

    /* All-1s pattern: C[i][j] = sum_{k=0..K-1} 1*1 = K for all (i,j). */
    for (size_t i = 0; i < a_n; i++) h_a[i] = 1.0f;
    for (size_t i = 0; i < b_n; i++) h_b[i] = 1.0f;
    memset(h_c, 0, c_n * sizeof(float));

    float *d_a, *d_b, *d_c;
    cudaMalloc(&d_a, a_n * sizeof(float));
    cudaMalloc(&d_b, b_n * sizeof(float));
    cudaMalloc(&d_c, c_n * sizeof(float));

    cudaMemcpy(d_a, h_a, a_n * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, b_n * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(16, 16, 1);
    dim3 grid((N + 15) / 16, (M + 15) / 16, 1);
    gemm_fp32<<<grid, block>>>(d_a, d_b, d_c, M, K, N);
    cudaDeviceSynchronize();

    cudaMemcpy(h_c, d_c, c_n * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    float expected = (float)K;
    for (size_t i = 0; i < c_n; i++) {
        if (h_c[i] != expected) {
            if (ok) {
                fprintf(stderr, "[%dx%dx%d] MISMATCH C[%zu] = %f (want %f)\n",
                        M, K, N, i, (double)h_c[i], (double)expected);
            }
            ok = 0;
        }
    }

    if (ok) {
        uint32_t bits;
        memcpy(&bits, &expected, 4);
        printf("[%dx%dx%d] OK — all %zu cells = %.1f (bits=0x%08x)\n",
               M, K, N, c_n, (double)expected, bits);
    }

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    free(h_a);
    free(h_b);
    free(h_c);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* Test cases from docs/jetson-gpu-mnist-plan.md §M2:
     *   - 4×4 (regression vs matmul4x4)
     *   - 8×8 (regression vs matmul8x8_grid)
     *   - 1×256×10 (the MNIST FC layer)
     *   - 16×16 (tile-aligned, no edge case)
     *   - 17×17 (tile-misaligned, exercises edge handling) */
    all_ok &= run_one(4, 4, 4);
    all_ok &= run_one(8, 8, 8);
    all_ok &= run_one(1, 256, 10);
    all_ok &= run_one(16, 16, 16);
    all_ok &= run_one(17, 17, 17);
    return all_ok ? 0 : 1;
}
