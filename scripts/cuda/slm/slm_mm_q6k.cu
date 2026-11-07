/*
 * slm_mm_q6k.cu - out[B,N] = x[B,K] @ W[N,K]^T with on-the-fly Q6_K decode.
 *
 * Same contract, cbuf ABI and launch shapes as slm_mm_q4k.cu, for the Q6_K
 * tensors of this model: output.weight and half of attn_v and ffn_down.
 *
 * W     Q6_K, row-major [N][K/256 blocks of 210 bytes]. 2-byte aligned at
 *       best (210 is not a multiple of 4), so every load here is 16-bit.
 *
 * Q6_K block: ql[128] qh[64] scales[16] (int8) d (FP16). Element value:
 *   w = d * sc * (q - 32), with q in 0..63 and sc per 16 elements.
 * For element e, with h = e/128, g = (e%128)/32, l = e%32:
 *   low 4 bits   ql[h*64 + (g&1)*32 + l], high nibble when g >= 2
 *   high 2 bits  (qh[h*32 + l] >> (2*g)) & 3
 *   scale        scales[h*8 + 2*g + l/16]
 *
 * Offset encoding as in slm_mm_q4k.cu, one more bit of precision:
 *   hv = 0x3C00 | (q << 4), so hv = 1 + q/64 exactly, and over a sub-block
 *   sum(w * x) = d*sc*(64*H - 96*S)
 *
 * Column permutation as in slm_mm_q4k.cu: lane c takes bytes 4c..4c+3 of a
 * 16-byte sub-block, so one 32-bit value per stream per row. Blocks are only
 * 2-byte aligned, so q6k_load32 reads the aligned 8 bytes around the value and
 * shifts; only a lane whose value crosses an 8-byte boundary loads again.
 */
#include "slm_common.cuh"

#define Q6K_BLOCK_BYTES 210
#ifndef Q4K_TILE_ROWS
#define Q4K_TILE_ROWS 15
#endif
#define Q6K_TILE_ROWS Q4K_TILE_ROWS

SLM_INL uint32_t q6k_u16(const uint8_t *p)
{
    return *reinterpret_cast<const uint16_t *>(p);
}

/* Two 6-bit values from a 16-bit ql load and a 16-bit qh load, packed as FP16
 * 1 + q/64. */
SLM_INL uint32_t q6k_pack(uint32_t ql, uint32_t qh, int nib_hi, int g)
{
    const uint32_t lo = (nib_hi ? (ql >> 4) : ql) & 0x0F0Fu;
    const uint32_t hi = (qh >> (2 * g)) & 0x0303u;
    const uint32_t v = lo | (hi << 4);
    return SLM_H_ONE2 | ((v & 0x003Fu) << 4) | ((v & 0x3F00u) << 12);
}

/* 32 bits at a 2-byte-aligned address. */
SLM_INL uint32_t q6k_load32(const uint8_t *p)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const int sh = (int)(a & 6u);
    const uint8_t *base = p - sh;
    uint32_t v = (uint32_t)(*reinterpret_cast<const uint64_t *>(base) >> (sh * 8));
    if (sh == 6) {
        v |= (uint32_t)*reinterpret_cast<const uint16_t *>(base + 8) << 16;
    }
    return v;
}

/* ---------------------------------------------------------------------- */
/* SIMT reference: one row and one batch per warp.                        */
/* ---------------------------------------------------------------------- */

