/*
 * slm_gqa_hmma.cu - grouped-query attention with Q.K^T and P.V on HMMA (#1167).
 *
 * kernel/gpu/slm_prog.c selects these for GQA when hd is 128 and nhq is a
 * multiple of nhkv, slm_gqa_w8 otherwise (the tiny test model has hd 32).
 * Measured against slm_gqa_w8 on jetson-nano-2 (scripts/cuda/harness/
 * gqa_bench.cu): decode 14.2 against 32.6 us at kv 64 and 53.8 against 619 us
 * at kv 2048; 64-row prefill 24.3 against 226 us at kv 64.
 *
 * Contract, the same as slm_gqa_w8: q and out are FP16 [B][nhq][hd]; k and v
 * are FP16 [T][nhkv][hd]; query row b sees positions 0..=pos[b] (the causal
 * mask); softmax(q.k / sqrt(hd)) . v with FP32 accumulators. Here hd must be
 * 128 (GQ_HD) and nhq a multiple of nhkv. KV slots at or past the largest
 * pos[b] + 1 are never read, so they may hold anything.
 *
 * Both kernels put the group of query heads that share one KV head on the
 * M side of mma.m16n8k16: M row m = b * group + j is query head
 * hkv * group + j of batch row b. One K or V tile then serves every query
 * head of the group, which slm_gqa_w8 reads group times. Scores S = Q.K^T
 * take 16 mma per 16 positions; P = exp(S - max) is rounded to FP16 and is
 * the A operand of P.V directly, because the accumulator layout of S matches
 * the A layout of the next mma (rows g and g+8, columns 2c and 2c+1).
 *
 *   slm_gqa_hmma_dec  B * group <= 16 (decode; Qwen2.5-1.5B: B <= 2).
 *       grid (nhkv, 128 / 16, 1), block (32, 8, 1). The warps of a CTA take
 *       interleaved 16-position tiles, each with its own online softmax, and
 *       merge in shared memory as slm_gqa_w8 does. A CTA writes 16 of the 128
 *       output dims; all eight CTAs of a KV head compute the same scores, so
 *       K is read eight times (from the L2) and V once, with no cross-CTA merge
 *       and no workspace.
 *   slm_gqa_hmma_pf   any B (prefill). grid (nhkv, ceil(ceil(B*group/16)/4), 1),
 *       block (32, 4, 1). Each warp owns one 16-row M tile and runs every
 *       position up to its rows' limit, as FlashAttention-2 does; the warps of
 *       a CTA share K and V tiles of 16 positions in shared memory, double
 *       buffered with cp.async.
 *
 * Dot products need no fixed order of dims, only the same one for Q and K, so
 * a lane's eight consecutive dims 32jj + 8c .. +7 of a row (one 16-byte load)
 * feed two k-steps: halves 0-1 and 2-3 are its k = 2c, 2c+1 and 2c+8, 2c+9 of
 * k-step 2jj, halves 4-7 the same of k-step 2jj+1.
 *
 * Host emulation: with SLM_GQA_EMU defined, scripts/cuda/harness/gqa_emu.h
 * supplies the GQ_* primitives and the bodies run lane by lane on the CPU
 * (gqa_emu_test.cpp). Without it, the primitives below are the device ones.
 */
#ifndef SLM_GQA_HMMA_CU
#define SLM_GQA_HMMA_CU

#include <cstdint>

#ifdef SLM_GQA_EMU
/* gqa_emu.h, included first by the emulator, defines GQ_FN, gq_u4 and the gq_*
 * primitives. */
#else
#include <cuda_fp16.h>

#define GQ_FN __device__ __forceinline__

typedef uint4 gq_u4;

/* C[16,8] += A[16,16] @ B[16,8], FP16 in, FP32 accumulators. Layout as
 * slm_mma in slm_common.cuh. */
