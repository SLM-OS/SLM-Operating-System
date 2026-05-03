/*
 * ga10b_channel_handoff.h — Shared structure between the Linux-side
 * channel helper and SLM-OS for inheriting a GPU channel across kexec.
 *
 * The Linux helper (scripts/gpu-channel-helper.c) creates an nvgpu
 * channel, fills in this structure with the channel's physical
 * addresses, and writes it to a fixed DRAM location. SLM-OS reads
 * the structure after kexec and uses the pre-created channel to
 * submit pushbuffer methods.
 *
 * The handoff address is GA10B_CHANNEL_HANDOFF_PHYS — chosen to be
 * within Jetson's NC memory region so SLM-OS can read it without
 * cache coherency issues. The Linux helper writes it via /dev/mem.
 *
 * Wire format: all fields are little-endian uint32/uint64.
 */

#ifndef GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H
#define GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Magic value to detect a valid handoff block.
 *
 * Production handoff discovery: SLM-OS scans a physical-memory range
 * for this magic value (the Linux helper allocates the handoff block
 * via nvmap which places it somewhere unpredictable in the IOVMM
 * heap). See ga10b_find_handoff_in_range() for the scanner and
 * kernel/gpu/nvidia/ga10b_bringup.c phase 6 for the range used. */
#define GA10B_CHANNEL_HANDOFF_MAGIC 0x47505548U  /* "GPUH" */

/*
 * Channel handoff block — written by Linux, read by SLM-OS.
 *
 * All physical addresses are CPU-physical (identity-mapped on GA10B
 * with SMMU passthrough). GPU-virtual addresses are in the channel's
 * GMMU address space (set up by nvgpu).
 */
struct ga10b_channel_handoff {
    uint32_t magic;             /* GA10B_CHANNEL_HANDOFF_MAGIC */
    uint32_t version;           /* 2: channel only.
                                 * 3: channel + compute-kernel launch
                                 *    state (shader, cbuf, qmd, output).
                                 * 4: v3 + `expected_payload` so SLM-OS
                                 *    can launch kernels other than the
                                 *    hard-coded-to-0xCAFE write_cafe —
                                 *    e.g. dot4 which writes 300.
                                 * 5: v4 + a multi-op pipeline pointer
                                 *    so SLM-OS can dispatch N kernels
                                 *    in sequence from one launch-kernel
                                 *    invocation (model-inference path).
                                 * 6: v5 + input_buf_phys/size so SLM-OS
                                 *    can swap the model's input tensor
                                 *    at runtime (per-image MNIST
                                 *    classification post-kexec).
                                 * 7: v6 + qmd_pool_* fields and per-op
                                 *    QMD-construction inputs (shader,
                                 *    cbuf, dims, register count) so
                                 *    SLM-OS can author fresh QMDs per
                                 *    dispatch instead of replaying the
                                 *    helper-baked chain. When version
                                 *    >= 7, `pipeline_ops_phys` points
                                 *    at an array of
                                 *    `struct ga10b_pipeline_op_v7`
                                 *    rather than the v6
                                 *    `struct ga10b_pipeline_op`.
                                 *    Workaround for the SKED elision
                                 *    bug — issue #558.
                                 * SLM-OS channel inherit accepts any of
                                 * v2..v7; `nvgpu launch-kernel` needs
                                 * at least v3 for shader/QMD fields,
                                 * runs the v5 pipeline if
                                 * `pipeline_n_ops > 0`, otherwise
                                 * single-shot per the v4 path. The v6
                                 * input swap path is opt-in via
                                 * `slm_gpu_set_mnist_input` /
                                 * `slm.gpu_set_mnist_input`. The v7
                                 * per-dispatch QMD path is the future
                                 * default once the helper writes v7. */
    uint32_t channel_id;        /* Diagnostic only; never used as the
                                 * doorbell token. See work_submit_token
                                 * below — the kernel's allocation path
                                 * may map channel_id to a different
                                 * token value (vGPU channel_base). */
    uint32_t tsg_id;            /* TSG ID the channel belongs to */

    /* USERD (User Submit Data) — where GP_PUT lives.
     * The PBDMA reads GP_PUT from this memory location (not a register).
     * SLM-OS writes GP_PUT here to submit GPFIFO entries. */
    uint64_t userd_phys;        /* physical address of USERD page */
    uint32_t userd_gp_put_offset; /* byte offset of GP_PUT within USERD */
    uint32_t userd_gp_get_offset; /* byte offset of GP_GET within USERD */