__global__ void slm_mm_q6k_simt(const half *x, const uint8_t *W, void *out,
                                const float *bias, int K, int N, int B,
                                int flags)
{
    const int row = blockIdx.x;
    const int bat = blockIdx.y;
    const int l = threadIdx.x;
    const int is = l >> 4;
    const int nb = K >> 8;
    const uint8_t *wrow = W + (size_t)row * nb * Q6K_BLOCK_BYTES;
    const half *xrow = x + (size_t)bat * K;

    float acc = 0.0f;
    for (int b = 0; b < nb; b++) {
        const uint8_t *blk = wrow + b * Q6K_BLOCK_BYTES;
        const float d = slm_h2f(q6k_u16(blk + 208));
        const int8_t *scales = reinterpret_cast<const int8_t *>(blk + 192);
        const half *xb = xrow + (b << 8);
        for (int h = 0; h < 2; h++) {
            const uint32_t ql0 = blk[h * 64 + l];
            const uint32_t ql1 = blk[h * 64 + 32 + l];
            const uint32_t qh = blk[128 + h * 32 + l];
            const int e = h * 128 + l;
            const int q0 = (int)((ql0 & 15u) | ((qh & 3u) << 4)) - 32;
            const int q1 = (int)((ql1 & 15u) | (((qh >> 2) & 3u) << 4)) - 32;
            const int q2 = (int)((ql0 >> 4) | (((qh >> 4) & 3u) << 4)) - 32;
            const int q3 = (int)((ql1 >> 4) | (((qh >> 6) & 3u) << 4)) - 32;
            acc += d * (float)scales[h * 8 + is] * (float)q0 *
                   __half2float(xb[e]);
            acc += d * (float)scales[h * 8 + 2 + is] * (float)q1 *
                   __half2float(xb[e + 32]);
            acc += d * (float)scales[h * 8 + 4 + is] * (float)q2 *
                   __half2float(xb[e + 64]);
            acc += d * (float)scales[h * 8 + 6 + is] * (float)q3 *
                   __half2float(xb[e + 96]);
        }
    }
    __syncwarp();
    for (int m = 16; m > 0; m >>= 1) {
        acc += __shfl_xor_sync(0xFFFFFFFFu, acc, m);
    }
    if (l == 0) {
        const float v = acc + (bias ? bias[row] : 0.0f);
        slm_store_out(out, (long)bat * N + row, v, flags);
    }
}

/* ---------------------------------------------------------------------- */
/* HMMA, B in 1..8 (layout notes in slm_mm_q4k.cu).                       */
/* ---------------------------------------------------------------------- */

