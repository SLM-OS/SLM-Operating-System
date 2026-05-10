/*
 * gqa_attn_f16.cu — Fused Grouped-Query Attention decode step on
 * Jetson GA10B (sixth SLM-on-GPU operator from the #540 menu —
 * the last big piece for an end-to-end GPU SLM forward pass).
 *
 * One-step GQA for the decode loop. At decode time the model has
 * just produced one new token; for that position we have:
 *   - one query row per attention head (n_head_q × head_dim FP16),
 *   - the cumulative key/value cache (seq_len × n_head_kv × head_dim
 *     FP16, for each of K and V),
 * and we want the attention output (n_head_q × head_dim FP16) that
 * the subsequent output projection consumes.
 *
 * Fuses three phases in a single kernel per Q head:
 *
 *   Phase 1 — logits[t] = (Q[h] · K[t, h_kv]) / sqrt(head_dim)
 *             for t ∈ [0, seq_len). Stash logits in shared memory.
 *             Find max(logits) via block reduction.
 *
 *   Phase 2 — softmax (numerically stable):
 *               logits[t] = exp(logits[t] − max)
 *               denom = sum_t logits[t]            (block reduction)
 *               logits[t] /= denom
 *
 *   Phase 3 — out[h, d] = sum_t logits[t] * V[t, h_kv, d]
 *             for d ∈ [0, head_dim).
 *
 * GQA pairing: every `group = n_head_q / n_head_kv` query heads share
 * one KV head. Qwen2.5-1.5B has group=6 (12 Q heads, 2 KV heads);
 * Llama-2 7B has group=1 (32:32, equivalent to MHA); Mistral / smaller
 * models have group=4 or 8.
 *
 * Causal mask: implicit. `seq_len` is the count of past+current
 * positions to attend to; future positions simply aren't passed in.
 *
 * Mirrors `runtime/src/inference/ops_transformer.rs::gqa_decode_step`.
 *
 * Mapping (one block per Q head, BLOCK_DIM threads per block):
 *   blockIdx.x  = hq (query head index)
 *   threadIdx.x = stride id for parallel work within the head
 *
 * Limits:
 *   - head_dim ≤ MAX_HEAD_DIM (256 — comfortable headroom over the
 *     128 used by every shipping Qwen / Llama / Phi).
 *   - seq_len  ≤ MAX_SEQ_LEN  (4096 — same as the Qwen2.5-1.5B
 *     context length). Tighter caps would shrink the per-block
 *     shared-memory footprint but rule out long-context decode.
 *
 * cbuf[0] layout (CUDA ABI for sm_87):
 *   [0x160] q          (const half *)   [n_head_q  × head_dim]
 *   [0x168] k          (const half *)   [seq_len × n_head_kv × head_dim]
 *   [0x170] v          (const half *)   [seq_len × n_head_kv × head_dim]
 *   [0x178] out        (half *)         [n_head_q × head_dim]
 *   [0x180] n_head_q   (int)
 *   [0x184] n_head_kv  (int)
 *   [0x188] head_dim   (int)
 *   [0x18C] seq_len    (int)
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o gqa_attn_f16 gqa_attn_f16.cu
 *   cuobjdump --extract-elf all gqa_attn_f16
 *   readelf -SW gqa_attn_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>

#define BLOCK_DIM     128
#define MAX_HEAD_DIM  256
#define MAX_SEQ_LEN   4096

__global__ void gqa_attn_f16(const half *q, const half *k, const half *v,
                              half *out,
                              int n_head_q, int n_head_kv,
                              int head_dim, int seq_len)
{
    int hq  = blockIdx.x;
    int tid = threadIdx.x;
    if (hq >= n_head_q) return;

    int group       = n_head_q / n_head_kv;
    int hkv         = hq / group;
    int kv_per_pos  = n_head_kv * head_dim;
    int q_off       = hq * head_dim;

    /* Shared scratch:
     *   q_cache[head_dim]    — Q row in FP32 (read once per t in phase 1
     *                          and once per d in phase 3; cache it once).
     *   logits[seq_len]      — per-position scores between phases 1↔2↔3.
     *   reduce_buf[BLOCK_DIM]— tree-reduction scratch for max + sum.
     *   global_max / inv_denom — broadcast scalars from thread 0.
     */
    __shared__ float q_cache[MAX_HEAD_DIM];
    __shared__ float logits[MAX_SEQ_LEN];
    __shared__ float reduce_buf[BLOCK_DIM];
    __shared__ float global_max;
    __shared__ float global_inv_denom;

    /* Cache Q row in FP32 (read many times below). */
    for (int d = tid; d < head_dim; d += BLOCK_DIM) {
        q_cache[d] = __half2float(q[q_off + d]);
    }
    __syncthreads();

    float inv_sqrt_head = rsqrtf((float)head_dim);

    /* Phase 1: compute every logit, track per-thread max. Each thread
     * strides over t and computes a full head_dim-long FP32 dot product
     * against q_cache. CPU mirrors this exactly (sequential dot product
     * per logit), so this phase is bit-identical to the CPU output
     * modulo __half2float rounding (already paid). */
    float local_max = -FLT_MAX;
    for (int t = tid; t < seq_len; t += BLOCK_DIM) {
        int k_off = t * kv_per_pos + hkv * head_dim;
        float acc = 0.0f;
        for (int d = 0; d < head_dim; d++) {
            acc += q_cache[d] * __half2float(k[k_off + d]);
        }
        float logit = acc * inv_sqrt_head;
        logits[t] = logit;
        if (logit > local_max) local_max = logit;
    }
    reduce_buf[tid] = local_max;
    __syncthreads();
    for (int s = BLOCK_DIM / 2; s > 0; s >>= 1) {
        if (tid < s) {
            float a = reduce_buf[tid];
            float b = reduce_buf[tid + s];
            reduce_buf[tid] = a > b ? a : b;
        }
        __syncthreads();
    }
    if (tid == 0) global_max = reduce_buf[0];
    __syncthreads();

    /* Phase 2: numerically-stable softmax. Subtract max, exp, sum,
     * normalize. Two passes over logits because the sum-reduce sits
     * between them; the second pass writes the normalized weights
     * back in place. */
    float local_sum = 0.0f;
    for (int t = tid; t < seq_len; t += BLOCK_DIM) {
        float e = __expf(logits[t] - global_max);
        logits[t] = e;
        local_sum += e;
    }
    reduce_buf[tid] = local_sum;
    __syncthreads();
    for (int s = BLOCK_DIM / 2; s > 0; s >>= 1) {
        if (tid < s) reduce_buf[tid] += reduce_buf[tid + s];
        __syncthreads();
    }
    if (tid == 0) {
        global_inv_denom = (reduce_buf[0] > 0.0f) ? 1.0f / reduce_buf[0] : 0.0f;
    }
    __syncthreads();

    for (int t = tid; t < seq_len; t += BLOCK_DIM) {
        logits[t] *= global_inv_denom;
    }
    __syncthreads();

    /* Phase 3: out[hq, d] = sum_t logits[t] * V[t, hkv, d]. Each thread
     * owns one (or more, if head_dim > BLOCK_DIM) output dim and walks
     * t sequentially — same accumulation order as the CPU. */
    for (int d = tid; d < head_dim; d += BLOCK_DIM) {
        float acc = 0.0f;
        for (int t = 0; t < seq_len; t++) {
            int v_off = t * kv_per_pos + hkv * head_dim + d;
            acc += logits[t] * __half2float(v[v_off]);
        }
        out[q_off + d] = __float2half(acc);
    }
}

