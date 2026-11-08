/*
 * slm_bw.cu - how fast can the GPU read weights at all? Reads a region of the
 * GGUF with no decode and no arithmetic beyond an XOR, to separate a memory
 * ceiling from a kernel limit. Patterns:
 *   seq32     each lane reads 32 bytes in order, warps walk the region
 *   rows15    the matmul tile shape: 15 rows, 2 per lane, 8 bytes each
 *   rows15_x  same, with an extra 16-byte activation load per lane
 *
 * Build: nvcc -O3 -std=c++17 -arch=sm_87 -I../slm slm_bw.cu -o slm_bw
 * Run:   ./slm_bw <model.gguf>
 */
#include <algorithm>
#include <chrono>
#include <vector>

#include "../slm/slm_common.cuh"
#include "gguf_lite.h"

#define CK(call)                                                              \
    do {                                                                      \
        cudaError_t _e = (call);                                              \
        if (_e != cudaSuccess) {                                              \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,                \
                    cudaGetErrorString(_e));                                  \
            exit(3);                                                          \
        }                                                                     \
    } while (0)

/* Every warp reads steps*32*32 bytes of its own contiguous slice. */
__global__ void bw_seq(const uint8_t *p, uint32_t *out, int steps)
{
    const uint8_t *base = p + (size_t)blockIdx.x * steps * 1024 + threadIdx.x * 32;
    uint32_t a = 0;
    for (int s = 0; s < steps; s++) {
        const slm_x16 v = *reinterpret_cast<const slm_x16 *>(base + (size_t)s * 1024);
        const slm_x16 w = *reinterpret_cast<const slm_x16 *>(base + (size_t)s * 1024 + 16);
        a ^= v.w0 ^ v.w1 ^ v.w2 ^ v.w3 ^ w.w0 ^ w.w1 ^ w.w2 ^ w.w3;
    }
    out[blockIdx.x * 32 + threadIdx.x] = a;
}

/* Matmul tile shape: lane reads 8 bytes from each of two rows per 32-byte pass. */
__global__ void bw_rows(const uint8_t *p, uint32_t *out, int row_bytes, int passes, int with_x,
                        const uint8_t *x)
{
    const int lane = threadIdx.x;
    const int g = lane >> 2, c = lane & 3;
    const uint8_t *r0 = p + (size_t)(blockIdx.x * 15 + g) * row_bytes;
    const uint8_t *r1 = r0 + (size_t)8 * row_bytes;
    uint64_t a = 0;
    for (int s = 0; s < passes; s++) {
        a ^= *reinterpret_cast<const uint64_t *>(r0 + s * 32 + 8 * c);
        a ^= *reinterpret_cast<const uint64_t *>(r1 + s * 32 + 8 * c);
        if (with_x && g == 0) {
            const slm_x16 v = *reinterpret_cast<const slm_x16 *>(x + s * 16 + 16 * c);
            a ^= v.w0;
        }
    }
    out[blockIdx.x * 32 + lane] = (uint32_t)a;
}

static double now_ms()
{
    using clk = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clk::now().time_since_epoch()).count();
}

int main(int argc, char **argv)
{
    Gguf g;
    if (argc < 2 || !gguf_read(argv[1], g)) {
        return 2;
    }
    uint8_t *d;
    uint32_t *o;
    uint8_t *x;
    CK(cudaMalloc(&d, g.data_size));
    CK(cudaMemcpy(d, g.data.data(), g.data_size, cudaMemcpyHostToDevice));
    CK(cudaMalloc(&x, 64 * 1024));
    CK(cudaMalloc(&o, 32 * 4 * 1000000));

    auto run = [&](const char *name, double bytes, auto &&launch) {
        std::vector<double> t;
        for (int i = 0; i < 7; i++) {
            const double s = now_ms();
            launch();
            CK(cudaGetLastError());
            CK(cudaDeviceSynchronize());
            t.push_back(now_ms() - s);
        }
        std::sort(t.begin(), t.end());
        printf("%-34s %8.1f MB %9.3f ms %7.1f GB/s\n", name, bytes / 1e6, t[3], bytes / t[3] / 1e6);
    };

    /* Sequential, at several warp counts and region sizes. */
    for (int warps : {64, 256, 1024, 4096}) {
        const int steps = (int)(256e6 / (warps * 1024.0));
        char name[64];
        snprintf(name, sizeof name, "seq32 warps=%d", warps);
        run(name, (double)warps * steps * 1024,
            [&] { bw_seq<<<warps, 32>>>(d, o, steps); });
    }
    /* Repeated small region: L2 hits, gives the ceiling above DRAM. */
    run("seq32 1 MB region (L2 test)", 64.0 * 16 * 1024,
        [&] { bw_seq<<<64, 32>>>(d, o, 16); });

    /* Tile shape at the row size of the real tensors. */
    const int row_bytes = 864;
    const int rows = 8960;
    const int tiles = (rows + 14) / 15;
    const int passes = row_bytes / 32;
    const double bytes = (double)tiles * 15 * row_bytes;
    run("rows15 1536x8960 shape", bytes,
        [&] { bw_rows<<<tiles, 32>>>(d, o, row_bytes, passes, 0, x); });
    run("rows15_x with x load", bytes,
        [&] { bw_rows<<<tiles, 32>>>(d, o, row_bytes, passes, 1, x); });
    /* Whole-model read, as a decode step does. */
    const int all_tiles = (int)(g.data_size / (15 * row_bytes)) - 1;
    run("rows15 whole model", (double)all_tiles * 15 * row_bytes,
        [&] { bw_rows<<<all_tiles, 32>>>(d, o, row_bytes, passes, 0, x); });
    return 0;
}
