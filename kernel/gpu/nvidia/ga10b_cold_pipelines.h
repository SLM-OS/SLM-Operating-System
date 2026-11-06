/*
 * ga10b_cold_pipelines.h - The scheduler-MLP and eviction-MLP GPU
 * pipelines built by SLM-OS on its own channel, without Linux (#1092),
 * and the per-kind store that lets every pipeline kind share that one
 * channel.
 *
 * After a UEFI-direct boot there are no Linux helpers, so nothing has
 * staged the pipelines scripts/gpu-kernel-sched-mlp.c and
 * scripts/gpu-kernel-eviction-qnet.c build before kexec. This module
 * builds the same buffers, cbufs and op chains kernel-side, in the VM
 * of the channel ga10b_cold_run() creates (the MNIST pipeline,
 * ga10b_mnist_pipeline.h, lives there too). Each kind gets its own
 * handoff: the same channel fields, its own pipeline fields and
 * pipeline_kind.
 *
 *   SCHED_MLP (kind 1), gpu-kernel-sched-mlp.c, 8 ops:
 *     Op 0/2/4/6  GEMM       x[1,K] W[K,N]   K,N = 108,256 / 256,256 /
 *                                                  256,128 / 128,42
 *     Op 1/3/5    Add+ReLU   + bias[N]
 *     Op 7        AddBias    + bias[42], no ReLU      -> logits[42]
 *
 *   EVICTION_QNET (kind 2), gpu-kernel-eviction-qnet.c, 9 ops over a
 *   batch of B = 64 rows (the helper's --batch default; slmos-kexec
 *   passes no --batch):
 *     Op 0/2/4/6  GEMM       x[B,K] W[K,N]   K,N = 27,64 / 64,32 /
 *                                                  32,16 / 16,1
 *     Op 1/3/5    Add+ReLU   + bias[N]
 *     Op 7        AddBias    + bias[1], no ReLU
 *     Op 8        Sigmoid    [B]                       -> scores[B]
 *
 * Handoff version. The helpers publish version 6 (helper-baked QMDs
 * replayed per dispatch). These builders publish version 7 like the
 * cold MNIST pipeline: the same buffers and the same baked QMD pages,
 * plus a 1024-slot QMD pool and v7 op entries whose QMD inputs (shader,
 * cbuf, grid, block, 128 registers) are the fields the helper's
 * gpu_populate_qmd_at + dim overrides encode. The v7 dispatch authors a
 * fresh QMD per launch, which avoids the SKED replay staleness (#558)
 * the v6 path needs the post-launch L2 evict to mask.
 *
 * Sources. Shaders come from the embedded operator library
 * (GEMM_GENERIC / ADD_BIAS / SIGMOID, SIMT, FP32). Weights are the
 * models the CPU paths run, in their own (row-major [OUT][IN]) layout,
 * transposed here into the [IN][OUT] layout gemm_fp32 reads, which is
 * what scripts/sched-extract-weights.py / eviction-extract-weights.py
 * write for the helpers:
 *   - sched: ai_mlp_w* / ai_mlp_b* from kernel/sched/ai/ai_weights_mlp.c
 *     (linked when AI_SCHED=ON; AI_WEIGHTS=real swaps in the real set,
 *     matching the CPU scheduler's built-in model. ai_mlp_forward_logits
 *     skips this pipeline while a runtime model is active, #1113);
 *   - eviction: runtime/src/mm/eviction/generated/mlp_policy_f32.rs,
 *     converted to C at build time by cmake/gen_eviction_mlp_weights.cmake
 *     (ga10b_eviction_weights.c, every platform).
 * The synthetic inputs are the extraction scripts' deterministic
 * vectors, as fixed bit patterns below.
 *
 * Everything in this header except the Jetson glue at the end is pure:
 * memory is reached through struct ga10b_gpu_ops, so it runs in the
 * unit tests. No floating point (-mgeneral-regs-only): weights and
 * activations are bytes.
 */
#ifndef GPU_NVIDIA_GA10B_COLD_PIPELINES_H
#define GPU_NVIDIA_GA10B_COLD_PIPELINES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ga10b_chan.h"
#include "ga10b_channel_handoff.h"

/* ---- Limits shared by every spec ---- */

