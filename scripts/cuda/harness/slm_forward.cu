/*
 * slm_forward.cu - whole-forward simulation of the #475 GPU program under
 * JetPack, all kernels in one CUDA graph over real weights, plus per-kernel
 * checks against the scalar CPU reference in slm_cpu_ref.h.
 *
 * Build on the board:
 *   nvcc -O3 -std=c++17 -arch=sm_87 -Xcompiler -fopenmp -lgomp -I../slm \
 *        slm_forward.cu -o slm_forward
 * Run:
 *   ./slm_forward <model.gguf> [--golden tiny.rec] [--simt] [--no-check]
 *                 [--prefill N] [--decode N] [--ctx N]
 *
 * Reports decode tok/s and prefill ms (graph time, and with logit readback),
 * a per-op table, the DRAM read rate implied, and the two tensor-core
 * utilization figures: mma issue cycles over matmul wall time, and the share
 * of matmul MACs done on tensor cores.
 */
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../slm/slm_mm_q4k.cu"
#include "../slm/slm_mm_q6k.cu"
#include "../slm/slm_mm_b1.cu"
#include "../slm/slm_mm_bs.cu"
#include "../slm/slm_ops.cu"
#include "gguf_lite.h"
#include "slm_cpu_ref.h"

#define CK(call)                                                              \
    do {                                                                      \
        cudaError_t _e = (call);                                              \
        if (_e != cudaSuccess) {                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,                \
                    cudaGetErrorString(_e));                                  \
            exit(3);                                                          \
        }                                                                     \
    } while (0)

static double now_ms()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

static double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/* ---- mma issue rate: saturate every warp with mma and nothing else ---- */

__global__ void cpi_mma(float *out, int reps)
{
    const uint32_t a = 0x3C003C00u;
    float c0 = 0.f, c1 = 0.f, c2 = 0.f, c3 = 0.f;
    __syncwarp();
    for (int i = 0; i < reps; i++) {
        slm_mma(c0, c1, c2, c3, a, a, a, a, a, a);
    }
    out[blockIdx.x * 32 + threadIdx.x] = c0 + c1 + c2 + c3;
}

/* ---- one program: a flat op table launched singly or as a graph ---- */

struct Op {
    std::string name;
    std::string cls; /* class for the per-op table */
    void *func;
    dim3 grid;
    dim3 block = dim3(32);
    std::vector<void *> params;
    double mma = 0;    /* mma instructions */
    double macs = 0;   /* matmul MACs (tensor-core or SIMT) */
    double bytes = 0;  /* weight bytes read */
    bool tc = false;   /* runs on tensor cores */
};

struct Program {
    std::vector<Op> ops;
    std::deque<std::vector<uint8_t>> store;
    cudaGraphExec_t exec = nullptr;

    template <typename T> void *keep(const T &v)
    {
        store.emplace_back(sizeof(T));
        memcpy(store.back().data(), &v, sizeof(T));
        return store.back().data();
    }

    void add_op(const std::string &name, const std::string &cls, void *f, dim3 grid)
    {
        Op o;
        o.name = name;
        o.cls = cls;
        o.func = f;
        o.grid = grid;
        ops.push_back(o);
    }

    template <typename T> void arg(const T &v) { ops.back().params.push_back(keep(v)); }

    void run_op(size_t i)
    {
        Op &o = ops[i];
        CK(cudaLaunchKernel(o.func, o.grid, o.block, o.params.data(), 0, nullptr));
    }

    void build_graph(double *inst_ms)
    {
        cudaGraph_t graph;
        CK(cudaGraphCreate(&graph, 0));
        cudaGraphNode_t prev = nullptr;
        for (Op &o : ops) {
            cudaKernelNodeParams p = {};
            p.func = o.func;
            p.gridDim = o.grid;
            p.blockDim = o.block;
            p.sharedMemBytes = 0;
            p.kernelParams = o.params.data();
            p.extra = nullptr;
            cudaGraphNode_t n;
            CK(cudaGraphAddKernelNode(&n, graph, prev ? &prev : nullptr, prev ? 1 : 0, &p));
            prev = n;
        }
        const double s = now_ms();
        CK(cudaGraphInstantiate(&exec, graph));
        *inst_ms = now_ms() - s;
        CK(cudaGraphDestroy(graph));
    }
};

/* ---- session arena on the device ---- */

struct Arena {
    uint8_t *w;
    float *run; /* running FP32 sum across the K ranges of a b64 matmul */
    half *x, *xn, *q, *k, *v, *a, *p, *g, *u, *s, *d;
    std::vector<half *> K, V;
    float *table, *logits;
    uint32_t *ids, *pos;
};

struct Model {
    Gguf g;
    int layers, hidden, ff, nhq, nhkv, hd, vocab, ctx;
    float eps;
    std::vector<float> rope;
    const GgufTensor &t(const std::string &n) const { return g.tensors.at(n); }
};

static bool g_simt = false;
static bool g_b64 = false;

static int mm_grid(int N) { return (N + Q4K_TILE_ROWS - 1) / Q4K_TILE_ROWS; }

/* Warps per CTA for a B=1 matmul: enough tiles to fill the SMs, never more
 * warps than weight blocks. */
static int mm_nw(int K, int N)
{
    const int want = (200 + mm_grid(N) - 1) / mm_grid(N);
    return std::max(1, std::min(std::min(8, K / 256), want));
}