GQ_FN void gq_mma(float (&c)[4], uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t b0,
                  uint32_t b1)
{
    __asm__ volatile("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 "
                     "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
                     : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3])
                     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
}

/* Four transposed 8x8 FP16 matrices from shared memory. Lanes 8i..8i+7 give
 * the row addresses of matrix i; lane t gets, in r[i], rows 2(t%4) and
 * 2(t%4)+1 of column t/4 of matrix i. */
GQ_FN void gq_ldsm_x4_t(uint32_t (&r)[4], const void *row)
{
    const uint32_t a = (uint32_t)__cvta_generic_to_shared(row);
    __asm__ volatile("ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%0,%1,%2,%3}, [%4];\n"
                     : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3])
                     : "r"(a));
}

/* 16 bytes global -> shared without registers; zero-filled when !ok, and then
 * the (valid) source is not read. */
GQ_FN void gq_cp16(void *dst, const void *src, bool ok)
{
    const uint32_t d = (uint32_t)__cvta_generic_to_shared(dst);
    __asm__ volatile("cp.async.cg.shared.global [%0], [%1], 16, %2;\n" ::"r"(d), "l"(src),
                     "r"(ok ? 16 : 0));
}
GQ_FN void gq_cp_commit() { __asm__ volatile("cp.async.commit_group;\n" ::); }
GQ_FN void gq_cp_wait1() { __asm__ volatile("cp.async.wait_group 1;\n" ::); }
GQ_FN void gq_cp_wait0() { __asm__ volatile("cp.async.wait_group 0;\n" ::); }

GQ_FN float gq_ex2(float x)
{
    float y;
    __asm__("ex2.approx.ftz.f32 %0, %1;\n" : "=f"(y) : "f"(x));
    return y;
}
GQ_FN float gq_shfl_xor(float v, int m) { return __shfl_xor_sync(0xFFFFFFFFu, v, m); }
GQ_FN bool gq_any(bool p) { return __any_sync(0xFFFFFFFFu, p) != 0; }
GQ_FN void gq_syncwarp() { __syncwarp(); }
GQ_FN void gq_syncthreads() { __syncthreads(); }
GQ_FN gq_u4 gq_ld16(const void *p) { return *reinterpret_cast<const gq_u4 *>(p); }

/* Two floats to FP16x2 (round to nearest even), low half first. */
GQ_FN uint32_t gq_pack(float lo, float hi)
{
    const __half2 h = __floats2half2_rn(lo, hi);
    return *reinterpret_cast<const uint32_t *>(&h);
}
/* Sum of the two FP16 halves of x as FP32. */
GQ_FN float gq_hsum(uint32_t x)
{
    const float2 f = __half22float2(*reinterpret_cast<const __half2 *>(&x));
    return f.x + f.y;
}
GQ_FN uint32_t gq_f2h(float v) { return (uint32_t)__half_as_ushort(__float2half_rn(v)); }
GQ_FN float gq_h2f(uint32_t h) { return __half2float(__ushort_as_half((unsigned short)h)); }
#endif

#define GQ_HD 128          /* head dim the kernels are built for */
#define GQ_ROW_CHUNKS 16   /* 16-byte chunks per KV row of one head */
#define GQ_DEC_WARPS 8
#define GQ_DEC_DS 16       /* output dims per decode CTA */
#define GQ_PF_WARPS 4
#define GQ_PF_KT 16        /* positions per prefill K/V tile */
/* log2(e) / sqrt(128): scores go to exp2 directly. */
#define GQ_SCALE_LOG2 0.12751743074602467f

/* Shared memory row r of a KV tile keeps logical 16-byte chunk q at
 * q ^ gq_swz(r). Lanes that read one chunk of eight consecutive rows
 * (ldmatrix), or chunks 4jj..4jj+3 of rows 2i and 2i+1 (a 16-byte LDS phase),
 * then hit eight different bank groups. */
