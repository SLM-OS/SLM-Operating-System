/*
 * test_operator_dispatch.c - Unit tests for the per-op dispatch
 * registry (#714, A.2).
 *
 * Covers:
 *
 *   1. Lookup hits + misses on op_kind.
 *   2. RMSNORM cbuf builder: writes the documented fields at the
 *      documented offsets within OPERATOR_CBUF0_BASE.
 *   3. RMSNORM launch shape: grid = (n_rows, 1, 1), block = (256, 1, 1).
 *   4. ROPE cbuf builder + launch shape: pinned offsets, identity-
 *      shape grid/block, barrier_count=0 (no syncthreads in SASS).
 *   5. Builder rejects malformed inputs (NULL, op_kind mismatch,
 *      zero rows / cols, NULL GPU VAs, odd head_dim).
 *
 * Pure-logic. No GPU, no MMIO. Runs on QEMU.
 */

#include "unity.h"
#include "../include/operator_dispatch.h"
#include "../include/gpu_handoff.h"

#include <stdint.h>
#include <string.h>

/* Pin the documented cbuf offsets at compile time so a future
 * rebuild of the SASS that nudges field positions surfaces here
 * before silently corrupting the dispatch path. */
_Static_assert(RMSNORM_CBUF_OFFSET_X     == 0x00,
               "RMSNORM x must live at cbuf[0] + 0x00");
_Static_assert(RMSNORM_CBUF_OFFSET_GAMMA == 0x08,
               "RMSNORM gamma must live at cbuf[0] + 0x08");
_Static_assert(RMSNORM_CBUF_OFFSET_OUT   == 0x10,
               "RMSNORM out must live at cbuf[0] + 0x10");
_Static_assert(RMSNORM_CBUF_OFFSET_N     == 0x18,
               "RMSNORM n must live at cbuf[0] + 0x18");
_Static_assert(RMSNORM_CBUF_OFFSET_EPS   == 0x1C,
               "RMSNORM eps must live at cbuf[0] + 0x1C");

_Static_assert(ROPE_CBUF_OFFSET_VEC       == 0x00,
               "ROPE vec must live at cbuf[0] + 0x00");
_Static_assert(ROPE_CBUF_OFFSET_POSITIONS == 0x08,
               "ROPE positions must live at cbuf[0] + 0x08");
_Static_assert(ROPE_CBUF_OFFSET_COS_SIN   == 0x10,
               "ROPE cos_sin must live at cbuf[0] + 0x10");
_Static_assert(ROPE_CBUF_OFFSET_BATCH     == 0x18,
               "ROPE batch must live at cbuf[0] + 0x18");
_Static_assert(ROPE_CBUF_OFFSET_NUM_HEADS == 0x1C,
               "ROPE num_heads must live at cbuf[0] + 0x1C");
_Static_assert(ROPE_CBUF_OFFSET_HEAD_DIM  == 0x20,
               "ROPE head_dim must live at cbuf[0] + 0x20");

_Static_assert(EMBEDDING_CBUF_OFFSET_TABLE     == 0x00,
               "EMBEDDING table must live at cbuf[0] + 0x00");
_Static_assert(EMBEDDING_CBUF_OFFSET_OUT       == 0x08,
               "EMBEDDING out must live at cbuf[0] + 0x08");
_Static_assert(EMBEDDING_CBUF_OFFSET_TOKEN_ID  == 0x10,
               "EMBEDDING token_id must live at cbuf[0] + 0x10");
_Static_assert(EMBEDDING_CBUF_OFFSET_EMBED_LEN == 0x14,
               "EMBEDDING embedding_length must live at cbuf[0] + 0x14");
_Static_assert(EMBEDDING_CBUF_OFFSET_ROW_BYTES == 0x18,
               "EMBEDDING table_row_bytes must live at cbuf[0] + 0x18");

_Static_assert(Q4K_DEQUANT_CBUF_OFFSET_BLOCKS == 0x00,
               "Q4K_DEQUANT blocks must live at cbuf[0] + 0x00");
_Static_assert(Q4K_DEQUANT_CBUF_OFFSET_OUT    == 0x08,
               "Q4K_DEQUANT out must live at cbuf[0] + 0x08");
_Static_assert(Q4K_DEQUANT_CBUF_OFFSET_NB     == 0x10,
               "Q4K_DEQUANT nb must live at cbuf[0] + 0x10");

