/*
 * slm_ops.cu - every non-matmul kernel of the Qwen2 forward pass (#475).
 *
 * All activations are FP16, row-major with the batch first. Every kernel takes
 * block (32,1,1). Row widths and head_dim must be multiples of 32 so that a
 * warp always covers whole chunks; RoPE works on half of head_dim, so it alone
 * guards a partial chunk. The cbuf[0] ABI is
 * flat arguments from 0x160; scripts/cuda/slm/MANIFEST.json holds the exact
 * table generated from the cubin.
 *
 * Positions are read from a u32 array (one per batch row) in the arena, so a
 * decode step patches memory instead of QMDs.
 */
#include <cmath>

#include "slm_chunks.h"
#include "slm_common.cuh"

#define Q4K_BLOCK 144
#define Q6K_BLOCK 210

/* ---- embedding lookup: out[B,K] = table[ids[b]], grid (B,1,1) ---- */

__global__ void slm_embed_q4k(const uint32_t *ids, const uint8_t *table,
                              half *out, int K, int B)
{
    const int bat = blockIdx.x;
    const int l = threadIdx.x;
    const int nb = K >> 8;
    const uint8_t *row = table + (size_t)ids[bat] * nb * Q4K_BLOCK;
    half *o = out + (size_t)bat * K;

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk = row + b * Q4K_BLOCK;
        const uint32_t *p = reinterpret_cast<const uint32_t *>(blk);
        const uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];
        const float d = slm_h2f(w0 & 0xFFFFu);
        const float dmin = slm_h2f(w0 >> 16);
        for (int j = 0; j < 4; j++) {
            const uint32_t q = blk[16 + (j << 5) + l];
            uint32_t sc, mn;
            const int is0 = 2 * j, is1 = 2 * j + 1;
            if (is0 < 4) {
                sc = (w1 >> (is0 * 8)) & 0x3Fu;
                mn = (w2 >> (is0 * 8)) & 0x3Fu;
            } else {
                const int s = (is0 - 4) * 8;
                sc = ((w3 >> s) & 0x0Fu) | (((w1 >> s) >> 2) & 0x30u);
                mn = ((w3 >> (s + 4)) & 0x0Fu) | (((w2 >> s) >> 2) & 0x30u);
            }
            o[(b << 8) + (j << 6) + l] =
                __float2half(d * (float)sc * (float)(q & 15u) - dmin * (float)mn);
            if (is1 < 4) {
                sc = (w1 >> (is1 * 8)) & 0x3Fu;
                mn = (w2 >> (is1 * 8)) & 0x3Fu;
            } else {
                const int s = (is1 - 4) * 8;
                sc = ((w3 >> s) & 0x0Fu) | (((w1 >> s) >> 2) & 0x30u);
                mn = ((w3 >> (s + 4)) & 0x0Fu) | (((w2 >> s) >> 2) & 0x30u);
            }
            o[(b << 8) + (j << 6) + 32 + l] =
                __float2half(d * (float)sc * (float)(q >> 4) - dmin * (float)mn);
        }
    }
}

__global__ void slm_embed_q6k(const uint32_t *ids, const uint8_t *table,
                              half *out, int K, int B)
{
    const int bat = blockIdx.x;
    const int l = threadIdx.x;
    const int is = l >> 4;
    const int nb = K >> 8;
    const uint8_t *row = table + (size_t)ids[bat] * nb * Q6K_BLOCK;
    half *o = out + (size_t)bat * K;

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk = row + b * Q6K_BLOCK;
        const float d = slm_h2f(*reinterpret_cast<const uint16_t *>(blk + 208));
        const int8_t *sc = reinterpret_cast<const int8_t *>(blk + 192);
        for (int h = 0; h < 2; h++) {
            const uint32_t ql0 = blk[h * 64 + l];
            const uint32_t ql1 = blk[h * 64 + 32 + l];
            const uint32_t qh = blk[128 + h * 32 + l];
            const int e = (b << 8) + h * 128 + l;
            const int q0 = (int)((ql0 & 15u) | ((qh & 3u) << 4)) - 32;
            const int q1 = (int)((ql1 & 15u) | (((qh >> 2) & 3u) << 4)) - 32;
            const int q2 = (int)((ql0 >> 4) | (((qh >> 4) & 3u) << 4)) - 32;
            const int q3 = (int)((ql1 >> 4) | (((qh >> 6) & 3u) << 4)) - 32;
            o[e] = __float2half(d * (float)sc[h * 8 + is] * (float)q0);
            o[e + 32] = __float2half(d * (float)sc[h * 8 + 2 + is] * (float)q1);
            o[e + 64] = __float2half(d * (float)sc[h * 8 + 4 + is] * (float)q2);
            o[e + 96] = __float2half(d * (float)sc[h * 8 + 6 + is] * (float)q3);
        }
    }
}