GQ_FN int gq_swz(int r) { return ((r & 1) << 2) | ((r >> 1) & 3); }

GQ_FN uint32_t gq_w(const gq_u4 &v, int i)
{
    return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w;
}

/* One 16-position block of online softmax for the two M rows a lane holds
 * (r = 0: row g, r = 1: row g + 8). s[u][e] are raw scores for positions
 * t0 + 8u + 2c + (e & 1) of row g + 8 * (e >> 1); lim[r] is the row's
 * position count (masked at and past it); full skips the mask. Returns the P
 * fragment as the A operand of P.V and the factor alpha[r] that rescales the
 * row's earlier O and l.
 *
 * PX: P in two FP16 parts, pa = fp16(p) and pl = fp16(p - pa), each the A
 * operand of its own P.V mma, so P carries about 22 bits as slm_gqa_w8's
 * FP32 P does. Without PX, P is FP16 alone (as FlashAttention and llama.cpp
 * use it): the output then errs by up to 2^-11 of |V|, which shows wherever
 * the weighted sum cancels to a small value. */
template <bool PX>
GQ_FN void gq_softmax_block(float (&s)[2][4], int t0, int c, const int (&lim)[2], bool full,
                            float (&mrow)[2], float (&lrow)[2], float (&alpha)[2],
                            uint32_t (&pa)[4], uint32_t (&pl)[4])
{
#pragma unroll
    for (int u = 0; u < 2; u++) {
#pragma unroll
        for (int e = 0; e < 4; e++) {
            const int t = t0 + 8 * u + 2 * c + (e & 1);
            const float x = s[u][e] * GQ_SCALE_LOG2;
            s[u][e] = (full || t < lim[e >> 1]) ? x : -INFINITY;
        }
    }
#pragma unroll
    for (int r = 0; r < 2; r++) {
        float mt = fmaxf(fmaxf(s[0][2 * r], s[0][2 * r + 1]), fmaxf(s[1][2 * r], s[1][2 * r + 1]));
        mt = fmaxf(mt, gq_shfl_xor(mt, 1));
        mt = fmaxf(mt, gq_shfl_xor(mt, 2));
        const float mn = fmaxf(mrow[r], mt);
        /* A row with nothing unmasked yet keeps max -inf; exp2 against 0
         * then gives p = 0 and alpha = 0 instead of NaN. */
        const float mu = mn == -INFINITY ? 0.0f : mn;
        alpha[r] = gq_ex2(mrow[r] - mu);
        mrow[r] = mn;
        const float e00 = gq_ex2(s[0][2 * r] - mu), e01 = gq_ex2(s[0][2 * r + 1] - mu);
        const float e10 = gq_ex2(s[1][2 * r] - mu), e11 = gq_ex2(s[1][2 * r + 1] - mu);
        const uint32_t p0 = gq_pack(e00, e01);
        const uint32_t p1 = gq_pack(e10, e11);
        /* The denominator sums the weights P.V really uses, so the output
         * stays a convex combination of V rows. */
        float add = gq_hsum(p0) + gq_hsum(p1);
        if (PX) {
            const uint32_t q0 = gq_pack(e00 - gq_h2f(p0 & 0xFFFFu), e01 - gq_h2f(p0 >> 16));
            const uint32_t q1 = gq_pack(e10 - gq_h2f(p1 & 0xFFFFu), e11 - gq_h2f(p1 >> 16));
            add += gq_hsum(q0) + gq_hsum(q1);
            pl[r] = q0;
            pl[2 + r] = q1;
        }
        lrow[r] = lrow[r] * alpha[r] + add;
        pa[r] = p0;     /* a0 (row g) or a1 (row g+8): positions t0 + 2c, +1 */
        pa[2 + r] = p1; /* a2 or a3: positions t0 + 8 + 2c, +1 */
    }
}

