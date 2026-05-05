/*
 * gpu_handoff.h - SLM-OS GPU handoff descriptor (M6.A scaffolding)
 *
 * The pre-kexec L4T loader (`scripts/slm-gpu-bringup.c`, M6.A-2 — to be
 * authored) builds an `slm_gpu_handoff_v1_t` instance describing the
 * GPU resources inherited across the kexec boundary: weight pool VA,
 * SASS kernel pool VA, channel/pushbuffer/semaphore/doorbell pages,
 * and a flat array of per-op descriptors that selects which SASS
 * kernel variant runs for each layer of the transformer pipeline.
 *
 * Post-kexec, SLM-OS reads the staged handoff page (kernel-side helper
 * `slm_gpu_get_handoff_phys()` returns the physical address; the Rust
 * runtime in `runtime/src/inference/gpu_slm.rs` consumes it).
 *
 * The op_desc array is laid out contiguously in memory immediately
 * after the header, so:
 *
 *     slm_gpu_handoff_v1_t *h    = (slm_gpu_handoff_v1_t *)addr;
 *     slm_gpu_op_desc_t    *ops  = (slm_gpu_op_desc_t *)(h + 1);
 *
 * Status: M6.A-1 schema only. The actual SASS kernels (M6.B / M6.C /
 * M6.D), pushbuffer construction, semaphore polling, and the pre-kexec
 * loader are deferred per `docs/specs/gpu-slm-handoff.md`.
 *
 * Op-kind values are mirrored in the Rust side as `OpKind`; keep the
 * two enums in sync. Tier values are mirrored as `Tier`.
 */

#ifndef GPU_HANDOFF_H
#define GPU_HANDOFF_H

#include <stdint.h>

/*
 * Magic / version. The bare-metal parser checks both BEFORE reading
 * any other field — if either fails the validation, the parser
 * abandons the handoff page and falls back to CPU. Bumping the
 * version is how we evolve the schema without confusing older
 * SLM-OS images.
 */
#define SLM_GPU_HANDOFF_MAGIC    0x534C4D47u   /* "SLMG" little-endian */
#define SLM_GPU_HANDOFF_VERSION  1u

/*
 * Cap on per-op descriptors. Sized for Qwen2.5-1.5B (28 layers × ~9
 * ops/layer = 252) with a small headroom; `slm-gpu-bringup` rejects
 * model plans that exceed this. Bumping this cap is a schema-version
 * bump.
 */
#define SLM_GPU_HANDOFF_MAX_OPS  256u

/*
 * Per-op tier flag. The pre-kexec loader sets this based on which
 * SASS kernels were successfully compiled and signed for the op.
 * The bare-metal dispatch reads it to pick which kernel offset to
 * jump to in the SASS pool. See docs/specs/gpu-slm-handoff.md
 * §"Tier model" for the full fallback ladder.
 */
#define SLM_GPU_TIER_AUTO  0u  /* prefer Tier1, fall back to Tier2 → CPU */
#define SLM_GPU_TIER_HMMA  1u  /* Tier 1: HMMA tensor-core kernel        */
#define SLM_GPU_TIER_SIMT  2u  /* Tier 2: CUDA-core fp16 fallback        */
#define SLM_GPU_TIER_CPU   3u  /* Skip GPU entirely; M4 NEON path        */

/*
 * Per-op descriptor. One entry per matmul / fused-attention / element-
 * wise op in the transformer pipeline. The pre-kexec loader fills in
 * `sass_kernel_offset` (offset into the SASS pool) and `tier`; SLM-OS
 * dispatches by `op_kind` × `tier`.
 *
 * `weight_va` is the GPU virtual address of the op's weights inside
 * `weight_pool_va`. Element-wise ops (RmsNorm, RoPE) have weight_size
 * == 0.
 *
 * Total size: 64 bytes. Stays power-of-two to keep `MAX_OPS` × this
 * struct cache-friendly.
 */
typedef struct {
    uint32_t op_kind;             /* enum slm_gpu_op_kind         */
    uint32_t tier;                /* SLM_GPU_TIER_*               */
    uint64_t sass_kernel_offset;  /* offset into sass_kernel_pool */
    uint64_t sass_kernel_size;
    uint64_t weight_va;           /* offset/VA into weight_pool   */
    uint64_t weight_size;
    uint32_t input_dim;
    uint32_t output_dim;
    uint32_t flags;               /* reserved; must be 0          */
    uint32_t _pad;
    /*
     * Reserved 8 bytes to round the struct to exactly 64 B so a
     * MAX_OPS × desc array stays cache-friendly and the
     * `_Static_assert` below has a clean numeric target. Future
     * additions land here before bumping SLM_GPU_HANDOFF_VERSION.
     */
    uint64_t _reserved;
} slm_gpu_op_desc_t;

