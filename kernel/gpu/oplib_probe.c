/*
 * oplib_probe.c - Boot-time op-tier probe (#714, B.2).
 *
 * Walks every SLM op_kind in [SLM_GPU_OP_RMSNORM, SLM_GPU_OP_LM_HEAD]
 * and validates that the dispatcher's per-op metadata accepts a
 * representative fixture. For ops that pass, calls into the Rust
 * runtime's TIER_TABLE setter so the SLM forward path routes that
 * op through the GPU dispatcher instead of the CPU NEON fallback.
 *
 * Two-stage scope per #714:
 *
 *   B.2 (this file, today): "static" validation — registry hit +
 *       cbuf-builder accepts a per-op fixture + launch-shape returns
 *       a non-zero grid. Sets Tier::Simt on success, leaves Cpu on
 *       miss. The fixtures here mirror the realistic SLM shapes the
 *       Qwen2.5-1.5B forward path will hit at run time.
 *
 *   B.3 (follow-on): once `Backend::execute` is wired through the
 *       OperatorLibraryBackend, extend this probe to actually fire
 *       each op's fixture through the GPU and compare against the
 *       embedded CPU reference output before flipping the tier.
 *
 * The probe runs after SASS pool staging because that's the earliest
 * point both halves of the dispatch path are ready: the SASS bytes
 * are GMMU-mapped (so a future B.3 hardware probe can fire), and the
 * dispatcher metadata is link-time present (so today's static probe
 * has something to validate).
 */

#include "oplib_probe.h"

#include "gpu_handoff.h"          /* SLM_GPU_OP_* */
#include "operator_dispatch.h"
#include "uart.h"
#include "slm_ffi.h"              /* slm_runtime_set_tier_simt */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Op-name table for the UART summary. Indices match SLM_GPU_OP_*
 * values 0..7. The 8-wide range is the SLM op set; the loop below
 * relies on `SLM_GPU_OP_LM_HEAD` being the last SLM op_kind. A
 * future enum addition that inserts a value into the SLM range
 * would silently drop the new op from this summary; pin the bound
 * at compile time so an enum drift breaks the build instead. */
_Static_assert(SLM_GPU_OP_RMSNORM == 0,
               "OP_NAMES table is indexed from 0 = SLM_GPU_OP_RMSNORM");
_Static_assert(SLM_GPU_OP_LM_HEAD == 7,
               "OP_NAMES table sizes (8) and the loop bound assume "
               "SLM_GPU_OP_LM_HEAD == 7. If the SLM op_kind range "
               "grows, widen OP_NAMES, the loop bound, and the "
               "`bool simt[8]` array in oplib_probe_run.");
static const char *const OP_NAMES[8] = {
    "RMSNORM",
    "ROPE",
    "EMBEDDING",
    "Q4K_DOT",
    "Q4K_GEMM",
    "GQA_ATTN",
    "SWIGLU",
    "LM_HEAD",
};

/* Build a representative fixture for the given op_kind. The fixture
 * must satisfy every defensive check in the per-op build_cbuf —
 * non-zero scalars, non-NULL VAs, divisibility / range constraints —
 * so a successful build means the dispatcher's C-side path is
 * link-time intact. The chosen shapes mirror Qwen2.5-1.5B at decode
 * time so the probe's "passes" set matches what the real forward
 * path will use.
 *
 * The fixture's GPU VAs are dummy non-zero values; the static probe
 * never dereferences them. The hardware probe variant in B.3 will
 * carve real scratch from OPLIB_POOL_OFF_SCRATCH* and overwrite
 * these. */
static int build_fixture(uint32_t op_kind,
                          struct operator_dispatch_args *out)
{
    memset(out, 0, sizeof(*out));
    out->op_kind = op_kind;
    /* Dummy VAs — non-zero so the NULL-check fires only on real
     * misuse, never on the probe itself. The values themselves are
     * never dereferenced in the static probe. */
    const uint64_t dummy_a = 0x1000ull;
    const uint64_t dummy_b = 0x2000ull;
    const uint64_t dummy_c = 0x3000ull;
    const uint64_t dummy_d = 0x4000ull;