/* Q fragments of one M row: four 16-byte chunks, dims 32jj + 8c .. +7. */
GQ_FN void gq_load_q(gq_u4 (&qr)[4], const uint16_t *q, int m, int M, int group, int nhq, int hkv,
                     int c)
{
    if (m < M) {
        const int b = m / group;
        const uint16_t *row = q + ((size_t)b * nhq + (size_t)hkv * group + (m - b * group)) * GQ_HD;
#pragma unroll
        for (int jj = 0; jj < 4; jj++) {
            qr[jj] = gq_ld16(row + 32 * jj + 8 * c);
        }
    } else {
#pragma unroll
        for (int jj = 0; jj < 4; jj++) {
            qr[jj].x = qr[jj].y = qr[jj].z = qr[jj].w = 0u;
        }
    }
}

/* S[16 rows][16 positions] = Q.K^T. kr[u][jj] is the lane's chunk jj of the K
 * row at position t0 + 8u + g.
 *
 * Each k-step's mma starts from zero and the eight partial sums are added in
 * FP32. The tensor core adds into its accumulator by truncation, so feeding it
 * the running score let the error grow with every k-step; on jetson-nano-2
 * that put large-score outputs at up to 11 FP16 ULP from the exact result,
 * against slm_gqa_w8's 2.9 (#1167). */
GQ_FN void gq_scores(float (&s)[2][4], const gq_u4 (&qa)[4], const gq_u4 (&qb)[4],
                     const gq_u4 (&kr)[2][4])
{
#pragma unroll
    for (int u = 0; u < 2; u++) {
        s[u][0] = s[u][1] = s[u][2] = s[u][3] = 0.0f;
#pragma unroll
        for (int jj = 0; jj < 4; jj++) {
#pragma unroll
            for (int h = 0; h < 2; h++) {
                float t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                gq_mma(t, gq_w(qa[jj], 2 * h), gq_w(qb[jj], 2 * h), gq_w(qa[jj], 2 * h + 1),
                       gq_w(qb[jj], 2 * h + 1), gq_w(kr[u][jj], 2 * h), gq_w(kr[u][jj], 2 * h + 1));
#pragma unroll
                for (int e = 0; e < 4; e++) {
                    s[u][e] += t[e];
                }
            }
        }
    }
}

/* Position limit (pos + 1) of M row m; rows past M take dflt. */
GQ_FN int gq_row_lim(const uint32_t *pos, int m, int M, int group, int dflt)
{
    return m < M ? (int)pos[m / group] + 1 : dflt;
}

/* ---- decode ---- */

struct gq_dec_smem {
    /* Per warp: one V tile, 16 positions x GQ_DEC_DS dims, swizzled. */
    gq_u4 v[GQ_DEC_WARPS][16 * GQ_DEC_DS / 8];
    float o[GQ_DEC_WARPS][16][GQ_DEC_DS];
    float m[GQ_DEC_WARPS][16];
    float l[GQ_DEC_WARPS][16];
};

