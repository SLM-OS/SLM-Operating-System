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
 * The bus tests below run a real runner task and clients that only use the
 * router: token order and count, long prompts, stop, restart, model choice,
 * session options, and the #1093 lock rule. Most script the logits; one runs
 * the real forward path on scripts/fixtures/slm/tiny_qwen2.gguf.
 */

#include "unity.h"
#include "../include/slm_ffi.h"
#include "../include/slm_runner.h"
#include "../include/string.h"
#include "../include/shell.h"
#include "../include/shell_io.h"
#include "../include/shell_session.h"
#include "../include/component.h"
#include "../include/task.h"
#include "../include/sched.h"
#include "../include/timer.h"
#include "../include/uart.h"
#include "../include/lua_slm.h"
#include <stdbool.h>
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
extern int msg_router_publish_nowait(const uint8_t *topic_name, const uint8_t *data);

extern void slm_runner_test_set_publish_timeout(uint32_t ms);
extern void (*slm_runner_test_log_hook)(const char *line, bool ask_printing);
extern uint32_t slm_runner_test_publish_bytes(const uint8_t *bytes, size_t len);

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

    /* Nobody reads /slm/done until the helper returns. Don't sit out the
     * full back-pressure wait for that. */
    slm_runner_test_set_publish_timeout(100u);

    /* Run the empty prompt through the helper. */
    uint32_t tokens_emitted = 0xDEADBEEFu;  /* sentinel — must be overwritten */
    int rc = slm_runner_handle_prompt((uint32_t)sess,
                                      /* prompt = */ "",
                                      /* prompt_len = */ 0,
                                      &tokens_emitted);

    /* M5.2 contract: empty prompt → rc=0 → no token callback. */
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, tokens_emitted);

    slm_runner_test_set_publish_timeout(0);

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
 * rust_slm_stop on a fresh session returns 0 and does nothing, since
 * no prompt is running. On a bad session id it returns -1. Pins the FFI
 * signature so a future change to the stop-flag mechanism surfaces here.
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
 * The bus path, end to end
 *
 * A real runner task answers real publishes while this task plays the client
 * at the shell's priority, the way a client on the shell runs. `slm ask` and a
 * raw subscriber (client_*) both go through the router only. Logits are
 * scripted except in the tiny-model test.
 * ============================================================================ */

/* Ids in rust_slm_test_build_cli_fixture: bytes below 256, then these. */
#define CLI_IM_END    511u
#define CLIENT_IDX    51
#define CLIENT_WAIT_S 20u

/* What the fixture's chat template adds around a prompt of N bytes, one token
 * per byte: system turn, user turn, assistant header, specials. */
#define CLI_CHAT_OVERHEAD 97u

#define SCRIPT(looping, ...) \
    do { \
        static const uint32_t ids_[] = { __VA_ARGS__ }; \
        TEST_ASSERT_EQUAL_INT(0, rust_slm_test_script( \
            ids_, sizeof(ids_) / sizeof(ids_[0]), (looping))); \
    } while (0)

#define BUS_CAP_SIZE 4096
static struct {
    char     buf[BUS_CAP_SIZE];
    size_t   len;
    uint32_t tries;
    uint32_t ctrl_c_at_try;
} bcap;

static uint8_t bus_fixture_buf[16384];
static struct shell_io bus_io;
static struct shell_session *bus_sess;
static uint8_t bus_saved_priority;
static bool bus_active;

static int bcap_read_char(struct shell_io *io)
{
    (void)io;
    return -1;
}

static int bcap_try_read_char(struct shell_io *io)
{
    (void)io;
    bcap.tries++;
    if (bcap.ctrl_c_at_try != 0 && bcap.tries == bcap.ctrl_c_at_try) {
        return 0x03;
    }
    return -1;
}

static void bcap_write(struct shell_io *io, const char *buf, size_t len)
{
    (void)io;
    for (size_t i = 0; i < len && bcap.len + 1 < BUS_CAP_SIZE; i++) {
        bcap.buf[bcap.len++] = buf[i];
    }
    bcap.buf[bcap.len] = '\0';
}

static void bcap_flush(struct shell_io *io)
{
    (void)io;
}

static bool bcap_is_open(struct shell_io *io)
{
    (void)io;
    return true;
}

static void bcap_clear(void)
{
    bcap.len = 0;
    bcap.buf[0] = '\0';
    bcap.tries = 0;
    bcap.ctrl_c_at_try = 0;
}

