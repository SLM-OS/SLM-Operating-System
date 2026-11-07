// slm-golden-driver: reference outputs for the SLM golden tests (#475).
//
// Everything this prints comes from upstream ggml / llama.cpp, never from
// SLM-OS code, so the vectors under scripts/fixtures/slm/ are an external
// reference. scripts/tools/slm-gen-goldens.py builds and drives it.
//
//   slm-golden-driver ops <in.rec> <out.rec>
//       Runs ggml's own operators over the inputs in <in.rec>.
//   slm-golden-driver greedy <model.gguf> <id,id,...> <n_predict> <out.rec>
//       Runs llama.cpp on the CPU, one token per decode, and picks each
//       next token by plain argmax over the raw logits.
//
// Record file format (shared with runtime/src/slm/golden.rs), little endian:
//   "SGR1", u32 record count, then per record:
//   u32 name_len, name bytes, u32 data_len, data bytes, pad to 4 bytes.

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ggml.h"
#include "ggml-cpu.h"
#include "llama.h"

extern "C" {
// Exported by libggml-base (ggml/src/ggml-quants.h), not in a public header.
void dequantize_row_q4_K(const void *x, float *y, int64_t k);
void dequantize_row_q6_K(const void *x, float *y, int64_t k);
void quantize_row_q8_K_ref(const float *x, void *y, int64_t k);
}

using Records = std::map<std::string, std::vector<uint8_t>>;

static Records read_records(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    std::vector<uint8_t> b;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) b.insert(b.end(), chunk, chunk + n);
    fclose(f);
    size_t p = 8;
    uint32_t count;
    memcpy(&count, b.data() + 4, 4);
    Records r;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nl, dl;
        memcpy(&nl, b.data() + p, 4); p += 4;
        std::string name((const char *)b.data() + p, nl); p += nl;
        memcpy(&dl, b.data() + p, 4); p += 4;
        r[name] = std::vector<uint8_t>(b.begin() + p, b.begin() + p + dl);
        p += (dl + 3) & ~3u;
    }
    return r;
}

static void write_records(const char *path, const Records &r) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite("SGR1", 1, 4, f);
    uint32_t count = (uint32_t)r.size();
    fwrite(&count, 4, 1, f);
    static const uint8_t zero[4] = {0, 0, 0, 0};
    for (const auto &kv : r) {
        uint32_t nl = (uint32_t)kv.first.size(), dl = (uint32_t)kv.second.size();
        fwrite(&nl, 4, 1, f);
        fwrite(kv.first.data(), 1, nl, f);
        fwrite(&dl, 4, 1, f);
        fwrite(kv.second.data(), 1, dl, f);
        fwrite(zero, 1, ((dl + 3) & ~3u) - dl, f);
    }
    fclose(f);
}

template <typename T>
static std::vector<uint8_t> bytes_of(const std::vector<T> &v) {
    const uint8_t *p = (const uint8_t *)v.data();
    return std::vector<uint8_t>(p, p + v.size() * sizeof(T));
}

static std::vector<float> floats(const Records &r, const char *name) {
    auto it = r.find(name);
    if (it == r.end()) { fprintf(stderr, "missing record %s\n", name); exit(1); }
    std::vector<float> v(it->second.size() / 4);
    memcpy(v.data(), it->second.data(), it->second.size());
    return v;
}

// A one-shot ggml graph: build with ctx, compute on one thread.
struct Graph {
    ggml_context *ctx;
    Graph() {
        ggml_init_params p = {256 * 1024 * 1024, nullptr, false};
        ctx = ggml_init(p);
    }
    ~Graph() { ggml_free(ctx); }
    void run(ggml_tensor *out) {
        ggml_cgraph *g = ggml_new_graph(ctx);
        ggml_build_forward_expand(g, out);
        if (ggml_graph_compute_with_ctx(ctx, g, 1) != GGML_STATUS_SUCCESS) {
            fprintf(stderr, "graph compute failed\n");
            exit(1);
        }
    }
};

