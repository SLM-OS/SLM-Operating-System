/*
 * swiglu_f16.cu — Fused SiLU + element-wise multiply on Jetson GA10B
 * (third SLM-on-GPU operator from the #540 menu).
 *
 * SwiGLU is the FFN activation used once per transformer layer in
 * Qwen2.5 / Llama / Phi-style SLMs. The full MLP block is
 *
 *     out = down(silu(gate(x)) * up(x))
 *
 * where `gate` and `up` are linear projections from hidden_size to
 * intermediate_size, `down` projects back, and the element-wise step
 *
 *     silu(g_i) * u_i = (g_i / (1 + exp(-g_i))) * u_i
 *
 * sits between the two matmul layers. This kernel implements that
 * element-wise step. Combined with the future Q4K_DOT.SIMT.Q4K (for
 * the gate / up / down matmuls), the full SwiGLU block becomes
 * Q4K_DOT(gate) → Q4K_DOT(up) → SWIGLU.SIMT.FP16 → Q4K_DOT(down).
 *
 * Mirrors `runtime/src/inference/ops_transformer.rs::silu_out` plus
 * the element-wise multiply that follows it inside `swiglu_mlp_q`.
 * Inputs and outputs are FP16; sigmoid is computed in FP32 to keep
 * `expf` precise across the gate's full range.
 *
 * Mapping (1D grid, one thread per element):
 *   blockIdx.x * blockDim.x + threadIdx.x = element index
 *   one thread per output FP16 element
 *
 * Element-wise pure: no shared memory, no reductions, no cross-thread
 * communication. The launch shape (rows × intermediate_size) is
 * flattened to a single int `n` — the kernel is row-agnostic.
 *
 * cbuf[0] layout (CUDA ABI for sm_87, mirrors gemm_hmma_fp16 / rmsnorm):
 *   [0x160] gate (const half *)   // [n] post-gate-projection FP16
 *   [0x168] up   (const half *)   // [n] post-up-projection FP16
 *   [0x170] out  (half *)         // [n] silu(gate) * up
 *   [0x178] n    (int)            // total element count (rows × intermediate)
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o swiglu_f16 swiglu_f16.cu
 *   cuobjdump --extract-elf all swiglu_f16
 *   readelf -SW swiglu_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define BLOCK_DIM 256

__global__ void swiglu_f16(const half *gate, const half *up, half *out, int n)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= n) return;

    float g = __half2float(gate[idx]);
    float u = __half2float(up[idx]);
    /* SiLU(g) = g * sigmoid(g) = g / (1 + exp(-g)). Computed in FP32
     * because expf precision drops sharply at small magnitudes when
     * narrowed to FP16 mid-computation. */
    float silu_g = g / (1.0f + __expf(-g));
    out[idx] = __float2half(silu_g * u);
}

/* ---- CPU reference (mirrors runtime/src/inference/ops_transformer.rs) ---- */

static void cpu_reference(const half *gate, const half *up, half *out, int n)
{
    for (int i = 0; i < n; i++) {
        float g = __half2float(gate[i]);
        float u = __half2float(up[i]);
        float silu_g = g / (1.0f + expf(-g));
        out[i] = __float2half(silu_g * u);
    }
}

/* ---- Fixture builder + harness ---- */

static uint16_t half_bits(half h)
{
    uint16_t b;
    memcpy(&b, &h, sizeof(b));
    return b;
}

static int abs_ulp(half a, half b)
{
    int ai = (int)half_bits(a);
    int bi = (int)half_bits(b);
    int d  = ai - bi;
    return d < 0 ? -d : d;
}