static void add_mm(Program &P, const Model &M, const Arena &A, const std::string &name,
                   const std::string &cls, const GgufTensor &t, const float *bias, const half *x,
                   void *out, int B, int flags)
{
    const int K = (int)t.dim0, N = (int)t.dim1;
    const bool q6 = t.type == GGML_Q6_K;
    if (t.type != GGML_Q4_K && !q6) {
        fprintf(stderr, "%s: unsupported type %u\n", name.c_str(), t.type);
        exit(2);
    }
    void *f;
    dim3 grid(mm_grid(N));
    int nw = 1;
    bool ranged = false;
    if (g_simt) {
        f = q6 ? (void *)slm_mm_q6k_simt : (void *)slm_mm_q4k_simt;
        grid = dim3(N, B, 1);
    } else if (B == 1) {
        f = q6 ? (void *)slm_mm_q6k_hmma_b1 : (void *)slm_mm_q4k_hmma_b1;
        nw = mm_nw(K, N);
    } else if (B <= 8) {
        f = q6 ? (void *)slm_mm_q6k_hmma_b8 : (void *)slm_mm_q4k_hmma_b8;
    } else {
        f = q6 ? (void *)slm_mm_q6k_hmma_b64 : (void *)slm_mm_q4k_hmma_b64;
        ranged = true;
    }
    /* Prefill path. Big matmuls share x through shared memory; small ones have too
     * few CTAs to fill the SMs that way and split per x tile instead. */
    if (ranged && N > 256 && !g_b64) {
        const dim3 g2((N + 119) / 120);
        P.add_op(name, cls, q6 ? (void *)slm_mm_q6k_hmma_bs : (void *)slm_mm_q4k_hmma_bs, g2);
        Op &o = P.ops.back();
        o.block = dim3(32, 8, 1);
        o.mma = (double)mm_grid(N) * (K / 256) * 16.0 * 8;
        o.macs = (double)N * K * B;
        o.bytes = (double)gg_row_bytes(t) * N;
        o.tc = true;
        P.arg(x);
        P.arg((const uint8_t *)(A.w + t.offset));
        P.arg(out);
        P.arg(bias);
        P.arg(K);
        P.arg(N);
        P.arg(B);
        P.arg(flags);
        return;
    }
    /* Small N: low-register per-tile warps. */
    if (ranged && N <= 256 && !g_b64) {
        const int xtiles = (B + 7) >> 3;
        P.add_op(name, cls, q6 ? (void *)slm_mm_q6k_hmma_b8 : (void *)slm_mm_q4k_hmma_b8,
                 dim3(mm_grid(N), xtiles, 1));
        Op &o = P.ops.back();
        o.mma = (double)mm_grid(N) * (K / 256) * 16.0 * xtiles;
        o.macs = (double)N * K * B;
        o.bytes = (double)gg_row_bytes(t) * N;
        o.tc = true;
        P.arg(x);
        P.arg((const uint8_t *)(A.w + t.offset));
        P.arg(out);
        P.arg(bias);
        P.arg(K);
        P.arg(N);
        P.arg(B);
        P.arg(flags | SLM_MM_SPLIT_XT);
        return;
    }
    if (ranged) {
        const int xtiles = (B + 7) >> 3;
        const int nb = K / 256;
        /* Whole K: ranges fit x in the L1 but measured slower overall (running-sum
         * reloads, cold starts). The kernel keeps them; see ADR 0003. */
        const int per = nb;
        const int split = N <= 256 ? SLM_MM_SPLIT_XT : 0;
        const dim3 g2(mm_grid(N), split ? xtiles : 1, 1);
        const int ranges = (nb + per - 1) / per;
        for (int r = 0; r < ranges; r++) {
            const int start = r * per, count = std::min(per, nb - start);
            const bool last = r == ranges - 1;
            P.add_op(name, cls, f, g2);
            Op &o = P.ops.back();
            o.mma = (double)mm_grid(N) * count * 16.0 * xtiles;
            o.macs = (double)N * count * 256 * B;
            o.bytes = (double)gg_row_bytes(t) * N * count / nb;
            o.tc = true;
            P.arg(x);
            P.arg((const uint8_t *)(A.w + t.offset));
            P.arg(last ? out : (void *)A.run);
            P.arg(r == 0 ? bias : (const float *)A.run);
            P.arg(K);
            P.arg(N);
            P.arg(B);
            P.arg((last ? flags : 0) | (r ? SLM_MM_BIAS_RUNNING : 0) | split | (start << 8) |
                  (count << 16));
        }
        return;
    }
    P.add_op(name, cls, f, grid);
    Op &o = P.ops.back();
    o.block = dim3(32, nw, 1);
    flags |= nw << 8;
    const int xt = (B + 7) >> 3;
    const double nb = K / 256.0;
    /* Q4_K: 4 passes (2 chunks x 2 nibbles) per j, 4 j per block.
     * Q6_K: 8 per half (2 chunks x 4 groups), 2 halves per block. */
    o.mma = g_simt ? 0 : (double)grid.x * nb * 16.0 * xt;
    o.macs = (double)N * K * B;
    o.bytes = (double)gg_row_bytes(t) * N;
    o.tc = !g_simt;
    P.arg(x);
    P.arg((const uint8_t *)(A.w + t.offset));
    P.arg(out);
    P.arg(bias);
    P.arg(K);
    P.arg(N);
    P.arg(B);
    P.arg(flags);
}

static const float *wf(const Arena &A, const GgufTensor &t)
{
    return reinterpret_cast<const float *>(A.w + t.offset);
}

static void add_rmsnorm(Program &P, const Model &M, const Arena &A, const std::string &name,
                        const GgufTensor &gt, const half *x, half *out, int B)
{
    P.add_op(name, "rmsnorm", (void *)slm_rmsnorm_w8, dim3(B));
    P.ops.back().block = dim3(32, 8, 1);
    P.arg(x);
    P.arg(wf(A, gt));
    P.arg(out);
    P.arg(M.hidden);
    P.arg(B);
    P.arg(M.eps);
}

static void add_add(Program &P, const std::string &name, const half *a, const half *b, half *out,
                    int total)
{
    P.add_op(name, "add", (void *)slm_add_w8, dim3((total + 255) / 256));
    P.ops.back().block = dim3(256);
    P.arg(a);
    P.arg(b);
    P.arg(out);
    P.arg(total);
}

static void add_rope(Program &P, const Model &M, const Arena &A, const std::string &name, half *x,
                     int nh, int B)
{
    P.add_op(name, "rope", (void *)slm_rope, dim3(nh, B, 1));
    P.arg((const half *)x);
    P.arg(x);
    P.arg((const float *)A.table);
    P.arg((const uint32_t *)A.pos);
    P.arg(nh);
    P.arg(M.hd);
    P.arg(B);
}

static void add_kv(Program &P, const std::string &name, const half *src, half *kv,
                   const uint32_t *pos, int width, int B)
{
    P.add_op(name, "kv_append", (void *)slm_kv_append, dim3(width / 32, B, 1));
    P.arg(src);
    P.arg(kv);
    P.arg(pos);
    P.arg(width);
    P.arg(B);
}

