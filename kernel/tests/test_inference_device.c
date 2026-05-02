/*
 * test_inference_device.c — Unit tests for inference_device.h.
 *
 * Coverage:
 *   - A fake backend round-trips through the dispatcher correctly,
 *     proving the vtable forwarders behave regardless of which
 *     backend is registered.
 *   - NULL dispatch returns INF_ERR_INVAL rather than crashing.
 *   - In AI_SCHED=ON builds, the "cpu-mlp" device is registered and
 *     visible via `inference_device_find`.
 *
 * No floating-point operations appear in this file — it lives in
 * the main kernel build which is compiled with `-mgeneral-regs-only`.
 * Tensor payload buffers are uint8_t/uint32_t; the backend under
 * test (the "fake-test" one below) treats them as opaque bytes.
 * Running the real NEON MLP end-to-end happens via the scheduler
 * path (`ai_mlp_assign_cpu`), which is exercised by the existing
 * AI scheduler tests in the ai_sched library.
 */

#include "unity.h"
#include "../include/inference_device.h"
#include "../include/uart.h"
#include "test_harness.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Fake backend — exercises the dispatcher independently of any real one.     */
/* -------------------------------------------------------------------------- */

static int fake_init_calls;
static int fake_run_calls;
static int fake_init(struct inference_device *dev)
{
    (void)dev;
    fake_init_calls++;
    return INF_OK;
}
static int fake_load_model(struct inference_device *dev, const void *b,
                           size_t n, inference_model_handle_t *out)
{
    (void)dev; (void)b; (void)n;
    *out = 7;                   /* arbitrary non-builtin handle */
    return INF_OK;
}
static int fake_run(struct inference_device *dev, inference_model_handle_t h,
                    const inference_tensor_t *in, inference_tensor_t *out)
{
    (void)dev;
    if (h != 7) return INF_ERR_INVAL;
    if (in->dtype != INF_DTYPE_INT32 || out->dtype != INF_DTYPE_INT32)
        return INF_ERR_BAD_TENSOR;
    /* Copy input to output, add 1 to every value — a deterministic
     * signature the test can check without FP. */
    uint32_t n = in->n_elems;
    if (out->n_elems < n) return INF_ERR_BAD_TENSOR;
    const uint32_t *ix = (const uint32_t *)in->data;
    uint32_t *ox = (uint32_t *)out->data;
    for (uint32_t i = 0; i < n; i++) ox[i] = ix[i] + 1u;
    fake_run_calls++;
    return INF_OK;
}
static int fake_free_model(struct inference_device *dev,
                           inference_model_handle_t h)
{
    (void)dev; (void)h;
    return INF_OK;
}
static const struct inference_device_ops fake_ops = {
    .name       = "fake-test",
    .caps       = INF_CAP_FP32,
    .init       = fake_init,
    .load_model = fake_load_model,
    .run        = fake_run,
    .free_model = fake_free_model,
};
static struct inference_device fake_dev = { .ops = &fake_ops };

/* Reset fake-backend counters to a known state. Called at the top
 * of every test that reads them so the suite is order-independent
 * — a new test that happens to exercise the fake backend ahead of
 * an existing one must not perturb its assertions. */
static void fake_reset_counters(void)
{
    fake_init_calls = 0;
    fake_run_calls  = 0;
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                       */
/* -------------------------------------------------------------------------- */

static void test_fake_backend_register(void)
{
    fake_reset_counters();
    int rc = inference_device_register(&fake_dev);
    TEST_ASSERT_EQUAL_INT(INF_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, fake_init_calls);

    struct inference_device *found = inference_device_find("fake-test");
    TEST_ASSERT_EQUAL_PTR(&fake_dev, found);
}

static void test_fake_backend_run(void)
{
    fake_reset_counters();
    inference_model_handle_t h = INF_INVALID_HANDLE;
    int rc = inference_load_model(&fake_dev, NULL, 0, &h);
    TEST_ASSERT_EQUAL_INT(INF_OK, rc);
    TEST_ASSERT_EQUAL_INT(7, h);

    uint32_t input[4]  = { 10, 20, 30, 40 };
    uint32_t output[4] = { 0, 0, 0, 0 };
    inference_tensor_t in  = { .data = input,  .n_elems = 4,
                               .dtype = INF_DTYPE_INT32, .rank = 1,
                               .shape = {4, 0, 0, 0} };
    inference_tensor_t out = { .data = output, .n_elems = 4,
                               .dtype = INF_DTYPE_INT32, .rank = 1,
                               .shape = {4, 0, 0, 0} };

    rc = inference_run(&fake_dev, h, &in, &out);
    TEST_ASSERT_EQUAL_INT(INF_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, fake_run_calls);
    TEST_ASSERT_EQUAL_UINT32(11, output[0]);
    TEST_ASSERT_EQUAL_UINT32(41, output[3]);

    TEST_ASSERT_EQUAL_INT(INF_OK, inference_free_model(&fake_dev, h));
}

static void test_null_dev_rejected(void)
{
    uint32_t buf[1] = {0};
    inference_tensor_t t = { .data = buf, .n_elems = 1,
                             .dtype = INF_DTYPE_INT32, .rank = 1,
                             .shape = {1, 0, 0, 0} };
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
                          inference_run(NULL, 0, &t, &t));
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
                          inference_load_model(NULL, NULL, 0, NULL));
}