_Static_assert(SWIGLU_CBUF_OFFSET_GATE == 0x00,
               "SWIGLU gate must live at cbuf[0] + 0x00");
_Static_assert(SWIGLU_CBUF_OFFSET_UP   == 0x08,
               "SWIGLU up must live at cbuf[0] + 0x08");
_Static_assert(SWIGLU_CBUF_OFFSET_OUT  == 0x10,
               "SWIGLU out must live at cbuf[0] + 0x10");
_Static_assert(SWIGLU_CBUF_OFFSET_N    == 0x18,
               "SWIGLU n must live at cbuf[0] + 0x18");

_Static_assert(Q4K_DOT_CBUF_OFFSET_X       == 0x00,
               "Q4K_DOT x must live at cbuf[0] + 0x00");
_Static_assert(Q4K_DOT_CBUF_OFFSET_WEIGHTS == 0x08,
               "Q4K_DOT weights must live at cbuf[0] + 0x08");
_Static_assert(Q4K_DOT_CBUF_OFFSET_OUT     == 0x10,
               "Q4K_DOT out must live at cbuf[0] + 0x10");
_Static_assert(Q4K_DOT_CBUF_OFFSET_K       == 0x18,
               "Q4K_DOT K must live at cbuf[0] + 0x18");
_Static_assert(Q4K_DOT_CBUF_OFFSET_N       == 0x1C,
               "Q4K_DOT N must live at cbuf[0] + 0x1C");

_Static_assert(GQA_ATTN_CBUF_OFFSET_Q         == 0x00,
               "GQA_ATTN q must live at cbuf[0] + 0x00");
_Static_assert(GQA_ATTN_CBUF_OFFSET_K         == 0x08,
               "GQA_ATTN k must live at cbuf[0] + 0x08");
_Static_assert(GQA_ATTN_CBUF_OFFSET_V         == 0x10,
               "GQA_ATTN v must live at cbuf[0] + 0x10");
_Static_assert(GQA_ATTN_CBUF_OFFSET_OUT       == 0x18,
               "GQA_ATTN out must live at cbuf[0] + 0x18");
_Static_assert(GQA_ATTN_CBUF_OFFSET_N_HEAD_Q  == 0x20,
               "GQA_ATTN n_head_q must live at cbuf[0] + 0x20");
_Static_assert(GQA_ATTN_CBUF_OFFSET_N_HEAD_KV == 0x24,
               "GQA_ATTN n_head_kv must live at cbuf[0] + 0x24");
_Static_assert(GQA_ATTN_CBUF_OFFSET_HEAD_DIM  == 0x28,
               "GQA_ATTN head_dim must live at cbuf[0] + 0x28");
_Static_assert(GQA_ATTN_CBUF_OFFSET_SEQ_LEN   == 0x2C,
               "GQA_ATTN seq_len must live at cbuf[0] + 0x2C");

void test_get_metadata_returns_rmsnorm(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_RMSNORM);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_RMSNORM, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_get_metadata_misses_unknown_op_kind(void)
{
    /* 0xFFFFFFFF is well out of range of any registered op_kind. */
    TEST_ASSERT_NULL(operator_dispatch_get_metadata(0xFFFFFFFFu));
    /* Q4K_GEMM is declared in the op-kind enum but has no SASS kernel
     * shipped (multi-row prefill matmul — see MANIFEST.json
     * future_entries) and no dispatcher metadata, so a lookup must
     * miss. Replaces Q4K_DOT (which was the placeholder before
     * #714 §B.1 registered it). */
    TEST_ASSERT_NULL(operator_dispatch_get_metadata(SLM_GPU_OP_Q4K_GEMM));
}

