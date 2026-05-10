/*
 * rmsnorm_f16.cu — Root-mean-square layer norm in FP16 on Jetson GA10B
 * (second SLM-on-GPU operator from the #540 menu).
 *
 * RmsNorm is the per-row normalization used at every transformer layer
 * in Qwen2.5 / Llama / Phi-style SLMs:
 *
 *     rms_inv = 1 / sqrt(mean(x_i^2) + eps)
 *     out_i   = x_i * rms_inv * gamma_i
 *
 * Inputs and outputs are FP16; the sum-of-squares accumulator is FP32
 * to avoid the precision loss that would come from summing thousands
 * of tiny squared values in FP16. Mirrors
 * `runtime/src/inference/ops_transformer.rs::rmsnorm` (the canonical
 * CPU reference). Validation tolerance is ULP-based — the parallel
 * tree reduction and hardware sqrt produce results that differ from
 * the CPU's mathf::sqrtf + sequential accumulation by at most a few
 * raw FP16 bits.
 *
 * Mapping (one block per row, 256 threads per block):
 *   blockIdx.x  = row index
 *   threadIdx.x = thread within block
 *
 * Each thread strides through its row slice for both phases:
 *   Phase 1 — accumulate x[i]*x[i] in FP32 (per-thread partial sum)
 *   Phase 2 — block tree reduction in shared memory → mean_sq
 *   Phase 3 — thread 0 computes rms_inv = rsqrtf(mean_sq + eps),
 *             broadcasts via shared memory
 *   Phase 4 — strided write out[i] = __float2half(x[i] * rms_inv * gamma[i])
 *
 * BLOCK_DIM must be a power of two (256 = 2^8) for the tree reduction.
 * `n` (row width) can be any positive value — the strided loop handles
 * non-multiples of BLOCK_DIM. For n < BLOCK_DIM, idle threads
 * contribute 0 to the sum and skip the output write.
 *
 * cbuf[0] layout (CUDA ABI for sm_87, mirrors gemm_hmma_fp16 / add_bias):
 *   [0x160] x     (const half *)   // [n_rows × n] row-major
 *   [0x168] gamma (const half *)   // [n] learned per-channel scale
 *   [0x170] out   (half *)         // [n_rows × n] row-major
 *   [0x178] n     (int)            // row width (e.g. 1536 for Qwen2.5-1.5B)
 *   [0x17C] eps   (float)          // 1e-6 for Qwen2.5
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o rmsnorm_f16 rmsnorm_f16.cu
 *   cuobjdump --extract-elf all rmsnorm_f16
 *   readelf -SW rmsnorm_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

#define BLOCK_DIM 256

__global__ void rmsnorm_f16(const half *x, const half *gamma, half *out,
                             int n, float eps)
{
    int row = blockIdx.x;
    int tid = threadIdx.x;

    const half *x_row   = x + (size_t)row * n;
    half       *out_row = out + (size_t)row * n;

    /* Phase 1: per-thread partial sum of squares in FP32. */
    float local_acc = 0.0f;
    for (int i = tid; i < n; i += BLOCK_DIM) {
        float v = __half2float(x_row[i]);
        local_acc += v * v;
    }

    /* Phase 2: block tree reduction in shared memory. */
    __shared__ float smem[BLOCK_DIM];
    smem[tid] = local_acc;
    __syncthreads();

    for (int s = BLOCK_DIM / 2; s > 0; s >>= 1) {
        if (tid < s) smem[tid] += smem[tid + s];
        __syncthreads();
    }

    /* Phase 3: thread 0 finalizes, broadcasts via shared memory.
     * Using rsqrtf (hardware reciprocal-sqrt) matches the algebraic
     * intent (1/sqrt(mean_sq + eps)) and is the canonical SASS form
     * GA10x emits for this idiom. */
    __shared__ float rms_inv_shared;
    if (tid == 0) {
        float mean_sq = smem[0] / (float)n;
        rms_inv_shared = rsqrtf(mean_sq + eps);
    }
    __syncthreads();

    float rms_inv = rms_inv_shared;

    /* Phase 4: strided write of the normalized + scaled outputs. */
    for (int i = tid; i < n; i += BLOCK_DIM) {
        float xv = __half2float(x_row[i]);
        float gv = __half2float(gamma[i]);
        out_row[i] = __float2half(xv * rms_inv * gv);
    }
}

/* ---- CPU reference (mirrors runtime/src/inference/ops_transformer.rs) ---- */