    /* GPFIFO ring — circular buffer of 8-byte entries.
     * Each entry points to a pushbuffer segment. */
    uint64_t gpfifo_phys;       /* physical address of GPFIFO ring */
    uint64_t gpfifo_gpu_va;     /* GPU VA of GPFIFO ring */
    uint32_t gpfifo_entries;    /* number of GPFIFO slots (power of 2) */
    uint32_t gpfifo_entry_size; /* bytes per entry (8 on Ampere) */

    /* Pushbuffer — pre-allocated DMA buffer for method data.
     * SLM-OS writes methods here, then adds a GPFIFO entry pointing
     * to this buffer. */
    uint64_t pushbuf_phys;      /* physical address */
    uint64_t pushbuf_gpu_va;    /* GPU VA */
    uint32_t pushbuf_size;      /* bytes (typically 4 KB - 64 KB) */
    uint32_t _pad0;

    /* Semaphore — GPU writes here on SEMAPHORE_RELEASE method.
     * SLM-OS polls this address to detect method completion. */
    uint64_t semaphore_phys;    /* physical address */
    uint64_t semaphore_gpu_va;  /* GPU VA */

    /* Instance block — the channel's GMMU root. */
    uint64_t inst_block_phys;   /* physical address (for diagnostics) */

    /* GP_PUT/GP_GET current values at handoff time. */
    uint32_t initial_gp_put;    /* GP_PUT value when helper wrote this */
    uint32_t initial_gp_get;    /* GP_GET value (should equal GP_PUT) */

    /* USERMODE doorbell token — what SLM-OS writes to BAR0+0xBB0090
     * to kick PBDMA. Captured from NVGPU_IOCTL_CHANNEL_SETUP_BIND's
     * work_submit_token field. Encodes (chid | runlist_id<<16) but
     * the kernel may adjust for vGPU channel_base, so treat it as
     * opaque and use verbatim. Nvgpu exposes no cheap path to
     * reconstruct it from channel_id alone. */
    uint32_t work_submit_token;
    uint32_t _pad1;             /* align struct size to 8 bytes */

    /* --- v3 extension: compute-kernel launch state. ---
     * Zero on v2. Populated by scripts/gpu-kernel-launch.c when
     * called with --preserve-for-kexec. SLM-OS's `nvgpu launch-kernel`
     * uses these to build a compute pushbuffer pointing at the
     * pre-uploaded shader + QMD and polls `output_phys` for the
     * expected payload. */
    uint64_t shader_phys;       /* CPU-physical of shader code */
    uint64_t shader_gpu_va;     /* GPU VA of shader (== QMD PROGRAM_ADDRESS) */
    uint64_t cbuf_phys;         /* CPU-physical of cbuf[0] */
    uint64_t cbuf_gpu_va;       /* GPU VA of cbuf[0] (kernel arg area) */
    uint64_t qmd_phys;          /* CPU-physical of the QMD page */
    uint64_t qmd_gpu_va;        /* GPU VA of the QMD (SEND_PCAS_A data = >>8) */
    uint64_t output_phys;       /* CPU-physical of kernel output buffer */
    uint64_t output_gpu_va;     /* GPU VA of output buffer (stored in
                                 * cbuf[0][0x160] for the kernel to read) */
    uint32_t shader_size;       /* bytes — typically 640 for write_cafe */
    uint32_t cbuf_size;         /* bytes — typically 512 */

    /* --- v4 extension: expected-payload for generic launch_kernel. ---
     * Zero on v2/v3. Populated by any helper running a kernel that
     * doesn't write 0xCAFE (e.g. dot4 → 300). SLM-OS's
     * `nvgpu launch-kernel` polls `output_phys` for this value;
     * v3 handoffs fall back to GA10B_SMOKETEST_SEM_PAYLOAD. */
    uint32_t expected_payload;  /* value the kernel writes to *output */
    uint32_t _pad2;             /* align struct size to 8 bytes */

    /* --- v5 extension: multi-op pipeline. ---
     * Zero on v2/v3/v4. Populated by helpers that need to chain N
     * kernel dispatches (model inference). When `pipeline_n_ops > 0`,
     * `pipeline_ops_phys` points to an array of N
     * struct ga10b_pipeline_op (defined below). SLM-OS's
     * `nvgpu launch-kernel` runs the chain instead of the single-QMD
     * path. */
    uint32_t pipeline_n_ops;
    uint32_t _pad3;
    uint64_t pipeline_ops_phys;