/* ---- CPU reference (mirrors gqa_decode_step) ---- */

static void cpu_reference(const half *q, const half *k, const half *v,
                          half *out,
                          int n_head_q, int n_head_kv,
                          int head_dim, int seq_len)
{
    int group      = n_head_q / n_head_kv;
    int kv_per_pos = n_head_kv * head_dim;
    float inv_sqrt_head = 1.0f / sqrtf((float)head_dim);

    /* Per-head logits scratch. */
    float *logits = (float *)malloc((size_t)seq_len * sizeof(float));

    for (int hq = 0; hq < n_head_q; hq++) {
        int hkv = hq / group;
        int q_off = hq * head_dim;

        float max_logit = -FLT_MAX;
        for (int t = 0; t < seq_len; t++) {
            int k_off = t * kv_per_pos + hkv * head_dim;
            float acc = 0.0f;
            for (int d = 0; d < head_dim; d++) {
                acc += __half2float(q[q_off + d]) * __half2float(k[k_off + d]);
            }
            float logit = acc * inv_sqrt_head;
            logits[t] = logit;
            if (logit > max_logit) max_logit = logit;
        }

        float denom = 0.0f;
        for (int t = 0; t < seq_len; t++) {
            float e = expf(logits[t] - max_logit);
            logits[t] = e;
            denom += e;
        }
        float inv_denom = denom > 0.0f ? 1.0f / denom : 0.0f;
        for (int t = 0; t < seq_len; t++) logits[t] *= inv_denom;

        for (int d = 0; d < head_dim; d++) {
            float acc = 0.0f;
            for (int t = 0; t < seq_len; t++) {
                int v_off = t * kv_per_pos + hkv * head_dim + d;
                acc += logits[t] * __half2float(v[v_off]);
            }
            out[q_off + d] = __float2half(acc);
        }
    }

    free(logits);
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

static int run_one(int n_head_q, int n_head_kv, int head_dim, int seq_len,
                   uint32_t seed_in, const char *label, int max_ulps)
{
    if (head_dim > MAX_HEAD_DIM || seq_len > MAX_SEQ_LEN) {
        fprintf(stderr, "[%s] head_dim=%d / seq_len=%d exceeds kernel caps\n",
                label, head_dim, seq_len);
        return 0;
    }
    if (n_head_q % n_head_kv != 0) {
        fprintf(stderr, "[%s] n_head_q must be a multiple of n_head_kv\n",
                label);
        return 0;
    }

    size_t q_n   = (size_t)n_head_q  * head_dim;
    size_t kv_n  = (size_t)seq_len   * n_head_kv * head_dim;

    half *h_q   = (half *)malloc(q_n  * sizeof(half));
    half *h_k   = (half *)malloc(kv_n * sizeof(half));
    half *h_v   = (half *)malloc(kv_n * sizeof(half));
    half *h_gpu = (half *)malloc(q_n  * sizeof(half));
    half *h_cpu = (half *)malloc(q_n  * sizeof(half));
    if (!h_q || !h_k || !h_v || !h_gpu || !h_cpu) { fprintf(stderr, "oom\n"); return 0; }

    /* Q ~ uniform(-0.5, 0.5). K ~ uniform(-0.5, 0.5). V ~ uniform(-1, 1).
     * Magnitudes that produce well-distributed softmax (no single
     * position dominates) — exercises the exp + sum pipeline most. */
    uint32_t seed = seed_in;
    for (size_t i = 0; i < q_n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) - 0.5f;
        h_q[i] = __float2half(v);
    }
    for (size_t i = 0; i < kv_n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) - 0.5f;
        h_k[i] = __float2half(v);
    }
    for (size_t i = 0; i < kv_n; i++) {
        seed = seed * 1664525u + 1013904223u;
        float v = ((float)(seed & 0xFFFF) / 65535.0f) * 2.0f - 1.0f;
        h_v[i] = __float2half(v);
    }

    cpu_reference(h_q, h_k, h_v, h_cpu,
                  n_head_q, n_head_kv, head_dim, seq_len);

    half *d_q, *d_k, *d_v, *d_out;
    cudaMalloc(&d_q,   q_n  * sizeof(half));
    cudaMalloc(&d_k,   kv_n * sizeof(half));
    cudaMalloc(&d_v,   kv_n * sizeof(half));
    cudaMalloc(&d_out, q_n  * sizeof(half));
    cudaMemcpy(d_q, h_q, q_n  * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_k, h_k, kv_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_v, h_v, kv_n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(n_head_q, 1, 1);
    gqa_attn_f16<<<grid, block>>>(d_q, d_k, d_v, d_out,
                                   n_head_q, n_head_kv, head_dim, seq_len);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_q); cudaFree(d_k); cudaFree(d_v); cudaFree(d_out);
        free(h_q); free(h_k); free(h_v); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, q_n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    int worst_ulp = 0;
    int reported = 0;
    for (size_t i = 0; i < q_n; i++) {
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
               "(worst=%d)\n", label, q_n, max_ulps, worst_ulp);
    } else {
        fprintf(stderr,
                "[%s] FAILED — worst diff %d ULPs (allowed %d)\n",
                label, worst_ulp, max_ulps);
    }

    cudaFree(d_q); cudaFree(d_k); cudaFree(d_v); cudaFree(d_out);
    free(h_q); free(h_k); free(h_v); free(h_gpu); free(h_cpu);
    return ok;
}

