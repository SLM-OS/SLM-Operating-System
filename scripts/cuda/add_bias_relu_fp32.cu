/*
 * add_bias_relu_fp32.cu — Fused fp32 Add-bias + ReLU on Jetson GA10B
 * (M3 of the MNIST-on-GPU plan).
 *
 * MNIST hits this pattern three times: after each Conv (with ReLU)
 * and once after the final MatMul (without ReLU). The kernel takes
 * an `apply_relu` flag so one SASS handles both cases.
 *
 * Math:
 *   out[n, c, h, w] = x[n, c, h, w] + bias[c]
 *   if apply_relu: out = max(0, out)
 *
 * Bias is per-channel (broadcasts over spatial dims). For the post-
 * MatMul Add (which has no spatial dims), the launcher passes
 * H = W = 1 so the broadcast becomes the identity.
 *
 * Mapping:
 *   blockIdx.z = batch n
 *   blockIdx.y = channel c
 *   blockIdx.x * blockDim.x + threadIdx.x = h*W + w (linear spatial)
 *   idx = ((n * C + c) * H * W) + (h*W + w)
 *   one thread per output element
 *
 * cbuf[0] layout (CUDA ABI):
 *   [0x160] x          (const float *)
 *   [0x168] bias       (const float *)
 *   [0x170] out        (float *)
 *   [0x178] N
 *   [0x17C] C
 *   [0x180] H
 *   [0x184] W
 *   [0x188] apply_relu (0 or 1)
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o add_bias_relu_fp32 add_bias_relu_fp32.cu
 *   cuobjdump --extract-elf all add_bias_relu_fp32
 *   readelf -SW add_bias_relu_fp32.2.sm_87.cubin | grep text
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

__global__ void add_bias_relu_fp32(const float *x, const float *bias,
                                    float *out,
                                    int N, int C, int H, int W,
                                    int apply_relu)
{
    int n = blockIdx.z;
    int c = blockIdx.y;
    int wh = blockIdx.x * blockDim.x + threadIdx.x;
    int spatial = H * W;
    if (n >= N || c >= C || wh >= spatial) return;

    int idx = ((n * C + c) * spatial) + wh;
    float v = x[idx] + bias[c];
    if (apply_relu && v < 0.0f) v = 0.0f;
    out[idx] = v;
}

static int run_one(int N, int C, int H, int W, int apply_relu)
{
    size_t total = (size_t)N * C * H * W;

    float *h_x    = (float *)malloc(total * sizeof(float));
    float *h_bias = (float *)malloc(C * sizeof(float));
    float *h_out  = (float *)malloc(total * sizeof(float));
    if (!h_x || !h_bias || !h_out) { fprintf(stderr, "oom\n"); return 1; }

    /* Test pattern: x = all 1.0f, bias[c] = c.
     * Result for relu=1: out[n,c,h,w] = max(0, 1.0 + c) = 1.0 + c
     * (since 1.0 + c >= 0 for c >= 0). */
    for (size_t i = 0; i < total; i++) h_x[i] = 1.0f;
    for (int c = 0; c < C; c++) h_bias[c] = (float)c;

    float *d_x, *d_bias, *d_out;
    cudaMalloc(&d_x, total * sizeof(float));
    cudaMalloc(&d_bias, C * sizeof(float));
    cudaMalloc(&d_out, total * sizeof(float));
    cudaMemcpy(d_x, h_x, total * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_bias, h_bias, C * sizeof(float), cudaMemcpyHostToDevice);

    int spatial = H * W;
    dim3 block(256, 1, 1);
    dim3 grid((spatial + 255) / 256, C, N);
    add_bias_relu_fp32<<<grid, block>>>(d_x, d_bias, d_out,
                                        N, C, H, W, apply_relu);
    cudaDeviceSynchronize();

    cudaMemcpy(h_out, d_out, total * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    int reported = 0;
    for (int n = 0; n < N; n++) {
        for (int c = 0; c < C; c++) {
            float expected = 1.0f + (float)c;
            for (int hw = 0; hw < spatial; hw++) {
                size_t idx = ((size_t)n * C + c) * spatial + hw;
                if (h_out[idx] != expected) {
                    if (reported < 4) {
                        fprintf(stderr,
                                "[%dx%dx%dx%d relu=%d] MISMATCH "
                                "out[%zu] = %f (want %f)\n",
                                N, C, H, W, apply_relu, idx,
                                (double)h_out[idx], (double)expected);
                        reported++;
                    }
                    ok = 0;
                }
            }
        }
    }

    if (ok) {
        printf("[N=%d C=%d H=%d W=%d relu=%d] OK — out[0]=%.1f\n",
               N, C, H, W, apply_relu, (double)h_out[0]);
    }

    cudaFree(d_x); cudaFree(d_bias); cudaFree(d_out);
    free(h_x); free(h_bias); free(h_out);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* MNIST shapes per docs/jetson-gpu-mnist-plan.md §M3:
     *   Conv1 output:  1×8×28×28  with ReLU
     *   Conv2 output:  1×16×14×14 with ReLU
     *   Final FC:      1×10×1×1   without ReLU */
    all_ok &= run_one(1, 8, 28, 28, 1);
    all_ok &= run_one(1, 16, 14, 14, 1);
    all_ok &= run_one(1, 10, 1, 1, 0);
    return all_ok ? 0 : 1;
}