    /* --- v6 extension: runtime input buffer. ---
     * Zero on v2..v5. Populated by helpers that want SLM-OS to be
     * able to swap the model's input tensor at runtime (e.g. classify
     * different MNIST digit images without re-running the launcher
     * pre-kexec). `input_buf_phys` is the CPU-physical address of
     * the input buffer the FIRST pipeline op reads from; SLM-OS
     * writes the new tensor bytes there and the next dispatch
     * picks them up. `input_buf_size` is the buffer's capacity in
     * bytes — SLM-OS bounds-checks user writes against this. */
    uint64_t input_buf_phys;
    uint32_t input_buf_size;

    /* Pipeline-kind discriminator. PR-3 of
     * docs/specs/gpu-policy-models.md repurposes the previously-
     * reserved `_pad4` slot as a `enum ga10b_pipeline_kind`. Default
     * value 0 = MNIST keeps backward compatibility with v6 handoffs
     * written before this field existed (Linux helpers wrote
     * _pad4=0, which now reads as KIND_MNIST). The SLM-OS scanner
     * branches on this when discriminating between handoffs of
     * different kinds in DRAM (e.g. MNIST + sched-MLP coexisting
     * across two host-side launchers running concurrently). */
    uint32_t pipeline_kind;

    /* --- v7 extension: per-dispatch QMD pool. ---
     * Zero on v2..v6. Populated by helpers that want SLM-OS to
     * author fresh QMDs per dispatch instead of replaying the
     * helper-baked chain (workaround for SKED elision; issue #558).
     *
     * When `version >= 7`:
     *   - `pipeline_ops_phys` points at an array of
     *     `struct ga10b_pipeline_op_v7` (80 B each), NOT the v6
     *     `struct ga10b_pipeline_op` (24 B). SLM-OS branches on
     *     `version` to choose the right indexing stride.
     *   - The pool descriptor below names a GMMU-mapped scratch
     *     region SLM-OS writes fresh QMD bytes into. SLM-OS
     *     advances through `qmd_pool_n_slots` slots round-robin
     *     so consecutive launches use distinct GPU VAs (forces
     *     SKED redecode and per-QMD INVALIDATE_*_CACHE bits to
     *     fire on each launch).
     *
     * Wire-format-skew note: the struct grew from 232 → 256 bytes
     * with this v7 extension. A pre-v7 helper that writes only the
     * first 232 bytes leaves these trailing fields uninitialised
     * — typically the nvmap allocator returns zeroed pages, but
     * that isn't formally guaranteed across kexec. The
     * load-bearing wire-format guard is `version`: SLM-OS's
     * dispatch path takes the v7 branch only when
     * `version >= 7`, and a pre-v7 helper writes `version` ≤ 6
     * deterministically. The `qmd_pool_n_slots > 0 &&
     * qmd_pool_phys != 0` checks in the dispatch path are a
     * defensive belt-and-braces against a buggy v7 producer that
     * advances the version field without populating the pool —
     * NOT a substitute for the version check.
     *
     * Sizing: 1024 slots × 256 B = 256 KiB is the recommended
     * minimum for SLM workloads (~370 ops/token for Qwen 2.5
     * 1.5B). MNIST works with much less but the pool is sized
     * once at channel setup so generous default is fine. See
     * docs/gpu-qmd-per-dispatch-plan.md §3. */
    uint64_t qmd_pool_phys;
    uint64_t qmd_pool_gpu_va;
    uint32_t qmd_pool_size_bytes;
    uint32_t qmd_pool_n_slots;
};

/* Pipeline-kind discriminator values stored in
 * `struct ga10b_channel_handoff::pipeline_kind`. Numbered to keep
 * 0 as MNIST so legacy v6 handoffs continue to dispatch correctly
 * without the producer having to set the field. */
enum ga10b_pipeline_kind {
    GA10B_PIPELINE_KIND_MNIST         = 0,
    GA10B_PIPELINE_KIND_SCHED_MLP     = 1,
    GA10B_PIPELINE_KIND_EVICTION_QNET = 2,
};

/* One entry per op in a v5/v6 pipeline. SLM-OS reads this array from
 * the DRAM page at handoff->pipeline_ops_phys when version <= 6. */
