/*
 * ref_variants.cpp - where does the logit difference from llama.cpp come from?
 *
 * Runs the Qwen2 forward pass on the CPU under each combination of two choices
 * and compares every step's logits with llama.cpp's greedy record:
 *   hidden state  F32 (llama.cpp) or FP16 (SLM-OS, CPU and GPU)
 *   matmul        Q8_K activations (llama.cpp, and the SLM-OS CPU path for
 *                 Q4_K weights) or the exact dot product (the SLM-OS GPU path)
 *
 * The F32 + Q8_K row should reproduce llama.cpp, which validates the model of
 * what it does. The FP16 + exact row is what the GPU kernels should give.
 *
 * Second mode: `q8k <model.gguf> <tensor>` compares one matmul's Q8_K result
 * with the true dot product on real weights, the question behind `slm gpu
 * opcheck` differing on Q4_K.
 *
 * Plain g++: g++ -O2 -std=c++17 ref_variants.cpp -o ref_variants -lm
 */
#include <algorithm>
#include <random>

#include "gguf_lite.h"
#include "slm_cpu_ref.h"

typedef std::vector<float> fv;

static float rnd16(float v) { return gg_f16_to_f32(gg_f32_to_f16(v)); }

/* Round-trip through Q8_K: ggml quantize_row_q8_K_ref, per 256 elements. */
static void q8k_round(const float *x, int n, float *o)
{
    for (int b = 0; b < n; b += 256) {
        float mx = 0, amax = 0;
        for (int i = 0; i < 256; i++) {
            if (fabsf(x[b + i]) > amax) {
                amax = fabsf(x[b + i]);
                mx = x[b + i];
            }
        }
        if (amax == 0) {
            std::fill(o + b, o + b + 256, 0.0f);
            continue;
        }
        const float iscale = -127.0f / mx;
        for (int i = 0; i < 256; i++) {
            const int q = std::min(127, (int)lrintf(iscale * x[b + i]));
            o[b + i] = (float)q / iscale;
        }
    }
}

struct Opt {
    bool f16_hidden, q8k;
    const char *name;
};

static fv mm(const Gguf &g, const GgufTensor &t, const fv &x, const Opt &o)
{
    const int K = (int)t.dim0, N = (int)t.dim1;
    fv in(x.size()), out(N), row(K);
    if (o.q8k) {
        q8k_round(x.data(), K, in.data());
    } else {
        in = x;
    }
    for (int r = 0; r < N; r++) {
        gg_decode_row(g.data.data(), t, r, row.data());
        double s = 0;
        for (int k = 0; k < K; k++) {
            s += (double)row[k] * in[k];
        }
        out[r] = (float)s;
    }
    return out;
}

struct Fwd {
    const Gguf &g;
    Opt o;
    int layers, H, F, nq, nk, hd;
    std::vector<float> rope;
    std::vector<fv> K, V;

    float rd(float v) const { return o.f16_hidden ? rnd16(v) : v; }
    void rdv(fv &v) const
    {
        if (o.f16_hidden) {
            for (float &f : v) {
                f = rnd16(f);
            }
        }
    }
    const float *wf(const std::string &n) const
    {
        return reinterpret_cast<const float *>(g.data.data() + g.tensors.at(n).offset);
    }
    const GgufTensor &t(const std::string &n) const { return g.tensors.at(n); }

    fv norm(const fv &x, const float *gm) const
    {
        double a = 0;
        for (float v : x) {
            a += (double)v * v;
        }
        const float inv = 1.0f / sqrtf((float)(a / x.size()) + g.rms_eps);
        fv y(x.size());
        for (size_t i = 0; i < x.size(); i++) {
            y[i] = rd(x[i] * inv * gm[i]);
        }
        return y;
    }

