/*
 * gguf_lite.h - minimal GGUF v3 reader and scalar Q4_K / Q6_K decoders for the
 * native CUDA harness. The decoders mirror runtime/src/inference/quant.rs.
 */
#ifndef GGUF_LITE_H
#define GGUF_LITE_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define GGML_F32 0
#define GGML_F16 1
#define GGML_Q4_K 12
#define GGML_Q6_K 14

struct GgufTensor {
    std::string name;
    uint64_t dim0 = 0; /* element count per row (K) */
    uint64_t dim1 = 1; /* row count (N) */
    uint32_t type = 0;
    uint64_t offset = 0; /* from the start of the tensor data */
};

struct Gguf {
    std::vector<uint8_t> data; /* tensor data region */
    std::map<std::string, GgufTensor> tensors;
    uint64_t data_size = 0;
};

static inline float gg_f16_to_f32(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 0x3FF;
    float v;
    if (e == 0) {
        v = (float)m * 5.9604644775390625e-08f;
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    }
    return s ? -v : v;
}

static inline uint16_t gg_f32_to_f16(float f)
{
    /* Round to nearest even, matching F2F on the GPU. */
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int e = (int)((x >> 23) & 255) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 255) == 255) {
        return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0));
    }
    if (e >= 31) {
        return (uint16_t)(sign | 0x7C00);
    }
    if (e <= 0) {
        if (e < -10) {
            return (uint16_t)sign;
        }
        m |= 0x800000;
        const int shift = 14 - e;
        const uint32_t r = m >> shift;
        const uint32_t rem = m & ((1u << shift) - 1);
        const uint32_t half = 1u << (shift - 1);
        return (uint16_t)(sign | (r + (rem > half || (rem == half && (r & 1)))));
    }
    const uint32_t base = ((uint32_t)e << 10) | (m >> 13);
    const uint32_t rem = m & 0x1FFF;
    return (uint16_t)(sign + (base + (rem > 0x1000 || (rem == 0x1000 && (base & 1)))));
}

static inline void gg_q4k_scale_min(const uint8_t *s, int i, uint32_t &sc, uint32_t &mn)
{
    if (i < 4) {
        sc = s[i] & 0x3F;
        mn = s[i + 4] & 0x3F;
    } else {
        sc = (s[i + 4] & 0x0F) | ((s[i - 4] >> 6) << 4);
        mn = (s[i + 4] >> 4) | ((s[i] >> 6) << 4);
    }
}

/* Decode one 144-byte Q4_K block into 256 floats. */
static inline void gg_q4k_block(const uint8_t *b, float *y)
{
    uint16_t dh, mh;
    memcpy(&dh, b, 2);
    memcpy(&mh, b + 2, 2);
    const float d = gg_f16_to_f32(dh), dmin = gg_f16_to_f32(mh);
    const uint8_t *s = b + 4, *qs = b + 16;
    for (int j = 0; j < 4; j++) {
        uint32_t sc1, mn1, sc2, mn2;
        gg_q4k_scale_min(s, 2 * j, sc1, mn1);
        gg_q4k_scale_min(s, 2 * j + 1, sc2, mn2);
        const float d1 = d * (float)sc1, m1 = dmin * (float)mn1;
        const float d2 = d * (float)sc2, m2 = dmin * (float)mn2;
        for (int l = 0; l < 32; l++) {
            const uint8_t q = qs[j * 32 + l];
            y[j * 64 + l] = d1 * (float)(q & 15) - m1;
            y[j * 64 + 32 + l] = d2 * (float)(q >> 4) - m2;
        }
    }
}

