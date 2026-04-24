/*
 * gpu-kernel-matmul4x4.c — Launch a 4×4 integer matrix multiply on
 * Jetson GA10B via raw nvgpu.
 *
 * Third kernel in the tree after write_cafe (1 scalar store) and
 * dot4 (4-element dot product). Single-thread, single-CTA — same
 * QMD defaults as its predecessors. The kernel itself is 16× the
 * arithmetic of dot4 (64 mul + 48 add + 32 loads + 16 stores across
 * the unrolled nested loops).
 *
 * Channel setup / QMD population / dispatch / submit+poll /
 * v4 handoff write all live in scripts/gpu-launch-common.{h,c}.
 * This file holds the matmul-specific orchestration: fill A and
 * Aᵀ with the pinned test values, lay out the three pointer args
 * in cbuf[0], poll C[0][0] (= 30) as the SLM-OS sentinel, and
 * verify all 16 result elements Linux-side.
 *
 * CUDA source: scripts/cuda/matmul4x4.cu.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-matmul4x4 \
 *       gpu-kernel-matmul4x4.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-matmul4x4 [--preserve-for-kexec]
 *                                    [--timeout-secs N]
 *                                    [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

/* Test matrices pinned in scripts/cuda/matmul4x4.cu. */
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

/* SLM-OS polls this one cell via the v4 handoff's expected_payload.
 * Picking C[0][0] = 30 lets the Linux-side launcher verify the full
 * matrix while SLM-OS just confirms the kernel ran (the inputs are
 * preserved across kexec, so re-dispatch recomputes deterministically). */
#define MATMUL_SENTINEL_OFFSET 0
#define MATMUL_SENTINEL_VALUE  30u

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static int validate_matrix(const int32_t *c)
{
    int ok = 1;
    printf("[launch] C = A × Aᵀ:\n");
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

    const char *shader_path = "./matmul4x4_shader.sass";
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

    /* Pre-fill A and B. Element layout is row-major int32_t; the
     * GPU dereferences a[i*4 + k] as `(int *)a.gpu_va + i*4 + k`,
     * which maps to CPU offset (i*4 + k) * 4 in a.cpu_va. */
    memset(a.cpu_va, 0, a.size_bytes);
    memset(b.cpu_va, 0, b.size_bytes);
    memcpy(a.cpu_va, MATMUL_A, sizeof(MATMUL_A));
    memcpy(b.cpu_va, MATMUL_B, sizeof(MATMUL_B));
    msync(a.cpu_va, a.size_bytes, MS_SYNC);
    msync(b.cpu_va, b.size_bytes, MS_SYNC);

    memset(c.cpu_va, 0, c.size_bytes);
    msync(c.cpu_va, c.size_bytes, MS_SYNC);

    /* cbuf[0] layout (verified via SASS dump of matmul4x4):
     *   [0x160..0x167] = a
     *   [0x168..0x16F] = b
     *   [0x170..0x177] = c
     * Identical to dot4's — CUDA's calling convention places
     * pointer args at the same offsets regardless of kernel size. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    gpu_launch_populate_qmd(&ctx);

    uint32_t pb_buf[32];
    size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, ctx.qmd_gva);
    printf("[launch] pushbuffer %zu dwords\n", pb_dwords);

    /* Poll C[0][0] for the sentinel value (= 30). */
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
    printf("[launch] SUCCESS: 4×4 matmul correct\n");

    if (!preserve) {
        return 0;
    }

    /* --- --preserve-for-kexec path ---
     * Write a v4 handoff so SLM-OS's `nvgpu launch-kernel` can
     * re-dispatch matmul4x4 post-kexec. The sentinel is C[0][0]
     * (first element of the output buffer) = 30. */
    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    /* `output_phys` / `output_gpu_va` point at C itself. C[0] is the
     * poll sentinel — SLM-OS polls the first uint32 at output_phys
     * for MATMUL_SENTINEL_VALUE. Since the sentinel offset is 0, the
     * full C and the sentinel view coincide and no offset math is
     * needed. */
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
