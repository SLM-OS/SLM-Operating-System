/*
 * maxpool2d_fp32.cu — fp32 windowed-max pooling on Jetson GA10B
 * (M4 of the MNIST-on-GPU plan).
 *
 * Math (matches ONNX MaxPool with auto_pad=NOTSET, pads=[0,0,0,0]):
 *   For each output cell (n, c, oh, ow):
 *     out[n,c,oh,ow] = max over (kh, kw) in [0, kH) × [0, kW) of
 *                      x[n, c, oh * stride_h + kh, ow * stride_w + kw]
 *
 * Output spatial dims:
 *   H_out = (H_in - kH) / stride_h + 1
 *   W_out = (W_in - kW) / stride_w + 1
 *
 * Mapping:
 *   blockIdx.z = n
 *   blockIdx.y = c
 *   blockIdx.x * blockDim.x + threadIdx.x = oh * W_out + ow
 *   one thread per output cell.
 *
 * cbuf[0] layout (CUDA ABI):
 *   [0x160] x         (const float *)
 *   [0x168] out       (float *)
 *   [0x170] N
 *   [0x174] C
 *   [0x178] H_in
 *   [0x17C] W_in
 *   [0x180] kH
 *   [0x184] kW
 *   [0x188] stride_h
 *   [0x18C] stride_w
 *   [0x190] H_out
 *   [0x194] W_out
 *
 * NOTE: The pointer args take 16 bytes (2 × 8 B); the first int
 * lands at 0x170 NOT 0x178. CUDA's ABI packs scalars after pointers
 * with minimum alignment per type — 4 B for int. Verify offsets via
 * cuobjdump after compile.
 *
 * Test cases per docs/jetson-gpu-mnist-plan.md §M4:
 *   - 1×8×28×28 → 1×8×14×14 (2×2 / stride 2)
 *   - 1×16×14×14 → 1×16×4×4 (3×3 / stride 3 — wastes 2 rows/cols
 *     at the bottom-right per ONNX SAME_UPPER=NOTSET semantics)
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cfloat>

__global__ void maxpool2d_fp32(const float *x, float *out,
                                int N, int C, int H_in, int W_in,
                                int kH, int kW,
                                int stride_h, int stride_w,
                                int H_out, int W_out)
{
    int n = blockIdx.z;
    int c = blockIdx.y;
    int oh_ow = blockIdx.x * blockDim.x + threadIdx.x;
    int spatial = H_out * W_out;
    if (n >= N || c >= C || oh_ow >= spatial) return;

    int oh = oh_ow / W_out;
    int ow = oh_ow % W_out;

    int h_base = oh * stride_h;
    int w_base = ow * stride_w;

    /* Init to first window element to avoid -inf bit pattern issues
     * in cross-platform polling. ONNX MaxPool with no padding
     * guarantees the window is fully in-bounds. */
    int idx_in0 = ((n * C + c) * H_in + h_base) * W_in + w_base;
    float m = x[idx_in0];

    #pragma unroll
    for (int dh = 0; dh < 7; dh++) {     /* upper bound for kH */
        if (dh >= kH) break;
        for (int dw = 0; dw < 7; dw++) { /* upper bound for kW */
            if (dw >= kW) break;
            if (dh == 0 && dw == 0) continue;
            int h = h_base + dh;
            int w = w_base + dw;
            int idx = ((n * C + c) * H_in + h) * W_in + w;
            float v = x[idx];
            if (v > m) m = v;
        }
    }

    int oidx = ((n * C + c) * H_out + oh) * W_out + ow;
    out[oidx] = m;
}

static void cpu_maxpool(const float *x, float *out,
                        int N, int C, int H_in, int W_in,
                        int kH, int kW, int sH, int sW)
{
    int H_out = (H_in - kH) / sH + 1;
    int W_out = (W_in - kW) / sW + 1;
    for (int n = 0; n < N; n++) {
        for (int c = 0; c < C; c++) {
            for (int oh = 0; oh < H_out; oh++) {
                for (int ow = 0; ow < W_out; ow++) {
                    float m = -FLT_MAX;
                    for (int dh = 0; dh < kH; dh++) {
                        for (int dw = 0; dw < kW; dw++) {
                            int h = oh * sH + dh;
                            int w = ow * sW + dw;
                            float v = x[((n * C + c) * H_in + h) * W_in + w];
                            if (v > m) m = v;
                        }
                    }
                    out[((n * C + c) * H_out + oh) * W_out + ow] = m;
                }
            }
        }
    }
}

static int run_one(int N, int C, int H_in, int W_in, int kH, int kW,
                   int sH, int sW)
{
    int H_out = (H_in - kH) / sH + 1;
    int W_out = (W_in - kW) / sW + 1;
    size_t in_n = (size_t)N * C * H_in * W_in;
    size_t out_n = (size_t)N * C * H_out * W_out;

    float *h_x = (float *)malloc(in_n * sizeof(float));
    float *h_out = (float *)malloc(out_n * sizeof(float));
    float *h_ref = (float *)malloc(out_n * sizeof(float));

    /* Test pattern: x[idx] = idx (so each window has a unique max
     * at its bottom-right corner). Validates spatial indexing. */
    for (size_t i = 0; i < in_n; i++) h_x[i] = (float)i;
    cpu_maxpool(h_x, h_ref, N, C, H_in, W_in, kH, kW, sH, sW);

    float *d_x, *d_out;
    cudaMalloc(&d_x, in_n * sizeof(float));
    cudaMalloc(&d_out, out_n * sizeof(float));
    cudaMemcpy(d_x, h_x, in_n * sizeof(float), cudaMemcpyHostToDevice);

    int spatial_out = H_out * W_out;
    dim3 block(256, 1, 1);
    dim3 grid((spatial_out + 255) / 256, C, N);
    maxpool2d_fp32<<<grid, block>>>(d_x, d_out,
                                     N, C, H_in, W_in,
                                     kH, kW, sH, sW,
                                     H_out, W_out);
    cudaDeviceSynchronize();
    cudaMemcpy(h_out, d_out, out_n * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    int report = 0;
    for (size_t i = 0; i < out_n; i++) {
        if (h_out[i] != h_ref[i]) {
            if (report < 4) {
                fprintf(stderr, "[%dx%dx%dx%d k=%dx%d s=%dx%d] "
                                "MISMATCH out[%zu]=%f want %f\n",
                        N, C, H_in, W_in, kH, kW, sH, sW, i,
                        (double)h_out[i], (double)h_ref[i]);
                report++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[N=%d C=%d %dx%d → %dx%d, k=%dx%d s=%dx%d] OK "
               "(out[0]=%.0f)\n",
               N, C, H_in, W_in, H_out, W_out, kH, kW, sH, sW,
               (double)h_out[0]);
    }

    cudaFree(d_x); cudaFree(d_out);
    free(h_x); free(h_out); free(h_ref);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* MNIST shapes per docs/jetson-gpu-mnist-plan.md §M4:
     *   MaxPool1: 1×8×28×28  → 1×8×14×14 (2×2 / stride 2)
     *   MaxPool2: 1×16×14×14 → 1×16×4×4  (3×3 / stride 3, drops
     *             the trailing 2 rows/cols per ONNX no-padding) */
    all_ok &= run_one(1, 8,  28, 28, 2, 2, 2, 2);
    all_ok &= run_one(1, 16, 14, 14, 3, 3, 3, 3);
    return all_ok ? 0 : 1;
}
