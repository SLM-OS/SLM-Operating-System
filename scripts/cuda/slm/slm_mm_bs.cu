/*
 * slm_mm_bs.cu - batched matmul for prefill with x held in shared memory.
 *
 * out[B,N] = x[B,K] @ W[N,K]^T + bias[N], same arguments and cbuf ABI as
 * slm_mm_q4k.cu and slm_mm_q6k.cu, B in 9..64. x must be 16-byte aligned.
 * Launch with block (32, 8, 1) and grid (ceil(N/120), 1, 1).
 *
 * Why: re-reading x from global memory is what stalls the b64 kernels. At B=64 x
 * is 196 KB against a 128 KB L1; one lane's miss holds its whole warp, so the
 * schedulers sit idle (measured 15 percent issue active). Here eight warps share
 * a CTA, each with its own 15 weight rows. For each pass over K, warp w loads the
 * x of batches 8w..8w+7 into shared memory; after a barrier every warp runs all
 * eight x tiles from there, so a warp reads x once for eight warps' use and
 * nothing it needs for the multiply can miss.
 *
 * Encoding as in slm_mm_q4k.cu: A holds FP16 1 + n/16 and row 15 is 1.0.
 */
#include <cmath>

#include "slm_common.cuh"

#ifndef Q4K_BLOCK_BYTES
#define Q4K_BLOCK_BYTES 144
#endif
#ifndef Q6K_BLOCK_BYTES
#define Q6K_BLOCK_BYTES 210
#endif
#define BS_TILE_ROWS 15
#define BS_WARPS 8

SLM_INL uint32_t bs_pack_lo(uint32_t u)
{
    return SLM_H_ONE2 | ((u & 0x000Fu) << 6) | ((u & 0x0F00u) << 14);
}

SLM_INL uint32_t bs_pack_hi(uint32_t u)
{
    return SLM_H_ONE2 | ((u & 0x00F0u) << 2) | ((u & 0xF000u) << 10);
}

SLM_INL void bs_q4k_factors(uint32_t w1, uint32_t w2, uint32_t w3, int is, float d, float dmin,
                            float &k, float &off)
{
    uint32_t sc, mn;
    if (is < 4) {
        const int s = is * 8;
        sc = (w1 >> s) & 0x3Fu;
        mn = (w2 >> s) & 0x3Fu;
    } else {
        const int s = (is - 4) * 8;
        sc = ((w3 >> s) & 0x0Fu) | (((w1 >> s) >> 2) & 0x30u);
        mn = ((w3 >> (s + 4)) & 0x0Fu) | (((w2 >> s) >> 2) & 0x30u);
    }
    k = 16.0f * d * (float)sc;
    off = k + dmin * (float)mn;
}

/* Store this warp's accumulators for every x tile. */
SLM_INL void bs_finish(float (&acc)[8][4], const float *bias, void *out, int N, int B, int r0,
                       int r1, bool has0, bool has1, int c, int flags)
{
    const float bias0 = (bias && has0) ? bias[r0] : 0.0f;
    const float bias1 = (bias && has1) ? bias[r1] : 0.0f;
#pragma unroll
    for (int xt = 0; xt < 8; xt++) {
        const int n0 = xt * 8 + 2 * c;
        const int n1 = n0 + 1;
        if (has0 && n0 < B) {
            slm_store_out(out, (long)n0 * N + r0, acc[xt][0] + bias0, flags);
        }
        if (has0 && n1 < B) {
            slm_store_out(out, (long)n1 * N + r0, acc[xt][1] + bias0, flags);
        }
        if (has1 && n0 < B) {
            slm_store_out(out, (long)n0 * N + r1, acc[xt][2] + bias1, flags);
        }
        if (has1 && n1 < B) {
            slm_store_out(out, (long)n1 * N + r1, acc[xt][3] + bias1, flags);
        }
    }
}

