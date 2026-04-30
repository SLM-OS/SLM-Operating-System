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
     * simple scan over the (NUL-terminated, < 60 byte) message.
     * Bound: the inner test inspects `data[i + 12]`, so i + 12 must
     * stay strictly below 60 (i.e. i + 12 < 60 → i + 13 <= 60 is
     * off-by-one — the 13th byte at index i+12 must be a valid
     * payload slot). Use `i + 12 < 60` to keep the lookahead
     * in-bounds even if the router ever stops NUL-padding past the
     * payload. */
    int found_zero_tokens = 0;
    for (size_t i = 0; data[i] != '\0' && i < 60; i++) {
        if (i + 12 < 60 &&
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
 * Direct FFI tests
 *
 * The runner-component tests above reach the rust_slm_prompt /
 * rust_slm_stop / rust_slm_session_reset / rust_slm_stats FFI
 * functions transitively through `slm_runner_handle_prompt`. The
 * tests below pin the FFI signatures themselves (callback typedef,
 * return-value contract, NULL-arg behavior) so a Rust-side type drift
 * surfaces here instead of at first hardware run.
 * ============================================================================ */

/* Token callback that records invocation counts + the last token id
 * it was handed. All four counters live in a struct so a single
 * static instance threads through the test cases without globals. */
typedef struct {
    uint32_t calls;
    uint32_t last_token_id;
    uint8_t  last_byte;
} TokenCbState;

static int32_t direct_token_cb(void *user, uint32_t token_id,
                               const uint8_t *bytes, size_t bytes_len)
{
    TokenCbState *st = (TokenCbState *)user;
    if (st) {
        st->calls += 1;
        st->last_token_id = token_id;
        if (bytes && bytes_len > 0) {
            st->last_byte = bytes[0];
        }
    }
    return 1;  /* keep generating */
}

/* Test bootstrap: load fixture + open small-ctx session. The kernel
 * test binary builds with `-mgeneral-regs-only`, so calling
 * `rust_slm_session_open` directly (it takes f32 by value) won't
 * compile here. Route through `slm_runner_setup_session_with_ctx`
 * (in slm_runner.c, which is built without that constraint). Inlined
 * via macro because the Unity asserts inside expand to bare `return;`
 * which a non-void helper can't host. */
#define DIRECT_FFI_TEST_OPEN(model_idx_var, sess_var) \
    rust_slm_test_reset(); \
    int model_idx_var = -1; \
    runner_load_fixture(&(model_idx_var)); \
    int sess_var = slm_runner_setup_session_with_ctx(4u); \
    TEST_ASSERT_GREATER_OR_EQUAL(0, sess_var)

#define DIRECT_FFI_TEST_CLOSE(model_idx_var, sess_var) \
    do { \
        TEST_ASSERT_EQUAL_INT32(0, rust_slm_session_close((uint32_t)(sess_var))); \
        TEST_ASSERT_EQUAL_INT(0, rust_slm_unload((uint32_t)(model_idx_var))); \
    } while (0)

/*
 * rust_slm_prompt with an empty prompt returns 0 and never invokes
 * the callback (greedy on zero-logits produces token id 0 = EOS,
 * decoder exits before first emission). Pins the M5.2 state-machine
 * contract that empty prompts are valid input.
 */
static void test_rust_slm_prompt_empty_returns_zero(void)
{
    DIRECT_FFI_TEST_OPEN(model_idx, sess);

    TokenCbState st = {0};
    int32_t rc = rust_slm_prompt((uint32_t)sess, NULL, 0, 0,
                                 direct_token_cb, &st);
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, st.calls);

    DIRECT_FFI_TEST_CLOSE(model_idx, sess);
}

/*
 * rust_slm_prompt rejects a NULL prompt pointer when prompt_len > 0.
 * Pins the FFI safety contract: callers paired (NULL, 0) must work
 * (drill empty prompt) but (NULL, N) must fail rather than read
 * garbage from address 0.
 */
static void test_rust_slm_prompt_null_with_length_fails(void)
{
    DIRECT_FFI_TEST_OPEN(model_idx, sess);

    TokenCbState st = {0};
    int32_t rc = rust_slm_prompt((uint32_t)sess, NULL, 8u, 0,
                                 direct_token_cb, &st);
    TEST_ASSERT_EQUAL_INT32(-1, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, st.calls);

    DIRECT_FFI_TEST_CLOSE(model_idx, sess);
}

/*
 * rust_slm_stop on a fresh session returns 0 (sets the cooperative
 * stop flag idempotently). On a bad session id it returns -1. Pins
 * the FFI signature so a future change to the stop-flag mechanism
 * surfaces here.
 */
static void test_rust_slm_stop_signatures(void)
{
    DIRECT_FFI_TEST_OPEN(model_idx, sess);

    TEST_ASSERT_EQUAL_INT32(0,  rust_slm_stop((uint32_t)sess));
    /* Double-stop is idempotent. */
    TEST_ASSERT_EQUAL_INT32(0,  rust_slm_stop((uint32_t)sess));
    /* Bad session id rejected. */
    TEST_ASSERT_EQUAL_INT32(-1, rust_slm_stop(0xDEADBEEFu));

    DIRECT_FFI_TEST_CLOSE(model_idx, sess);
}

/*
 * rust_slm_session_reset on a fresh session returns 0 and leaves the
 * KV cache empty. On a bad session id it returns -1. The reset
 * itself can't be observed without running a prompt first; this
 * test pins the FFI signature and the success-on-fresh contract.
 */
static void test_rust_slm_session_reset_signatures(void)
{
    DIRECT_FFI_TEST_OPEN(model_idx, sess);

    TEST_ASSERT_EQUAL_INT32(0,  rust_slm_session_reset((uint32_t)sess));
    TEST_ASSERT_EQUAL_INT32(-1, rust_slm_session_reset(0xDEADBEEFu));

    DIRECT_FFI_TEST_CLOSE(model_idx, sess);
}

/*
 * rust_slm_stats on a fresh session returns 0 and the SlmStatsC
 * struct is fully zeroed (no prompts run yet). Pins the FFI's
 * struct-pointer-out pattern + the zero-initial-state invariant.
 * NULL out-pointer rejected with -1.
 */
static void test_rust_slm_stats_fresh_session_zero(void)
{
    DIRECT_FFI_TEST_OPEN(model_idx, sess);

    SlmStatsC stats = {
        .prompts_completed = 0xFFFFFFFFu,
        .tokens_in         = 0xFFFFFFFFu,
        .tokens_out        = 0xFFFFFFFFu,
        .last_ttft_ns      = 0xFFFFFFFFFFFFFFFFull,
        .last_decode_ns    = 0xFFFFFFFFFFFFFFFFull,
    };
    int32_t rc = rust_slm_stats((uint32_t)sess, &stats);
    TEST_ASSERT_EQUAL_INT32(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.prompts_completed);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.tokens_in);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.tokens_out);
    TEST_ASSERT_EQUAL_UINT64(0ull, stats.last_ttft_ns);
    TEST_ASSERT_EQUAL_UINT64(0ull, stats.last_decode_ns);

    /* Bad session id. */
    TEST_ASSERT_EQUAL_INT32(-1, rust_slm_stats(0xDEADBEEFu, &stats));

    DIRECT_FFI_TEST_CLOSE(model_idx, sess);
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

    /* Direct FFI signature tests (round-1 review follow-up). */
    RUN_TEST(test_rust_slm_prompt_empty_returns_zero);
    RUN_TEST(test_rust_slm_prompt_null_with_length_fails);
    RUN_TEST(test_rust_slm_stop_signatures);
    RUN_TEST(test_rust_slm_session_reset_signatures);
    RUN_TEST(test_rust_slm_stats_fresh_session_zero);

    return (int)UnityEnd();
}