#define GA10B_CP_MAX_SHADERS      3u
#define GA10B_CP_MAX_WEIGHTS      8u
#define GA10B_CP_MAX_OPS          9u
#define GA10B_CP_MAX_PTR_ARGS     3u
#define GA10B_CP_MAX_INT_ARGS     5u

/* 64 KB zero-padded shader buffers (gpu_load_shader_buffer,
 * gpu-launch-common.c:542; gpu_launch_setup's ctx shader). */
#define GA10B_CP_SHADER_BUF_BYTES 65536u
#define GA10B_CP_PAGE_BYTES       4096u
/* QMD pool: the cold MNIST pipeline's size (gpu-kernel-mnist.c
 * --qmd-pool-slots default 1024, 256 B each). */
#define GA10B_CP_QMD_POOL_SLOTS   1024u
#define GA10B_CP_QMD_SLOT_BYTES   256u
/* GPU_LAUNCH_REGISTER_COUNT_V_DEFAULT (gpu-launch-common.h). */
#define GA10B_CP_REGISTER_COUNT_V 128u
#define GA10B_CP_CBUF_PARAM_BASE  0x160u
/* GPU_LAUNCH_CBUF_SIZE_B: the cbuf image must fit this. */
#define GA10B_CP_CBUF_MIN_BYTES   512u

/* Kernel-argument references (as in ga10b_mnist_pipeline.h). */
#define GA10B_CP_REF_INPUT        0x00u
#define GA10B_CP_REF_WEIGHT(w)    (0x10u + (uint8_t)(w))
#define GA10B_CP_REF_OUTPUT(op)   (0x20u + (uint8_t)(op))

/* ---- Tables ---- */

struct ga10b_cp_weight_desc {
    const char *file;           /* the helper's weights-dir file name */
    uint32_t bytes;             /* fp32 payload */
    /* Source layout [rows][cols] row-major, written to the GPU buffer
     * as [cols][rows] (the extraction scripts' transpose). rows = 0:
     * copied as is (biases). */
    uint32_t rows;
    uint32_t cols;
};

struct ga10b_cp_op_desc {
    const char *name;           /* the helper's op_names[] */
    uint8_t shader;             /* index into spec->shader_op_kind[] */
    uint8_t n_ptr;              /* 8-byte pointer args at 0x160 + 8*i */
    uint8_t n_int;              /* 4-byte int args right after them */
    uint8_t ptr[GA10B_CP_MAX_PTR_ARGS];
    int32_t ints[GA10B_CP_MAX_INT_ARGS];
    uint32_t grid[3];
    uint32_t block[3];
    uint32_t output_bytes;      /* payload; the buffer is page-rounded */
    /* pipeline_sentinels.bin (cell, bits) of the extraction script's CPU
     * reference on the synthetic input. The v7 op's output_phys points
     * at the cell, as the helper's v6 entry does, except for the last
     * op, whose output_phys is the buffer start: that is where
     * ga10b_bringup_read_pipeline_output reads the result. */
    uint32_t sentinel_cell;
    uint32_t sentinel_bits;
};

struct ga10b_cp_spec {
    uint32_t kind;              /* enum ga10b_pipeline_kind */
    const char *name;
    uint32_t n_shaders;
    uint32_t shader_op_kind[GA10B_CP_MAX_SHADERS];  /* SLM_GPU_OP_*, SIMT FP32 */
    const char *shader_name[GA10B_CP_MAX_SHADERS];  /* *_shader.sass stem */
    uint32_t n_weights;
    const struct ga10b_cp_weight_desc *weights;
    uint32_t input_bytes;       /* handoff input_buf_size */
    uint32_t n_ops;
    const struct ga10b_cp_op_desc *ops;
    uint32_t result_bytes;      /* what the consumer reads from the last op */
};

/* Shader slots of both specs. */
enum { GA10B_CP_SH_GEMM = 0, GA10B_CP_SH_ADDRELU = 1, GA10B_CP_SH_SIGMOID = 2 };

/* Per-dispatch rows of the eviction pipeline (--batch default). */
#define GA10B_CP_EVICT_BATCH        64u
#define GA10B_CP_EVICT_FEATURES     27u
#define GA10B_CP_SCHED_STATE_DIM    108u
#define GA10B_CP_SCHED_ACTIONS      42u

extern const struct ga10b_cp_spec ga10b_cp_sched_spec;
extern const struct ga10b_cp_spec ga10b_cp_evict_spec;

