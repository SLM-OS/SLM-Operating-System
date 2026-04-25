/*
 * matmul4x4.cu — 4×4 integer matrix multiply on Jetson GA10B.
 *
 * C = A * B, all int32_t 4×4 matrices in row-major layout.
 * Single-thread, single-CTA launch. 64 multiplies + 48 adds + 32
 * global loads + 16 global stores — next step up from dot4 (3 global
 * loads × 4 = 12, 4 mul + 3 add).
 *
 * Purpose: first matmul-shaped kernel in the tree. Exercises the
 * two-nested-loop inner kernel that every inference workload runs.
 * Single-thread so QMD configuration stays identical to dot4 /
 * write_cafe (1 CTA × 1 thread, REGISTER_COUNT_V=128 default).
 * Multi-thread scaling (thread-per-element, block-tiled, etc.) is a
 * separate follow-up that requires QMD grid/block dim tuning.
 *
 * Test vectors (pinned in the host `main` and the Linux-direct
 * launcher):
 *   A = [[ 1, 2, 3, 4],
 *        [ 5, 6, 7, 8],
 *        [ 9,10,11,12],
 *        [13,14,15,16]]
 *   B = Aᵀ (transpose)
 *   Expected C = A × Aᵀ = Gram matrix:
 *     C[0][0] =  1² +  2² +  3² +  4² =  30
 *     C[0][1] = 1·5 + 2·6 + 3·7 + 4·8 = 70
 *     C[1][1] = 5² + 6² + 7² + 8² = 174
 *     etc.
 *
 * C[0][0] = 30 is the SLM-OS-side poll sentinel (via v4
 * expected_payload). Full 4×4 result is validated by the host
 * launcher pre-kexec.
 *
 * Compile on Jetson:
 *   nvcc -arch=sm_87 -o matmul4x4 matmul4x4.cu
 *   cuobjdump --extract-elf all matmul4x4
 *   # .text._Z9matmul4x4PKiS0_Pi gets dd'd into matmul4x4_shader.sass
 */
#include <cstdio>
#include <cstdint>

__global__ void matmul4x4(const int *a, const int *b, int *c)
{
    #pragma unroll
    for (int i = 0; i < 4; i++) {
        #pragma unroll
        for (int j = 0; j < 4; j++) {
            int sum = 0;
            #pragma unroll
            for (int k = 0; k < 4; k++) {
                sum += a[i * 4 + k] * b[k * 4 + j];
            }
            c[i * 4 + j] = sum;
        }
    }
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

    matmul4x4<<<1, 1>>>(d_a, d_b, d_c);
    cudaDeviceSynchronize();

    int h_c[16] = {0};
    cudaMemcpy(h_c, d_c, 16 * sizeof(int), cudaMemcpyDeviceToHost);

    printf("C = A * Aᵀ (expected A² along the diagonal):\n");
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
    printf("C[0][0] = %d (sentinel for SLM-OS expected_payload)\n", h_c[0]);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    return all_ok ? 0 : 1;
}
