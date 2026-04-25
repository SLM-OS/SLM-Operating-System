/*
 * gpu-kernel-add-bias-relu-fp32.c — Launch the fused fp32 Add-bias
 * + ReLU kernel on Jetson GA10B (M3 of the MNIST-on-GPU plan).
 *
 * One launcher binary covers all three MNIST Add invocations:
 *   --shape conv1   (N=1 C=8 H=28 W=28, ReLU on)
 *   --shape conv2   (N=1 C=16 H=14 W=14, ReLU on)
 *   --shape fc      (N=1 C=10 H=1 W=1, ReLU off)
 *
 * cbuf[0] layout (CUDA ABI for the kernel signature):
 *   [0x160] x
 *   [0x168] bias
 *   [0x170] out
 *   [0x178] N
 *   [0x17C] C
 *   [0x180] H
 *   [0x184] W
 *   [0x188] apply_relu
 *
 * Test pattern: x = all 1.0f, bias[c] = c.
 *   With ReLU on  : out[n,c,h,w] = max(0, 1 + c) = 1 + c  (c >= 0)
 *   With ReLU off : out[n,c,h,w] = 1 + c
 * Sentinel out[0,0,0,0] = 1.0f = 0x3F800000 in either case (c=0).
 *
 * CUDA source: scripts/cuda/add_bias_relu_fp32.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-add-bias-relu-fp32 \
 *       gpu-kernel-add-bias-relu-fp32.c gpu-launch-common.c
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

static uint32_t round_up_page(size_t bytes)
{
    size_t r = (bytes + 4095u) & ~((size_t)4095);
    if (r > UINT32_MAX) {
        fprintf(stderr, "shape too large (%zu bytes)\n", r);
        exit(1);
    }
    return (uint32_t)r ? (uint32_t)r : 4096u;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./add_bias_relu_fp32_shader.sass";
    const char *shape_name = NULL;
    bool preserve = false;
    int timeout_secs = 900;
    int N = 1, C = 0, H = 0, W = 0, apply_relu = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shape") == 0 && i + 1 < argc) {
            shape_name = argv[++i];
        } else if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            preserve = true;
        } else if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = 900;
        } else if (argv[i][0] != '-') {
            shader_path = argv[i];
        }
    }

    if (!shape_name) {
        fprintf(stderr,
                "usage: %s --shape {conv1|conv2|fc} "
                "[--preserve-for-kexec] [--timeout-secs N] "
                "[shader.sass]\n", argv[0]);
        return 1;
    }
    if (strcmp(shape_name, "conv1") == 0)
        { C = 8;  H = 28; W = 28; apply_relu = 1; }
    else if (strcmp(shape_name, "conv2") == 0)
        { C = 16; H = 14; W = 14; apply_relu = 1; }
    else if (strcmp(shape_name, "fc") == 0)
        { C = 10; H = 1;  W = 1;  apply_relu = 0; }
    else {
        fprintf(stderr, "unknown shape: %s\n", shape_name);
        return 1;
    }
    printf("[launch] shape=%s N=%d C=%d H=%d W=%d apply_relu=%d\n",
           shape_name, N, C, H, W, apply_relu);

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    size_t total = (size_t)N * C * H * W;
    uint32_t x_bytes    = round_up_page(total * sizeof(float));
    uint32_t bias_bytes = round_up_page((size_t)C * sizeof(float));
    uint32_t out_bytes  = round_up_page(total * sizeof(float));

    struct gpu_buffer x    = gpu_alloc_buffer(&ctx, x_bytes,    4096);
    struct gpu_buffer bias = gpu_alloc_buffer(&ctx, bias_bytes, 4096);
    struct gpu_buffer out  = gpu_alloc_buffer(&ctx, out_bytes,  4096);

    /* Fill x with 1.0f, bias[c] = c. */
    {
        float *xp = (float *)x.cpu_va;
        float *bp = (float *)bias.cpu_va;
        for (size_t i = 0; i < total; i++) xp[i] = 1.0f;
        for (int c = 0; c < C; c++) bp[c] = (float)c;
    }
    memset(out.cpu_va, 0, out.size_bytes);
    msync(x.cpu_va, x.size_bytes, MS_SYNC);
    msync(bias.cpu_va, bias.size_bytes, MS_SYNC);
    msync(out.cpu_va, out.size_bytes, MS_SYNC);

    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = x.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = bias.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = out.gpu_va;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x178) = N;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x17C) = C;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x180) = H;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x184) = W;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x188) = apply_relu;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    /* CTA = 256×1×1, grid = ceil(H*W/256) × C × N. */
    int spatial = H * W;
    uint32_t grid_x = (uint32_t)((spatial + 255) / 256);
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 256);
    /* DIM1 stays at 1 (default) — kernel uses only threadIdx.x. */
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, grid_x);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, (uint32_t)C);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                     QMD_CTA_RASTER_DEPTH_LO, (uint32_t)N);
    msync(ctx.qmd_va, 4096, MS_SYNC);
    gpu_write_builtin_dims(&ctx, 256, 1, 1, grid_x, (uint32_t)C, (uint32_t)N);
    printf("[launch] grid=%ux%dx%d CTA=256×1×1 (%d active elem)\n",
           grid_x, C, N, (int)total);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);

    /* Sentinel: out[0,0,0,0] = 1.0f = 0x3F800000 (c=0 case). */
    float sentinel_f = 1.0f;
    uint32_t sentinel_bits;
    memcpy(&sentinel_bits, &sentinel_f, 4);
    volatile uint32_t *poll_va = (volatile uint32_t *)out.cpu_va;
    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                  sentinel_bits, 5000);

    msync(out.cpu_va, out.size_bytes, MS_INVALIDATE | MS_SYNC);
    __asm__ volatile("dsb sy" ::: "memory");

    int all_ok = 1;
    int report = 0;
    float *op = (float *)out.cpu_va;
    for (int n = 0; n < N; n++) {
        for (int c = 0; c < C; c++) {
            float expected = 1.0f + (float)c;
            for (int hw = 0; hw < spatial; hw++) {
                size_t idx = ((size_t)n * C + c) * spatial + hw;
                if (op[idx] != expected) {
                    if (report < 4) {
                        fprintf(stderr,
                                "  out[n=%d,c=%d,hw=%d] = %f (expected %f)\n",
                                n, c, hw, (double)op[idx],
                                (double)expected);
                        report++;
                    }
                    all_ok = 0;
                }
            }
        }
    }

    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] GP_GET=%u (want 1)  sentinel_ok=%d  all_ok=%d\n",
           gp_get, ok, all_ok);

    if (!all_ok) {
        fprintf(stderr, "[launch] FAIL: %s shape mismatch\n", shape_name);
        return 1;
    }
    printf("[launch] SUCCESS: %s shape correct\n", shape_name);

    if (!preserve) {
        return 0;
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  out.phys, out.gpu_va,
                                                  sentinel_bits);
    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   out_phys=0x%llx  sentinel=0x%08x (= 1.0f)\n",
           (unsigned long long)out.phys, sentinel_bits);
    printf("[launch] Sleeping up to %d s — kexec now:\n", timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