__global__ void slm_mm_q6k_hmma_b8(const half *x, const uint8_t *W, void *out,
                                   const float *bias, int K, int N, int B,
                                   int flags)
{
    const int lane = threadIdx.x;
    const int gr = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q6K_BLOCK_BYTES;

    const int r0 = blockIdx.x * Q6K_TILE_ROWS + gr;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = gr < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    const bool xact = gr < B;
    const half *xrow = x + (size_t)gr * K;
    /* Row 15's bits read as zero, which encodes exactly 1.0. */
    const uint32_t keep1 = gr == 7 ? 0u : 0xFFFFFFFFu;
    const int off4 = 4 * c;

    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    __syncwarp();

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q6K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q6K_BLOCK_BYTES;
        const float d0 = has0 ? slm_h2f(q6k_u16(blk0 + 208)) : 0.0f;
        const float d1 = has1 ? slm_h2f(q6k_u16(blk1 + 208)) : 0.0f;

        for (int h = 0; h < 2; h++) {
            const uint32_t s0a = q6k_load32(blk0 + 192 + h * 8);
            const uint32_t s0b = q6k_load32(blk0 + 196 + h * 8);
            const uint32_t s1a = q6k_load32(blk1 + 192 + h * 8);
            const uint32_t s1b = q6k_load32(blk1 + 196 + h * 8);

            for (int t = 0; t < 2; t++) {
                const int l = t * 16 + off4;
                const uint32_t la0 = q6k_load32(blk0 + h * 64 + l);
                const uint32_t lb0 = q6k_load32(blk0 + h * 64 + 32 + l);
                const uint32_t qh0 = q6k_load32(blk0 + 128 + h * 32 + l);
                const uint32_t la1 = q6k_load32(blk1 + h * 64 + l) & keep1;
                const uint32_t lb1 = q6k_load32(blk1 + h * 64 + 32 + l) & keep1;
                const uint32_t qh1 = q6k_load32(blk1 + 128 + h * 32 + l) & keep1;

                for (int g = 0; g < 4; g++) {
                    const int nib_hi = g >> 1;
                    const uint32_t q0 = (g & 1) ? lb0 : la0;
                    const uint32_t q1 = (g & 1) ? lb1 : la1;
                    const uint32_t a0 = q6k_pack(q0, qh0, nib_hi, g);
                    const uint32_t a1 = q6k_pack(q1, qh1, nib_hi, g);
                    const uint32_t a2 = q6k_pack(q0 >> 16, qh0 >> 16, nib_hi, g);
                    const uint32_t a3 = q6k_pack(q1 >> 16, qh1 >> 16, nib_hi, g);

                    uint32_t b0 = 0, b1 = 0;
                    if (xact) {
                        const uint64_t xv = *reinterpret_cast<const uint64_t *>(
                            xrow + (b << 8) + h * 128 + g * 32 + l);
                        b0 = (uint32_t)xv;
                        b1 = (uint32_t)(xv >> 32);
                    }
                    __syncwarp();

                    float c0 = 0.f, c1 = 0.f, c2 = 0.f, c3 = 0.f;
                    slm_mma(c0, c1, c2, c3, a0, a1, a2, a3, b0, b1);

                    const float s_0 = __shfl_sync(0xFFFFFFFFu, c2, 28 + c);
                    const float s_1 = __shfl_sync(0xFFFFFFFFu, c3, 28 + c);

                    const int idx = 2 * g + t;
                    const uint32_t sw0 = idx < 4 ? s0a : s0b;
                    const uint32_t sw1 = idx < 4 ? s1a : s1b;
                    const int sh = (idx & 3) * 8;
                    const float f0 = d0 * (float)(int8_t)(sw0 >> sh);
                    const float f1 = d1 * (float)(int8_t)(sw1 >> sh);
                    /* f * (64*H - 96*S) */
                    acc0 += f0 * (64.0f * c0 - 96.0f * s_0);
                    acc1 += f0 * (64.0f * c1 - 96.0f * s_1);
                    acc2 += f1 * (64.0f * c2 - 96.0f * s_0);
                    acc3 += f1 * (64.0f * c3 - 96.0f * s_1);
                }
            }
        }
    }

    const int b0 = 2 * c;
    const int b1 = 2 * c + 1;
    const float bias0 = (bias && has0) ? bias[r0] : 0.0f;
    const float bias1 = (bias && has1) ? bias[r1] : 0.0f;
    if (has0 && b0 < B) {
        slm_store_out(out, (long)b0 * N + r0, acc0 + bias0, flags);
    }
    if (has0 && b1 < B) {
        slm_store_out(out, (long)b1 * N + r0, acc1 + bias0, flags);
    }
    if (has1 && b0 < B) {
        slm_store_out(out, (long)b0 * N + r1, acc2 + bias1, flags);
    }
    if (has1 && b1 < B) {
        slm_store_out(out, (long)b1 * N + r1, acc3 + bias1, flags);
    }
}

/* ---------------------------------------------------------------------- */
/* HMMA, B in 1..64: eight x tiles per weight tile, register accumulators. */
/* ---------------------------------------------------------------------- */

#define Q6K_XTILES 8