/* Spec for a pipeline kind; NULL for MNIST (ga10b_mnist_pipeline.h) or
 * an unknown kind. */
const struct ga10b_cp_spec *ga10b_cp_spec_for_kind(uint32_t kind);

/* Synthetic inputs: scripts/sched-extract-weights.py in_vec
 * ((i / 53.5) - 1.0, 108 floats) and eviction-extract-weights.py
 * in_vec ((i / 130.0) - 0.1, 27 floats), as fp32 bits. */
extern const uint32_t ga10b_cp_sched_input_synth[GA10B_CP_SCHED_STATE_DIM];
extern const uint32_t ga10b_cp_evict_input_synth[GA10B_CP_EVICT_FEATURES];

/* CPU-reference outputs on those inputs (expected_logits.bin /
 * expected_score.bin, synthetic sched weights): what a correct GPU run
 * returns within the helpers' tolerances (1e-3 per logit, 1e-4 for the
 * score). */
extern const uint32_t ga10b_cp_sched_expected_logits[GA10B_CP_SCHED_ACTIONS];
#define GA10B_CP_SCHED_EXPECTED_ARGMAX   8u
#define GA10B_CP_EVICT_EXPECTED_SCORE    0x3f2c7334u   /* 0.67363286 */

/* Eviction MLP weights from mlp_policy_f32.rs (ga10b_eviction_weights.c),
 * in the Rust [OUT][IN] layout. */
extern const float ga10b_evict_mlp_w_l1[64 * 27];
extern const float ga10b_evict_mlp_b_l1[64];
extern const float ga10b_evict_mlp_w_l2[32 * 64];
extern const float ga10b_evict_mlp_b_l2[32];
extern const float ga10b_evict_mlp_w_l3[16 * 32];
extern const float ga10b_evict_mlp_b_l3[16];
extern const float ga10b_evict_mlp_w_out[1 * 16];
extern const float ga10b_evict_mlp_b_out[1];

/* ---- Pure helpers ---- */

/* Outputs (fp32 bit patterns) that differ from the reference by more
 * than `tol_ppm` parts per million of max(1, |want|), or are NaN/Inf
 * (or |x| >= 2^20). `tol_ppm` is capped at 1000000. Integer-only. Used by `nvgpu cold check` to compare
 * a GPU run on the synthetic input with the helpers' CPU reference. */
uint32_t ga10b_cp_count_mismatch(const uint32_t *got, const uint32_t *want,
                                 uint32_t n, uint32_t tol_ppm);

/* Index of the largest of `n` fp32 bit patterns (first on ties). */
uint32_t ga10b_cp_argmax(const uint32_t *v, uint32_t n);

/* Copy `rows` x `cols` 4-byte cells from row-major `src` into `dst` as
 * a row-major [cols][rows] matrix. */
void ga10b_cp_transpose32(uint8_t *dst, const uint8_t *src, uint32_t rows,
                          uint32_t cols);

/* Write one weight's GPU-buffer bytes (transposed per its desc) into
 * `dst` (at least desc->bytes). Returns 0, or -1 on a bad argument or a
 * desc whose rows * cols * 4 != bytes. */
int ga10b_cp_weight_fill(const struct ga10b_cp_weight_desc *w, uint8_t *dst,
                         const uint8_t *src);

/* GPU VAs the cbufs point at. */
struct ga10b_cp_vas {
    uint64_t input;
    uint64_t weight[GA10B_CP_MAX_WEIGHTS];
    uint64_t output[GA10B_CP_MAX_OPS];
};

/* Resolve a GA10B_CP_REF_* to its VA; 0 for an invalid reference or a
 * forward reference to a later op's output. */
uint64_t ga10b_cp_ref_va(const struct ga10b_cp_spec *s, const struct ga10b_cp_vas *va,
                         uint8_t ref, uint32_t op);

/* Write op `op`'s cbuf[0] image (zeroed first): built-in dims at
 * 0x00..0x14, pointer args from 0x160, int args after them. `cap` >=
 * GA10B_CP_CBUF_MIN_BYTES. Returns 0, or -1 on a bad argument. */
int ga10b_cp_fill_cbuf(const struct ga10b_cp_spec *s, uint32_t op,
                       const struct ga10b_cp_vas *va, uint8_t *cbuf, size_t cap);

/* ---- Building ---- */

