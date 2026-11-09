/*
 * gqa_check.h - GQA test inputs, the exact reference and the pass rule (#1167).
 *
 * Shared by the host emulation (gqa_emu_test.cpp) and the board benchmark
 * (gqa_bench.cu), so both judge the kernels by the same cases and numbers.
 *
 * Reference: softmax(q.k / sqrt(hd)) . v in double over the FP16 inputs, with
 * no rounding before the output. A kernel's FP16 output passes when every
 * element is finite, within the opcheck bound |got - ref| <= 0.002 |ref| + 0.03
 * (runtime/src/slm/gpu_program.rs) and within 4 FP16 ULP of ref (the bound
 * slm_forward's per-kernel check holds slm_gqa_w8 to), the ULP taken at
 * max(|ref|, 2^-6) (GQA_ULP_FLOOR). The _p16 kernels are held to the opcheck
 * bound only.
 *
 * Cases: the opcheck's own inputs (its LFSR, rows 1, 2, 9, 63, 64 at positions
 * 16 + r), long context (rows ending at kv 512, 2048, 4096), other value
 * ranges, odd lengths and positions that are not consecutive. KV slots at and
 * past the largest pos + 1 hold FP16 NaN and Inf, which a kernel must never
 * use.
 */
#ifndef GQA_CHECK_H
#define GQA_CHECK_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "gguf_lite.h"

struct GqaShape {
    int nhq = 12; /* Qwen2.5-1.5B */
    int nhkv = 2;
    int hd = 128;
};

enum GqaDist {
    GQA_OPCHECK, /* the opcheck LFSR: values in [0, 4) */
    GQA_FLAT,    /* tiny q: near-uniform weights over every position */
    GQA_LARGE,   /* large q and k with outliers: one position dominates */
    GQA_NORMAL,  /* N(0, 1.5), as slm_forward's checks */
};

struct GqaCase {
    std::string name;
    std::vector<uint32_t> pos; /* one per row; B = pos.size() */
    int cap;                   /* KV slots allocated */
    GqaDist dist;
};

struct GqaData {
    int B = 0, cap = 0, tmax = 0;
    std::vector<uint32_t> pos;
    std::vector<uint16_t> q, k, v;
};

/* runtime/src/slm/gpu_program.rs Lfsr::next_f16. */
struct GqaLfsr {
    uint32_t s;
    explicit GqaLfsr(uint32_t seed) : s(seed | 1u) {}
    uint16_t next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const int v = (int)(s >> 8) % 8192;
        return gg_f32_to_f16((float)v / 2048.0f);
    }
};

static inline GqaData gqa_make(const GqaShape &S, const GqaCase &c, uint32_t seed)
{
    GqaData d;
    d.B = (int)c.pos.size();
    d.cap = c.cap;
    d.pos = c.pos;
    d.tmax = 0;
    for (uint32_t p : c.pos) {
        d.tmax = std::max(d.tmax, (int)p + 1);
    }
    const size_t nq = (size_t)d.B * S.nhq * S.hd;
    const size_t per = (size_t)S.nhkv * S.hd;
    const size_t nkv = (size_t)d.cap * per;
    const size_t live = (size_t)d.tmax * per;
    d.q.resize(nq);
    d.k.resize(nkv);
    d.v.resize(nkv);
    if (c.dist == GQA_OPCHECK) {
        GqaLfsr rq(0x1234), rk(0x5555), rv(0x2468);
        for (auto &x : d.q) {
            x = rq.next();
        }
        for (size_t i = 0; i < live; i++) {
            d.k[i] = rk.next();
            d.v[i] = rv.next();
        }
    } else {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        auto h = [](float f) { return gg_f32_to_f16(std::max(-60000.0f, std::min(60000.0f, f))); };
        for (auto &x : d.q) {
            x = c.dist == GQA_FLAT    ? h(0.05f * uni(rng))
                : c.dist == GQA_LARGE ? h(8.0f * nd(rng))
                                      : h(1.5f * nd(rng));
        }
        for (size_t i = 0; i < live; i++) {
            float kk = c.dist == GQA_FLAT ? 4.0f * uni(rng) : c.dist == GQA_LARGE ? 8.0f * nd(rng)
                                                                                : 1.5f * nd(rng);
            if (c.dist == GQA_LARGE && (i % 97) == 5) {
                kk = 60.0f; /* outlier dims, as Qwen's K biases give */
            }
            d.k[i] = h(kk);
            d.v[i] = c.dist == GQA_FLAT ? h(4.0f * uni(rng)) : h((c.dist == GQA_LARGE ? 2.0f : 1.5f) * nd(rng));
        }
    }
    /* Never-valid slots: NaN, +Inf, -Inf, 65504. */
    static const uint16_t poison[4] = {0x7E00, 0x7C00, 0xFC00, 0x7BFF};
    for (size_t i = live; i < nkv; i++) {
        d.k[i] = poison[i & 3];
        d.v[i] = poison[(i >> 1) & 3];
    }
    return d;
}

