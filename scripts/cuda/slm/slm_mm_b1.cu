/*
 * slm_mm_b1.cu - single-row matmul, out[N] = W[N,K] @ x[K], for decode.
 *
 * Same contract and cbuf ABI as slm_mm_q4k.cu and slm_mm_q6k.cu with B = 1, plus
 * two differences, both for speed:
 *
 *   flags bits 8..15 = nw, warps per CTA (0 means 1). Launch with block
 *   (32, nw, 1) and grid (ceil(N/15), 1, 1). Each warp takes every nw-th weight
 *   block. A big matmul has plenty of warps and does not need this; a small one
 *   (attn_k has 18 tiles) is otherwise limited by one warp's serial latency.
 *
 *   x must be 16-byte aligned, as for the permuted b8 and b64 kernels.
 *
 * The SMs are issue-limited on these kernels (measured: about one instruction
 * per active cycle per scheduler, warps queued behind it, DRAM well under its
 * ceiling), so everything here counts instructions per mma:
 *
 *   - Every lane loads x, so there is no branch, and only column 0 is used.
 *     Columns 1..7 repeat it and their results are ignored, so c1 and c3 are
 *     never reset and only c0 and c2 are read: half the zeroing and half the
 *     FMAs of the b8 kernel.
 *   - Weights go in as raw FP16 subnormals, n * 2^-24 instead of the 1 + n/16 of
 *     the other kernels, so packing is one shift and one mask. Over a sub-block,
 *     with H = sum(n * 2^-24 * x) and S = sum(x): Q4_K gives
 *     d*sc*2^24*H - dmin*mn*S, Q6_K gives d*sc*(2^24*H - 32*S). Row 15 stays 1.0
 *     to produce S.
 *   - A 32-bit register of FP16 takes bytes 0 and 2 of the weight word, so the x
 *     register holds elements 0 and 2, and the next holds 1 and 3.
 *   - The loops over j are unrolled, which folds the scale field selection.
 */
#include <cmath>

#include "slm_common.cuh"

#ifndef Q4K_BLOCK_BYTES
#define Q4K_BLOCK_BYTES 144
#endif
#ifndef Q6K_BLOCK_BYTES
#define Q6K_BLOCK_BYTES 210
#endif
#define B1_TILE_ROWS 15
#define B1_MAX_WARPS 8

/* A value in the low bits of an FP16 is a subnormal, n * 2^-24, exactly. The tensor
 * core honours these (slm_spike's probe_subnormal pins it), so no exponent needs
 * to be merged in. The two 16-bit halves of a register come from bytes 0 and 2. */
#define B1_N4 0x000F000Fu
#define B1_N6 0x003F003Fu
#define B1_2P24 16777216.0f

/* Elements (0,2) and (1,3) of two consecutive x words. */
SLM_INL uint32_t b1_x02(uint32_t w0, uint32_t w1)
{
    return (w0 & 0xFFFFu) | (w1 << 16);
}

SLM_INL uint32_t b1_x13(uint32_t w0, uint32_t w1)
{
    return (w0 >> 16) | (w1 & 0xFFFF0000u);
}

/* Publish this warp's two accumulators and sum them in warp 0. */
SLM_INL void b1_finish(float acc0, float acc2, int nw, int w, int g, int c, const float *bias,
                       void *out, int r0, int r1, bool has0, bool has1, int flags,
                       float (*shared)[8][2])
{
    if (nw > 1) {
        if (c == 0) {
            shared[w][g][0] = acc0;
            shared[w][g][1] = acc2;
        }
        __syncthreads();
        if (w != 0) {
            return;
        }
        if (c == 0) {
            for (int i = 1; i < nw; i++) {
                acc0 += shared[i][g][0];
                acc2 += shared[i][g][1];
            }
        }
    }
    if (c == 0) {
        if (has0) {
            slm_store_out(out, r0, acc0 + (bias ? bias[r0] : 0.0f), flags);
        }
        if (has1) {
            slm_store_out(out, r1, acc2 + (bias ? bias[r1] : 0.0f), flags);
        }
    }
}

/* ---- Q4_K ---- */

SLM_INL void b1_q4k_scales(uint32_t w1, uint32_t w2, uint32_t w3, int is, float d16, float dmin,
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
    k = d16 * (float)sc;
    off = dmin * (float)mn;
}

