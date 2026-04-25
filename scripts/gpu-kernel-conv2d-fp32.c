/*
 * gpu-kernel-conv2d-fp32.c — Launch the fp32 direct Conv2D kernel
 * on Jetson GA10B (M5 of the MNIST-on-GPU plan).
 *
 * One launcher binary, two MNIST shape presets:
 *   --shape conv1   x [1,1,28,28] w [8,1,5,5]    → out [1,8,28,28]
 *   --shape conv2   x [1,8,14,14] w [16,8,5,5]   → out [1,16,14,14]
 *
 * Both use SAME_UPPER padding which collapses to symmetric pad=2
 * for the 5×5 kernels (pad = (kH-1)/2 = 2 when kH is odd).
 *
 * cbuf[0] layout (CUDA ABI for the kernel signature):
 *   [0x160] x        [0x178] N        [0x18C] kH
 *   [0x168] w        [0x17C] C_in     [0x190] kW
 *   [0x170] out      [0x180] H        [0x194] pad_h
 *                    [0x184] W        [0x198] pad_w
 *                    [0x188] C_out
 *
 * Test pattern: x = w = all 1.0f. Result for an interior cell
 * = C_in * kH * kW; result for corner is reduced by out-of-bounds
 * reads. Cross-check uses a CPU reference implementation for the
 * full output.
 *
 * Sentinel out[0,0,0,0] (top-left corner of channel 0): the in-
 * bounds portion of the kernel window is (kH - pad_h) × (kW - pad_w)
 * = 3×3 = 9 cells per input channel, so corner = 9 * C_in.
 *
 * CUDA source: scripts/cuda/conv2d_fp32_direct.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-conv2d-fp32 \
 *       gpu-kernel-conv2d-fp32.c gpu-launch-common.c
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
    if (r == 0) r = 4096;
    if (r > UINT32_MAX) {
        fprintf(stderr, "shape too large (%zu)\n", r);
        exit(1);
    }
    return (uint32_t)r;
}

static void cpu_conv(const float *x, const float *w, float *out,
                     int N, int C_in, int H, int W,
                     int C_out, int kH, int kW,
                     int pad_h, int pad_w)
{
    for (int n = 0; n < N; n++) {
        for (int co = 0; co < C_out; co++) {
            for (int oh = 0; oh < H; oh++) {
                for (int ow = 0; ow < W; ow++) {
                    float s = 0.0f;
                    for (int ci = 0; ci < C_in; ci++) {
                        for (int kh = 0; kh < kH; kh++) {
                            int ih = oh + kh - pad_h;
                            if (ih < 0 || ih >= H) continue;
                            for (int kw = 0; kw < kW; kw++) {
                                int iw = ow + kw - pad_w;
                                if (iw < 0 || iw >= W) continue;
                                int xi = ((n * C_in + ci) * H + ih) * W + iw;
                                int wi = ((co * C_in + ci) * kH + kh) * kW + kw;
                                s += x[xi] * w[wi];
                            }
                        }
                    }
                    out[((n * C_out + co) * H + oh) * W + ow] = s;
                }
            }
        }
    }
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./conv2d_fp32_direct_shader.sass";
    const char *shape_name = NULL;
    bool preserve = false;
    int timeout_secs = 900;
    int N = 1, C_in = 0, H = 0, W = 0, C_out = 0, kH = 5, kW = 5;
    int pad_h = 2, pad_w = 2;

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
                "usage: %s --shape {conv1|conv2} "
                "[--preserve-for-kexec] [--timeout-secs N] "
                "[shader.sass]\n", argv[0]);
        return 1;
    }
    if (strcmp(shape_name, "conv1") == 0)
        { C_in = 1;  H = 28; W = 28; C_out = 8;  }
    else if (strcmp(shape_name, "conv2") == 0)
        { C_in = 8;  H = 14; W = 14; C_out = 16; }
    else {
        fprintf(stderr, "unknown shape: %s\n", shape_name);
        return 1;
    }
    printf("[launch] shape=%s N=%d C_in=%d %dx%d → C_out=%d k=%dx%d pad=%d\n",
           shape_name, N, C_in, H, W, C_out, kH, kW, pad_h);

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    size_t in_n  = (size_t)N * C_in * H * W;
    size_t w_n   = (size_t)C_out * C_in * kH * kW;
    size_t out_n = (size_t)N * C_out * H * W;
    uint32_t in_bytes  = round_up_page(in_n  * sizeof(float));
    uint32_t w_bytes   = round_up_page(w_n   * sizeof(float));
    uint32_t out_bytes = round_up_page(out_n * sizeof(float));

    struct gpu_buffer xb = gpu_alloc_buffer(&ctx, in_bytes,  4096);
    struct gpu_buffer wb = gpu_alloc_buffer(&ctx, w_bytes,   4096);
    struct gpu_buffer ob = gpu_alloc_buffer(&ctx, out_bytes, 4096);

    {
        float *xp = (float *)xb.cpu_va;
        float *wp = (float *)wb.cpu_va;
        for (size_t i = 0; i < in_n; i++) xp[i] = 1.0f;
        for (size_t i = 0; i < w_n;  i++) wp[i] = 1.0f;
    }
    memset(ob.cpu_va, 0, ob.size_bytes);
    msync(xb.cpu_va, xb.size_bytes, MS_SYNC);
    msync(wb.cpu_va, wb.size_bytes, MS_SYNC);
    msync(ob.cpu_va, ob.size_bytes, MS_SYNC);

    /* CPU reference for cross-check. */
    float *ref = (float *)malloc(out_n * sizeof(float));
    cpu_conv((float *)xb.cpu_va, (float *)wb.cpu_va, ref,
             N, C_in, H, W, C_out, kH, kW, pad_h, pad_w);

    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = xb.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = wb.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = ob.gpu_va;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x178) = N;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x17C) = C_in;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x180) = H;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x184) = W;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x188) = C_out;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x18C) = kH;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x190) = kW;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x194) = pad_h;
    *(int32_t  *)((char *)ctx.cbuf_va + 0x198) = pad_w;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    int spatial = H * W;
    uint32_t grid_x = (uint32_t)((spatial + 255) / 256);
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 256);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, grid_x);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, (uint32_t)C_out);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                     QMD_CTA_RASTER_DEPTH_LO, (uint32_t)N);
    /* Conv2D's inner triple-loop may use more than the default 128
     * regs/thread. cubin .nv.info will tell us at SASS extraction
     * time; if it's > 128, override REGISTER_COUNT_V here. Leaving
     * the default for the first try since cuobjdump will surface
     * any spill before we ever submit. */
    msync(ctx.qmd_va, 4096, MS_SYNC);
    gpu_write_builtin_dims(&ctx, 256, 1, 1,
                            grid_x, (uint32_t)C_out, (uint32_t)N);
    printf("[launch] grid=%ux%dx%d CTA=256×1×1\n", grid_x, C_out, N);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);

    /* Sentinel: out[0,0,0,0] from the CPU reference. */
    float sentinel_f = ref[0];
    uint32_t sentinel_bits;
    memcpy(&sentinel_bits, &sentinel_f, 4);
    volatile uint32_t *poll_va = (volatile uint32_t *)ob.cpu_va;
    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                  sentinel_bits, 5000);

    msync(ob.cpu_va, ob.size_bytes, MS_INVALIDATE | MS_SYNC);
    __asm__ volatile("dsb sy" ::: "memory");

    int all_ok = 1;
    int report = 0;
    float *op = (float *)ob.cpu_va;
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
           "(sentinel=0x%08x = %.1f)\n",
           gp_get, ok, all_ok, sentinel_bits, (double)sentinel_f);

    if (!all_ok) {
        fprintf(stderr, "[launch] FAIL: %s conv mismatch\n", shape_name);
        free(ref);
        return 1;
    }
    /* Print a few cells for visual sanity. */
    printf("[launch] sample cells:\n");
    printf("  out[0,0,0,0] (top-left corner)   = %.1f\n", (double)op[0]);
    printf("  out[0,0,%d,%d] (interior)         = %.1f\n",
           H/2, W/2, (double)op[((H/2) * W) + W/2]);
    printf("  out[0,%d,%d,%d] (last channel)    = %.1f\n",
           C_out - 1, H/2, W/2,
           (double)op[((C_out - 1) * H + H/2) * W + W/2]);
    printf("[launch] SUCCESS: %s conv correct (%zu cells)\n",
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
                                                  ob.phys, ob.gpu_va,
                                                  sentinel_bits);
    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   out_phys=0x%llx  sentinel=0x%08x (%.1f)\n",
           (unsigned long long)ob.phys, sentinel_bits, (double)sentinel_f);
    printf("[launch] Sleeping up to %d s — kexec now.\n", timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
