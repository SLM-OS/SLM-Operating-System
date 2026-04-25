/*
 * dot4.cu — Minimum viable "real compute" kernel for SLM-OS GPU
 * validation on Jetson GA10B.
 *
 * Computes a 4-element integer dot product: out = Σ a[i] * b[i].
 * Single-thread, single-CTA launch (<<<1,1>>>) so QMD fields match
 * the write_cafe smoke test's: REGISTER_COUNT_V=128, grid=1×1×1,
 * block=1×1×1, no shared memory, no SLM. Exercises real arithmetic
 * (4 multiplies, 3 adds) and multiple global loads — the next
 * meaningful milestone after the write_cafe store-only kernel.
 *
 * Arguments via CUDA's cbuf[0] convention at byte offset 0x160:
 *   0x160: a_ptr  (8 B)
 *   0x168: b_ptr  (8 B)
 *   0x170: out_ptr (8 B)
 *
 * Host-side test values: a = {1, 2, 3, 4}, b = {10, 20, 30, 40}.
 * Expected result: 1*10 + 2*20 + 3*30 + 4*40 = 300.
 *
 * Compile + extract SASS on a Jetson with CUDA 12.6 toolchain:
 *   nvcc -arch=sm_87 -o dot4 dot4.cu
 *   cuobjdump --extract-elf all dot4
 *   dd if=dot4.2.sm_87.cubin of=dot4_shader.sass \
 *     bs=1 skip=$(section_offset) count=$(section_size)
 *
 * (The smoke-test runner validates both the kernel output and the
 * SASS-extraction recipe; see scripts/gpu-kernel-launch.c for the
 * analogous write_cafe pattern.)
 */
#include <cstdio>
#include <cstdint>

__global__ void dot4(const int *a, const int *b, int *out)
{
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += a[i] * b[i];
    }
    *out = sum;
}

int main()
{
    int h_a[4] = { 1, 2, 3, 4 };
    int h_b[4] = { 10, 20, 30, 40 };

    int *d_a, *d_b, *d_out;
    cudaMalloc(&d_a, 4 * sizeof(int));
    cudaMalloc(&d_b, 4 * sizeof(int));
    cudaMalloc(&d_out, sizeof(int));

    cudaMemcpy(d_a, h_a, 4 * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_b, h_b, 4 * sizeof(int), cudaMemcpyHostToDevice);

    dot4<<<1, 1>>>(d_a, d_b, d_out);
    cudaDeviceSynchronize();

    int h_out = 0;
    cudaMemcpy(&h_out, d_out, sizeof(int), cudaMemcpyDeviceToHost);
    printf("dot4 result = %d (expected 300)\n", h_out);

    cudaFree(d_a);
    cudaFree(d_b);
    cudaFree(d_out);
    return (h_out == 300) ? 0 : 1;
}
