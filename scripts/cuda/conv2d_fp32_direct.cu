/*
 * conv2d_fp32_direct.cu — fp32 2D convolution on Jetson GA10B
 * (M5 of the MNIST-on-GPU plan, the largest single piece in the
 * plan).
 *
 * Direct convolution (no im2col). Each output cell is the sum over
 * (c_in, kH, kW) of weight × in-bounds input. Zero-padding on
 * out-of-bounds reads (matches ONNX Conv with `auto_pad=SAME_UPPER`
 * and the 5×5/stride-1 kernels MNIST uses, where SAME_UPPER
 * collapses to symmetric pad=2 because the kernel size is odd).
 *
 * Math (assumes stride=1, dilations=1, group=1 — all true for MNIST):
 *   out[n, c_out, oh, ow] = sum over (c_in, kh, kw) of
 *       w[c_out, c_in, kh, kw]
 *     * x[n, c_in, oh + kh - pad_h, ow + kw - pad_w]
 *   (out-of-bounds x reads contribute 0)
 *
 * Mapping:
 *   blockIdx.z = n
 *   blockIdx.y = c_out
 *   blockIdx.x * blockDim.x + threadIdx.x = oh * W + ow
 *
 * cbuf[0] layout (CUDA ABI):
 *   [0x160] x        (pointer)
 *   [0x168] w        (pointer)
 *   [0x170] out      (pointer)
 *   [0x178] N
 *   [0x17C] C_in
 *   [0x180] H
 *   [0x184] W
 *   [0x188] C_out
 *   [0x18C] kH
 *   [0x190] kW
 *   [0x194] pad_h
 *   [0x198] pad_w
 *
 * Test cases per docs/jetson-gpu-mnist-plan.md §M5:
 *   - Conv1: x [1,1,28,28], w [8,1,5,5]    → out [1,8,28,28], pad=2
 *   - Conv2: x [1,8,14,14], w [16,8,5,5]   → out [1,16,14,14], pad=2
 *
 * Test pattern: x = all 1.0f, w = all 1.0f. Result for an interior
 * output cell = C_in * kH * kW (no padding effect); result for a
 * corner cell is reduced by the number of out-of-bounds reads.
 *
 * For the sentinel: the top-left corner of out (oh=0, ow=0) loses
 * the top-left (pad_h × pad_w) corner of the kernel window, the
 * entire top pad_h rows, and the entire left pad_w columns. With
 * pad=2, kH=kW=5, the in-bounds portion is (kH-pad_h) × (kW-pad_w)
 * = 3 × 3 = 9 cells per input channel, so the corner value is
 * 9 * C_in.
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o conv2d_fp32_direct conv2d_fp32_direct.cu
 *   cuobjdump --extract-elf all conv2d_fp32_direct
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

__global__ void conv2d_fp32_direct(
    const float *x, const float *w, float *out,
    int N, int C_in, int H, int W,
    int C_out, int kH, int kW,
    int pad_h, int pad_w)
{
    int n = blockIdx.z;
    int c_out = blockIdx.y;
    int oh_ow = blockIdx.x * blockDim.x + threadIdx.x;
    int spatial = H * W;
    if (n >= N || c_out >= C_out || oh_ow >= spatial) return;

    int oh = oh_ow / W;
    int ow = oh_ow % W;

    float sum = 0.0f;
    for (int c_in = 0; c_in < C_in; c_in++) {
        for (int kh = 0; kh < kH; kh++) {
            int ih = oh + kh - pad_h;
            if (ih < 0 || ih >= H) continue;
            for (int kw = 0; kw < kW; kw++) {
                int iw = ow + kw - pad_w;
                if (iw < 0 || iw >= W) continue;
                int xi = ((n * C_in + c_in) * H + ih) * W + iw;
                int wi = ((c_out * C_in + c_in) * kH + kh) * kW + kw;
                sum += x[xi] * w[wi];
            }
        }
    }

    int oi = ((n * C_out + c_out) * H + oh) * W + ow;
    out[oi] = sum;
}

static void cpu_conv(const float *x, const float *w, float *out,
                     int N, int C_in, int H, int W,
                     int C_out, int kH, int kW,
                     int pad_h, int pad_w)
{
    for (int n = 0; n < N; n++) {
        for (int co = 0; co < C_out; co++) {
            for (int oh = 0; oh < H; oh++) {
                for (int ow = 0; ow < W; ow++) {
                    float s = 0.0f;
                    for (int ci = 0; ci < C_in; ci++) {
                        for (int kh = 0; kh < kH; kh++) {
                            int ih = oh + kh - pad_h;
                            if (ih < 0 || ih >= H) continue;
                            for (int kw = 0; kw < kW; kw++) {
                                int iw = ow + kw - pad_w;
                                if (iw < 0 || iw >= W) continue;
                                int xi = ((n * C_in + ci) * H + ih) * W + iw;
                                int wi = ((co * C_in + ci) * kH + kh) * kW + kw;
                                s += x[xi] * w[wi];
                            }
                        }
                    }
                    out[((n * C_out + co) * H + oh) * W + ow] = s;
                }
            }
        }
    }
}

static int run_one(int N, int C_in, int H, int W,
                   int C_out, int kH, int kW,
                   int pad_h, int pad_w)
{
    size_t in_n  = (size_t)N * C_in * H * W;
    size_t w_n   = (size_t)C_out * C_in * kH * kW;
    size_t out_n = (size_t)N * C_out * H * W;

    float *h_x   = (float *)malloc(in_n  * sizeof(float));
    float *h_w   = (float *)malloc(w_n   * sizeof(float));
    float *h_out = (float *)malloc(out_n * sizeof(float));
    float *h_ref = (float *)malloc(out_n * sizeof(float));

    for (size_t i = 0; i < in_n; i++) h_x[i] = 1.0f;
    for (size_t i = 0; i < w_n;  i++) h_w[i] = 1.0f;
    cpu_conv(h_x, h_w, h_ref, N, C_in, H, W, C_out, kH, kW, pad_h, pad_w);

    float *d_x, *d_w, *d_out;
    cudaMalloc(&d_x,   in_n  * sizeof(float));
    cudaMalloc(&d_w,   w_n   * sizeof(float));
    cudaMalloc(&d_out, out_n * sizeof(float));
    cudaMemcpy(d_x, h_x, in_n  * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_w, h_w, w_n   * sizeof(float), cudaMemcpyHostToDevice);

    int spatial = H * W;
    dim3 block(256, 1, 1);
    dim3 grid((spatial + 255) / 256, C_out, N);
    conv2d_fp32_direct<<<grid, block>>>(d_x, d_w, d_out,
                                         N, C_in, H, W,
                                         C_out, kH, kW,
                                         pad_h, pad_w);
    cudaDeviceSynchronize();
    cudaMemcpy(h_out, d_out, out_n * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    int report = 0;
    for (size_t i = 0; i < out_n; i++) {
        if (h_out[i] != h_ref[i]) {
            if (report < 4) {
                fprintf(stderr, "  [N=%d C_in=%d H=%d C_out=%d k=%d] "
                                "MISMATCH out[%zu]=%f want %f\n",
                        N, C_in, H, C_out, kH, i,
                        (double)h_out[i], (double)h_ref[i]);
                report++;
            }
            ok = 0;
        }
    }
    if (ok) {
        printf("[N=%d C_in=%d %dx%d → C_out=%d k=%dx%d pad=%d] OK "
               "(corner=%.0f, interior=%.0f)\n",
               N, C_in, H, W, C_out, kH, kW, pad_h,
               (double)h_out[0],
               (double)h_out[((C_out / 2) * H + H / 2) * W + W / 2]);
    }

    cudaFree(d_x); cudaFree(d_w); cudaFree(d_out);
    free(h_x); free(h_w); free(h_out); free(h_ref);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* MNIST shapes per docs/jetson-gpu-mnist-plan.md §M5. */
    all_ok &= run_one(1, 1,  28, 28, 8,  5, 5, 2, 2);  /* Conv1 */
    all_ok &= run_one(1, 8,  14, 14, 16, 5, 5, 2, 2);  /* Conv2 */
    return all_ok ? 0 : 1;
}