__global__ void slm_mm_q4k_hmma_bs(const half *x, const uint8_t *W, void *out, const float *bias,
                                   int K, int N, int B, int flags)
{
    const int lane = threadIdx.x;
    const int w = threadIdx.y;
    const int g = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q4K_BLOCK_BYTES;

    const int r0 = (blockIdx.x * BS_WARPS + w) * BS_TILE_ROWS + g;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = g < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? (size_t)r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? (size_t)r1 : 0) * row_bytes;
    const uint32_t keep1 = g == 7 ? 0u : 0xFFFFFFFFu;
    const int off8 = 8 * c;
    const int bat = w * 8 + g;
    const half *xrow = x + (size_t)bat * K;
    const bool xact = bat < B;

    /* [x tile][group][column c][nibble] of one 16-byte x value. */
    __shared__ __align__(16) slm_x16 bsq4_x[8][8][4][2];

    float acc[8][4];
#pragma unroll
    for (int xt = 0; xt < 8; xt++) {
#pragma unroll
        for (int i = 0; i < 4; i++) {
            acc[xt][i] = 0.0f;
        }
    }

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const slm_x16 h0 = *reinterpret_cast<const slm_x16 *>(blk0);
        const slm_x16 h1 = *reinterpret_cast<const slm_x16 *>(blk1);
        const float d0 = has0 ? slm_h2f(h0.w0 & 0xFFFFu) : 0.0f;
        const float m0 = has0 ? slm_h2f(h0.w0 >> 16) : 0.0f;
        const float d1 = has1 ? slm_h2f(h1.w0 & 0xFFFFu) : 0.0f;
        const float m1 = has1 ? slm_h2f(h1.w0 >> 16) : 0.0f;

        for (int j = 0; j < 4; j++) {
            slm_x16 xl = {0, 0, 0, 0}, xh = {0, 0, 0, 0};
            if (xact) {
                const half *xe = xrow + (b << 8) + (j << 6) + off8;
                xl = *reinterpret_cast<const slm_x16 *>(xe);
                xh = *reinterpret_cast<const slm_x16 *>(xe + 32);
            }
            bsq4_x[w][g][c][0] = xl;
            bsq4_x[w][g][c][1] = xh;
            const uint64_t q0 = *reinterpret_cast<const uint64_t *>(blk0 + 16 + (j << 5) + off8);
            const uint64_t q1 = *reinterpret_cast<const uint64_t *>(blk1 + 16 + (j << 5) + off8);
            __syncthreads();

            const uint32_t u0a = (uint32_t)q0, u0b = (uint32_t)(q0 >> 32);
            const uint32_t u1a = (uint32_t)q1 & keep1, u1b = (uint32_t)(q1 >> 32) & keep1;
            float kl0, offl0, kl1, offl1, kh0, offh0, kh1, offh1;
            bs_q4k_factors(h0.w1, h0.w2, h0.w3, 2 * j, d0, m0, kl0, offl0);
            bs_q4k_factors(h1.w1, h1.w2, h1.w3, 2 * j, d1, m1, kl1, offl1);
            bs_q4k_factors(h0.w1, h0.w2, h0.w3, 2 * j + 1, d0, m0, kh0, offh0);
            bs_q4k_factors(h1.w1, h1.w2, h1.w3, 2 * j + 1, d1, m1, kh1, offh1);

#pragma unroll
            for (int hi = 0; hi < 2; hi++) {
                const uint32_t a0a = hi ? bs_pack_hi(u0a) : bs_pack_lo(u0a);
                const uint32_t a1a = hi ? bs_pack_hi(u1a) : bs_pack_lo(u1a);
                const uint32_t a2a = hi ? bs_pack_hi(u0a >> 16) : bs_pack_lo(u0a >> 16);
                const uint32_t a3a = hi ? bs_pack_hi(u1a >> 16) : bs_pack_lo(u1a >> 16);
                const uint32_t a0b = hi ? bs_pack_hi(u0b) : bs_pack_lo(u0b);
                const uint32_t a1b = hi ? bs_pack_hi(u1b) : bs_pack_lo(u1b);
                const uint32_t a2b = hi ? bs_pack_hi(u0b >> 16) : bs_pack_lo(u0b >> 16);
                const uint32_t a3b = hi ? bs_pack_hi(u1b >> 16) : bs_pack_lo(u1b >> 16);
                const float k0 = hi ? kh0 : kl0;
                const float off0 = hi ? offh0 : offl0;
                const float k1 = hi ? kh1 : kl1;
                const float off1 = hi ? offh1 : offl1;

#pragma unroll
                for (int xt = 0; xt < 8; xt++) {
                    const slm_x16 xv = bsq4_x[xt][g][c][hi];
                    float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
                    slm_mma(c0, c1, c2, c3, a0a, a1a, a2a, a3a, xv.w0, xv.w1);
                    slm_mma(c0, c1, c2, c3, a0b, a1b, a2b, a3b, xv.w2, xv.w3);
                    const float s0 = __shfl_sync(0xFFFFFFFFu, c2, 28 + c);
                    const float s1 = __shfl_sync(0xFFFFFFFFu, c3, 28 + c);
                    acc[xt][0] = fmaf(-off0, s0, fmaf(k0, c0, acc[xt][0]));
                    acc[xt][1] = fmaf(-off0, s1, fmaf(k0, c1, acc[xt][1]));
                    acc[xt][2] = fmaf(-off1, s0, fmaf(k1, c2, acc[xt][2]));
                    acc[xt][3] = fmaf(-off1, s1, fmaf(k1, c3, acc[xt][3]));
                }
            }
            __syncthreads();
        }
    }
    bs_finish(acc, bias, out, N, B, r0, r1, has0, has1, c, flags);
}

