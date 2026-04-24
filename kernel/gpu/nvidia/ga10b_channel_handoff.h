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
                                 * SLM-OS channel inherit accepts any of
                                 * v2/v3/v4; `nvgpu launch-kernel` needs
                                 * at least v3 for shader/QMD fields and
                                 * falls back to GA10B_SMOKETEST_SEM_PAYLOAD
                                 * for v3 handoffs that don't carry
                                 * `expected_payload`. */
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
};

/* Wire-format size is locked: both the Linux helper and SLM-OS
 * depend on this exact layout. Any struct reorder or field addition
 * breaks the handoff silently — the static_assert catches it at
 * compile time on both sides. */
_Static_assert(sizeof(struct ga10b_channel_handoff) == 200,
               "ga10b_channel_handoff layout changed — update Linux "
               "helper (scripts/gpu-channel-helper.c, "
               "scripts/gpu-kernel-launch.c, scripts/gpu-launch-common.c) "
               "and bump version");

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

#endif /* GPU_NVIDIA_GA10B_CHANNEL_HANDOFF_H */