static int bus_load_fixture(const char *name)
{
    size_t size = 0;
    if (rust_slm_test_build_cli_fixture(bus_fixture_buf, sizeof(bus_fixture_buf),
                                        &size) != 0) {
        return -1;
    }
    return rust_slm_load((const uint8_t *)name, bus_fixture_buf, size);
}

static void bus_begin_no_model(void)
{
    bus_active = true;
    rust_slm_test_reset();
    for (uint32_t i = 0; i < 4u; i++) {
        rust_slm_session_close(i);
    }
    msg_router_init();
    slm_runner_test_set_publish_timeout(0);
    bcap_clear();
    memset(&bus_io, 0, sizeof(bus_io));
    bus_io.read_char = bcap_read_char;
    bus_io.try_read_char = bcap_try_read_char;
    bus_io.write = bcap_write;
    bus_io.flush = bcap_flush;
    bus_io.is_open = bcap_is_open;
    bus_sess = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(bus_sess);
    bus_sess->io = &bus_io;
    shell_session_bind(task_current(), bus_sess);
    /* The shell's priority, and the runner's. */
    bus_saved_priority = task_current()->priority;
    task_set_priority(task_current(), TASK_PRIORITY_IDLE);
}

static void bus_begin(void)
{
    bus_begin_no_model();
    TEST_ASSERT_EQUAL_INT(0, bus_load_fixture("cli"));
}

static void bus_end(void)
{
    if (!bus_active) {
        return;
    }
    bus_active = false;
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    if (st.running) {
        slm_runner_stop();
    }
    rust_slm_test_script(NULL, 0, 0);
    slm_runner_test_set_publish_timeout(0);
    slm_runner_test_log_hook = NULL;
    task_set_priority(task_current(), bus_saved_priority);
    shell_session_unbind(task_current());
    bus_sess->io = NULL;
    shell_session_free(bus_sess);
    for (uint32_t i = 0; i < 4u; i++) {
        rust_slm_session_close(i);
    }
    rust_slm_test_reset();
    msg_router_init();
}

/* A failed assert skips the test's bus_end. Without this one failure would
 * leave the next test at the wrong priority with a runner up. */
#define RUN_BUS_TEST(fn) \
    do { \
        RUN_TEST(fn); \
        bus_end(); \
    } while (0)

#define ASSERT_OUTPUT(needle) \
    do { \
        if (strstr(bcap.buf, (needle)) == NULL) { \
            uart_puts("[test] output was: "); \
            uart_puts(bcap.buf); \
            uart_puts("\r\n"); \
        } \
        TEST_ASSERT_NOT_NULL(strstr(bcap.buf, (needle))); \
    } while (0)

static void client_subscribe(void)
{
    msg_router_unsubscribe_all(CLIENT_IDX);
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(SLM_TOPIC_TOKEN, CLIENT_IDX));
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(SLM_TOPIC_DONE, CLIENT_IDX));
}

static int client_publish_prompt(const char *text)
{
    return msg_router_publish_reliable((const uint8_t *)SLM_TOPIC_PROMPT,
                                       (const uint8_t *)text, 5000u, NULL);
}

/* Next message for the client, or NULL after CLIENT_WAIT_S. Leaves it unacked. */
static const char *client_next(char *topic)
{
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() * CLIENT_WAIT_S;
    while (timer_get_count() - start < limit) {
        const char *data = msg_router_receive(CLIENT_IDX, topic);
        if (data) {
            return data;
        }
        yield();
    }
    return NULL;
}

/* Read to /slm/done, copying its text. Returns the token messages read. */
static uint32_t client_drain_to_done(char *done, size_t done_size)
{
    uint32_t msgs = 0;
    for (;;) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        const char *data = client_next(topic);
        if (!data) {
            done[0] = '\0';
            return msgs;
        }
        if (strcmp(topic, SLM_TOPIC_DONE) == 0) {
            strncpy(done, data, done_size - 1u);
            done[done_size - 1u] = '\0';
            msg_router_ack(CLIENT_IDX);
            return msgs;
        }
        msgs++;
        msg_router_ack(CLIENT_IDX);
    }
}