/* Decode one 210-byte Q6_K block into 256 floats. */
static inline void gg_q6k_block(const uint8_t *b, float *y)
{
    uint16_t dh;
    memcpy(&dh, b + 208, 2);
    const float d = gg_f16_to_f32(dh);
    for (int half = 0; half < 2; half++) {
        const uint8_t *ql = b + half * 64, *qh = b + 128 + half * 32;
        const int8_t *sc = (const int8_t *)(b + 192 + half * 8);
        float *o = y + half * 128;
        for (int l = 0; l < 32; l++) {
            const int is = l / 16;
            const int q1 = ((ql[l] & 15) | ((qh[l] & 3) << 4)) - 32;
            const int q2 = ((ql[l + 32] & 15) | (((qh[l] >> 2) & 3) << 4)) - 32;
            const int q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
            const int q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
            o[l] = d * (float)sc[is] * (float)q1;
            o[l + 32] = d * (float)sc[is + 2] * (float)q2;
            o[l + 64] = d * (float)sc[is + 4] * (float)q3;
            o[l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
    }
}

static inline size_t gg_row_bytes(const GgufTensor &t)
{
    switch (t.type) {
    case GGML_Q4_K:
        return (size_t)(t.dim0 / 256) * 144;
    case GGML_Q6_K:
        return (size_t)(t.dim0 / 256) * 210;
    case GGML_F32:
        return (size_t)t.dim0 * 4;
    case GGML_F16:
        return (size_t)t.dim0 * 2;
    default:
        fprintf(stderr, "unsupported tensor type %u\n", t.type);
        exit(2);
    }
}

/* Decode one row of a tensor to floats. y has room for dim0 values. */
static inline void gg_decode_row(const uint8_t *data, const GgufTensor &t, size_t row, float *y)
{
    const uint8_t *p = data + t.offset + row * gg_row_bytes(t);
    if (t.type == GGML_Q4_K) {
        for (uint64_t b = 0; b < t.dim0 / 256; b++) {
            gg_q4k_block(p + b * 144, y + b * 256);
        }
    } else if (t.type == GGML_Q6_K) {
        for (uint64_t b = 0; b < t.dim0 / 256; b++) {
            gg_q6k_block(p + b * 210, y + b * 256);
        }
    } else if (t.type == GGML_F32) {
        memcpy(y, p, t.dim0 * 4);
    } else {
        fprintf(stderr, "gg_decode_row: bad type %u\n", t.type);
        exit(2);
    }
}

static inline bool gguf_read(const char *path, Gguf &g)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return false;
    }
    auto rd = [&](void *p, size_t n) {
        if (fread(p, 1, n, f) != n) {
            fprintf(stderr, "short read\n");
            exit(2);
        }
    };
    auto r32 = [&]() { uint32_t v; rd(&v, 4); return v; };
    auto r64 = [&]() { uint64_t v; rd(&v, 8); return v; };
    auto rstr = [&]() {
        std::string s(r64(), '\0');
        rd(&s[0], s.size());
        return s;
    };
    char magic[4];
    rd(magic, 4);
    if (memcmp(magic, "GGUF", 4)) {
        fprintf(stderr, "%s: not GGUF\n", path);
        return false;
    }
    (void)r32();
    const uint64_t nt = r64(), nk = r64();
    uint64_t align = 32;
    static const uint8_t sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    auto skip_val = [&](uint32_t t, auto &&self) -> void {
        if (t == 8) {
            const uint64_t n = r64();
            fseeko(f, (off_t)n, SEEK_CUR);
        } else if (t == 9) {
            const uint32_t et = r32();
            const uint64_t n = r64();
            if (et == 8 || et == 9) {
                for (uint64_t i = 0; i < n; i++) {
                    self(et, self);
                }
            } else {
                fseeko(f, (off_t)(n * sizes[et]), SEEK_CUR);
            }
        } else {
            fseeko(f, sizes[t], SEEK_CUR);
        }
    };
    for (uint64_t i = 0; i < nk; i++) {
        const std::string k = rstr();
        const uint32_t t = r32();
        if (k == "general.alignment") {
            align = r32();
        } else {
            skip_val(t, skip_val);
        }
    }
    uint64_t maxend = 0;
    for (uint64_t i = 0; i < nt; i++) {
        GgufTensor ti;
        ti.name = rstr();
        const uint32_t nd = r32();
        std::vector<uint64_t> d(nd);
        for (uint32_t j = 0; j < nd; j++) {
            d[j] = r64();
        }
        ti.dim0 = nd > 0 ? d[0] : 1;
        ti.dim1 = nd > 1 ? d[1] : 1;
        ti.type = r32();
        ti.offset = r64();
        g.tensors[ti.name] = ti;
        const uint64_t end = ti.offset + gg_row_bytes(ti) * ti.dim1;
        if (end > maxend) {
            maxend = end;
        }
    }
    off_t pos = ftello(f);
    pos = (pos + (off_t)align - 1) / (off_t)align * (off_t)align;
    fseeko(f, pos, SEEK_SET);
    g.data_size = maxend;
    g.data.resize(maxend);
    rd(g.data.data(), maxend);
    fclose(f);
    return true;
}

#endif /* GGUF_LITE_H */
