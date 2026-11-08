// llama_ref_driver.cpp - llama.cpp CUDA reference logits for the #475 harness.
//
// Mirrors bench/475/run-llama.sh: all layers on the GPU, context 4096, batch 64,
// greedy decode by plain argmax. Records every step's full logits so the
// harness can compare them with its own under teacher forcing.
//
//   llama_ref_driver run <model.gguf> <prompt.txt> <n_predict> <out.rec>
//       prompt.ids     tokens of the prompt, tokenized as llama-completion does
//       greedy.tokens  n_predict greedy tokens
//       greedy.logits  (1 + n_predict) rows of n_vocab: after the prompt, then
//                      after each generated token
//       greedy.n_vocab
//   llama_ref_driver detok <model.gguf> <id,id,...>
//       Prints the text of the given tokens.
//
// Build on the board (reference only, links the board's llama.cpp build):
//   L=/root/llama-475/llama.cpp
//   g++ -O2 -std=c++17 llama_ref_driver.cpp -I$L/include -I$L/ggml/include \
//       -L$L/build/bin -lllama -lggml -lggml-base -Wl,-rpath,$L/build/bin \
//       -o llama_ref_driver
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "llama.h"

typedef std::map<std::string, std::vector<uint8_t>> Records;

template <typename T> static std::vector<uint8_t> bytes_of(const std::vector<T> &v)
{
    std::vector<uint8_t> b(v.size() * sizeof(T));
    memcpy(b.data(), v.data(), b.size());
    return b;
}

// Same layout as the SLM goldens: "SGR1", count, then name and data, each
// data padded to 4 bytes.
static void write_records(const char *path, const Records &r)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite("SGR1", 1, 4, f);
    const uint32_t n = (uint32_t)r.size();
    fwrite(&n, 4, 1, f);
    for (const auto &kv : r) {
        const uint32_t nl = (uint32_t)kv.first.size(), dl = (uint32_t)kv.second.size();
        fwrite(&nl, 4, 1, f);
        fwrite(kv.first.data(), 1, nl, f);
        fwrite(&dl, 4, 1, f);
        fwrite(kv.second.data(), 1, dl, f);
        static const uint8_t pad[3] = {0, 0, 0};
        fwrite(pad, 1, ((dl + 3) & ~3u) - dl, f);
    }
    fclose(f);
}

static llama_model *load(const char *path)
{
    llama_backend_init();
    llama_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 99;
    llama_model *m = llama_model_load_from_file(path, mp);
    if (!m) {
        fprintf(stderr, "model load failed\n");
        exit(1);
    }
    return m;
}

static int cmd_run(const char *model_path, const char *prompt_path, int n_predict, const char *out)
{
    llama_model *model = load(model_path);
    const llama_vocab *vocab = llama_model_get_vocab(model);
    const int n_vocab = llama_vocab_n_tokens(vocab);

    std::ifstream in(prompt_path);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // llama-completion drops one trailing newline from a prompt file.
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    std::vector<llama_token> prompt(text.size() + 16);
    const int n = llama_tokenize(vocab, text.c_str(), (int)text.size(), prompt.data(),
                                 (int)prompt.size(), true, true);
    if (n < 0) {
        fprintf(stderr, "tokenize failed\n");
        return 1;
    }
    prompt.resize(n);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096;
    cp.n_batch = 64;
    cp.n_threads = 4;
    cp.n_threads_batch = 4;
    llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        fprintf(stderr, "context init failed\n");
        return 1;
    }

    // Prompt in batches of 64, logits for the last token only.
    for (size_t at = 0; at < prompt.size(); at += 64) {
        const int len = (int)std::min<size_t>(64, prompt.size() - at);
        llama_batch b = llama_batch_init(len, 0, 1);
        for (int i = 0; i < len; i++) {
            b.token[i] = prompt[at + i];
            b.pos[i] = (llama_pos)(at + i);
            b.n_seq_id[i] = 1;
            b.seq_id[i][0] = 0;
            b.logits[i] = (at + i + 1 == prompt.size());
        }
        b.n_tokens = len;
        if (llama_decode(ctx, b) != 0) {
            fprintf(stderr, "prompt decode failed\n");
            return 1;
        }
        llama_batch_free(b);
    }

    std::vector<int32_t> gen;
    std::vector<float> logits;
    const float *row = llama_get_logits_ith(ctx, -1);
    for (int step = 0;; step++) {
        logits.insert(logits.end(), row, row + n_vocab);
        if (step == n_predict) {
            break;
        }
        int best = 0;
        for (int i = 1; i < n_vocab; i++) {
            if (row[i] > row[best]) {
                best = i;
            }
        }
        gen.push_back(best);
        llama_token tok = best;
        if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
            fprintf(stderr, "decode failed\n");
            return 1;
        }
        row = llama_get_logits_ith(ctx, -1);
    }

    Records r;
    std::vector<int32_t> ids(prompt.begin(), prompt.end());
    r["prompt.ids"] = bytes_of(ids);
    r["greedy.tokens"] = bytes_of(gen);
    r["greedy.logits"] = bytes_of(logits);
    r["greedy.n_vocab"] = bytes_of(std::vector<int32_t>{n_vocab});
    write_records(out, r);
    printf("prompt %zu tokens, %d generated\n", prompt.size(), (int)gen.size());
    llama_free(ctx);
    llama_model_free(model);
    return 0;
}

static int cmd_detok(const char *model_path, const char *csv)
{
    llama_model *model = load(model_path);
    const llama_vocab *vocab = llama_model_get_vocab(model);
    std::stringstream ss(csv);
    std::string item;
    std::string out;
    while (std::getline(ss, item, ',')) {
        char buf[256];
        const int n = llama_token_to_piece(vocab, std::atoi(item.c_str()), buf, sizeof buf, 0, true);
        if (n > 0) {
            out.append(buf, n);
        }
    }
    fwrite(out.data(), 1, out.size(), stdout);
    printf("\n");
    llama_model_free(model);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "run")) {
        return cmd_run(argv[2], argv[3], atoi(argv[4]), argv[5]);
    }
    if (argc == 4 && !strcmp(argv[1], "detok")) {
        return cmd_detok(argv[2], argv[3]);
    }
    fprintf(stderr, "usage: %s run <model> <prompt.txt> <n> <out.rec> | detok <model> <ids>\n", argv[0]);
    return 2;
}
