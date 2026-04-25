/*
 * gpu-kernel-matmul8x8-grid.c — Launch an 8×8 integer matrix multiply
 * on Jetson GA10B as a 2×2 grid of 4×4-thread CTAs (M0 of the
 * MNIST-on-GPU plan, docs/archive/plans/jetson-gpu-mnist-plan.md).
 *
 * First multi-CTA dispatch in the tree: every prior kernel ran with
 * grid 1×1×1 and varied only the per-CTA thread count. Here the grid
 * grows to 2×2, so the QMD's CTA_RASTER_WIDTH / CTA_RASTER_HEIGHT
 * (currently hardwired to 1 in gpu_launch_populate_qmd) need
 * launcher-side overrides on top of the CTA_THREAD_DIM overrides
 * pioneered by matmul4x4_mt.
 *
 * QMD overrides:
 *   CTA_THREAD_DIM0 : 1 → 4
 *   CTA_THREAD_DIM1 : 1 → 4
 *   CTA_RASTER_WIDTH  : 1 → 2   (NEW)
 *   CTA_RASTER_HEIGHT : 1 → 2   (NEW)
 *
 * cbuf[0] layout (same CUDA convention as matmul4x4 / dot4):
 *   [0x160..0x167] = a   (8×8 input matrix)
 *   [0x168..0x16F] = b   (= Aᵀ)
 *   [0x170..0x177] = c   (8×8 output)
 *
 * SLM-OS sentinel: C[0][0] = 204 (= 0xCC). v4 handoff carries this
 * via expected_payload so `nvgpu launch-kernel` can poll for it.
 *
 * CUDA source: scripts/cuda/matmul8x8_grid.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-matmul8x8-grid \
 *       gpu-kernel-matmul8x8-grid.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-matmul8x8-grid [--preserve-for-kexec]
 *                                         [--timeout-secs N]
 *                                         [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

/* Test data: A = [1..64] row-major 8×8, B = Aᵀ, C = A·Aᵀ Gram matrix.
 * Computed at startup so the launcher and the CUDA host main() use
 * identical reference values. */
static int32_t MATMUL_A[64];
static int32_t MATMUL_B[64];
static int32_t MATMUL_EXPECTED[64];

/* SLM-OS-side sentinel: C[0][0] = 1²+2²+...+8² = 204. */
#define MATMUL_SENTINEL_OFFSET 0
#define MATMUL_SENTINEL_VALUE  204u

static void compute_test_vectors(void)
{
    for (int v = 0; v < 64; v++) MATMUL_A[v] = v + 1;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            MATMUL_B[i * 8 + j] = MATMUL_A[j * 8 + i];
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            int32_t s = 0;
            for (int k = 0; k < 8; k++) {
                s += MATMUL_A[i * 8 + k] * MATMUL_B[k * 8 + j];
            }
            MATMUL_EXPECTED[i * 8 + j] = s;
        }
    }
}

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static int validate_matrix(const int32_t *c)
{
    int ok = 1;
    printf("[launch] C = A × Aᵀ (multi-CTA 8×8, 2×2 grid of 4×4 CTAs):\n");
    for (int i = 0; i < 8; i++) {
        printf("  ");
        for (int j = 0; j < 8; j++) {
            int32_t v = c[i * 8 + j];
            int32_t e = MATMUL_EXPECTED[i * 8 + j];
            printf(" %6d", v);
            if (v != e) ok = 0;
        }
        printf("\n");
    }
    /* Per-cell mismatch report on failure for debug clarity. */
    if (!ok) {
        printf("[launch] mismatches:\n");
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                int32_t v = c[i * 8 + j];
                int32_t e = MATMUL_EXPECTED[i * 8 + j];
                if (v != e) {
                    printf("  C[%d][%d] = %d (expected %d)\n", i, j, v, e);
                }
            }
        }
    }
    return ok;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    const char *shader_path = "./matmul8x8_grid_shader.sass";
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

    compute_test_vectors();

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    struct gpu_launch_ctx ctx = {0};
    size_t shader_size = 0;
    gpu_launch_setup(&ctx, shader_path, &shader_size);
    printf("[launch] shader %zu bytes\n", shader_size);

    /* Three 4 KB extra buffers: A, B (inputs, 256 B used each),
     * C (output, 256 B used). */
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

    /* CUDA's calling convention pins the three pointer args at these
     * offsets regardless of the kernel's grid/block shape. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    /* QMD overrides for multi-CTA + multi-thread dispatch.
     * gpu_launch_populate_qmd defaults everything to 1×1×1 grid and
     * 1×1×1 thread block. Bump CTA_THREAD_DIM0/1 to 4 (16 threads/CTA)
     * and CTA_RASTER_WIDTH/HEIGHT to 2 (2×2 = 4 CTAs in the grid).
     *
     * Total: 4 CTAs × 16 threads = 64 threads, one per output cell. */
    uint32_t *qmd = (uint32_t *)ctx.qmd_va;
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 4);
    gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 4);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                     QMD_CTA_RASTER_WIDTH_LO, 2);
    gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                     QMD_CTA_RASTER_HEIGHT_LO, 2);
    msync(ctx.qmd_va, 4096, MS_SYNC);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords (grid=2×2×1, CTA=4×4×1)\n",
           pb_dwords);

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
    printf("[launch] SUCCESS: multi-CTA 8×8 matmul correct "
           "(64 threads across 4 CTAs)\n");

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