/*
 * Op-kind enum — keep in sync with runtime/src/inference/gpu_slm.rs::OpKind.
 *
 * Tier 1 / Tier 2 SASS kernels exist only for the matmul-shaped ops
 * (Q4kDot, Q4kGemm, GqaAttn, SwiGlu, LmHead). The element-wise /
 * gather ops (RmsNorm, Rope, Embedding) ship as a single SIMT kernel
 * variant — see plan §M6.D.
 */
enum slm_gpu_op_kind {
    SLM_GPU_OP_RMSNORM   = 0,  /* element-wise, single tier         */
    SLM_GPU_OP_ROPE      = 1,  /* element-wise, single tier         */
    SLM_GPU_OP_EMBEDDING = 2,  /* gather, single tier               */
    SLM_GPU_OP_Q4K_DOT   = 3,  /* matmul (decode), Tier-1 + Tier-2  */
    SLM_GPU_OP_Q4K_GEMM  = 4,  /* matmul (prefill), Tier-1 + Tier-2 */
    SLM_GPU_OP_GQA_ATTN  = 5,  /* fused attention, Tier-1 + Tier-2  */
    SLM_GPU_OP_SWIGLU    = 6,  /* fused MLP, Tier-1 + Tier-2        */
    SLM_GPU_OP_LM_HEAD   = 7,  /* final projection, Tier-1 + Tier-2 */
};

/*
 * Handoff header. Followed in memory by `op_count` consecutive
 * `slm_gpu_op_desc_t` entries.
 *
 * Architecture dimensions mirror runtime/src/slm/gguf.rs::ArchInfo
 * so the bare-metal side does not need to re-parse the GGUF header
 * — the L4T loader already did the parse pre-kexec.
 *
 * `arch_kind`: 0 = qwen2, 1 = llama. Future architectures bump the
 * enum; an unknown value causes the bare-metal parser to fall back
 * to CPU.
 */
typedef struct {
    /* Schema gates: validate BEFORE reading any other field. */
    uint32_t magic;            /* SLM_GPU_HANDOFF_MAGIC   */
    uint32_t version;          /* SLM_GPU_HANDOFF_VERSION */
    uint32_t op_count;         /* total ops across all layers (≤ MAX_OPS) */
    uint32_t arch_kind;        /* 0 = qwen2, 1 = llama    */

    /* Architecture dimensions (mirrors runtime/src/slm/gguf.rs::ArchInfo). */
    uint32_t block_count;
    uint32_t embedding_length;
    uint32_t head_count;
    uint32_t head_count_kv;
    uint32_t head_dim;
    uint32_t feed_forward_length;
    uint32_t context_length;
    uint32_t vocab_size;

    /* Channel resources (inherited from L4T's nvgpu). */
    uint64_t channel_id;
    uint64_t pushbuffer_va;
    uint64_t pushbuffer_size;
    uint64_t semaphore_page_va;
    uint64_t doorbell_page_va;

    /*
     * Weight pool: contiguous chunk in GPU VA space holding all
     * Q4_K-packed weight tensors. Per-op `weight_va` offsets index
     * into it.
     */
    uint64_t weight_pool_va;
    uint64_t weight_pool_size;

    /*
     * SASS kernel pool: read-only code segment with the M6.B / M6.C /
     * M6.D kernels concatenated. Per-op `sass_kernel_offset` selects
     * one. The pool is mapped read-only / executable on the GPU.
     */
    uint64_t sass_kernel_pool_va;
    uint64_t sass_kernel_pool_size;
} slm_gpu_handoff_v1_t;

/*
 * Catch struct bloat early. 256 bytes leaves comfortable headroom
 * for adding a few more channel-resource fields without forcing a
 * version bump every time. If a future expansion legitimately needs
 * more, bump the cap and the schema version together.
 */
_Static_assert(sizeof(slm_gpu_handoff_v1_t) <= 256,
               "slm_gpu_handoff_v1_t must stay <= 256 bytes; "
               "bump SLM_GPU_HANDOFF_VERSION when growing the header.");

/*
 * Pin the exact header size so the runtime-side Rust mirror's
 * `const _: () = assert!(size_of::<HandoffHeader>() == ...)` can
 * track it. Op-desc array offset = sizeof(header).
 */
_Static_assert(sizeof(slm_gpu_handoff_v1_t) == 120,
               "slm_gpu_handoff_v1_t must be exactly 120 bytes; "
               "if growing, update HANDOFF_HEADER_SIZE_BYTES in "
               "runtime/src/inference/gpu_slm.rs and bump version.");

/* Per-op descriptor must stay 64 bytes — keeps MAX_OPS × desc array
 * a clean 16 KB. */
_Static_assert(sizeof(slm_gpu_op_desc_t) == 64,
               "slm_gpu_op_desc_t must be exactly 64 bytes.");

#endif /* GPU_HANDOFF_H */