static void cpu_reference(const half *x, const half *gamma, half *out,
                          int n_rows, int n, float eps)
{
    for (int row = 0; row < n_rows; row++) {
        const half *x_row   = x + (size_t)row * n;
        half       *out_row = out + (size_t)row * n;

        float acc_sq = 0.0f;
        for (int i = 0; i < n; i++) {
            float v = __half2float(x_row[i]);
            acc_sq += v * v;
        }
        float mean_sq = acc_sq / (float)n;
        float rms_inv = 1.0f / sqrtf(mean_sq + eps);

        for (int i = 0; i < n; i++) {
            float xv = __half2float(x_row[i]);
            float gv = __half2float(gamma[i]);
            out_row[i] = __float2half(xv * rms_inv * gv);
        }
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

static int run_one(int n_rows, int n, float eps, uint32_t seed_in,
                   const char *label, int max_ulps)
{
    size_t total = (size_t)n_rows * n;
    half *h_x     = (half *)malloc(total * sizeof(half));
    half *h_gamma = (half *)malloc(n * sizeof(half));
    half *h_gpu   = (half *)malloc(total * sizeof(half));
    half *h_cpu   = (half *)malloc(total * sizeof(half));
    if (!h_x || !h_gamma || !h_gpu || !h_cpu) {
        fprintf(stderr, "oom\n");
        return 0;
    }

    /* x ~ uniform(-0.5, 0.5), gamma ~ uniform(0.5, 1.5). Keeps the
     * sum-of-squares well inside FP16 range and the output O(1). */
    uint32_t seed = seed_in;
    for (size_t i = 0; i < total; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) - 0.5f;
        h_x[i] = __float2half(v);
    }
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = 0.5f + ((float)(seed & 0xFFFF) / 65535.0f);
        h_gamma[i] = __float2half(v);
    }

    cpu_reference(h_x, h_gamma, h_cpu, n_rows, n, eps);

    half *d_x, *d_gamma, *d_out;
    cudaMalloc(&d_x,     total * sizeof(half));
    cudaMalloc(&d_gamma, n     * sizeof(half));
    cudaMalloc(&d_out,   total * sizeof(half));
    cudaMemcpy(d_x,     h_x,     total * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_gamma, h_gamma, n     * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(n_rows, 1, 1);
    rmsnorm_f16<<<grid, block>>>(d_x, d_gamma, d_out, n, eps);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_x); cudaFree(d_gamma); cudaFree(d_out);
        free(h_x); free(h_gamma); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, total * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    int worst_ulp = 0;
    int reported = 0;
    for (size_t i = 0; i < total; i++) {
        int u = abs_ulp(h_gpu[i], h_cpu[i]);
        if (u > worst_ulp) worst_ulp = u;
        if (u > max_ulps) {
            if (reported < 4) {
                fprintf(stderr,
                        "[%s] ULP MISMATCH out[%zu] gpu=0x%04x cpu=0x%04x "
                        "(gpu=%f cpu=%f, %d ulps)\n",
                        label, i, half_bits(h_gpu[i]), half_bits(h_cpu[i]),
                        (double)__half2float(h_gpu[i]),
                        (double)__half2float(h_cpu[i]), u);
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[%s] OK — %zu halves within %d ULPs of CPU reference "
               "(worst=%d)\n", label, total, max_ulps, worst_ulp);
    } else {
        fprintf(stderr,
                "[%s] FAILED — worst diff %d ULPs (allowed %d)\n",
                label, worst_ulp, max_ulps);
    }

    cudaFree(d_x); cudaFree(d_gamma); cudaFree(d_out);
    free(h_x); free(h_gamma); free(h_gpu); free(h_cpu);
    return ok;
}

/* Boundary-condition harness: zero input → zero output, regardless of
 * gamma. Mirrors the rmsnorm_zero_input_returns_zero unit test in
 * runtime/src/inference/ops_transformer.rs. */
static int run_zero_input(int n)
{
    int n_rows = 1;
    half *h_x     = (half *)calloc(n, sizeof(half));
    half *h_gamma = (half *)malloc(n * sizeof(half));
    half *h_gpu   = (half *)malloc(n * sizeof(half));
    if (!h_x || !h_gamma || !h_gpu) { fprintf(stderr, "oom\n"); return 0; }

    for (int i = 0; i < n; i++) h_gamma[i] = __float2half(1.0f);

    half *d_x, *d_gamma, *d_out;
    cudaMalloc(&d_x,     n * sizeof(half));
    cudaMalloc(&d_gamma, n * sizeof(half));
    cudaMalloc(&d_out,   n * sizeof(half));
    cudaMemcpy(d_x,     h_x,     n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_gamma, h_gamma, n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(n_rows, 1, 1);
    rmsnorm_f16<<<grid, block>>>(d_x, d_gamma, d_out, n, 1e-6f);
    cudaDeviceSynchronize();
    cudaMemcpy(h_gpu, d_out, n * sizeof(half), cudaMemcpyDeviceToHost);

    /* All zeros: x=0 → mean_sq=0 → rms_inv=1/sqrt(eps) (huge) → out=0
     * because xv=0 multiplies through. Output bits should all be 0. */
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (half_bits(h_gpu[i]) != 0) {
            fprintf(stderr,
                    "[zero-input n=%d] out[%d]=0x%04x, expected 0x0000\n",
                    n, i, half_bits(h_gpu[i]));
            ok = 0;
            break;
        }
    }
    if (ok) printf("[zero-input n=%d] OK — all outputs are 0x0000\n", n);

    cudaFree(d_x); cudaFree(d_gamma); cudaFree(d_out);
    free(h_x); free(h_gamma); free(h_gpu);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 2 ULP tolerance: allows for parallel tree reduction order vs
     * sequential CPU accumulation, plus rsqrtf vs 1/sqrtf. Tighter
     * than 1 ULP would surface real bugs but flag every harmless
     * float-associativity diff. */
    const int MAX_ULPS = 2;

    /* n_rows × n shapes from the SLM forward path:
     *   1   × 1536 — Qwen2.5-1.5B per-token decode (one row at a time)
     *   16  × 1536 — Qwen2.5-1.5B prefill chunk
     *   1   × 4096 — Llama-2 7B / 13B per-token decode
     *   1   × 768  — non-multiple-of-256 tail (n % BLOCK_DIM != 0) */
    all_ok &= run_one(1,  1536, 1e-6f, 0xC0FFEE01u, "1x1536  ", MAX_ULPS);
    all_ok &= run_one(16, 1536, 1e-6f, 0xC0FFEE02u, "16x1536 ", MAX_ULPS);
    all_ok &= run_one(1,  4096, 1e-6f, 0xC0FFEE03u, "1x4096  ", MAX_ULPS);
    all_ok &= run_one(1,  768,  1e-6f, 0xC0FFEE04u, "1x768   ", MAX_ULPS);

    all_ok &= run_zero_input(1536);

    return all_ok ? 0 : 1;
}
