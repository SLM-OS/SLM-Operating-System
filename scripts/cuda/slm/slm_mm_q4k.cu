/*
 * slm_mm_q4k.cu - out[B,N] = x[B,K] @ W[N,K]^T with on-the-fly Q4_K decode.
 *
 * x     FP16, row-major [B][K]. K is a multiple of 256.
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
 */
#include "slm_common.cuh"

#define Q4K_BLOCK_BYTES 144
#ifndef Q4K_TILE_ROWS
#define Q4K_TILE_ROWS 15
#endif

/* Pack the two selected nibbles of a 16-bit load as FP16 1 + n/16.
 * hi selects the high nibble of each byte. */
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
    const uint32_t *p = reinterpret_cast<const uint32_t *>(blk);
    const uint32_t w0 = p[0];
    /* Invalid row (past N, or the S row): scale everything to zero. */
    h.d = valid ? slm_h2f(w0 & 0xFFFFu) : 0.0f;
    h.dmin = valid ? slm_h2f(w0 >> 16) : 0.0f;
    h.w1 = p[1];
    h.w2 = p[2];
    h.w3 = p[3];
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
    const bool xact = g < B;
    const char *xp = reinterpret_cast<const char *>(x + (size_t)g * K);

    const int e0 = 2 * c;
    const int e1 = 2 * c + 8;

    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    __syncwarp();

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const q4k_hdr h0 = q4k_load_hdr(blk0, has0);
        const q4k_hdr h1 = q4k_load_hdr(blk1, has1);

        for (int j = 0; j < 4; j++) {
            const uint8_t *qs0 = blk0 + 16 + (j << 5);
            const uint8_t *qs1 = blk1 + 16 + (j << 5);
            const int ebase = (b << 8) + (j << 6);
            float cl0 = 0.0f, cl1 = 0.0f, cl2 = 0.0f, cl3 = 0.0f;
            float ch0 = 0.0f, ch1 = 0.0f, ch2 = 0.0f, ch3 = 0.0f;

            for (int t = 0; t < 16; t += 8) {
                /* Each pass handles elements t*2..t*2+15 of the low and of the
                 * high nibble sub-block; both come from the same qs bytes. */
                const int q = t * 2;
                const uint32_t u00 =
                    *reinterpret_cast<const uint16_t *>(qs0 + q + e0);
                const uint32_t u01 =
                    *reinterpret_cast<const uint16_t *>(qs0 + q + e1);
                const uint32_t u10 =
                    *reinterpret_cast<const uint16_t *>(qs1 + q + e0);
                const uint32_t u11 =
                    *reinterpret_cast<const uint16_t *>(qs1 + q + e1);

                uint32_t bl0 = 0, bl1 = 0, bh0 = 0, bh1 = 0;
                if (xact) {
                    const int k = ebase + q;
                    const uint32_t *xl = reinterpret_cast<const uint32_t *>(
                        xp + 2 * (k + e0));
                    const uint32_t *xl2 = reinterpret_cast<const uint32_t *>(
                        xp + 2 * (k + e1));
                    bl0 = *xl;
                    bl1 = *xl2;
                    bh0 = xl[16];
                    bh1 = xl2[16];
                }
                __syncwarp();

                const uint32_t a0l = q4k_pack_lo(u00);
                const uint32_t a2l = q4k_pack_lo(u01);
                const uint32_t a0h = q4k_pack_hi(u00);
                const uint32_t a2h = q4k_pack_hi(u01);
                const uint32_t a1l = g == 7 ? SLM_H_ONE2 : q4k_pack_lo(u10);
                const uint32_t a3l = g == 7 ? SLM_H_ONE2 : q4k_pack_lo(u11);
                const uint32_t a1h = g == 7 ? SLM_H_ONE2 : q4k_pack_hi(u10);
                const uint32_t a3h = g == 7 ? SLM_H_ONE2 : q4k_pack_hi(u11);

                slm_mma(cl0, cl1, cl2, cl3, a0l, a1l, a2l, a3l, bl0, bl1);
                slm_mma(ch0, ch1, ch2, ch3, a0h, a1h, a2h, a3h, bh0, bh1);
            }

            /* S for columns 2c and 2c+1 lives in group 7's row-15 registers. */
            const float sl0 = __shfl_sync(0xFFFFFFFFu, cl2, 28 + c);
            const float sl1 = __shfl_sync(0xFFFFFFFFu, cl3, 28 + c);
            const float sh0 = __shfl_sync(0xFFFFFFFFu, ch2, 28 + c);
            const float sh1 = __shfl_sync(0xFFFFFFFFu, ch3, 28 + c);

            float k, off;
            q4k_factors(h0, 2 * j, k, off);
            acc0 = fmaf(k, cl0, acc0) - off * sl0;
            acc1 = fmaf(k, cl1, acc1) - off * sl1;
            q4k_factors(h1, 2 * j, k, off);
            acc2 = fmaf(k, cl2, acc2) - off * sl0;
            acc3 = fmaf(k, cl3, acc3) - off * sl1;
            q4k_factors(h0, 2 * j + 1, k, off);
            acc0 = fmaf(k, ch0, acc0) - off * sh0;
            acc1 = fmaf(k, ch1, acc1) - off * sh1;
            q4k_factors(h1, 2 * j + 1, k, off);
            acc2 = fmaf(k, ch2, acc2) - off * sh0;
            acc3 = fmaf(k, ch3, acc3) - off * sh1;
        }
    }

    /* Columns 2c and 2c+1 are batches 2c and 2c+1. */
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
    const int e0 = 2 * c;
    const int e1 = 2 * c + 8;
    const int xtiles = (B + 7) >> 3;

    /* acc[xt][0..3]: same mapping as the b8 kernel, one set per x tile. Kept
     * in shared memory: registers would cost 32 per lane and a stack if the
     * compiler spilled them, and SLM-OS launches these without a stack. */
    __shared__ __align__(16) float smem_acc[Q4K_XTILES * 16 * 8];
    /* Row r, batch-in-tile n of tile xt: smem_acc[xt*128 + r*8 + n]. */
    for (int xt = 0; xt < xtiles; xt++) {
        smem_acc[xt * 128 + g * 8 + 2 * c] = 0.0f;
        smem_acc[xt * 128 + g * 8 + 2 * c + 1] = 0.0f;
        smem_acc[xt * 128 + (g + 8) * 8 + 2 * c] = 0.0f;
        smem_acc[xt * 128 + (g + 8) * 8 + 2 * c + 1] = 0.0f;
    }
    __syncwarp();

    for (int b = 0; b < nb; b++) {
        const uint8_t *blk0 = w0 + b * Q4K_BLOCK_BYTES;
        const uint8_t *blk1 = w1 + b * Q4K_BLOCK_BYTES;
        const q4k_hdr h0 = q4k_load_hdr(blk0, has0);
        const q4k_hdr h1 = q4k_load_hdr(blk1, has1);

        for (int j = 0; j < 4; j++) {
            const uint8_t *qs0 = blk0 + 16 + (j << 5);
            const uint8_t *qs1 = blk1 + 16 + (j << 5);
            const int ebase = (b << 8) + (j << 6);

            /* Load both 16-byte chunks of the 32 qs bytes once. */
            const uint32_t ua00 =
                *reinterpret_cast<const uint16_t *>(qs0 + e0);
            const uint32_t ua01 =
                *reinterpret_cast<const uint16_t *>(qs0 + e1);
            const uint32_t ua10 =
                *reinterpret_cast<const uint16_t *>(qs1 + e0);
            const uint32_t ua11 =
                *reinterpret_cast<const uint16_t *>(qs1 + e1);
            const uint32_t ub00 =
                *reinterpret_cast<const uint16_t *>(qs0 + 16 + e0);
            const uint32_t ub01 =
                *reinterpret_cast<const uint16_t *>(qs0 + 16 + e1);
            const uint32_t ub10 =
                *reinterpret_cast<const uint16_t *>(qs1 + 16 + e0);
            const uint32_t ub11 =
                *reinterpret_cast<const uint16_t *>(qs1 + 16 + e1);

            float kl0, offl0, kl1, offl1, kh0, offh0, kh1, offh1;
            q4k_factors(h0, 2 * j, kl0, offl0);
            q4k_factors(h1, 2 * j, kl1, offl1);
            q4k_factors(h0, 2 * j + 1, kh0, offh0);
            q4k_factors(h1, 2 * j + 1, kh1, offh1);

            for (int hi = 0; hi < 2; hi++) {
                const uint32_t a0a = hi ? q4k_pack_hi(ua00) : q4k_pack_lo(ua00);
                const uint32_t a2a = hi ? q4k_pack_hi(ua01) : q4k_pack_lo(ua01);
                const uint32_t a0b = hi ? q4k_pack_hi(ub00) : q4k_pack_lo(ub00);
                const uint32_t a2b = hi ? q4k_pack_hi(ub01) : q4k_pack_lo(ub01);
                uint32_t a1a = hi ? q4k_pack_hi(ua10) : q4k_pack_lo(ua10);
                uint32_t a3a = hi ? q4k_pack_hi(ua11) : q4k_pack_lo(ua11);
                uint32_t a1b = hi ? q4k_pack_hi(ub10) : q4k_pack_lo(ub10);
                uint32_t a3b = hi ? q4k_pack_hi(ub11) : q4k_pack_lo(ub11);
                if (g == 7) {
                    a1a = a3a = a1b = a3b = SLM_H_ONE2;
                }
                const float k0 = hi ? kh0 : kl0;
                const float off0 = hi ? offh0 : offl0;
                const float k1 = hi ? kh1 : kl1;
                const float off1 = hi ? offh1 : offl1;
                const int ek = ebase + (hi << 5);

                for (int xt = 0; xt < xtiles; xt++) {
                    const int bat = xt * 8 + g;
                    uint32_t ba0 = 0, ba1 = 0, bb0 = 0, bb1 = 0;
                    if (bat < B) {
                        const char *xp = reinterpret_cast<const char *>(
                            x + (size_t)bat * K + ek);
                        ba0 = *reinterpret_cast<const uint32_t *>(
                            xp + 2 * e0);
                        ba1 = *reinterpret_cast<const uint32_t *>(
                            xp + 2 * e1);
                        bb0 = *reinterpret_cast<const uint32_t *>(
                            xp + 32 + 2 * e0);
                        bb1 = *reinterpret_cast<const uint32_t *>(
                            xp + 32 + 2 * e1);
                    }
                    __syncwarp();
                    float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f, c3 = 0.0f;
                    slm_mma(c0, c1, c2, c3, a0a, a1a, a2a, a3a, ba0, ba1);
                    slm_mma(c0, c1, c2, c3, a0b, a1b, a2b, a3b, bb0, bb1);

                    const float s0 = __shfl_sync(0xFFFFFFFFu, c2, 28 + c);
                    const float s1 = __shfl_sync(0xFFFFFFFFu, c3, 28 + c);

                    float *row0 = &smem_acc[xt * 128 + g * 8 + 2 * c];
                    float *row1 = &smem_acc[xt * 128 + (g + 8) * 8 + 2 * c];
                    row0[0] += k0 * c0 - off0 * s0;
                    row0[1] += k0 * c1 - off0 * s1;
                    row1[0] += k1 * c2 - off1 * s0;
                    row1[1] += k1 * c3 - off1 * s1;
                }
            }
        }
    }

    const float bias0 = (bias && has0) ? bias[r0] : 0.0f;
    const float bias1 = (bias && has1) ? bias[r1] : 0.0f;
    for (int xt = 0; xt < xtiles; xt++) {
        const int n0 = xt * 8 + 2 * c;
        const int n1 = n0 + 1;
        const float *row0 = &smem_acc[xt * 128 + g * 8 + 2 * c];
        const float *row1 = &smem_acc[xt * 128 + (g + 8) * 8 + 2 * c];
        if (has0 && n0 < B) {
            slm_store_out(out, (long)n0 * N + r0, row0[0] + bias0, flags);
        }
        if (has0 && n1 < B) {
            slm_store_out(out, (long)n1 * N + r0, row0[1] + bias0, flags);
        }
        if (has1 && n0 < B) {
            slm_store_out(out, (long)n0 * N + r1, row1[0] + bias1, flags);
        }
        if (has1 && n1 < B) {
            slm_store_out(out, (long)n1 * N + r1, row1[1] + bias1, flags);
        }
    }
}