/* Exact reference: out [B][nhq][hd] in double. */
static inline void gqa_ref_exact(const GqaShape &S, const GqaData &d, std::vector<double> &out)
{
    const int group = S.nhq / S.nhkv;
    const size_t per = (size_t)S.nhkv * S.hd;
    out.assign((size_t)d.B * S.nhq * S.hd, 0.0);
    std::vector<float> fq(d.q.size()), fk((size_t)d.tmax * per), fv((size_t)d.tmax * per);
    for (size_t i = 0; i < fq.size(); i++) {
        fq[i] = gg_f16_to_f32(d.q[i]);
    }
    for (size_t i = 0; i < fk.size(); i++) {
        fk[i] = gg_f16_to_f32(d.k[i]);
        fv[i] = gg_f16_to_f32(d.v[i]);
    }
    const double inv_sqrt = 1.0 / std::sqrt((double)S.hd);
#pragma omp parallel for collapse(2) schedule(dynamic)
    for (int b = 0; b < d.B; b++) {
        for (int hq = 0; hq < S.nhq; hq++) {
            const int T = (int)d.pos[b] + 1;
            const int hkv = hq / group;
            const float *qr = &fq[((size_t)b * S.nhq + hq) * S.hd];
            std::vector<double> lg(T);
            double mx = -INFINITY;
            for (int t = 0; t < T; t++) {
                const float *kt = &fk[(size_t)t * per + (size_t)hkv * S.hd];
                double s = 0;
                for (int i = 0; i < S.hd; i++) {
                    s += (double)qr[i] * (double)kt[i];
                }
                lg[t] = s * inv_sqrt;
                mx = std::max(mx, lg[t]);
            }
            double den = 0;
            for (int t = 0; t < T; t++) {
                lg[t] = std::exp(lg[t] - mx);
                den += lg[t];
            }
            double *o = &out[((size_t)b * S.nhq + hq) * S.hd];
            for (int t = 0; t < T; t++) {
                const float *vt = &fv[(size_t)t * per + (size_t)hkv * S.hd];
                const double w = lg[t] / den;
                for (int i = 0; i < S.hd; i++) {
                    o[i] += w * (double)vt[i];
                }
            }
        }
    }
}

struct GqaErr {
    double max_ulp = 0;  /* rule metric: FP16 ULP of max(|ref|, GQA_ULP_FLOOR) */
    double raw_ulp = 0;  /* information: FP16 ULP of |ref| itself */
    double mean_ulp = 0; /* of the rule metric */
    double max_abs = 0;
    double worst_ref = 0, worst_got = 0; /* element with the largest rule metric */
    long over = 0;      /* outside the opcheck bound */
    long nonfinite = 0; /* NaN or Inf */
    bool pass = false;
};

/* Below |ref| = 2^-6 one FP16 ULP (2^-17 and less) is finer than the FP32
 * rounding that any FP32-accumulating kernel (slm_gqa_w8 included) gathers
 * over up to 4096 terms of magnitude about 4 (about 2^-16), so the ULP is
 * taken at 2^-6 there. */
#define GQA_ULP_FLOOR 0.015625

static inline double gqa_ulp16(double r)
{
    const double a = std::fabs(r);
    if (a < 6.103515625e-05) {
        return 5.9604644775390625e-08; /* 2^-24 */
    }
    return std::ldexp(1.0, (int)std::floor(std::log2(a)) - 10);
}

/* ulp_rule: also hold the kernel to 4 FP16 ULP. Without it only the opcheck
 * bound and finiteness decide (the _p16 kernels, whose FP16 P alone errs by
 * up to 2^-11 of |V|; the ULP figures are still printed). */