struct ga10b_pipeline_op {
    uint64_t qmd_gpu_va;        /* GPU VA of this op's QMD (256 B aligned) */
    uint64_t output_phys;       /* CPU-physical sentinel target */
    uint32_t expected_payload;  /* value to poll for; 0 → "any non-zero" */
    uint32_t flags;             /* reserved (0 today) */
};

/* v7 per-op layout. Used when `handoff->version >= 7`. The leading
 * 24 bytes are layout-compatible with `struct ga10b_pipeline_op` so
 * read-only consumers of the v6 fields can use a single struct (this
 * one). Indexing stride differs between versions, though, so SLM-OS
 * dispatch must branch on `handoff->version` to pick the right
 * sizeof when walking `pipeline_ops_phys`.
 *
 * v7 adds the inputs needed for SLM-OS to author a fresh QMD per
 * dispatch. The Linux helper writes these alongside the existing
 * `qmd_gpu_va` field; SLM-OS uses the new fields to call
 * `ga10b_qmd_populate` against a slot in the QMD pool, then submits
 * the freshly-built QMD's GPU VA via `SEND_PCAS_A` instead of
 * `qmd_gpu_va`. The original `qmd_gpu_va` is kept for byte-compare
 * validation (Phase 4 of docs/gpu-qmd-per-dispatch-plan.md).
 *
 * Block dims are u32 here for wire-format simplicity even though the
 * QMD's CTA_THREAD_DIM fields are 16 bits wide — the encoder masks
 * to the right width when packing.
 */
struct ga10b_pipeline_op_v7 {
    /* v6-compatible prefix — same 24-byte layout as
     * `struct ga10b_pipeline_op`. Pinned by static asserts below. */
    uint64_t qmd_gpu_va;
    uint64_t output_phys;
    uint32_t expected_payload;
    uint32_t flags;

    /* v7 additions: QMD construction inputs. */
    uint64_t shader_gpu_va;     /* GPU VA of compiled SASS for this op */
    uint64_t cbuf_gpu_va;       /* GPU VA of cbuf[0] (CUDA param area) */
    uint32_t register_count_v;  /* per-thread register usage */
    uint32_t grid_x;            /* CTA raster width */
    uint32_t grid_y;            /* CTA raster height */
    uint32_t grid_z;            /* CTA raster depth */
    uint32_t block_x;           /* threads per CTA, dim 0 */
    uint32_t block_y;           /* threads per CTA, dim 1 */
    uint32_t block_z;           /* threads per CTA, dim 2 */
    uint32_t smem_size_bytes;   /* shared memory per block */
    uint32_t slm_size_bytes;    /* shader local memory per thread */
    uint32_t barrier_count;     /* num_control_barriers */
};

/* Upper bound on pipeline length, enforced by the kernel-side runner.
 * Two values, one per per-op layout: the launcher allocates a single
 * 4 KB page for the ops array, so the cap is `4096 / sizeof(op)`.
 * Anything past that would dereference into adjacent memory.
 *
 *   v6 ops (24 B): 4096 / 24 = 170 max
 *   v7 ops (80 B): 4096 / 80 = 51  max
 *
 * MNIST uses 8 ops on either layout. Larger SLMs (Qwen 2.5 1.5B has
 * ~370 ops/token) need either the launcher to allocate >1 page or
 * a different ops-array structure entirely — tracked separately
 * (#573 covers the dispatch architecture rework). */
#define GA10B_PIPELINE_MAX_OPS    170u   /* v6 cap (existing) */
#define GA10B_PIPELINE_V7_MAX_OPS 51u    /* v7 cap (4096 / 80) */

/* Wire-format size is locked: both the Linux helper and SLM-OS
 * depend on this exact layout. Any struct reorder or field addition
 * breaks the handoff silently — the static_assert catches it at
 * compile time on both sides. v7 grew the struct by 24 bytes
 * (qmd_pool_phys + qmd_pool_gpu_va + qmd_pool_size_bytes +
 * qmd_pool_n_slots) → 232 + 24 = 256. */
_Static_assert(sizeof(struct ga10b_channel_handoff) == 256,
               "ga10b_channel_handoff layout changed — update Linux "
               "helper (scripts/gpu-channel-helper.c, "
               "scripts/gpu-kernel-launch.c, scripts/gpu-launch-common.c) "
               "and bump version");
_Static_assert(sizeof(struct ga10b_pipeline_op) == 24,
               "ga10b_pipeline_op layout changed — Linux + SLM-OS "
               "must agree on the per-op size");