template <bool PX>
GQ_FN void gqa_dec_body(const uint16_t *q, const uint16_t *k, const uint16_t *v,
                        const uint32_t *pos, uint16_t *out, int nhq, int nhkv, int hd, int B,
                        int lane, int w, int hkv, int slice, gq_dec_smem *sm)
{
    const int group = nhkv > 0 ? nhq / nhkv : 0;
    const int M = B * group;
    if (hd != GQ_HD || group <= 0 || group * nhkv != nhq || M <= 0 || M > 16) {
        return; /* outside the contract: the launch must not select this kernel */
    }
    const int g = lane >> 2;
    const int c = lane & 3;
    const int d0 = slice * GQ_DEC_DS;
    const size_t per_pos = (size_t)nhkv * GQ_HD;
    const uint16_t *kh = k + (size_t)hkv * GQ_HD;
    const uint16_t *vh = v + (size_t)hkv * GQ_HD + d0;

    int tmax = 1;
    int tmin = 1 << 30;
    for (int b = 0; b < B; b++) {
        const int t = (int)pos[b] + 1;
        tmax = t > tmax ? t : tmax;
        tmin = t < tmin ? t : tmin;
    }
    const int lim[2] = {gq_row_lim(pos, g, M, group, tmax), gq_row_lim(pos, g + 8, M, group, tmax)};

    gq_u4 qa[4], qb[4];
    gq_load_q(qa, q, g, M, group, nhq, hkv, c);
    gq_load_q(qb, q, g + 8, M, group, nhq, hkv, c);

    float o[GQ_DEC_DS / 8][4];
#pragma unroll
    for (int n = 0; n < GQ_DEC_DS / 8; n++) {
        o[n][0] = o[n][1] = o[n][2] = o[n][3] = 0.0f;
    }
    float mrow[2] = {-INFINITY, -INFINITY};
    float lrow[2] = {0.0f, 0.0f};
    gq_u4 *vs = sm->v[w];

    for (int t0 = 16 * w; t0 < tmax; t0 += 16 * GQ_DEC_WARPS) {
        gq_u4 kr[2][4];
#pragma unroll
        for (int u = 0; u < 2; u++) {
            const int t = t0 + 8 * u + g;
            const bool ok = t < tmax;
            const uint16_t *row = kh + (size_t)(ok ? t : 0) * per_pos;
#pragma unroll
            for (int jj = 0; jj < 4; jj++) {
                if (ok) {
                    kr[u][jj] = gq_ld16(row + 32 * jj + 8 * c);
                } else {
                    kr[u][jj].x = kr[u][jj].y = kr[u][jj].z = kr[u][jj].w = 0u;
                }
            }
        }
        /* V tile: lane = position (lane >> 1), chunk (lane & 1) of the slice. */
        {
            const int p = lane >> 1;
            const int ch = lane & 1;
            gq_u4 x;
            if (t0 + p < tmax) {
                x = gq_ld16(vh + (size_t)(t0 + p) * per_pos + 8 * ch);
            } else {
                x.x = x.y = x.z = x.w = 0u;
            }
            gq_syncwarp(); /* the previous tile's ldmatrix is done with vs */
            vs[2 * p + (ch ^ ((p >> 2) & 1))] = x;
            gq_syncwarp();
        }

        float s[2][4];
        gq_scores(s, qa, qb, kr);
        float alpha[2];
        uint32_t pa[4], pl[4];
        gq_softmax_block<PX>(s, t0, c, lim, t0 + 16 <= tmin, mrow, lrow, alpha, pa, pl);
        if (gq_any(alpha[0] != 1.0f || alpha[1] != 1.0f)) {
#pragma unroll
            for (int n = 0; n < GQ_DEC_DS / 8; n++) {
                o[n][0] *= alpha[0];
                o[n][1] *= alpha[0];
                o[n][2] *= alpha[1];
                o[n][3] *= alpha[1];
            }
        }
        /* Matrix i = lane >> 3: positions 8 (i & 1) .. +7, chunk i >> 1. */
        {
            const int i = lane >> 3;
            const int r = (lane & 7) + 8 * (i & 1);
            uint32_t vb[4];
            gq_ldsm_x4_t(vb, &vs[2 * r + ((i >> 1) ^ ((r >> 2) & 1))]);
            gq_mma(o[0], pa[0], pa[1], pa[2], pa[3], vb[0], vb[1]);
            gq_mma(o[1], pa[0], pa[1], pa[2], pa[3], vb[2], vb[3]);
            if (PX) {
                gq_mma(o[0], pl[0], pl[1], pl[2], pl[3], vb[0], vb[1]);
                gq_mma(o[1], pl[0], pl[1], pl[2], pl[3], vb[2], vb[3]);
            }
        }
    }

    /* Whole-row l: the four lanes of a quad hold disjoint positions. */
#pragma unroll
    for (int r = 0; r < 2; r++) {
        lrow[r] += gq_shfl_xor(lrow[r], 1);
        lrow[r] += gq_shfl_xor(lrow[r], 2);
    }
    if (c == 0) {
        sm->m[w][g] = mrow[0];
        sm->m[w][g + 8] = mrow[1];
        sm->l[w][g] = lrow[0];
        sm->l[w][g + 8] = lrow[1];
    }
#pragma unroll
    for (int n = 0; n < GQ_DEC_DS / 8; n++) {
        sm->o[w][g][8 * n + 2 * c] = o[n][0];
        sm->o[w][g][8 * n + 2 * c + 1] = o[n][1];
        sm->o[w][g + 8][8 * n + 2 * c] = o[n][2];
        sm->o[w][g + 8][8 * n + 2 * c + 1] = o[n][3];
    }
    gq_syncthreads();

    /* Merge the warps: bring each to the largest maximum. A warp that saw no
     * position of the row has l = 0 and adds nothing. */
    for (int e = w * 32 + lane; e < M * GQ_DEC_DS; e += 32 * GQ_DEC_WARPS) {
        const int m = e / GQ_DEC_DS;
        const int d = e - m * GQ_DEC_DS;
        float gm = -INFINITY;
#pragma unroll
        for (int i = 0; i < GQ_DEC_WARPS; i++) {
            gm = fmaxf(gm, sm->m[i][m]);
        }
        float tot = 0.0f;
        float acc = 0.0f;
#pragma unroll
        for (int i = 0; i < GQ_DEC_WARPS; i++) {
            const float f = sm->l[i][m] > 0.0f ? gq_ex2(sm->m[i][m] - gm) : 0.0f;
            tot += f * sm->l[i][m];
            acc += f * sm->o[i][m][d];
        }
        const int b = m / group;
        const size_t idx = ((size_t)b * nhq + (size_t)hkv * group + (m - b * group)) * GQ_HD + d0 + d;
        out[idx] = (uint16_t)gq_f2h(tot > 0.0f ? acc / tot : 0.0f);
    }
}

