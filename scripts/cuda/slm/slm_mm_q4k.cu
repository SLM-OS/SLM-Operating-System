/*
 * slm_mm_q4k.cu - out[B,N] = x[B,K] @ W[N,K]^T with on-the-fly Q4_K decode.
 *
 * x     FP16, row-major [B][K]. K is a multiple of 256. 16-byte aligned.
 * W     Q4_K, row-major [N][K/256 blocks of 144 bytes]. 16-byte aligned.
 * bias  FP32 [N] or null; added to every output row.
 * out   row-major [B][N], FP16 when SLM_MM_OUT_F16 is set, else FP32.
 *
 * cbuf[0] ABI, shared by all three kernels (sm_87 params start at 0x160):
 *   0x160 x      0x168 W      0x170 out    0x178 bias
 *   0x180 K      0x184 N      0x188 B      0x18C flags
 *
 * Launch shapes, block (32,1,1) for all:
 *   slm_mm_q4k_simt     grid (N, B, 1)        B any
 *   slm_mm_q4k_hmma_b8  grid (ceil(N/15),1,1) B in 1..8
 *   slm_mm_q4k_hmma_b64 grid (ceil(N/15),1,1) B in 1..64
 *
 * Offset encoding (HMMA kernels). Scaling a nibble n to FP16 per element
 * would cost several instructions per element, which is most of the kernel.
 * Instead each nibble is written straight into the mantissa of FP16 1.0:
 *
 *   h = 0x3C00 | (n << 6), so h = 1 + n/16 exactly, with one mask and shift.
 *
 * Q4_K defines w = d*sc*n - dmin*mn, with sc and mn per 32-element sub-block.
 * Over one sub-block, with S = sum(x) and H = sum(h * x):
 *
 *   sum(w * x) = 16*d*sc*(H - S) - dmin*mn*S
 *
 * A tile therefore carries 15 weight rows and one row of 1.0, whose
 * accumulator is S. The tensor core produces H and S; one FMA per output per
 * sub-block applies the scales. The tile holds 15 rows, not 16, hence the
 * ceil(N/15) grid. Decoded elements stay exact: FP16 rounding of a scaled
 * weight never happens.
 *
 * Column permutation (HMMA kernels). A warp is limited by its load round
 * trips: a warp stepping through fresh weight bytes pays about one DRAM trip
 * per load. mma columns may be in any order provided A and B agree, so instead
 * of row-major column order, lane c of a pass over 32 qs bytes takes bytes
 * 8c..8c+7 of each of its two rows with one 8-byte load, and the x elements
 * for those bytes with one 16-byte load. The first four bytes form one mma, the
 * next four another. That is two W loads and two x loads per four mma.
 */
#include <cmath>

#include "slm_common.cuh"

#define Q4K_BLOCK_BYTES 144
#ifndef Q4K_TILE_ROWS
#define Q4K_TILE_ROWS 15
#endif

/* Pack the two selected nibbles of the low 16 bits as FP16 1 + n/16. */
SLM_INL uint32_t q4k_pack_lo(uint32_t u)
{
    return SLM_H_ONE2 | ((u & 0x000Fu) << 6) | ((u & 0x0F00u) << 14);
}

SLM_INL uint32_t q4k_pack_hi(uint32_t u)
{
    return SLM_H_ONE2 | ((u & 0x00F0u) << 2) | ((u & 0xF000u) << 10);
}

/* Scale and min for sub-block is of a block with scale words w1,w2,w3
 * (bytes 4..7, 8..11, 12..15 of the block). Mirrors ggml's 6-bit packing. */
SLM_INL void q4k_scale_min(uint32_t w1, uint32_t w2, uint32_t w3, int is,
                           uint32_t &sc, uint32_t &mn)
{
    if (is < 4) {
        const int s = is * 8;
        sc = (w1 >> s) & 0x3Fu;
        mn = (w2 >> s) & 0x3Fu;
    } else {
        const int s = (is - 4) * 8;
        sc = ((w3 >> s) & 0x0Fu) | (((w1 >> s) >> 2) & 0x30u);
        mn = ((w3 >> (s + 4)) & 0x0Fu) | (((w2 >> s) >> 2) & 0x30u);
    }
}

/*
 * Per-lane per-row state: the block header, and the two scale factors for the
 * current sub-block, kept as (k = 16*d*sc, off = k + dmin*mn), so that
 *   acc += k * H - off * S
 */
struct q4k_hdr {
    float d, dmin;
    uint32_t w1, w2, w3;
};

