/*
 * gqa_bench.cu - HMMA GQA against slm_gqa_w8 under JetPack (#1167).
 *
 * Build and run on the board (run-gqa-bench.sh does both):
 *   nvcc -O3 -std=c++17 -arch=sm_87 -Xcompiler -fopenmp -lgomp -I../slm \
 *        gqa_bench.cu -o gqa_bench
 *   ./gqa_bench [--checks-only] [--no-checks] [--only <case>] [--runs N] [--raw file.csv]
 *
 * Checks: every case of gqa_check.h on slm_gqa_w8 and the four HMMA kernels
 * (slm_gqa_hmma_dec / _dec_p16 for B * group <= 16, slm_gqa_hmma_pf / _pf_p16
 * for all), against the exact reference. KV slots past the causal limit hold
 * NaN and Inf.
 *
 * Timing, Qwen2.5-1.5B shapes, 28 layers with their own q, K, V and out:
 *   decode   1 row at position kv - 1: slm_gqa_w8, hmma_dec, hmma_dec_p16
 *   prefill  64 rows at kv - 64 .. kv - 1: slm_gqa_w8, hmma_pf, hmma_pf_p16
 * Method "launch": a kernel that reads 16 MB scrubs the L2, then CUDA events
 * time one GQA launch; a run times all 28 layers for every kernel, the kernel
 * order rotating from run to run. Method "graph": the 28 launches of one
 * kernel in one CUDA graph, no scrub, timed as a whole. Three warm-up runs,
 * then --runs (default 21). Same inputs for every kernel. A "null" kernel
 * (empty, block 32x8) gives the launch-and-event overhead every figure carries.
 *
 * Verdict per point (PR #1178): the HMMA kernel wins when its median per
 * launch is at least 20 percent below slm_gqa_w8's and it saves at least
 * 50 us per token (28 layers).
 *
 * GPU and EMC clocks: the paths in GQA_GPU_FREQ and GQA_EMC_RATE (set by
 * run-gqa-bench.sh) are read before and after each point.
 */
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../slm/slm_ops.cu"
/* -DGQA_KERNEL_SRC='"path"' builds another copy (the mutation run). The _p16
 * comparison kernels exist only with GQA_P16_VARIANTS. */
#define GQA_P16_VARIANTS 1
#ifndef GQA_KERNEL_SRC
#define GQA_KERNEL_SRC "../slm/slm_gqa_hmma.cu"
#endif
#include GQA_KERNEL_SRC
#include "gqa_check.h"

#define CK(call)                                                                                 \
    do {                                                                                         \
        cudaError_t _e = (call);                                                                 \
        if (_e != cudaSuccess) {                                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(_e));          \
            exit(3);                                                                             \
        }                                                                                        \
    } while (0)

enum Which { W_W8, W_DEC, W_PF, W_NULL };

struct Kern {
    const char *name;
    void *fn;
    Which which;
    bool ulp_rule;
};

__global__ void gqa_null(const half *, const half *, const half *, const uint32_t *, half *, int,
                         int, int, int)
{
}

static const Kern K_W8 = {"slm_gqa_w8", (void *)slm_gqa_w8, W_W8, true};
static const Kern K_DEC = {"hmma_dec", (void *)slm_gqa_hmma_dec, W_DEC, true};
static const Kern K_DEC16 = {"hmma_dec_p16", (void *)slm_gqa_hmma_dec_p16, W_DEC, false};
static const Kern K_PF = {"hmma_pf", (void *)slm_gqa_hmma_pf, W_PF, true};
static const Kern K_PF16 = {"hmma_pf_p16", (void *)slm_gqa_hmma_pf_p16, W_PF, false};
static const Kern K_NULL = {"null", (void *)gqa_null, W_NULL, false};

static void shape_of(const Kern &k, const GqaShape &S, int B, dim3 &g, dim3 &b)
{
    const int group = S.nhq / S.nhkv;
    switch (k.which) {
    case W_W8:
    case W_NULL:
        g = dim3(S.nhq, B, 1);
        b = dim3(32, 8, 1);
        break;
    case W_DEC:
        g = dim3(S.nhkv, GQ_HD / GQ_DEC_DS, 1);
        b = dim3(32, GQ_DEC_WARPS, 1);
        break;
    case W_PF: {
        const int mt = (B * group + 15) / 16;
        g = dim3(S.nhkv, (mt + GQ_PF_WARPS - 1) / GQ_PF_WARPS, 1);
        b = dim3(32, GQ_PF_WARPS, 1);
        break;
    }
    }
}