_Static_assert(sizeof(struct ga10b_pipeline_op_v7) == 80,
               "ga10b_pipeline_op_v7 layout changed — Linux helper "
               "and SLM-OS must agree on the v7 per-op size");

/* Field-offset pins for the v3 extension. A reorder that preserves
 * sizeof() (e.g. swapping two uint64_t fields) wouldn't fire the
 * size assert above but would silently mis-address the kernel-
 * launch state. These catch that; the v2 fields are pinned
 * implicitly by the identical layout on both sides of a v2 helper
 * that only reads up to offset 120. `offsetof` needs <stddef.h>,
 * included at the top of this header. */
_Static_assert(offsetof(struct ga10b_channel_handoff, shader_phys)    == 120,
               "v3 shader_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, shader_gpu_va)  == 128,
               "v3 shader_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, cbuf_phys)      == 136,
               "v3 cbuf_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, cbuf_gpu_va)    == 144,
               "v3 cbuf_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_phys)       == 152,
               "v3 qmd_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_gpu_va)     == 160,
               "v3 qmd_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, output_phys)    == 168,
               "v3 output_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, output_gpu_va)  == 176,
               "v3 output_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, shader_size)    == 184,
               "v3 shader_size offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, cbuf_size)      == 188,
               "v3 cbuf_size offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, expected_payload) == 192,
               "v4 expected_payload offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, pipeline_n_ops) == 200,
               "v5 pipeline_n_ops offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, pipeline_ops_phys) == 208,
               "v5 pipeline_ops_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, input_buf_phys) == 216,
               "v6 input_buf_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, input_buf_size) == 224,
               "v6 input_buf_size offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, pipeline_kind) == 228,
               "PR-3 pipeline_kind offset drifted (must be 228 — last "
               "u32 of the v6 struct, was previously _pad4)");

/* v7 pool descriptor — appended after pipeline_kind. Pinning each
 * field's offset lets a v7-aware reader detect a v6-built handoff
 * (where these bytes would be zero) and fall back to the v6 dispatch
 * path. */
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_pool_phys) == 232,
               "v7 qmd_pool_phys offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_pool_gpu_va) == 240,
               "v7 qmd_pool_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_pool_size_bytes) == 248,
               "v7 qmd_pool_size_bytes offset drifted");
_Static_assert(offsetof(struct ga10b_channel_handoff, qmd_pool_n_slots) == 252,
               "v7 qmd_pool_n_slots offset drifted");

_Static_assert(offsetof(struct ga10b_pipeline_op, qmd_gpu_va) == 0,
               "pipeline_op.qmd_gpu_va must be at offset 0");
_Static_assert(offsetof(struct ga10b_pipeline_op, output_phys) == 8,
               "pipeline_op.output_phys must be at offset 8");
_Static_assert(offsetof(struct ga10b_pipeline_op, expected_payload) == 16,
               "pipeline_op.expected_payload must be at offset 16");

/* v7 per-op layout — leading 24 bytes match v6, then new fields
 * append. Pinning each field's offset catches accidental reorders
 * that would leave sizeof() unchanged but break wire compatibility. */
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, qmd_gpu_va) == 0,
               "v7 op qmd_gpu_va must match v6 layout (offset 0)");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, output_phys) == 8,
               "v7 op output_phys must match v6 layout (offset 8)");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, expected_payload) == 16,
               "v7 op expected_payload must match v6 layout (offset 16)");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, flags) == 20,
               "v7 op flags must match v6 layout (offset 20)");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, shader_gpu_va) == 24,
               "v7 op shader_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, cbuf_gpu_va) == 32,
               "v7 op cbuf_gpu_va offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, register_count_v) == 40,
               "v7 op register_count_v offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, grid_x) == 44,
               "v7 op grid_x offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, grid_y) == 48,
               "v7 op grid_y offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, grid_z) == 52,
               "v7 op grid_z offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, block_x) == 56,
               "v7 op block_x offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, block_y) == 60,
               "v7 op block_y offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, block_z) == 64,
               "v7 op block_z offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, smem_size_bytes) == 68,
               "v7 op smem_size_bytes offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, slm_size_bytes) == 72,
               "v7 op slm_size_bytes offset drifted");
_Static_assert(offsetof(struct ga10b_pipeline_op_v7, barrier_count) == 76,
               "v7 op barrier_count offset drifted");

