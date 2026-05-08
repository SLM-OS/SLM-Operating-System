/*
 * rope_f16.cu — Rotary Position Embedding in FP16 on Jetson GA10B
 * (fourth SLM-on-GPU operator from the #540 menu).
 *
 * RoPE rotates pairs `(vec[2i], vec[2i+1])` of every Q/K vector by an
 * angle that depends on (token position, pair index). The cos/sin
 * factors are precomputed once into a flat FP32 lookup table; the
 * kernel just gathers from it and applies a 2×2 rotation:
 *
 *     v0' = v0 * cos − v1 * sin
 *     v1' = v0 * sin + v1 * cos
 *
 * Convention is GPT-NeoX / Llama / Qwen2 pair-interleave: pair `i` is
 * `(vec[2i], vec[2i+1])`. Mirrors
 * `runtime/src/inference/ops_transformer.rs::RopeTable::apply` and is
 * called once per attention block on Q (n_heads heads) and once on K
 * (n_kv_heads heads).
 *
 * Mapping (one block per (token, head), one thread per pair):
 *   blockIdx.x  = token_idx * num_heads + head_idx
 *   threadIdx.x = pair_idx ∈ [0, head_dim/2)
 *
 * Each thread loads its (cos, sin) from
 * `cos_sin[positions[token_idx] * head_dim + 2*pair_idx]` and rotates
 * its two FP16 elements in-place. Kernel is element-wise across pairs:
 * no shared memory, no reductions, no cross-thread communication.
 *
 * cbuf[0] layout (CUDA ABI for sm_87):
 *   [0x160] vec        (half *)         in/out, [batch × num_heads × head_dim]
 *   [0x168] positions  (const int *)    [batch] — one position per token
 *   [0x170] cos_sin    (const float *)  [max_pos × head_dim], cos at even, sin at odd
 *   [0x178] batch      (int)
 *   [0x17C] num_heads  (int)
 *   [0x180] head_dim   (int)
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o rope_f16 rope_f16.cu
 *   cuobjdump --extract-elf all rope_f16
 *   readelf -SW rope_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>

__global__ void rope_f16(half *vec, const int *positions,
                          const float *cos_sin,
                          int batch, int num_heads, int head_dim)
{
    int vec_idx  = blockIdx.x;          /* 0..batch*num_heads-1   */
    int pair_idx = threadIdx.x;         /* 0..head_dim/2-1        */
    int half_dim = head_dim >> 1;
    if (pair_idx >= half_dim) return;

    int token_idx = vec_idx / num_heads;
    if (token_idx >= batch) return;

    int pos = positions[token_idx];
    int cos_base = pos * head_dim + (pair_idx << 1);
    float c = cos_sin[cos_base];
    float s = cos_sin[cos_base + 1];

    int vec_base = vec_idx * head_dim + (pair_idx << 1);
    float v0 = __half2float(vec[vec_base]);
    float v1 = __half2float(vec[vec_base + 1]);
    vec[vec_base    ] = __float2half(v0 * c - v1 * s);
    vec[vec_base + 1] = __float2half(v0 * s + v1 * c);
}

/* ---- CPU reference (mirrors runtime/src/inference/ops_transformer.rs::RopeTable::apply) ---- */