static ggml_tensor *tensor1(Graph &g, const std::vector<float> &v) {
    ggml_tensor *t = ggml_new_tensor_1d(g.ctx, GGML_TYPE_F32, (int64_t)v.size());
    memcpy(t->data, v.data(), v.size() * 4);
    return t;
}

static std::vector<float> out_floats(ggml_tensor *t) {
    std::vector<float> v(ggml_nelements(t));
    memcpy(v.data(), t->data, v.size() * 4);
    return v;
}

// ggml_mul_mat on a quantized [cols, rows] matrix. This is how llama.cpp runs
// every projection: the activation goes through Q8_K on the fly for Q4_K and
// Q6_K alike, then the type's vec_dot.
static std::vector<float> mul_mat_q(ggml_type type, const std::vector<uint8_t> &qdata,
                                    int64_t cols, int64_t rows, const std::vector<float> &x) {
    Graph g;
    ggml_tensor *w = ggml_new_tensor_2d(g.ctx, type, cols, rows);
    if (ggml_nbytes(w) != qdata.size()) {
        fprintf(stderr, "quantized size mismatch %zu vs %zu\n", ggml_nbytes(w), qdata.size());
        exit(1);
    }
    memcpy(w->data, qdata.data(), qdata.size());
    ggml_tensor *out = ggml_mul_mat(g.ctx, w, tensor1(g, x));
    g.run(out);
    return out_floats(out);
}

static std::vector<uint8_t> quantize(ggml_type type, const std::vector<float> &src,
                                     int64_t cols, int64_t rows) {
    std::vector<uint8_t> q(ggml_row_size(type, cols) * rows);
    ggml_quantize_chunk(type, src.data(), q.data(), 0, rows, cols, nullptr);
    return q;
}

static int cmd_ops(const char *in_path, const char *out_path) {
    Records in = read_records(in_path);
    Records out;

    // RMSNorm: ggml_rms_norm then the learned scale, as build_norm(LLM_NORM_RMS).
    {
        auto x = floats(in, "rmsnorm.x");
        auto gamma = floats(in, "rmsnorm.gamma");
        Graph g;
        ggml_tensor *n = ggml_rms_norm(g.ctx, tensor1(g, x), 1e-6f);
        ggml_tensor *r = ggml_mul(g.ctx, n, tensor1(g, gamma));
        g.run(r);
        out["rmsnorm.out"] = bytes_of(out_floats(r));
    }

    // RoPE: Qwen2 is GGML_ROPE_TYPE_NEOX (llama_model_rope_type).
    {
        auto x = floats(in, "rope.x");
        auto params = floats(in, "rope.params");  // head_dim, n_head, freq_base
        auto positions = floats(in, "rope.positions");
        const int head_dim = (int)params[0], n_head = (int)params[1];
        for (size_t i = 0; i < positions.size(); i++) {
            Graph g;
            ggml_tensor *a = ggml_new_tensor_3d(g.ctx, GGML_TYPE_F32, head_dim, n_head, 1);
            memcpy(a->data, x.data(), x.size() * 4);
            ggml_tensor *pos = ggml_new_tensor_1d(g.ctx, GGML_TYPE_I32, 1);
            ((int32_t *)pos->data)[0] = (int32_t)positions[i];
            ggml_tensor *r = ggml_rope_ext(g.ctx, a, pos, nullptr, head_dim,
                                           GGML_ROPE_TYPE_NEOX, 32768, params[2], 1.0f,
                                           0.0f, 1.0f, 0.0f, 0.0f);
            g.run(r);
            out["rope.out." + std::to_string(i)] = bytes_of(out_floats(r));
        }
    }

    // SiLU(gate) * up, the element-wise heart of build_ffn(LLM_FFN_SWISH, LLM_FFN_PAR).
    {
        auto gate = floats(in, "swiglu.gate");
        auto up = floats(in, "swiglu.up");
        Graph g;
        ggml_tensor *s = ggml_silu(g.ctx, tensor1(g, gate));
        ggml_tensor *r = ggml_mul(g.ctx, s, tensor1(g, up));
        g.run(r);
        out["swiglu.out"] = bytes_of(out_floats(r));
    }

    // Dequantization and vec_dot, on one block-aligned matrix per type.
    auto q_x = floats(in, "quant.x");
    auto q_src = floats(in, "quant.src");
    auto q_dims = floats(in, "quant.dims");  // cols, rows
    const int64_t cols = (int64_t)q_dims[0], rows = (int64_t)q_dims[1];
    {
        auto q4 = quantize(GGML_TYPE_Q4_K, q_src, cols, rows);
        std::vector<float> d4(cols * rows);
        dequantize_row_q4_K(q4.data(), d4.data(), cols * rows);
        out["q4k.bytes"] = q4;
        out["q4k.dequant"] = bytes_of(d4);
        out["q4k.dot"] = bytes_of(mul_mat_q(GGML_TYPE_Q4_K, q4, cols, rows, q_x));

        auto q6 = quantize(GGML_TYPE_Q6_K, q_src, cols, rows);
        std::vector<float> d6(cols * rows);
        dequantize_row_q6_K(q6.data(), d6.data(), cols * rows);
        out["q6k.bytes"] = q6;
        out["q6k.dequant"] = bytes_of(d6);
        out["q6k.dot"] = bytes_of(mul_mat_q(GGML_TYPE_Q6_K, q6, cols, rows, q_x));

        std::vector<uint8_t> q8(ggml_row_size(GGML_TYPE_Q8_K, cols));
        quantize_row_q8_K_ref(q_x.data(), q8.data(), cols);
        out["q8k.bytes"] = q8;
    }
    write_records(out_path, out);
    return 0;
}