    fv step(uint32_t id, int pos)
    {
        fv x(H);
        gg_decode_row(g.data.data(), t("token_embd.weight"), id, x.data());
        rdv(x);
        for (int l = 0; l < layers; l++) {
            const std::string b = "blk." + std::to_string(l) + ".";
            fv n = norm(x, wf(b + "attn_norm.weight"));
            fv q = mm(g, t(b + "attn_q.weight"), n, o), k = mm(g, t(b + "attn_k.weight"), n, o),
               v = mm(g, t(b + "attn_v.weight"), n, o);
            const float *qb = wf(b + "attn_q.bias"), *kb = wf(b + "attn_k.bias"),
                        *vb = wf(b + "attn_v.bias");
            for (size_t i = 0; i < q.size(); i++) {
                q[i] = rd(q[i] + qb[i]);
            }
            for (size_t i = 0; i < k.size(); i++) {
                k[i] = rd(k[i] + kb[i]);
                v[i] = rd(v[i] + vb[i]);
            }
            const float *tb = &rope[(size_t)pos * hd];
            auto rot = [&](fv &a, int nh) {
                for (int h = 0; h < nh; h++) {
                    float *p = &a[(size_t)h * hd];
                    for (int i = 0; i < hd / 2; i++) {
                        const float v0 = p[i], v1 = p[i + hd / 2];
                        p[i] = rd(v0 * tb[2 * i] - v1 * tb[2 * i + 1]);
                        p[i + hd / 2] = rd(v0 * tb[2 * i + 1] + v1 * tb[2 * i]);
                    }
                }
            };
            rot(q, nq);
            rot(k, nk);
            K[l].insert(K[l].end(), k.begin(), k.end());
            V[l].insert(V[l].end(), v.begin(), v.end());
            const int T = pos + 1, per = nk * hd, group = nq / nk;
            fv a(q.size());
            for (int hq = 0; hq < nq; hq++) {
                const int hkv = hq / group;
                fv lg(T);
                float mxv = -INFINITY;
                for (int ti = 0; ti < T; ti++) {
                    double s = 0;
                    for (int d = 0; d < hd; d++) {
                        s += (double)q[(size_t)hq * hd + d] * K[l][(size_t)ti * per + (size_t)hkv * hd + d];
                    }
                    lg[ti] = (float)s / sqrtf((float)hd);
                    mxv = fmaxf(mxv, lg[ti]);
                }
                double den = 0;
                for (float &f : lg) {
                    f = expf(f - mxv);
                    den += f;
                }
                for (int d = 0; d < hd; d++) {
                    double s = 0;
                    for (int ti = 0; ti < T; ti++) {
                        s += lg[ti] / den * V[l][(size_t)ti * per + (size_t)hkv * hd + d];
                    }
                    a[(size_t)hq * hd + d] = rd((float)s);
                }
            }
            fv p = mm(g, t(b + "attn_output.weight"), a, o);
            for (int i = 0; i < H; i++) {
                x[i] = rd(x[i] + p[i]);
            }
            n = norm(x, wf(b + "ffn_norm.weight"));
            fv gt = mm(g, t(b + "ffn_gate.weight"), n, o), up = mm(g, t(b + "ffn_up.weight"), n, o);
            fv s(F);
            for (int i = 0; i < F; i++) {
                const float ga = rd(gt[i]), u = rd(up[i]);
                s[i] = rd(rd(ga / (1.0f + expf(-ga))) * u);
            }
            fv d = mm(g, t(b + "ffn_down.weight"), s, o);
            for (int i = 0; i < H; i++) {
                x[i] = rd(x[i] + rd(d[i]));
            }
        }
        fv n = norm(x, wf("output_norm.weight"));
        return mm(g, t("output.weight"), n, o);
    }
};

static int cmd_q8k(int argc, char **argv)
{
    Gguf g;
    if (argc < 4 || !gguf_read(argv[2], g)) {
        return 2;
    }
    const GgufTensor &t = g.tensors.at(argv[3]);
    std::mt19937 rng(475);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    fv x(t.dim0);
    for (float &v : x) {
        v = rnd16(nd(rng));
    }
    Opt ex = {true, false, ""}, q8 = {true, true, ""};
    const fv a = mm(g, t, x, ex), b = mm(g, t, x, q8);
    double worst = 0, rms = 0, sum = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const double e = fabs((double)a[i] - b[i]);
        worst = std::max(worst, e);
        rms += e * e;
        sum += (double)a[i] * a[i];
    }
    printf("%s: Q8_K vs true dot: max %.4f rms %.4f, output rms %.4f\n", argv[3], worst,
           sqrt(rms / a.size()), sqrt(sum / a.size()));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "q8k")) {
        return cmd_q8k(argc, argv);
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s tiny.gguf greedy.rec | q8k model.gguf tensor\n", argv[0]);
        return 2;
    }
    Gguf g;
    if (!gguf_read(argv[1], g)) {
        return 2;
    }
    auto rec = read_rec(argv[2]);
    std::vector<uint32_t> ids(rec.at("prompt.ids").size() / 4), toks(rec.at("greedy.tokens").size() / 4);
    memcpy(ids.data(), rec.at("prompt.ids").data(), ids.size() * 4);
    memcpy(toks.data(), rec.at("greedy.tokens").data(), toks.size() * 4);
    ids.insert(ids.end(), toks.begin(), toks.end());
    const int vocab = (int)g.tensors.at("output.weight").dim1;
    const auto &lb = rec.at("greedy.logits");
    const int steps = (int)(lb.size() / 4 / vocab);
    fv want(lb.size() / 4);
    memcpy(want.data(), lb.data(), lb.size());

    const Opt opts[] = {{false, true, "F32 hidden + Q8_K (llama.cpp)"},
                        {false, false, "F32 hidden + exact dot"},
                        {true, true, "FP16 hidden + Q8_K"},
                        {true, false, "FP16 hidden + exact dot (GPU)"}};
    printf("%d steps, vocab %d\n", steps, vocab);
    for (const Opt &o : opts) {
        Fwd f = {g, o, (int)g.block_count, (int)g.hidden, (int)g.ff, (int)g.nhq, (int)g.nhkv,
                 (int)(g.hidden / g.nhq), {}, {}, {}};
        ref_rope_table(f.hd, 128, g.rope_base, f.rope);
        f.K.resize(f.layers);
        f.V.resize(f.layers);
        double worst = 0;
        int wstep = 0, top_ok = 0;
        for (int s = 0; s < steps; s++) {
            const fv l = f.step(ids[s], s);
            const float *w = &want[(size_t)s * vocab];
            for (int i = 0; i < vocab; i++) {
                const double e = fabs((double)l[i] - w[i]);
                if (e > worst) {
                    worst = e;
                    wstep = s;
                }
            }
            top_ok += std::max_element(l.begin(), l.end()) - l.begin() ==
                      std::max_element(w, w + vocab) - w;
        }
        printf("%-32s max logit error %7.4f (step %d), top-1 same %d/%d\n", o.name, worst, wstep,
               top_ok, steps);
    }
    return 0;
}