static void cpu_reference(half *vec, const int *positions,
                          const float *cos_sin,
                          int batch, int num_heads, int head_dim)
{
    int half_dim = head_dim / 2;
    for (int t = 0; t < batch; t++) {
        int pos = positions[t];
        for (int h = 0; h < num_heads; h++) {
            int vec_off = (t * num_heads + h) * head_dim;
            int cos_off = pos * head_dim;
            for (int i = 0; i < half_dim; i++) {
                float c = cos_sin[cos_off + 2 * i];
                float s = cos_sin[cos_off + 2 * i + 1];
                float v0 = __half2float(vec[vec_off + 2 * i]);
                float v1 = __half2float(vec[vec_off + 2 * i + 1]);
                vec[vec_off + 2 * i]     = __float2half(v0 * c - v1 * s);
                vec[vec_off + 2 * i + 1] = __float2half(v0 * s + v1 * c);
            }
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

/* Build a flat cos/sin LUT for `max_pos` positions × `head_dim` slots
 * using the standard θ_i = base ^ (-2i/head_dim) schedule. base = 10000
 * matches every Llama-family model and Qwen2.5. */
static void build_cos_sin_table(float *cos_sin, int max_pos, int head_dim,
                                float base)
{
    int half_dim = head_dim / 2;
    for (int pos = 0; pos < max_pos; pos++) {
        for (int i = 0; i < half_dim; i++) {
            float exponent = -2.0f * (float)i / (float)head_dim;
            float theta_i = powf(base, exponent);
            float angle = (float)pos * theta_i;
            cos_sin[pos * head_dim + 2 * i]     = cosf(angle);
            cos_sin[pos * head_dim + 2 * i + 1] = sinf(angle);
        }
    }
}

static int run_one(int batch, int num_heads, int head_dim, int max_pos,
                   uint32_t seed_in, const char *label, int max_ulps)
{
    if (head_dim & 1) {
        fprintf(stderr, "[%s] head_dim must be even\n", label);
        return 0;
    }

    size_t vec_n   = (size_t)batch * num_heads * head_dim;
    size_t lut_n   = (size_t)max_pos * head_dim;
    half  *h_vec0  = (half  *)malloc(vec_n * sizeof(half));   /* original */
    half  *h_gpu   = (half  *)malloc(vec_n * sizeof(half));
    half  *h_cpu   = (half  *)malloc(vec_n * sizeof(half));
    int   *h_pos   = (int   *)malloc((size_t)batch * sizeof(int));
    float *h_lut   = (float *)malloc(lut_n * sizeof(float));
    if (!h_vec0 || !h_gpu || !h_cpu || !h_pos || !h_lut) {
        fprintf(stderr, "oom\n");
        return 0;
    }

    /* Random vec ~ uniform(-1, 1), positions ~ random in [0, max_pos). */
    uint32_t seed = seed_in;
    for (size_t i = 0; i < vec_n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
        h_vec0[i] = __float2half(v);
    }
    for (int t = 0; t < batch; t++) {
        seed = seed * 1664525u + 1013904223u;
        h_pos[t] = (int)(seed % (uint32_t)max_pos);
    }

    build_cos_sin_table(h_lut, max_pos, head_dim, 10000.0f);

    /* CPU reference: rotate a copy in place. */
    memcpy(h_cpu, h_vec0, vec_n * sizeof(half));
    cpu_reference(h_cpu, h_pos, h_lut, batch, num_heads, head_dim);

    /* GPU: same input, same LUT, same positions. */
    half  *d_vec; int *d_pos; float *d_lut;
    cudaMalloc(&d_vec, vec_n * sizeof(half));
    cudaMalloc(&d_pos, (size_t)batch * sizeof(int));
    cudaMalloc(&d_lut, lut_n * sizeof(float));
    cudaMemcpy(d_vec, h_vec0, vec_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_pos, h_pos, (size_t)batch * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_lut, h_lut, lut_n * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(head_dim / 2, 1, 1);
    dim3 grid(batch * num_heads, 1, 1);
    rope_f16<<<grid, block>>>(d_vec, d_pos, d_lut, batch, num_heads, head_dim);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_vec); cudaFree(d_pos); cudaFree(d_lut);
        free(h_vec0); free(h_gpu); free(h_cpu); free(h_pos); free(h_lut);
        return 0;
    }

    cudaMemcpy(h_gpu, d_vec, vec_n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    int worst_ulp = 0;
    int reported = 0;
    for (size_t i = 0; i < vec_n; i++) {
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
               "(worst=%d)\n", label, vec_n, max_ulps, worst_ulp);
    } else {
        fprintf(stderr,
                "[%s] FAILED — worst diff %d ULPs (allowed %d)\n",
                label, worst_ulp, max_ulps);
    }

    cudaFree(d_vec); cudaFree(d_pos); cudaFree(d_lut);
    free(h_vec0); free(h_gpu); free(h_cpu); free(h_pos); free(h_lut);
    return ok;
}

/* Boundary case: position 0 → cos = 1, sin = 0 → vec unchanged.
 * Mirrors the rope_table_position_zero_is_identity unit test in
 * runtime/src/inference/ops_transformer.rs. */
static int run_position_zero(int batch, int num_heads, int head_dim)
{
    int max_pos = 16;
    size_t vec_n = (size_t)batch * num_heads * head_dim;
    size_t lut_n = (size_t)max_pos * head_dim;

    half  *h_vec0 = (half  *)malloc(vec_n * sizeof(half));
    half  *h_gpu  = (half  *)malloc(vec_n * sizeof(half));
    int   *h_pos  = (int   *)calloc((size_t)batch, sizeof(int));   /* all 0 */
    float *h_lut  = (float *)malloc(lut_n * sizeof(float));
    if (!h_vec0 || !h_gpu || !h_pos || !h_lut) {
        fprintf(stderr, "oom\n");
        return 0;
    }

    uint32_t seed = 0xBADBEEF1u;
    for (size_t i = 0; i < vec_n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
        h_vec0[i] = __float2half(v);
    }
    build_cos_sin_table(h_lut, max_pos, head_dim, 10000.0f);

    half *d_vec; int *d_pos; float *d_lut;
    cudaMalloc(&d_vec, vec_n * sizeof(half));
    cudaMalloc(&d_pos, (size_t)batch * sizeof(int));
    cudaMalloc(&d_lut, lut_n * sizeof(float));
    cudaMemcpy(d_vec, h_vec0, vec_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_pos, h_pos, (size_t)batch * sizeof(int), cudaMemcpyHostToDevice);
    cudaMemcpy(d_lut, h_lut, lut_n * sizeof(float), cudaMemcpyHostToDevice);

    dim3 block(head_dim / 2, 1, 1);
    dim3 grid(batch * num_heads, 1, 1);
    rope_f16<<<grid, block>>>(d_vec, d_pos, d_lut, batch, num_heads, head_dim);
    cudaDeviceSynchronize();
    cudaMemcpy(h_gpu, d_vec, vec_n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    /* pos=0: cos=1, sin=0 in the LUT. The kernel still does
     * v0*1 - v1*0 = v0 and v0*0 + v1*1 = v1, but the FP32 round-trip
     * (half → float → half) is allowed to differ from the original
     * by 0 ULPs. Anything else means the kernel did real arithmetic
     * with the wrong factors. */
    int worst = 0;
    for (size_t i = 0; i < vec_n; i++) {
        int u = abs_ulp(h_gpu[i], h_vec0[i]);
        if (u > worst) worst = u;
        if (u > 0) {
            fprintf(stderr,
                    "[pos0 batch=%d heads=%d hd=%d] vec[%zu] gpu=0x%04x "
                    "orig=0x%04x (%d ulps)\n",
                    batch, num_heads, head_dim, i,
                    half_bits(h_gpu[i]), half_bits(h_vec0[i]), u);
            ok = 0;
            break;
        }
    }
    if (ok) {
        printf("[pos0 batch=%d heads=%d hd=%d] OK — vec unchanged "
               "(worst=%d ULPs)\n", batch, num_heads, head_dim, worst);
    }

    cudaFree(d_vec); cudaFree(d_pos); cudaFree(d_lut);
    free(h_vec0); free(h_gpu); free(h_pos); free(h_lut);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 2-ULP tolerance: each thread does its own pair independently in
     * FP32, so there's no parallel-reduction order divergence. The
     * only ULP source is __float2half rounding vs Rust's manual
     * f32_to_f16 in gguf.rs. Most outputs will be bit-exact; the
     * tolerance just covers boundary-rounding cases. */
    const int MAX_ULPS = 2;

    /* Production shapes from the SLM forward path:
     *   Qwen2.5-1.5B: head_dim=128, n_heads=12 (Q), n_kv_heads=2 (K).
     *     decode  → batch=1, heads=12, hd=128 → 1536 elements
     *     prefill → batch=16, heads=12, hd=128 → 24576 elements
     *   Llama-2 7B:   head_dim=128, n_heads=32 (Q), n_kv_heads=32 (K).
     *     decode  → batch=1, heads=32, hd=128 → 4096 elements
     *   Smaller-head model: head_dim=64 — exercises 32-thread blocks.
     *     decode  → batch=1, heads=8, hd=64  → 512 elements */
    all_ok &= run_one(1,  12, 128, 4096, 0xC0FFEE01u, "qwen-Q  ", MAX_ULPS);
    all_ok &= run_one(1,   2, 128, 4096, 0xC0FFEE02u, "qwen-K  ", MAX_ULPS);
    all_ok &= run_one(16, 12, 128, 4096, 0xC0FFEE03u, "qwen-pre", MAX_ULPS);
    all_ok &= run_one(1,  32, 128, 4096, 0xC0FFEE04u, "llama-Q ", MAX_ULPS);
    all_ok &= run_one(1,   8,  64, 4096, 0xC0FFEE05u, "hd64    ", MAX_ULPS);

    all_ok &= run_position_zero(1, 12, 128);

    return all_ok ? 0 : 1;
}
