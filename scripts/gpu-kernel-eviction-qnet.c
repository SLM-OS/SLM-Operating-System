/*
 * gpu-kernel-eviction-qnet.c — Run the eviction-MLP forward pass on
 * the Jetson GA10B GPU (PR-5 of docs/design/gpu-policy-models.md).
 *
 * Nine kernel dispatches chained through the v6 channel handoff, each
 * over a batch of B candidate rows (`--batch`, default 256; #1020):
 *
 *   Op 0  GEMM (L1)        x [B,27]   W [27,64]  → out [B,64]
 *   Op 1  Add+ReLU (L1)    + bias [64], apply_relu=1
 *   Op 2  GEMM (L2)        x [B,64]   W [64,32]  → out [B,32]
 *   Op 3  Add+ReLU (L2)    + bias [32], apply_relu=1
 *   Op 4  GEMM (L3)        x [B,32]   W [32,16]  → out [B,16]
 *   Op 5  Add+ReLU (L3)    + bias [16], apply_relu=1
 *   Op 6  GEMM (OUT)       x [B,16]   W [16, 1]  → out [B, 1]
 *   Op 7  AddBias (OUT)    + bias [ 1], apply_relu=0
 *   Op 8  Sigmoid (OUT)    x [B]                 → out [B]
 *
 * One dispatch scores every candidate of a `select_victim`: SLM-OS
 * writes up to B rows of 27 features into the input buffer and reads
 * back that many scores from the start of op 8's output. Unused rows
 * are computed and ignored. Row r of every intermediate buffer is
 * candidate r, so a caller writing fewer than B rows still finds its
 * scores at the front. No new SASS: GEMM takes M = B, AddBiasRelu
 * takes N = B on blockIdx.z, and sigmoid is element-wise. A
 * --batch 1 run stages the original single-row pipeline.
 *
 * Direct analog of scripts/gpu-kernel-sched-mlp.c with three
 * differences:
 *
 *   - 4 layers (27→64→32→16→1) instead of (108→256→256→128→42),
 *     so the per-buffer byte counts shrink ~10×.
 *   - A 9th trailing op runs the new sigmoid_fp32 SASS shader on the
 *     single-element OUT addbias result.
 *   - Final-output validation switches from "argmax + |err| < 1e-3"
 *     to "scalar |gpu - cpu| < 1e-4" (the eviction MLP emits a
 *     single fp32 score in [0, 1]).
 *
 * Reuses the existing GEMM + AddBiasRelu shader kit and adds
 * sigmoid_fp32 (one new SASS). The GEMM kernel is shape-parameterised
 * at dispatch time and handles every (M=1, K, N) combination the
 * eviction MLP needs without recompilation. AddBiasRelu's existing
 * `apply_relu` cbuf flag covers both hidden-layer (=1) and OUT-layer
 * (=0) cases.
 *
 * Weights and inputs come from scripts/eviction-weights/ (extracted
 * from runtime/src/mm/eviction/generated/mlp_policy_f32.rs by
 * scripts/eviction-extract-weights.py). Sentinels are a precomputed
 * (cell_idx, bits) pair per op so the per-op completion poll picks
 * a guaranteed-non-zero cell (some post-ReLU layers have a zero in
 * cell 0).
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-eviction-qnet \
 *       gpu-kernel-eviction-qnet.c gpu-launch-common.c
 *
 * Usage (Linux self-check, no kexec):
 *   sudo ./gpu-kernel-eviction-qnet [--weights-dir PATH] [--shader-dir PATH]
 *                                   [--batch N]
 *
 * Usage (with v6 handoff for SLM-OS PR-6 to consume post-kexec):
 *   sudo ./gpu-kernel-eviction-qnet --preserve-for-kexec [--timeout-secs N]
 *
 * Success criterion (PR-5): GPU score matches CPU reference within
 * 1e-4 absolute error — for row 0 against expected_score.bin, and for
 * every row against an fp32 forward pass computed here. Prints a SUCCESS line on pass, FAIL on
 * mismatch. The eviction MLP is technically a Q-net in the simulator's
 * RL framing — hence the `EVICTION_QNET` pipeline_kind constant — but
 * the deployed model is a feed-forward sigmoid MLP, so "score" is the
 * accurate term for the output here.
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

#define EVICT_OP_COUNT    9
#define EVICT_INPUT_DIM   27   /* BlockFeatures = [f32; 27] (runtime_mlp.rs) */
#define EVICT_OUTPUT_DIM   1   /* single fp32 eviction score in [0, 1] */
/* Rows per dispatch (#1020). Defaults to the maximum: a decision with
 * more candidates than this falls back to the CPU (`oversize`), and
 * the weight pool alone has 512 blocks, while the extra GPU work for
 * unused rows is negligible at these layer sizes. */
