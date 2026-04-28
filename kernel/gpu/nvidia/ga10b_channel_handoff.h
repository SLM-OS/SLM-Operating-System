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
                                 * SLM-OS channel inherit accepts any of
                                 * v2..v6; `nvgpu launch-kernel` needs
                                 * at least v3 for shader/QMD fields,
                                 * runs the v5 pipeline if
                                 * `pipeline_n_ops > 0`, otherwise
                                 * single-shot per the v4 path. The v6
                                 * input swap path is opt-in via
                                 * `slm_gpu_set_mnist_input` /
                                 * `slm.gpu_set_mnist_input`. */
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

/* One entry per op in a v5 pipeline. SLM-OS reads this array from
 * the DRAM page at handoff->pipeline_ops_phys. */
struct ga10b_pipeline_op {
    uint64_t qmd_gpu_va;        /* GPU VA of this op's QMD (256 B aligned) */
    uint64_t output_phys;       /* CPU-physical sentinel target */
    uint32_t expected_payload;  /* value to poll for; 0 → "any non-zero" */
    uint32_t flags;             /* reserved (0 today) */
};

/* Upper bound on pipeline length, enforced by the kernel-side runner.
 * The launcher allocates a single 4 KB page for the ops array
 * (4096 / sizeof(struct ga10b_pipeline_op) = 170), so SLM-OS rejects
 * any handoff that claims more — anything past that would dereference
 * past the page into adjacent memory. MNIST currently uses 8 ops;
 * real SLMs may need this raised, but a finite cap matters more than
 * a generous one. */
#define GA10B_PIPELINE_MAX_OPS 170u

/* Wire-format size is locked: both the Linux helper and SLM-OS
 * depend on this exact layout. Any struct reorder or field addition
 * breaks the handoff silently — the static_assert catches it at
 * compile time on both sides. */
_Static_assert(sizeof(struct ga10b_channel_handoff) == 232,
               "ga10b_channel_handoff layout changed — update Linux "
               "helper (scripts/gpu-channel-helper.c, "
               "scripts/gpu-kernel-launch.c, scripts/gpu-launch-common.c) "
               "and bump version");
_Static_assert(sizeof(struct ga10b_pipeline_op) == 24,
               "ga10b_pipeline_op layout changed — Linux + SLM-OS "
               "must agree on the per-op size");

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
               "u32 in the struct, was previously _pad4)");
_Static_assert(offsetof(struct ga10b_pipeline_op, qmd_gpu_va) == 0,
               "pipeline_op.qmd_gpu_va must be at offset 0");
_Static_assert(offsetof(struct ga10b_pipeline_op, output_phys) == 8,
               "pipeline_op.output_phys must be at offset 8");
_Static_assert(offsetof(struct ga10b_pipeline_op, expected_payload) == 16,
               "pipeline_op.expected_payload must be at offset 16");

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

#endif /* GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H */