static void build_program(Program &P, const Model &M, const Arena &A, int B)
{
    const int H = M.hidden, HV = M.nhkv * M.hd;
    P.add_op("embed", "embed",
             M.t("token_embd.weight").type == GGML_Q6_K ? (void *)slm_embed_q6k : (void *)slm_embed_q4k,
             dim3(B));
    P.arg((const uint32_t *)A.ids);
    P.arg((const uint8_t *)(A.w + M.t("token_embd.weight").offset));
    P.arg(A.x);
    P.arg(H);
    P.arg(B);
    for (int l = 0; l < M.layers; l++) {
        const std::string b = "blk." + std::to_string(l) + ".";
        add_rmsnorm(P, M, A, b + "attn_norm", M.t(b + "attn_norm.weight"), A.x, A.xn, B);
        add_mm(P, M, A, b + "attn_q", "matmul 1536x1536", M.t(b + "attn_q.weight"),
               wf(A, M.t(b + "attn_q.bias")), A.xn, A.q, B, SLM_MM_OUT_F16);
        add_mm(P, M, A, b + "attn_k", "matmul 1536x256", M.t(b + "attn_k.weight"),
               wf(A, M.t(b + "attn_k.bias")), A.xn, A.k, B, SLM_MM_OUT_F16);
        add_mm(P, M, A, b + "attn_v", "matmul 1536x256", M.t(b + "attn_v.weight"),
               wf(A, M.t(b + "attn_v.bias")), A.xn, A.v, B, SLM_MM_OUT_F16);
        add_rope(P, M, A, b + "rope_q", A.q, M.nhq, B);
        add_rope(P, M, A, b + "rope_k", A.k, M.nhkv, B);
        add_kv(P, b + "kv_k", A.k, A.K[l], A.pos, HV, B);
        add_kv(P, b + "kv_v", A.v, A.V[l], A.pos, HV, B);
        P.add_op(b + "gqa", "gqa", (void *)slm_gqa_w8, dim3(M.nhq, B, 1));
        P.ops.back().block = dim3(32, 8, 1);
        P.arg((const half *)A.q);
        P.arg((const half *)A.K[l]);
        P.arg((const half *)A.V[l]);
        P.arg((const uint32_t *)A.pos);
        P.arg(A.a);
        P.arg(M.nhq);
        P.arg(M.nhkv);
        P.arg(M.hd);
        P.arg(B);
        add_mm(P, M, A, b + "attn_out", "matmul 1536x1536", M.t(b + "attn_output.weight"), nullptr,
               A.a, A.p, B, SLM_MM_OUT_F16);
        add_add(P, b + "res_attn", A.x, A.p, A.x, B * H);
        add_rmsnorm(P, M, A, b + "ffn_norm", M.t(b + "ffn_norm.weight"), A.x, A.xn, B);
        add_mm(P, M, A, b + "ffn_gate", "matmul 1536x8960", M.t(b + "ffn_gate.weight"), nullptr,
               A.xn, A.g, B, SLM_MM_OUT_F16);
        add_mm(P, M, A, b + "ffn_up", "matmul 1536x8960", M.t(b + "ffn_up.weight"), nullptr, A.xn,
               A.u, B, SLM_MM_OUT_F16);
        P.add_op(b + "swiglu", "swiglu", (void *)slm_swiglu_w8, dim3((B * M.ff + 255) / 256));
        P.ops.back().block = dim3(256);
        P.arg((const half *)A.g);
        P.arg((const half *)A.u);
        P.arg(A.s);
        P.arg(B * M.ff);
        add_mm(P, M, A, b + "ffn_down", "matmul 8960x1536", M.t(b + "ffn_down.weight"), nullptr,
               A.s, A.d, B, SLM_MM_OUT_F16);
        add_add(P, b + "res_ffn", A.x, A.d, A.x, B * H);
    }
    add_rmsnorm(P, M, A, "output_norm", M.t("output_norm.weight"), A.x, A.xn, B);
    /* lm_head on the last batch row only. */
    add_mm(P, M, A, "lm_head", "lm_head", M.t("output.weight"), nullptr, A.xn + (size_t)(B - 1) * H,
           A.logits, 1, 0);
}

/* ---- CPU reference for one step, keeps x after every layer ---- */

struct RefState {
    std::vector<h16> K, V;
};

static void ref_step(const Model &M, RefState &R, const uint32_t *ids, const uint32_t *pos, int B,
                     std::vector<h16> *layer_x, std::vector<float> &logits)
{
    const int H = M.hidden, HQ = M.nhq * M.hd, HV = M.nhkv * M.hd;
    const Gguf &g = M.g;
    h16 x((size_t)B * H), xn(x.size()), q((size_t)B * HQ), k((size_t)B * HV), v(k.size()),
        a(q.size()), p(x.size()), gt((size_t)B * M.ff), up(gt.size()), s(gt.size()), d(x.size());
    const uint8_t *data = g.data.data();
    auto wfc = [&](const std::string &n) {
        return reinterpret_cast<const float *>(data + M.t(n).offset);
    };
    ref_embed(g, M.t("token_embd.weight"), ids, B, x.data());
    if (layer_x) {
        layer_x->clear();
    }
    for (int l = 0; l < M.layers; l++) {
        const std::string b = "blk." + std::to_string(l) + ".";
        ref_rmsnorm(x.data(), wfc(b + "attn_norm.weight"), M.eps, H, B, xn.data());
        ref_project(g, M.t(b + "attn_q.weight"), wfc(b + "attn_q.bias"), xn.data(), B, q.data());
        ref_project(g, M.t(b + "attn_k.weight"), wfc(b + "attn_k.bias"), xn.data(), B, k.data());
        ref_project(g, M.t(b + "attn_v.weight"), wfc(b + "attn_v.bias"), xn.data(), B, v.data());
        ref_rope(M.rope, pos, M.nhq, M.hd, B, q.data());
        ref_rope(M.rope, pos, M.nhkv, M.hd, B, k.data());
        for (int i = 0; i < B; i++) {
            memcpy(&R.K[l][(size_t)pos[i] * HV], &k[(size_t)i * HV], (size_t)HV * 2);
            memcpy(&R.V[l][(size_t)pos[i] * HV], &v[(size_t)i * HV], (size_t)HV * 2);
        }
        ref_gqa(q.data(), R.K[l].data(), R.V[l].data(), pos, M.nhq, M.nhkv, M.hd, B, a.data());
        ref_project(g, M.t(b + "attn_output.weight"), nullptr, a.data(), B, p.data());
        ref_add(x.data(), p.data(), x.size(), x.data());
        ref_rmsnorm(x.data(), wfc(b + "ffn_norm.weight"), M.eps, H, B, xn.data());
        ref_project(g, M.t(b + "ffn_gate.weight"), nullptr, xn.data(), B, gt.data());
        ref_project(g, M.t(b + "ffn_up.weight"), nullptr, xn.data(), B, up.data());
        ref_swiglu(gt.data(), up.data(), gt.size(), s.data());
        ref_project(g, M.t(b + "ffn_down.weight"), nullptr, s.data(), B, d.data());
        ref_add(x.data(), d.data(), x.size(), x.data());
        if (layer_x) {
            layer_x->push_back(x);
        }
    }
    ref_rmsnorm(x.data(), wfc("output_norm.weight"), M.eps, H, B, xn.data());
    logits.resize(M.vocab);
    ref_matmul(g, M.t("output.weight"), &xn[(size_t)(B - 1) * H], 1, logits.data());
}

/* ---- per-kernel checks: identical inputs, compare one op's output ---- */

/* Error of FP16 output against reference, in units of the reference FP16 ULP. */
static double ulp_err(uint16_t got, uint16_t want)
{
    const float g = gg_f16_to_f32(got), w = gg_f16_to_f32(want);
    const float a = fabsf(w);
    int e;
    frexpf(a < 6.103515625e-05f ? 6.103515625e-05f : a, &e);
    const float ulp = ldexpf(1.0f, e - 11);
    return fabsf(g - w) / ulp;
}

static int g_fails = 0;

static void report_h16(const char *name, const std::vector<uint16_t> &got, const uint16_t *want,
                       double tol_ulp)
{
    double worst = 0;
    double sum = 0;
    for (size_t i = 0; i < got.size(); i++) {
        const double e = ulp_err(got[i], want[i]);
        worst = std::max(worst, e);
        sum += e;
    }
    const bool ok = worst <= tol_ulp;
    printf("  %-26s max %8.2f ULP16, mean %6.3f, tol %.1f: %s\n", name, worst,
           sum / got.size(), tol_ulp, ok ? "PASS" : "FAIL");
    if (!ok) {
        g_fails++;
    }
}

template <typename T> static void up(T *dst, const std::vector<T> &v)
{
    CK(cudaMemcpy(dst, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice));
}