#define EVICT_BATCH_DEFAULT 256
/* Op 8 (sigmoid) runs as a single 256-thread CTA, and the Linux-side
 * completion check below waits on every score cell of it. */
#define EVICT_BATCH_MAX   256

/* Layer in/out shapes — MUST match LAYERS in
 * scripts/eviction-extract-weights.py and L*_IN / L*_OUT in
 * runtime/src/mm/eviction/runtime_mlp.rs. Hard-coded rather than read
 * from a manifest because the constants are spec-pinned and a
 * mismatch would silently corrupt the dispatch. */
static const int LAYER_IN[]  = { EVICT_INPUT_DIM, 64, 32, 16 };
static const int LAYER_OUT[] = { 64,              32, 16,  1 };

/* Per-layer file-name pairs match the Rust source identifiers
 * (W_L1/B_L1, W_L2/B_L2, W_L3/B_L3, W_OUT/B_OUT) so the launcher's
 * disk layout mirrors mlp_policy_f32.rs symbol-for-symbol. */
static const char *const W_NAMES[] = { "W_L1", "W_L2", "W_L3", "W_OUT" };
static const char *const B_NAMES[] = { "B_L1", "B_L2", "B_L3", "B_OUT" };

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

/* Plain fp32 forward pass over one feature row, from the weight
 * buffers' CPU mappings. W is [K][N] row-major, as gemm_fp32 reads it
 * (b[k * N + j]). The Linux self-check compares every GPU row to this. */
static float cpu_reference_score(const float *x, struct gpu_buffer W[4],
                                 struct gpu_buffer b[4])
{
    float act[2][64];
    const float *in = x;
    for (int li = 0; li < 4; li++) {
        const float *w    = (const float *)W[li].cpu_va;
        const float *bias = (const float *)b[li].cpu_va;
        float *out = act[li & 1];
        for (int j = 0; j < LAYER_OUT[li]; j++) {
            float sum = 0.0f;
            for (int k = 0; k < LAYER_IN[li]; k++) {
                sum += in[k] * w[k * LAYER_OUT[li] + j];
            }
            sum += bias[j];
            if (li < 3 && sum < 0.0f) sum = 0.0f;
            out[j] = sum;
        }
        in = out;
    }
    return 1.0f / (1.0f + expf(-in[0]));
}

/* Per-op metadata. One QMD page + one cbuf page + one output buffer
 * per op; sentinels read from pipeline_sentinels.bin. */