SLM_INL uint32_t bs_u32(const uint8_t *p)
{
    return (uint32_t)*reinterpret_cast<const uint16_t *>(p) |
           ((uint32_t)*reinterpret_cast<const uint16_t *>(p + 2) << 16);
}

__global__ void slm_mm_q6k_hmma_bs(const half *x, const uint8_t *W, void *out, const float *bias,
                                   int K, int N, int B, int flags)
{
    const int lane = threadIdx.x;
    const int w = threadIdx.y;
    const int gr = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q6K_BLOCK_BYTES;

    const int r0 = (blockIdx.x * BS_WARPS + w) * BS_TILE_ROWS + gr;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = gr < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? (size_t)r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? (size_t)r1 : 0) * row_bytes;
    const uint32_t keep1 = gr == 7 ? 0u : 0xFFFFFFFFu;
    const int off4 = 4 * c;
    const int bat = w * 8 + gr;
    const half *xrow = x + (size_t)bat * K;
    const bool xact = bat < B;

    /* [x tile][group][column c][sub-group g] of one 8-byte x value. */
    __shared__ __align__(8) uint64_t bsq6_x[8][8][4][4];

    float acc[8][4];
#pragma unroll
    for (int xt = 0; xt < 8; xt++) {
#pragma unroll
        for (int i = 0; i < 4; i++) {
            acc[xt][i] = 0.0f;
        }
    }

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q6K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q6K_BLOCK_BYTES;
        const float d0 = has0 ? slm_h2f(*reinterpret_cast<const uint16_t *>(blk0 + 208)) : 0.0f;
        const float d1 = has1 ? slm_h2f(*reinterpret_cast<const uint16_t *>(blk1 + 208)) : 0.0f;

        for (int h = 0; h < 2; h++) {
            const uint32_t s0a = bs_u32(blk0 + 192 + h * 8), s0b = bs_u32(blk0 + 196 + h * 8);
            const uint32_t s1a = bs_u32(blk1 + 192 + h * 8), s1b = bs_u32(blk1 + 196 + h * 8);

            for (int t = 0; t < 2; t++) {
                const int l = t * 16 + off4;
                const int ek = (b << 8) + h * 128 + l;
#pragma unroll
                for (int g = 0; g < 4; g++) {
                    uint64_t xv = 0;
                    if (xact) {
                        xv = *reinterpret_cast<const uint64_t *>(xrow + ek + g * 32);
                    }
                    bsq6_x[w][gr][c][g] = xv;
                }
                const uint32_t la0 = bs_u32(blk0 + h * 64 + l);
                const uint32_t lb0 = bs_u32(blk0 + h * 64 + 32 + l);
                const uint32_t qh0 = bs_u32(blk0 + 128 + h * 32 + l);
                const uint32_t la1 = bs_u32(blk1 + h * 64 + l) & keep1;
                const uint32_t lb1 = bs_u32(blk1 + h * 64 + 32 + l) & keep1;
                const uint32_t qh1 = bs_u32(blk1 + 128 + h * 32 + l) & keep1;
                __syncthreads();

#pragma unroll
                for (int g = 0; g < 4; g++) {
                    const int ns = (g >> 1) * 4;
                    const uint32_t q0 = (g & 1) ? lb0 : la0;
                    const uint32_t q1 = (g & 1) ? lb1 : la1;
                    const uint32_t v0 = ((q0 >> ns) & 0x0F0F0F0Fu) |
                                        (((qh0 >> (2 * g)) & 0x03030303u) << 4);
                    const uint32_t v1 = ((q1 >> ns) & 0x0F0F0F0Fu) |
                                        (((qh1 >> (2 * g)) & 0x03030303u) << 4);
                    /* FP16 1 + q/64 in adjacent bytes' pairs, as q6k_pack. */
                    const uint32_t a0 = SLM_H_ONE2 | ((v0 & 0x003Fu) << 4) | ((v0 & 0x3F00u) << 12);
                    const uint32_t a1 = SLM_H_ONE2 | ((v1 & 0x003Fu) << 4) | ((v1 & 0x3F00u) << 12);
                    const uint32_t a2 = SLM_H_ONE2 | ((v0 & 0x003F0000u) >> 12) | ((v0 & 0x3F000000u) >> 4);
                    const uint32_t a3 = SLM_H_ONE2 | ((v1 & 0x003F0000u) >> 12) | ((v1 & 0x3F000000u) >> 4);

                    const int idx = 2 * g + t;
                    const uint32_t sw0 = idx < 4 ? s0a : s0b;
                    const uint32_t sw1 = idx < 4 ? s1a : s1b;
                    const int sh = (idx & 3) * 8;
                    const float f0 = d0 * (float)(int8_t)(sw0 >> sh);
                    const float f1 = d1 * (float)(int8_t)(sw1 >> sh);

#pragma unroll
                    for (int xt = 0; xt < 8; xt++) {
                        const uint64_t xv = bsq6_x[xt][gr][c][g];
                        float c0 = 0.f, c1 = 0.f, c2 = 0.f, c3 = 0.f;
                        slm_mma(c0, c1, c2, c3, a0, a1, a2, a3, (uint32_t)xv,
                                (uint32_t)(xv >> 32));
                        const float s_0 = -96.0f * __shfl_sync(0xFFFFFFFFu, c2, 28 + c);
                        const float s_1 = -96.0f * __shfl_sync(0xFFFFFFFFu, c3, 28 + c);
                        acc[xt][0] = fmaf(f0, fmaf(64.0f, c0, s_0), acc[xt][0]);
                        acc[xt][1] = fmaf(f0, fmaf(64.0f, c1, s_1), acc[xt][1]);
                        acc[xt][2] = fmaf(f1, fmaf(64.0f, c2, s_0), acc[xt][2]);
                        acc[xt][3] = fmaf(f1, fmaf(64.0f, c3, s_1), acc[xt][3]);
                    }
                }
                __syncthreads();
            }
        }
    }
    bs_finish(acc, bias, out, N, B, r0, r1, has0, has1, c, flags);
}
