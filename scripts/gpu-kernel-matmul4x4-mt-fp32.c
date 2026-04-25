/*
 * gpu-kernel-matmul4x4-mt-fp32.c — Launch the fp32 matmul4x4_mt
 * kernel on Jetson GA10B (M1 of the MNIST-on-GPU plan).
 *
 * Same launcher shape as gpu-kernel-matmul4x4-mt.c (16 threads in a
 * 4×4 CTA, 1×1×1 grid, identical cbuf layout). The differences:
 *   - inputs/outputs are float instead of int32_t
 *   - sentinel value is the fp32 bit pattern of C[0][0] = 30.0f,
 *     i.e. 0x41F00000
 *
 * cbuf[0] layout (CUDA convention, identical to the int kernels):
 *   [0x160..0x167] = a   (4×4 fp32 input)
 *   [0x168..0x16F] = b   (= Aᵀ)
 *   [0x170..0x177] = c   (4×4 fp32 output)
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-matmul4x4-mt-fp32 \
 *       gpu-kernel-matmul4x4-mt-fp32.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-matmul4x4-mt-fp32 [--preserve-for-kexec]
 *                                            [--timeout-secs N]
 *                                            [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

static const float MATMUL_A[16] = {
     1.f,  2.f,  3.f,  4.f,
     5.f,  6.f,  7.f,  8.f,
     9.f, 10.f, 11.f, 12.f,
    13.f, 14.f, 15.f, 16.f,
};
static const float MATMUL_B[16] = {
    1.f, 5.f,  9.f, 13.f,
    2.f, 6.f, 10.f, 14.f,
    3.f, 7.f, 11.f, 15.f,
    4.f, 8.f, 12.f, 16.f,
};
static const float MATMUL_EXPECTED[16] = {
     30.f,  70.f, 110.f, 150.f,
     70.f, 174.f, 278.f, 382.f,
    110.f, 278.f, 446.f, 614.f,
    150.f, 382.f, 614.f, 846.f,
};

/* SLM-OS-side sentinel: C[0][0] = 30.0f — bit pattern 0x41F00000.
 * Exact-equality polling is safe here because 30.0 is exactly
 * representable in fp32; later kernels (Conv outputs) will use the
 * sentinel-via-zero pattern instead. */
#define MATMUL_SENTINEL_OFFSET   0
#define MATMUL_SENTINEL_BITS_F32 0x41F00000u

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static int validate_matrix(const float *c)
{
    int ok = 1;
    printf("[launch] C = A × Aᵀ (fp32 multi-threaded 4×4):\n");
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float v = c[i * 4 + j];
            float e = MATMUL_EXPECTED[i * 4 + j];
            printf("  C[%d][%d] = %7.1f  (expected %7.1f)%s\n",
                   i, j, (double)v, (double)e,
                   v == e ? "" : "  <-- MISMATCH");
            if (v != e) ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./matmul4x4_mt_fp32_shader.sass";
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

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    struct gpu_buffer a = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer b = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer c = gpu_alloc_buffer(&ctx, 4096, 4096);

    memset(a.cpu_va, 0, a.size_bytes);
    memset(b.cpu_va, 0, b.size_bytes);
    memcpy(a.cpu_va, MATMUL_A, sizeof(MATMUL_A));
    memcpy(b.cpu_va, MATMUL_B, sizeof(MATMUL_B));
    msync(a.cpu_va, a.size_bytes, MS_SYNC);
    msync(b.cpu_va, b.size_bytes, MS_SYNC);

    memset(c.cpu_va, 0, c.size_bytes);
    msync(c.cpu_va, c.size_bytes, MS_SYNC);

    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    /* CTA dim override: 4×4×1 (16 threads). Grid stays 1×1×1.
     * If FFMA register pressure forces a higher REGISTER_COUNT_V
     * than the 128 default, the cubin's EIATTR_REGCOUNT will tell
     * us during SASS extraction; for now leave the default and
     * patch it post-hoc if dispatch produces wrong output. */
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 4);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 4);
    msync(ctx.qmd_va, 4096, MS_SYNC);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords (CTA=4×4×1, fp32)\n", pb_dwords);

    /* Sentinel polling: poll the fp32 bit pattern of 30.0 as a
     * uint32. The shared submit-and-poll helper just compares
     * bits, so this works for any value the kernel writes. */
    volatile uint32_t *poll_va =
        (volatile uint32_t *)((char *)c.cpu_va +
                              MATMUL_SENTINEL_OFFSET * sizeof(float));
    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                  MATMUL_SENTINEL_BITS_F32, 2000);

    float *c_result = (float *)c.cpu_va;
    msync(c.cpu_va, c.size_bytes, MS_INVALIDATE | MS_SYNC);
    __asm__ volatile("dsb sy" ::: "memory");
    int all_ok = validate_matrix(c_result);

    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] GP_GET=%u (want 1)  sentinel_ok=%d  matrix_ok=%d\n",
           gp_get, ok, all_ok);

    if (!all_ok) {
        fprintf(stderr, "[launch] FAIL: matrix mismatch\n");
        return 1;
    }
    printf("[launch] SUCCESS: fp32 4×4 matmul correct (FFMA dispatch)\n");

    if (!preserve) {
        return 0;
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  c.phys, c.gpu_va,
                                                  MATMUL_SENTINEL_BITS_F32);

    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   c_phys=0x%llx  c_gva=0x%lx  sentinel=0x%08x (= 30.0f)\n",
           (unsigned long long)c.phys, (unsigned long)c.gpu_va,
           MATMUL_SENTINEL_BITS_F32);
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
