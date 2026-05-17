/*
 * sigmoid_fp32.cu — Element-wise fp32 sigmoid on Jetson GA10B
 * (PR-5 of the GPU eviction-policy plan, docs/design/gpu-policy-models.md §A).
 *
 * Math:
 *   out[i] = 1.0f / (1.0f + expf(-x[i]))
 *
 * The eviction MLP (`runtime/src/mm/eviction/runtime_mlp.rs::predict`)
 * applies sigmoid to a single fp32 scalar at the end of its 27→64→32→
 * 16→1 forward pass. The kernel is written for an arbitrary N so the
 * same SASS can drive a future per-candidate batched dispatch
 * (CACHEUS-style ensembles) without re-extraction.
 *
 * Numerical match: the runtime CPU reference uses `libm::expf`
 * (no_std fallback for `f32::exp`), so this kernel uses the libdevice
 * `expf` rather than the `__expf` intrinsic — the launcher's
 * GPU/CPU agreement gate runs at 1e-4 absolute and that headroom is
 * what we spend the libdevice precision on.
 *
 * Mapping:
 *   blockIdx.x * blockDim.x + threadIdx.x = i (linear element)
 *   one thread per output element
 *
 * cbuf[0] layout (CUDA ABI — matches add_bias_relu_fp32 / gemm_fp32
 * conventions, scalars right-packed against the pointer triple):
 *   [0x160] x       (const float *)
 *   [0x168] out     (float *)
 *   [0x170] N       (int32_t element count)
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o sigmoid_fp32 sigmoid_fp32.cu
 *   cuobjdump --extract-elf all sigmoid_fp32
 *   readelf -SW sigmoid_fp32.2.sm_87.cubin | grep text
 */
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>

__global__ void sigmoid_fp32(const float *x, float *out, int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= N) return;
    out[i] = 1.0f / (1.0f + expf(-x[i]));
}

/* CPU reference — identical to the runtime path at
 * runtime/src/mm/eviction/runtime_mlp.rs::predict line 144:
 *   1.0 / (1.0 + libm::expf(-out))
 * libm::expf wraps the same C runtime expf, so bit-equivalent to
 * `expf` from <cmath>. */
static float sigmoid_ref(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

static int run_one(int N, const float *inputs)
{
    float *h_x   = (float *)malloc(N * sizeof(float));
    float *h_out = (float *)malloc(N * sizeof(float));
    if (!h_x || !h_out) { fprintf(stderr, "oom\n"); return 0; }
    for (int i = 0; i < N; i++) h_x[i] = inputs[i];

    float *d_x, *d_out;
    cudaMalloc(&d_x, N * sizeof(float));
    cudaMalloc(&d_out, N * sizeof(float));
    cudaMemcpy(d_x, h_x, N * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(256, 1, 1);
    dim3 grid((N + 255) / 256, 1, 1);
    sigmoid_fp32<<<grid, block>>>(d_x, d_out, N);
    cudaDeviceSynchronize();

    cudaMemcpy(h_out, d_out, N * sizeof(float), cudaMemcpyDeviceToHost);

    int ok = 1;
    int reported = 0;
    float max_abs_err = 0.0f;
    for (int i = 0; i < N; i++) {
        float expected = sigmoid_ref(h_x[i]);
        float err = fabsf(h_out[i] - expected);
        if (err > max_abs_err) max_abs_err = err;
        /* PR-5's launcher-level gate is 1e-4 absolute. The
         * per-element kernel itself should be tighter — 1e-6 absolute
         * is achievable since the operation is a single libdevice
         * expf + reciprocal.
         *
         * Mixed abs/rel pattern so the test is robust at the tails
         * of the test sweep (sigmoid(±10), sigmoid(±30)) and across
         * future CUDA toolchain bumps that might shift libdevice
         * expf by an extra ULP. We pass if EITHER:
         *   - absolute error is ≤ 1e-6 (the tight target), OR
         *   - relative error is ≤ 1e-5 (10× looser, dominates near
         *     the saturation asymptote where fp32 representation
         *     itself rounds away the sub-ULP differences).
         * Both gates are still 100× tighter than the launcher
         * tolerance the rest of PR-5 depends on. */
        float tol = 1e-6f;
        if (1e-5f * fabsf(expected) > tol) tol = 1e-5f * fabsf(expected);
        if (err > tol) {
            if (reported < 4) {
                fprintf(stderr,
                        "[N=%d] MISMATCH out[%d] = %.9f "
                        "(want %.9f, |err|=%.3e, tol=%.3e, input=%.4f)\n",
                        N, i, (double)h_out[i], (double)expected,
                        (double)err, (double)tol, (double)h_x[i]);
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[N=%d] OK — max |err| = %.3e (in[0]=%.3f → out[0]=%.6f)\n",
               N, (double)max_abs_err,
               (double)h_x[0], (double)h_out[0]);
    } else {
        printf("[N=%d] FAIL — max |err| = %.3e\n",
               N, (double)max_abs_err);
    }

    cudaFree(d_x); cudaFree(d_out);
    free(h_x); free(h_out);
    return ok;
}

int main()
{
    int all_ok = 1;

    /* N=1 — the steady-state eviction MLP shape (single scalar
     * output after the four-layer forward pass). */
    {
        float inputs[1] = { 0.42f };
        all_ok &= run_one(1, inputs);
    }

    /* N=11 — sweep across input magnitudes covering the well-behaved
     * range and the saturation tails. Sigmoid(-30) ≈ 9.36e-14,
     * sigmoid(+30) ≈ 1 - 9.36e-14; both saturate to fp32 endpoints
     * within machine epsilon. */
    {
        float inputs[11] = {
            -30.0f, -10.0f, -3.0f, -1.0f, -0.5f,
              0.0f,
              0.5f,   1.0f,  3.0f, 10.0f, 30.0f,
        };
        all_ok &= run_one(11, inputs);
    }

    /* N=8 — batched shape a future CACHEUS-on-GPU dispatch could
     * use (one fp32 score per candidate, 8-candidate snapshot). */
    {
        float inputs[8] = {
            -2.5f, -1.0f, -0.25f, -0.1f, 0.1f, 0.25f, 1.0f, 2.5f,
        };
        all_ok &= run_one(8, inputs);
    }

    return all_ok ? 0 : 1;
}