static void test_find_nonexistent_returns_null(void)
{
    TEST_ASSERT_NULL(inference_device_find("no-such-backend"));
    TEST_ASSERT_NULL(inference_device_find(NULL));
}

/* `inference_device_find` must compare the FULL strings; a prefix
 * lookup must not false-match a longer registered name. Pins the
 * strcmp-replacement of the previous hand-rolled char-by-char
 * compare (PR #598 / REVIEW_SUGGESTIONS.md). */
static void test_find_prefix_does_not_match(void)
{
    /* `fake-test` is registered above. Querying just `fake` (a
     * proper prefix) must NOT match. */
    TEST_ASSERT_NULL(inference_device_find("fake"));

    /* Same in the other direction — querying a SUPER-string of a
     * registered name must also NOT match. */
    TEST_ASSERT_NULL(inference_device_find("fake-test-extra"));
}

/* Empty SEARCH string returns NULL because no registered device
 * has a zero-length name. `inference_device_register` rejects a
 * NULL `dev->ops->name` pointer but doesn't actively forbid a
 * zero-length name string; in practice every registrant uses a
 * non-empty literal, so an empty search never hits anything. */
static void test_find_empty_string_returns_null(void)
{
    TEST_ASSERT_NULL(inference_device_find(""));
}

static void test_default_device_set(void)
{
    /* fake_dev was registered earlier in the suite; the default is
     * either fake_dev (if nothing was registered before it) or
     * whatever preceded it. Either way, setting default to fake_dev
     * and back round-trips cleanly. */
    struct inference_device *prev = inference_device_default();
    TEST_ASSERT_NOT_NULL(prev);

    int rc = inference_device_set_default(&fake_dev);
    TEST_ASSERT_EQUAL_INT(INF_OK, rc);
    TEST_ASSERT_EQUAL_PTR(&fake_dev, inference_device_default());

    rc = inference_device_set_default(prev);
    TEST_ASSERT_EQUAL_INT(INF_OK, rc);
    TEST_ASSERT_EQUAL_PTR(prev, inference_device_default());
}

static void test_set_default_unknown_device_rejected(void)
{
    /* A device struct that was never registered must not become
     * default — prevents a caller from accidentally defaulting
     * to an un-init'd backend. */
    static struct inference_device orphan = { .ops = &fake_ops };
    int rc = inference_device_set_default(&orphan);
    TEST_ASSERT_EQUAL_INT(INF_ERR_NODEV, rc);
}

#if defined(CONFIG_AI_SCHEDULER)
static void test_cpu_mlp_registered(void)
{
    struct inference_device *dev = inference_device_find("cpu-mlp");
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_TRUE(dev->initialised);
    TEST_ASSERT_TRUE(dev->ops->caps & INF_CAP_FP32);
}
#endif

int test_suite_inference_device(void)
{
    UnityBegin("Inference Device Abstraction");

    RUN_TEST(test_fake_backend_register);
    RUN_TEST(test_fake_backend_run);
    RUN_TEST(test_null_dev_rejected);
    RUN_TEST(test_find_nonexistent_returns_null);
    RUN_TEST(test_find_prefix_does_not_match);
    RUN_TEST(test_find_empty_string_returns_null);
    RUN_TEST(test_default_device_set);
    RUN_TEST(test_set_default_unknown_device_rejected);
#if defined(CONFIG_AI_SCHEDULER)
    RUN_TEST(test_cpu_mlp_registered);
#endif

    return UnityEnd();
}