/*
 * Validate a candidate handoff block. Returns 0 iff magic, version,
 * addresses (all non-null), and gpfifo_entries (non-zero power of two)
 * are all correct. Pure-logic — no MMIO, host-testable.
 */
int ga10b_validate_handoff(const struct ga10b_channel_handoff *h);

/*
 * Scan a physical-memory range for the handoff magic at the given
 * stride. Returns the address of the first match, or 0 if not found.
 * Production callers use the IOVMM heap range; host tests pass their
 * own range over a mocked buffer.
 */
uint64_t ga10b_find_handoff_in_range(uint64_t start, uint64_t end,
                                     uint64_t stride);

/*
 * Same as `ga10b_find_handoff_in_range` but only returns a match
 * whose `pipeline_kind` field equals `wanted_kind` (one of `enum
 * ga10b_pipeline_kind`). Skips magic-matching candidates whose
 * validation fails or whose kind doesn't match, continuing the
 * scan past each. Returns 0 if no kind-matching handoff exists.
 *
 * PR-3 of docs/specs/gpu-policy-models.md. Production callers wrap
 * this in `ga10b_bringup_channel_kind`; host tests pass mocked
 * memory ranges to validate the kind-discriminator logic.
 *
 * Pure-logic — no MMIO, no globals, host-testable.
 */
uint64_t ga10b_find_handoff_of_kind_in_range(uint64_t start, uint64_t end,
                                             uint64_t stride,
                                             uint32_t wanted_kind);

/*
 * Pick the poll-target payload for `nvgpu launch-kernel`. v4 handoffs
 * carry a per-kernel `expected_payload`; v3 (and v4 handoffs where
 * the field is left zero) fall back to the caller-supplied default.
 *
 * `fallback` is typically GA10B_SMOKETEST_SEM_PAYLOAD (0xCAFE from
 * the write_cafe era) so existing v3 flows keep working even after
 * SLM-OS was taught to accept v4.
 *
 * Pure-logic — no MMIO, host-testable.
 */
static inline uint32_t
ga10b_pick_launch_payload(const struct ga10b_channel_handoff *h,
                          uint32_t fallback)
{
    return (h->version >= 4u && h->expected_payload != 0u)
           ? h->expected_payload
           : fallback;
}

/*
 * Test whether a freshly-read poll value indicates the dispatched
 * kernel has completed.
 *
 * Two modes, distinguished by `expected_payload`:
 *
 *   exact-match (expected_payload != 0): the kernel writes a known
 *     bit pattern (0xCAFE, the dot4 sentinel 300, etc.) — used for
 *     legacy v3/v4 single-shot dispatches where the launcher and
 *     SLM-OS agree on the value ahead of time.
 *
 *   any-non-zero (expected_payload == 0): the kernel's output is
 *     not predictable bit-for-bit (e.g. MNIST conv outputs whose
 *     low fp32 bits depend on FFMA ordering vs CPU reference). The
 *     caller pre-zeroes the poll target; any non-zero write counts
 *     as completion. Used by v5 multi-op pipelines.
 *
 * Pure-logic — host-testable.
 */
static inline bool
ga10b_poll_match(uint32_t poll_val, uint32_t expected_payload)
{
    return (expected_payload == 0u)
           ? (poll_val != 0u)
           : (poll_val == expected_payload);
}

/*
 * Compute the next GP_PUT value for a GPFIFO of `gpfifo_entries`
 * entries (must be a non-zero power of two), wrapping at the ring
 * boundary. Mirrors nvgpu's `gv11b_userd_gp_put` — see
 * `nvgpu-common-fifo-submit.c:nvgpu_submit_append_gpfifo_kernel` and
 * `nvgpu-hal-fifo-userd_gv11b.c:gv11b_userd_gp_put` in the reference
 * cache. PBDMA's internal tracking expects a value strictly less than
 * `gpfifo_entries`; writing an unmasked value at the wraparound (i.e.
 * gp_put == gpfifo_entries) silently stops PBDMA from seeing new
 * submits — observed as the `GP_GET didn't advance` wedge in #601
 * after the ring shrank from 1024 to 512 entries to fit one page.
 *
 * Pure-logic — no MMIO, host-testable.
 */
static inline uint32_t
ga10b_next_gp_put(uint32_t cur_gp_put, uint32_t gpfifo_entries)
{
    return (cur_gp_put + 1u) & (gpfifo_entries - 1u);
}