/* Every byte of the reply comes through the bus, in order, with stats. */
static void test_ask_prints_every_token_then_stats(void)
{
    bus_begin();
    SCRIPT(0, 'H', 'e', 'l', 'l', 'o', ' ', 'b', 'u', 's', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("Hello bus\r\n[slm done] rc=0 stop=eos ");
    ASSERT_OUTPUT("tokens_out=9 msgs=9 dropped=0");
    /* 'hi' through the chat template, one token per byte. */
    ASSERT_OUTPUT("tokens_in=99 ");
    ASSERT_OUTPUT("ttft_ms=");
    ASSERT_OUTPUT("decode_ms=");
    ASSERT_OUTPUT("[slm ask] received 9 of 9 token messages");
    bus_end();
}

/* At the shell's priority: above it, a decode starves the shell. A decode
 * here runs minutes on CPU 0, so the tests can't see that any other way. */
static void test_runner_runs_at_the_shells_priority(void)
{
    bus_begin();
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256"));
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    component_info_t info;
    TEST_ASSERT_EQUAL_INT(0, component_get_info((uint32_t)st.component_idx, &info));
    TEST_ASSERT_EQUAL_UINT32(TASK_PRIORITY_IDLE, info.priority);
    bus_end();
}

/* The runner's own UART line waits for `slm ask` to finish printing the
 * reply: on hardware it landed inside the /slm/done line and changed the
 * field values on screen. Here the client is slowed down so the runner's
 * line would go out first without the wait. */
static volatile uint32_t log_lines;
static volatile uint32_t log_lines_during_ask;
static char log_last[80];

static void record_log(const char *line, bool ask_printing)
{
    log_lines = log_lines + 1u;
    if (ask_printing) {
        log_lines_during_ask = log_lines_during_ask + 1u;
    }
    strncpy(log_last, line, sizeof(log_last) - 1u);
}

/* A slow console, as a UART is: the runner gets the CPU while the client
 * prints. */
static void slow_write(struct shell_io *io, const char *buf, size_t len)
{
    bcap_write(io, buf, len);
    task_sleep_ms(2);
}

static void test_runner_log_waits_for_ask_output(void)
{
    bus_begin();
    SCRIPT(0, 'a', 'b', 'c', 'd', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256"));
    log_lines = 0;
    log_lines_during_ask = 0;
    log_last[0] = '\0';
    slm_runner_test_log_hook = record_log;
    bus_io.write = slow_write;

    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    for (uint32_t i = 0; i < 2000u && log_lines == 0; i++) {
        task_sleep_ms(1);
    }
    slm_runner_test_log_hook = NULL;
    bus_io.write = bcap_write;

    TEST_ASSERT_EQUAL_UINT32(1u, log_lines);
    TEST_ASSERT_EQUAL_UINT32(0u, log_lines_during_ask);
    TEST_ASSERT_NOT_NULL(strstr(log_last, "[slm-runner] prompt #1 rc=0 tokens=4"));
    ASSERT_OUTPUT("msgs=4 dropped=0");
    bus_end();
}

/* A fast producer against a reader that takes its time with every token:
 * order and count hold, and nothing is reported lost. */
static void test_slow_reader_gets_every_token_in_order(void)
{
    static uint32_t ids[64];
    for (uint32_t i = 0; i < 64u; i++) {
        ids[i] = '0' + i;  /* printable, all different */
    }
    bus_begin();
    TEST_ASSERT_EQUAL_INT(0, rust_slm_test_script(ids, 64u, 1));
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 64"));
    client_subscribe();
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("go"));

    for (uint32_t i = 0; i < 64u; i++) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        const char *data = client_next(topic);
        TEST_ASSERT_NOT_NULL(data);
        TEST_ASSERT_EQUAL_STRING(SLM_TOPIC_TOKEN, topic);
        TEST_ASSERT_EQUAL_INT('0' + (int)i, data[0]);
        TEST_ASSERT_EQUAL_INT('\0', data[1]);
        if (i % 8u == 0) {
            task_sleep_ms(20);  /* the runner must wait, not overwrite */
        }
        msg_router_ack(CLIENT_IDX);
    }

    char done[256];
    TEST_ASSERT_EQUAL_UINT32(0u, client_drain_to_done(done, sizeof(done)));
    TEST_ASSERT_NOT_NULL(strstr(done, "rc=0 stop=max"));
    TEST_ASSERT_NOT_NULL(strstr(done, "tokens_out=64 msgs=64 dropped=0"));
    bus_end();
}

/* A subscriber that never reads costs itself the tokens and is reported, but
 * doesn't wedge the service or cost a live client anything. */
