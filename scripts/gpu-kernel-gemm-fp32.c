/*
 * gpu-kernel-gemm-fp32.c — Launch the parameterized fp32 GEMM
 * kernel on Jetson GA10B (M2 of the MNIST-on-GPU plan).
 *
 * First launcher in the tree to:
 *   - pass scalar shape parameters (M, K, N) via cbuf alongside
 *     the three pointer args
 *   - size all buffers based on a runtime shape rather than baked-
 *     in test data
 *   - compute its grid dimensions from M/N at launch time
 *
 * cbuf[0] layout (CUDA ABI for `gemm_fp32(const float *a,
 *                                         const float *b,
 *                                         float *c,
 *                                         int M, int K, int N)`):
 *   [0x160] a (8 B pointer)
 *   [0x168] b (8 B pointer)
 *   [0x170] c (8 B pointer)
 *   [0x178] M (4 B int)
 *   [0x17C] K (4 B int)
 *   [0x180] N (4 B int)
 *
 * Test pattern (matches scripts/cuda/gemm_fp32.cu host main): A
 * and B all 1.0f → C[i,j] = K for all (i, j). Sentinel C[0][0] = K
 * as an fp32 bit pattern.
 *
 * CUDA source: scripts/cuda/gemm_fp32.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-gemm-fp32 \
 *       gpu-kernel-gemm-fp32.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-gemm-fp32 --m M --k K --n N
 *                                    [--preserve-for-kexec]
 *                                    [--timeout-secs N]
 *                                    [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

/* Round up to a 4 KB page. nvmap allocations are page-granular and
 * gpu_alloc_buffer rounds internally too, but doing it here makes
 * the per-shape sizing transparent in the [launch] log. */