__global__ void slm_mm_q6k_hmma_b64(const half *x, const uint8_t *W, void *out,
                                    const float *bias, int K, int N, int B,
                                    int flags)
{
    const int lane = threadIdx.x;
    const int gr = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q6K_BLOCK_BYTES;

    const int r0 = blockIdx.x * Q6K_TILE_ROWS + gr;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = gr < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    const uint32_t keep1 = gr == 7 ? 0u : 0xFFFFFFFFu;
    const int off4 = 4 * c;
    const int xtiles = (B + 7) >> 3;

    float acc[Q6K_XTILES][4];
#pragma unroll
    for (int xt = 0; xt < Q6K_XTILES; xt++) {
#pragma unroll
        for (int i = 0; i < 4; i++) {
            acc[xt][i] = 0.0f;
        }
    }
    __syncwarp();

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q6K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q6K_BLOCK_BYTES;
        const float d0 = has0 ? slm_h2f(q6k_u16(blk0 + 208)) : 0.0f;
        const float d1 = has1 ? slm_h2f(q6k_u16(blk1 + 208)) : 0.0f;

        for (int h = 0; h < 2; h++) {
            const uint32_t s0a = q6k_load32(blk0 + 192 + h * 8);
            const uint32_t s0b = q6k_load32(blk0 + 196 + h * 8);
            const uint32_t s1a = q6k_load32(blk1 + 192 + h * 8);
            const uint32_t s1b = q6k_load32(blk1 + 196 + h * 8);

            for (int t = 0; t < 2; t++) {
                const int l = t * 16 + off4;
                const uint32_t la0 = q6k_load32(blk0 + h * 64 + l);
                const uint32_t lb0 = q6k_load32(blk0 + h * 64 + 32 + l);
                const uint32_t qh0 = q6k_load32(blk0 + 128 + h * 32 + l);
                const uint32_t la1 = q6k_load32(blk1 + h * 64 + l) & keep1;
                const uint32_t lb1 = q6k_load32(blk1 + h * 64 + 32 + l) & keep1;
                const uint32_t qh1 = q6k_load32(blk1 + 128 + h * 32 + l) & keep1;

                for (int g = 0; g < 4; g++) {
                    const int nib_hi = g >> 1;
                    const uint32_t q0 = (g & 1) ? lb0 : la0;
                    const uint32_t q1 = (g & 1) ? lb1 : la1;
                    const uint32_t a0 = q6k_pack(q0, qh0, nib_hi, g);
                    const uint32_t a1 = q6k_pack(q1, qh1, nib_hi, g);
                    const uint32_t a2 = q6k_pack(q0 >> 16, qh0 >> 16, nib_hi, g);
                    const uint32_t a3 = q6k_pack(q1 >> 16, qh1 >> 16, nib_hi, g);

                    const int idx = 2 * g + t;
                    const uint32_t sw0 = idx < 4 ? s0a : s0b;
                    const uint32_t sw1 = idx < 4 ? s1a : s1b;
                    const int sh = (idx & 3) * 8;
                    const float f0 = d0 * (float)(int8_t)(sw0 >> sh);
                    const float f1 = d1 * (float)(int8_t)(sw1 >> sh);
                    const float k0 = 64.0f * f0, off0 = 96.0f * f0;
                    const float k1 = 64.0f * f1, off1 = 96.0f * f1;
                    const int ek = (b << 8) + h * 128 + g * 32 + l;

#pragma unroll
                    for (int xt = 0; xt < Q6K_XTILES; xt++) {
                        if (xt >= xtiles) {
                            continue;
                        }
                        const int bat = xt * 8 + gr;
                        uint32_t b0 = 0, b1 = 0;
                        if (bat < B) {
                            const uint64_t xv = *reinterpret_cast<const uint64_t *>(
                                x + (size_t)bat * K + ek);
                            b0 = (uint32_t)xv;
                            b1 = (uint32_t)(xv >> 32);
                        }
                        __syncwarp();
                        float c0 = 0.f, c1 = 0.f, c2 = 0.f, c3 = 0.f;
                        slm_mma(c0, c1, c2, c3, a0, a1, a2, a3, b0, b1);
                        const float s_0 = __shfl_sync(0xFFFFFFFFu, c2, 28 + c);
                        const float s_1 = __shfl_sync(0xFFFFFFFFu, c3, 28 + c);
                        acc[xt][0] += k0 * c0 - off0 * s_0;
                        acc[xt][1] += k0 * c1 - off0 * s_1;
                        acc[xt][2] += k1 * c2 - off1 * s_0;
                        acc[xt][3] += k1 * c3 - off1 * s_1;
                    }
                }
            }
        }
    }

    const float bias0 = (bias && has0) ? bias[r0] : 0.0f;
    const float bias1 = (bias && has1) ? bias[r1] : 0.0f;
#pragma unroll
    for (int xt = 0; xt < Q6K_XTILES; xt++) {
        if (xt >= xtiles) {
            continue;
        }
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