/* ---- prefill ---- */

struct gq_pf_smem {
    /* Two stages of K and V tiles, GQ_PF_KT positions x 128 dims, swizzled. */
    gq_u4 k[2][GQ_PF_KT * GQ_ROW_CHUNKS];
    gq_u4 v[2][GQ_PF_KT * GQ_ROW_CHUNKS];
};

/* Issue one stage: K and V rows t0 .. t0 + GQ_PF_KT - 1 of head hkv. */
GQ_FN void gq_pf_issue(gq_pf_smem *sm, int st, const uint16_t *kh, const uint16_t *vh,
                       size_t per_pos, int t0, int tcta, int tid)
{
    for (int e = tid; e < GQ_PF_KT * GQ_ROW_CHUNKS; e += 32 * GQ_PF_WARPS) {
        const int r = e >> 4;
        const int ch = e & 15;
        const int t = t0 + r;
        const bool ok = t < tcta;
        const size_t off = (size_t)(ok ? t : 0) * per_pos + 8 * ch;
        const int slot = r * GQ_ROW_CHUNKS + (ch ^ gq_swz(r & 7));
        gq_cp16(&sm->k[st][slot], kh + off, ok);
        gq_cp16(&sm->v[st][slot], vh + off, ok);
    }
    gq_cp_commit();
}

