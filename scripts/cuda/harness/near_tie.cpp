/*
 * near_tie.cpp - is a greedy-text divergence a near tie? (#1167, G4)
 *
 * Two greedy runs of one prompt (say main and a branch) agree up to some token
 * and then pick different ones. This teacher-forces the prompt and the shared
 * tokens through the exact CPU reference of ADR 0004 (slm_cpu_ref.h: FP16
 * activations between ops, every dot product exact in double over the
 * dequantized weights, the GPU kernels' rounding points) in one batched pass,
 * and prints, at the divergence step, the reference's top two tokens, their
 * logit gap and the logits of the two runs' choices. For context it also
 * prints, over the shared steps, how often the reference's top token is the
 * forced one and the smallest gap seen.
 *
 *   g++ -O2 -std=c++17 -fopenmp near_tie.cpp -o near_tie
 *   ./near_tie <model.gguf> <ids.txt>
 *
 * ids.txt (near-tie.sh writes it with the runtime's tokenizer, as `slm prompt
 * --raw` encodes): three lines "prompt <ids>", "a <ids>", "b <ids>", where a
 * and b are the two runs' reply tokens.
 */
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gguf_lite.h"
#include "slm_cpu_ref.h"

static std::vector<uint32_t> read_ids(const std::map<std::string, std::string> &m, const char *k)
{
    std::vector<uint32_t> v;
    auto it = m.find(k);
    if (it == m.end()) {
        fprintf(stderr, "ids file has no '%s' line\n", k);
        exit(2);
    }
    std::istringstream s(it->second);
    uint32_t x;
    while (s >> x) {
        v.push_back(x);
    }
    return v;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <model.gguf> <ids.txt>\n", argv[0]);
        return 2;
    }
    std::map<std::string, std::string> lines;
    {
        std::ifstream f(argv[2]);
        std::string l;
        while (std::getline(f, l)) {
            const size_t sp = l.find(' ');
            if (sp != std::string::npos) {
                lines[l.substr(0, sp)] = l.substr(sp + 1);
            }
        }
    }
    const std::vector<uint32_t> prompt = read_ids(lines, "prompt");
    const std::vector<uint32_t> a = read_ids(lines, "a");
    const std::vector<uint32_t> b = read_ids(lines, "b");
    size_t d = 0;
    while (d < a.size() && d < b.size() && a[d] == b[d]) {
        d++;
    }
    if (d == a.size() || d == b.size()) {
        printf("the two replies do not diverge within their tokens\n");
        return 1;
    }

    Gguf g;
    if (!gguf_read(argv[1], g)) {
        return 2;
    }
    const int L = (int)g.block_count, H = (int)g.hidden, nhq = (int)g.nhq, nhkv = (int)g.nhkv;
    const int hd = H / nhq, HQ = nhq * hd, HV = nhkv * hd;
    auto T = [&](const std::string &n) -> const GgufTensor & { return g.tensors.at(n); };
    const int FF = (int)T("blk.0.ffn_gate.weight").dim1;
    const int V = (int)T("output.weight").dim1;
    const uint8_t *data = g.data.data();
    auto wf = [&](const std::string &n) { return reinterpret_cast<const float *>(data + T(n).offset); };

    /* Rows: the prompt, then the shared reply tokens; row r predicts the
     * token after it, so the last row predicts reply token d. */
    std::vector<uint32_t> ids = prompt;
    ids.insert(ids.end(), a.begin(), a.begin() + (long)d);
    const int B = (int)ids.size();
    std::vector<uint32_t> pos(B);
    for (int i = 0; i < B; i++) {
        pos[i] = (uint32_t)i;
    }
    printf("model %s: layers %d hidden %d heads %d/%d hd %d vocab %d\n", argv[1], L, H, nhq, nhkv,
           hd, V);
    printf("prompt %zu tokens; replies agree for %zu tokens, then a %u, b %u (reply index %zu, "
           "position %d)\n",
           prompt.size(), d, a[d], b[d], d, B);

    std::vector<float> rope;
    ref_rope_table(hd, B + 1, g.rope_base, rope);
    h16 x((size_t)B * H), xn(x.size()), q((size_t)B * HQ), k((size_t)B * HV), v(k.size()),
        att(q.size()), p(x.size()), gt((size_t)B * FF), up(gt.size()), s(gt.size()), dn(x.size());
    ref_embed(g, T("token_embd.weight"), ids.data(), B, x.data());
    for (int l = 0; l < L; l++) {
        const std::string pre = "blk." + std::to_string(l) + ".";
        ref_rmsnorm(x.data(), wf(pre + "attn_norm.weight"), g.rms_eps, H, B, xn.data());
        ref_project(g, T(pre + "attn_q.weight"), wf(pre + "attn_q.bias"), xn.data(), B, q.data());
        ref_project(g, T(pre + "attn_k.weight"), wf(pre + "attn_k.bias"), xn.data(), B, k.data());
        ref_project(g, T(pre + "attn_v.weight"), wf(pre + "attn_v.bias"), xn.data(), B, v.data());
        ref_rope(rope, pos.data(), nhq, hd, B, q.data());
        ref_rope(rope, pos.data(), nhkv, hd, B, k.data());
        /* Row i is position i, so the batch's K and V are the cache. */
        ref_gqa(q.data(), k.data(), v.data(), pos.data(), nhq, nhkv, hd, B, att.data());
        ref_project(g, T(pre + "attn_output.weight"), nullptr, att.data(), B, p.data());
        ref_add(x.data(), p.data(), x.size(), x.data());
        ref_rmsnorm(x.data(), wf(pre + "ffn_norm.weight"), g.rms_eps, H, B, xn.data());
        ref_project(g, T(pre + "ffn_gate.weight"), nullptr, xn.data(), B, gt.data());
        ref_project(g, T(pre + "ffn_up.weight"), nullptr, xn.data(), B, up.data());
        ref_swiglu(gt.data(), up.data(), gt.size(), s.data());
        ref_project(g, T(pre + "ffn_down.weight"), nullptr, s.data(), B, dn.data());
        ref_add(x.data(), dn.data(), x.size(), x.data());
        fprintf(stderr, "layer %d done\n", l);
    }
    ref_rmsnorm(x.data(), wf("output_norm.weight"), g.rms_eps, H, B, xn.data());
    /* Logits for the rows that predict reply tokens 0..d. */
    const int first = (int)prompt.size() - 1;
    const int R = B - first;
    std::vector<float> lg((size_t)R * V);
    ref_matmul(g, T("output.weight"), &xn[(size_t)first * H], R, lg.data());

    auto top2 = [&](const float *row, int &t1, int &t2) {
        t1 = 0;
        t2 = 1;
        if (row[t2] > row[t1]) {
            std::swap(t1, t2);
        }
        for (int i = 2; i < V; i++) {
            if (row[i] > row[t1]) {
                t2 = t1;
                t1 = i;
            } else if (row[i] > row[t2]) {
                t2 = i;
            }
        }
    };
    int agree = 0;
    float min_gap = 1e30f;
    int min_gap_step = -1;
    for (int r = 0; r < R - 1; r++) {
        const float *row = &lg[(size_t)r * V];
        int t1, t2;
        top2(row, t1, t2);
        agree += (uint32_t)t1 == a[r];
        if (row[t1] - row[t2] < min_gap) {
            min_gap = row[t1] - row[t2];
            min_gap_step = r;
        }
    }
    printf("shared steps 0..%zu: exact reference's top token = the runs' token at %d of %zu; "
           "smallest top-1/top-2 gap %.4f logits at reply index %d\n",
           d - 1, agree, d, min_gap, min_gap_step);
    const float *row = &lg[(size_t)(R - 1) * V];
    int t1, t2;
    top2(row, t1, t2);
    printf("divergence (reply index %zu): exact reference top-1 %d (%.4f), top-2 %d (%.4f), gap %.4f "
           "logits\n",
           d, t1, row[t1], t2, row[t2], row[t1] - row[t2]);
    printf("  a chose %u: logit %.4f (%s)\n", a[d], row[a[d]],
           (int)a[d] == t1 ? "reference top-1" : (int)a[d] == t2 ? "reference top-2" : "neither");
    printf("  b chose %u: logit %.4f (%s)\n", b[d], row[b[d]],
           (int)b[d] == t1 ? "reference top-1" : (int)b[d] == t2 ? "reference top-2" : "neither");
    printf("  |logit(a) - logit(b)| = %.4f\n", fabsf(row[a[d]] - row[b[d]]));
    return 0;
}