__global__ void slm_mm_q4k_hmma_b1(const half *x, const uint8_t *W, void *out, const float *bias,
                                   int K, int N, int B, int flags)
{
    const int lane = threadIdx.x;
    const int w = threadIdx.y;
    const int g = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const int nw = ((flags >> 8) & 0xFF) ? ((flags >> 8) & 0xFF) : 1;
    const size_t row_bytes = (size_t)nb * Q4K_BLOCK_BYTES;

    const int r0 = blockIdx.x * B1_TILE_ROWS + g;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = g < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    const uint32_t keep1 = g == 7 ? 0u : 0xFFFFFFFFu;
    const uint32_t one1 = g == 7 ? SLM_H_ONE2 : 0u;
    const int off8 = 8 * c;

    float acc0 = 0.0f, acc2 = 0.0f;
    float cl1 = 0.f, cl3 = 0.f, ch1 = 0.f, ch3 = 0.f;
    __syncwarp();

    for (int b = w; b < nb; b += nw) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const slm_x16 h0 = *reinterpret_cast<const slm_x16 *>(blk0);
        const slm_x16 h1 = *reinterpret_cast<const slm_x16 *>(blk1);
        const float d0 = has0 ? B1_2P24 * slm_h2f(h0.w0 & 0xFFFFu) : 0.0f;
        const float m0 = has0 ? slm_h2f(h0.w0 >> 16) : 0.0f;
        const float d1 = has1 ? B1_2P24 * slm_h2f(h1.w0 & 0xFFFFu) : 0.0f;
        const float m1 = has1 ? slm_h2f(h1.w0 >> 16) : 0.0f;
        const half *xb = x + (b << 8) + off8;

#pragma unroll
        for (int j = 0; j < 4; j++) {
            const uint64_t q0 = *reinterpret_cast<const uint64_t *>(blk0 + 16 + (j << 5) + off8);
            const uint64_t q1 = *reinterpret_cast<const uint64_t *>(blk1 + 16 + (j << 5) + off8);
            const uint32_t u0a = (uint32_t)q0, u0b = (uint32_t)(q0 >> 32);
            const uint32_t u1a = (uint32_t)q1 & keep1, u1b = (uint32_t)(q1 >> 32) & keep1;
            const slm_x16 xl = *reinterpret_cast<const slm_x16 *>(xb + (j << 6));
            const slm_x16 xh = *reinterpret_cast<const slm_x16 *>(xb + (j << 6) + 32);

            float cl0 = 0.f, cl2 = 0.f, ch0 = 0.f, ch2 = 0.f;
            /* A registers: (byte0,byte2) with x(0,2), (byte1,byte3) with x(1,3).
             * Low nibble of byte0 is already in place, the others shift down. */
            slm_mma(cl0, cl1, cl2, cl3,
                    u0a & B1_N4, one1 | (u1a & B1_N4),
                    (u0a >> 8) & B1_N4, one1 | ((u1a >> 8) & B1_N4),
                    b1_x02(xl.w0, xl.w1), b1_x13(xl.w0, xl.w1));
            slm_mma(cl0, cl1, cl2, cl3,
                    u0b & B1_N4, one1 | (u1b & B1_N4),
                    (u0b >> 8) & B1_N4, one1 | ((u1b >> 8) & B1_N4),
                    b1_x02(xl.w2, xl.w3), b1_x13(xl.w2, xl.w3));
            slm_mma(ch0, ch1, ch2, ch3,
                    (u0a >> 4) & B1_N4, one1 | ((u1a >> 4) & B1_N4),
                    (u0a >> 12) & B1_N4, one1 | ((u1a >> 12) & B1_N4),
                    b1_x02(xh.w0, xh.w1), b1_x13(xh.w0, xh.w1));
            slm_mma(ch0, ch1, ch2, ch3,
                    (u0b >> 4) & B1_N4, one1 | ((u1b >> 4) & B1_N4),
                    (u0b >> 12) & B1_N4, one1 | ((u1b >> 12) & B1_N4),
                    b1_x02(xh.w2, xh.w3), b1_x13(xh.w2, xh.w3));

            /* Every column repeats x, so S is row 15 column 0, held by lane 28. */
            const float sl = __shfl_sync(0xFFFFFFFFu, cl2, 28);
            const float sh = __shfl_sync(0xFFFFFFFFu, ch2, 28);

            /* q8_1 scales of the two 32-element blocks (1 for plain x). */
            const float dl = slm_xq_d(x, K, flags, (b << 3) + 2 * j);
            const float dh = slm_xq_d(x, K, flags, (b << 3) + 2 * j + 1);
            float k, off;
            b1_q4k_scales(h0.w1, h0.w2, h0.w3, 2 * j, d0, m0, k, off);
            acc0 = fmaf(dl, fmaf(-off, sl, k * cl0), acc0);
            b1_q4k_scales(h1.w1, h1.w2, h1.w3, 2 * j, d1, m1, k, off);
            acc2 = fmaf(dl, fmaf(-off, sl, k * cl2), acc2);
            b1_q4k_scales(h0.w1, h0.w2, h0.w3, 2 * j + 1, d0, m0, k, off);
            acc0 = fmaf(dh, fmaf(-off, sh, k * ch0), acc0);
            b1_q4k_scales(h1.w1, h1.w2, h1.w3, 2 * j + 1, d1, m1, k, off);
            acc2 = fmaf(dh, fmaf(-off, sh, k * ch2), acc2);
        }
    }

    __shared__ float b1q4_shared[B1_MAX_WARPS][8][2];
    b1_finish(acc0, acc2, nw, w, g, c, bias, out, r0, r1, has0, has1, flags, b1q4_shared);
}

/* ---- Q6_K ---- */

/* 32 bits at a 2-byte-aligned address. Two 16-bit loads cost fewer instructions
 * than an aligned load, a shift and a branch, and instructions are the limit. */
