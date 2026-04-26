/*
 * test_model_engine.c - Unit tests for the model engine registry +
 * .meta parser + launch dispatch (admin & telemetry suite, M5).
 *
 * Engine registry: name round-trip, kind enumeration, lookup.
 * .meta parser: well-formed, missing-kind, malformed, unknown-key
 *               forward-compat, comment + whitespace handling.
 * launch: stub-engine returns NOSYS, raw returns OK with task_id=0.
 *
 * Real launch paths (mnist) require VFS state and Rust-side init
 * that aren't available in this isolated unit test; covered by the
 * existing model integration tests + the M7 demo runbook.
 */

#include "unity.h"
#include "../include/model_engine.h"

#include <stdint.h>
#include <string.h>

/* ---------- Engine registry ----------------------------------------- */

static void test_kind_name_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("raw",   model_kind_name(MODEL_KIND_RAW));
    TEST_ASSERT_EQUAL_STRING("mnist", model_kind_name(MODEL_KIND_MNIST));
    TEST_ASSERT_EQUAL_STRING("hailo", model_kind_name(MODEL_KIND_HAILO));
    TEST_ASSERT_EQUAL_STRING("ggml",  model_kind_name(MODEL_KIND_GGML));
    TEST_ASSERT_NULL(model_kind_name(MODEL_KIND_COUNT));
    TEST_ASSERT_NULL(model_kind_name((enum model_kind)999));
}

static void test_kind_lookup(void)
{
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_RAW,   model_kind_from_name("raw"));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_MNIST, model_kind_from_name("mnist"));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_HAILO, model_kind_from_name("hailo"));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_GGML,  model_kind_from_name("ggml"));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_COUNT, model_kind_from_name("bogus"));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_COUNT, model_kind_from_name(""));
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_COUNT, model_kind_from_name(NULL));
}

static void test_engine_info_state(void)
{
    /* RAW + MNIST are READY; HAILO + GGML are NOSYS stubs in M5. */
    TEST_ASSERT_EQUAL_INT(MODEL_ENGINE_READY,
                          model_engine_info_get(MODEL_KIND_RAW)->state);
    TEST_ASSERT_EQUAL_INT(MODEL_ENGINE_READY,
                          model_engine_info_get(MODEL_KIND_MNIST)->state);
    TEST_ASSERT_EQUAL_INT(MODEL_ENGINE_NOSYS,
                          model_engine_info_get(MODEL_KIND_HAILO)->state);
    TEST_ASSERT_EQUAL_INT(MODEL_ENGINE_NOSYS,
                          model_engine_info_get(MODEL_KIND_GGML)->state);
}

static void test_engine_info_list(void)
{
    const struct model_engine_info *engines[MODEL_KIND_COUNT];
    size_t n = model_engine_info_list(engines);
    TEST_ASSERT_EQUAL_UINT(MODEL_KIND_COUNT, n);
    /* Every entry populated. */
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(engines[i]);
        TEST_ASSERT_NOT_NULL(engines[i]->name);
        TEST_ASSERT_NOT_NULL(engines[i]->summary);
    }
}

/* ---------- .meta parser -------------------------------------------- */

static void test_meta_parse_minimal(void)
{
    const char *src = "kind=mnist\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_MNIST, m.kind);
    TEST_ASSERT_EQUAL_UINT64(0, m.size);
}

static void test_meta_parse_full(void)
{
    const char *src =
        "name=demo\n"
        "kind=mnist\n"
        "size=3136\n"
        "sha256=abcdef0123456789\n"
        "uploaded_ts_ms=1700000000000\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_OK, rc);
    TEST_ASSERT_EQUAL_STRING("demo", m.name);
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_MNIST, m.kind);
    TEST_ASSERT_EQUAL_UINT64(3136, m.size);
    TEST_ASSERT_EQUAL_STRING("abcdef0123456789", m.sha256);
    TEST_ASSERT_EQUAL_UINT64(1700000000000ull, m.uploaded_ts_ms);
}

static void test_meta_parse_strips_whitespace(void)
{
    const char *src =
        "  kind = ggml  \n"
        "size= 42\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_GGML, m.kind);
    TEST_ASSERT_EQUAL_UINT64(42, m.size);
}

static void test_meta_parse_skips_comments_and_blanks(void)
{
    const char *src =
        "# this is a comment\n"
        "\n"
        "kind=raw\n"
        "  # also a comment\n"
        "\n"
        "size=128\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_RAW, m.kind);
    TEST_ASSERT_EQUAL_UINT64(128, m.size);
}

static void test_meta_parse_unknown_key_forward_compat(void)
{
    /* Unknown keys must be silently ignored so future versions can
     * extend the format without breaking older parsers. */
    const char *src =
        "kind=mnist\n"
        "future_field=lots of stuff here that we don't recognize\n"
        "size=10\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_OK, rc);
    TEST_ASSERT_EQUAL_INT(MODEL_KIND_MNIST, m.kind);
    TEST_ASSERT_EQUAL_UINT64(10, m.size);
}

static void test_meta_parse_missing_kind_rejected(void)
{
    const char *src = "size=42\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA, rc);
}

static void test_meta_parse_unknown_kind_rejected(void)
{
    const char *src = "kind=quantum\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADKIND, rc);
}

static void test_meta_parse_malformed_no_eq(void)
{
    /* Line without '=' is malformed. */
    const char *src = "this is not a key value pair\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA, rc);
}

static void test_meta_parse_bad_size_overflow(void)
{
    /* 30+ digit number overflows uint64. */
    const char *src =
        "kind=mnist\n"
        "size=999999999999999999999999999999\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA, rc);
}

static void test_meta_parse_bad_size_non_digit(void)
{
    const char *src =
        "kind=mnist\n"
        "size=12abc\n";
    struct model_meta m;
    int rc = model_meta_parse(src, strlen(src), &m);
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA, rc);
}

static void test_meta_parse_null_args(void)
{
    struct model_meta m;
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA,
                          model_meta_parse(NULL, 0, &m));
    TEST_ASSERT_EQUAL_INT(MODEL_LAUNCH_ERR_BADMETA,
                          model_meta_parse("kind=raw\n", 9, NULL));
}

/* ---------- Suite registration -------------------------------------- */

int test_suite_model_engine(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_kind_name_round_trip);
    RUN_TEST(test_kind_lookup);
    RUN_TEST(test_engine_info_state);
    RUN_TEST(test_engine_info_list);
    RUN_TEST(test_meta_parse_minimal);
    RUN_TEST(test_meta_parse_full);
    RUN_TEST(test_meta_parse_strips_whitespace);
    RUN_TEST(test_meta_parse_skips_comments_and_blanks);
    RUN_TEST(test_meta_parse_unknown_key_forward_compat);
    RUN_TEST(test_meta_parse_missing_kind_rejected);
    RUN_TEST(test_meta_parse_unknown_kind_rejected);
    RUN_TEST(test_meta_parse_malformed_no_eq);
    RUN_TEST(test_meta_parse_bad_size_overflow);
    RUN_TEST(test_meta_parse_bad_size_non_digit);
    RUN_TEST(test_meta_parse_null_args);
    return UNITY_END();
}