struct evict_op {
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
    const char *weights_dir = "./eviction-weights";
    const char *shader_dir  = ".";
    int batch = EVICT_BATCH_DEFAULT;
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
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            batch = atoi(argv[++i]);
        }
    }
    if (batch < 1 || batch > EVICT_BATCH_MAX) {
        fprintf(stderr, "--batch must be 1..%d (got %d)\n",
                EVICT_BATCH_MAX, batch);
        return 1;
    }
    const size_t B = (size_t)batch;
    printf("[evict] batch = %zu rows per dispatch\n", B);

    struct sigaction sa = { .sa_handler = on_term };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* --- Channel + shader setup. The "primary" shader passed to
     * gpu_launch_setup is gemm_fp32 since it's used for the first op
     * (and four of the nine). add_bias_relu and sigmoid load
     * alongside. */
    struct gpu_launch_ctx ctx = {0};
    char path[512];
    make_path(path, sizeof(path), shader_dir, "gemm_fp32_shader.sass");
    size_t gemm_size = 0;
    gpu_launch_setup(&ctx, path, &gemm_size);
    printf("[evict] gemm shader %zu bytes (in ctx)\n", gemm_size);

    size_t addrelu_size = 0;
    make_path(path, sizeof(path), shader_dir, "add_bias_relu_fp32_shader.sass");
    struct gpu_buffer addrelu_shader =
        gpu_load_shader_buffer(&ctx, path, &addrelu_size);
    printf("[evict] addrelu shader %zu bytes\n", addrelu_size);

    size_t sigmoid_size = 0;
    make_path(path, sizeof(path), shader_dir, "sigmoid_fp32_shader.sass");
    struct gpu_buffer sigmoid_shader =
        gpu_load_shader_buffer(&ctx, path, &sigmoid_size);
    printf("[evict] sigmoid shader %zu bytes\n", sigmoid_size);

    uint64_t gemm_shader_gva    = ctx.shader_gva;
    uint64_t addrelu_shader_gva = addrelu_shader.gpu_va;
    uint64_t sigmoid_shader_gva = sigmoid_shader.gpu_va;

    /* --- Weight + bias + input buffers. Total ~17 KB of weights,
     * matching the PAYLOAD_LEN_V1 byte budget in runtime_mlp.rs minus
     * the 8-byte blob header. Each weight is rounded up to the
     * nearest 4 KB for the alloc but we only msync the actual
     * payload bytes. */
    struct gpu_buffer W[4], b[4];
    static const size_t W_bytes[4] = {
         27u *  64u * 4u,   /*  6,912 */
         64u *  32u * 4u,   /*  8,192 */
         32u *  16u * 4u,   /*  2,048 */
         16u *   1u * 4u,   /*     64 */
    };
    static const size_t b_bytes[4] = {
         64u * 4u,
         32u * 4u,
         16u * 4u,
          1u * 4u,
    };
    for (int li = 0; li < 4; li++) {
        W[li] = gpu_alloc_buffer(&ctx, round_up_page(W_bytes[li]), 4096);
        b[li] = gpu_alloc_buffer(&ctx, round_up_page(b_bytes[li]), 4096);
        char wp[512], bp[512];
        char wfile[64], bfile[64];
        snprintf(wfile, sizeof(wfile), "%s.bin", W_NAMES[li]);
        snprintf(bfile, sizeof(bfile), "%s.bin", B_NAMES[li]);
        make_path(wp, sizeof(wp), weights_dir, wfile);
        make_path(bp, sizeof(bp), weights_dir, bfile);
        load_into_buffer(&W[li], wp, W_bytes[li]);
        load_into_buffer(&b[li], bp, b_bytes[li]);
    }

    /* B rows × 27 features. Row 0 is the synthetic input the
     * extractor's expected_score.bin was computed from; rows 1..B-1
     * are deterministic perturbations of it, so the self-check below
     * catches a batched-indexing bug (every row scored as row 0, or
     * rows swapped) that identical rows would hide. */
    const size_t input_bytes = B * EVICT_INPUT_DIM * 4u;
    struct gpu_buffer input =
        gpu_alloc_buffer(&ctx, round_up_page(input_bytes), 4096);
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "input_synth.bin");
        load_into_buffer(&input, p, EVICT_INPUT_DIM * 4u);
        float *rows = (float *)input.cpu_va;
        for (size_t r = 1; r < B; r++) {
            for (int k = 0; k < EVICT_INPUT_DIM; k++) {
                float s = (float)((int)((r * 7u + (size_t)k * 3u) % 9u) - 4)
                          / 4.0f;   /* in [-1, 1] */
                rows[r * EVICT_INPUT_DIM + k] =
                    rows[k] * (1.0f + 0.1f * s) + 0.05f * s;
            }
        }
        msync(input.cpu_va, input.size_bytes, MS_SYNC);
    }
    printf("[evict] weights + %zu input rows loaded\n", B);

    /* --- Read per-op sentinel (cell_idx, bits) from disk. --- */
    uint32_t sentinel_idx[EVICT_OP_COUNT] = {0};
    uint32_t sentinel_bits[EVICT_OP_COUNT] = {0};
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "pipeline_sentinels.bin");
        FILE *fp = fopen(p, "rb");
        if (!fp) { perror(p); exit(1); }
        for (int i = 0; i < EVICT_OP_COUNT; i++) {
            if (fread(&sentinel_idx[i],  4, 1, fp) != 1 ||
                fread(&sentinel_bits[i], 4, 1, fp) != 1) {
                fprintf(stderr, "short read on %s\n", p); exit(1);
            }
        }
        fclose(fp);
    }

    /* --- Per-op metadata + buffer alloc.
     *
     * 9 ops in (gemm, addrelu, gemm, addrelu, gemm, addrelu, gemm,
     * addbias, sigmoid) order. Output sizes for ops 0..7 follow
     * LAYER_OUT[]; op 8 (sigmoid) operates on the single OUT cell. */
    static const char *op_names[EVICT_OP_COUNT] = {
        "00_gemmL1",   "01_addreluL1",
        "02_gemmL2",   "03_addreluL2",
        "04_gemmL3",   "05_addreluL3",
        "06_gemmOUT",  "07_addbiasOUT",
        "08_sigmoid",
    };
    struct evict_op ops[EVICT_OP_COUNT];
    for (int i = 0; i < EVICT_OP_COUNT; i++) {
        size_t out_floats;
        if (i < 8) {
            int li = i / 2;
            out_floats = B * (size_t)LAYER_OUT[li];
        } else {
            /* Sigmoid is element-wise on the OUT-layer output cells. */
            out_floats = B * (size_t)EVICT_OUTPUT_DIM;
        }
        size_t out_bytes = out_floats * 4u;

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
     * Pattern (mirrors gpu-kernel-sched-mlp.c): for each of 4 layers,
     * a gemm + add_bias_relu (or add_bias-no-relu for OUT). After all
     * 8 layer-ops, a trailing sigmoid op consumes the OUT-layer
     * addbias output.
     */
    for (int li = 0; li < 4; li++) {
        int gemm_op = li * 2;
        int post_op = li * 2 + 1;
        int K = LAYER_IN[li];
        int N = LAYER_OUT[li];
        int M = batch;

        /* GEMM op. 16×16 thread CTA, ceil(N/16) × ceil(M/16) grid,
         * one output row per candidate. */
        {
            struct evict_op *op = &ops[gemm_op];
            uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
            memset(cbuf, 0, op->cbuf_page.size_bytes);
            uint32_t grid_x = ((uint32_t)N + 15u) / 16u;
            uint32_t grid_y = ((uint32_t)M + 15u) / 16u;
            cbuf[0] = 16; cbuf[1] = 16; cbuf[2] = 1;
            cbuf[3] = grid_x; cbuf[4] = grid_y; cbuf[5] = 1;

            /* Input pointer: `input` for L1, previous addrelu output
             * for L2..L3, addrelu output of L3 for OUT. */
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

        /* AddRelu / AddBias op. The [M, N] GEMM output is treated as
         * NCHW with n = row (blockIdx.z), c = column (blockIdx.y),
         * H = W = 1, so bias[c] broadcasts across rows.
         * apply_relu = 1 for layers 0..2 (hidden); 0 for layer 3
         * (OUT, sigmoid follows in a separate op). */
        {
            struct evict_op *op = &ops[post_op];
            uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
            memset(cbuf, 0, op->cbuf_page.size_bytes);
            cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;          /* blockDim */
            cbuf[3] = 1;   cbuf[4] = (uint32_t)N;             /* grid */
            cbuf[5] = (uint32_t)M;
            *(uint64_t *)((char *)cbuf + 0x160) = ops[gemm_op].output.gpu_va;
            *(uint64_t *)((char *)cbuf + 0x168) = b[li].gpu_va;
            *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
            *(int32_t  *)((char *)cbuf + 0x178) = M;          /* N */
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
            gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                             QMD_CTA_RASTER_DEPTH_LO, (uint32_t)M);
            msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
        }
    }

    /* Sigmoid op (op 8): element-wise on the B OUT cells.
     * cbuf layout per scripts/cuda/sigmoid_fp32.cu:
     *   [0x160] x        (const float *)
     *   [0x168] out      (float *)
     *   [0x170] N        (int32_t)
     * blockDim.x = 256, grid.x = ceil(B/256) = 1 (B <= EVICT_BATCH_MAX).
     * The kernel's early-return on `i >= N` masks the unused lanes. */
    {
        struct evict_op *op = &ops[8];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;            /* blockDim */
        cbuf[3] = 1;   cbuf[4] = 1;   cbuf[5] = 1;          /* grid */
        *(uint64_t *)((char *)cbuf + 0x160) = ops[7].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x170) = batch * EVICT_OUTPUT_DIM;
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, sigmoid_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, 1);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 1);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Pre-zero every output buffer so the per-cell sentinel poll
     * sees a 0 → expected transition (any-non-zero mode). */
    for (int i = 0; i < EVICT_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    printf("[evict] all 9 ops prepared. Linux-side dispatch:\n");

    /* --- Linux-side dispatch loop. Submit each op, poll its sentinel
     * cell for any-non-zero (expected_payload=0), advance. The bit
     * pattern actually written is captured for printing alongside
     * the CPU reference — they may ULP-diverge from the kernel's
     * fma-ordered accumulation but should land in the same range. */
    uint32_t actual_sentinel_bits[EVICT_OP_COUNT];
    uint32_t pb_buf[32];
    for (int i = 0; i < EVICT_OP_COUNT; i++) {
        struct evict_op *op = &ops[i];
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
        printf("[evict]   op[%d %s] qmd_gva=0x%lx sentinel cell %u: "
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
                    "[evict] op[%d %s] sentinel timeout (no non-zero "
                    "write at cell %u)\n",
                    i, op->name, op->sentinel_cell_idx);
            return 1;
        }
    }
    printf("[evict] all 9 ops fired Linux-side\n");

    /* The op-8 poll above watched row 0's score only. Sigmoid output
     * is strictly positive, so wait (bounded) for every row's cell to
     * leave its pre-zeroed state before reading the batch. */
    const volatile uint32_t *score_bits =
        (const volatile uint32_t *)ops[8].output.cpu_va;
    for (int tries = 0; tries < 1000; tries++) {
        msync(ops[8].output.cpu_va, ops[8].output.size_bytes,
              MS_INVALIDATE | MS_SYNC);
        __asm__ volatile("dsb sy" ::: "memory");
        size_t r = 0;
        while (r < B && score_bits[r] != 0u) r++;
        if (r == B) break;
        usleep(100);
    }

    /* --- Validate every row against the fp32 CPU reference. --- */
    const float *scores = (const float *)ops[8].output.cpu_va;
    const float *rows   = (const float *)input.cpu_va;
    float worst_err = 0.0f;
    size_t worst_row = 0;
    for (size_t r = 0; r < B; r++) {
        float ref = cpu_reference_score(&rows[r * EVICT_INPUT_DIM], W, b);
        float err = fabsf(scores[r] - ref);
        if (!(err <= worst_err)) {   /* also catches NaN */
            worst_err = err;
            worst_row = r;
        }
    }
    printf("[evict] batch rows: worst |gpu - cpu| = %.3e (row %zu)\n",
           (double)worst_err, worst_row);
    if (!(worst_err < 1e-4f)) {
        fprintf(stderr,
                "[evict] FAIL: row %zu |err| %.3e >= 1e-4 tolerance "
                "(gpu=%.9f)\n",
                worst_row, (double)worst_err, (double)scores[worst_row]);
        return 1;
    }

    /* --- Row 0 against the extractor's reference score. --- */
    float score;
    memcpy(&score, ops[8].output.cpu_va, sizeof(score));

    float expected;
    {
        char p[512];
        make_path(p, sizeof(p), weights_dir, "expected_score.bin");
        FILE *fp = fopen(p, "rb");
        if (!fp) { perror(p); return 1; }
        if (fread(&expected, 1, sizeof(expected), fp) != sizeof(expected)) {
            fprintf(stderr, "short read on %s\n", p); return 1;
        }
        fclose(fp);
    }

    float abs_err = fabsf(score - expected);
    printf("[evict] GPU score = %.9f\n", (double)score);
    printf("[evict] CPU score = %.9f\n", (double)expected);
    printf("[evict] |err| = %.3e\n", (double)abs_err);

    /* Tolerance: 1e-4 absolute. fp32 accumulation through 4 small
     * matvecs + a single sigmoid is numerically tight; the looser
     * 1e-4 (vs the sched MLP's 1e-3 across 42 logits) is achievable
     * because we have only one output element and shorter dot
     * products. */
    if (abs_err >= 1e-4f) {
        fprintf(stderr,
                "[evict] FAIL: |err| %.3e >= 1e-4 tolerance\n",
                (double)abs_err);
        return 1;
    }
    printf("[evict] SUCCESS: GPU and CPU agree on eviction score "
           "(score=%.6f, |err|=%.3e, %zu rows)\n",
           (double)score, (double)abs_err, B);

    if (!preserve) return 0;

    /* --- v6 handoff for SLM-OS post-kexec consumption. ---
     *
     * Tagged with `pipeline_kind = GA10B_PIPELINE_KIND_EVICTION_QNET`
     * (= 2; defined at kernel/gpu/nvidia/ga10b_channel_handoff.h:388).
     * SLM-OS's per-kind handoff scan in
     * `ga10b_bringup_channel(b, KIND_EVICTION_QNET)` finds this in
     * DRAM even when MNIST and/or sched-MLP handoffs coexist from
     * separately-staged producer runs. */
    struct gpu_buffer pipe_ops = gpu_alloc_buffer(&ctx, 4096, 4096);
    {
        memset(pipe_ops.cpu_va, 0, pipe_ops.size_bytes);
        struct ga10b_pipeline_op *p =
            (struct ga10b_pipeline_op *)pipe_ops.cpu_va;
        for (int i = 0; i < EVICT_OP_COUNT; i++) {
            p[i].qmd_gpu_va       = ops[i].qmd_page.gpu_va;
            /* Op 8's output_phys is where SLM-OS reads the scores
             * (ga10b_bringup_read_pipeline_output reads n_rows × 4
             * bytes from the last op's output_phys), so it must be
             * the start of the buffer, i.e. row 0. */
            p[i].output_phys      = ops[i].output.phys +
                                    (i == EVICT_OP_COUNT - 1
                                         ? 0u
                                         : ops[i].sentinel_cell_idx * 4u);
            p[i].expected_payload = 0u;
            p[i].flags            = 0;
        }
        msync(pipe_ops.cpu_va, pipe_ops.size_bytes, MS_SYNC);
    }
    for (int i = 0; i < EVICT_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    uint64_t handoff_phys = gpu_write_handoff_v6(&ctx, handoff_va,
        ops[8].output.phys, ops[8].output.gpu_va,
        ops[8].sentinel_bits,
        EVICT_OP_COUNT, pipe_ops.phys,
        input.phys, (uint32_t)input_bytes,
        GA10B_PIPELINE_KIND_EVICTION_QNET);

    printf("[evict] Handoff at phys 0x%llx (version=6, %d ops, "
           "kind=EVICTION_QNET=%d)\n",
           (unsigned long long)handoff_phys, EVICT_OP_COUNT,
           GA10B_PIPELINE_KIND_EVICTION_QNET);
    printf("[evict]   pipeline_ops_phys=0x%llx\n",
           (unsigned long long)pipe_ops.phys);
    printf("[evict]   input_buf_phys=0x%llx (%zu bytes = %zu rows "
           "— runtime swap)\n",
           (unsigned long long)input.phys, input_bytes, B);
    printf("[evict]   final scores at phys 0x%llx (%zu fp32)\n",
           (unsigned long long)ops[8].output.phys, B);
    printf("[evict] Sleeping up to %d s — kexec now (when PR-6 lands).\n",
           timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