struct ga10b_cp_sources {
    const uint8_t *sass[GA10B_CP_MAX_SHADERS];
    uint32_t sass_bytes[GA10B_CP_MAX_SHADERS];       /* 1..65536 */
    const uint8_t *weight[GA10B_CP_MAX_WEIGHTS];     /* source layout, desc.bytes */
    /* Initial input: `input_bytes` (<= spec->input_bytes) copied to the
     * start of the input buffer, the rest zero. NULL = all zeros. The
     * eviction helper also fills rows 1..63 with perturbations of row 0
     * for its self-check; here they stay zero (every dispatch overwrites
     * the rows it uses). */
    const uint8_t *input;
    uint32_t input_bytes;
};

struct ga10b_cp_pipeline {
    const struct ga10b_cp_spec *spec;
    struct ga10b_dma shader[GA10B_CP_MAX_SHADERS];
    struct ga10b_dma weight[GA10B_CP_MAX_WEIGHTS];
    struct ga10b_dma input;
    struct ga10b_dma qmd[GA10B_CP_MAX_OPS];
    struct ga10b_dma cbuf[GA10B_CP_MAX_OPS];
    struct ga10b_dma out[GA10B_CP_MAX_OPS];
    struct ga10b_dma qmd_pool;
    struct ga10b_dma ops;       /* v7 ops array: CPU-only, not GPU-mapped */
    uint64_t shader_va[GA10B_CP_MAX_SHADERS];
    uint32_t sass_bytes[GA10B_CP_MAX_SHADERS];
    uint64_t qmd_va[GA10B_CP_MAX_OPS];
    uint64_t cbuf_va[GA10B_CP_MAX_OPS];
    uint64_t qmd_pool_va;
    struct ga10b_cp_vas va;
    bool built;
};

#define GA10B_CP_OK            0
#define GA10B_CP_ERR_ARG      (-1)
#define GA10B_CP_ERR_NOMEM    (-2)
#define GA10B_CP_ERR_MAP      (-3)
/* ga10b_cold_pipeline_build: the kind's shaders or weights are not in
 * this build (logged); the kind stays on the CPU. Not an error. */
#define GA10B_CP_SKIP         (-10)

/* Allocate, fill, clean and map every buffer of `spec` into `vm` (user
 * range, not GPU-L2 cacheable like the helpers' MAP_BUFFER_EX with flags
 * 0), write the baked QMD page per op, the v7 ops array and a zeroed QMD
 * pool. The caller invalidates the VM's TLB (the channel has run, so the
 * GPU may hold stale walks), under the dispatch lock when a dispatch can
 * be in flight. Same failure policy as
 * ga10b_mnist_pipeline_build: pages allocated before a failure are
 * leaked, since some may already be mapped in the live VM. */
int ga10b_cp_build(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                   const struct ga10b_cp_spec *spec,
                   const struct ga10b_cp_sources *src, struct ga10b_cp_pipeline *p);

/* Free a built pipeline whose VM is torn down too (tests only). */
void ga10b_cp_free(const struct ga10b_gpu_ops *ops, struct ga10b_cp_pipeline *p);

/* v7 ops-array entry for op `op` (pure; the builder uses it). */
void ga10b_cp_fill_op_v7(const struct ga10b_cp_pipeline *p, uint32_t op,
                         struct ga10b_pipeline_op_v7 *o);

/* The version-7 handoff: ga10b_chan_fill_handoff(ch) for the channel
 * fields, then the pipeline fields gpu_write_handoff_v6 sets
 * (expected_payload = last op's sentinel bits, n_ops, ops array, input
 * buffer, pipeline_kind) and the QMD pool. An unbuilt pipeline leaves
 * the version-2 channel handoff. */
void ga10b_cp_fill_handoff(const struct ga10b_cp_pipeline *p, const struct ga10b_chan *ch,
                           struct ga10b_channel_handoff *h);

/* ---- Per-kind handoff store ----
 *
 * One handoff per pipeline kind, all on the same channel. ensure_bringup
 * (slm_ffi.c) installs the stored handoff of the kind it is asked for
 * when the active one differs. */

struct ga10b_cp_store {
    struct ga10b_channel_handoff h[GA10B_PIPELINE_KIND_COUNT];
    bool have[GA10B_PIPELINE_KIND_COUNT];
    /* QMD-pool cursor each kind had when it was switched away from. */
    uint32_t qmd_cursor[GA10B_PIPELINE_KIND_COUNT];
};