template <typename T> static std::vector<T> down(const T *src, size_t n)
{
    std::vector<T> v(n);
    CK(cudaDeviceSynchronize());
    CK(cudaMemcpy(v.data(), src, n * sizeof(T), cudaMemcpyDeviceToHost));
    return v;
}

static h16 h_from(const half *p, size_t n)
{
    std::vector<half> tmp = down(p, n);
    h16 out(n);
    memcpy(out.data(), tmp.data(), n * 2);
    return out;
}

static void h_up(half *d, const h16 &v)
{
    CK(cudaMemcpy(d, v.data(), v.size() * 2, cudaMemcpyHostToDevice));
}

static void check_kernels(const Model &M, Arena &A)
{
    printf("\nper-kernel checks against the CPU reference (real weights):\n");
    const int H = M.hidden;
    std::mt19937 rng(475);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    auto rand_h = [&](size_t n, float scale) {
        h16 v(n);
        for (size_t i = 0; i < n; i++) {
            v[i] = gg_f32_to_f16(scale * nd(rng));
        }
        return v;
    };

    /* Matmul: 1 row-of-rows tolerance is relative to the output's RMS. FP32
     * accumulation of FP16 products over 1536 terms stays well inside 1e-3. */
    struct MC {
        std::string tensor;
    };
    std::vector<MC> mcs = {{"blk.0.attn_q.weight"}, {"blk.0.attn_k.weight"},
                           {"blk.0.ffn_gate.weight"}};
    for (int l = 0; l < M.layers; l++) {
        const std::string b = "blk." + std::to_string(l) + ".";
        if (M.t(b + "attn_v.weight").type == GGML_Q6_K) {
            mcs.push_back({b + "attn_v.weight"});
            break;
        }
    }
    for (int want : {GGML_Q4_K, GGML_Q6_K}) {
        for (int l = 0; l < M.layers; l++) {
            const std::string n = "blk." + std::to_string(l) + ".ffn_down.weight";
            if ((int)M.t(n).type == want) {
                mcs.push_back({n});
                break;
            }
        }
    }
    mcs.push_back({"output.weight"});

    const float *bias_none = nullptr;
    for (const MC &m : mcs) {
        const GgufTensor &t = M.t(m.tensor);
        const int K = (int)t.dim0, N = (int)t.dim1;
        const bool is_head = m.tensor == "output.weight";
        for (int B : {1, 5, 8, 37, 64}) {
            if (is_head && B > 1) {
                continue;
            }
            const h16 x = rand_h((size_t)B * K, 1.0f);
            std::vector<float> ref((size_t)B * N);
            ref_matmul(M.g, t, x.data(), B, ref.data());
            double rms = 0;
            for (float v : ref) {
                rms += (double)v * v;
            }
            rms = sqrt(rms / ref.size());

            half *dx;
            float *dout;
            CK(cudaMalloc(&dx, x.size() * 2));
            CK(cudaMalloc(&dout, ref.size() * 4));
            h_up(dx, x);

            std::vector<std::pair<const char *, void *>> variants;
            const bool q6 = t.type == GGML_Q6_K;
            if (B == 1) {
                variants.push_back({"hmma_b1", q6 ? (void *)slm_mm_q6k_hmma_b1 : (void *)slm_mm_q4k_hmma_b1});
                variants.push_back({"hmma_b1w", q6 ? (void *)slm_mm_q6k_hmma_b1 : (void *)slm_mm_q4k_hmma_b1});
            }
            if (B <= 8) {
                variants.push_back({"hmma_b8", q6 ? (void *)slm_mm_q6k_hmma_b8 : (void *)slm_mm_q4k_hmma_b8});
            }
            variants.push_back({"hmma_b64", q6 ? (void *)slm_mm_q6k_hmma_b64 : (void *)slm_mm_q4k_hmma_b64});
            if (B <= 8 && !is_head) {
                variants.push_back({"simt", q6 ? (void *)slm_mm_q6k_simt : (void *)slm_mm_q4k_simt});
            }
            for (auto &var : variants) {
                const bool simt = !strcmp(var.first, "simt");
                const dim3 grid = simt ? dim3(N, B, 1) : dim3(mm_grid(N));
                const bool wide = !strcmp(var.first, "hmma_b1w");
                const int nw = wide ? std::max(2, mm_nw(K, N)) : 1;
                if (wide && K / 256 < 2) {
                    continue;
                }
                int flags = wide ? nw << 8 : 0;
                const uint8_t *wp = A.w + t.offset;
                void *params[] = {&dx, &wp, &dout, &bias_none, (void *)&K, (void *)&N, &B, &flags};
                CK(cudaLaunchKernel(var.second, grid, dim3(32, nw, 1), params, 0, nullptr));
                const std::vector<float> got = down(dout, ref.size());
                double worst = 0;
                for (size_t i = 0; i < ref.size(); i++) {
                    worst = std::max(worst, fabs((double)got[i] - ref[i]));
                }
                const bool ok = worst <= 1e-3 * rms;
                printf("  %-26s %-8s B=%-2d max_err %.2e (%.1e of rms), tol 1e-3: %s\n",
                       m.tensor.c_str(), var.first, B, worst, worst / rms, ok ? "PASS" : "FAIL");
                if (!ok) {
                    g_fails++;
                }
            }
            /* b8 per x tile, for small N in prefill. */
            if (B > 8 && !is_head) {
                int flags = SLM_MM_SPLIT_XT;
                const uint8_t *wp = A.w + t.offset;
                void *f = q6 ? (void *)slm_mm_q6k_hmma_b8 : (void *)slm_mm_q4k_hmma_b8;
                void *params[] = {&dx, &wp, &dout, &bias_none, (void *)&K, (void *)&N, &B, &flags};
                CK(cudaLaunchKernel(f, dim3(mm_grid(N), (B + 7) >> 3, 1), dim3(32), params, 0, nullptr));
                const std::vector<float> got = down(dout, ref.size());
                double worst = 0;
                for (size_t i = 0; i < ref.size(); i++) {
                    worst = std::max(worst, fabs((double)got[i] - ref[i]));
                }
                const bool ok = worst <= 1e-3 * rms;
                printf("  %-26s %-8s B=%-2d max_err %.2e (%.1e of rms), tol 1e-3: %s\n",
                       m.tensor.c_str(), "b8 xt", B, worst, worst / rms, ok ? "PASS" : "FAIL");
                if (!ok) {
                    g_fails++;
                }
            }
            /* Shared-memory x kernel. */
            if (B > 8 && !is_head) {
                int flags = 0;
                const uint8_t *wp = A.w + t.offset;
                void *f = q6 ? (void *)slm_mm_q6k_hmma_bs : (void *)slm_mm_q4k_hmma_bs;
                void *params[] = {&dx, &wp, &dout, &bias_none, (void *)&K, (void *)&N, &B, &flags};
                CK(cudaLaunchKernel(f, dim3((N + 119) / 120), dim3(32, 8, 1), params, 0, nullptr));
                const std::vector<float> got = down(dout, ref.size());
                double worst = 0;
                for (size_t i = 0; i < ref.size(); i++) {
                    worst = std::max(worst, fabs((double)got[i] - ref[i]));
                }
                const bool ok = worst <= 1e-3 * rms;
                printf("  %-26s %-8s B=%-2d max_err %.2e (%.1e of rms), tol 1e-3: %s\n",
                       m.tensor.c_str(), "hmma_bs", B, worst, worst / rms, ok ? "PASS" : "FAIL");
                if (!ok) {
                    g_fails++;
                }
            }
            /* b64 in K ranges through a running sum, and split per x tile. */
            if (!is_head && B > 8) {
                const int nb = K / 256;
                const int per = std::max(1, nb / 3);
                const uint8_t *wp = A.w + t.offset;
                void *f = q6 ? (void *)slm_mm_q6k_hmma_b64 : (void *)slm_mm_q4k_hmma_b64;
                for (int split = 0; split <= SLM_MM_SPLIT_XT; split += SLM_MM_SPLIT_XT) {
                    const dim3 grid(mm_grid(N), split ? (B + 7) >> 3 : 1, 1);
                    for (int st = 0; st < nb; st += per) {
                        const int count = std::min(per, nb - st);
                        int flags = (st ? SLM_MM_BIAS_RUNNING : 0) | split | (st << 8) | (count << 16);
                        const float *bp = st ? dout : nullptr;
                        void *params[] = {&dx, &wp, &dout, &bp, (void *)&K, (void *)&N, &B, &flags};
                        CK(cudaLaunchKernel(f, grid, dim3(32), params, 0, nullptr));
                    }
                    const std::vector<float> got = down(dout, ref.size());
                    double worst = 0;
                    for (size_t i = 0; i < ref.size(); i++) {
                        worst = std::max(worst, fabs((double)got[i] - ref[i]));
                    }
                    const bool ok = worst <= 1e-3 * rms;
                    printf("  %-26s %-8s B=%-2d max_err %.2e (%.1e of rms), tol 1e-3: %s\n",
                           m.tensor.c_str(), split ? "b64 xt" : "b64 rng", B, worst, worst / rms,
                           ok ? "PASS" : "FAIL");
                    if (!ok) {
                        g_fails++;
                    }
                }
            }
            CK(cudaFree(dx));
            CK(cudaFree(dout));
        }
    }

    /* Bias and FP16 output path of the matmul. */
    {
        const GgufTensor &t = M.t("blk.0.attn_k.weight");
        const int B = 3;
        const h16 x = rand_h((size_t)B * t.dim0, 1.0f);
        h16 ref((size_t)B * t.dim1);
        const float *bias = reinterpret_cast<const float *>(M.g.data.data() +
                                                            M.t("blk.0.attn_k.bias").offset);
        ref_project(M.g, t, bias, x.data(), B, ref.data());
        h_up(A.xn, x);
        const int K = (int)t.dim0, N = (int)t.dim1, fl = SLM_MM_OUT_F16;
        const uint8_t *wp = A.w + t.offset;
        const float *db = wf(A, M.t("blk.0.attn_k.bias"));
        const half *in = A.xn;
        void *out = A.k;
        void *params[] = {&in, &wp, &out, &db, (void *)&K, (void *)&N, (void *)&B, (void *)&fl};
        CK(cudaLaunchKernel(t.type == GGML_Q6_K ? (void *)slm_mm_q6k_hmma_b8 : (void *)slm_mm_q4k_hmma_b8,
                            dim3(mm_grid(N)), dim3(32), params, 0, nullptr));
        report_h16("matmul bias+FP16 B=3", h_from(A.k, ref.size()), ref.data(), 3.0);
    }

    /* Embedding. */
    {
        const int B = 7;
        std::vector<uint32_t> ids(B);
        for (int i = 0; i < B; i++) {
            ids[i] = (uint32_t)(rng() % M.vocab);
        }
        up(A.ids, ids);
        h16 ref((size_t)B * H);
        ref_embed(M.g, M.t("token_embd.weight"), ids.data(), B, ref.data());
        const uint32_t *ip = A.ids;
        const uint8_t *wp = A.w + M.t("token_embd.weight").offset;
        half *o = A.x;
        void *params[] = {&ip, &wp, &o, (void *)&H, (void *)&B};
        CK(cudaLaunchKernel(M.t("token_embd.weight").type == GGML_Q6_K ? (void *)slm_embed_q6k
                                                                        : (void *)slm_embed_q4k,
                            dim3(B), dim3(32), params, 0, nullptr));
        report_h16("embed B=7", h_from(A.x, ref.size()), ref.data(), 1.0);
    }

    /* RMSNorm with real gamma. */
    {
        const int B = 5;
        const h16 x = rand_h((size_t)B * H, 3.0f);
        h16 ref(x.size());
        const float *gamma =
            reinterpret_cast<const float *>(M.g.data.data() + M.t("blk.0.attn_norm.weight").offset);
        ref_rmsnorm(x.data(), gamma, M.eps, H, B, ref.data());
        h_up(A.x, x);
        const half *in = A.x;
        const float *gp = wf(A, M.t("blk.0.attn_norm.weight"));
        half *o = A.xn;
        float eps = M.eps;
        void *params[] = {&in, &gp, &o, (void *)&H, (void *)&B, &eps};
        CK(cudaLaunchKernel((void *)slm_rmsnorm, dim3(B), dim3(32), params, 0, nullptr));
        report_h16("rmsnorm B=5", h_from(A.xn, ref.size()), ref.data(), 2.0);
        CK(cudaLaunchKernel((void *)slm_rmsnorm_w8, dim3(B), dim3(32, 8, 1), params, 0, nullptr));
        report_h16("rmsnorm_w8 B=5", h_from(A.xn, ref.size()), ref.data(), 2.0);
    }

    /* Residual add. */
    {
        const int total = 3 * H;
        const h16 a = rand_h(total, 2.0f), b = rand_h(total, 2.0f);
        h16 ref(total);
        ref_add(a.data(), b.data(), total, ref.data());
        h_up(A.x, a);
        h_up(A.p, b);
        const half *pa = A.x, *pb = A.p;
        half *o = A.q;
        int tt = total;
        void *params[] = {&pa, &pb, &o, &tt};
        CK(cudaLaunchKernel((void *)slm_add, dim3((total + 31) / 32), dim3(32), params, 0, nullptr));
        report_h16("add", h_from(A.q, ref.size()), ref.data(), 1.0);
        CK(cudaLaunchKernel((void *)slm_add_w8, dim3((total + 255) / 256), dim3(256), params, 0, nullptr));
        report_h16("add_w8", h_from(A.q, ref.size()), ref.data(), 1.0);
    }

    /* Standalone bias add. */
    {
        const int B = 3;
        const int n = M.hidden;
        const h16 a = rand_h((size_t)B * n, 2.0f);
        const float *bias =
            reinterpret_cast<const float *>(M.g.data.data() + M.t("blk.0.attn_q.bias").offset);
        h16 ref(a.size());
        for (int b = 0; b < B; b++) {
            for (int i = 0; i < n; i++) {
                ref[(size_t)b * n + i] =
                    gg_f32_to_f16(gg_f16_to_f32(a[(size_t)b * n + i]) + bias[i]);
            }
        }
        h_up(A.x, a);
        const half *pa = A.x;
        const float *pb = wf(A, M.t("blk.0.attn_q.bias"));
        half *o = A.q;
        void *params[] = {&pa, &pb, &o, (void *)&n, (void *)&B};
        CK(cudaLaunchKernel((void *)slm_add_bias, dim3(n / 32, B, 1), dim3(32), params, 0, nullptr));
        report_h16("add_bias B=3", h_from(A.q, ref.size()), ref.data(), 1.0);
    }

    /* SwiGLU. */
    {
        const int total = 2 * M.ff;
        const h16 g = rand_h(total, 2.0f), u = rand_h(total, 2.0f);
        h16 ref(total);
        ref_swiglu(g.data(), u.data(), total, ref.data());
        h_up(A.g, g);
        h_up(A.u, u);
        const half *pg = A.g, *pu = A.u;
        half *o = A.s;
        int tt = total;
        void *params[] = {&pg, &pu, &o, &tt};
        CK(cudaLaunchKernel((void *)slm_swiglu, dim3((total + 31) / 32), dim3(32), params, 0, nullptr));
        report_h16("swiglu", h_from(A.s, ref.size()), ref.data(), 2.0);
        CK(cudaLaunchKernel((void *)slm_swiglu_w8, dim3((total + 255) / 256), dim3(256), params, 0, nullptr));
        report_h16("swiglu_w8 fast math", h_from(A.s, ref.size()), ref.data(), 2.0);
    }

    /* RoPE with per-row positions, then KV append, then GQA over a causal mask
     * with existing KV. */
    {
        const int B = 4, nhq = M.nhq, nhkv = M.nhkv, hd = M.hd;
        const int past = 13;
        const int HQ = nhq * hd, HV = nhkv * hd;
        std::vector<uint32_t> pos(B);
        for (int i = 0; i < B; i++) {
            pos[i] = past + i;
        }
        up(A.pos, pos);

        h16 q = rand_h((size_t)B * HQ, 1.5f), ref_q = q;
        ref_rope(M.rope, pos.data(), nhq, hd, B, ref_q.data());
        h_up(A.q, q);
        const half *in = A.q;
        half *o = A.q;
        const float *tb = A.table;
        const uint32_t *pp = A.pos;
        int vb = B, vnh = nhq, vhd = hd;
        void *pr[] = {&in, &o, &tb, &pp, &vnh, &vhd, &vb};
        CK(cudaLaunchKernel((void *)slm_rope, dim3(nhq, B, 1), dim3(32), pr, 0, nullptr));
        const h16 got_q = h_from(A.q, ref_q.size());
        report_h16("rope B=4 per-row pos", got_q, ref_q.data(), 2.0);

        /* Existing KV for positions 0..past-1. */
        const int T = past + B;
        h16 refk = rand_h((size_t)T * HV, 1.5f), refv = rand_h((size_t)T * HV, 1.5f);
        h16 newk((size_t)B * HV), newv((size_t)B * HV);
        for (int i = 0; i < B; i++) {
            memcpy(&newk[(size_t)i * HV], &refk[(size_t)(past + i) * HV], (size_t)HV * 2);
            memcpy(&newv[(size_t)i * HV], &refv[(size_t)(past + i) * HV], (size_t)HV * 2);
        }
        h16 oldk = refk, oldv = refv;
        std::fill(oldk.begin() + (size_t)past * HV, oldk.end(), 0);
        std::fill(oldv.begin() + (size_t)past * HV, oldv.end(), 0);
        h_up(A.K[0], oldk);
        h_up(A.V[0], oldv);
        h_up(A.k, newk);
        h_up(A.v, newv);
        for (int w = 0; w < 2; w++) {
            const half *src = w ? A.v : A.k;
            half *dst = w ? A.V[0] : A.K[0];
            int width = HV;
            void *pk[] = {&src, &dst, &pp, &width, &vb};
            CK(cudaLaunchKernel((void *)slm_kv_append, dim3(HV / 32, B, 1), dim3(32), pk, 0, nullptr));
        }
        report_h16("kv_append (K)", h_from(A.K[0], refk.size()), refk.data(), 0.0);

        h16 ref_a((size_t)B * HQ);
        ref_gqa(ref_q.data(), refk.data(), refv.data(), pos.data(), nhq, nhkv, hd, B, ref_a.data());
        const half *pq = A.q, *pk = A.K[0], *pv = A.V[0];
        half *pa = A.a;
        int vk = nhkv;
        void *pg[] = {&pq, &pk, &pv, &pp, &pa, &vnh, &vk, &vhd, &vb};
        CK(cudaLaunchKernel((void *)slm_gqa, dim3(nhq, B, 1), dim3(32), pg, 0, nullptr));
        report_h16("gqa B=4 T=14..17 causal", h_from(A.a, ref_a.size()), ref_a.data(), 4.0);
        CK(cudaLaunchKernel((void *)slm_gqa_w8, dim3(nhq, B, 1), dim3(32, 8, 1), pg, 0, nullptr));
        report_h16("gqa_w8 B=4 T=14..17", h_from(A.a, ref_a.size()), ref_a.data(), 4.0);
    }
    CK(cudaDeviceSynchronize());
}