/* Boundary case: seq_len = 1 → softmax of one logit = 1.0 → out = V[0, hkv]
 * regardless of Q and K. Mirrors gqa_decode_single_position_returns_v in
 * runtime/src/inference/ops_transformer.rs. */
static int run_single_position(int n_head_q, int n_head_kv, int head_dim)
{
    int seq_len = 1;
    int group   = n_head_q / n_head_kv;
    size_t q_n  = (size_t)n_head_q  * head_dim;
    size_t kv_n = (size_t)seq_len   * n_head_kv * head_dim;

    half *h_q   = (half *)malloc(q_n  * sizeof(half));
    half *h_k   = (half *)malloc(kv_n * sizeof(half));
    half *h_v   = (half *)malloc(kv_n * sizeof(half));
    half *h_gpu = (half *)malloc(q_n  * sizeof(half));
    if (!h_q || !h_k || !h_v || !h_gpu) { fprintf(stderr, "oom\n"); return 0; }

    /* Arbitrary Q/K — softmax of a single logit collapses to 1.0 so
     * the QK product never affects the output. V is 0..n−1 so we can
     * spot-check the gather. */
    for (size_t i = 0; i < q_n; i++)  h_q[i] = __float2half(0.5f);
    for (size_t i = 0; i < kv_n; i++) h_k[i] = __float2half(0.25f);
    for (int hkv = 0; hkv < n_head_kv; hkv++) {
        for (int d = 0; d < head_dim; d++) {
            float val = (float)(hkv * 1000 + d);
            h_v[hkv * head_dim + d] = __float2half(val);
        }
    }

    half *d_q, *d_k, *d_v, *d_out;
    cudaMalloc(&d_q,   q_n  * sizeof(half));
    cudaMalloc(&d_k,   kv_n * sizeof(half));
    cudaMalloc(&d_v,   kv_n * sizeof(half));
    cudaMalloc(&d_out, q_n  * sizeof(half));
    cudaMemcpy(d_q, h_q, q_n  * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_k, h_k, kv_n * sizeof(half), cudaMemcpyHostToDevice);
    cudaMemcpy(d_v, h_v, kv_n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(BLOCK_DIM, 1, 1);
    dim3 grid(n_head_q, 1, 1);
    gqa_attn_f16<<<grid, block>>>(d_q, d_k, d_v, d_out,
                                   n_head_q, n_head_kv, head_dim, seq_len);
    cudaDeviceSynchronize();
    cudaMemcpy(h_gpu, d_out, q_n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    for (int hq = 0; hq < n_head_q; hq++) {
        int hkv = hq / group;
        for (int d = 0; d < head_dim; d++) {
            float expected = (float)(hkv * 1000 + d);
            half  exp_h    = __float2half(expected);
            half  got_h    = h_gpu[hq * head_dim + d];
            if (half_bits(got_h) != half_bits(exp_h)) {
                fprintf(stderr,
                        "[seq_len=1 hq=%d d=%d] got %f, expected V[%d,%d]=%f\n",
                        hq, d,
                        (double)__half2float(got_h),
                        hkv, d, (double)expected);
                ok = 0;
                hq = n_head_q;  /* break outer */
                break;
            }
        }
    }
    if (ok) {
        printf("[seq_len=1 nhq=%d nhkv=%d hd=%d] OK — out = V[0, hkv] "
               "bit-exactly\n", n_head_q, n_head_kv, head_dim);
    }

    cudaFree(d_q); cudaFree(d_k); cudaFree(d_v); cudaFree(d_out);
    free(h_q); free(h_k); free(h_v); free(h_gpu);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 8 ULPs is the right scale for FP16 attention output. Sources
     * of divergence:
     *   - __expf (CUDA fast intrinsic, ~2 ULP) vs libm::expf in CPU.
     *   - Softmax denom: tree reduction across BLOCK_DIM partial
     *     sums (different order than CPU's sequential sum).
     *   - Logit max: tree reduction (associative — no real divergence).
     *   - Phase 1 dot product per-logit: bit-identical (sequential
     *     in both CPU and GPU per (hq, t)).
     *   - Phase 3 attn-V sum: bit-identical (sequential per d).
     *
     * The softmax denom is the dominant source. For seq_len ≤ 512 it
     * lands at 1-3 ULPs typical; it grows toward 8 at seq_len=2048
     * because more terms → more reorder-sensitive sum. */
    const int MAX_ULPS = 8;

    /* Production shapes from the SLM forward path:
     *   Qwen2.5-1.5B:  nhq=12, nhkv=2, hd=128 (group=6 GQA)
     *   Llama-2 7B:    nhq=32, nhkv=32, hd=128 (group=1 = MHA equiv)
     *   Mistral-style: nhq=32, nhkv=8, hd=128 (group=4 GQA)
     *   MQA fixture:   nhq=8,  nhkv=1, hd=64  (group=8, smaller head)
     * Mix seq_len from short (1, 2) to long-context (2048). */
    all_ok &= run_one(12, 2,  128, 1,    0xC0FFEE01u, "qwen-len1   ", MAX_ULPS);
    all_ok &= run_one(12, 2,  128, 64,   0xC0FFEE02u, "qwen-len64  ", MAX_ULPS);
    all_ok &= run_one(12, 2,  128, 1024, 0xC0FFEE03u, "qwen-len1024", MAX_ULPS);
    all_ok &= run_one(12, 2,  128, 2048, 0xC0FFEE04u, "qwen-len2048", MAX_ULPS);
    all_ok &= run_one(32, 32, 128, 256,  0xC0FFEE05u, "llama-MHA   ", MAX_ULPS);
    all_ok &= run_one(32, 8,  128, 256,  0xC0FFEE06u, "mistral-GQA ", MAX_ULPS);
    all_ok &= run_one(8,  1,  64,  128,  0xC0FFEE07u, "MQA-hd64    ", MAX_ULPS);

    all_ok &= run_single_position(12, 2, 128);

    return all_ok ? 0 : 1;
}