SLM_INL q4k_hdr q4k_load_hdr(const uint8_t *blk, bool valid)
{
    q4k_hdr h;
    const slm_x16 v = *reinterpret_cast<const slm_x16 *>(blk);
    /* Invalid row (past N, or the S row): scale everything to zero. */
    h.d = valid ? slm_h2f(v.w0 & 0xFFFFu) : 0.0f;
    h.dmin = valid ? slm_h2f(v.w0 >> 16) : 0.0f;
    h.w1 = v.w1;
    h.w2 = v.w2;
    h.w3 = v.w3;
    return h;
}

SLM_INL void q4k_factors(const q4k_hdr &h, int is, float &k, float &off)
{
    uint32_t sc, mn;
    q4k_scale_min(h.w1, h.w2, h.w3, is, sc, mn);
    k = 16.0f * h.d * (float)sc;
    off = k + h.dmin * (float)mn;
}

/* ---------------------------------------------------------------------- */
/* SIMT reference (plan decision M6-D4): one row and one batch per warp.  */
/* ---------------------------------------------------------------------- */

__global__ void slm_mm_q4k_simt(const half *x, const uint8_t *W, void *out,
                                const float *bias, int K, int N, int B,
                                int flags)
{
    const int row = blockIdx.x;
    const int bat = blockIdx.y;
    const int l = threadIdx.x;
    const int nb = K >> 8;
    const uint8_t *wrow = W + (size_t)row * nb * Q4K_BLOCK_BYTES;
    const half *xrow = x + (size_t)bat * K;

    float acc = 0.0f;
    for (int b = 0; b < nb; b++) {
        const uint8_t *blk = wrow + b * Q4K_BLOCK_BYTES;
        const q4k_hdr h = q4k_load_hdr(blk, true);
        const half *xb = xrow + (b << 8);
        for (int j = 0; j < 4; j++) {
            const uint32_t q = blk[16 + (j << 5) + l];
            uint32_t sc, mn;
            q4k_scale_min(h.w1, h.w2, h.w3, 2 * j, sc, mn);
            const float w0 = h.d * (float)sc * (float)(q & 0xFu) -
                             h.dmin * (float)mn;
            q4k_scale_min(h.w1, h.w2, h.w3, 2 * j + 1, sc, mn);
            const float w1 = h.d * (float)sc * (float)(q >> 4) -
                             h.dmin * (float)mn;
            const int e = (j << 6) + l;
            acc += w0 * __half2float(xb[e]);
            acc += w1 * __half2float(xb[e + 32]);
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
/* HMMA, B in 1..8: decode workhorse, accumulators in registers.          */
/* ---------------------------------------------------------------------- */

__global__ void slm_mm_q4k_hmma_b8(const half *x, const uint8_t *W, void *out,
                                   const float *bias, int K, int N, int B,
                                   int flags)
{
    const int lane = threadIdx.x;
    const int g = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q4K_BLOCK_BYTES;

    const int r0 = blockIdx.x * Q4K_TILE_ROWS + g;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    /* Group 7's second row is the row of 1.0 that produces S. */
    const bool has1 = g < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    /* First batch of this CTA's x tile. */
    const int base = (flags & SLM_MM_SPLIT_XT) ? (int)blockIdx.y * 8 : 0;
    const bool xact = base + g < B;
    const size_t xs = slm_xstride_rt(K, flags);
    const half *xrow = x + (size_t)(base + g) * xs;
    /* q8_1 scale rows of this lane's two output batches. */
    const half *xr0 = x + (size_t)min(base + 2 * c, B - 1) * xs;
    const half *xr1 = x + (size_t)min(base + 2 * c + 1, B - 1) * xs;
    /* Row 15's nibbles read as zero, which encodes exactly 1.0. */
    const uint32_t keep1 = g == 7 ? 0u : 0xFFFFFFFFu;
    const int off8 = 8 * c;

    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    __syncwarp();

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const q4k_hdr h0 = q4k_load_hdr(blk0, has0);
        const q4k_hdr h1 = q4k_load_hdr(blk1, has1);

        for (int j = 0; j < 4; j++) {
            const uint64_t d0 = *reinterpret_cast<const uint64_t *>(
                blk0 + 16 + (j << 5) + off8);
            const uint64_t d1 = *reinterpret_cast<const uint64_t *>(
                blk1 + 16 + (j << 5) + off8);
            const uint32_t u0a = (uint32_t)d0, u0b = (uint32_t)(d0 >> 32);
            const uint32_t u1a = (uint32_t)d1 & keep1;
            const uint32_t u1b = (uint32_t)(d1 >> 32) & keep1;

            slm_x16 xl = {0, 0, 0, 0}, xh = {0, 0, 0, 0};
            if (xact) {
                const half *xe = xrow + (b << 8) + (j << 6) + off8;
                xl = *reinterpret_cast<const slm_x16 *>(xe);
                xh = *reinterpret_cast<const slm_x16 *>(xe + 32);
            }
            __syncwarp();

            float cl0 = 0.f, cl1 = 0.f, cl2 = 0.f, cl3 = 0.f;
            float ch0 = 0.f, ch1 = 0.f, ch2 = 0.f, ch3 = 0.f;
            slm_mma(cl0, cl1, cl2, cl3, q4k_pack_lo(u0a), q4k_pack_lo(u1a),
                    q4k_pack_lo(u0a >> 16), q4k_pack_lo(u1a >> 16), xl.w0, xl.w1);
            slm_mma(cl0, cl1, cl2, cl3, q4k_pack_lo(u0b), q4k_pack_lo(u1b),
                    q4k_pack_lo(u0b >> 16), q4k_pack_lo(u1b >> 16), xl.w2, xl.w3);
            slm_mma(ch0, ch1, ch2, ch3, q4k_pack_hi(u0a), q4k_pack_hi(u1a),
                    q4k_pack_hi(u0a >> 16), q4k_pack_hi(u1a >> 16), xh.w0, xh.w1);
            slm_mma(ch0, ch1, ch2, ch3, q4k_pack_hi(u0b), q4k_pack_hi(u1b),
                    q4k_pack_hi(u0b >> 16), q4k_pack_hi(u1b >> 16), xh.w2, xh.w3);

            /* S for columns 2c and 2c+1 lives in group 7's row-15 registers. */
            const float sl0 = __shfl_sync(0xFFFFFFFFu, cl2, 28 + c);
            const float sl1 = __shfl_sync(0xFFFFFFFFu, cl3, 28 + c);
            const float sh0 = __shfl_sync(0xFFFFFFFFu, ch2, 28 + c);
            const float sh1 = __shfl_sync(0xFFFFFFFFu, ch3, 28 + c);

            const int bl = (b << 3) + 2 * j;
            const float dl0 = slm_xq_d_rt(xr0, K, flags, bl), dl1 = slm_xq_d_rt(xr1, K, flags, bl);
            const float dh0 = slm_xq_d_rt(xr0, K, flags, bl + 1), dh1 = slm_xq_d_rt(xr1, K, flags, bl + 1);
            float k, off;
            q4k_factors(h0, 2 * j, k, off);
            acc0 += dl0 * (k * cl0 - off * sl0);
            acc1 += dl1 * (k * cl1 - off * sl1);
            q4k_factors(h1, 2 * j, k, off);
            acc2 += dl0 * (k * cl2 - off * sl0);
            acc3 += dl1 * (k * cl3 - off * sl1);
            q4k_factors(h0, 2 * j + 1, k, off);
            acc0 += dh0 * (k * ch0 - off * sh0);
            acc1 += dh1 * (k * ch1 - off * sh1);
            q4k_factors(h1, 2 * j + 1, k, off);
            acc2 += dh0 * (k * ch2 - off * sh0);
            acc3 += dh1 * (k * ch3 - off * sh1);
        }
    }

    /* Columns 2c and 2c+1 are batches 2c and 2c+1 of this x tile. */
    const int b0 = base + 2 * c;
    const int b1 = base + 2 * c + 1;
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
/* HMMA, B in 1..64: eight x tiles per weight tile, one weight decode.    */
/* ---------------------------------------------------------------------- */

#define Q4K_XTILES 8

__global__ void slm_mm_q4k_hmma_b64(const half *x, const uint8_t *W, void *out,
                                    const float *bias, int K, int N, int B,
                                    int flags)
{
    const int lane = threadIdx.x;
    const int g = lane >> 2;
    const int c = lane & 3;
    const int nb = K >> 8;
    const size_t row_bytes = (size_t)nb * Q4K_BLOCK_BYTES;

    const int r0 = blockIdx.x * Q4K_TILE_ROWS + g;
    const int r1 = r0 + 8;
    const bool has0 = r0 < N;
    const bool has1 = g < 7 && r1 < N;
    const uint8_t *w0 = W + (has0 ? r0 : 0) * row_bytes;
    const uint8_t *w1 = W + (has1 ? r1 : 0) * row_bytes;
    const uint32_t keep1 = g == 7 ? 0u : 0xFFFFFFFFu;
    const int off8 = 8 * c;
    const int xtiles = (B + 7) >> 3;
    const bool running = flags & SLM_MM_BIAS_RUNNING;
    const int only = (flags & SLM_MM_SPLIT_XT) ? (int)blockIdx.y : -1;
    const int bstart = SLM_MM_BSTART(flags);
    const int bend = SLM_MM_BCOUNT(flags) ? bstart + SLM_MM_BCOUNT(flags) : nb;

    /* acc[xt][0..3], same mapping as the b8 kernel, one set per x tile. In
     * registers: the unrolled constant-bound loops below leave no variable
     * index, so this needs no stack. oplib-extract.py refuses the kernel if
     * that ever stops being true. */
    float acc[Q4K_XTILES][4];
#pragma unroll
    for (int xt = 0; xt < Q4K_XTILES; xt++) {
#pragma unroll
        for (int i = 0; i < 4; i++) {
            acc[xt][i] = 0.0f;
        }
    }
    __syncwarp();

    for (int b = bstart; b < bend; b++) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const q4k_hdr h0 = q4k_load_hdr(blk0, has0);
        const q4k_hdr h1 = q4k_load_hdr(blk1, has1);

        for (int j = 0; j < 4; j++) {
            const uint64_t d0 = *reinterpret_cast<const uint64_t *>(
                blk0 + 16 + (j << 5) + off8);
            const uint64_t d1 = *reinterpret_cast<const uint64_t *>(
                blk1 + 16 + (j << 5) + off8);
            const uint32_t u0a = (uint32_t)d0, u0b = (uint32_t)(d0 >> 32);
            const uint32_t u1a = (uint32_t)d1 & keep1;
            const uint32_t u1b = (uint32_t)(d1 >> 32) & keep1;

            float kl0, offl0, kl1, offl1, kh0, offh0, kh1, offh1;
            q4k_factors(h0, 2 * j, kl0, offl0);
            q4k_factors(h1, 2 * j, kl1, offl1);
            q4k_factors(h0, 2 * j + 1, kh0, offh0);
            q4k_factors(h1, 2 * j + 1, kh1, offh1);

#pragma unroll
            for (int hi = 0; hi < 2; hi++) {
                const uint32_t a0a = hi ? q4k_pack_hi(u0a) : q4k_pack_lo(u0a);
                const uint32_t a1a = hi ? q4k_pack_hi(u1a) : q4k_pack_lo(u1a);
                const uint32_t a2a = hi ? q4k_pack_hi(u0a >> 16) : q4k_pack_lo(u0a >> 16);
                const uint32_t a3a = hi ? q4k_pack_hi(u1a >> 16) : q4k_pack_lo(u1a >> 16);
                const uint32_t a0b = hi ? q4k_pack_hi(u0b) : q4k_pack_lo(u0b);
                const uint32_t a1b = hi ? q4k_pack_hi(u1b) : q4k_pack_lo(u1b);
                const uint32_t a2b = hi ? q4k_pack_hi(u0b >> 16) : q4k_pack_lo(u0b >> 16);
                const uint32_t a3b = hi ? q4k_pack_hi(u1b >> 16) : q4k_pack_lo(u1b >> 16);
                const float k0 = hi ? kh0 : kl0;
                const float off0 = hi ? offh0 : offl0;
                const float k1 = hi ? kh1 : kl1;
                const float off1 = hi ? offh1 : offl1;
                const int ek = (b << 8) + (j << 6) + (hi << 5) + off8;

#pragma unroll
                for (int xt = 0; xt < Q4K_XTILES; xt++) {
                    if (xt >= xtiles || (only >= 0 && xt != only)) {
                        continue;
                    }
                    const int bat = xt * 8 + g;
                    slm_x16 xv = {0, 0, 0, 0};
                    if (bat < B) {
                        xv = *reinterpret_cast<const slm_x16 *>(x + (size_t)bat * K + ek);
                    }
                    __syncwarp();
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
        }
    }

    const float bias0 = (!running && bias && has0) ? bias[r0] : 0.0f;
    const float bias1 = (!running && bias && has1) ? bias[r1] : 0.0f;
#pragma unroll
    for (int xt = 0; xt < Q4K_XTILES; xt++) {
        if (xt >= xtiles || (only >= 0 && xt != only)) {
            continue;
        }
        const int n0 = xt * 8 + 2 * c;
        const int n1 = n0 + 1;
        if (running) {
            if (has0 && n0 < B) {
                acc[xt][0] += bias[(long)n0 * N + r0];
            }
            if (has0 && n1 < B) {
                acc[xt][1] += bias[(long)n1 * N + r0];
            }
            if (has1 && n0 < B) {
                acc[xt][2] += bias[(long)n0 * N + r1];
            }
            if (has1 && n1 < B) {
                acc[xt][3] += bias[(long)n1 * N + r1];
            }
        }
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
