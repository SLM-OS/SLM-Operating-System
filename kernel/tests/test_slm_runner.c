/*
 * SLM Runner Component Tests (Phase SLM, M8.1)
 *
 * Exercises the slm-runner component's three core contracts without
 * spinning up a task or waiting on the entry function's idle timeout:
 *
 *   1. setup_session refuses to open when no SLM is loaded and logs
 *      a friendly hint pointing the operator at `slm load`.
 *   2. setup_session succeeds once a synthetic GGUF is registered
 *      via the M1.4 test fixture (rust_slm_test_build_qwen_fixture).
 *   3. handle_prompt with an empty UTF-8 prompt drives the M5.2
 *      decoder through a no-token completion and publishes /slm/done
 *      so subscribers observe per-prompt completion. tokens_emitted
 *      stays at 0 because greedy sampling on zero logits returns
 *      token id 0 every step and the decoder stops at EOS without
 *      invoking the callback.
 *
 * Real text generation (non-empty prompts, multi-token streaming) is
 * exercised end-to-end in the integration deploy path documented in
 * docs/specs/slm-integration.md once M5.3 plumbs real weights.
 */

#include "unity.h"
#include "../include/slm_ffi.h"
#include "../include/string.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Externals exposed by kernel/src/slm_runner.c
 * ============================================================================ */

extern int slm_runner_setup_session(void);
extern int slm_runner_setup_session_with_ctx(uint32_t max_ctx);
extern int slm_runner_handle_prompt(uint32_t session_id,
                                    const char *prompt,
                                    size_t prompt_len,
                                    uint32_t *tokens_emitted_out);

/* Message router (see kernel/src/component_runtime.c for the kernel-side
 * extern declarations). */
extern void msg_router_init(void);
extern int  msg_router_subscribe(const char *topic_name, int component_idx);
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);

/* ============================================================================
 * Fixture
 * ============================================================================ */

/* Match TEST_FIXTURE_CAP in test_slm_load.c — a Qwen2-shaped GGUF
 * with up to 128 vocab entries fits in 4 KB. */
#define RUNNER_FIXTURE_CAP    (4u * 1024u)
static uint8_t runner_fixture_buf[RUNNER_FIXTURE_CAP];

static void runner_load_fixture(int *out_idx)
{
    size_t fixture_size = 0;
    int rc = rust_slm_test_build_qwen_fixture(
        /* vocab_size = */ 8u,
        runner_fixture_buf, sizeof(runner_fixture_buf),
        &fixture_size);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(fixture_size > 0 && fixture_size <= sizeof(runner_fixture_buf));

    int idx = rust_slm_load((const uint8_t *)"runner-fixture",
                            runner_fixture_buf, fixture_size);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    *out_idx = idx;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/*
 * setup_session must return -1 when the registry is empty. The runner
 * relies on this contract to short-circuit before opening a session
 * over a phantom slot 0; the test stands in for "boot with no `slm
 * load`".
 */
static void test_slm_runner_setup_fails_without_model(void)
{
    rust_slm_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    /* Use the small-ctx variant so the count-gate short-circuits
     * before any heap allocation can fault the test harness. */
    int rc = slm_runner_setup_session_with_ctx(4u);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}

/*
 * Once a synthetic GGUF is loaded, setup_session must open a real
 * session and return its id (>= 0). The runner's production defaults
 * (max_ctx=4096) allocate a ~117 MB KV cache for the Qwen2.5-1.5B
 * fixture, which overflows the 1 MB Rust heap the test harness gives
 * us. The small-ctx variant keeps the allocation under a few KB while
 * still exercising the same setup path (count-gate +
 * rust_slm_session_open).
 */
static void test_slm_runner_setup_succeeds_after_load(void)
{
    rust_slm_test_reset();
    int model_idx = -1;
    runner_load_fixture(&model_idx);

    int sess = slm_runner_setup_session_with_ctx(4u);
    TEST_ASSERT_GREATER_OR_EQUAL(0, sess);

    /* Tidy up so the session pool isn't full for later tests. */
    int32_t close_rc = rust_slm_session_close((uint32_t)sess);
    TEST_ASSERT_EQUAL_INT(0, close_rc);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)model_idx));
}

