/*
 * test_ref.cpp - host check that the harness's scalar CPU reference agrees with
 * the ggml golden vectors in scripts/fixtures/slm/golden_ops.rec. Every GPU
 * kernel is judged against this reference on the board, so it must be right
 * first. Plain g++, no CUDA.
 */
#include <cassert>

#include "../slm/slm_chunks.h"
#include "gguf_lite.h"
#include "slm_cpu_ref.h"

static std::map<std::string, std::vector<uint8_t>> R;

static std::vector<float> f32s(const std::string &n)
{
    const auto &b = R.at(n);
    std::vector<float> v(b.size() / 4);
    memcpy(v.data(), b.data(), b.size());
    return v;
}

static h16 fp16s(const std::string &n)
{
    const auto f = f32s(n);
    h16 v(f.size());
    for (size_t i = 0; i < f.size(); i++) {
        v[i] = gg_f32_to_f16(f[i]);
    }
    return v;
}

static int fails = 0;

/*
 * slm_rope's control flow, one warp of 32 lanes per head, against the CPU
 * reference. A truncating chunk count skipped every pair at head_dim 32, which
 * is what tiny_qwen2.gguf uses: the kernel returned its input untouched.
 */
static void check_rope_coverage(int hd)
{
    const int half_hd = hd >> 1, nh = 2;
    std::vector<float> table;
    ref_rope_table(hd, 8, 1000000.0f, table);
    h16 in((size_t)nh * hd);
    for (size_t i = 0; i < in.size(); i++) {
        in[i] = gg_f32_to_f16(1.0f + 0.03125f * (float)i);
    }
    const uint32_t pos = 5;
    h16 want = in, got = in;
    ref_rope(table, &pos, nh, hd, 1, want.data());
    std::vector<int> visits(in.size(), 0);
    const float *t = &table[(size_t)pos * hd];
    for (int head = 0; head < nh; head++) {
        const size_t base = (size_t)head * hd;
        for (int c = 0; c < SLM_ROPE_CHUNKS(half_hd); c++) {
            for (int l = 0; l < 32; l++) {
                const int i = (c << 5) + l;
                if (i >= half_hd) {
                    continue;
                }
                const float v0 = gg_f16_to_f32(in[base + i]);
                const float v1 = gg_f16_to_f32(in[base + i + half_hd]);
                got[base + i] = gg_f32_to_f16(v0 * t[2 * i] - v1 * t[2 * i + 1]);
                got[base + i + half_hd] = gg_f32_to_f16(v0 * t[2 * i + 1] + v1 * t[2 * i]);
                visits[base + i]++;
                visits[base + i + half_hd]++;
            }
        }
    }
    bool ok = got == want && got != in;
    for (int v : visits) {
        ok = ok && v == 1;
    }
    printf("rope coverage hd=%-3d every element rotated once: %s\n", hd, ok ? "PASS" : "FAIL");
    fails += !ok;
}

static void cmp_f32(const char *name, const float *got, const std::vector<float> &want, float tol)
{
    double worst = 0;
    for (size_t i = 0; i < want.size(); i++) {
        worst = std::max(worst, (double)fabsf(got[i] - want[i]) / (fabsf(want[i]) + 1e-3));
    }
    const bool ok = worst <= tol;
    printf("%-14s worst relative error %.2e, tol %.1e: %s\n", name, worst, tol, ok ? "PASS" : "FAIL");
    fails += !ok;
}

/* FP16 output against a real-valued reference: half a ULP plus 3e-6 relative. */
static void cmp_h16(const char *name, const h16 &got, const std::vector<float> &want, float ulps)
{
    double worst = 0;
    for (size_t i = 0; i < want.size(); i++) {
        const float a = fabsf(want[i]);
        int e;
        frexpf(a < 6.103515625e-05f ? 6.103515625e-05f : a, &e);
        const float half = ldexpf(0.5f, e - 11);
        const double err = fabsf(gg_f16_to_f32(got[i]) - want[i]) - 3e-6f * a;
        worst = std::max(worst, err / half);
    }
    const bool ok = worst <= ulps;
    printf("%-14s worst error %.2f half-ULP, tol %.2f: %s\n", name, worst, ulps, ok ? "PASS" : "FAIL");
    fails += !ok;
}

int main(int argc, char **argv)
{
    R = read_rec(argc > 1 ? argv[1] : "scripts/fixtures/slm/golden_ops.rec");

    {
        const auto &b = R.at("q4k.bytes");
        std::vector<float> y(b.size() / 144 * 256);
        for (size_t i = 0; i < b.size() / 144; i++) {
            gg_q4k_block(&b[i * 144], &y[i * 256]);
        }
        cmp_f32("q4k dequant", y.data(), f32s("q4k.dequant"), 1e-5f);
    }
    {
        const auto &b = R.at("q6k.bytes");
        std::vector<float> y(b.size() / 210 * 256);
        for (size_t i = 0; i < b.size() / 210; i++) {
            gg_q6k_block(&b[i * 210], &y[i * 256]);
        }
        cmp_f32("q6k dequant", y.data(), f32s("q6k.dequant"), 1e-5f);
    }
    {
        const h16 x = fp16s("rmsnorm.x");
        const auto gamma = f32s("rmsnorm.gamma");
        const auto want = f32s("rmsnorm.out");
        h16 out(x.size());
        /* ggml's norm eps in the generator is llama.cpp's default for the model. */
        ref_rmsnorm(x.data(), gamma.data(), 1e-6f, (int)x.size(), 1, out.data());
        cmp_h16("rmsnorm", out, want, 2.02f);
    }
    {
        const auto p = f32s("rope.params");
        const auto positions = f32s("rope.positions");
        const int hd = (int)p[0], nh = (int)p[1];
        int maxp = 0;
        for (float v : positions) {
            maxp = std::max(maxp, (int)v);
        }
        std::vector<float> table;
        ref_rope_table(hd, maxp + 1, p[2], table);
        for (size_t i = 0; i < positions.size(); i++) {
            h16 v = fp16s("rope.x");
            const uint32_t pos = (uint32_t)positions[i];
            ref_rope(table, &pos, nh, hd, 1, v.data());
            char name[32];
            snprintf(name, sizeof name, "rope pos=%u", pos);
            cmp_h16(name, v, f32s("rope.out." + std::to_string(i)), 2.02f);
        }
    }
    {
        const auto d = f32s("gqa.dims");
        const int nq = (int)d[0], nk = (int)d[1], dim = (int)d[2], seq = (int)d[3];
        const h16 q = fp16s("gqa.q"), k = fp16s("gqa.k"), v = fp16s("gqa.v");
        h16 out((size_t)nq * dim);
        const uint32_t pos = (uint32_t)seq - 1;
        ref_gqa(q.data(), k.data(), v.data(), &pos, nq, nk, dim, 1, out.data());
        cmp_h16("gqa", out, f32s("gqa.out"), 2.02f);
    }
    {
        const h16 g = fp16s("swiglu.gate"), u = fp16s("swiglu.up");
        h16 out(g.size());
        ref_swiglu(g.data(), u.data(), g.size(), out.data());
        /* Two rounding points, see check_swiglu in runtime/src/slm/golden.rs. */
        cmp_h16("swiglu", out, f32s("swiglu.out"), 4.0f);
    }
    for (int hd : {32, 64, 128, 256}) {
        check_rope_coverage(hd);
    }
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