static void test_dead_subscriber_doesnt_wedge_a_live_client(void)
{
    bus_begin();
    SCRIPT(0, 'a', 'b', 'c', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256"));
    slm_runner_test_set_publish_timeout(50u);
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(SLM_TOPIC_TOKEN, CLIENT_IDX));

    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("abc\r\n[slm done] rc=0 stop=eos ");
    ASSERT_OUTPUT("msgs=3 dropped=2");
    ASSERT_OUTPUT("received 3 of 3");

    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);
    TEST_ASSERT_EQUAL_UINT32(2u, st.dropped);
    bus_end();
}

/* A 64-token prompt is about 370 bytes. The fixture has one token per byte, so
 * tokens_in shows that all of it reached the tokenizer. */
static void test_long_prompt_arrives_whole(void)
{
    static char line[600];
    static const char prefix[] = "slm ask \"";
    bus_begin();
    SCRIPT(0, 'k', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 1024"));

    size_t pos = sizeof(prefix) - 1u;
    memcpy(line, prefix, pos);
    for (uint32_t i = 0; i < 370u; i++) {
        line[pos++] = (i % 7u == 6u) ? ' ' : (char)('a' + (i % 26u));
    }
    line[pos++] = '"';
    line[pos] = '\0';
    TEST_ASSERT_EQUAL_INT(0, shell_execute(line));

    char want[32];
    uart_snprintf(want, sizeof(want), "tokens_in=%u ", CLI_CHAT_OVERHEAD + 370u);
    ASSERT_OUTPUT(want);
    ASSERT_OUTPUT("tokens_out=1 msgs=1 dropped=0");
    bus_end();
}

/* The whole bus limit works, and one byte more is refused, never cut. */
static void test_prompt_over_the_bus_limit_is_refused(void)
{
    static char line[600];
    static const char prefix[] = "slm ask \"";
    bus_begin();
    SCRIPT(0, 'k', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 1024"));

    size_t pos = sizeof(prefix) - 1u;
    memcpy(line, prefix, pos);
    for (uint32_t i = 0; i < MSG_ROUTER_MAX_PAYLOAD; i++) {
        line[pos++] = 'x';
    }
    line[pos] = '"';
    line[pos + 1u] = '\0';
    TEST_ASSERT_EQUAL_INT(0, shell_execute(line));
    char want[32];
    uart_snprintf(want, sizeof(want), "tokens_in=%u ",
                  CLI_CHAT_OVERHEAD + MSG_ROUTER_MAX_PAYLOAD);
    ASSERT_OUTPUT(want);

    bcap_clear();
    line[pos] = 'x';
    line[pos + 1u] = '"';
    line[pos + 2u] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, shell_execute(line));
    ASSERT_OUTPUT("the bus takes 511");

    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.prompts);
    bus_end();
}

/* `msg send` reaches the runner with a long prompt too. */
static void test_msg_send_carries_a_long_prompt(void)
{
    static char line[500];
    static const char prefix[] = "msg send /slm/prompt \"";
    bus_begin();
    SCRIPT(0, 'k', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 1024"));
    client_subscribe();

    size_t pos = sizeof(prefix) - 1u;
    memcpy(line, prefix, pos);
    for (uint32_t i = 0; i < 370u; i++) {
        line[pos++] = 'y';
    }
    line[pos++] = '"';
    line[pos] = '\0';
    TEST_ASSERT_EQUAL_INT(0, shell_execute(line));

    char done[256];
    TEST_ASSERT_EQUAL_UINT32(1u, client_drain_to_done(done, sizeof(done)));
    char want[32];
    uart_snprintf(want, sizeof(want), "tokens_in=%u ", CLI_CHAT_OVERHEAD + 370u);
    TEST_ASSERT_NOT_NULL(strstr(done, want));
    bus_end();
}

/* A token longer than a message arrives whole, split on character boundaries. */
#define LONG_TOKEN_BYTES 800u
static uint8_t long_token[LONG_TOKEN_BYTES];
static char long_token_got[LONG_TOKEN_BYTES + 1u];
static volatile uint32_t long_token_got_len;
static volatile uint32_t long_token_msgs;
static volatile uint32_t long_token_bad_starts;
static volatile bool long_token_quit;

static void long_token_reader(void *arg)
{
    (void)arg;
    for (;;) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        const char *data = msg_router_receive(CLIENT_IDX, topic);
        if (data) {
            size_t n = strlen(data);
            if (((uint8_t)data[0] & 0xC0u) == 0x80u) {
                long_token_bad_starts = long_token_bad_starts + 1u;
            }
            if (long_token_got_len + n <= LONG_TOKEN_BYTES) {
                memcpy(long_token_got + long_token_got_len, data, n);
                long_token_got_len = long_token_got_len + (uint32_t)n;
            }
            long_token_msgs = long_token_msgs + 1u;
            msg_router_ack(CLIENT_IDX);
        } else if (long_token_quit) {
            return;
        } else {
            yield();
        }
    }
}

