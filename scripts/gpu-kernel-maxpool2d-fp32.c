/*
 * gpu-kernel-maxpool2d-fp32.c — Launch the fp32 MaxPool2D kernel on
 * Jetson GA10B (M4 of the MNIST-on-GPU plan).
 *
 * One launcher binary, two MNIST shape presets:
 *   --shape pool1   1×8×28×28  → 1×8×14×14 (2×2 / stride 2)
 *   --shape pool2   1×16×14×14 → 1×16×4×4  (3×3 / stride 3)
 *
 * cbuf[0] layout (CUDA ABI for the kernel signature):
 *   [0x160] x       (pointer)
 *   [0x168] out     (pointer)
 *   [0x170] N
 *   [0x174] C
 *   [0x178] H_in
 *   [0x17C] W_in
 *   [0x180] kH
 *   [0x184] kW
 *   [0x188] stride_h
 *   [0x18C] stride_w
 *   [0x190] H_out
 *   [0x194] W_out
 *
 * Test pattern: x[idx] = (float)idx so every window has a unique max
 * at its bottom-right corner. The full output cell-for-cell is
 * compared against a CPU reference.
 *
 * Sentinel out[0,0,0,0] for the test pattern with x[idx]=idx is the
 * value at the bottom-right of the first window. For (kH=2, sH=2)
 * starting at (0, 0) over a [N=1,C=8,H=28,W=28] tensor, that
 * window covers x[0,0,0..1, 0..1]; the max is x[0,0,1,1] = 1*W=28.
 * For the Conv2 → MaxPool2 case (3×3 stride 3 over 14×14), the
 * first window's max is x[0,0,2,2] = 2*14+2 = 30.
 *
 * CUDA source: scripts/cuda/maxpool2d_fp32.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-maxpool2d-fp32 \
 *       gpu-kernel-maxpool2d-fp32.c gpu-launch-common.c
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <float.h>

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static uint32_t round_up_page(size_t bytes)
{
    size_t r = (bytes + 4095u) & ~((size_t)4095);
    if (r == 0) r = 4096;
    if (r > UINT32_MAX) {
        fprintf(stderr, "shape too large (%zu)\n", r);
        exit(1);
    }
    return (uint32_t)r;
}

static void cpu_ref(const float *x, float *out,
                    int N, int C, int H_in, int W_in,
                    int kH, int kW, int sH, int sW,
                    int H_out, int W_out)
{
    for (int n = 0; n < N; n++) {
        for (int c = 0; c < C; c++) {
            for (int oh = 0; oh < H_out; oh++) {
                for (int ow = 0; ow < W_out; ow++) {
                    float m = -FLT_MAX;
                    for (int dh = 0; dh < kH; dh++) {
                        for (int dw = 0; dw < kW; dw++) {
                            int h = oh * sH + dh;
                            int w = ow * sW + dw;
                            float v = x[((n * C + c) * H_in + h) * W_in + w];
                            if (v > m) m = v;
                        }
                    }
                    out[((n * C + c) * H_out + oh) * W_out + ow] = m;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./maxpool2d_fp32_shader.sass";
    const char *shape_name = NULL;
    bool preserve = false;
    int timeout_secs = 900;
    int N = 1, C = 0, H_in = 0, W_in = 0, kH = 0, kW = 0, sH = 0, sW = 0;

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
                "usage: %s --shape {pool1|pool2} "
                "[--preserve-for-kexec] [--timeout-secs N] "
                "[shader.sass]\n", argv[0]);
        return 1;
    }
    if (strcmp(shape_name, "pool1") == 0)
        { C =  8; H_in = 28; W_in = 28; kH = 2; kW = 2; sH = 2; sW = 2; }
    else if (strcmp(shape_name, "pool2") == 0)
        { C = 16; H_in = 14; W_in = 14; kH = 3; kW = 3; sH = 3; sW = 3; }
    else {
        fprintf(stderr, "unknown shape: %s\n", shape_name);
        return 1;
    }
    int H_out = (H_in - kH) / sH + 1;
    int W_out = (W_in - kW) / sW + 1;
    printf("[launch] shape=%s N=%d C=%d %dx%d → %dx%d kH=%d kW=%d "
           "sH=%d sW=%d\n",
           shape_name, N, C, H_in, W_in, H_out, W_out, kH, kW, sH, sW);

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    size_t in_n  = (size_t)N * C * H_in * W_in;
    size_t out_n = (size_t)N * C * H_out * W_out;
    uint32_t in_bytes  = round_up_page(in_n  * sizeof(float));
    uint32_t out_bytes = round_up_page(out_n * sizeof(float));

    struct gpu_buffer x   = gpu_alloc_buffer(&ctx, in_bytes,  4096);
    struct gpu_buffer out = gpu_alloc_buffer(&ctx, out_bytes, 4096);

    /* x[idx] = (float)idx — a unique value per cell so window
     * indexing bugs surface immediately. */
    {
        float *xp = (float *)x.cpu_va;
        for (size_t i = 0; i < in_n; i++) xp[i] = (float)i;
    }
    memset(out.cpu_va, 0, out.size_bytes);
    msync(x.cpu_va, x.size_bytes, MS_SYNC);
    msync(out.cpu_va, out.size_bytes, MS_SYNC);

    /* Compute CPU reference for cross-check. */
    float *ref = (float *)malloc(out_n * sizeof(float));
    cpu_ref((float *)x.cpu_va, ref, N, C, H_in, W_in,
            kH, kW, sH, sW, H_out, W_out);

    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = x.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = out.gpu_va;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x170) = N;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x174) = C;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x178) = H_in;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x17C) = W_in;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x180) = kH;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x184) = kW;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x188) = sH;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x18C) = sW;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x190) = H_out;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x194) = W_out;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    int spatial_out = H_out * W_out;
    uint32_t grid_x = (uint32_t)((spatial_out + 255) / 256);
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 256);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, grid_x);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, (uint32_t)C);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                     QMD_CTA_RASTER_DEPTH_LO, (uint32_t)N);
    msync(ctx.qmd_va, 4096, MS_SYNC);
    gpu_write_builtin_dims(&ctx, 256, 1, 1, grid_x, (uint32_t)C, (uint32_t)N);
    printf("[launch] grid=%ux%dx%d CTA=256×1×1\n", grid_x, C, N);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);

    /* Sentinel: out[0,0,0,0] from the CPU reference. */
    float sentinel_f = ref[0];
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
    for (size_t i = 0; i < out_n; i++) {
        if (op[i] != ref[i]) {
            if (report < 4) {
                fprintf(stderr, "  out[%zu]=%f (expected %f)\n",
                        i, (double)op[i], (double)ref[i]);
                report++;
            }
            all_ok = 0;
        }
    }

    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] GP_GET=%u (want 1)  sentinel_ok=%d  all_ok=%d  "
           "(sentinel=0x%08x = %.0f)\n",
           gp_get, ok, all_ok, sentinel_bits, (double)sentinel_f);

    if (!all_ok) {
        fprintf(stderr, "[launch] FAIL: %s pool mismatch\n", shape_name);
        free(ref);
        return 1;
    }
    printf("[launch] SUCCESS: %s pool correct (%zu cells)\n",
           shape_name, out_n);
    free(ref);

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
    printf("[launch]   out_phys=0x%llx  sentinel=0x%08x (%.0f)\n",
           (unsigned long long)out.phys, sentinel_bits, (double)sentinel_f);
    printf("[launch] Sleeping up to %d s — kexec now.\n", timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
