/*
 * gpu-kernel-matmul4x4-mt.c — Launch a multi-threaded 4×4 integer
 * matrix multiply on Jetson GA10B via raw nvgpu. First launcher in
 * the tree to override the default 1×1×1 CTA thread dims: 16 threads
 * arranged 4×4, each computing one C[i][j] cell.
 *
 * Same math, same test vectors, same cbuf layout as matmul4x4 (CUDA's
 * calling convention pins the three pointer args at 0x160/0x168/0x170
 * regardless of whether the kernel is single- or multi-threaded).
 * The only QMD delta vs matmul4x4:
 *   CTA_THREAD_DIMENSION0 : 1 → 4
 *   CTA_THREAD_DIMENSION1 : 1 → 4
 * DIMENSION2 stays at 1 (single-plane CTA).
 *
 * CUDA source: scripts/cuda/matmul4x4_mt.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-matmul4x4-mt \
 *       gpu-kernel-matmul4x4-mt.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-matmul4x4-mt [--preserve-for-kexec]
 *                                       [--timeout-secs N]
 *                                       [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

/* Test matrices pinned in scripts/cuda/matmul4x4_mt.cu (same inputs
 * as matmul4x4 — result is identical, only the dispatch shape changes). */
static const int32_t MATMUL_A[16] = {
     1,  2,  3,  4,
     5,  6,  7,  8,
     9, 10, 11, 12,
    13, 14, 15, 16,
};
/* B = Aᵀ */
static const int32_t MATMUL_B[16] = {
    1, 5,  9, 13,
    2, 6, 10, 14,
    3, 7, 11, 15,
    4, 8, 12, 16,
};
/* Expected C = A × Aᵀ. */
static const int32_t MATMUL_EXPECTED[16] = {
     30,  70, 110, 150,
     70, 174, 278, 382,
    110, 278, 446, 614,
    150, 382, 614, 846,
};

#define MATMUL_SENTINEL_OFFSET 0
#define MATMUL_SENTINEL_VALUE  30u

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static int validate_matrix(const int32_t *c)
{
    int ok = 1;
    printf("[launch] C = A × Aᵀ (multi-threaded 4×4):\n");
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t v = c[i * 4 + j];
            int32_t e = MATMUL_EXPECTED[i * 4 + j];
            printf("  C[%d][%d] = %5d  (expected %5d)%s\n",
                   i, j, v, e, v == e ? "" : "  <-- MISMATCH");
            if (v != e) ok = 0;
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./matmul4x4_mt_shader.sass";
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

    /* Three 4 KB extra buffers: A, B (inputs), C (output). */
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

    /* Same cbuf layout as matmul4x4 — CUDA's calling convention pins
     * pointer args at these offsets regardless of kernel shape. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    /* Override CTA thread dims: 4×4×1 instead of the 1×1×1 default.
     * Grid is still 1×1×1 (single CTA, 16 threads total).
     *
     * msync the full 4 KB page to match gpu_launch_populate_qmd's
     * pattern — both reach the same kernel page granularity, but
     * keeping the size identical avoids "did this msync miss
     * something" reading questions later. */
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 4);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 4);
    msync(ctx.qmd_va, 4096, MS_SYNC);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords (CTA=4×4×1)\n", pb_dwords);

    volatile uint32_t *poll_va =
        (volatile uint32_t *)((char *)c.cpu_va +
                              MATMUL_SENTINEL_OFFSET * sizeof(int32_t));
    int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                  MATMUL_SENTINEL_VALUE, 2000);

    int32_t *c_result = (int32_t *)c.cpu_va;
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
    printf("[launch] SUCCESS: multi-threaded 4×4 matmul correct "
           "(16 threads, one cell each)\n");

    if (!preserve) {
        return 0;
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v4(&ctx, handoff_va,
                                                  c.phys, c.gpu_va,
                                                  MATMUL_SENTINEL_VALUE);

    printf("[launch] Handoff at phys 0x%llx (version=4)\n",
           (unsigned long long)handoff_phys);
    printf("[launch]   a_phys=0x%llx  a_gva=0x%lx\n",
           (unsigned long long)a.phys, (unsigned long)a.gpu_va);
    printf("[launch]   b_phys=0x%llx  b_gva=0x%lx\n",
           (unsigned long long)b.phys, (unsigned long)b.gpu_va);
    printf("[launch]   c_phys=0x%llx  c_gva=0x%lx  sentinel=C[0][0]=%u\n",
           (unsigned long long)c.phys, (unsigned long)c.gpu_va,
           MATMUL_SENTINEL_VALUE);
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
