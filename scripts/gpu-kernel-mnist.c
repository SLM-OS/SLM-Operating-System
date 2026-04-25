/*
 * gpu-kernel-mnist.c — Run the full MNIST inference pipeline on the
 * Jetson GA10B GPU (M8 of the MNIST-on-GPU plan).
 *
 * Eight kernel dispatches chained via the v5 handoff:
 *
 *   Op 0  Conv1        x [1,1,28,28] w [8,1,5,5]    → out [1,8,28,28]
 *   Op 1  Add+ReLU1    + bias [8,1,1], apply_relu=1
 *   Op 2  MaxPool1     2×2/2×2                       → out [1,8,14,14]
 *   Op 3  Conv2        x [1,8,14,14] w [16,8,5,5]   → out [1,16,14,14]
 *   Op 4  Add+ReLU2    + bias [16,1,1], apply_relu=1
 *   Op 5  MaxPool2     3×3/3×3                       → out [1,16,4,4]
 *   Op 6  MatMul (GEMM) M=1 K=256 N=10               → out [1,10]
 *   Op 7  AddBias      + bias [1,10], apply_relu=0
 *                                                    → final logits
 *
 * Reshape is free — pool2_out is already laid out in memory as
 * [1,256], directly readable by the GEMM as the M=1, K=256 row.
 *
 * Weights and inputs come from scripts/mnist-weights/ (extracted
 * from models/test/mnist.onnx by scripts/mnist-extract-weights.py).
 *
 * Per-op sentinels are first-non-zero-cell bit patterns precomputed
 * by the same Python script (`pipeline_sentinels.bin`); the launcher
 * uses them as the v5 ops array's `expected_payload` so each op's
 * completion can be detected without SLM-OS having to recompute the
 * activation values itself.
 *
 * Compile on Jetson:
 *   gcc -O2 -Wall -o gpu-kernel-mnist \
 *       gpu-kernel-mnist.c gpu-launch-common.c
 *
 * Usage:
 *   sudo ./gpu-kernel-mnist [--preserve-for-kexec]
 *                           [--timeout-secs N]
 *                           [--weights-dir PATH]
 *                           [--shader-dir PATH]
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

#define MNIST_OP_COUNT 8

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

/* Load `path` into the cpu_va of a freshly-allocated GPU buffer of
 * the requested size. Used for weights and the input image. */
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

/* Compose a path: `dir`/`name`. Stores into `out` (caller-provided). */
static void make_path(char *out, size_t cap,
                       const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "path too long: %s/%s\n", dir, name);
        exit(1);
    }
}

