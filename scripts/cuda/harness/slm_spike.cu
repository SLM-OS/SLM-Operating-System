/*
 * slm_spike.cu - #475 C3K design spike, run natively under JetPack.
 *
 * Times the Q4_K matmul in HMMA and SIMT on real weights at the shapes of
 * attn_q (1536x1536), ffn_gate (1536x8960) and ffn_down (8960x1536), checks
 * each against a scalar CPU reference, and measures launch overhead for a
 * chain of trivial kernels (individual launches and a CUDA graph).
 *
 * Build on the board:
 *   nvcc -O3 -std=c++17 -arch=sm_87 -I../slm slm_spike.cu -o slm_spike
 * Run:
 *   ./slm_spike <model.gguf> [iters]
 */
#include <algorithm>
#include <chrono>
#include <random>
#include <vector>

#include "../slm/slm_mm_q4k.cu"
#include "gguf_lite.h"

/* ---- layout probe: pins the register mapping documented in slm_common ---- */

__global__ void probe_mma(const half *A, const half *Bm, float *C)
{
    const int lane = threadIdx.x;
    const int g = lane >> 2;
    const int c = lane & 3;
    /* A row-major [16][16]; Bm indexed [k][n] row-major [16][8]; C [16][8]. */
    auto a = [&](int r, int k) {
        return (uint32_t)__half_as_ushort(A[r * 16 + k]) |
               ((uint32_t)__half_as_ushort(A[r * 16 + k + 1]) << 16);
    };
    auto b = [&](int k) {
        return (uint32_t)__half_as_ushort(Bm[k * 8 + g]) |
               ((uint32_t)__half_as_ushort(Bm[(k + 1) * 8 + g]) << 16);
    };
    float c0 = 0.f, c1 = 0.f, c2 = 0.f, c3 = 0.f;
    const uint32_t a0 = a(g, 2 * c), a1 = a(g + 8, 2 * c);
    const uint32_t a2 = a(g, 2 * c + 8), a3 = a(g + 8, 2 * c + 8);
    const uint32_t b0 = b(2 * c), b1 = b(2 * c + 8);
    __syncwarp();
    slm_mma(c0, c1, c2, c3, a0, a1, a2, a3, b0, b1);
    C[g * 8 + 2 * c] = c0;
    C[g * 8 + 2 * c + 1] = c1;
    C[(g + 8) * 8 + 2 * c] = c2;
    C[(g + 8) * 8 + 2 * c + 1] = c3;
}

/* ---- trivial kernel for the launch-overhead chain ---- */

__global__ void chain_nop(int *p, int i)
{
    if (threadIdx.x == 0) {
        p[blockIdx.x] = i;
    }
}

/* ---- helpers ---- */

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

static int test_probe()
{
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(-8, 8);
    half hA[256], hB[128];
    float a[256], b[128], want[128], got[128];
    for (int i = 0; i < 256; i++) {
        a[i] = (float)dist(rng);
        hA[i] = __float2half(a[i]);
    }
    for (int i = 0; i < 128; i++) {
        b[i] = (float)dist(rng);
        hB[i] = __float2half(b[i]);
    }
    for (int m = 0; m < 16; m++) {
        for (int n = 0; n < 8; n++) {
            float s = 0;
            for (int k = 0; k < 16; k++) {
                s += a[m * 16 + k] * b[k * 8 + n];
            }
            want[m * 8 + n] = s;
        }
    }
    half *dA, *dB;
    float *dC;
    CK(cudaMalloc(&dA, sizeof hA));
    CK(cudaMalloc(&dB, sizeof hB));
    CK(cudaMalloc(&dC, sizeof got));
    CK(cudaMemcpy(dA, hA, sizeof hA, cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dB, hB, sizeof hB, cudaMemcpyHostToDevice));
    probe_mma<<<1, 32>>>(dA, dB, dC);
    CK(cudaGetLastError());
    CK(cudaDeviceSynchronize());
    CK(cudaMemcpy(got, dC, sizeof got, cudaMemcpyDeviceToHost));
    int bad = 0;
    for (int i = 0; i < 128; i++) {
        if (got[i] != want[i]) {
            if (bad++ < 4) {
                printf("  probe mismatch C[%d][%d] got %g want %g\n", i / 8, i % 8,
                       (double)got[i], (double)want[i]);
            }
        }
    }
    printf("mma_layout: %s\n", bad ? "FAIL" : "PASS");
    CK(cudaFree(dA));
    CK(cudaFree(dB));
    CK(cudaFree(dC));
    return bad;
}

