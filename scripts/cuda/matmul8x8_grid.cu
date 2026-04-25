/*
 * matmul8x8_grid.cu — 8×8 integer matrix multiply on Jetson GA10B,
 * dispatched as a 2×2 grid of 4×4-thread CTAs. Each CTA computes
 * one 4×4 tile of the 8×8 output C.
 *
 * First multi-CTA kernel in the SLM-OS tree. Previous parallelism
 * lived inside a single CTA (matmul4x4_mt: 16 threads, 1 CTA).
 * This kernel exercises the QMD's CTA_RASTER_WIDTH / CTA_RASTER_HEIGHT
 * fields (currently hardwired to 1 in gpu_launch_populate_qmd) and
 * the kernel reads SR_CTAID alongside SR_TID to map a (block, thread)
 * pair to a single output cell.
 *
 * Mapping:
 *   block_row    = blockIdx.y      (0 or 1)
 *   block_col    = blockIdx.x      (0 or 1)
 *   thread_row   = threadIdx.y     (0..3)
 *   thread_col   = threadIdx.x     (0..3)
 *   global_row i = block_row * 4 + thread_row
 *   global_col j = block_col * 4 + thread_col
 *   C[i][j] = sum_{k=0..7} A[i][k] * B[k][j]
 *
 * QMD deltas vs matmul4x4_mt:
 *   CTA_RASTER_WIDTH  : 1 → 2
 *   CTA_RASTER_HEIGHT : 1 → 2
 *   CTA_THREAD_DIM0/1 : stay at 4
 *
 * Test data (pinned in main): A = [1..64] row-major, B = Aᵀ.
 * Expected C = A·Aᵀ Gram matrix; C[0][0] = 1²+2²+...+8² = 204.
 *
 * SLM-OS sentinel: C[0][0] = 204 (= 0xCC).
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o matmul8x8_grid matmul8x8_grid.cu
 *   cuobjdump --extract-elf all matmul8x8_grid
 *   readelf -SW matmul8x8_grid.2.sm_87.cubin | grep text
 *   # dd .text._Z14matmul8x8_gridPKiS0_Pi section into
 *   #   matmul8x8_grid_shader.sass
 */
#include <cstdio>
#include <cstdint>

__global__ void matmul8x8_grid(const int *a, const int *b, int *c)
{
    int i = blockIdx.y * 4 + threadIdx.y;
    int j = blockIdx.x * 4 + threadIdx.x;
    int sum = 0;
    #pragma unroll
    for (int k = 0; k < 8; k++) {
        sum += a[i * 8 + k] * b[k * 8 + j];
    }
    c[i * 8 + j] = sum;
}

int main()
{
    int h_a[64];
    int h_b[64];
    for (int v = 0; v < 64; v++) h_a[v] = v + 1;  /* A = [1..64] row-major */

    /* B = Aᵀ: B[i][j] = A[j][i] */
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            h_b[i * 8 + j] = h_a[j * 8 + i];
        }
    }

    /* Reference: C = A * Aᵀ (Gram matrix). */
    int h_expected[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int s = 0;
            for (int k = 0; k < 8; k++) {
                s += h_a[i * 8 + k] * h_b[k * 8 + j];
            }
            h_expected[i * 8 + j] = s;
        }
    }

    int *d_a, *d_b, *d_c;
    cudaMalloc(&d_a, 64 * sizeof(int));
    cudaMalloc(&d_b, 64 * sizeof(int));
    cudaMalloc(&d_c, 64 * sizeof(int));

    cudaMemcpy(d_a, h_a, 64 * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, 64 * sizeof(int), cudaMemcpyHostToDevice);

    /* 2×2 grid of 4×4-thread CTAs. */
    dim3 block(4, 4, 1);
    dim3 grid(2, 2, 1);
    matmul8x8_grid<<<grid, block>>>(d_a, d_b, d_c);
    cudaDeviceSynchronize();

    int h_c[64] = {0};
    cudaMemcpy(h_c, d_c, 64 * sizeof(int), cudaMemcpyDeviceToHost);

    printf("C = A * Aᵀ (multi-CTA 8×8, 2×2 grid of 4×4 CTAs):\n");
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int v = h_c[i * 8 + j];
            int e = h_expected[i * 8 + j];
            printf(" %6d", v);
            if (v != e) all_ok = 0;
        }
        printf("\n");
    }
    printf("C[0][0] = %d  (expected 204, sentinel for SLM-OS)\n", h_c[0]);
    printf("all_ok = %d\n", all_ok);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    return all_ok ? 0 : 1;
}