static inline GqaErr gqa_compare(const std::vector<uint16_t> &got, const std::vector<double> &ref,
                                 bool ulp_rule = true)
{
    GqaErr e;
    double sum = 0;
    for (size_t i = 0; i < got.size(); i++) {
        const double g = gg_f16_to_f32(got[i]);
        if (!std::isfinite(g)) {
            e.nonfinite++;
            continue;
        }
        const double ab = std::fabs(g - ref[i]);
        const double u = ab / gqa_ulp16(std::max(std::fabs(ref[i]), GQA_ULP_FLOOR));
        if (u > e.max_ulp) {
            e.max_ulp = u;
            e.worst_ref = ref[i];
            e.worst_got = g;
        }
        e.raw_ulp = std::max(e.raw_ulp, ab / gqa_ulp16(ref[i]));
        e.max_abs = std::max(e.max_abs, ab);
        sum += u;
        if (!(ab <= 0.002 * std::fabs(ref[i]) + 0.03)) {
            e.over++;
        }
    }
    e.mean_ulp = got.empty() ? 0 : sum / (double)got.size();
    e.pass = e.nonfinite == 0 && e.over == 0 && (!ulp_rule || e.max_ulp <= 4.0);
    return e;
}

static inline void gqa_report(const char *kernel, const GqaCase &c, const GqaErr &e,
                              bool ulp_rule = true)
{
    printf("  %-13s %-30s ulp max %7.2f mean %5.3f (ref %+.3e got %+.3e) raw %8.2f abs %.2e "
           "over %ld nonfinite %ld: %s%s\n",
           kernel, c.name.c_str(), e.max_ulp, e.mean_ulp, e.worst_ref, e.worst_got, e.raw_ulp,
           e.max_abs, e.over, e.nonfinite, e.pass ? "PASS" : "FAIL",
           ulp_rule ? "" : " (opcheck rule)");
}

/* Per-case seed from the name (FNV-1a), so a filtered run sees the same data. */
static inline uint32_t gqa_seed(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char ch : s) {
        h = (h ^ ch) * 16777619u;
    }
    return h;
}

static inline std::vector<uint32_t> gqa_run(int B, int last)
{
    std::vector<uint32_t> p(B);
    for (int i = 0; i < B; i++) {
        p[i] = (uint32_t)(last - (B - 1) + i);
    }
    return p;
}

/* The check matrix. long_ctx adds the kv 4096 cases. */
static inline std::vector<GqaCase> gqa_cases(bool long_ctx)
{
    std::vector<GqaCase> v;
    const char *dn[4] = {"opcheck", "flat", "large", "normal"};
    for (int rows : {1, 2, 9, 63, 64}) {
        std::vector<uint32_t> p(rows);
        for (int r = 0; r < rows; r++) {
            p[r] = (uint32_t)(16 + r);
        }
        v.push_back({"opcheck rows " + std::to_string(rows), p, 80, GQA_OPCHECK});
    }
    std::vector<int> kvs = {512, 2048};
    if (long_ctx) {
        kvs.push_back(4096);
    }
    int di = 0;
    for (int kv : kvs) {
        for (int rows : {1, 2, 9, 63, 64}) {
            const GqaDist d = (GqaDist)(di++ % 4);
            v.push_back({"kv " + std::to_string(kv) + " rows " + std::to_string(rows) + " " + dn[d],
                         gqa_run(rows, kv - 1), kv + 16, d});
        }
    }
    for (int d = 0; d < 4; d++) {
        v.push_back({std::string("kv 2048 rows 1 ") + dn[d], gqa_run(1, 2047), 2048 + 16, (GqaDist)d});
        v.push_back({std::string("kv 2048 rows 64 ") + dn[d], gqa_run(64, 2047), 2048 + 16,
                     (GqaDist)d});
    }
    v.push_back({"kv 1 rows 1", {0}, 16, GQA_NORMAL});
    v.push_back({"kv 17 rows 1", {16}, 32, GQA_NORMAL});
    v.push_back({"kv 2047 rows 1 flat", {2046}, 2047, GQA_FLAT}); /* no slack after the last row */
    v.push_back({"rows 2 pos 100,7", {100, 7}, 128, GQA_NORMAL});
    {
        std::mt19937 rng(7);
        std::vector<uint32_t> p(9);
        for (auto &x : p) {
            x = rng() % 2000;
        }
        v.push_back({"rows 9 random pos < 2000", p, 2048, GQA_NORMAL});
    }
    v.push_back({"rows 64 all pos 1000", std::vector<uint32_t>(64, 1000), 1001, GQA_NORMAL});
    {
        std::vector<uint32_t> p(63);
        for (int r = 0; r < 63; r++) {
            p[r] = (uint32_t)(700 - 9 * r);
        }
        v.push_back({"rows 63 decreasing pos", p, 701, GQA_LARGE});
    }
    return v;
}

#endif /* GQA_CHECK_H */
