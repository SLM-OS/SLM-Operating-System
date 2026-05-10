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
    /* Q4K_DOT exists in the operator library but isn't yet registered
     * in the dispatcher (lands in #714). */
    TEST_ASSERT_NULL(operator_dispatch_get_metadata(SLM_GPU_OP_Q4K_DOT));
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

int test_suite_operator_dispatch(void)
{
    UnityBegin("test_operator_dispatch.c");
    RUN_TEST(test_get_metadata_returns_rmsnorm);
    RUN_TEST(test_get_metadata_misses_unknown_op_kind);
    RUN_TEST(test_get_metadata_returns_rope);
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
    return UnityEnd();
}