#define GA10B_CP_STORE_OK          0
#define GA10B_CP_STORE_ERR_ARG    (-1)   /* NULL / not a v7 pipeline handoff */
#define GA10B_CP_STORE_ERR_KIND   (-2)   /* pipeline_kind out of range */
#define GA10B_CP_STORE_ERR_NONE   (-3)   /* nothing stored for the kind */
#define GA10B_CP_STORE_ERR_CHAN   (-4)   /* active handoff is another channel */

/* Store a copy of `h` under h->pipeline_kind (replacing any earlier
 * one). Accepts only a valid v7 pipeline handoff (magic,
 * ga10b_handoff_is_v7, ga10b_v7_validate_handoff). */
int ga10b_cp_store_put(struct ga10b_cp_store *st, const struct ga10b_channel_handoff *h);

/* Stored handoff for `kind`, or NULL. */
const struct ga10b_channel_handoff *ga10b_cp_store_get(const struct ga10b_cp_store *st,
                                                       uint32_t kind);

/* The handoff to install when switching the channel to `kind`: the
 * stored one, with the GPFIFO cursor (initial_gp_put / initial_gp_get)
 * taken from `active`, the handoff installed now. The dispatch path
 * keeps the channel's live cursor in the installed handoff, so
 * installing a stored copy as is would move GP_PUT back to where it was
 * when the pipeline was built, behind GP_GET. `active` may be NULL or
 * not hold a handoff (magic 0): the stored cursor is used. An active
 * handoff on a different channel (USERD / GPFIFO / pushbuffer / token
 * differ) is refused. */
int ga10b_cp_switch_handoff(const struct ga10b_cp_store *st, uint32_t kind,
                            const struct ga10b_channel_handoff *active,
                            struct ga10b_channel_handoff *out);

/* Record `from_kind`'s QMD-pool cursor (it is being switched away from;
 * ignored when out of range) and return the cursor `to_kind` left off
 * at (0 the first time). Each kind has its own QMD pool, so restoring
 * its cursor keeps the #558 rotation going instead of re-authoring
 * slot 0 on every switch. */
uint32_t ga10b_cp_store_swap_cursor(struct ga10b_cp_store *st, uint32_t from_kind,
                                    uint32_t from_cursor, uint32_t to_kind);

/* The kernel's store (the cold bring-up fills it). */
struct ga10b_cp_store *ga10b_cp_store_global(void);

/* ---- Jetson glue (ga10b_cold_pipelines_embed.c) ---- */

struct ga10b_bringup;

/* Fill `src` for `kind` (SCHED_MLP / EVICTION_QNET) from the operator
 * library and the linked weights. Returns 0, or -1 with the reason
 * logged: a shader missing from the library (stub library, or SASS not
 * extracted), or for SCHED_MLP an AI_SCHED=OFF build (no ai_mlp
 * weights). */
int ga10b_cold_pipeline_sources(uint32_t kind, struct ga10b_cp_sources *src);

/* Build `kind`'s pipeline on the cold channel and store its handoff
 * (not installed). Returns 0 (also when it is already built: a
 * `nvgpu cold` retry), GA10B_CP_SKIP when its sources are not in this
 * build, or a negative GA10B_CP_ERR_* (build failure, or the store
 * rejected the handoff). */
int ga10b_cold_pipeline_build(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch,
                              uint32_t kind);

/* Store the currently installed handoff (e.g. the cold MNIST v7 one
 * after ga10b_mnist_cold_install) under its kind. Returns
 * GA10B_CP_STORE_* (ERR_ARG when nothing locally built is installed). */
int ga10b_cold_pipeline_store_active(void);

/* Install `kind`'s stored handoff as the active one, carrying the live
 * GPFIFO cursor over (ga10b_cp_switch_handoff +
 * ga10b_bringup_install_handoff). For ensure_bringup: the caller holds
 * g_gpu_dispatch_lock (so it must NOT go through
 * slm_gpu_install_handoff, which takes that lock). Returns 0 or a
 * negative GA10B_CP_STORE_* / -1 from the install. */
int ga10b_cold_pipeline_install(struct ga10b_bringup *b, uint32_t kind);

#endif /* GPU_NVIDIA_GA10B_COLD_PIPELINES_H */
