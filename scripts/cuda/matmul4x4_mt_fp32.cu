/*
 * matmul4x4_mt_fp32.cu — fp32 4×4 matrix multiply on Jetson GA10B
 * (M1 of the MNIST-on-GPU plan).
 *
 * Same shape and CTA layout as matmul4x4_mt (16 threads in a 4×4
 * CTA, single-CTA grid). The change is purely numeric: int32 IMAD
 * arithmetic becomes fp32 FFMA. This is the first kernel in the
 * SLM-OS tree to dispatch fp32 SASS from raw nvgpu — every prior
 * kernel was integer.
 *
 * Test data: A = [1..16] row-major as fp32, B = Aᵀ, C = A·Aᵀ.
 * C[0][0] = 1²+2²+3²+4² = 30.0f. Bit pattern 0x41F00000 — exactly
 * representable in fp32, so polling for that exact bit pattern via
 * the v4 handoff's expected_payload works.
 *
 * Risks documented in docs/jetson-gpu-mnist-plan.md §M1:
 *   - REGISTER_COUNT_V may need to grow above 128 for FFMA chains.
 *     Check after compile via cuobjdump --extract-elf + reading
 *     EIATTR_REGCOUNT in .nv.info.
 *   - Polling exact fp32 bit pattern is sound for small integers
 *     in fp32; not portable for arbitrary computed results (M5
 *     onwards must use sentinel-via-zero + cross-check pattern).
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o matmul4x4_mt_fp32 matmul4x4_mt_fp32.cu
 *   cuobjdump --extract-elf all matmul4x4_mt_fp32
 *   readelf -SW matmul4x4_mt_fp32.2.sm_87.cubin | grep text
 *   # dd .text._Z16matmul4x4_mt_fp32PKfS0_Pf into .sass blob
 */
#include <cstdio>
#include <cstdint>

__global__ void matmul4x4_mt_fp32(const float *a, const float *b, float *c)
{
    int i = threadIdx.y;
    int j = threadIdx.x;
    float sum = 0.0f;
    #pragma unroll
    for (int k = 0; k < 4; k++) {
        sum += a[i * 4 + k] * b[k * 4 + j];
    }
    c[i * 4 + j] = sum;
}

int main()
{
    float h_a[16] = {
         1.f,  2.f,  3.f,  4.f,
         5.f,  6.f,  7.f,  8.f,
         9.f, 10.f, 11.f, 12.f,
        13.f, 14.f, 15.f, 16.f,
    };
    /* B = Aᵀ */
    float h_b[16] = {
        1.f, 5.f,  9.f, 13.f,
        2.f, 6.f, 10.f, 14.f,
        3.f, 7.f, 11.f, 15.f,
        4.f, 8.f, 12.f, 16.f,
    };
    float h_expected[16] = {
         30.f,  70.f, 110.f, 150.f,
         70.f, 174.f, 278.f, 382.f,
        110.f, 278.f, 446.f, 614.f,
        150.f, 382.f, 614.f, 846.f,
    };

    float *d_a, *d_b, *d_c;
    cudaMalloc(&d_a, 16 * sizeof(float));
    cudaMalloc(&d_b, 16 * sizeof(float));
    cudaMalloc(&d_c, 16 * sizeof(float));

    cudaMemcpy(d_a, h_a, 16 * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, 16 * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(4, 4, 1);
    dim3 grid(1, 1, 1);
    matmul4x4_mt_fp32<<<grid, block>>>(d_a, d_b, d_c);
    cudaDeviceSynchronize();

    float h_c[16] = {0};
    cudaMemcpy(h_c, d_c, 16 * sizeof(float), cudaMemcpyDeviceToHost);

    printf("C = A * Aᵀ (fp32 multi-threaded 4×4):\n");
    int all_ok = 1;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float v = h_c[i * 4 + j];
            float e = h_expected[i * 4 + j];
            uint32_t v_bits;
            __builtin_memcpy(&v_bits, &v, 4);
            printf("  C[%d][%d] = %7.1f  (bits=0x%08x, expected %7.1f)%s\n",
                   i, j, (double)v, v_bits, (double)e,
                   v == e ? "" : "  <-- MISMATCH");
            if (v != e) all_ok = 0;
        }
    }
    /* Print the sentinel bit pattern explicitly — the launcher and
     * SLM-OS both poll for this exact uint32 value. */
    uint32_t sent_bits;
    float sent = 30.0f;
    __builtin_memcpy(&sent_bits, &sent, 4);
    printf("Sentinel: C[0][0] = 30.0f → bit pattern 0x%08x\n", sent_bits);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_c);
    return all_ok ? 0 : 1;
}