SLM_INL uint32_t b1_load32(const uint8_t *p)
{
    return (uint32_t)*reinterpret_cast<const uint16_t *>(p) |
           ((uint32_t)*reinterpret_cast<const uint16_t *>(p + 2) << 16);
}

__global__ void slm_mm_q6k_hmma_b1(const half *x, const uint8_t *W, void *out, const float *bias,
                                   int K, int N, int B, int flags)
{
    const int lane = threadIdx.x;
    const int w = threadIdx.y;
    const int gr = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const int nw = ((flags >> 8) & 0xFF) ? ((flags >> 8) & 0xFF) : 1;
    const size_t row_bytes = (size_t)nb * Q6K_BLOCK_BYTES;

    const int r0 = blockIdx.x * B1_TILE_ROWS + gr;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = gr < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    const uint32_t keep1 = gr == 7 ? 0u : 0xFFFFFFFFu;
    const uint32_t one1 = gr == 7 ? SLM_H_ONE2 : 0u;
    const int off4 = 4 * c;

    float acc0 = 0.0f, acc2 = 0.0f;
    float c1 = 0.f, c3 = 0.f;
    __syncwarp();

    for (int b = w; b < nb; b += nw) {
        const uint8_t *blk0 = w0 + b * Q6K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q6K_BLOCK_BYTES;
        const float d0 = has0 ? slm_h2f(*reinterpret_cast<const uint16_t *>(blk0 + 208)) : 0.0f;
        const float d1 = has1 ? slm_h2f(*reinterpret_cast<const uint16_t *>(blk1 + 208)) : 0.0f;

#pragma unroll
        for (int h = 0; h < 2; h++) {
            const uint32_t s0a = b1_load32(blk0 + 192 + h * 8);
            const uint32_t s0b = b1_load32(blk0 + 196 + h * 8);
            const uint32_t s1a = b1_load32(blk1 + 192 + h * 8);
            const uint32_t s1b = b1_load32(blk1 + 196 + h * 8);

#pragma unroll
            for (int t = 0; t < 2; t++) {
                const int l = t * 16 + off4;
                const uint32_t la0 = b1_load32(blk0 + h * 64 + l);
                const uint32_t lb0 = b1_load32(blk0 + h * 64 + 32 + l);
                const uint32_t qh0 = b1_load32(blk0 + 128 + h * 32 + l);
                const uint32_t la1 = b1_load32(blk1 + h * 64 + l) & keep1;
                const uint32_t lb1 = b1_load32(blk1 + h * 64 + 32 + l) & keep1;
                const uint32_t qh1 = b1_load32(blk1 + 128 + h * 32 + l) & keep1;
                const half *xk = x + (b << 8) + h * 128 + l;

#pragma unroll
                for (int g = 0; g < 4; g++) {
                    /* Four 6-bit values per word: low nibble in bits 0..3 of
                     * each byte, high two bits in bits 4..5. */
                    const uint32_t q0 = (g & 1) ? lb0 : la0;
                    const uint32_t q1 = (g & 1) ? lb1 : la1;
                    const int ns = (g >> 1) * 4;
                    const uint32_t v0 = ((q0 >> ns) & 0x0F0F0F0Fu) |
                                        (((qh0 >> (2 * g)) & 0x03030303u) << 4);
                    const uint32_t v1 = ((q1 >> ns) & 0x0F0F0F0Fu) |
                                        (((qh1 >> (2 * g)) & 0x03030303u) << 4);
                    /* x elements l..l+3: an 8-byte load. */
                    const uint64_t xv = *reinterpret_cast<const uint64_t *>(xk + g * 32);
                    const uint32_t xw0 = (uint32_t)xv, xw1 = (uint32_t)(xv >> 32);

                    float c0 = 0.f, c2 = 0.f;
                    slm_mma(c0, c1, c2, c3,
                            v0 & B1_N6, one1 | (v1 & B1_N6),
                            (v0 >> 8) & B1_N6, one1 | ((v1 >> 8) & B1_N6),
                            b1_x02(xw0, xw1), b1_x13(xw0, xw1));
                    const float s = __shfl_sync(0xFFFFFFFFu, c2, 28);
                    const float s32 = -32.0f * s;

                    const int idx = 2 * g + t;
                    const uint32_t sw0 = idx < 4 ? s0a : s0b;
                    const uint32_t sw1 = idx < 4 ? s1a : s1b;
                    const int sh = (idx & 3) * 8;
                    const float dx = slm_xq_d(x, K, flags, (b << 3) + h * 4 + g);
                    const float f0 = dx * d0 * (float)(int8_t)(sw0 >> sh);
                    const float f1 = dx * d1 * (float)(int8_t)(sw1 >> sh);
                    /* f * (2^24*H - 32*S) */
                    acc0 = fmaf(f0, fmaf(B1_2P24, c0, s32), acc0);
                    acc2 = fmaf(f1, fmaf(B1_2P24, c2, s32), acc2);
                }
            }
        }
    }

    __shared__ float b1q6_shared[B1_MAX_WARPS][8][2];
    b1_finish(acc0, acc2, nw, w, gr, c, bias, out, r0, r1, has0, has1, flags, b1q6_shared);
}