    switch (op_kind) {
    case SLM_GPU_OP_RMSNORM:
        /* Qwen2.5 per-token decode: 1 row, n=1536. */
        out->u.rmsnorm.x_gpu_va     = dummy_a;
        out->u.rmsnorm.gamma_gpu_va = dummy_b;
        out->u.rmsnorm.out_gpu_va   = dummy_c;
        out->u.rmsnorm.n_rows       = 1u;
        out->u.rmsnorm.n            = 1536u;
        out->u.rmsnorm.eps_bits     = 0x358637BDu;  /* 1e-6f */
        return 0;
    case SLM_GPU_OP_ROPE:
        /* Qwen2.5 attention: 1 token × 12 heads × 128 head_dim. */
        out->u.rope.vec_gpu_va       = dummy_a;
        out->u.rope.positions_gpu_va = dummy_b;
        out->u.rope.cos_sin_gpu_va   = dummy_c;
        out->u.rope.batch            = 1u;
        out->u.rope.num_heads        = 12u;
        out->u.rope.head_dim         = 128u;
        return 0;
    case SLM_GPU_OP_EMBEDDING:
        /* Qwen2.5 vocab → 1536-wide row → 6 super-blocks × 144 B. */
        out->u.embedding.table_gpu_va     = dummy_a;
        out->u.embedding.out_gpu_va       = dummy_b;
        out->u.embedding.token_id         = 0u;
        out->u.embedding.embedding_length = 1536u;
        out->u.embedding.table_row_bytes  = 864u;   /* 6 × 144 */
        return 0;
    case SLM_GPU_OP_Q4K_DOT:
        /* Qwen2.5 Q/O projection: K=1536, N=1536. */
        out->u.q4k_dot.x_gpu_va       = dummy_a;
        out->u.q4k_dot.weights_gpu_va = dummy_b;
        out->u.q4k_dot.out_gpu_va     = dummy_c;
        out->u.q4k_dot.k              = 1536u;
        out->u.q4k_dot.n              = 1536u;
        return 0;
    case SLM_GPU_OP_GQA_ATTN:
        /* Qwen2.5: 12 Q heads × 2 KV heads × 128 head_dim, seq=1. */
        out->u.gqa_attn.q_gpu_va    = dummy_a;
        out->u.gqa_attn.k_gpu_va    = dummy_b;
        out->u.gqa_attn.v_gpu_va    = dummy_c;
        out->u.gqa_attn.out_gpu_va  = dummy_d;
        out->u.gqa_attn.n_head_q    = 12u;
        out->u.gqa_attn.n_head_kv   = 2u;
        out->u.gqa_attn.head_dim    = 128u;
        out->u.gqa_attn.seq_len     = 1u;
        return 0;
    case SLM_GPU_OP_SWIGLU:
        /* Qwen2.5 per-token decode: n = intermediate_size = 8960. */
        out->u.swiglu.gate_gpu_va = dummy_a;
        out->u.swiglu.up_gpu_va   = dummy_b;
        out->u.swiglu.out_gpu_va  = dummy_c;
        out->u.swiglu.n           = 8960u;
        return 0;
    /* Q4K_GEMM, LM_HEAD have no SASS shipped today (Q4K_GEMM is in
     * MANIFEST.json::future_entries; LM_HEAD reuses Q4K_DOT under
     * the hood) — the registry returns NULL for these and we leave
     * the tier at Cpu. No fixture needed. */
    default:
        return -1;
    }
}

int oplib_probe_run(void)
{
    int simt_count = 0;
    bool simt[8] = { false, false, false, false,
                     false, false, false, false };

    for (uint32_t op = 0; op < 8u; op++) {
        const struct operator_dispatch_metadata *m =
            operator_dispatch_get_metadata(op);
        if (m == NULL) {
            /* Not registered — leave at Cpu. */
            continue;
        }

        struct operator_dispatch_args args;
        if (build_fixture(op, &args) != 0) {
            continue;
        }

        /* 4 KB cbuf scratch on the kernel stack; the probe's stack
         * frame is short-lived so this fits comfortably under the
         * 256 KB STACK_SIZE budget. */
        uint8_t cbuf[4096];
        memset(cbuf, 0, sizeof(cbuf));
        if (m->build_cbuf(cbuf, &args) != 0) {
            continue;
        }

        struct operator_launch_shape shape;
        memset(&shape, 0, sizeof(shape));
        if (m->launch_shape(&args, &shape) != 0) {
            continue;
        }
        /* A zero grid would mean the launch-shape function silently
         * accepted out-of-range args; treat as a probe failure. */
        if (shape.grid_x == 0u || shape.block_x == 0u) {
            continue;
        }

        /* Static path validates — flip the Rust tier table. */
        if (slm_runtime_set_tier_simt(op) == 0) {
            simt[op] = true;
            simt_count++;
        }
    }

    /* One-line UART summary, op-by-op. */
    uart_puts("[oplib-probe]");
    for (uint32_t op = 0; op < 8u; op++) {
        uart_printf(" %s=%s", OP_NAMES[op], simt[op] ? "Simt" : "Cpu");
    }
    uart_puts("\r\n");

    return simt_count;
}