template <bool PX>
GQ_FN void gqa_pf_body(const uint16_t *q, const uint16_t *k, const uint16_t *v,
                       const uint32_t *pos, uint16_t *out, int nhq, int nhkv, int hd, int B,
                       int lane, int w, int hkv, int ygrp, gq_pf_smem *sm)
{
    const int group = nhkv > 0 ? nhq / nhkv : 0;
    const int M = B * group;
    if (hd != GQ_HD || group <= 0 || group * nhkv != nhq || M <= 0) {
        return; /* outside the contract: the launch must not select this kernel */
    }
    const int g = lane >> 2;
    const int c = lane & 3;
    const int m0 = (ygrp * GQ_PF_WARPS + w) * 16; /* this warp's M tile */
    const size_t per_pos = (size_t)nhkv * GQ_HD;
    const uint16_t *kh = k + (size_t)hkv * GQ_HD;
    const uint16_t *vh = v + (size_t)hkv * GQ_HD;

    /* Positions this CTA must load: the largest limit of its rows. Each
     * warp's own range, and the range every one of its rows sees. */
    const int cta_m0 = ygrp * GQ_PF_WARPS * 16;
    int tcta = 1;
    for (int b = cta_m0 / group; b < B && b * group < cta_m0 + GQ_PF_WARPS * 16; b++) {
        const int t = (int)pos[b] + 1;
        tcta = t > tcta ? t : tcta;
    }
    int tw = 0;
    int tmin = 1 << 30;
    for (int b = m0 / group; b < B && b * group < m0 + 16; b++) {
        const int t = (int)pos[b] + 1;
        tw = t > tw ? t : tw;
        tmin = t < tmin ? t : tmin;
    }
    const int lim[2] = {gq_row_lim(pos, m0 + g, M, group, tw),
                        gq_row_lim(pos, m0 + g + 8, M, group, tw)};

    gq_u4 qa[4], qb[4];
    gq_load_q(qa, q, m0 + g, M, group, nhq, hkv, c);
    gq_load_q(qb, q, m0 + g + 8, M, group, nhq, hkv, c);

    float o[GQ_HD / 8][4];
#pragma unroll
    for (int n = 0; n < GQ_HD / 8; n++) {
        o[n][0] = o[n][1] = o[n][2] = o[n][3] = 0.0f;
    }
    float mrow[2] = {-INFINITY, -INFINITY};
    float lrow[2] = {0.0f, 0.0f};
    const int tid = w * 32 + lane;
    const int ntiles = (tcta + GQ_PF_KT - 1) / GQ_PF_KT;

    gq_pf_issue(sm, 0, kh, vh, per_pos, 0, tcta, tid);
    for (int it = 0; it < ntiles; it++) {
        const int st = it & 1;
        if (it + 1 < ntiles) {
            gq_pf_issue(sm, st ^ 1, kh, vh, per_pos, (it + 1) * GQ_PF_KT, tcta, tid);
            gq_cp_wait1();
        } else {
            gq_cp_wait0();
        }
        gq_syncthreads(); /* stage st is complete and visible to every warp */

        const int t0 = it * GQ_PF_KT;
        if (t0 < tw) {
            const gq_u4 *ks = sm->k[st];
            const gq_u4 *vs = sm->v[st];
            gq_u4 kr[2][4];
#pragma unroll
            for (int u = 0; u < 2; u++) {
                const int r = 8 * u + g;
#pragma unroll
                for (int jj = 0; jj < 4; jj++) {
                    kr[u][jj] = ks[r * GQ_ROW_CHUNKS + ((4 * jj + c) ^ gq_swz(g))];
                }
            }
            float s[2][4];
            gq_scores(s, qa, qb, kr);
            float alpha[2];
            uint32_t pa[4], pl[4];
            gq_softmax_block<PX>(s, t0, c, lim, t0 + 16 <= tmin, mrow, lrow, alpha, pa, pl);
            if (gq_any(alpha[0] != 1.0f || alpha[1] != 1.0f)) {
#pragma unroll
                for (int n = 0; n < GQ_HD / 8; n++) {
                    o[n][0] *= alpha[0];
                    o[n][1] *= alpha[0];
                    o[n][2] *= alpha[1];
                    o[n][3] *= alpha[1];
                }
            }
            /* Matrix i = lane >> 3: positions 8 (i & 1) .. +7, chunk
             * 2 np + (i >> 1), i.e. dims 16 np .. 16 np + 15 per x4. */
            const int i = lane >> 3;
            const int r = (lane & 7) + 8 * (i & 1);
#pragma unroll
            for (int np = 0; np < GQ_HD / 16; np++) {
                uint32_t vb[4];
                gq_ldsm_x4_t(vb, &vs[r * GQ_ROW_CHUNKS + ((2 * np + (i >> 1)) ^ gq_swz(r & 7))]);
                gq_mma(o[2 * np], pa[0], pa[1], pa[2], pa[3], vb[0], vb[1]);
                gq_mma(o[2 * np + 1], pa[0], pa[1], pa[2], pa[3], vb[2], vb[3]);
                if (PX) {
                    gq_mma(o[2 * np], pl[0], pl[1], pl[2], pl[3], vb[0], vb[1]);
                    gq_mma(o[2 * np + 1], pl[0], pl[1], pl[2], pl[3], vb[2], vb[3]);
                }
            }
        }
        gq_syncthreads(); /* everyone is done with stage st before it is refilled */
    }

#pragma unroll
    for (int r = 0; r < 2; r++) {
        lrow[r] += gq_shfl_xor(lrow[r], 1);
        lrow[r] += gq_shfl_xor(lrow[r], 2);
    }
#pragma unroll
    for (int r = 0; r < 2; r++) {
        const int m = m0 + g + 8 * r;
        if (m < M) {
            const int b = m / group;
            uint16_t *row = out + ((size_t)b * nhq + (size_t)hkv * group + (m - b * group)) * GQ_HD;
            const float inv = lrow[r] > 0.0f ? 1.0f / lrow[r] : 0.0f;
#pragma unroll
            for (int n = 0; n < GQ_HD / 8; n++) {
                *reinterpret_cast<uint32_t *>(row + 8 * n + 2 * c) =
                    gq_pack(o[n][2 * r] * inv, o[n][2 * r + 1] * inv);
            }
        }
    }
}