void test_get_metadata_returns_rope(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_ROPE);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_ROPE, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_rmsnorm_build_cbuf_writes_documented_fields(void)
{
    /* 4 KB-zeroed cbuf page; the builder writes only into the
     * OPERATOR_CBUF0_BASE region. Any byte outside [0x160, 0x180)
     * must stay zero. */
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va     = 0x4000000000ull,
            .gamma_gpu_va = 0x4000001000ull,
            .out_gpu_va   = 0x4000002000ull,
            .n_rows       = 12,
            .n            = 1536,
            .eps_bits     = 0x358637BDu,    /* 1e-6f IEEE 754 bit pattern */
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t x_va, g_va, o_va;
    uint32_t n_field;
    uint32_t eps_field_bits;
    memcpy(&x_va,           p + RMSNORM_CBUF_OFFSET_X,     sizeof(x_va));
    memcpy(&g_va,           p + RMSNORM_CBUF_OFFSET_GAMMA, sizeof(g_va));
    memcpy(&o_va,           p + RMSNORM_CBUF_OFFSET_OUT,   sizeof(o_va));
    memcpy(&n_field,        p + RMSNORM_CBUF_OFFSET_N,     sizeof(n_field));
    memcpy(&eps_field_bits, p + RMSNORM_CBUF_OFFSET_EPS,   sizeof(eps_field_bits));

    TEST_ASSERT_EQUAL_UINT64(0x4000000000ull, x_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000001000ull, g_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000002000ull, o_va);
    TEST_ASSERT_EQUAL_UINT32(1536u, n_field);
    /* eps_bits is the IEEE 754 FP32 bit pattern for 1e-6f. */
    TEST_ASSERT_EQUAL_UINT32(0x358637BDu, eps_field_bits);

    /* Confirm no out-of-range writes. The full RMSNORM arg block
     * is [0x00, 0x20) within cbuf[0] (last field = eps at 0x1C +
     * 4 B = 0x20). Bytes before 0x160 and from 0x180 onward must
     * still be zero. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x20;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_rmsnorm_launch_shape_qwen_decode(void)
{
    /* Qwen2.5-1.5B per-token decode: 1 row, n=1536. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va = 0x1000, .gamma_gpu_va = 0x2000,
            .out_gpu_va = 0x3000,
            .n_rows = 1, .n = 1536, .eps_bits = 0x358637BDu,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.grid_y);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.grid_z);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.block_y);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.block_z);
}

void test_rmsnorm_launch_shape_prefill_chunk(void)
{
    /* 16-row prefill chunk. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va = 0x1000, .gamma_gpu_va = 0x2000,
            .out_gpu_va = 0x3000,
            .n_rows = 16, .n = 1536, .eps_bits = 0x358637BDu,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(16u,  shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
}

void test_rmsnorm_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va     = 0x1000, .gamma_gpu_va = 0x2000,
            .out_gpu_va   = 0x3000,
            .n_rows = 1, .n = 1536, .eps_bits = 0x358637BDu,
        },
    };

    /* NULL cbuf. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    /* NULL args. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    /* op_kind mismatch — the args claim Q4K_DOT but the registry's
     * builder is RMSNORM-only. The lookup goes via op_kind so this
     * actually hits the "no metadata for Q4K_DOT" path. */
    {
        struct operator_dispatch_args bad = base;
        bad.op_kind = SLM_GPU_OP_Q4K_DOT;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* Zero n / n_rows. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.rmsnorm.n = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.rmsnorm.n_rows = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* NULL GPU VAs. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.rmsnorm.x_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_rmsnorm_launch_shape_rejects_zero_rows(void)
{
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_RMSNORM,
        .u.rmsnorm = {
            .x_gpu_va = 0x1000, .gamma_gpu_va = 0x2000,
            .out_gpu_va = 0x3000,
            .n_rows = 0, .n = 1536, .eps_bits = 0x358637BDu,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_rope_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_ROPE,
        .u.rope = {
            .vec_gpu_va       = 0x4000010000ull,
            .positions_gpu_va = 0x4000020000ull,
            .cos_sin_gpu_va   = 0x4000030000ull,
            .batch            = 4u,
            .num_heads        = 12u,
            .head_dim         = 64u,
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t vec_va, pos_va, cs_va;
    uint32_t batch, heads, hdim;
    memcpy(&vec_va, p + ROPE_CBUF_OFFSET_VEC,       sizeof(vec_va));
    memcpy(&pos_va, p + ROPE_CBUF_OFFSET_POSITIONS, sizeof(pos_va));
    memcpy(&cs_va,  p + ROPE_CBUF_OFFSET_COS_SIN,   sizeof(cs_va));
    memcpy(&batch,  p + ROPE_CBUF_OFFSET_BATCH,     sizeof(batch));
    memcpy(&heads,  p + ROPE_CBUF_OFFSET_NUM_HEADS, sizeof(heads));
    memcpy(&hdim,   p + ROPE_CBUF_OFFSET_HEAD_DIM,  sizeof(hdim));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, vec_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, pos_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000030000ull, cs_va);
    TEST_ASSERT_EQUAL_UINT32(4u,  batch);
    TEST_ASSERT_EQUAL_UINT32(12u, heads);
    TEST_ASSERT_EQUAL_UINT32(64u, hdim);

    /* The full ROPE arg block is [0x00, 0x24) within cbuf[0] (last
     * field = head_dim at 0x20 + 4 B = 0x24). Bytes before 0x160
     * and from 0x184 onward must still be zero. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x24;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_rope_launch_shape_qwen_attention(void)
{
    /* Qwen2.5 attention head shape: 4 tokens × 12 heads × 64 dim.
     * Expected grid = (4*12, 1, 1), block = (64/2, 1, 1). */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_ROPE,
        .u.rope = {
            .vec_gpu_va = 0x1000, .positions_gpu_va = 0x2000,
            .cos_sin_gpu_va = 0x3000,
            .batch = 4u, .num_heads = 12u, .head_dim = 64u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(48u, shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(1u,  shape.grid_y);
    TEST_ASSERT_EQUAL_UINT32(1u,  shape.grid_z);
    TEST_ASSERT_EQUAL_UINT32(32u, shape.block_x);
    TEST_ASSERT_EQUAL_UINT32(1u,  shape.block_y);
    TEST_ASSERT_EQUAL_UINT32(1u,  shape.block_z);
    /* No shared memory, no SLM, no barriers — RoPE has no
     * cross-thread communication. */
    TEST_ASSERT_EQUAL_UINT32(0u, shape.smem_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, shape.slm_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u, shape.barrier_count);
}

void test_rope_launch_shape_decode(void)
{
    /* Single-token decode: 1 × 1 × 8. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_ROPE,
        .u.rope = {
            .vec_gpu_va = 0x1000, .positions_gpu_va = 0x2000,
            .cos_sin_gpu_va = 0x3000,
            .batch = 1u, .num_heads = 1u, .head_dim = 8u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(4u, shape.block_x);
}

void test_rope_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_ROPE,
        .u.rope = {
            .vec_gpu_va       = 0x1000,
            .positions_gpu_va = 0x2000,
            .cos_sin_gpu_va   = 0x3000,
            .batch = 1u, .num_heads = 1u, .head_dim = 8u,
        },
    };

    /* NULL cbuf / args. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    /* Zero batch / num_heads / head_dim. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.batch = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.num_heads = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.head_dim = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* Odd head_dim — pairs are (vec[2i], vec[2i+1]); an odd dim
     * would silently round down and skip the last element. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.head_dim = 7u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* NULL GPU VAs. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.vec_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.positions_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.rope.cos_sin_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_rope_launch_shape_rejects_odd_head_dim(void)
{
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_ROPE,
        .u.rope = {
            .vec_gpu_va = 0x1000, .positions_gpu_va = 0x2000,
            .cos_sin_gpu_va = 0x3000,
            .batch = 1u, .num_heads = 1u, .head_dim = 7u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_get_metadata_returns_embedding(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_EMBEDDING);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_EMBEDDING, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_embedding_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    /* Qwen2.5-1.5B: embedding_length=1536 → (1536/256)*144 = 864 B per row. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va     = 0x4000010000ull,
            .out_gpu_va       = 0x4000020000ull,
            .token_id         = 12345u,
            .embedding_length = 1536u,
            .table_row_bytes  = 864u,
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t table_va, out_va;
    uint32_t tid, embed_len, row_bytes;
    memcpy(&table_va,  p + EMBEDDING_CBUF_OFFSET_TABLE,     sizeof(table_va));
    memcpy(&out_va,    p + EMBEDDING_CBUF_OFFSET_OUT,       sizeof(out_va));
    memcpy(&tid,       p + EMBEDDING_CBUF_OFFSET_TOKEN_ID,  sizeof(tid));
    memcpy(&embed_len, p + EMBEDDING_CBUF_OFFSET_EMBED_LEN, sizeof(embed_len));
    memcpy(&row_bytes, p + EMBEDDING_CBUF_OFFSET_ROW_BYTES, sizeof(row_bytes));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, table_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, out_va);
    TEST_ASSERT_EQUAL_UINT32(12345u, tid);
    TEST_ASSERT_EQUAL_UINT32(1536u,  embed_len);
    TEST_ASSERT_EQUAL_UINT32(864u,   row_bytes);

    /* Full EMBEDDING arg block is [0x00, 0x1C) within cbuf[0] (last
     * field = table_row_bytes at 0x18 + 4 B = 0x1C). Bytes before
     * 0x160 and from 0x17C onward must still be zero. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x1C;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_embedding_launch_shape_qwen(void)
{
    /* Qwen2.5-1.5B: 1536-wide row → 6 super-blocks. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .token_id = 0u, .embedding_length = 1536u,
            .table_row_bytes = 864u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(6u,   shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.grid_y);
    TEST_ASSERT_EQUAL_UINT32(1u,   shape.grid_z);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
    TEST_ASSERT_EQUAL_UINT32(0u,   shape.smem_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u,   shape.slm_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u,   shape.barrier_count);
}

void test_embedding_launch_shape_smollm_padded(void)
{
    /* SmolLM2-135M: embedding_length=576 stored across 3 super-blocks
     * (768 dequantized capacity, only 576 written — padding tail). */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .token_id = 42u, .embedding_length = 576u,
            .table_row_bytes = 432u,    /* 3 * 144 */
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(3u,   shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
}

void test_embedding_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .token_id = 0u, .embedding_length = 1536u,
            .table_row_bytes = 864u,
        },
    };

    /* NULL cbuf / args. */
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    /* Zero embedding_length / table_row_bytes. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.embedding_length = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.table_row_bytes = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* table_row_bytes not a multiple of 144 — would silently truncate
     * the last partial super-block. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.table_row_bytes = 144u + 1u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* embedding_length exceeds dequantized capacity (n_blocks × 256).
     * 864 B → 6 blocks → 1536 max; passing 1537 should fail. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.embedding_length = 1537u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* NULL GPU VAs. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.table_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.embedding.out_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_embedding_launch_shape_rejects_misaligned_row(void)
{
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_EMBEDDING,
        .u.embedding = {
            .table_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .token_id = 0u, .embedding_length = 1536u,
            .table_row_bytes = 100u,    /* not a multiple of 144 */
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_get_metadata_returns_q4k_dequant(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_Q4K_DEQUANT);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_Q4K_DEQUANT, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_q4k_dequant_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DEQUANT,
        .u.q4k_dequant = {
            .blocks_gpu_va = 0x4000010000ull,
            .out_gpu_va    = 0x4000020000ull,
            .nb            = 6u,    /* Qwen2.5 1536-wide row */
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t blocks_va, out_va;
    uint32_t nb;
    memcpy(&blocks_va, p + Q4K_DEQUANT_CBUF_OFFSET_BLOCKS, sizeof(blocks_va));
    memcpy(&out_va,    p + Q4K_DEQUANT_CBUF_OFFSET_OUT,    sizeof(out_va));
    memcpy(&nb,        p + Q4K_DEQUANT_CBUF_OFFSET_NB,     sizeof(nb));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, blocks_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, out_va);
    TEST_ASSERT_EQUAL_UINT32(6u, nb);

    /* Q4K_DEQUANT arg block ends at 0x10 + 4 = 0x14. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x14;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_q4k_dequant_launch_shape(void)
{
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DEQUANT,
        .u.q4k_dequant = {
            .blocks_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .nb = 6u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(6u,   shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
    TEST_ASSERT_EQUAL_UINT32(0u,   shape.barrier_count);
}

void test_q4k_dequant_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_Q4K_DEQUANT,
        .u.q4k_dequant = {
            .blocks_gpu_va = 0x1000, .out_gpu_va = 0x2000,
            .nb = 6u,
        },
    };

    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dequant.nb = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dequant.blocks_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dequant.out_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_get_metadata_returns_swiglu(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_SWIGLU);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_SWIGLU, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_swiglu_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    /* Qwen2.5-1.5B per-token decode: intermediate=8960, n=8960. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_SWIGLU,
        .u.swiglu = {
            .gate_gpu_va = 0x4000010000ull,
            .up_gpu_va   = 0x4000020000ull,
            .out_gpu_va  = 0x4000030000ull,
            .n           = 8960u,
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t gate_va, up_va, out_va;
    uint32_t n;
    memcpy(&gate_va, p + SWIGLU_CBUF_OFFSET_GATE, sizeof(gate_va));
    memcpy(&up_va,   p + SWIGLU_CBUF_OFFSET_UP,   sizeof(up_va));
    memcpy(&out_va,  p + SWIGLU_CBUF_OFFSET_OUT,  sizeof(out_va));
    memcpy(&n,       p + SWIGLU_CBUF_OFFSET_N,    sizeof(n));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, gate_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, up_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000030000ull, out_va);
    TEST_ASSERT_EQUAL_UINT32(8960u, n);

    /* SWIGLU arg block ends at 0x18 + 4 = 0x1C. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x1C;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_swiglu_launch_shape_qwen_decode(void)
{
    /* Qwen2.5-1.5B per-token: n=8960 → ceil(8960/256) = 35 blocks. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_SWIGLU,
        .u.swiglu = {
            .gate_gpu_va = 0x1000, .up_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .n = 8960u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(35u,  shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(256u, shape.block_x);
}

void test_swiglu_launch_shape_partial_last_block(void)
{
    /* n=257 → ceil(257/256) = 2 blocks; last block 1-wide. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_SWIGLU,
        .u.swiglu = {
            .gate_gpu_va = 0x1000, .up_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .n = 257u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(2u, shape.grid_x);
}

void test_swiglu_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_SWIGLU,
        .u.swiglu = {
            .gate_gpu_va = 0x1000, .up_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .n = 256u,
        },
    };

    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    {
        struct operator_dispatch_args bad = base;
        bad.u.swiglu.n = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.swiglu.gate_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.swiglu.up_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.swiglu.out_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_get_metadata_returns_q4k_dot(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_Q4K_DOT);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_Q4K_DOT, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_q4k_dot_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    /* Qwen2.5-1.5B Q/O projection: K=1536, N=1536. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DOT,
        .u.q4k_dot = {
            .x_gpu_va       = 0x4000010000ull,
            .weights_gpu_va = 0x4000020000ull,
            .out_gpu_va     = 0x4000030000ull,
            .k              = 1536u,
            .n              = 1536u,
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t x_va, w_va, o_va;
    uint32_t k, n;
    memcpy(&x_va, p + Q4K_DOT_CBUF_OFFSET_X,       sizeof(x_va));
    memcpy(&w_va, p + Q4K_DOT_CBUF_OFFSET_WEIGHTS, sizeof(w_va));
    memcpy(&o_va, p + Q4K_DOT_CBUF_OFFSET_OUT,     sizeof(o_va));
    memcpy(&k,    p + Q4K_DOT_CBUF_OFFSET_K,       sizeof(k));
    memcpy(&n,    p + Q4K_DOT_CBUF_OFFSET_N,       sizeof(n));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, x_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, w_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000030000ull, o_va);
    TEST_ASSERT_EQUAL_UINT32(1536u, k);
    TEST_ASSERT_EQUAL_UINT32(1536u, n);

    /* Q4K_DOT arg block ends at 0x1C + 4 = 0x20. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x20;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_q4k_dot_launch_shape_qwen_qo(void)
{
    /* Qwen2.5-1.5B Q/O projection: K=1536, N=1536 → grid=(1536,1,1). */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DOT,
        .u.q4k_dot = {
            .x_gpu_va = 0x1000, .weights_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .k = 1536u, .n = 1536u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(1536u, shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(256u,  shape.block_x);
    TEST_ASSERT_EQUAL_UINT32(1024u, shape.smem_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(0u,    shape.slm_size_bytes);
    /* __syncthreads() in the tree reduction — barrier slot required. */
    TEST_ASSERT_EQUAL_UINT32(1u,    shape.barrier_count);
}

void test_q4k_dot_launch_shape_lm_head(void)
{
    /* Qwen2.5 LM head: K=1536, N=151936 (vocab). */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_Q4K_DOT,
        .u.q4k_dot = {
            .x_gpu_va = 0x1000, .weights_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .k = 1536u, .n = 151936u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(151936u, shape.grid_x);
}

void test_q4k_dot_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_Q4K_DOT,
        .u.q4k_dot = {
            .x_gpu_va = 0x1000, .weights_gpu_va = 0x2000,
            .out_gpu_va = 0x3000, .k = 1536u, .n = 1536u,
        },
    };

    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    /* Zero K / N. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.k = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.n = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* K not a multiple of 256 — would silently drop a partial
     * super-block. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.k = 257u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* NULL VAs. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.x_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.weights_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.q4k_dot.out_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

void test_get_metadata_returns_gqa_attn(void)
{
    const struct operator_dispatch_metadata *m =
        operator_dispatch_get_metadata(SLM_GPU_OP_GQA_ATTN);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SLM_GPU_OP_GQA_ATTN, m->op_kind);
    TEST_ASSERT_NOT_NULL(m->build_cbuf);
    TEST_ASSERT_NOT_NULL(m->launch_shape);
}

void test_gqa_attn_build_cbuf_writes_documented_fields(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));

    /* Qwen2.5-1.5B GQA: 12 Q heads × 2 KV heads × 128 head_dim,
     * seq_len=128. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_GQA_ATTN,
        .u.gqa_attn = {
            .q_gpu_va    = 0x4000010000ull,
            .k_gpu_va    = 0x4000020000ull,
            .v_gpu_va    = 0x4000030000ull,
            .out_gpu_va  = 0x4000040000ull,
            .n_head_q    = 12u,
            .n_head_kv   = 2u,
            .head_dim    = 128u,
            .seq_len     = 128u,
        },
    };

    int rc = operator_dispatch_build_cbuf(cbuf, &args);
    TEST_ASSERT_EQUAL_INT(0, rc);

    uint8_t *p = cbuf + OPERATOR_CBUF0_BASE;
    uint64_t q_va, k_va, v_va, o_va;
    uint32_t nq, nkv, hd, sl;
    memcpy(&q_va, p + GQA_ATTN_CBUF_OFFSET_Q,         sizeof(q_va));
    memcpy(&k_va, p + GQA_ATTN_CBUF_OFFSET_K,         sizeof(k_va));
    memcpy(&v_va, p + GQA_ATTN_CBUF_OFFSET_V,         sizeof(v_va));
    memcpy(&o_va, p + GQA_ATTN_CBUF_OFFSET_OUT,       sizeof(o_va));
    memcpy(&nq,   p + GQA_ATTN_CBUF_OFFSET_N_HEAD_Q,  sizeof(nq));
    memcpy(&nkv,  p + GQA_ATTN_CBUF_OFFSET_N_HEAD_KV, sizeof(nkv));
    memcpy(&hd,   p + GQA_ATTN_CBUF_OFFSET_HEAD_DIM,  sizeof(hd));
    memcpy(&sl,   p + GQA_ATTN_CBUF_OFFSET_SEQ_LEN,   sizeof(sl));

    TEST_ASSERT_EQUAL_UINT64(0x4000010000ull, q_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000020000ull, k_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000030000ull, v_va);
    TEST_ASSERT_EQUAL_UINT64(0x4000040000ull, o_va);
    TEST_ASSERT_EQUAL_UINT32(12u, nq);
    TEST_ASSERT_EQUAL_UINT32(2u,  nkv);
    TEST_ASSERT_EQUAL_UINT32(128u, hd);
    TEST_ASSERT_EQUAL_UINT32(128u, sl);

    /* GQA_ATTN arg block ends at 0x2C + 4 = 0x30. */
    for (size_t i = 0; i < OPERATOR_CBUF0_BASE; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
    for (size_t i = OPERATOR_CBUF0_BASE + 0x30;
         i < sizeof(cbuf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, cbuf[i]);
    }
}

void test_gqa_attn_launch_shape_qwen(void)
{
    /* Qwen2.5-1.5B: grid_x = n_head_q = 12, block_x = 128. */
    struct operator_dispatch_args args = {
        .op_kind = SLM_GPU_OP_GQA_ATTN,
        .u.gqa_attn = {
            .q_gpu_va = 0x1000, .k_gpu_va = 0x2000,
            .v_gpu_va = 0x3000, .out_gpu_va = 0x4000,
            .n_head_q = 12u, .n_head_kv = 2u,
            .head_dim = 128u, .seq_len = 128u,
        },
    };
    struct operator_launch_shape shape;
    int rc = operator_dispatch_launch_shape(&args, &shape);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(12u,  shape.grid_x);
    TEST_ASSERT_EQUAL_UINT32(128u, shape.block_x);
    /* Static smem from kernel: q_cache + logits + reduce_buf + 2 floats =
     * 17928 raw, rounded up to 18176 (next 256-B boundary) for GA10B's
     * QMD SHARED_MEMORY_SIZE granularity. */
    TEST_ASSERT_EQUAL_UINT32(18176u, shape.smem_size_bytes);
    TEST_ASSERT_EQUAL_UINT32(1u,     shape.barrier_count);
}

void test_gqa_attn_build_cbuf_rejects_malformed(void)
{
    uint8_t cbuf[4096];
    memset(cbuf, 0, sizeof(cbuf));
    struct operator_dispatch_args base = {
        .op_kind = SLM_GPU_OP_GQA_ATTN,
        .u.gqa_attn = {
            .q_gpu_va = 0x1000, .k_gpu_va = 0x2000,
            .v_gpu_va = 0x3000, .out_gpu_va = 0x4000,
            .n_head_q = 12u, .n_head_kv = 2u,
            .head_dim = 128u, .seq_len = 128u,
        },
    };

    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(NULL, &base));
    TEST_ASSERT_EQUAL_INT(-1,
        operator_dispatch_build_cbuf(cbuf, NULL));

    /* Zero scalars. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.n_head_q = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.n_head_kv = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.head_dim = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.seq_len = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* n_head_q not divisible by n_head_kv — would map some Q heads
     * to the wrong KV head. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.n_head_q = 13u;
        bad.u.gqa_attn.n_head_kv = 2u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* head_dim or seq_len exceeding the static smem caps. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.head_dim = GQA_ATTN_MAX_HEAD_DIM + 1u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.seq_len = GQA_ATTN_MAX_SEQ_LEN + 1u;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }

    /* NULL VAs. */
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.q_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.k_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.v_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
    {
        struct operator_dispatch_args bad = base;
        bad.u.gqa_attn.out_gpu_va = 0;
        TEST_ASSERT_EQUAL_INT(-1,
            operator_dispatch_build_cbuf(cbuf, &bad));
    }
}

int test_suite_operator_dispatch(void)
{
    UnityBegin("test_operator_dispatch.c");
    RUN_TEST(test_get_metadata_returns_rmsnorm);
    RUN_TEST(test_get_metadata_misses_unknown_op_kind);
    RUN_TEST(test_get_metadata_returns_rope);
    RUN_TEST(test_get_metadata_returns_embedding);
    RUN_TEST(test_get_metadata_returns_q4k_dequant);
    RUN_TEST(test_get_metadata_returns_swiglu);
    RUN_TEST(test_get_metadata_returns_q4k_dot);
    RUN_TEST(test_get_metadata_returns_gqa_attn);
    RUN_TEST(test_rmsnorm_build_cbuf_writes_documented_fields);
    RUN_TEST(test_rmsnorm_launch_shape_qwen_decode);
    RUN_TEST(test_rmsnorm_launch_shape_prefill_chunk);
    RUN_TEST(test_rmsnorm_build_cbuf_rejects_malformed);
    RUN_TEST(test_rmsnorm_launch_shape_rejects_zero_rows);
    RUN_TEST(test_rope_build_cbuf_writes_documented_fields);
    RUN_TEST(test_rope_launch_shape_qwen_attention);
    RUN_TEST(test_rope_launch_shape_decode);
    RUN_TEST(test_rope_build_cbuf_rejects_malformed);
    RUN_TEST(test_rope_launch_shape_rejects_odd_head_dim);
    RUN_TEST(test_embedding_build_cbuf_writes_documented_fields);
    RUN_TEST(test_embedding_launch_shape_qwen);
    RUN_TEST(test_embedding_launch_shape_smollm_padded);
    RUN_TEST(test_embedding_build_cbuf_rejects_malformed);
    RUN_TEST(test_embedding_launch_shape_rejects_misaligned_row);
    RUN_TEST(test_q4k_dequant_build_cbuf_writes_documented_fields);
    RUN_TEST(test_q4k_dequant_launch_shape);
    RUN_TEST(test_q4k_dequant_build_cbuf_rejects_malformed);
    RUN_TEST(test_swiglu_build_cbuf_writes_documented_fields);
    RUN_TEST(test_swiglu_launch_shape_qwen_decode);
    RUN_TEST(test_swiglu_launch_shape_partial_last_block);
    RUN_TEST(test_swiglu_build_cbuf_rejects_malformed);
    RUN_TEST(test_q4k_dot_build_cbuf_writes_documented_fields);
    RUN_TEST(test_q4k_dot_launch_shape_qwen_qo);
    RUN_TEST(test_q4k_dot_launch_shape_lm_head);
    RUN_TEST(test_q4k_dot_build_cbuf_rejects_malformed);
    RUN_TEST(test_gqa_attn_build_cbuf_writes_documented_fields);
    RUN_TEST(test_gqa_attn_launch_shape_qwen);
    RUN_TEST(test_gqa_attn_build_cbuf_rejects_malformed);
    return UnityEnd();
}