/* ---- RMSNorm, F32 gamma: grid (B,1,1) ---- */

__global__ void slm_rmsnorm(const half *x, const float *gamma, half *out,
                            int n, int B, float eps)
{
    const int bat = blockIdx.x;
    const int l = threadIdx.x;
    const int chunks = n >> 5;
    const half *xr = x + (size_t)bat * n;
    half *o = out + (size_t)bat * n;

    float acc = 0.0f;
    for (int c = 0; c < chunks; c++) {
        const float v = __half2float(xr[(c << 5) + l]);
        acc += v * v;
    }
    __syncwarp();
    for (int m = 16; m > 0; m >>= 1) {
        acc += __shfl_xor_sync(0xFFFFFFFFu, acc, m);
    }
    const float inv = 1.0f / sqrtf(acc / (float)n + eps);
    for (int c = 0; c < chunks; c++) {
        const int i = (c << 5) + l;
        o[i] = __float2half(__half2float(xr[i]) * inv * gamma[i]);
    }
}

/* ---- out[i] = a[i] + b[i], over total = B*n elements: grid (ceil(total/32)) ---- */

__global__ void slm_add(const half *a, const half *b, half *out, int total)
{
    const int i = (blockIdx.x << 5) + threadIdx.x;
    if (i < total) {
        out[i] = __float2half(__half2float(a[i]) + __half2float(b[i]));
    }
}

/* ---- out[B,n] = x[B,n] + bias[n]: grid (n/32, B, 1) ---- */

__global__ void slm_add_bias(const half *x, const float *bias, half *out, int n,
                             int B)
{
    const int col = (blockIdx.x << 5) + threadIdx.x;
    const size_t i = (size_t)blockIdx.y * n + col;
    out[i] = __float2half(__half2float(x[i]) + bias[col]);
}

/* ---- SwiGLU: out[i] = silu(gate[i]) * up[i], grid (ceil(total/32)) ---- */

__global__ void slm_swiglu(const half *gate, const half *up, half *out, int total)
{
    const int i = (blockIdx.x << 5) + threadIdx.x;
    if (i < total) {
        const float g = __half2float(gate[i]);
        /* Rounds through FP16 between the steps, as the CPU path does. */
        const half s = __float2half(g / (1.0f + expf(-g)));
        out[i] = __float2half(__half2float(s) * __half2float(up[i]));
    }
}

/* ---- RoPE, half-split pairing: grid (nh, B, 1) ----
 * x and out are [B][nh][hd]; in place is allowed. table is [max_pos][hd] of
 * (cos, sin) FP32 pairs. pos[b] is the position of batch row b. */