/*
 * Sanity-check a single entry in the v5 pipeline ops array. Returns
 * true iff the op has plausible non-zero addresses (qmd_gpu_va,
 * output_phys). Does NOT verify that those addresses are actually
 * mapped or that the QMD content is sensible — those checks happen
 * implicitly when SLM-OS dispatches the op and polls the output.
 *
 * Used by the kernel-side pipeline runner to fail fast on a
 * malformed handoff rather than dispatching a QMD address of 0
 * (which the GPU treats as a noop with no error reported).
 *
 * Pure-logic — host-testable.
 */
static inline bool
ga10b_pipeline_op_is_valid(const struct ga10b_pipeline_op *op)
{
    return op != NULL
        && op->qmd_gpu_va != 0u
        && op->output_phys != 0u;
}

/*
 * Should the dispatch path take the v7 (per-dispatch QMD) branch?
 *
 * Returns true iff the handoff carries the load-bearing v7 markers:
 * `version >= 7` (the wire-format check), pipeline_n_ops > 0 (any
 * pipeline at all), qmd_pool_n_slots > 0 (a usable pool sized), and
 * qmd_pool_phys != 0 (a usable pool mapped).
 *
 * A v6 helper writes `version = 6` and zero pool fields, so
 * `is_v7` returns false and the dispatch path falls through to the
 * v5/v6 helper-baked-QMD branch. A v7 helper that hasn't allocated
 * a pool would advance `version` but leave the pool fields zero,
 * which also returns false — defensive belt-and-braces against a
 * partial v7 rollout.
 *
 * Pure-logic — host-testable.
 */
static inline bool
ga10b_handoff_is_v7(const struct ga10b_channel_handoff *h)
{
    return h != NULL
        && h->version >= 7u
        && h->pipeline_n_ops > 0u
        && h->qmd_pool_n_slots > 0u
        && h->qmd_pool_phys != 0u;
}

/*
 * Reasons a v7 handoff that *passed* `ga10b_handoff_is_v7` may
 * still be malformed. The dispatch path returns -1 + sets
 * `last_error_phase = 8` on any of these.
 */
enum ga10b_v7_validation_error {
    GA10B_V7_OK                       = 0,
    GA10B_V7_ERR_OPS_PHYS_ZERO        = 1,  /* pipeline_ops_phys == 0 */
    GA10B_V7_ERR_OPS_EXCEED_CAP       = 2,  /* pipeline_n_ops > V7_MAX */
    GA10B_V7_ERR_POOL_SIZE_INSUFFICIENT = 3,/* qmd_pool_size_bytes < n_slots × 256 */
    GA10B_V7_ERR_NULL_HANDOFF         = 4,  /* h == NULL */
};

/*
 * Validate a handoff that's already been classified v7 by
 * `ga10b_handoff_is_v7`. Returns GA10B_V7_OK on a well-formed
 * handoff, or one of the GA10B_V7_ERR_* codes describing the
 * specific malformation.
 *
 * Self-enforcing contract: `ga10b_handoff_is_v7` already null-
 * checks, so a properly-sequenced caller never passes NULL — but
 * a future direct caller that skipped `is_v7` would otherwise
 * NULL-deref. The explicit check returns
 * GA10B_V7_ERR_NULL_HANDOFF instead. Defense-in-depth, no hot-
 * path cost.
 *
 * Pure-logic — host-testable. The dispatch-path call site logs the
 * specific failure via uart_printf and returns -1 to the bringup
 * caller; the host harness uses the return code directly.
 */
static inline enum ga10b_v7_validation_error
ga10b_v7_validate_handoff(const struct ga10b_channel_handoff *h)
{
    if (h == NULL) {
        return GA10B_V7_ERR_NULL_HANDOFF;
    }
    if (h->pipeline_ops_phys == 0u) {
        return GA10B_V7_ERR_OPS_PHYS_ZERO;
    }
    if (h->pipeline_n_ops > GA10B_PIPELINE_V7_MAX_OPS) {
        return GA10B_V7_ERR_OPS_EXCEED_CAP;
    }
    if ((uint64_t)h->qmd_pool_size_bytes <
        (uint64_t)h->qmd_pool_n_slots * 256u) {
        return GA10B_V7_ERR_POOL_SIZE_INSUFFICIENT;
    }
    return GA10B_V7_OK;
}

#endif /* GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H */
