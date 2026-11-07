/*
 * slm_cpu_ref.h - scalar CPU reference for the Qwen2 forward pass.
 *
 * Follows runtime/src/slm/forward.rs and runtime/src/inference/ops_transformer.rs
 * op by op, with FP16 activations and the same rounding points. Differences
 * from the Rust path, both deliberate:
 *   - matmul accumulates exact decoded weights in double instead of quantizing
 *     the activation to Q8_K, so it is the ideal result of the GPU's FP16
 *     activation times decoded weight, without Q8_K noise.
 *   - RMSNorm uses F32 gamma, as the GPU kernel does (the Rust path rounds
 *     gamma to FP16 first).
 */
#ifndef SLM_CPU_REF_H
#define SLM_CPU_REF_H

#include <cmath>
#include <cstdint>
#include <vector>

#include "gguf_lite.h"

typedef std::vector<uint16_t> h16;

static inline void ref_matmul(const Gguf &g, const GgufTensor &t, const uint16_t *x, int B,
                              float *out)
{
    const int K = (int)t.dim0, N = (int)t.dim1;
    std::vector<float> fx((size_t)B * K);
    for (size_t i = 0; i < fx.size(); i++) {
        fx[i] = gg_f16_to_f32(x[i]);
    }
#pragma omp parallel
    {
        std::vector<float> row(K);
#pragma omp for schedule(static)
        for (int r = 0; r < N; r++) {
            gg_decode_row(g.data.data(), t, r, row.data());
            for (int b = 0; b < B; b++) {
                const float *xb = &fx[(size_t)b * K];
                double s = 0;
                for (int k = 0; k < K; k++) {
                    s += (double)row[k] * (double)xb[k];
                }
                out[(size_t)b * N + r] = (float)s;
            }
        }
    }
}

/* ggml quantize_q8_1 round trip in place: per 32 values d = amax/127,
 * q = roundf(v/d), value = FP16(d) * q, as llama.cpp's CUDA dot products use it. */
static inline void ref_q8_1(float *v, int n)
{
    for (int b = 0; b < n; b += 32) {
        float amax = 0;
        for (int i = 0; i < 32; i++) {
            amax = fmaxf(amax, fabsf(v[b + i]));
        }
        const float d = amax / 127.0f;
        const float d16 = gg_f16_to_f32(gg_f32_to_f16(d));
        for (int i = 0; i < 32; i++) {
            v[b + i] = amax == 0.0f ? 0.0f : d16 * roundf(v[b + i] / d);
        }
    }
}

/* ref_matmul with FP32 inputs. */
static inline void ref_matmul_f(const Gguf &g, const GgufTensor &t, const float *fx, int B,
                                float *out)
{
    const int K = (int)t.dim0, N = (int)t.dim1;
#pragma omp parallel
    {
        std::vector<float> row(K);
#pragma omp for schedule(static)
        for (int r = 0; r < N; r++) {
            gg_decode_row(g.data.data(), t, r, row.data());
            for (int b = 0; b < B; b++) {
                double s = 0;
                for (int k = 0; k < K; k++) {
                    s += (double)row[k] * (double)fx[(size_t)b * K + k];
                }
                out[(size_t)b * N + r] = (float)s;
            }
        }
    }
}

/* Matmul plus optional F32 bias, rounded to FP16 per row. */
static inline void ref_project(const Gguf &g, const GgufTensor &t, const float *bias,
                               const uint16_t *x, int B, uint16_t *out)
{
    const int N = (int)t.dim1;
    std::vector<float> f((size_t)B * N);
    ref_matmul(g, t, x, B, f.data());
    for (int b = 0; b < B; b++) {
        for (int r = 0; r < N; r++) {
            const float v = f[(size_t)b * N + r] + (bias ? bias[r] : 0.0f);
            out[(size_t)b * N + r] = gg_f32_to_f16(v);
        }
    }
}

static inline void ref_rmsnorm(const uint16_t *x, const float *gamma, float eps, int n, int B,
                               uint16_t *out)
{
    for (int b = 0; b < B; b++) {
        const uint16_t *xr = x + (size_t)b * n;
        float acc = 0;
        for (int i = 0; i < n; i++) {
            const float v = gg_f16_to_f32(xr[i]);
            acc += v * v;
        }
        const float inv = 1.0f / sqrtf(acc / (float)n + eps);
        for (int i = 0; i < n; i++) {
            out[(size_t)b * n + i] = gg_f32_to_f16(gg_f16_to_f32(xr[i]) * inv * gamma[i]);
        }
    }
}