static int cmd_greedy(const char *model_path, const char *ids_csv, int n_predict,
                      const char *out_path) {
    llama_backend_init();
    llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) { fprintf(stderr, "model load failed\n"); return 1; }
    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    std::vector<llama_token> prompt;
    std::stringstream ss(ids_csv);
    std::string item;
    while (std::getline(ss, item, ',')) prompt.push_back(std::atoi(item.c_str()));

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)(prompt.size() + n_predict + 8);
    cp.n_batch = 1;
    cp.n_threads = 1;
    cp.n_threads_batch = 1;
    llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "context init failed\n"); return 1; }

    std::vector<int32_t> generated;
    std::vector<float> logits;  // one row of n_vocab per decode
    std::vector<float> margins;
    llama_token tok = prompt[0];
    size_t next_prompt = 1;
    for (;;) {
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
            fprintf(stderr, "decode failed\n");
            return 1;
        }
        const float *row = llama_get_logits_ith(ctx, -1);
        logits.insert(logits.end(), row, row + n_vocab);
        if (next_prompt < prompt.size()) {
            tok = prompt[next_prompt++];
            continue;
        }
        // Plain argmax, first index wins a tie, same as Sampler::Greedy.
        int best = 0;
        for (int i = 1; i < n_vocab; i++) if (row[i] > row[best]) best = i;
        float second = -INFINITY;
        for (int i = 0; i < n_vocab; i++) if (i != best && row[i] > second) second = row[i];
        margins.push_back(row[best] - second);
        generated.push_back(best);
        tok = best;
        if ((int)generated.size() == n_predict) break;
    }

    Records out;
    out["greedy.tokens"] = bytes_of(generated);
    out["greedy.logits"] = bytes_of(logits);
    out["greedy.margins"] = bytes_of(margins);
    std::vector<int32_t> nv = {n_vocab};
    out["greedy.n_vocab"] = bytes_of(nv);
    write_records(out_path, out);

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "ops")) return cmd_ops(argv[2], argv[3]);
    if (argc == 6 && !strcmp(argv[1], "greedy")) return cmd_greedy(argv[2], argv[3], atoi(argv[4]), argv[5]);
    fprintf(stderr, "usage: %s ops <in> <out> | greedy <model> <ids> <n> <out>\n", argv[0]);
    return 2;
}