#ifndef SLM_GQA_EMU
/* Arguments and cbuf ABI as slm_gqa_w8: q, k, v, pos, out, nhq, nhkv, hd, B.
 * The _p16 kernels round P to FP16 alone (see gq_softmax_block); they are
 * built only with GQA_P16_VARIANTS (the harness's comparison), never into the
 * operator library. */
#define GQ_KERNEL(name, body, px, warps, smem_t)                                                   \
    __global__ void __launch_bounds__(32 * (warps))                                                \
        name(const half *q, const half *k, const half *v, const uint32_t *pos, half *out, int nhq, \
             int nhkv, int hd, int B)                                                              \
    {                                                                                              \
        __shared__ smem_t sm;                                                                      \
        body<px>(reinterpret_cast<const uint16_t *>(q), reinterpret_cast<const uint16_t *>(k),     \
                 reinterpret_cast<const uint16_t *>(v), pos, reinterpret_cast<uint16_t *>(out),    \
                 nhq, nhkv, hd, B, threadIdx.x, threadIdx.y, blockIdx.x, blockIdx.y, &sm);         \
    }
GQ_KERNEL(slm_gqa_hmma_dec, gqa_dec_body, true, GQ_DEC_WARPS, gq_dec_smem)
GQ_KERNEL(slm_gqa_hmma_pf, gqa_pf_body, true, GQ_PF_WARPS, gq_pf_smem)
#ifdef GQA_P16_VARIANTS
GQ_KERNEL(slm_gqa_hmma_dec_p16, gqa_dec_body, false, GQ_DEC_WARPS, gq_dec_smem)
GQ_KERNEL(slm_gqa_hmma_pf_p16, gqa_pf_body, false, GQ_PF_WARPS, gq_pf_smem)
#endif
#undef GQ_KERNEL
#endif

#endif /* SLM_GQA_HMMA_CU */