__global__ void slm_rope(const half *x, half *out, const float *table,
                         const uint32_t *pos, int nh, int hd, int B)
{
    const int head = blockIdx.x;
    const int bat = blockIdx.y;
    const int l = threadIdx.x;
    const int half_hd = hd >> 1;
    /* Rounded up: half_hd is below 32 for head_dim 32, as in tiny_qwen2.gguf,
     * and a truncating shift would make this a silent no-op. */
    const int chunks = SLM_ROPE_CHUNKS(half_hd);
    const size_t base = ((size_t)bat * nh + head) * hd;
    const float *t = table + (size_t)pos[bat] * hd;
    for (int c = 0; c < chunks; c++) {
        const int i = (c << 5) + l;
        if (i >= half_hd) {
            continue;
        }
        const float cs = t[2 * i];
        const float sn = t[2 * i + 1];
        const float v0 = __half2float(x[base + i]);
        const float v1 = __half2float(x[base + i + half_hd]);
        out[base + i] = __float2half(v0 * cs - v1 * sn);
        out[base + i + half_hd] = __float2half(v0 * sn + v1 * cs);
    }
}

/* ---- KV append: kv[pos[b]][0..width) = src[b][0..width), grid (width/32, B, 1) ---- */

__global__ void slm_kv_append(const half *src, half *kv, const uint32_t *pos,
                              int width, int B)
{
    const int col = (blockIdx.x << 5) + threadIdx.x;
    const int bat = blockIdx.y;
    kv[(size_t)pos[bat] * width + col] = src[(size_t)bat * width + col];
}

/* ---- GQA with online softmax: grid (nhq, B, 1) ----
 *
 * q and out are [B][nhq][hd]; k and v are [T][nhkv][hd]. Query row b sees
 * positions 0..=pos[b], which is the causal mask. Softmax is computed online
 * (running max, rescaled sum), so the kernel never stores a logit vector and
 * needs no buffer proportional to T. The sum of p*V lives in shared memory
 * because hd is a runtime value and SLM-OS launches without a stack.
 */

#define GQA_MAX_HD 256

__global__ void slm_gqa(const half *q, const half *k, const half *v,
                        const uint32_t *pos, half *out, int nhq, int nhkv,
                        int hd, int B)
{
    const int hq = blockIdx.x;
    const int bat = blockIdx.y;
    const int l = threadIdx.x;
    const int group = nhq / nhkv;
    const int hkv = hq / group;
    const int chunks = hd >> 5;
    const int kv_per_pos = nhkv * hd;
    const int T = (int)pos[bat] + 1;
    const float inv_sqrt = 1.0f / sqrtf((float)hd);
    const half *qr = q + ((size_t)bat * nhq + hq) * hd;
    half *o = out + ((size_t)bat * nhq + hq) * hd;

    __shared__ __align__(16) float gqa_acc[GQA_MAX_HD];
    for (int c = 0; c < chunks; c++) {
        gqa_acc[(c << 5) + l] = 0.0f;
    }

    float mx = -INFINITY;
    float sum = 0.0f;
    for (int t = 0; t < T; t++) {
        const size_t off = (size_t)t * kv_per_pos + (size_t)hkv * hd;
        const half *kt = k + off;
        const half *vt = v + off;
        float acc = 0.0f;
        for (int c = 0; c < chunks; c++) {
            const int i = (c << 5) + l;
            acc += __half2float(qr[i]) * __half2float(kt[i]);
        }
        __syncwarp();
        for (int m = 16; m > 0; m >>= 1) {
            acc += __shfl_xor_sync(0xFFFFFFFFu, acc, m);
        }
        const float logit = acc * inv_sqrt;
        if (logit > mx) {
            /* First position: mx is -inf and there is nothing to rescale. */
            const float f = t ? expf(mx - logit) : 0.0f;
            sum *= f;
            for (int c = 0; c < chunks; c++) {
                gqa_acc[(c << 5) + l] *= f;
            }
            mx = logit;
        }
        const float p = expf(logit - mx);
        sum += p;
        for (int c = 0; c < chunks; c++) {
            const int i = (c << 5) + l;
            gqa_acc[i] += p * __half2float(vt[i]);
        }
    }
    const float inv = sum > 0.0f ? 1.0f / sum : 0.0f;
    for (int c = 0; c < chunks; c++) {
        const int i = (c << 5) + l;
        o[i] = __float2half(gqa_acc[i] * inv);
    }
}
