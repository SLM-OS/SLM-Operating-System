/*
 * gpu-kernel-sched-mlp.c — Run the AI scheduler MLP forward pass on
 * the Jetson GA10B GPU (PR-2 of docs/design/gpu-policy-models.md).
 *
 * Eight kernel dispatches chained through the v6 channel handoff:
 *
 *   Op 0  GEMM (L0)        x [1,108]   W [108,256]  → out [1,256]
 *   Op 1  Add+ReLU (L0)    + bias [256], apply_relu=1
 *   Op 2  GEMM (L1)        x [1,256]   W [256,256]  → out [1,256]
 *   Op 3  Add+ReLU (L1)    + bias [256], apply_relu=1
 *   Op 4  GEMM (L2)        x [1,256]   W [256,128]  → out [1,128]
 *   Op 5  Add+ReLU (L2)    + bias [128], apply_relu=1
 *   Op 6  GEMM (L3)        x [1,128]   W [128, 42]  → out [1, 42]
 *   Op 7  AddBias (L3)     + bias [ 42], apply_relu=0   (final logits)
 *
 * Reuses the MNIST shader kit (`gemm_fp32`, `add_bias_relu_fp32`)
 * unchanged — the GEMM kernel is shape-parameterised at dispatch
 * time and handles every (M=1, K, N) combination the sched MLP
 * needs without recompilation.
 *
 * Weights and inputs come from scripts/sched-weights/ (extracted
 * from kernel/sched/ai/ai_weights_mlp.c by
 * scripts/sched-extract-weights.py). Sentinels are a precomputed
 * (cell_idx, bits) pair per op so the per-op completion poll picks
 * a guaranteed-non-zero cell (some post-ReLU layers have a zero in
 * cell 0).
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-sched-mlp \
 *       gpu-kernel-sched-mlp.c gpu-launch-common.c
 *
 * Usage (Linux self-check, no kexec):
 *   sudo ./gpu-kernel-sched-mlp [--weights-dir PATH] [--shader-dir PATH]
 *
 * Usage (with v6 handoff for SLM-OS PR-3 to consume post-kexec):
 *   sudo ./gpu-kernel-sched-mlp --preserve-for-kexec [--timeout-secs N]
 *
 * Success criterion (PR-2): GPU logits match CPU reference within
 * a per-element absolute error of 1e-3 on every component, AND the
 * argmaxes agree. Prints a SUCCESS line on pass, FAIL on mismatch.
 */
#include "gpu-launch-common.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <math.h>

#define SCHED_OP_COUNT 8
#define SCHED_INPUT_DIM   108  /* AI_STATE_DIM, kernel/sched/ai/ai_types.h */
#define SCHED_OUTPUT_DIM  42   /* AI_SCHED_N_ACTIONS on Jetson (Pi 5: 24) */

/* Layer in/out shapes — MUST match LAYERS in
 * scripts/sched-extract-weights.py and AI_MLP_LAYER*_{IN,OUT} in
 * kernel/sched/ai/ai_types.h. The launcher hard-codes them rather
 * than reading a manifest because the constants are platform-pinned
 * and a mismatch would silently corrupt the dispatch. */
static const int LAYER_IN[]  = { SCHED_INPUT_DIM, 256, 256, 128 };
static const int LAYER_OUT[] = { 256,             256, 128,  42 };

static volatile sig_atomic_t g_shutdown;
static void on_term(int sig) { (void)sig; g_shutdown = 1; }

static uint32_t round_up_page(size_t bytes)
{
    size_t r = (bytes + 4095u) & ~((size_t)4095);
    if (r == 0) r = 4096;
    if (r > UINT32_MAX) {
        fprintf(stderr, "size too large (%zu)\n", r);
        exit(1);
    }
    return (uint32_t)r;
}

/* Load `path` into the cpu_va of an already-allocated GPU buffer.
 * Used for weights and the input vector. */
static void load_into_buffer(struct gpu_buffer *buf,
                              const char *path,
                              size_t expected_bytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); exit(1); }
    if (fread(buf->cpu_va, 1, expected_bytes, fp) != expected_bytes) {
        fprintf(stderr, "short read on %s\n", path); exit(1);
    }
    fclose(fp);
    msync(buf->cpu_va, buf->size_bytes, MS_SYNC);
}