static inline void ref_add(const uint16_t *a, const uint16_t *b, size_t total, uint16_t *out)
{
    for (size_t i = 0; i < total; i++) {
        out[i] = gg_f32_to_f16(gg_f16_to_f32(a[i]) + gg_f16_to_f32(b[i]));
    }
}

static inline void ref_swiglu(const uint16_t *gate, const uint16_t *up, size_t total, uint16_t *out)
{
    for (size_t i = 0; i < total; i++) {
        const float g = gg_f16_to_f32(gate[i]);
        const uint16_t s = gg_f32_to_f16(g / (1.0f + expf(-g)));
        out[i] = gg_f32_to_f16(gg_f16_to_f32(s) * gg_f16_to_f32(up[i]));
    }
}

/* Same float sequence as RopeTable::new. */
static inline void ref_rope_table(int hd, int max_pos, float base, std::vector<float> &t)
{
    t.assign((size_t)hd * max_pos, 0.0f);
    const float scale = powf(base, -2.0f / (float)hd);
    for (int pos = 0; pos < max_pos; pos++) {
        float angle = (float)pos;
        for (int i = 0; i < hd / 2; i++) {
            t[(size_t)pos * hd + 2 * i] = cosf(angle);
            t[(size_t)pos * hd + 2 * i + 1] = sinf(angle);
            angle *= scale;
        }
    }
}

static inline void ref_rope(const std::vector<float> &t, const uint32_t *pos, int nh, int hd,
                            int B, uint16_t *x)
{
    for (int b = 0; b < B; b++) {
        const float *row = &t[(size_t)pos[b] * hd];
        for (int h = 0; h < nh; h++) {
            uint16_t *v = x + ((size_t)b * nh + h) * hd;
            for (int i = 0; i < hd / 2; i++) {
                const float v0 = gg_f16_to_f32(v[i]), v1 = gg_f16_to_f32(v[i + hd / 2]);
                v[i] = gg_f32_to_f16(v0 * row[2 * i] - v1 * row[2 * i + 1]);
                v[i + hd / 2] = gg_f32_to_f16(v0 * row[2 * i + 1] + v1 * row[2 * i]);
            }
        }
    }
}

/* gqa_decode_step for each batch row, sees positions 0..=pos[b]. */
static inline void ref_gqa(const uint16_t *q, const uint16_t *k, const uint16_t *v,
                           const uint32_t *pos, int nhq, int nhkv, int hd, int B, uint16_t *out)
{
    const int group = nhq / nhkv;
    const int per_pos = nhkv * hd;
    const float inv_sqrt = 1.0f / sqrtf((float)hd);
#pragma omp parallel for collapse(2) schedule(static)
    for (int b = 0; b < B; b++) {
        for (int hq = 0; hq < nhq; hq++) {
            const int T = (int)pos[b] + 1;
            const int hkv = hq / group;
            const uint16_t *qr = q + ((size_t)b * nhq + hq) * hd;
            std::vector<float> lg(T);
            float mx = -INFINITY;
            for (int t = 0; t < T; t++) {
                const uint16_t *kt = k + (size_t)t * per_pos + (size_t)hkv * hd;
                float acc = 0;
                for (int d = 0; d < hd; d++) {
                    acc += gg_f16_to_f32(qr[d]) * gg_f16_to_f32(kt[d]);
                }
                lg[t] = acc * inv_sqrt;
                mx = fmaxf(mx, lg[t]);
            }
            float den = 0;
            for (int t = 0; t < T; t++) {
                lg[t] = expf(lg[t] - mx);
                den += lg[t];
            }
            const float inv = den > 0 ? 1.0f / den : 0.0f;
            for (int d = 0; d < hd; d++) {
                float acc = 0;
                for (int t = 0; t < T; t++) {
                    acc += lg[t] * inv *
                           gg_f16_to_f32(v[(size_t)t * per_pos + (size_t)hkv * hd + d]);
                }
                out[((size_t)b * nhq + hq) * hd + d] = gg_f32_to_f16(acc);
            }
        }
    }
}

static inline void ref_embed(const Gguf &g, const GgufTensor &t, const uint32_t *ids, int B,
                             uint16_t *out)
{
    const int K = (int)t.dim0;
    std::vector<float> row(K);
    for (int b = 0; b < B; b++) {
        gg_decode_row(g.data.data(), t, ids[b], row.data());
        for (int i = 0; i < K; i++) {
            out[(size_t)b * K + i] = gg_f32_to_f16(row[i]);
        }
    }
}

#endif /* SLM_CPU_REF_H */
