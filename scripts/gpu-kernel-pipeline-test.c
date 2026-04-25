/*
 * gpu-kernel-pipeline-test.c — Launch a 2-op pipeline on Jetson GA10B
 * (M6 of the MNIST-on-GPU plan).
 *
 * Smallest end-to-end validation of the v5 multi-op handoff path.
 * Both ops are matmul4x4_mt_fp32 (the M1 kernel) with different
 * inputs so we can tell them apart in the output:
 *
 *   Op 0: A0 = [1..16] row-major, B0 = A0ᵀ
 *         → C0 = A0·A0ᵀ Gram matrix; C0[0][0] = 1²+2²+3²+4² = 30.0
 *
 *   Op 1: A1 = [2..17], B1 = A1ᵀ
 *         → C1 = A1·A1ᵀ;          C1[0][0] = 2²+3²+4²+5² = 54.0
 *
 * Linux side:
 *   - Allocates a separate QMD + cbuf + a/b/c buffer set for each op
 *   - Builds a 2-entry array of struct ga10b_pipeline_op describing
 *     each op's QMD GPU VA, output physical, and expected sentinel
 *   - Dispatches both ops in sequence locally (proves the chain
 *     works in raw nvgpu) and validates both outputs
 *   - Optionally writes a v5 handoff so SLM-OS can re-dispatch
 *     the same chain post-kexec via `nvgpu launch-kernel`
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-pipeline-test \
 *       gpu-kernel-pipeline-test.c gpu-launch-common.c
 *
 * Usage: sudo ./gpu-kernel-pipeline-test [--preserve-for-kexec]
 *                                        [--timeout-secs N]
 *                                        [path/to/shader.sass]
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>

#define OP_COUNT 2

static const float OP0_A[16] = {
     1.f,  2.f,  3.f,  4.f,
     5.f,  6.f,  7.f,  8.f,
     9.f, 10.f, 11.f, 12.f,
    13.f, 14.f, 15.f, 16.f,
};
static const float OP0_B[16] = {
    1.f, 5.f,  9.f, 13.f,
    2.f, 6.f, 10.f, 14.f,
    3.f, 7.f, 11.f, 15.f,
    4.f, 8.f, 12.f, 16.f,
};
static const float OP1_A[16] = {
     2.f,  3.f,  4.f,  5.f,
     6.f,  7.f,  8.f,  9.f,
    10.f, 11.f, 12.f, 13.f,
    14.f, 15.f, 16.f, 17.f,
};
static const float OP1_B[16] = {
    2.f, 6.f, 10.f, 14.f,
    3.f, 7.f, 11.f, 15.f,
    4.f, 8.f, 12.f, 16.f,
    5.f, 9.f, 13.f, 17.f,
};
/* Sentinels: C0[0][0] = 30.0f → 0x41F00000
 *            C1[0][0] = 54.0f → 0x42580000 */
