/*
 * ga10b_mnist_pipeline.h - The 8-op MNIST GPU pipeline built by SLM-OS
 * on its own channel, without Linux (#1088).
 *
 * Reproduces kernel-side what scripts/gpu-kernel-mnist.c builds before
 * kexec when run with `--preserve-for-kexec --qmd-pool` (the v7
 * handoff, SIMT GEMM tier): four shader buffers, six weight buffers,
 * the input buffer, one cbuf page / QMD page / output buffer per op,
 * the 1024-slot QMD pool and the v7 pipeline_ops array. The result is
 * a version-7 struct ga10b_channel_handoff that the existing dispatch
 * path (ga10b_dispatch_v7_pipeline, ga10b_bringup_set_input,
 * ga10b_bringup_read_pipeline_output, so slm_gpu_run_mnist and the
 * Rust MNIST fastpath) runs unchanged.
 *
 *   Op 0  Conv1      x [1,1,28,28]  w [8,1,5,5]  -> [1,8,28,28]
 *   Op 1  Add+ReLU1  + bias [8]                  -> [1,8,28,28]
 *   Op 2  MaxPool1   2x2 / 2                     -> [1,8,14,14]
 *   Op 3  Conv2      w [16,8,5,5]                -> [1,16,14,14]
 *   Op 4  Add+ReLU2  + bias [16]                 -> [1,16,14,14]
 *   Op 5  MaxPool2   3x3 / 3                     -> [1,16,4,4]
 *   Op 6  GEMM       M=1 K=256 N=10              -> [1,10]
 *   Op 7  AddBias    + bias [10], no ReLU        -> logits [1,10]
 *
 * Sources: shaders come from the embedded operator library
 * (CONV2D / ADD_BIAS / MAXPOOL / GEMM_GENERIC, SIMT, FP32), weights
 * from the ONNX initializers of models/test/mnist.onnx (the same file
 * the CPU path and scripts/mnist-extract-weights.py read), and the
 * initial input from scripts/fixtures/mnist_input_synth.bin (the
 * extraction script's deterministic synthetic image). The embedded
 * lookups live in ga10b_mnist_embed.c (Jetson only); everything here
 * takes its sources as arguments and reaches memory through struct
 * ga10b_gpu_ops, so it runs unchanged in the unit tests.
 *
 * No floating point: the kernel is built with -mgeneral-regs-only, so
 * weights and activations are handled as bytes.
 */
#ifndef GPU_NVIDIA_GA10B_MNIST_PIPELINE_H
#define GPU_NVIDIA_GA10B_MNIST_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ga10b_chan.h"

struct ga10b_channel_handoff;
struct ga10b_pipeline_op_v7;

/* ---- Fixed shape (gpu-kernel-mnist.c) ---- */

#define GA10B_MNIST_N_OPS            8u
#define GA10B_MNIST_INPUT_BYTES      3136u     /* 1*1*28*28*4, :329 / :975 */
#define GA10B_MNIST_LOGITS_BYTES     40u       /* 10 fp32 */
/* Every shader buffer is a 64 KB allocation zero-padded past the SASS
 * (gpu-launch-common.c:397 / :557), so instruction prefetch past the
 * last instruction reads zeros. */
#define GA10B_MNIST_SHADER_BUF_BYTES 65536u
#define GA10B_MNIST_CBUF_PAGE_BYTES  4096u     /* :378 */
#define GA10B_MNIST_QMD_PAGE_BYTES   4096u     /* :377 */
/* --qmd-pool-slots default (gpu-kernel-mnist.c:180) x 256 B. */
#define GA10B_MNIST_QMD_POOL_SLOTS   1024u
#define GA10B_MNIST_QMD_SLOT_BYTES   256u
/* GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT (gpu-launch-common.h:191). */
#define GA10B_MNIST_REGISTER_COUNT_V 128u
/* Kernel arguments start at cbuf[0][0x160] (CUDA param area after the
 * built-in dims at 0x00..0x14). */
#define GA10B_MNIST_CBUF_PARAM_BASE  0x160u

enum ga10b_mnist_shader {
    GA10B_MNIST_SH_CONV = 0,     /* conv2d_fp32_direct     CONV2D       */
    GA10B_MNIST_SH_ADDRELU,      /* add_bias_relu_fp32     ADD_BIAS     */
    GA10B_MNIST_SH_POOL,         /* maxpool2d_fp32         MAXPOOL      */
    GA10B_MNIST_SH_GEMM,         /* gemm_fp32              GEMM_GENERIC */
    GA10B_MNIST_N_SHADERS
};

enum ga10b_mnist_weight {
    GA10B_MNIST_W_CONV1 = 0,
    GA10B_MNIST_B_CONV1,
    GA10B_MNIST_W_CONV2,
    GA10B_MNIST_B_CONV2,
    GA10B_MNIST_W_FC,
    GA10B_MNIST_B_FC,
    GA10B_MNIST_N_WEIGHTS
};