/*
 * handle_prompt with an empty prompt must:
 *   - return 0 (the M5.2 decoder accepts empty prompts as a state-
 *     machine drill),
 *   - not emit any /slm/token messages (the synthetic vocab leaves
 *     greedy sampling at token id 0 = EOS, no callback invocation),
 *   - publish /slm/done so subscribers see per-prompt completion.
 *
 * The test subscribes a fake component (slot 1, picked to avoid
 * colliding with the real shell's component 0) to /slm/done and
 * asserts the message arrives with the expected payload prefix.
 */
static void test_slm_runner_publishes_done_after_empty_prompt(void)
{
    rust_slm_test_reset();
    msg_router_init();

    /* Subscribe a fake component to /slm/done so we can observe the
     * publish. Component idx 1 is unused by the test harness. */
    const int fake_comp = 1;
    msg_router_unsubscribe_all(fake_comp);
    int sub_rc = msg_router_subscribe("/slm/done", fake_comp);
    TEST_ASSERT_EQUAL_INT(0, sub_rc);

    int model_idx = -1;
    runner_load_fixture(&model_idx);

    int sess = slm_runner_setup_session_with_ctx(4u);
    TEST_ASSERT_GREATER_OR_EQUAL(0, sess);

    /* Run the empty prompt through the helper. */
    uint32_t tokens_emitted = 0xDEADBEEFu;  /* sentinel — must be overwritten */
    int rc = slm_runner_handle_prompt((uint32_t)sess,
                                      /* prompt = */ "",
                                      /* prompt_len = */ 0,
                                      &tokens_emitted);

    /* M5.2 contract: empty prompt → rc=0 → no token callback. */
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, tokens_emitted);

    /* Drain /slm/done. The router publishes asynchronously and waits
     * for ack; handle_prompt's publish path returns when the receive
     * mailbox is filled (one-shot subscriber). */
    char topic_buf[16];
    const char *data = msg_router_receive(fake_comp, topic_buf);
    TEST_ASSERT_NOT_NULL(data);
    /* Topic name comparison — the router copies up to TOPIC_NAME_LEN
     * bytes and NUL-pads the rest. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(topic_buf, "/slm/done", 9));
    /* Done message format: "rc=<n> tokens_out=<n> prompts=<n>". The
     * counters live behind the prefix; assert the prefix and the
     * "tokens_out=0" substring rather than the entire string so the
     * test stays robust to future telemetry additions. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(data, "rc=0", 4));

    /* Find tokens_out=0 in the payload. memcmp can't search; do a
     * simple scan over the (NUL-terminated, < 60 byte) message. */
    int found_zero_tokens = 0;
    for (size_t i = 0; data[i] != '\0' && i < 60; i++) {
        if (i + 13 <= 60 &&
            memcmp(&data[i], "tokens_out=0", 12) == 0 &&
            (data[i + 12] == ' ' || data[i + 12] == '\0')) {
            found_zero_tokens = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_zero_tokens);

    msg_router_ack(fake_comp);

    /* Cleanup. */
    msg_router_unsubscribe_all(fake_comp);
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_close((uint32_t)sess));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)model_idx));
}

/* ============================================================================
 * Suite Runner
 * ============================================================================ */

int test_suite_slm_runner(void)
{
    UnityBegin("SLM Runner Component Tests");

    RUN_TEST(test_slm_runner_setup_fails_without_model);
    RUN_TEST(test_slm_runner_setup_succeeds_after_load);
    RUN_TEST(test_slm_runner_publishes_done_after_empty_prompt);

    return (int)UnityEnd();
}