/* ---- end-to-end comparison with the CPU reference over a real prompt ---- */

static void report_logits(const char *label, const std::vector<float> &got,
                          const std::vector<float> &ref, double tol)
{
    double worst = 0, rms = 0;
    size_t top_g = 0, top_r = 0;
    for (size_t i = 0; i < ref.size(); i++) {
        const double e = fabs((double)got[i] - ref[i]);
        worst = std::max(worst, e);
        rms += e * e;
        if (got[i] > got[top_g]) {
            top_g = i;
        }
        if (ref[i] > ref[top_r]) {
            top_r = i;
        }
    }
    const bool ok = worst <= tol && top_g == top_r;
    printf("  %-22s logit max_err %.4f rms_err %.4f top1 got %zu want %zu, tol %.2f: %s\n", label,
           worst, sqrt(rms / ref.size()), top_g, top_r, tol, ok ? "PASS" : "FAIL");
    if (!ok) {
        g_fails++;
    }
}

static void set_step(Arena &A, const std::vector<uint32_t> &ids, uint32_t pos0)
{
    std::vector<uint32_t> pos(ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        pos[i] = pos0 + (uint32_t)i;
    }
    up(A.ids, ids);
    up(A.pos, pos);
}

int main(int argc, char **argv)
{
    const char *path = nullptr, *golden = nullptr;
    bool do_check = true;
    int prefill = 63, decode = 32, ctx = 512;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--simt") {
            g_simt = true;
        } else if (a == "--b64") {
            g_b64 = true;
        } else if (a == "--no-check") {
            do_check = false;
        } else if (a == "--golden") {
            golden = argv[++i];
        } else if (a == "--prefill") {
            prefill = atoi(argv[++i]);
        } else if (a == "--decode") {
            decode = atoi(argv[++i]);
        } else if (a == "--ctx") {
            ctx = atoi(argv[++i]);
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "usage: %s model.gguf [--golden rec] [--simt] [--no-check] "
                        "[--prefill N] [--decode N] [--ctx N]\n", argv[0]);
        return 2;
    }
    cudaDeviceProp prop;
    CK(cudaGetDeviceProperties(&prop, 0));
    printf("device %s, SMs %d, clock %d kHz\n", prop.name, prop.multiProcessorCount, prop.clockRate);
    printf("matmul tier: %s\n", g_simt ? "SIMT" : "HMMA");

    Model M;
    if (!gguf_read(path, M.g)) {
        return 2;
    }
    M.layers = (int)M.g.block_count;
    M.hidden = (int)M.g.hidden;
    M.ff = (int)M.g.ff;
    M.nhq = (int)M.g.nhq;
    M.nhkv = (int)M.g.nhkv;
    M.hd = M.hidden / M.nhq;
    M.vocab = (int)M.t("output.weight").dim1;
    M.ctx = ctx;
    M.eps = M.g.rms_eps;
    ref_rope_table(M.hd, M.ctx, M.g.rope_base, M.rope);
    printf("model: layers %d hidden %d ff %d heads %d/%d hd %d vocab %d, %.1f MB\n", M.layers,
           M.hidden, M.ff, M.nhq, M.nhkv, M.hd, M.vocab, M.g.data_size / 1e6);

    const int Bmax = std::max(64, prefill);
    Arena A;
    CK(cudaMalloc(&A.w, M.g.data_size));
    CK(cudaMemcpy(A.w, M.g.data.data(), M.g.data_size, cudaMemcpyHostToDevice));
    const size_t nH = (size_t)Bmax * M.hidden, nF = (size_t)Bmax * M.ff;
    const size_t nV = (size_t)Bmax * M.nhkv * M.hd;
    for (half **p : {&A.x, &A.xn, &A.q, &A.a, &A.p, &A.d}) {
        CK(cudaMalloc(p, nH * 2));
    }
    for (half **p : {&A.k, &A.v}) {
        CK(cudaMalloc(p, nV * 2));
    }
    for (half **p : {&A.g, &A.u, &A.s}) {
        CK(cudaMalloc(p, nF * 2));
    }
    A.K.resize(M.layers);
    A.V.resize(M.layers);
    const size_t kvb = (size_t)M.ctx * M.nhkv * M.hd * 2;
    for (int l = 0; l < M.layers; l++) {
        CK(cudaMalloc(&A.K[l], kvb));
        CK(cudaMalloc(&A.V[l], kvb));
    }
    CK(cudaMalloc(&A.run, (size_t)Bmax * std::max(M.ff, M.hidden) * 4));
    CK(cudaMalloc(&A.table, M.rope.size() * 4));
    up(A.table, M.rope);
    CK(cudaMalloc(&A.logits, (size_t)M.vocab * 4));
    CK(cudaMalloc(&A.ids, (size_t)Bmax * 4));
    CK(cudaMalloc(&A.pos, (size_t)Bmax * 4));

    if (do_check) {
        check_kernels(M, A);
    }

    /* ---- mma issue rate ---- */
    double mma_rate = 0;
    {
        float *d;
        const int warps = prop.multiProcessorCount * 32;
        const int reps = 200000;
        CK(cudaMalloc(&d, (size_t)warps * 32 * 4));
        std::vector<double> ts;
        for (int r = 0; r < 5; r++) {
            const double s = now_ms();
            cpi_mma<<<warps, 32>>>(d, reps);
            CK(cudaDeviceSynchronize());
            ts.push_back(now_ms() - s);
        }
        const double ms = median(ts);
        mma_rate = (double)warps * reps / (ms * 1e-3);
        printf("\nmma issue: %.2f G mma/s across all SMs\n", mma_rate / 1e9);
        CK(cudaFree(d));
    }

    /* ---- build both programs ---- */
    Program dec, pre;
    build_program(dec, M, A, 1);
    double dec_inst, pre_inst = 0;
    dec.build_graph(&dec_inst);
    if (prefill > 0) {
        build_program(pre, M, A, prefill);
        pre.build_graph(&pre_inst);
    }
    printf("graph: decode %zu ops, instantiate %.1f ms; prefill(B=%d) %zu ops, instantiate %.1f ms\n",
           dec.ops.size(), dec_inst, prefill, pre.ops.size(), pre_inst);

    /* ---- per-op table for decode at a realistic position ---- */
    RefState R;
    R.K.resize(M.layers);
    R.V.resize(M.layers);
    const size_t kvn = (size_t)M.ctx * M.nhkv * M.hd;
    for (int l = 0; l < M.layers; l++) {
        R.K[l].assign(kvn, 0);
        R.V[l].assign(kvn, 0);
    }

    std::mt19937 rng(1);
    std::vector<uint32_t> prompt(prefill);
    for (int i = 0; i < prefill; i++) {
        prompt[i] = rng() % std::min(M.vocab, 20000);
    }

    /* Prefill timing. Each run rewrites the same KV slots. */
    std::vector<double> tpre, tpre_rb;
    std::vector<float> logits_pre;
    if (prefill > 0) {
        set_step(A, prompt, 0);
        for (int r = 0; r < 9; r++) {
            const double s = now_ms();
            CK(cudaGraphLaunch(pre.exec, 0));
            CK(cudaStreamSynchronize(0));
            const double m = now_ms();
            logits_pre = down(A.logits, M.vocab);
            tpre.push_back(m - s);
            tpre_rb.push_back(now_ms() - s);
        }
        printf("\nprefill %d tokens: graph median %.2f ms, best %.2f ms, with logit readback %.2f ms\n",
               prefill, median(tpre), *std::min_element(tpre.begin(), tpre.end()), median(tpre_rb));
    }

    /* Decode: greedy from the prefill state. */
    std::vector<double> tdec, tdec_rb;
    std::vector<uint32_t> gen;
    uint32_t next = 0;
    if (!logits_pre.empty()) {
        next = (uint32_t)(std::max_element(logits_pre.begin(), logits_pre.end()) - logits_pre.begin());
    }
    std::vector<float> logits;
    for (int i = 0; i < decode; i++) {
        set_step(A, {next}, (uint32_t)prefill + i);
        const double s = now_ms();
        CK(cudaGraphLaunch(dec.exec, 0));
        CK(cudaStreamSynchronize(0));
        const double m = now_ms();
        logits = down(A.logits, M.vocab);
        const double e = now_ms();
        tdec.push_back(m - s);
        tdec_rb.push_back(e - s);
        gen.push_back(next);
        next = (uint32_t)(std::max_element(logits.begin(), logits.end()) - logits.begin());
    }
    const double dmed = median(tdec), drb = median(tdec_rb);
    printf("decode %d tokens at pos %d: graph median %.2f ms (%.1f tok/s), with readback %.2f ms (%.1f tok/s)\n",
           decode, prefill, dmed, 1000.0 / dmed, drb, 1000.0 / drb);

    /* Per-op breakdown: op by op with a sync each, includes launch overhead. */
    {
        std::map<std::string, std::pair<int, double>> by;
        double mma = 0, macs = 0, macs_tc = 0, bytes = 0, mm_ms = 0;
        double opx = 0;
        for (int rep = 0; rep < 3; rep++) {
            set_step(A, {next}, (uint32_t)prefill + decode);
            for (size_t i = 0; i < dec.ops.size(); i++) {
                const double s = now_ms();
                dec.run_op(i);
                CK(cudaDeviceSynchronize());
                const double t = now_ms() - s;
                Op &o = dec.ops[i];
                auto &e = by[o.cls];
                if (rep == 0) {
                    e.first++;
                }
                e.second += t / 3.0;
                opx += t / 3.0;
                if (rep == 0 && o.macs > 0) {
                    mma += o.mma;
                    macs += o.macs;
                    macs_tc += o.tc ? o.macs : 0;
                    bytes += o.bytes;
                }
                if (o.macs > 0) {
                    mm_ms += t / 3.0;
                }
            }
        }
        printf("\nper-op decode breakdown (op by op with sync, includes launch overhead):\n");
        for (auto &kv : by) {
            printf("  %-18s %4d ops  total %8.3f ms  %8.1f us/op\n", kv.first.c_str(), kv.second.first,
                   kv.second.second, kv.second.second * 1e3 / kv.second.first);
        }
        printf("  %-18s %4zu ops  total %8.3f ms\n", "ALL", dec.ops.size(), opx);
        printf("weight bytes read per token: %.1f MB; at the graph time that is %.1f GB/s\n",
               bytes / 1e6, bytes / dmed / 1e6);
        /* GQA Q dot K and p dot V count as matmul MACs done in SIMT. */
        const double T = prefill + decode;
        const double gqa_macs = 2.0 * M.layers * M.nhq * T * M.hd;
        printf("tensor-core utilization, labelled:\n");
        printf("  mma issue cycles over matmul wall time: %.1f%% (%.2fM mma at %.2f G mma/s max, matmul time %.2f ms op-by-op)\n",
               100.0 * (mma / mma_rate) / (mm_ms * 1e-3), mma / 1e6, mma_rate / 1e9, mm_ms);
        printf("  MAC share on tensor cores: %.2f%% (%.0fM of %.0fM, GQA at T=%.0f counted as SIMT)\n",
               100.0 * macs_tc / (macs + gqa_macs), macs_tc / 1e6, (macs + gqa_macs) / 1e6, T);
    }

    /* Same breakdown for prefill. */
    if (prefill > 0) {
        std::map<std::string, std::pair<int, double>> by;
        set_step(A, prompt, 0);
        double all = 0;
        for (size_t i = 0; i < pre.ops.size(); i++) {
            const double s = now_ms();
            pre.run_op(i);
            CK(cudaDeviceSynchronize());
            const double t = now_ms() - s;
            auto &e = by[pre.ops[i].cls];
            e.first++;
            e.second += t;
            all += t;
        }
        printf("\nper-op prefill breakdown B=%d (op by op with sync):\n", prefill);
        for (auto &kv : by) {
            printf("  %-18s %4d ops  total %8.3f ms  %8.1f us/op\n", kv.first.c_str(), kv.second.first,
                   kv.second.second, kv.second.second * 1e3 / kv.second.first);
        }
        printf("  %-18s %4zu ops  total %8.3f ms\n", "ALL", pre.ops.size(), all);
    }

    /* ---- whole-forward comparison with the CPU reference ---- */
    if (do_check) {
        printf("\nwhole-forward comparison with the CPU reference:\n");
        const int nprompt = prefill;
        std::vector<uint32_t> ids(prompt.begin(), prompt.begin() + nprompt);
        const double c0 = now_ms();
        std::vector<float> rl;
        std::vector<h16> lx;
        std::vector<uint32_t> pos(nprompt);
        for (int i = 0; i < nprompt; i++) {
            pos[i] = i;
        }
        /* Batched prefill on both sides, B = nprompt. */
        ref_step(M, R, ids.data(), pos.data(), nprompt, &lx, rl);
        printf("  (reference prefill of %d tokens took %.0f ms)\n", nprompt, now_ms() - c0);
        Program p8;
        build_program(p8, M, A, nprompt);
        set_step(A, ids, 0);
        for (size_t i = 0; i < p8.ops.size(); i++) {
            p8.run_op(i);
        }
        report_logits("prefill", down(A.logits, M.vocab), rl, 0.1);
        /* Two decode steps from there. */
        uint32_t nx = (uint32_t)(std::max_element(rl.begin(), rl.end()) - rl.begin());
        for (int i = 0; i < 2; i++) {
            const uint32_t p = (uint32_t)nprompt + i;
            ref_step(M, R, &nx, &p, 1, nullptr, rl);
            set_step(A, {nx}, p);
            CK(cudaGraphLaunch(dec.exec, 0));
            CK(cudaStreamSynchronize(0));
            report_logits("decode", down(A.logits, M.vocab), rl, 0.1);
            nx = (uint32_t)(std::max_element(rl.begin(), rl.end()) - rl.begin());
        }
    }

    /* ---- llama.cpp golden, tiny model ----
     * greedy.logits row s holds the logits after token s of prompt + generated,
     * so step one token at a time. llama.cpp quantizes activations to Q8_K and
     * the GPU does not; ref_variants shows that accounts for the difference (1.40
     * for exact dot + FP16 on the CPU). A tolerance of 1.5 reflects that measured
     * gap, and the top token must match at every step. */
    if (golden) {
        auto rec = read_rec(golden);
        const auto &idb = rec.at("prompt.ids");
        const auto &tb = rec.at("greedy.tokens");
        const auto &lgb = rec.at("greedy.logits");
        std::vector<uint32_t> seq((idb.size() + tb.size()) / 4);
        memcpy(seq.data(), idb.data(), idb.size());
        memcpy(seq.data() + idb.size() / 4, tb.data(), tb.size());
        const int steps = (int)(lgb.size() / 4 / M.vocab);
        double worst = 0;
        int top = 0;
        for (int st = 0; st < steps; st++) {
            set_step(A, {seq[st]}, (uint32_t)st);
            CK(cudaGraphLaunch(dec.exec, 0));
            CK(cudaStreamSynchronize(0));
            const std::vector<float> got = down(A.logits, M.vocab);
            const float *w = reinterpret_cast<const float *>(lgb.data()) + (size_t)st * M.vocab;
            for (int i = 0; i < M.vocab; i++) {
                worst = std::max(worst, fabs((double)got[i] - w[i]));
            }
            top += std::max_element(got.begin(), got.end()) - got.begin() ==
                   std::max_element(w, w + M.vocab) - w;
        }
        const bool ok = worst <= 1.5 && top == steps;
        printf("  llama.cpp greedy, %d steps: max logit error %.4f, tol 1.50; top-1 same %d/%d: %s\n",
               steps, worst, top, steps, ok ? "PASS" : "FAIL");
        if (!ok) {
            g_fails++;
        }
    }

    printf("\n%s (%d failures)\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