struct ga10b_mnist_weight_desc {
    const char *onnx_name;       /* initializer name in mnist.onnx */
    uint32_t bytes;              /* payload (fp32) */
    uint32_t buf_bytes;          /* allocation, gpu-kernel-mnist.c:298-303 */
};
extern const struct ga10b_mnist_weight_desc
    ga10b_mnist_weights[GA10B_MNIST_N_WEIGHTS];

/* Kernel-argument references: a pointer argument names the input
 * buffer, a weight buffer, or an earlier op's output buffer. */
#define GA10B_MNIST_REF_INPUT        0x00u
#define GA10B_MNIST_REF_WEIGHT(w)    (0x10u + (uint8_t)(w))
#define GA10B_MNIST_REF_OUTPUT(op)   (0x20u + (uint8_t)(op))
#define GA10B_MNIST_MAX_PTR_ARGS     3u
#define GA10B_MNIST_MAX_INT_ARGS     10u

struct ga10b_mnist_op_desc {
    const char *name;
    uint8_t shader;              /* enum ga10b_mnist_shader */
    uint8_t n_ptr;               /* 8-byte pointer args at 0x160 + 8*i */
    uint8_t n_int;               /* 4-byte int args right after them */
    uint8_t ptr[GA10B_MNIST_MAX_PTR_ARGS];
    int32_t ints[GA10B_MNIST_MAX_INT_ARGS];
    uint32_t grid[3];
    uint32_t block[3];
    uint32_t output_bytes;       /* payload; buffer is page-rounded */
    /* First non-zero output cell of the CPU reference on the synthetic
     * input (pipeline_sentinels.bin). The v7 op's output_phys points at
     * this cell, as the helper's does (gpu-kernel-mnist.c:898). */
    uint32_t sentinel_cell;
};
extern const struct ga10b_mnist_op_desc ga10b_mnist_ops[GA10B_MNIST_N_OPS];

/* pipeline_sentinels.bin bits of op 7, which the helper passes as the
 * handoff's expected_payload (gpu-kernel-mnist.c:973). */
#define GA10B_MNIST_OP7_SENTINEL_BITS 0xc003b6c9u

/* Round up to 4 KB; 0 becomes one page (gpu-kernel-mnist.c:78). */
uint32_t ga10b_mnist_round_page(uint32_t bytes);

/* GPU VAs the cbufs point at. */
struct ga10b_mnist_vas {
    uint64_t input;
    uint64_t weight[GA10B_MNIST_N_WEIGHTS];
    uint64_t output[GA10B_MNIST_N_OPS];
};

/* Resolve a GA10B_MNIST_REF_* to its VA; 0 for an invalid reference or
 * an output of op >= `op` (a forward reference). */
uint64_t ga10b_mnist_ref_va(const struct ga10b_mnist_vas *va, uint8_t ref,
                            uint32_t op);

/* Write op `op`'s cbuf[0] image into `cbuf` (zeroed first): built-in
 * dims at 0x00..0x14, pointer args, int args. `cap` must be at least
 * 512 (GPU_LAUNCH_CBUF_SIZE_B). Returns 0, or -1 on a bad argument. */
int ga10b_mnist_fill_cbuf(uint32_t op, const struct ga10b_mnist_vas *va,
                          uint8_t *cbuf, size_t cap);

/* ---- ONNX initializer lookup (protobuf walk, no allocation) ---- */

#define GA10B_ONNX_MAX_DIMS     8u
#define GA10B_ONNX_FLOAT        1u   /* TensorProto.DataType.FLOAT */

#define GA10B_ONNX_OK            0
#define GA10B_ONNX_ERR_ARG      (-1)
#define GA10B_ONNX_ERR_FORMAT   (-2)  /* malformed protobuf */
#define GA10B_ONNX_ERR_NOT_FOUND (-3)
#define GA10B_ONNX_ERR_ENCODING (-4)  /* not contiguous fp32 data */

struct ga10b_onnx_tensor {
    const uint8_t *data;         /* little-endian fp32, unaligned */
    size_t bytes;
    uint32_t data_type;
    uint32_t ndims;
    int64_t dims[GA10B_ONNX_MAX_DIMS];
};

/* Find initializer `name` in ModelProto `model` (graph = field 7,
 * initializer = GraphProto field 5). Accepts FLOAT tensors stored as
 * raw_data (field 9) or packed float_data (field 4); `out->data`
 * points into `model`. Checks that the byte count equals the product
 * of the dims times 4. */
int ga10b_onnx_find_initializer(const uint8_t *model, size_t len,
                                const char *name,
                                struct ga10b_onnx_tensor *out);

/* ---- Building the pipeline ---- */