enum Kern { K_SIMT, K_B8, K_B64 };
static const char *kname[] = {"simt", "hmma_b8", "hmma_b64"};

struct Case {
    const char *label;
    const char *tensor;
};

static void launch(Kern k, const half *x, const uint8_t *W, void *out, int K, int N, int B)
{
    const int grid15 = (N + Q4K_TILE_ROWS - 1) / Q4K_TILE_ROWS;
    switch (k) {
    case K_SIMT:
        slm_mm_q4k_simt<<<dim3(N, B, 1), 32>>>(x, W, out, nullptr, K, N, B, 0);
        break;
    case K_B8:
        slm_mm_q4k_hmma_b8<<<grid15, 32>>>(x, W, out, nullptr, K, N, B, 0);
        break;
    case K_B64:
        slm_mm_q4k_hmma_b64<<<grid15, 32>>>(x, W, out, nullptr, K, N, B, 0);
        break;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s model.gguf [iters]\n", argv[0]);
        return 2;
    }
    const int iters = argc > 2 ? atoi(argv[2]) : 9;
    cudaDeviceProp prop;
    CK(cudaGetDeviceProperties(&prop, 0));
    printf("device %s, SMs %d, clock %d kHz, mem %.1f GB\n", prop.name,
           prop.multiProcessorCount, prop.clockRate, prop.totalGlobalMem / 1e9);

    int fails = test_probe();

    Gguf g;
    double t0 = now_ms();
    if (!gguf_read(argv[1], g)) {
        return 2;
    }
    printf("gguf: %.1f MB tensor data, %zu tensors, read in %.0f ms\n", g.data_size / 1e6,
           g.tensors.size(), now_ms() - t0);
    uint8_t *dW;
    CK(cudaMalloc(&dW, g.data_size));
    CK(cudaMemcpy(dW, g.data.data(), g.data_size, cudaMemcpyHostToDevice));

    /* Pick a Q4_K ffn_down: half the layers are Q6_K. */
    std::string down;
    for (int l = 0; l < 28 && down.empty(); l++) {
        const std::string n = "blk." + std::to_string(l) + ".ffn_down.weight";
        if (g.tensors.at(n).type == GGML_Q4_K) {
            down = n;
        }
    }
    const Case cases[] = {
        {"attn_q 1536x1536", "blk.0.attn_q.weight"},
        {"ffn_gate 1536x8960", "blk.0.ffn_gate.weight"},
        {"ffn_down 8960x1536", down.c_str()},
    };

    struct Bcase {
        Kern k;
        int B;
    };
    const Bcase bcases[] = {
        {K_SIMT, 1}, {K_B8, 1}, {K_B64, 1}, {K_B8, 8}, {K_SIMT, 64}, {K_B64, 64},
    };

    std::mt19937 rng(475);
    std::normal_distribution<float> nd(0.0f, 1.0f);

    printf("\n%-20s %-9s %3s %10s %9s %9s %10s %10s\n", "shape", "kernel", "B", "ms(median)",
           "ms(best)", "GB/s", "Gelem/s", "max_err");
    for (const Case &cs : cases) {
        const GgufTensor &t = g.tensors.at(cs.tensor);
        if (t.type != GGML_Q4_K) {
            printf("%s: not Q4_K (type %u), skipped\n", cs.tensor, t.type);
            continue;
        }
        const int K = (int)t.dim0, N = (int)t.dim1;
        const size_t rb = gg_row_bytes(t);
        const int maxB = 64;
        std::vector<uint16_t> hx((size_t)maxB * K);
        std::vector<float> fx((size_t)maxB * K);
        for (size_t i = 0; i < hx.size(); i++) {
            hx[i] = gg_f32_to_f16(nd(rng));
            fx[i] = gg_f16_to_f32(hx[i]);
        }
        half *dx;
        float *dout;
        CK(cudaMalloc(&dx, hx.size() * 2));
        CK(cudaMalloc(&dout, (size_t)maxB * N * 4));
        CK(cudaMemcpy(dx, hx.data(), hx.size() * 2, cudaMemcpyHostToDevice));

        /* CPU reference over all 64 batches, in double. */
        std::vector<float> ref((size_t)maxB * N), row(K);
        for (int r = 0; r < N; r++) {
            gg_decode_row(g.data.data(), t, r, row.data());
            for (int b = 0; b < maxB; b++) {
                double s = 0;
                const float *xb = &fx[(size_t)b * K];
                for (int k = 0; k < K; k++) {
                    s += (double)row[k] * (double)xb[k];
                }
                ref[(size_t)b * N + r] = (float)s;
            }
        }

        std::vector<float> got((size_t)maxB * N);
        const uint8_t *W = dW + t.offset;
        for (const Bcase &bc : bcases) {
            /* The SIMT re-decodes per batch; one pass is enough at B=64. */
            const int it = (bc.k == K_SIMT && bc.B > 1) ? 2 : iters;
            std::vector<double> times;
            for (int i = 0; i < it; i++) {
                const double s = now_ms();
                launch(bc.k, dx, W, dout, K, N, bc.B);
                CK(cudaGetLastError());
                CK(cudaDeviceSynchronize());
                times.push_back(now_ms() - s);
            }
            CK(cudaMemcpy(got.data(), dout, (size_t)bc.B * N * 4, cudaMemcpyDeviceToHost));
            double maxe = 0, rms = 0;
            for (size_t i = 0; i < (size_t)bc.B * N; i++) {
                const double e = fabs((double)got[i] - (double)ref[i]);
                maxe = std::max(maxe, e);
                rms += (double)ref[i] * ref[i];
            }
            rms = sqrt(rms / ((size_t)bc.B * N));
            const double med = median(times), best = *std::min_element(times.begin(), times.end());
            const double bytes = (double)N * rb;
            const double elems = (double)N * K * bc.B;
            printf("%-20s %-9s %3d %10.3f %9.3f %9.1f %10.1f %10.2e  (rel %.1e)\n", cs.label,
                   kname[bc.k], bc.B, med, best, bytes / med / 1e6, elems / med / 1e6, maxe,
                   rms > 0 ? maxe / rms : 0.0);
            if (maxe > 1e-3 * std::max(rms, 1e-6)) {
                printf("  ** above tolerance 1e-3 of rms\n");
                fails++;
            }
        }
        CK(cudaFree(dx));
        CK(cudaFree(dout));
    }

    /* ---- launch overhead with a trivial kernel chain ---- */
    const int chain = 500;
    int *dp;
    CK(cudaMalloc(&dp, 4096));
    {
        std::vector<double> t1;
        for (int i = 0; i < 200; i++) {
            const double s = now_ms();
            chain_nop<<<1, 32>>>(dp, i);
            CK(cudaDeviceSynchronize());
            t1.push_back(now_ms() - s);
        }
        printf("\nsingle launch + sync: median %.1f us\n", median(t1) * 1e3);

        std::vector<double> t2;
        for (int r = 0; r < 9; r++) {
            const double s = now_ms();
            for (int i = 0; i < chain; i++) {
                chain_nop<<<1, 32>>>(dp, i);
            }
            CK(cudaDeviceSynchronize());
            t2.push_back(now_ms() - s);
        }
        printf("chain of %d launches, one sync: median %.3f ms = %.2f us per op\n", chain,
               median(t2), median(t2) * 1e3 / chain);
    }
    {
        cudaGraph_t graph;
        CK(cudaGraphCreate(&graph, 0));
        cudaGraphNode_t prev = nullptr;
        int zero = 0;
        void *params[] = {&dp, &zero};
        for (int i = 0; i < chain; i++) {
            cudaKernelNodeParams p = {};
            p.func = (void *)chain_nop;
            p.gridDim = dim3(1);
            p.blockDim = dim3(32);
            p.sharedMemBytes = 0;
            p.kernelParams = params;
            p.extra = nullptr;
            cudaGraphNode_t n;
            CK(cudaGraphAddKernelNode(&n, graph, prev ? &prev : nullptr, prev ? 1 : 0, &p));
            prev = n;
        }
        cudaGraphExec_t ex;
        const double s0 = now_ms();
        CK(cudaGraphInstantiate(&ex, graph));
        printf("graph instantiate (%d nodes): %.3f ms\n", chain, now_ms() - s0);
        std::vector<double> t3;
        for (int r = 0; r < 21; r++) {
            const double s = now_ms();
            CK(cudaGraphLaunch(ex, 0));
            CK(cudaStreamSynchronize(0));
            t3.push_back(now_ms() - s);
        }
        printf("graph of %d chained nodes: median %.3f ms = %.2f us per op\n", chain, median(t3),
               median(t3) * 1e3 / chain);
        CK(cudaGraphExecDestroy(ex));
        CK(cudaGraphDestroy(graph));
    }

    printf("\n%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