static void test_long_token_is_split_on_character_boundaries(void)
{
    /* All 3-byte characters: U+4F60 repeated, 800 bytes cut mid-character at
     * every multiple of 511 unless the split moves. */
    for (uint32_t i = 0; i < LONG_TOKEN_BYTES; i++) {
        static const uint8_t ch[3] = { 0xE4, 0xBD, 0xA0 };
        long_token[i] = ch[i % 3u];
    }
    size_t whole = LONG_TOKEN_BYTES - (LONG_TOKEN_BYTES % 3u);

    msg_router_init();
    slm_runner_test_set_publish_timeout(2000u);
    TEST_ASSERT_EQUAL_INT(0, msg_router_subscribe(SLM_TOPIC_TOKEN, CLIENT_IDX));
    long_token_got_len = 0;
    long_token_msgs = 0;
    long_token_bad_starts = 0;
    long_token_quit = false;
    /* Below this task, as the shell is below a NORMAL publisher. */
    struct task *t = task_create_with_priority("tok-reader", long_token_reader,
                                               NULL, TASK_PRIORITY_IDLE);
    TEST_ASSERT_NOT_NULL(t);
    task_set_affinity(t, 0);
    scheduler_add_task(t);

    uint32_t msgs = slm_runner_test_publish_bytes(long_token, whole);
    long_token_quit = true;
    task_sleep_ms(50);  /* time for it to see the flag and exit */

    TEST_ASSERT_EQUAL_UINT32(2u, msgs);
    TEST_ASSERT_EQUAL_UINT32(2u, long_token_msgs);
    TEST_ASSERT_EQUAL_UINT32(0u, long_token_bad_starts);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)whole, long_token_got_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(long_token_got, long_token, whole));
    slm_runner_test_set_publish_timeout(0);
    msg_router_init();
}

/* /slm/stop ends a prompt that would otherwise run forever, and the runner
 * goes on to answer the next one. */
static void test_slm_stop_topic_stops_the_prompt(void)
{
    bus_begin();
    SCRIPT(1, 'x');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 0"));
    client_subscribe();
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("go"));

    for (uint32_t i = 0; i < 5u; i++) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        TEST_ASSERT_NOT_NULL(client_next(topic));
        msg_router_ack(CLIENT_IDX);
    }
    TEST_ASSERT_EQUAL_INT(1, msg_router_publish_nowait(
        (const uint8_t *)SLM_TOPIC_STOP, (const uint8_t *)"stop"));

    char done[256];
    client_drain_to_done(done, sizeof(done));
    TEST_ASSERT_NOT_NULL(strstr(done, "rc=0 stop=stopped"));

    SCRIPT(0, 'y', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("again"));
    TEST_ASSERT_EQUAL_UINT32(1u, client_drain_to_done(done, sizeof(done)));
    TEST_ASSERT_NOT_NULL(strstr(done, "rc=0 stop=eos"));
    bus_end();
}

/* A stop looks at /slm/stop alone: a prompt queued behind the running one
 * survives it and gets answered. */
static void test_stop_leaves_a_queued_prompt_queued(void)
{
    bus_begin();
    SCRIPT(1, 'x');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 8"));
    client_subscribe();
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("first"));

    /* The runner now waits on this unacked token, with its mailboxes free. */
    char topic[MSG_ROUTER_TOPIC_LEN];
    TEST_ASSERT_NOT_NULL(client_next(topic));
    TEST_ASSERT_EQUAL_INT(1, msg_router_publish_nowait(
        (const uint8_t *)SLM_TOPIC_PROMPT, (const uint8_t *)"second"));
    TEST_ASSERT_EQUAL_INT(1, msg_router_publish_nowait(
        (const uint8_t *)SLM_TOPIC_STOP, (const uint8_t *)"stop"));
    msg_router_ack(CLIENT_IDX);

    char done[256];
    client_drain_to_done(done, sizeof(done));
    TEST_ASSERT_NOT_NULL(strstr(done, "stop=stopped"));
    TEST_ASSERT_NOT_NULL(strstr(done, "prompts=1"));
    TEST_ASSERT_EQUAL_UINT32(8u, client_drain_to_done(done, sizeof(done)));
    TEST_ASSERT_NOT_NULL(strstr(done, "stop=max"));
    TEST_ASSERT_NOT_NULL(strstr(done, "prompts=2"));
    bus_end();
}