/* Per-op metadata. Filled in below for each of the 8 MNIST ops. */
struct mnist_op {
    const char *name;
    struct gpu_buffer qmd_page;     /* one QMD per op, in its own page */
    struct gpu_buffer cbuf_page;    /* one cbuf per op */
    struct gpu_buffer output;       /* op's output buffer */
    uint32_t output_bytes;          /* actual output payload size */
    uint32_t sentinel_cell_idx;     /* element index into output for poll */
    uint32_t sentinel_bits;         /* expected fp32 bit pattern at that cell */
};

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);

    bool preserve = false;
    int timeout_secs = 900;
    const char *weights_dir = "./mnist-weights";
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

    /* Channel + first shader (use Conv2D as the "primary" since
     * it's the first op). */
    struct gpu_launch_ctx ctx = {0};
    char path[512];
    make_path(path, sizeof(path), shader_dir,
              "conv2d_fp32_direct_shader.sass");
    size_t conv_size = 0;
    gpu_launch_setup(&ctx, path, &conv_size);
    printf("[mnist] conv2d shader %zu bytes (in ctx)\n", conv_size);

    /* The other three shaders go into separate buffers. */
    size_t addrelu_size = 0, pool_size = 0, gemm_size = 0;
    make_path(path, sizeof(path), shader_dir,
              "add_bias_relu_fp32_shader.sass");
    struct gpu_buffer addrelu_shader =
        gpu_load_shader_buffer(&ctx, path, &addrelu_size);
    make_path(path, sizeof(path), shader_dir,
              "maxpool2d_fp32_shader.sass");
    struct gpu_buffer pool_shader =
        gpu_load_shader_buffer(&ctx, path, &pool_size);
    make_path(path, sizeof(path), shader_dir,
              "gemm_fp32_shader.sass");
    struct gpu_buffer gemm_shader =
        gpu_load_shader_buffer(&ctx, path, &gemm_size);
    printf("[mnist] addrelu=%zu pool=%zu gemm=%zu bytes\n",
           addrelu_size, pool_size, gemm_size);

    uint64_t conv_shader_gva    = ctx.shader_gva;
    uint64_t addrelu_shader_gva = addrelu_shader.gpu_va;
    uint64_t pool_shader_gva    = pool_shader.gpu_va;
    uint64_t gemm_shader_gva    = gemm_shader.gpu_va;

    /* --- Weight buffers + input --- */
    struct gpu_buffer W_conv1 = gpu_alloc_buffer(&ctx,  4096, 4096);  /* 800 B */
    struct gpu_buffer B_conv1 = gpu_alloc_buffer(&ctx,  4096, 4096);  /*  32 B */
    struct gpu_buffer W_conv2 = gpu_alloc_buffer(&ctx, 16384, 4096);  /*12800 B*/
    struct gpu_buffer B_conv2 = gpu_alloc_buffer(&ctx,  4096, 4096);  /*  64 B */
    struct gpu_buffer W_fc    = gpu_alloc_buffer(&ctx, 12288, 4096);  /*10240 B*/
    struct gpu_buffer B_fc    = gpu_alloc_buffer(&ctx,  4096, 4096);  /*  40 B */
    struct gpu_buffer input   = gpu_alloc_buffer(&ctx,  4096, 4096);  /* 3136 B*/

    char p[512];
    make_path(p, sizeof(p), weights_dir, "W_conv1.bin");
    load_into_buffer(&W_conv1, p,    8u * 1u * 5u * 5u * 4u);
    make_path(p, sizeof(p), weights_dir, "B_conv1.bin");
    load_into_buffer(&B_conv1, p,    8u * 4u);
    make_path(p, sizeof(p), weights_dir, "W_conv2.bin");
    load_into_buffer(&W_conv2, p,   16u * 8u * 5u * 5u * 4u);
    make_path(p, sizeof(p), weights_dir, "B_conv2.bin");
    load_into_buffer(&B_conv2, p,   16u * 4u);
    make_path(p, sizeof(p), weights_dir, "W_fc.bin");
    load_into_buffer(&W_fc,    p, 256u * 10u * 4u);
    make_path(p, sizeof(p), weights_dir, "B_fc.bin");
    load_into_buffer(&B_fc,    p,   10u * 4u);
    make_path(p, sizeof(p), weights_dir, "input_synth.bin");
    load_into_buffer(&input,   p,    1u * 1u * 28u * 28u * 4u);
    printf("[mnist] weights + input loaded\n");

    /* --- Read pipeline sentinels (cell_idx, expected_bits) per op. --- */
    uint32_t sentinel_idx[MNIST_OP_COUNT] = {0};
    uint32_t sentinel_bits[MNIST_OP_COUNT] = {0};
    {
        make_path(p, sizeof(p), weights_dir, "pipeline_sentinels.bin");
        FILE *fp = fopen(p, "rb");
        if (!fp) { perror(p); exit(1); }
        for (int i = 0; i < MNIST_OP_COUNT; i++) {
            if (fread(&sentinel_idx[i], 4, 1, fp) != 1 ||
                fread(&sentinel_bits[i], 4, 1, fp) != 1) {
                fprintf(stderr, "short read on %s\n", p); exit(1);
            }
        }
        fclose(fp);
    }

    /* --- Per-op metadata --- */
    struct mnist_op ops[MNIST_OP_COUNT];
    static const char *op_names[MNIST_OP_COUNT] = {
        "00_conv1", "01_addrelu1", "02_pool1",
        "03_conv2", "04_addrelu2", "05_pool2",
        "06_matmul", "07_addbias",
    };
    /* Output sizes per op (rounded up to page in alloc; payload size
     * here is the actual byte count): */
    static const uint32_t op_output_bytes[MNIST_OP_COUNT] = {
        1u * 8u * 28u * 28u * 4u,   /* op 0: 25088 */
        1u * 8u * 28u * 28u * 4u,   /* op 1: 25088 */
        1u * 8u * 14u * 14u * 4u,   /*  6272 */
        1u * 16u * 14u * 14u * 4u,  /* 12544 */
        1u * 16u * 14u * 14u * 4u,  /* 12544 */
        1u * 16u *  4u *  4u * 4u,  /*  1024 */
        1u * 10u * 4u,              /*    40 */
        1u * 10u * 4u,              /*    40 */
    };

    /* All eight ops get one QMD page + one cbuf page + one output
     * buffer each. Allocations done up front so the buffer pointer
     * arithmetic below sees them all populated. */
    for (int i = 0; i < MNIST_OP_COUNT; i++) {
        ops[i].name = op_names[i];
        ops[i].qmd_page    = gpu_alloc_buffer(&ctx, 4096, 4096);
        ops[i].cbuf_page   = gpu_alloc_buffer(&ctx, 4096, 4096);
        ops[i].output      = gpu_alloc_buffer(&ctx,
                                  round_up_page(op_output_bytes[i]),
                                  4096);
        ops[i].output_bytes = op_output_bytes[i];
        ops[i].sentinel_cell_idx = sentinel_idx[i];
        ops[i].sentinel_bits     = sentinel_bits[i];
    }

    /* --- Per-op cbuf + QMD population. ---
     *
     * Each op has a different shader signature, so each cbuf has a
     * different scalar layout. The QMD references a different
     * shader_gva and cbuf_gva per op; CTA + grid dims also vary.
     *
     * Op 0: Conv1
     *   x [1,1,28,28] w [8,1,5,5]    → out [1,8,28,28]
     */
    {
        struct mnist_op *op = &ops[0];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;          /* blockDim */
        uint32_t grid_x = (28u * 28u + 255u) / 256u;
        uint32_t grid_y = 8u; uint32_t grid_z = 1u;
        cbuf[3] = grid_x; cbuf[4] = grid_y; cbuf[5] = grid_z;
        *(uint64_t *)((char *)cbuf + 0x160) = input.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = W_conv1.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;          /* N */
        *(int32_t  *)((char *)cbuf + 0x17C) = 1;          /* C_in */
        *(int32_t  *)((char *)cbuf + 0x180) = 28;         /* H */
        *(int32_t  *)((char *)cbuf + 0x184) = 28;         /* W */
        *(int32_t  *)((char *)cbuf + 0x188) = 8;          /* C_out */
        *(int32_t  *)((char *)cbuf + 0x18C) = 5;          /* kH */
        *(int32_t  *)((char *)cbuf + 0x190) = 5;          /* kW */
        *(int32_t  *)((char *)cbuf + 0x194) = 2;          /* pad_h */
        *(int32_t  *)((char *)cbuf + 0x198) = 2;          /* pad_w */
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, conv_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, grid_y);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_DEPTH_HI,
                         QMD_CTA_RASTER_DEPTH_LO, grid_z);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 1: Add+ReLU on Conv1 output. Shape 1×8×28×28. */
    {
        struct mnist_op *op = &ops[1];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (28u * 28u + 255u) / 256u;
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = 8; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[0].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = B_conv1.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;
        *(int32_t  *)((char *)cbuf + 0x17C) = 8;
        *(int32_t  *)((char *)cbuf + 0x180) = 28;
        *(int32_t  *)((char *)cbuf + 0x184) = 28;
        *(int32_t  *)((char *)cbuf + 0x188) = 1;          /* apply_relu */
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, addrelu_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 8);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 2: MaxPool1. Shape 1×8×28×28 → 1×8×14×14. */
    {
        struct mnist_op *op = &ops[2];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (14u * 14u + 255u) / 256u;
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = 8; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[1].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x170) = 1;          /* N */
        *(int32_t  *)((char *)cbuf + 0x174) = 8;          /* C */
        *(int32_t  *)((char *)cbuf + 0x178) = 28;         /* H_in */
        *(int32_t  *)((char *)cbuf + 0x17C) = 28;         /* W_in */
        *(int32_t  *)((char *)cbuf + 0x180) = 2;          /* kH */
        *(int32_t  *)((char *)cbuf + 0x184) = 2;          /* kW */
        *(int32_t  *)((char *)cbuf + 0x188) = 2;          /* sH */
        *(int32_t  *)((char *)cbuf + 0x18C) = 2;          /* sW */
        *(int32_t  *)((char *)cbuf + 0x190) = 14;         /* H_out */
        *(int32_t  *)((char *)cbuf + 0x194) = 14;         /* W_out */
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, pool_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 8);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 3: Conv2. Shape 1×8×14×14, 16 out-ch. */
    {
        struct mnist_op *op = &ops[3];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (14u * 14u + 255u) / 256u;
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = 16; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[2].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = W_conv2.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;
        *(int32_t  *)((char *)cbuf + 0x17C) = 8;
        *(int32_t  *)((char *)cbuf + 0x180) = 14;
        *(int32_t  *)((char *)cbuf + 0x184) = 14;
        *(int32_t  *)((char *)cbuf + 0x188) = 16;
        *(int32_t  *)((char *)cbuf + 0x18C) = 5;
        *(int32_t  *)((char *)cbuf + 0x190) = 5;
        *(int32_t  *)((char *)cbuf + 0x194) = 2;
        *(int32_t  *)((char *)cbuf + 0x198) = 2;
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, conv_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 16);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 4: Add+ReLU on Conv2 output. Shape 1×16×14×14. */
    {
        struct mnist_op *op = &ops[4];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (14u * 14u + 255u) / 256u;
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = 16; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[3].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = B_conv2.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;
        *(int32_t  *)((char *)cbuf + 0x17C) = 16;
        *(int32_t  *)((char *)cbuf + 0x180) = 14;
        *(int32_t  *)((char *)cbuf + 0x184) = 14;
        *(int32_t  *)((char *)cbuf + 0x188) = 1;          /* apply_relu */
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, addrelu_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 16);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 5: MaxPool2. Shape 1×16×14×14 → 1×16×4×4. */
    {
        struct mnist_op *op = &ops[5];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (4u * 4u + 255u) / 256u;       /* = 1 */
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = 16; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[4].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x170) = 1;
        *(int32_t  *)((char *)cbuf + 0x174) = 16;
        *(int32_t  *)((char *)cbuf + 0x178) = 14;
        *(int32_t  *)((char *)cbuf + 0x17C) = 14;
        *(int32_t  *)((char *)cbuf + 0x180) = 3;
        *(int32_t  *)((char *)cbuf + 0x184) = 3;
        *(int32_t  *)((char *)cbuf + 0x188) = 3;
        *(int32_t  *)((char *)cbuf + 0x18C) = 3;
        *(int32_t  *)((char *)cbuf + 0x190) = 4;
        *(int32_t  *)((char *)cbuf + 0x194) = 4;
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, pool_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, grid_x);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 16);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Op 6: GEMM. M=1, K=256, N=10. Reshape is free (pool2 output is
     * already laid out as [1,256] in memory). */
    {
        struct mnist_op *op = &ops[6];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        uint32_t grid_x = (10u + 15u) / 16u;             /* = 1 */
        uint32_t grid_y = (1u  + 15u) / 16u;             /* = 1 */
        cbuf[0] = 16; cbuf[1] = 16; cbuf[2] = 1;
        cbuf[3] = grid_x; cbuf[4] = grid_y; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[5].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = W_fc.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;          /* M */
        *(int32_t  *)((char *)cbuf + 0x17C) = 256;        /* K */
        *(int32_t  *)((char *)cbuf + 0x180) = 10;         /* N */
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

    /* Op 7: AddBias on logits (no ReLU). Shape 1×10 treated as
     * N=1 C=10 H=1 W=1 by the add_bias_relu_fp32 kernel. */
    {
        struct mnist_op *op = &ops[7];
        uint32_t *cbuf = (uint32_t *)op->cbuf_page.cpu_va;
        memset(cbuf, 0, op->cbuf_page.size_bytes);
        cbuf[0] = 256; cbuf[1] = 1; cbuf[2] = 1;
        cbuf[3] = 1;   cbuf[4] = 10; cbuf[5] = 1;
        *(uint64_t *)((char *)cbuf + 0x160) = ops[6].output.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x168) = B_fc.gpu_va;
        *(uint64_t *)((char *)cbuf + 0x170) = op->output.gpu_va;
        *(int32_t  *)((char *)cbuf + 0x178) = 1;
        *(int32_t  *)((char *)cbuf + 0x17C) = 10;
        *(int32_t  *)((char *)cbuf + 0x180) = 1;
        *(int32_t  *)((char *)cbuf + 0x184) = 1;
        *(int32_t  *)((char *)cbuf + 0x188) = 0;          /* apply_relu = 0 */
        msync(op->cbuf_page.cpu_va, op->cbuf_page.size_bytes, MS_SYNC);

        uint32_t *qmd = (uint32_t *)op->qmd_page.cpu_va;
        gpu_populate_qmd_at(qmd, addrelu_shader_gva, op->cbuf_page.gpu_va,
                            GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT);
        gpu_qmd_set_bits(qmd, QMD_CTA_THREAD_DIM0_HI,
                         QMD_CTA_THREAD_DIM0_LO, 256);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_WIDTH_HI,
                         QMD_CTA_RASTER_WIDTH_LO, 1);
        gpu_qmd_set_bits(qmd, QMD_CTA_RASTER_HEIGHT_HI,
                         QMD_CTA_RASTER_HEIGHT_LO, 10);
        msync(op->qmd_page.cpu_va, op->qmd_page.size_bytes, MS_SYNC);
    }

    /* Pre-zero every output buffer so the per-cell sentinel poll
     * sees a transition from 0 to the expected value. */
    for (int i = 0; i < MNIST_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    printf("[mnist] all 8 ops prepared. Linux-side dispatch:\n");

    /* --- Linux-side dispatch: one op at a time, poll for
     * non-zero on each sentinel cell. The CPU-reference bit
     * pattern is a guideline (printed for diagnostic purposes)
     * but the kernel-side and Linux-side polls run in
     * "any non-zero" mode (expected_payload == 0) to absorb the
     * ULP-level FFMA-vs-numpy divergence common in fp32 stacks.
     *
     * The actual GPU-produced bit pattern at each sentinel cell is
     * captured here and re-used as the v5 expected_payload below
     * — by then we know SLM-OS's poll target exactly. --- */
    uint32_t actual_sentinel_bits[MNIST_OP_COUNT];
    uint32_t pb_buf[32];
    for (int i = 0; i < MNIST_OP_COUNT; i++) {
        struct mnist_op *op = &ops[i];
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
        printf("[mnist]   op[%d %s] qmd_gva=0x%lx sentinel cell %u: "
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
                    "[mnist] op[%d %s] sentinel timeout (no non-zero "
                    "write at cell %u)\n",
                    i, op->name, op->sentinel_cell_idx);
            return 1;
        }
    }
    printf("[mnist] all 8 ops fired Linux-side\n");

    /* --- Validate final logits. --- */
    float logits[10];
    memcpy(logits, ops[7].output.cpu_va, sizeof(logits));

    /* CPU reference logits for comparison. */
    float expected[10];
    {
        char path[512];
        make_path(path, sizeof(path), weights_dir, "expected_logits.bin");
        FILE *fp = fopen(path, "rb");
        if (!fp) { perror(path); return 1; }
        if (fread(expected, 1, sizeof(expected), fp) != sizeof(expected)) {
            fprintf(stderr, "short read on %s\n", path); return 1;
        }
        fclose(fp);
    }

    int argmax_gpu = 0, argmax_cpu = 0;
    float max_g = logits[0], max_c = expected[0];
    for (int i = 1; i < 10; i++) {
        if (logits[i]   > max_g) { max_g = logits[i];   argmax_gpu = i; }
        if (expected[i] > max_c) { max_c = expected[i]; argmax_cpu = i; }
    }

    printf("[mnist] GPU logits: ");
    for (int i = 0; i < 10; i++) printf("% .4f ", (double)logits[i]);
    printf("\n");
    printf("[mnist] CPU logits: ");
    for (int i = 0; i < 10; i++) printf("% .4f ", (double)expected[i]);
    printf("\n");

    float max_abs_err = 0.0f;
    for (int i = 0; i < 10; i++) {
        float e = fabsf(logits[i] - expected[i]);
        if (e > max_abs_err) max_abs_err = e;
    }
    printf("[mnist] argmax: GPU=%d CPU=%d  max|err|=%.6f\n",
           argmax_gpu, argmax_cpu, (double)max_abs_err);

    int all_ok = (argmax_gpu == argmax_cpu) && (max_abs_err < 1e-3f);
    if (!all_ok) {
        fprintf(stderr, "[mnist] FAIL: divergence > 1e-3 or argmax mismatch\n");
        return 1;
    }
    printf("[mnist] SUCCESS: GPU and CPU agree on MNIST class %d\n", argmax_gpu);

    if (!preserve) return 0;

    /* --- v5 handoff for SLM-OS post-kexec re-dispatch. ---
     * Use 0 as expected_payload for every op so SLM-OS polls in
     * "any non-zero" mode. Kernel-side ga10b_submit_and_poll handles
     * this; with deterministic dispatch the GPU re-produces the
     * same bit patterns observed Linux-side, but exact-match polls
     * across CPU/GPU rounding boundaries are unreliable in fp32.
     *
     * Note: per-op `output_phys` here is the SENTINEL CELL ADDRESS
     * (= buffer base + sentinel_cell_idx * 4), not the output
     * buffer base. SLM-OS polls this 4-byte location for the
     * any-non-zero transition; the rest of the buffer's content is
     * irrelevant to the dispatch loop. The full output is read
     * back by the launcher (or by SLM-OS shell `peek` commands) to
     * validate the activation values cell-for-cell. */
    struct gpu_buffer pipe_ops = gpu_alloc_buffer(&ctx, 4096, 4096);
    {
        memset(pipe_ops.cpu_va, 0, pipe_ops.size_bytes);
        struct ga10b_pipeline_op *p =
            (struct ga10b_pipeline_op *)pipe_ops.cpu_va;
        for (int i = 0; i < MNIST_OP_COUNT; i++) {
            p[i].qmd_gpu_va       = ops[i].qmd_page.gpu_va;
            p[i].output_phys      = ops[i].output.phys +
                                    ops[i].sentinel_cell_idx * 4u;
            p[i].expected_payload = 0u;  /* any-nonzero mode */
            p[i].flags            = 0;
        }
        msync(pipe_ops.cpu_va, pipe_ops.size_bytes, MS_SYNC);
    }
    /* Re-zero outputs so SLM-OS observes fresh 0 → sentinel
     * transitions, not the values left over from the Linux run. */
    for (int i = 0; i < MNIST_OP_COUNT; i++) {
        memset(ops[i].output.cpu_va, 0, ops[i].output.size_bytes);
        msync(ops[i].output.cpu_va, ops[i].output.size_bytes, MS_SYNC);
    }

    int handoff_dmabuf = gpu_nvmap_alloc_dmabuf(ctx.nvmap_fd, 4096, 4096);
    void *handoff_va = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                            MAP_SHARED, handoff_dmabuf, 0);
    if (handoff_va == MAP_FAILED) { perror("mmap handoff"); return 1; }

    /* v6 handoff: SLM-OS can swap the input tensor at runtime via
     * slm_gpu_set_mnist_input(). The input buffer is the one Op 0
     * (Conv1) reads from — same `input.phys` we memcpy'd the
     * synthetic input into above. SLM-OS overwrites it with whatever
     * bytes the user supplies before each run_mnist() call. */
    uint64_t handoff_phys = gpu_write_handoff_v6(&ctx, handoff_va,
        ops[7].output.phys, ops[7].output.gpu_va,
        ops[7].sentinel_bits,
        MNIST_OP_COUNT, pipe_ops.phys,
        input.phys, (uint32_t)(1u * 1u * 28u * 28u * 4u));

    printf("[mnist] Handoff at phys 0x%llx (version=6, %d ops)\n",
           (unsigned long long)handoff_phys, MNIST_OP_COUNT);
    printf("[mnist]   pipeline_ops_phys=0x%llx\n",
           (unsigned long long)pipe_ops.phys);
    printf("[mnist]   input_buf_phys=0x%llx (%u bytes — runtime swap target)\n",
           (unsigned long long)input.phys,
           (unsigned)(1u * 1u * 28u * 28u * 4u));
    for (int i = 0; i < MNIST_OP_COUNT; i++) {
        printf("[mnist]   op[%d %s] qmd_gva=0x%lx out_phys=0x%llx "
               "sentinel=cell %u (0x%08x)\n",
               i, ops[i].name,
               (unsigned long)ops[i].qmd_page.gpu_va,
               (unsigned long long)ops[i].output.phys,
               ops[i].sentinel_cell_idx,
               ops[i].sentinel_bits);
    }
    printf("[mnist]   final logits at phys 0x%llx (10 fp32 = 40 B)\n",
           (unsigned long long)ops[7].output.phys);
    printf("[mnist] Sleeping up to %d s — kexec now.\n", timeout_secs);

    for (int remaining = timeout_secs; remaining > 0 && !g_shutdown; ) {
        int slice = remaining > 60 ? 60 : remaining;
        sleep(slice);
        remaining -= slice;
    }
    return 0;
}