static int run_one(int n, uint32_t seed_in, const char *label, int max_ulps)
{
    half *h_gate = (half *)malloc(n * sizeof(half));
    half *h_up   = (half *)malloc(n * sizeof(half));
    half *h_gpu  = (half *)malloc(n * sizeof(half));
    half *h_cpu  = (half *)malloc(n * sizeof(half));
    if (!h_gate || !h_up || !h_gpu || !h_cpu) {
        fprintf(stderr, "oom\n");
        return 0;
    }

    /* gate ~ uniform(-3, 3) — covers SiLU's full transition zone
     * (silu(-3) ≈ -0.142, silu(0) = 0, silu(+3) ≈ 2.857).
     * up   ~ uniform(-1, 1) — typical residual-stream magnitudes. */
    uint32_t seed = seed_in;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float gv = ((float)(seed & 0xFFFF) / 65535.0f) * 6.0f - 3.0f;
        h_gate[i] = __float2half(gv);
        seed = seed * 1664525u + 1013904223u;
        float uv = ((float)(seed & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
        h_up[i] = __float2half(uv);
    }

    cpu_reference(h_gate, h_up, h_cpu, n);

    half *d_gate, *d_up, *d_out;
    cudaMalloc(&d_gate, n * sizeof(half));
    cudaMalloc(&d_up,   n * sizeof(half));
    cudaMalloc(&d_out,  n * sizeof(half));
    cudaMemcpy(d_gate, h_gate, n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_up,   h_up,   n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid((n + BLOCK_DIM - 1) / BLOCK_DIM, 1, 1);
    swiglu_f16<<<grid, block>>>(d_gate, d_up, d_out, n);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_gate); cudaFree(d_up); cudaFree(d_out);
        free(h_gate); free(h_up); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    int worst_ulp = 0;
    int reported = 0;
    for (int i = 0; i < n; i++) {
        int u = abs_ulp(h_gpu[i], h_cpu[i]);
        if (u > worst_ulp) worst_ulp = u;
        if (u > max_ulps) {
            if (reported < 4) {
                fprintf(stderr,
                        "[%s] ULP MISMATCH out[%d] gpu=0x%04x cpu=0x%04x "
                        "(gpu=%f cpu=%f, gate=%f up=%f, %d ulps)\n",
                        label, i, half_bits(h_gpu[i]), half_bits(h_cpu[i]),
                        (double)__half2float(h_gpu[i]),
                        (double)__half2float(h_cpu[i]),
                        (double)__half2float(h_gate[i]),
                        (double)__half2float(h_up[i]), u);
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[%s] OK — %d halves within %d ULPs of CPU reference "
               "(worst=%d)\n", label, n, max_ulps, worst_ulp);
    } else {
        fprintf(stderr,
                "[%s] FAILED — worst diff %d ULPs (allowed %d)\n",
                label, worst_ulp, max_ulps);
    }

    cudaFree(d_gate); cudaFree(d_up); cudaFree(d_out);
    free(h_gate); free(h_up); free(h_gpu); free(h_cpu);
    return ok;
}

/* Boundary-condition harness: gate = 0 → silu(0) = 0 → out = 0
 * regardless of up. Pinpoints any bug in the sigmoid path. */
static int run_zero_gate(int n)
{
    half *h_gate = (half *)calloc(n, sizeof(half));
    half *h_up   = (half *)malloc(n * sizeof(half));
    half *h_gpu  = (half *)malloc(n * sizeof(half));
    if (!h_gate || !h_up || !h_gpu) { fprintf(stderr, "oom\n"); return 0; }

    /* up = 1.0 everywhere — out should still be 0 because silu(0)=0. */
    for (int i = 0; i < n; i++) h_up[i] = __float2half(1.0f);

    half *d_gate, *d_up, *d_out;
    cudaMalloc(&d_gate, n * sizeof(half));
    cudaMalloc(&d_up,   n * sizeof(half));
    cudaMalloc(&d_out,  n * sizeof(half));
    cudaMemcpy(d_gate, h_gate, n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_up,   h_up,   n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid((n + BLOCK_DIM - 1) / BLOCK_DIM, 1, 1);
    swiglu_f16<<<grid, block>>>(d_gate, d_up, d_out, n);
    cudaDeviceSynchronize();
    cudaMemcpy(h_gpu, d_out, n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (half_bits(h_gpu[i]) != 0) {
            fprintf(stderr,
                    "[zero-gate n=%d] out[%d]=0x%04x, expected 0x0000\n",
                    n, i, half_bits(h_gpu[i]));
            ok = 0;
            break;
        }
    }
    if (ok) printf("[zero-gate n=%d] OK — all outputs are 0x0000\n", n);

    cudaFree(d_gate); cudaFree(d_up); cudaFree(d_out);
    free(h_gate); free(h_up); free(h_gpu);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 2-ULP tolerance: __expf (CUDA fast intrinsic) vs libm::expf both
     * round-to-nearest in FP32 but their final-bit results can differ
     * for inputs in the SiLU transition zone. Tighter than this would
     * surface real bugs but flag harmless intrinsic-precision diffs. */
    const int MAX_ULPS = 2;

    /* Production shapes: n = total elements (rows × intermediate_size).
     *   8960   — Qwen2.5-1.5B per-token decode (1 × 8960)
     *   11008  — Llama-2 7B per-token decode (1 × 11008)
     *   143360 — Qwen2.5-1.5B prefill chunk (16 × 8960)
     *   1023   — non-multiple-of-256 tail (n % BLOCK_DIM != 0,
     *            (1023 + 255)/256 = 4 blocks, last block has 255
     *            active threads + 1 idle that hits the bounds check). */
    all_ok &= run_one(8960,   0xC0FFEE01u, "n=8960   ", MAX_ULPS);
    all_ok &= run_one(11008,  0xC0FFEE02u, "n=11008  ", MAX_ULPS);
    all_ok &= run_one(143360, 0xC0FFEE03u, "n=143360 ", MAX_ULPS);
    all_ok &= run_one(1023,   0xC0FFEE04u, "n=1023   ", MAX_ULPS);

    all_ok &= run_zero_gate(8960);

    return all_ok ? 0 : 1;
}