struct Dev {
    half *q = nullptr, *k = nullptr, *v = nullptr, *out = nullptr;
    uint32_t *pos = nullptr;
    int B = 0;
    void *args[9];
    int nhq, nhkv, hd;
    void bind(const GqaShape &S)
    {
        nhq = S.nhq;
        nhkv = S.nhkv;
        hd = S.hd;
        args[0] = &q;
        args[1] = &k;
        args[2] = &v;
        args[3] = &pos;
        args[4] = &out;
        args[5] = &nhq;
        args[6] = &nhkv;
        args[7] = &hd;
        args[8] = &B;
    }
};

static Dev upload(const GqaShape &S, const GqaData &d)
{
    Dev x;
    x.B = d.B;
    CK(cudaMalloc(&x.q, d.q.size() * 2));
    CK(cudaMalloc(&x.k, d.k.size() * 2));
    CK(cudaMalloc(&x.v, d.v.size() * 2));
    CK(cudaMalloc(&x.out, d.q.size() * 2));
    CK(cudaMalloc(&x.pos, d.pos.size() * 4));
    CK(cudaMemcpy(x.q, d.q.data(), d.q.size() * 2, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(x.k, d.k.data(), d.k.size() * 2, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(x.v, d.v.data(), d.v.size() * 2, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(x.pos, d.pos.data(), d.pos.size() * 4, cudaMemcpyHostToDevice));
    x.bind(S);
    return x;
}

static void release(Dev &x)
{
    cudaFree(x.q);
    cudaFree(x.k);
    cudaFree(x.v);
    cudaFree(x.out);
    cudaFree(x.pos);
}

static void launch(const Kern &k, const GqaShape &S, Dev &x, cudaStream_t st = 0)
{
    dim3 g, b;
    shape_of(k, S, x.B, g, b);
    CK(cudaLaunchKernel(k.fn, g, b, x.args, 0, st));
}

/* ---- checks ---- */

static std::string g_only; /* --only: cases whose name contains this */

static int run_checks(const GqaShape &S)
{
    printf("\nchecks against the exact reference (gqa_check.h):\n");
    const int group = S.nhq / S.nhkv;
    int fails = 0, n = 0, w8_fails = 0;
    for (const GqaCase &c : gqa_cases(true)) {
        if (c.name.find(g_only) == std::string::npos) {
            continue;
        }
        const GqaData d = gqa_make(S, c, gqa_seed(c.name));
        std::vector<double> ref;
        gqa_ref_exact(S, d, ref);
        Dev x = upload(S, d);
        std::vector<const Kern *> ks = {&K_W8, &K_PF, &K_PF16};
        if (d.B * group <= 16) {
            ks.insert(ks.begin() + 1, {&K_DEC, &K_DEC16});
        }
        for (const Kern *k : ks) {
            CK(cudaMemset(x.out, 0xFF, d.q.size() * 2)); /* NaN: unwritten outputs fail */
            launch(*k, S, x);
            CK(cudaDeviceSynchronize());
            std::vector<uint16_t> got(d.q.size());
            CK(cudaMemcpy(got.data(), x.out, got.size() * 2, cudaMemcpyDeviceToHost));
            const GqaErr e = gqa_compare(got, ref, k->ulp_rule);
            gqa_report(k->name, c, e, k->ulp_rule);
            if (k == &K_W8) {
                w8_fails += !e.pass; /* the baseline, for comparison only */
            } else {
                fails += !e.pass;
                n++;
            }
        }
        release(x);
        fflush(stdout);
    }
    printf("checks: %d of %d HMMA checks failed; slm_gqa_w8 failed %d (not counted)\n", fails, n,
           w8_fails);
    return fails;
}

/* ---- timing ---- */

__global__ void l2_scrub(const uint4 *buf, size_t n, uint32_t *sink)
{
    uint32_t acc = 0;
    for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n;
         i += (size_t)gridDim.x * blockDim.x) {
        const uint4 v = buf[i];
        acc ^= v.x ^ v.y ^ v.z ^ v.w;
    }
    if (acc == 0x9E3779B9u) {
        *sink = acc;
    }
}

static std::string read_first_line(const char *env)
{
    const char *p = getenv(env);
    if (!p || !*p) {
        return "n/a";
    }
    std::ifstream f(p);
    std::string s;
    if (!std::getline(f, s)) {
        return "unreadable";
    }
    return s;
}

static double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

struct Stat {
    double tok_med = 0, tok_min = 0, tok_max = 0; /* us per token (28 launches) */
    double launch_med = 0;                        /* us per launch */
    double graph_med = 0, graph_min = 0, graph_max = 0;
};

int main(int argc, char **argv)
{
    bool checks = true, timing = true;
    int runs = 21;
    std::string raw = "gqa_bench_raw.csv";
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--checks-only") {
            timing = false;
        } else if (a == "--no-checks") {
            checks = false;
        } else if (a == "--runs" && i + 1 < argc) {
            runs = atoi(argv[++i]);
        } else if (a == "--only" && i + 1 < argc) {
            g_only = argv[++i];
        } else if (a == "--raw" && i + 1 < argc) {
            raw = argv[++i];
        }
    }
    cudaDeviceProp prop;
    CK(cudaGetDeviceProperties(&prop, 0));
    printf("gqa_bench: %s, %d SMs, L2 %d KB, CUDA %d\n", prop.name, prop.multiProcessorCount,
           prop.l2CacheSize / 1024, CUDART_VERSION);
    const GqaShape S{};
    int fails = 0;
    if (checks) {
        fails = run_checks(S);
    }
    if (!timing) {
        return fails ? 1 : 0;
    }

    const int L = 28;
    const int warm = 3;
    const size_t scrub_n = (16u << 20) / 16;
    uint4 *scrub;
    uint32_t *sink;
    CK(cudaMalloc(&scrub, scrub_n * 16));
    CK(cudaMemset(scrub, 1, scrub_n * 16));
    CK(cudaMalloc(&sink, 4));
    FILE *csv = fopen(raw.c_str(), "w");
    fprintf(csv, "method,mode,kv,kernel,run,layer,us\n");

    /* Wake the GPU clock: about a second of scrubbing. */
    for (int i = 0; i < 400; i++) {
        l2_scrub<<<64, 256>>>(scrub, scrub_n, sink);
    }
    CK(cudaDeviceSynchronize());

    cudaEvent_t ev[2 * 28];
    for (auto &e : ev) {
        CK(cudaEventCreate(&e));
    }
    std::map<std::string, Stat> stats;
    const int kvs[] = {64, 128, 256, 512, 1024, 2048, 4096};
    printf("\ntiming: %d warm-up runs, %d runs, %d layers; us\n", warm, runs, L);
    for (int mode = 0; mode < 2; mode++) {
        const int B = mode ? 64 : 1;
        const char *mname = mode ? "prefill" : "decode";
        std::vector<const Kern *> ks = {&K_NULL, &K_W8};
        if (mode) {
            ks.push_back(&K_PF);
            ks.push_back(&K_PF16);
        } else {
            ks.push_back(&K_DEC);
            ks.push_back(&K_DEC16);
        }
        for (int kv : kvs) {
            if (kv < B) {
                continue;
            }
            const std::string clk0 = read_first_line("GQA_GPU_FREQ");
            const std::string emc0 = read_first_line("GQA_EMC_RATE");
            std::vector<Dev> lay(L);
            for (int l = 0; l < L; l++) {
                const GqaCase c{"layer", gqa_run(B, kv - 1), kv, GQA_NORMAL};
                lay[l] = upload(S, gqa_make(S, c, 1000u * kv + 37u * l + mode));
            }
            const int nk = (int)ks.size();
            std::vector<std::vector<double>> tok(nk), per(nk), graph(nk);
            /* Method launch. */
            for (int r = 0; r < warm + runs; r++) {
                for (int j = 0; j < nk; j++) {
                    const int ki = (j + r) % nk;
                    const Kern &k = *ks[ki];
                    for (int l = 0; l < L; l++) {
                        l2_scrub<<<64, 256>>>(scrub, scrub_n, sink);
                        CK(cudaEventRecord(ev[2 * l]));
                        launch(k, S, lay[l]);
                        CK(cudaEventRecord(ev[2 * l + 1]));
                    }
                    CK(cudaDeviceSynchronize());
                    double sum = 0;
                    for (int l = 0; l < L; l++) {
                        float ms;
                        CK(cudaEventElapsedTime(&ms, ev[2 * l], ev[2 * l + 1]));
                        const double us = 1000.0 * ms;
                        if (r >= warm) {
                            per[ki].push_back(us);
                            fprintf(csv, "launch,%s,%d,%s,%d,%d,%.3f\n", mname, kv, k.name, r - warm,
                                    l, us);
                        }
                        sum += us;
                    }
                    if (r >= warm) {
                        tok[ki].push_back(sum);
                    }
                }
            }
            /* Method graph. */
            for (int ki = 0; ki < nk; ki++) {
                cudaStream_t st;
                CK(cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking));
                cudaGraph_t gr;
                cudaGraphExec_t ge;
                CK(cudaStreamBeginCapture(st, cudaStreamCaptureModeGlobal));
                for (int l = 0; l < L; l++) {
                    launch(*ks[ki], S, lay[l], st);
                }
                CK(cudaStreamEndCapture(st, &gr));
                CK(cudaGraphInstantiate(&ge, gr, 0));
                for (int r = 0; r < warm + runs; r++) {
                    CK(cudaEventRecord(ev[0], st));
                    CK(cudaGraphLaunch(ge, st));
                    CK(cudaEventRecord(ev[1], st));
                    CK(cudaStreamSynchronize(st));
                    float ms;
                    CK(cudaEventElapsedTime(&ms, ev[0], ev[1]));
                    if (r >= warm) {
                        graph[ki].push_back(1000.0 * ms);
                        fprintf(csv, "graph,%s,%d,%s,%d,all,%.3f\n", mname, kv, ks[ki]->name,
                                r - warm, 1000.0 * ms);
                    }
                }
                CK(cudaGraphExecDestroy(ge));
                CK(cudaGraphDestroy(gr));
                CK(cudaStreamDestroy(st));
            }
            const std::string clk1 = read_first_line("GQA_GPU_FREQ");
            const std::string emc1 = read_first_line("GQA_EMC_RATE");
            printf("%s kv %d: gpu freq %s -> %s, emc %s -> %s\n", mname, kv, clk0.c_str(),
                   clk1.c_str(), emc0.c_str(), emc1.c_str());
            for (int ki = 0; ki < nk; ki++) {
                Stat s;
                s.tok_med = median(tok[ki]);
                s.tok_min = *std::min_element(tok[ki].begin(), tok[ki].end());
                s.tok_max = *std::max_element(tok[ki].begin(), tok[ki].end());
                s.launch_med = median(per[ki]);
                s.graph_med = median(graph[ki]);
                s.graph_min = *std::min_element(graph[ki].begin(), graph[ki].end());
                s.graph_max = *std::max_element(graph[ki].begin(), graph[ki].end());
                stats[std::string(mname) + "/" + std::to_string(kv) + "/" + ks[ki]->name] = s;
                printf("  %-13s launch: per launch med %8.2f; per token med %9.1f (min %9.1f max "
                       "%9.1f) | graph per token med %9.1f (min %9.1f max %9.1f)\n",
                       ks[ki]->name, s.launch_med, s.tok_med, s.tok_min, s.tok_max, s.graph_med,
                       s.graph_min, s.graph_max);
            }
            fflush(stdout);
            for (Dev &x : lay) {
                release(x);
            }
        }
    }
    fclose(csv);

    printf("\nsummary (launch method, medians; win = >= 20%% faster per launch and >= 50 us per "
           "token):\n");
    printf("  %-8s %5s %-13s %10s %10s %8s %12s  %s\n", "mode", "kv", "kernel", "us/launch",
           "w8 us", "ratio", "save us/tok", "verdict");
    for (int mode = 0; mode < 2; mode++) {
        const char *mname = mode ? "prefill" : "decode";
        for (int kv : kvs) {
            const std::string base = std::string(mname) + "/" + std::to_string(kv) + "/";
            if (!stats.count(base + "slm_gqa_w8")) {
                continue;
            }
            const Stat &w = stats[base + "slm_gqa_w8"];
            for (const char *kn : {mode ? "hmma_pf" : "hmma_dec", mode ? "hmma_pf_p16" : "hmma_dec_p16"}) {
                const Stat &h = stats[base + kn];
                const double ratio = h.launch_med / w.launch_med;
                const double save = w.tok_med - h.tok_med;
                const bool win = ratio <= 0.8 && save >= 50.0;
                printf("  %-8s %5d %-13s %10.2f %10.2f %8.3f %12.1f  %s\n", mname, kv, kn,
                       h.launch_med, w.launch_med, ratio, save, win ? "WIN" : "no win");
            }
        }
    }
    printf("null kernel (launch + events): decode %.2f us, prefill %.2f us per launch (kv 64)\n",
           stats["decode/64/null"].launch_med, stats["prefill/64/null"].launch_med);
    return fails ? 1 : 0;
}
