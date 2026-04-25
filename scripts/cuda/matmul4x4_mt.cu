/*
 * matmul4x4_mt.cu — Multi-threaded 4×4 integer matrix multiply on
 * Jetson GA10B. Same math as matmul4x4.cu but with a 4×4 thread
 * block: each thread computes one output element independently.
 *
 * First truly parallel kernel in the tree. Previous kernels ran on
 * 1 thread of 1 CTA (1 / 1024 of the GPU); this one launches 16
 * threads of a single 4×4-shaped CTA. Still single-CTA, so grid
 * dims stay 1×1×1 — scaling beyond that's the next follow-up.
 *
 * What changes vs matmul4x4:
 *   - Kernel reads threadIdx.x/y (SR_TID hardware registers) rather
 *     than iterating with software counters.
 *   - QMD needs CTA_THREAD_DIMENSION0 = 4, DIMENSION1 = 4 (was 1,1,1).
 *   - Each thread writes exactly one cell; no conflicts.
 *
 * Launcher: scripts/gpu-kernel-matmul4x4-mt.c. Validates all 16
 * cells and uses C[0][0] = 30 as the SLM-OS sentinel.
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o matmul4x4_mt matmul4x4_mt.cu
 *   cuobjdump --extract-elf all matmul4x4_mt
 *   # dd .text._Z11matmul4x4_mtPKiS0_Pi section into
 *   #   matmul4x4_mt_shader.sass
 */
#include <cstdio>
#include <cstdint>

__global__ void matmul4x4_mt(const int *a, const int *b, int *c)
{
    int i = threadIdx.y;
    int j = threadIdx.x;
    int sum = 0;
    #pragma unroll
    for (int k = 0; k < 4; k++) {
        sum += a[i * 4 + k] * b[k * 4 + j];
    }
    c[i * 4 + j] = sum;
}

int main()
{
    int h_a[16] = {
         1,  2,  3,  4,
         5,  6,  7,  8,
         9, 10, 11, 12,
        13, 14, 15, 16,
    };
    /* B = Aᵀ */
    int h_b[16] = {
        1, 5,  9, 13,
        2, 6, 10, 14,
        3, 7, 11, 15,
        4, 8, 12, 16,
    };
    int h_expected[16] = {
         30,  70, 110, 150,
         70, 174, 278, 382,
        110, 278, 446, 614,
        150, 382, 614, 846,
    };

    int *d_a, *d_b, *d_c;
    cudaMalloc(&d_a, 16 * sizeof(int));
    cudaMalloc(&d_b, 16 * sizeof(int));
    cudaMalloc(&d_c, 16 * sizeof(int));

    cudaMemcpy(d_a, h_a, 16 * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, 16 * sizeof(int), cudaMemcpyHostToDevice);

    dim3 block(4, 4, 1);
    dim3 grid(1, 1, 1);
    matmul4x4_mt<<<grid, block>>>(d_a, d_b, d_c);
    cudaDeviceSynchronize();

    int h_c[16] = {0};
    cudaMemcpy(h_c, d_c, 16 * sizeof(int), cudaMemcpyDeviceToHost);

    printf("C = A * Aᵀ (multi-threaded 4×4):\n");
    int all_ok = 1;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int v = h_c[i * 4 + j];
            int e = h_expected[i * 4 + j];
            printf("  C[%d][%d] = %5d  (expected %5d)%s\n",
                   i, j, v, e, v == e ? "" : "  <-- MISMATCH");
            if (v != e) all_ok = 0;
        }
    }

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    return all_ok ? 0 : 1;
}