struct ga10b_mnist_sources {
    const uint8_t *sass[GA10B_MNIST_N_SHADERS];
    uint32_t sass_bytes[GA10B_MNIST_N_SHADERS];   /* 1..65536 */
    const uint8_t *weight[GA10B_MNIST_N_WEIGHTS]; /* ga10b_mnist_weights[].bytes each */
    const uint8_t *input;        /* GA10B_MNIST_INPUT_BYTES, or NULL = zeros */
};

/* Fill `src->weight[]` from an ONNX model. Returns GA10B_ONNX_OK or
 * the first lookup's error; also fails with GA10B_ONNX_ERR_ENCODING if
 * a tensor's size differs from ga10b_mnist_weights[].bytes. */
int ga10b_mnist_weights_from_onnx(const uint8_t *model, size_t len,
                                  struct ga10b_mnist_sources *src);

struct ga10b_mnist_pipeline {
    struct ga10b_dma shader[GA10B_MNIST_N_SHADERS];
    struct ga10b_dma weight[GA10B_MNIST_N_WEIGHTS];
    struct ga10b_dma input;
    struct ga10b_dma qmd[GA10B_MNIST_N_OPS];
    struct ga10b_dma cbuf[GA10B_MNIST_N_OPS];
    struct ga10b_dma out[GA10B_MNIST_N_OPS];
    struct ga10b_dma qmd_pool;
    struct ga10b_dma ops;        /* v7 ops array: CPU-only, not GPU-mapped */
    uint64_t shader_va[GA10B_MNIST_N_SHADERS];
    uint32_t sass_bytes[GA10B_MNIST_N_SHADERS];
    uint64_t qmd_va[GA10B_MNIST_N_OPS];
    uint64_t cbuf_va[GA10B_MNIST_N_OPS];
    uint64_t qmd_pool_va;
    struct ga10b_mnist_vas va;
    bool built;
};

#define GA10B_MNIST_OK            0
#define GA10B_MNIST_ERR_ARG      (-1)
#define GA10B_MNIST_ERR_NOMEM    (-2)
#define GA10B_MNIST_ERR_MAP      (-3)

/* Allocate, fill, clean and map every buffer into `vm` (user range,
 * GPU-uncached like the helper's MAP_BUFFER_EX with flags 0), then
 * write the baked QMD per op (ga10b_qmd_populate; kept for
 * byte-compare like the helper's qmd pages), the v7 ops array and a
 * zeroed QMD pool, and invalidate the VM's TLB. On failure the pages
 * allocated so far are leaked, not freed: some may already be mapped in
 * `vm`, and freeing them would leave PTEs to reused memory. */
int ga10b_mnist_pipeline_build(const struct ga10b_gpu_ops *ops,
                               struct ga10b_vm *vm,
                               const struct ga10b_mnist_sources *src,
                               struct ga10b_mnist_pipeline *p);

/* Free a successfully built pipeline whose VM is being torn down too
 * (tests); it does not unmap, so never use it on a live VM. */
void ga10b_mnist_pipeline_free(const struct ga10b_gpu_ops *ops,
                               struct ga10b_mnist_pipeline *p);

/* v7 ops-array entry for op `op` (pure; the builder uses it). */
void ga10b_mnist_fill_op_v7(const struct ga10b_mnist_pipeline *p, uint32_t op,
                            struct ga10b_pipeline_op_v7 *o);

/* The full version-7 handoff: ga10b_chan_fill_handoff(ch) for the
 * channel fields, then the gpu_write_handoff_v7 fields
 * (gpu-launch-common.c:1196-1244). The semaphore stays the channel's
 * own page rather than aliasing op 7's logits page (the helper's
 * choice, :1211): the dispatch writes it at +0x800, so a separate page
 * cannot clobber logits. */
void ga10b_mnist_pipeline_fill_handoff(const struct ga10b_mnist_pipeline *p,
                                       const struct ga10b_chan *ch,
                                       struct ga10b_channel_handoff *h);

/* ---- Jetson glue (ga10b_mnist_embed.c) ----
 *
 * Shaders from the embedded operator library, weights from the
 * embedded mnist.onnx, input from the embedded synthetic image.
 * Returns 0, or -1 when the operator library lacks one of the four
 * SIMT FP32 shaders (stub library: no SASS at build time) or the ONNX
 * walk fails; the reason is logged. */
int ga10b_mnist_sources_embedded(struct ga10b_mnist_sources *src);

/* Build the pipeline on the cold channel and install the v7 handoff
 * (ga10b_bringup_install_handoff). Call after the GA10B_COLD_CHANNEL
 * step. Returns 0, or a negative GA10B_MNIST_ERR_* / -1; on failure
 * the v2 channel handoff stays installed. */
int ga10b_mnist_cold_install(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch);

#endif /* GPU_NVIDIA_GA10B_MNIST_PIPELINE_H */