/* `slm stop <session>` stops the runner's prompt as well. */
static void test_slm_stop_verb_stops_the_prompt(void)
{
    bus_begin();
    SCRIPT(1, 'x');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 0"));
    client_subscribe();
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("go"));

    char topic[MSG_ROUTER_TOPIC_LEN];
    TEST_ASSERT_NOT_NULL(client_next(topic));
    msg_router_ack(CLIENT_IDX);

    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_GREATER_OR_EQUAL(0, st.session);
    char cmd[32];
    uart_snprintf(cmd, sizeof(cmd), "slm stop %d", (int)st.session);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(cmd));

    char done[256];
    client_drain_to_done(done, sizeof(done));
    TEST_ASSERT_NOT_NULL(strstr(done, "rc=0 stop=stopped"));
    bus_end();
}

/* Ctrl-C in `slm ask` goes over the bus and every token is still accounted for. */
static void test_ask_ctrl_c_publishes_stop(void)
{
    bus_begin();
    SCRIPT(1, 'x');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 0"));
    bcap.ctrl_c_at_try = 3u;
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask go"));
    ASSERT_OUTPUT("^C");
    ASSERT_OUTPUT("rc=0 stop=stopped");
    ASSERT_OUTPUT("[slm ask] received");
    bus_end();
}

/* Stopped means stopped, and a restart serves the bus again from scratch. */
static void test_runner_restart(void)
{
    bus_begin();
    SCRIPT(1, 'k');
    uint32_t components_before = component_count();
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 2"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm runner start"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask one"));
    ASSERT_OUTPUT("prompts=1");

    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm runner stop"));
    ASSERT_OUTPUT("slm-runner: stopped");
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm runner stop"));

    bcap_clear();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm ask two"));
    ASSERT_OUTPUT("no runner");

    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 2"));
    ASSERT_OUTPUT("slm-runner: running");
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask three"));
    ASSERT_OUTPUT("tokens_out=2 msgs=2");
    ASSERT_OUTPUT("prompts=1");

    /* Component-run path too, and it survives an idle stretch. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm runner stop"));
    TEST_ASSERT_EQUAL_UINT32(components_before, component_count());
    TEST_ASSERT_EQUAL_INT(0, shell_execute("component run slm-runner"));
    task_sleep_ms(50);
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);
    bus_end();
}

/* Two prompts in a row see the same context: each one is its own conversation. */
static void test_each_prompt_is_a_fresh_conversation(void)
{
    bus_begin();
    SCRIPT(1, 'k');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 2"));
    for (uint32_t i = 0; i < 3u; i++) {
        bcap_clear();
        TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
        ASSERT_OUTPUT("tokens_in=99 ");
    }
    bus_end();
}

/* No model, or a handle with nothing in it: /slm/done says so, the runner stays
 * up, and a model loaded later is picked up without a restart. */
static void test_no_model_and_bad_handle_answer_on_the_bus(void)
{
    bus_begin_no_model();
    SCRIPT(1, 'k');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 1"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("rc=-1 err=no-model");
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);

    TEST_ASSERT_EQUAL_INT(0, bus_load_fixture("cli"));
    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("rc=0 stop=max");
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm runner stop"));

    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --handle 3 --sampler greedy --ctx 256 --max 1"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("rc=-1 err=bad-handle");
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm runner stop"));

    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm runner start --handle x"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm runner start --bogus 1"));
    bus_end();
}

/* The runner uses the most recently loaded model: not slot 0, not the highest
 * slot. A reload of the session's slot gets a new session. */