#define OP0_SENTINEL_BITS 0x41F00000u  /* 30.0f */
#define OP1_SENTINEL_BITS 0x42580000u  /* 54.0f */

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static int validate_corner(const float *c, float expected, const char *tag)
{
    if (c[0] != expected) {
        fprintf(stderr, "[%s] FAIL: corner=%f expected %f\n",
                tag, (double)c[0], (double)expected);
        return 0;
    }
    printf("[%s] OK: corner=%.1f\n", tag, (double)c[0]);
    return 1;
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

    /* Op 0 reuses ctx.qmd / ctx.cbuf. Op 1 needs its own QMD page
     * + cbuf page so the pipeline can dispatch each op with its
     * own kernel args. Both ops share the same shader. */
    struct gpu_buffer qmd1 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer cbuf1 = gpu_alloc_buffer(&ctx, 4096, 4096);

    /* Per-op input/output buffers. */
    struct gpu_buffer a0 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer b0 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer c0 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer a1 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer b1 = gpu_alloc_buffer(&ctx, 4096, 4096);
    struct gpu_buffer c1 = gpu_alloc_buffer(&ctx, 4096, 4096);

    /* Pre-fill A0/B0/A1/B1; zero C0/C1. */
    memset(a0.cpu_va, 0, a0.size_bytes); memcpy(a0.cpu_va, OP0_A, sizeof(OP0_A));
    memset(b0.cpu_va, 0, b0.size_bytes); memcpy(b0.cpu_va, OP0_B, sizeof(OP0_B));
    memset(c0.cpu_va, 0, c0.size_bytes);
    memset(a1.cpu_va, 0, a1.size_bytes); memcpy(a1.cpu_va, OP1_A, sizeof(OP1_A));
    memset(b1.cpu_va, 0, b1.size_bytes); memcpy(b1.cpu_va, OP1_B, sizeof(OP1_B));
    memset(c1.cpu_va, 0, c1.size_bytes);
    msync(a0.cpu_va, a0.size_bytes, MS_SYNC);
    msync(b0.cpu_va, b0.size_bytes, MS_SYNC);
    msync(c0.cpu_va, c0.size_bytes, MS_SYNC);
    msync(a1.cpu_va, a1.size_bytes, MS_SYNC);
    msync(b1.cpu_va, b1.size_bytes, MS_SYNC);
    msync(c1.cpu_va, c1.size_bytes, MS_SYNC);

    /* Op 0 cbuf — built-in dims (CTA 4×4×1, grid 1×1×1) + the three
     * pointer args at the standard CUDA offsets. */
    memset(ctx.cbuf_va, 0, 4096);
    *(uint64_t *)((char *)ctx.cbuf_va + 0x160) = a0.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x168) = b0.gpu_va;
    *(uint64_t *)((char *)ctx.cbuf_va + 0x170) = c0.gpu_va;
    msync(ctx.cbuf_va, 4096, MS_SYNC);

    /* Op 1 cbuf — same layout, different pointers. */
    memset(cbuf1.cpu_va, 0, cbuf1.size_bytes);
    *(uint64_t *)((char *)cbuf1.cpu_va + 0x160) = a1.gpu_va;
    *(uint64_t *)((char *)cbuf1.cpu_va + 0x168) = b1.gpu_va;
    *(uint64_t *)((char *)cbuf1.cpu_va + 0x170) = c1.gpu_va;
    msync(cbuf1.cpu_va, cbuf1.size_bytes, MS_SYNC);

    /* QMD 0: standard populate + CTA 4×4 override + builtin-dims into
     * ctx.cbuf (op 0's cbuf). */
    gpu_launch_populate_qmd(&ctx);
    {
        uint32_t *qmd0 = (uint32_t *)ctx.qmd_va;
        gpu_qmd_set_bits(qmd0, QMD_CTA_THREAD_DIM0_HI, QMD_CTA_THREAD_DIM0_LO, 4);
        gpu_qmd_set_bits(qmd0, QMD_CTA_THREAD_DIM1_HI, QMD_CTA_THREAD_DIM1_LO, 4);
    }
    msync(ctx.qmd_va, 4096, MS_SYNC);
    gpu_write_builtin_dims(&ctx, 4, 4, 1, 1, 1, 1);

    /* QMD 1: byte-copy QMD0 to qmd1's page, then override the
     * CBUF[0] address fields so it points at cbuf1.gpu_va instead
     * of ctx.cbuf_gva. Everything else (PROGRAM_ADDRESS, CTA dims,
     * cache invalidates) is identical because the shader and CTA
     * shape are the same. */
    memcpy(qmd1.cpu_va, ctx.qmd_va, 256);
    {
        uint32_t *qmd1p = (uint32_t *)qmd1.cpu_va;
        gpu_qmd_set_bits(qmd1p,
                         QMD_CBUF_ADDR_LO_BASE + 31,
                         QMD_CBUF_ADDR_LO_BASE,
                         cbuf1.gpu_va & 0xFFFFFFFFu);
        gpu_qmd_set_bits(qmd1p,
                         QMD_CBUF_ADDR_HI_BASE + 16,
                         QMD_CBUF_ADDR_HI_BASE,
                         (cbuf1.gpu_va >> 32) & 0x1FFFFu);
    }
    msync(qmd1.cpu_va, qmd1.size_bytes, MS_SYNC);

    /* Op 1 also needs blockDim/gridDim in its cbuf at 0x0..0x14
     * because the shader (matmul4x4_mt_fp32) reads SR_TID directly
     * but doesn't read blockDim — so this is moot for THIS kernel,
     * but the contract is "every cbuf gets builtin dims" so future
     * parameterized kernels chained behind it work. */
    {
        uint32_t *p = (uint32_t *)cbuf1.cpu_va;
        p[0] = 4; p[1] = 4; p[2] = 1;  /* blockDim.{x,y,z} */
        p[3] = 1; p[4] = 1; p[5] = 1;  /* gridDim.{x,y,z}  */
        msync(cbuf1.cpu_va, cbuf1.size_bytes, MS_SYNC);
    }

    /* Pipeline ops array. */
    struct gpu_buffer pipe_ops = gpu_alloc_buffer(&ctx, 4096, 4096);
    {
        memset(pipe_ops.cpu_va, 0, pipe_ops.size_bytes);
        struct ga10b_pipeline_op *ops =
            (struct ga10b_pipeline_op *)pipe_ops.cpu_va;
        ops[0].qmd_gpu_va       = ctx.qmd_gva;
        ops[0].output_phys      = c0.phys;
        ops[0].expected_payload = OP0_SENTINEL_BITS;
        ops[0].flags            = 0;
        ops[1].qmd_gpu_va       = qmd1.gpu_va;
        ops[1].output_phys      = c1.phys;
        ops[1].expected_payload = OP1_SENTINEL_BITS;
        ops[1].flags            = 0;
        msync(pipe_ops.cpu_va, pipe_ops.size_bytes, MS_SYNC);
    }
    printf("[launch] pipeline ops at phys=0x%llx (2 entries, 48 B)\n",
           (unsigned long long)pipe_ops.phys);

    /* --- Linux-side dispatch: replicate what SLM-OS will do. --- */
    uint32_t pb_buf[32];
    for (int op_idx = 0; op_idx < OP_COUNT; op_idx++) {
        uint64_t qmd_gva = (op_idx == 0) ? ctx.qmd_gva : qmd1.gpu_va;
        uint64_t out_phys = (op_idx == 0) ? c0.phys : c1.phys;
        uint32_t expected = (op_idx == 0) ? OP0_SENTINEL_BITS
                                           : OP1_SENTINEL_BITS;
        struct gpu_buffer *out_buf = (op_idx == 0) ? &c0 : &c1;
        size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf, qmd_gva);
        volatile uint32_t *poll_va = (volatile uint32_t *)out_buf->cpu_va;
        printf("[launch] op[%d] dispatch (qmd_gva=0x%lx)\n",
               op_idx, (unsigned long)qmd_gva);
        int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                      expected, 2000);
        if (!ok) {
            fprintf(stderr, "[launch] op[%d] sentinel timeout\n", op_idx);
            return 1;
        }
        msync(out_buf->cpu_va, out_buf->size_bytes,
              MS_INVALIDATE | MS_SYNC);
        __asm__ volatile("dsb sy" ::: "memory");
    }

    int ok0 = validate_corner((float *)c0.cpu_va, 30.0f, "op0 (A0=[1..16])");
    int ok1 = validate_corner((float *)c1.cpu_va, 54.0f, "op1 (A1=[2..17])");
    if (!ok0 || !ok1) return 1;

    uint32_t gp_get = ((volatile uint32_t *)ctx.userd_va)
                        [GPU_LAUNCH_USERD_GP_GET_WORD];
    printf("[launch] GP_GET=%u (want 2 after 2-op pipeline)\n", gp_get);
    printf("[launch] SUCCESS: 2-op pipeline correct (Linux-side)\n");

    if (!preserve) {
        return 0;
    }

    /* --- v5 handoff for SLM-OS post-kexec re-dispatch. --- */
    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    /* Zero op outputs before kexec so SLM-OS sees a fresh "before
     * dispatch" state. The Linux-side run wrote 30.0/54.0 into c0/c1;
     * we need those zeroed so SLM-OS's poll observes the GPU re-
     * computing the sentinel. */
    memset(c0.cpu_va, 0, c0.size_bytes);
    memset(c1.cpu_va, 0, c1.size_bytes);
    msync(c0.cpu_va, c0.size_bytes, MS_SYNC);
    msync(c1.cpu_va, c1.size_bytes, MS_SYNC);

    uint64_t handoff_phys = gpu_write_handoff_v5(&ctx, handoff_va,
                                                  c1.phys, c1.gpu_va,
                                                  OP1_SENTINEL_BITS,
                                                  OP_COUNT,
                                                  pipe_ops.phys);

    printf("[launch] Handoff at phys 0x%llx (version=5, %d ops)\n",
           (unsigned long long)handoff_phys, OP_COUNT);
    printf("[launch]   pipeline_ops_phys=0x%llx\n",
           (unsigned long long)pipe_ops.phys);
    printf("[launch]   op[0] qmd=0x%lx out=0x%llx sentinel=0x%08x (30.0f)\n",
           (unsigned long)ctx.qmd_gva, (unsigned long long)c0.phys,
           OP0_SENTINEL_BITS);
    printf("[launch]   op[1] qmd=0x%lx out=0x%llx sentinel=0x%08x (54.0f)\n",
           (unsigned long)qmd1.gpu_va, (unsigned long long)c1.phys,
           OP1_SENTINEL_BITS);
    printf("[launch] Sleeping up to %d s — kexec now.\n", timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
