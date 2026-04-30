/*
 * gpu-kernel-launch.c — Launch the trivial "write 0xCAFE to *out"
 * compute kernel on Jetson GA10B via the raw nvgpu interface. No
 * libcuda.
 *
 * The heavy lifting (channel setup, QMD population, dispatch
 * pushbuffer, submit+poll, handoff serialization) lives in
 * scripts/gpu-launch-common.{h,c}. This file is the write_cafe-
 * specific orchestration: load the shader, populate cbuf[0] with
 * the single output pointer at CUDA's fixed 0x160 offset, submit,
 * poll for 0xCAFE, optionally preserve the channel + v4 handoff
 * through kexec so SLM-OS's `nvgpu launch-kernel` can re-fire the
 * kernel post-handoff.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-launch \
 *       gpu-kernel-launch.c gpu-launch-common.c
 *
 * Shader blob: ./write_cafe_shader.sass (640 B, from
 * ../slmos-reference-cache/derivatives/shaders/cuda_write_shader.sass — the .text section of a
 * CUDA-compiled cuda_write cubin).
 *
 * Usage: sudo ./gpu-kernel-launch [--preserve-for-kexec]
 *                                 [--timeout-secs N]
 *                                 [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#define SEM_PAYLOAD 0x0000CAFEu

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./write_cafe_shader.sass";
    bool preserve = false;
    int timeout_secs = 900;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            preserve = true;
        } else if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = 900;
        } else if (argv[i][0] != '-') {
            shader_path = argv[i];
        }
    }

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Channel + common buffers (userd, gpfifo, pb, qmd, shader,
     * cbuf), shader uploaded. */
    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    /* Kernel-specific extra buffer: the output buffer. write_cafe
     * writes 0xCAFE here via STG.E at the pointer it reads from
     * cbuf[0][0x160]. */
    struct gpu_buffer out = gpu_alloc_buffer(&ctx, 4096, 4096);

    /* Populate cbuf[0]. CUDA kernels read their arguments from
     * cbuf[0][0x160+]. write_cafe's single `out` pointer lives at
     * offset 0x160 as a 64-bit VA. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = out.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    /* Zero the output so a non-zero post-submit read is unambiguously
     * from the kernel (not from a prior run or from pre-kexec state). */
    memset(out.cpu_va, 0, 4096);
    msync(out.cpu_va, 4096, MS_SYNC);

    /* Populate the QMD with defaults + write_cafe-specific shader VA
     * (via ctx->shader_gva). write_cafe fits the default registers /
     * dims / memory-size / cbuf-size — no per-kernel overrides. */
    gpu_launch_populate_qmd(&ctx);

    /* Build + dispatch the 13-dword compute pushbuffer. */
    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords\n", pb_dwords);

    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords,
                                  (volatile uint32_t *)out.cpu_va,
                                  SEM_PAYLOAD,
                                  2000);

    uint32_t val = *(volatile uint32_t *)out.cpu_va;
    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] result: out=0x%08x (want 0x%x) GP_GET=%u (want 1)\n",
           val, SEM_PAYLOAD, gp_get);
    if (ok) {
        printf("[launch] SUCCESS: kernel ran\n");
    } else if (gp_get == 1) {
        printf("[launch] PARTIAL: pushbuffer consumed, kernel did not run\n");
    } else {
        printf("[launch] FAIL: PBDMA did not advance\n");
    }

    if (!preserve) {
        return ok ? 0 : 1;
    }

    if (!ok) {
        fprintf(stderr, "[launch] kernel failed pre-kexec; refusing to "
                "preserve (channel state is suspect)\n");
        return 1;
    }

    /* --- --preserve-for-kexec path ---
     * Write a v4 handoff block so SLM-OS's `nvgpu launch-kernel` can
     * find the channel state + QMD + expected payload in DRAM post-
     * kexec. Keep all the nvgpu fds open so the channel + GPU
     * mappings stay live through the kexec. */
    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  out.phys, out.gpu_va,
                                                  SEM_PAYLOAD);

    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   shader_phys=0x%llx  shader_gva=0x%lx (%zu B)\n",
           (unsigned long long)gpu_virt_to_phys(ctx.shader_va),
           (unsigned long)ctx.shader_gva,
           shader_size);
    printf("[launch]   cbuf_phys=0x%llx  cbuf_gva=0x%lx\n",
           (unsigned long long)gpu_virt_to_phys(ctx.cbuf_va),
           (unsigned long)ctx.cbuf_gva);
    printf("[launch]   qmd_phys=0x%llx  qmd_gva=0x%lx\n",
           (unsigned long long)gpu_virt_to_phys(ctx.qmd_va),
           (unsigned long)ctx.qmd_gva);
    printf("[launch]   output_phys=0x%llx  output_gva=0x%lx  "
           "expected_payload=0x%x\n",
           (unsigned long long)out.phys,
           (unsigned long)out.gpu_va, SEM_PAYLOAD);
    printf("[launch] Sleeping up to %d s — kexec now:\n", timeout_secs);
    printf("[launch]   sudo slmos-kexec --no-gpu-suspend /tmp/slmos.elf\n");

    /* Sleep in bounded slices so SIGTERM responsiveness stays snappy. */
    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    printf("[launch] Exiting (%s) — channel releasing.\n",
           g_shutdown ? "signal" : "timeout");

    return 0;
}