static uint32_t round_up_page(size_t bytes)
{
    size_t r = (bytes + 4095u) & ~((size_t)4095);
    if (r > UINT32_MAX) {
        fprintf(stderr, "gemm: shape too large (%zu bytes)\n", r);
        exit(1);
    }
    return (uint32_t)r;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./gemm_fp32_shader.sass";
    bool preserve = false;
    int timeout_secs = 900;
    int M = 0, K = 0, N = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--m") == 0 && i + 1 < argc) {
            M = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            K = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--n") == 0 && i + 1 < argc) {
            N = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            preserve = true;
        } else if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = 900;
        } else if (argv[i][0] != '-') {
            shader_path = argv[i];
        }
    }
    if (M <= 0 || K <= 0 || N <= 0) {
        fprintf(stderr,
                "usage: %s --m M --k K --n N [--preserve-for-kexec] "
                "[--timeout-secs N] [shader.sass]\n", argv[0]);
        return 1;
    }
    printf("[launch] gemm shape M=%d K=%d N=%d\n", M, K, N);

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    uint32_t a_bytes = round_up_page((size_t)M * K * sizeof(float));
    uint32_t b_bytes = round_up_page((size_t)K * N * sizeof(float));
    uint32_t c_bytes = round_up_page((size_t)M * N * sizeof(float));
    printf("[launch] buffers: A=%u B=%u C=%u bytes\n",
           a_bytes, b_bytes, c_bytes);

    struct gpu_buffer a = gpu_alloc_buffer(&ctx, a_bytes, 4096);
    struct gpu_buffer b = gpu_alloc_buffer(&ctx, b_bytes, 4096);
    struct gpu_buffer c = gpu_alloc_buffer(&ctx, c_bytes, 4096);

    /* All-1s test pattern: C[i,j] = sum_{k=0..K-1} 1*1 = K. */
    {
        float *ap = (float *)a.cpu_va;
        float *bp = (float *)b.cpu_va;
        for (size_t i = 0; i < (size_t)M * K; i++) ap[i] = 1.0f;
        for (size_t i = 0; i < (size_t)K * N; i++) bp[i] = 1.0f;
    }
    memset(c.cpu_va, 0, c.size_bytes);
    msync(a.cpu_va, a.size_bytes, MS_SYNC);
    msync(b.cpu_va, b.size_bytes, MS_SYNC);
    msync(c.cpu_va, c.size_bytes, MS_SYNC);

    /* cbuf scalars at 0x178/0x17C/0x180. CUDA's ABI lays kernel
     * params out at consecutive offsets after the pointer args
     * (a/b/c at 0x160/0x168/0x170, then int M/K/N start at 0x178). */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c.gpu_va;
    *(int32_t *)((char *)ctx.cbuf_va + 0x178) = M;
    *(int32_t *)((char *)ctx.cbuf_va + 0x17C) = K;
    *(int32_t *)((char *)ctx.cbuf_va + 0x180) = N;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    /* CTA: 16×16 = 256 threads. Grid: ceil(N/16) × ceil(M/16) so
     * blockIdx.y maps to row-tile, blockIdx.x to col-tile (matching
     * the kernel's i/j computation). */
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 16);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 16);
    uint32_t grid_w = (N + 15) / 16;
    uint32_t grid_h = (M + 15) / 16;
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, grid_w);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, grid_h);
    msync(ctx.qmd_va, 4096, MS_SYNC);

    /* CUDA built-in vars (blockDim/gridDim) at cbuf[0][0..0x14] —
     * the gemm SASS computes `i = blockIdx.y * blockDim.y + tid.y`
     * by reading blockDim from cbuf, so an unset cbuf collapses
     * every CTA's writes onto the (0,0) tile. */
    gpu_write_builtin_dims(&ctx, 16, 16, 1, grid_w, grid_h, 1);

    printf("[launch] grid=%ux%ux1 CTA=16×16×1 (%u threads, %u active)\n",
           grid_w, grid_h, grid_w * grid_h * 256, M * N);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);

    /* Sentinel: C[0][0] = K as fp32 bits. */
    float sentinel = (float)K;
    uint32_t sentinel_bits;
    memcpy(&sentinel_bits, &sentinel, 4);
    volatile uint32_t *poll_va = (volatile uint32_t *)c.cpu_va;
    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                  sentinel_bits, 5000);

    msync(c.cpu_va, c.size_bytes, MS_INVALIDATE | MS_SYNC);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Validate every cell. With K up to 256 and float = 1.0, the sum
     * is exactly representable; exact equality is sound. */
    int all_ok = 1;
    int report = 0;
    float *cp = (float *)c.cpu_va;
    for (size_t idx = 0; idx < (size_t)M * N; idx++) {
        if (cp[idx] != sentinel) {
            if (report < 4) {
                fprintf(stderr,
                        "  C[%zu] = %f (expected %f)\n",
                        idx, (double)cp[idx], (double)sentinel);
                report++;
            }
            all_ok = 0;
        }
    }

    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] GP_GET=%u (want 1)  sentinel_ok=%d  matrix_ok=%d  "
           "(sentinel=0x%08x = %.1f)\n",
           gp_get, ok, all_ok, sentinel_bits, (double)sentinel);

    if (!all_ok) {
        fprintf(stderr, "[launch] FAIL: %dx%dx%d matrix mismatch\n", M, K, N);
        return 1;
    }
    printf("[launch] SUCCESS: %dx%dx%d gemm correct (all cells = %.1f)\n",
           M, K, N, (double)sentinel);

    if (!preserve) {
        return 0;
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  c.phys, c.gpu_va,
                                                  sentinel_bits);

    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   c_phys=0x%llx  c_gva=0x%lx  sentinel=0x%08x (%.1f)\n",
           (unsigned long long)c.phys, (unsigned long)c.gpu_va,
           sentinel_bits, (double)sentinel);
    printf("[launch] Sleeping up to %d s — kexec now:\n", timeout_secs);
    printf("[launch]   sudo slmos-kexec --no-gpu-suspend /tmp/slmos.elf\n");

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    printf("[launch] Exiting (%s) — channel releasing.\n",
           g_shutdown ? "signal" : "timeout");

    return 0;
}