static void make_path(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "path too long: %s/%s\n", dir, name);
        exit(1);
    }
}

/* Per-op metadata. One QMD page + one cbuf page + one output buffer
 * per op; sentinels read from pipeline_sentinels.bin. */
struct sched_op {
    const char *name;
    struct gpu_buffer qmd_page;
    struct gpu_buffer cbuf_page;
    struct gpu_buffer output;
    uint32_t output_bytes;
    uint32_t sentinel_cell_idx;
    uint32_t sentinel_bits;
};

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    bool preserve = false;
    int timeout_secs = 900;
    const char *weights_dir = "./sched-weights";
    const char *shader_dir  = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--preserve-for-kexec") == 0) {
            preserve = true;
        } else if (strcmp(argv[i], "--timeout-secs") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
            if (timeout_secs <= 0) timeout_secs = 900;
        } else if (strcmp(argv[i], "--weights-dir") == 0 && i + 1 < argc) {
            weights_dir = argv[++i];
        } else if (strcmp(argv[i], "--shader-dir") == 0 && i + 1 < argc) {
            shader_dir = argv[++i];
        }
    }

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* --- Channel + shader setup. The "primary" shader passed to
     * gpu_launch_setup is gemm_fp32 since it's used for the first op
     * (and four of the eight). add_bias_relu loads alongside. */
    struct gpu_launch_ctx ctx = {0};
    char path[512];
    make_path(path, sizeof(path), shader_dir, "gemm_fp32_shader.sass");
    size_t gemm_size = 0;
    gpu_launch_setup(&ctx, path, &gemm_size);
    printf("[sched] gemm shader %zu bytes (in ctx)\n", gemm_size);

    size_t addrelu_size = 0;
    make_path(path, sizeof(path), shader_dir, "add_bias_relu_fp32_shader.sass");
    struct gpu_buffer addrelu_shader =
        gpu_load_shader_buffer(&ctx, path, &addrelu_size);
    printf("[sched] addrelu shader %zu bytes\n", addrelu_size);

    uint64_t gemm_shader_gva    = ctx.shader_gva;
    uint64_t addrelu_shader_gva = addrelu_shader.gpu_va;

    /* --- Weight + bias + input buffers. Total ~516 KB of weights;
     * each weight is rounded up to the nearest 4 KB for the alloc
     * but we only `msync` the actual payload bytes. */
    struct gpu_buffer W[4], b[4];
    static const size_t W_bytes[4] = {
        108u * 256u * 4u,   /*  110592 */
        256u * 256u * 4u,   /*  262144 */
        256u * 128u * 4u,   /*  131072 */
        128u *  42u * 4u,   /*   21504 */
    };
    static const size_t b_bytes[4] = {
        256u * 4u,
        256u * 4u,
        128u * 4u,
         42u * 4u,
    };
    for (int li = 0; li < 4; li++) {
        W[li] = gpu_alloc_buffer(&ctx, round_up_page(W_bytes[li]), 4096);
        b[li] = gpu_alloc_buffer(&ctx, round_up_page(b_bytes[li]), 4096);
        char p[512], q[512];
        snprintf(p, sizeof(p), "W%d.bin", li);
        snprintf(q, sizeof(q), "b%d.bin", li);
        char wp[512], bp[512];
        make_path(wp, sizeof(wp), weights_dir, p);
        make_path(bp, sizeof(bp), weights_dir, q);
        load_into_buffer(&W[li], wp, W_bytes[li]);
        load_into_buffer(&b[li], bp, b_bytes[li]);
    }

    struct gpu_buffer input = gpu_alloc_buffer(&ctx, 4096, 4096);  /* 432 B */
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "input_synth.bin");
        load_into_buffer(&input, p, SCHED_INPUT_DIM * 4u);
    }
    printf("[sched] weights + input loaded\n");

    /* --- Read per-op sentinel (cell_idx, bits) from disk. --- */
    uint32_t sentinel_idx[SCHED_OP_COUNT] = {0};
    uint32_t sentinel_bits[SCHED_OP_COUNT] = {0};
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "pipeline_sentinels.bin");
        FILE *fp = fopen(p, "rb");
        if (!fp) { perror(p); exit(1); }
        for (int i = 0; i < SCHED_OP_COUNT; i++) {
            if (fread(&sentinel_idx[i],  4, 1, fp) != 1 ||
                fread(&sentinel_bits[i], 4, 1, fp) != 1) {
                fprintf(stderr, "short read on %s\n", p); exit(1);
            }
        }
        fclose(fp);
    }

    /* --- Per-op metadata + buffer alloc.
     *
     * 8 ops in (gemm, addrelu, gemm, addrelu, gemm, addrelu, gemm, addbias)
     * order. Output sizes follow LAYER_OUT[]: each gemm and the
     * matching addrelu both have output size LAYER_OUT[li] floats
     * (the addrelu is element-wise and preserves shape). */
    static const char *op_names[SCHED_OP_COUNT] = {
        "00_gemm0",     "01_addrelu0",
        "02_gemm1",     "03_addrelu1",
        "04_gemm2",     "05_addrelu2",
        "06_gemm3",     "07_addbias3",
    };
    struct sched_op ops[SCHED_OP_COUNT];
    for (int i = 0; i < SCHED_OP_COUNT; i++) {
        int li = i / 2;                          /* layer index 0..3 */
        size_t out_floats = (size_t)LAYER_OUT[li];
        size_t out_bytes  = out_floats * 4u;

        ops[i].name              = op_names[i];
        ops[i].qmd_page          = gpu_alloc_buffer(&ctx, 4096, 4096);
        ops[i].cbuf_page         = gpu_alloc_buffer(&ctx, 4096, 4096);
        ops[i].output            = gpu_alloc_buffer(&ctx,
                                                     round_up_page(out_bytes),
                                                     4096);
        ops[i].output_bytes      = (uint32_t)out_bytes;
        ops[i].sentinel_cell_idx = sentinel_idx[i];
        ops[i].sentinel_bits     = sentinel_bits[i];
    }

    /* --- Per-op cbuf + QMD population. ---
     *
     * Pattern repeats four times: gemm followed by addrelu (or addbias
     * on the final layer). The gemm cbuf layout is gemm_fp32.cu's
     * (a, b, c, M, K, N); the addrelu cbuf is add_bias_relu_fp32.cu's
     * (x, bias, out, N, C, H, W, apply_relu). Both shaders use the
     * 0x160-based cbuf scalar offset baseline.
     *
     * For the first gemm the input is `input` (the synthetic feature
     * vector); subsequent gemms read from the previous addrelu's
     * output. Each addrelu reads from the matching gemm's output.
     */
    for (int li = 0; li < 4; li++) {
        int gemm_op = li * 2;
        int post_op = li * 2 + 1;
        int K = LAYER_IN[li];
        int N = LAYER_OUT[li];
        int M = 1;

        /* GEMM op. 16×16 thread CTA, ceil(N/16) × ceil(M/16) grid. */
        {
            struct sched_op *op = &ops[gemm_op];
            uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
            memset(cbuf, 0, op->cbuf_page.size_bytes);
            uint32_t grid_x = ((uint32_t)N + 15u) / 16u;
            uint32_t grid_y = ((uint32_t)M + 15u) / 16u;
            cbuf[0] = 16; cbuf[1] = 16; cbuf[2] = 1;
            cbuf[3] = grid_x; cbuf[4] = grid_y; cbuf[5] = 1;

            /* Input pointer: `input` for L0, previous addrelu output
             * for L1..L3. */
            uint64_t a_gva = (li == 0) ? input.gpu_va
                                       : ops[post_op - 2].output.gpu_va;
            *(uint64_t *)((char *)cbuf + 0x160) = a_gva;
            *(uint64_t *)((char *)cbuf + 0x168) = W[li].gpu_va;
            *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
            *(int32_t  *)((char *)cbuf + 0x178) = M;
            *(int32_t  *)((char *)cbuf + 0x17C) = K;
            *(int32_t  *)((char *)cbuf + 0x180) = N;
            msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

            uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
            gpu_populate_qmd_at(qmd, gemm_shader_gva, op->cbuf_page.gpu_va,
                                GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
            gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                             QMD_CTA_THREAD_DIM0_LO, 16);
            gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM1_HI,
                             QMD_CTA_THREAD_DIM1_LO, 16);
            gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                             QMD_CTA_RASTER_WIDTH_LO, grid_x);
            gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                             QMD_CTA_RASTER_HEIGHT_LO, grid_y);
            msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
        }

        /* AddRelu / AddBias op. 1D row treated as N=1 C=N H=1 W=1.
         * apply_relu = 1 for layers 0..2; 0 for layer 3 (final). */
        {
            struct sched_op *op = &ops[post_op];
            uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
            memset(cbuf, 0, op->cbuf_page.size_bytes);
            cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;          /* blockDim */
            cbuf[3] = 1;   cbuf[4] = (uint32_t)N; cbuf[5] = 1; /* grid */
            *(uint64_t *)((char *)cbuf + 0x160) = ops[gemm_op].output.gpu_va;
            *(uint64_t *)((char *)cbuf + 0x168) = b[li].gpu_va;
            *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
            *(int32_t  *)((char *)cbuf + 0x178) = 1;          /* N */
            *(int32_t  *)((char *)cbuf + 0x17C) = N;          /* C */
            *(int32_t  *)((char *)cbuf + 0x180) = 1;          /* H */
            *(int32_t  *)((char *)cbuf + 0x184) = 1;          /* W */
            *(int32_t  *)((char *)cbuf + 0x188) = (li == 3) ? 0 : 1; /* apply_relu */
            msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

            uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
            gpu_populate_qmd_at(qmd, addrelu_shader_gva, op->cbuf_page.gpu_va,
                                GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
            gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                             QMD_CTA_THREAD_DIM0_LO, 256);
            gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                             QMD_CTA_RASTER_WIDTH_LO, 1);
            gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                             QMD_CTA_RASTER_HEIGHT_LO, (uint32_t)N);
            msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
        }
    }

    /* Pre-zero every output buffer so the per-cell sentinel poll
     * sees a 0 → expected transition (any-non-zero mode). */
    for (int i = 0; i < SCHED_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    printf("[sched] all 8 ops prepared. Linux-side dispatch:\n");

    /* --- Linux-side dispatch loop. Same shape as gpu-kernel-mnist's
     * pre-kexec validation: submit each op, poll its sentinel cell
     * for any-non-zero (expected_payload=0), advance. The bit
     * pattern actually written is captured for printing alongside
     * the CPU reference — they may ULP-diverge from the kernel's
     * fma-ordered accumulation but should land in the same range. */
    uint32_t actual_sentinel_bits[SCHED_OP_COUNT];
    uint32_t pb_buf[32];
    for (int i = 0; i < SCHED_OP_COUNT; i++) {
        struct sched_op *op = &ops[i];
        size_t pb_dwords = gpu_build_launch_pushbuffer(pb_buf,
                                                        op->qmd_page.gpu_va);
        volatile uint32_t *poll_va =
            (volatile uint32_t *)((char *)op->output.cpu_va +
                                   op->sentinel_cell_idx * 4u);
        int ok = gpu_submit_and_poll(&ctx, pb_buf, pb_dwords, poll_va,
                                      0u, /* any non-zero */ 5000);
        msync(op->output.cpu_va, op->output.size_bytes,
              MS_INVALIDATE | MS_SYNC);
        __asm__ volatile("dsb sy" ::: "memory");
        actual_sentinel_bits[i] = *poll_va;
        printf("[sched]   op[%d %s] qmd_gva=0x%lx sentinel cell %u: "
               "got=0x%08x (CPU ref=0x%08x, %s)\n",
               i, op->name,
               (unsigned long)op->qmd_page.gpu_va,
               op->sentinel_cell_idx,
               actual_sentinel_bits[i],
               op->sentinel_bits,
               actual_sentinel_bits[i] == op->sentinel_bits
                   ? "exact" : "ULP-divergent");
        if (!ok) {
            fprintf(stderr,
                    "[sched] op[%d %s] sentinel timeout (no non-zero "
                    "write at cell %u)\n",
                    i, op->name, op->sentinel_cell_idx);
            return 1;
        }
    }
    printf("[sched] all 8 ops fired Linux-side\n");

    /* --- Validate final logits against the CPU reference. --- */
    float logits[SCHED_OUTPUT_DIM];
    memcpy(logits, ops[7].output.cpu_va, sizeof(logits));

    float expected[SCHED_OUTPUT_DIM];
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "expected_logits.bin");
        FILE *fp = fopen(p, "rb");
        if (!fp) { perror(p); return 1; }
        if (fread(expected, 1, sizeof(expected), fp) != sizeof(expected)) {
            fprintf(stderr, "short read on %s\n", p); return 1;
        }
        fclose(fp);
    }

    int argmax_gpu = 0, argmax_cpu = 0;
    float max_g = logits[0], max_c = expected[0];
    float max_abs_err = 0.0f;
    for (int i = 0; i < SCHED_OUTPUT_DIM; i++) {
        if (logits[i]   > max_g) { max_g = logits[i];   argmax_gpu = i; }
        if (expected[i] > max_c) { max_c = expected[i]; argmax_cpu = i; }
        float e = fabsf(logits[i] - expected[i]);
        if (e > max_abs_err) max_abs_err = e;
    }

    printf("[sched] GPU logits (first 8): ");
    for (int i = 0; i < 8; i++) printf("% .4f ", (double)logits[i]);
    printf("...\n");
    printf("[sched] CPU logits (first 8): ");
    for (int i = 0; i < 8; i++) printf("% .4f ", (double)expected[i]);
    printf("...\n");
    printf("[sched] argmax: GPU=%d CPU=%d  max|err|=%.6f\n",
           argmax_gpu, argmax_cpu, (double)max_abs_err);

    /* Tolerance: 1e-3 absolute is loose enough to absorb the per-layer
     * fp32 ULP drift accumulated across 4 GEMMs of size-256 inputs.
     * The MNIST launcher uses the same threshold for its smaller
     * pipeline. The argmax check is the strict-correctness gate;
     * the magnitude check guards against catastrophic divergence. */
    int all_ok = (argmax_gpu == argmax_cpu) && (max_abs_err < 1e-3f);
    if (!all_ok) {
        fprintf(stderr,
                "[sched] FAIL: divergence > 1e-3 or argmax mismatch\n");
        return 1;
    }
    printf("[sched] SUCCESS: GPU and CPU agree on action %d\n", argmax_gpu);

    if (!preserve) return 0;

    /* --- v6 handoff for SLM-OS post-kexec consumption. ---
     *
     * Tagged with `pipeline_kind = GA10B_PIPELINE_KIND_SCHED_MLP` (PR-3
     * of gpu-policy-models.md). SLM-OS's per-kind handoff scan in
     * `ga10b_bringup_channel(b, KIND_SCHED_MLP)` finds this in DRAM
     * even when an MNIST handoff coexists from a separately-staged
     * `gpu-kernel-mnist --preserve-for-kexec` run. */
    struct gpu_buffer pipe_ops = gpu_alloc_buffer(&ctx, 4096, 4096);
    {
        memset(pipe_ops.cpu_va, 0, pipe_ops.size_bytes);
        struct ga10b_pipeline_op *p =
            (struct ga10b_pipeline_op *)pipe_ops.cpu_va;
        for (int i = 0; i < SCHED_OP_COUNT; i++) {
            p[i].qmd_gpu_va       = ops[i].qmd_page.gpu_va;
            p[i].output_phys      = ops[i].output.phys +
                                    ops[i].sentinel_cell_idx * 4u;
            p[i].expected_payload = 0u;
            p[i].flags            = 0;
        }
        msync(pipe_ops.cpu_va, pipe_ops.size_bytes, MS_SYNC);
    }
    for (int i = 0; i < SCHED_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v6(&ctx, handoff_va,
        ops[7].output.phys, ops[7].output.gpu_va,
        ops[7].sentinel_bits,
        SCHED_OP_COUNT, pipe_ops.phys,
        input.phys, (uint32_t)(SCHED_INPUT_DIM * 4u),
        GA10B_PIPELINE_KIND_SCHED_MLP);

    printf("[sched] Handoff at phys 0x%llx (version=6, %d ops)\n",
           (unsigned long long)handoff_phys, SCHED_OP_COUNT);
    printf("[sched]   pipeline_ops_phys=0x%llx\n",
           (unsigned long long)pipe_ops.phys);
    printf("[sched]   input_buf_phys=0x%llx (%u bytes — runtime swap)\n",
           (unsigned long long)input.phys,
           (unsigned)(SCHED_INPUT_DIM * 4u));
    printf("[sched]   final logits at phys 0x%llx (%d fp32)\n",
           (unsigned long long)ops[7].output.phys, SCHED_OUTPUT_DIM);
    printf("[sched] Sleeping up to %d s — kexec now (when PR-3 lands).\n",
           timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