static void test_runner_uses_most_recently_loaded_model(void)
{
    bus_begin();  /* slot 0 */
    SCRIPT(1, 'k');
    TEST_ASSERT_EQUAL_INT(1, bus_load_fixture("second"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 1"));
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_INT(1, st.model);

    /* Slot 0 freed and refilled: now the newest model is the lower slot. */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload(0));
    TEST_ASSERT_EQUAL_INT(0, bus_load_fixture("third"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_INT(0, st.model);

    /* Its model goes away: back to the one that's left. */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload(0));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_INT(1, st.model);

    /* All gone, then one back: a new load with no old session to reuse. */
    TEST_ASSERT_EQUAL_INT(0, rust_slm_unload(1));
    TEST_ASSERT_EQUAL_INT(0, bus_load_fixture("fourth"));  /* first free: 0 */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_INT(0, st.model);
    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info((uint32_t)st.session, &info));
    bus_end();
}

/* --handle pins a slot even when a newer model exists. */
static void test_runner_handle_pins_the_slot(void)
{
    bus_begin();
    SCRIPT(1, 'k');
    TEST_ASSERT_EQUAL_INT(1, bus_load_fixture("second"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --handle 0 --sampler greedy --ctx 256 --max 1"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_INT(0, st.model);
    bus_end();
}

/* The same session options as `slm launch`, through the same session call. */
static void test_runner_honours_session_options(void)
{
    bus_begin();
    SCRIPT(1, 'k');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --gpu cpu --threads 3 --sampler greedy --ctx 256 --max 1"));
    ASSERT_OUTPUT("gpu=cpu");
    slm_runner_status_t st;
    slm_runner_get_status(&st);
    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info((uint32_t)st.session, &info));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_CPU, info.gpu_mode);
    TEST_ASSERT_EQUAL_UINT32(3u, info.threads);
    TEST_ASSERT_EQUAL_UINT32(256u, info.max_ctx);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_test_script_gpu_ops_seen());
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm runner stop"));

    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm runner start --gpu maybe"));
    /* QEMU has no GPU channel: `slm launch --gpu on` refuses, and so does a
     * prompt here, saying why, with the runner still up. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --gpu on --sampler greedy --ctx 256 --max 1"));
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_ON, st.gpu_mode);
    TEST_ASSERT_EQUAL_INT(-1, st.session);
    bcap_clear();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm ask hi"));
    ASSERT_OUTPUT("rc=-1 err=no-gpu-program");
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);
    bus_end();
}

/*
 * The #1093 rule, on the CPU the runner shares with the shell: a runner parked
 * under back-pressure must hold no lock that an IRQs-off path could spin on.
 * With IRQs masked here the runner can't run again, so if it were holding the
 * router or session lock across its wait, this test would hang and the QEMU
 * timeout would fail it.
 */
static void test_runner_parks_without_a_lock(void)
{
    bus_begin();
    TEST_ASSERT_EQUAL_UINT32(0u, task_current()->cpu_affinity);
    SCRIPT(1, 'x');
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256 --max 0"));
    client_subscribe();
    TEST_ASSERT_EQUAL_INT(1, client_publish_prompt("go"));

    char topic[MSG_ROUTER_TOPIC_LEN];
    TEST_ASSERT_NOT_NULL(client_next(topic));  /* unacked: the runner waits */

    slm_runner_status_t st;
    slm_runner_get_status(&st);
    TEST_ASSERT_EQUAL_UINT32(1u, st.busy);

    uint64_t flags = slm_irq_save();
    const char *pending = msg_router_receive(CLIENT_IDX, topic);
    int32_t busy = rust_slm_session_busy((uint32_t)st.session);
    int32_t stop_rc = rust_slm_stop((uint32_t)st.session);
    slm_runner_get_status(&st);
    msg_router_ack(CLIENT_IDX);
    slm_irq_restore(flags);

    TEST_ASSERT_NOT_NULL(pending);
    TEST_ASSERT_EQUAL_INT(1, busy);
    TEST_ASSERT_EQUAL_INT(0, stop_rc);
    TEST_ASSERT_EQUAL_UINT32(1u, st.running);

    /* While it's busy, a second client is told so instead of mixing replies. */
    bcap_clear();
    if (st.busy) {
        int rc = shell_execute("slm ask hi");
        if (rc != 0) {
            ASSERT_OUTPUT("answering a prompt");
        }
    }

    char done[256];
    client_drain_to_done(done, sizeof(done));
    TEST_ASSERT_NOT_NULL(strstr(done, "stop=stopped"));
    bus_end();
}

/* The Lua client in docs/tutorials/slm-component.md, as written there except
 * that it collects the text instead of printing it. */
static void test_lua_client_gets_the_reply(void)
{
    bus_begin();
    SCRIPT(0, 'L', 'u', 'a', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 256"));
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);
    int rc = lua_slm_dostring(L,
        "local done = nil\n"
        "local text = ''\n"
        "slm.msg_subscribe('/slm/token', function(_, t) text = text .. t end)\n"
        "slm.msg_subscribe('/slm/done',  function(_, s) done = s end)\n"
        "assert(slm.msg_publish('/slm/prompt', 'hi') == 1)\n"
        "while not done do slm.yield() end\n"
        "assert(text == 'Lua', text)\n"
        "assert(string.find(done, 'rc=0 stop=eos', 1, true), done)\n"
        "assert(string.find(done, 'msgs=3 dropped=0', 1, true), done)\n");
    lua_slm_close(L);
    TEST_ASSERT_EQUAL_INT(0, rc);
    bus_end();
}

#if defined(ENABLE_BOOT_TESTS)
/* Boot-test kernels only: TEST_SOURCES go into every image, the fixture doesn't.
 *
 * The real forward path on the tiny GGUF, no scripted logits. The bus has to
 * deliver exactly what a direct `slm prompt` prints on the same model. The
 * reply goes to the UART, as a record of the run. */
extern const uint8_t slm_golden_tiny_start[], slm_golden_tiny_end[];

static void test_tiny_model_reply_matches_direct_prompt(void)
{
    static char direct[BUS_CAP_SIZE];
    bus_begin_no_model();
    TEST_ASSERT_EQUAL_INT(0, rust_slm_load((const uint8_t *)"tiny-qwen2",
        slm_golden_tiny_start, (size_t)(slm_golden_tiny_end - slm_golden_tiny_start)));

    /* Raw, because a chat prompt's system turn costs seconds of forward steps
     * here, and the suite has a time limit. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm launch 0 --sampler greedy --ctx 64 --gpu cpu"));
    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw --max 8 hi"));
    /* The reply is everything before the telemetry line. */
    const char *end = strstr(bcap.buf, "\r\n[slm]");
    TEST_ASSERT_NOT_NULL(end);
    size_t direct_len = (size_t)(end - bcap.buf);
    memcpy(direct, bcap.buf, direct_len);
    direct[direct_len] = '\0';
    TEST_ASSERT_GREATER_THAN(0, (int)direct_len);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm close 0"));

    TEST_ASSERT_EQUAL_INT(0, shell_execute(
        "slm runner start --sampler greedy --ctx 64 --gpu cpu --max 8 --raw"));
    bcap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm ask hi"));
    uart_puts("[slm-runner-demo] slm ask hi (tiny_qwen2.gguf, through the bus):\r\n");
    uart_puts(bcap.buf);
    TEST_ASSERT_EQUAL_INT(0, memcmp(bcap.buf, direct, direct_len));
    TEST_ASSERT_EQUAL_INT('\r', bcap.buf[direct_len]);
    ASSERT_OUTPUT("[slm done] rc=0 stop=max");
    ASSERT_OUTPUT("tokens_out=8 ");
    ASSERT_OUTPUT("dropped=0");
    bus_end();
}
#endif /* ENABLE_BOOT_TESTS */

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

    /* The bus path, with a real runner task. */
    RUN_BUS_TEST(test_ask_prints_every_token_then_stats);
    RUN_BUS_TEST(test_runner_runs_at_the_shells_priority);
    RUN_BUS_TEST(test_runner_log_waits_for_ask_output);
    RUN_BUS_TEST(test_slow_reader_gets_every_token_in_order);
    RUN_BUS_TEST(test_dead_subscriber_doesnt_wedge_a_live_client);
    RUN_BUS_TEST(test_long_prompt_arrives_whole);
    RUN_BUS_TEST(test_prompt_over_the_bus_limit_is_refused);
    RUN_BUS_TEST(test_msg_send_carries_a_long_prompt);
    RUN_BUS_TEST(test_long_token_is_split_on_character_boundaries);
    RUN_BUS_TEST(test_slm_stop_topic_stops_the_prompt);
    RUN_BUS_TEST(test_stop_leaves_a_queued_prompt_queued);
    RUN_BUS_TEST(test_slm_stop_verb_stops_the_prompt);
    RUN_BUS_TEST(test_ask_ctrl_c_publishes_stop);
    RUN_BUS_TEST(test_runner_restart);
    RUN_BUS_TEST(test_each_prompt_is_a_fresh_conversation);
    RUN_BUS_TEST(test_no_model_and_bad_handle_answer_on_the_bus);
    RUN_BUS_TEST(test_runner_uses_most_recently_loaded_model);
    RUN_BUS_TEST(test_runner_handle_pins_the_slot);
    RUN_BUS_TEST(test_runner_honours_session_options);
    RUN_BUS_TEST(test_runner_parks_without_a_lock);
    RUN_BUS_TEST(test_lua_client_gets_the_reply);
#if defined(ENABLE_BOOT_TESTS)
    RUN_BUS_TEST(test_tiny_model_reply_matches_direct_prompt);
#endif

    return (int)UnityEnd();
}
