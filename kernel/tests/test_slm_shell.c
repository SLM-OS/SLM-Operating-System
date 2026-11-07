/*
 * SLM Shell Verb Tests (Phase SLM, M7.1).
 *
 * Exercises the entry-point dispatch in cmd_slm — covering the
 * three main behaviors that don't require live model bytes:
 *
 *   1. `slm load <bad-path>` returns non-zero (file not found).
 *   2. `slm status` with an empty registry prints "models=0
 *      sessions=0".
 *   3. `slm <unknown-verb>` returns non-zero with help text.
 *
 * and the prompt path end to end through `shell_execute`: quoting,
 * --gpu / --threads, --raw / --max, EOS handling, streaming, UTF-8 and
 * stopping a running prompt. Those use a small fixture with scripted logits
 * (rust_slm_test_script), because a real forward pass needs real weights.
 *
 * Hardware-side acceptance (real GGUF load, coherent text) lives in the
 * integration / hardware-deploy path documented in
 * docs/design/slm-integration.md.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_internal.h"
#include "../include/slm_shell.h"
#include "../include/slm_ffi.h"
#include "../include/string.h"
#include "../include/shell_io.h"
#include "../include/shell_session.h"
#include "../include/task.h"
#include "../include/sched.h"

/* ============================================================================
 * Tests
 * ========================================================================= */

/*
 * `slm load /<missing>` → non-zero. The file doesn't exist in the
 * test VFS, so vfs_stat_path returns -1 and the verb fails cleanly.
 * Confirms the verb dispatch and error path don't crash.
 */
static void test_slm_shell_load_invalid_path(void)
{
    /* Reset the SLM registry so the test starts from a known state. */
    rust_slm_test_reset();

    /* shell_execute parses the line into argv. The path is unlikely
     * to exist in the test VFS image. */
    int ret = shell_execute("slm load /no/such/slm/file.gguf");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm status` with no models loaded returns 0 and prints a one-line
 * summary. We can't easily snoop the UART output here, but we assert
 * the dispatch returns 0 (success) which proves both the verb table
 * lookup and the status handler ran to completion.
 */
static void test_slm_shell_status_with_no_models(void)
{
    rust_slm_test_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());

    int ret = shell_execute("slm status");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * `slm <garbage-verb>` returns -1 and prints help. Confirms the
 * dispatcher's fall-through path fires for unknown verbs without
 * crashing the shell.
 */
static void test_slm_shell_dispatch_unknown_verb(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm garbage");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * `slm` with no verb prints usage and returns non-zero.
 */
static void test_slm_shell_no_verb_prints_usage(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm info <bad-handle>` returns non-zero (slot empty).
 */
static void test_slm_shell_info_empty_slot(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm info 0");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * `slm list` with empty registry returns 0 ("No SLMs loaded.").
 */
static void test_slm_shell_list_empty_registry(void)
{
    rust_slm_test_reset();

    int ret = shell_execute("slm list");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * `slm xload` argument-validation coverage
 * ----------------------------------------------------------------------------
 * The streaming body of `slm xload` requires a live telnet session in
 * binary mode and can't be exercised under Unity, but the verb's
 * argument-parse + cap-check guards run synchronously before any IO
 * begins. Cover each negative branch that returns before the read
 * loop so a future regression in dispatch / parse shows up in CI
 * instead of on hardware. Each case must NOT alter the registry; we
 * assert that with a `rust_slm_count()` check after.
 * ========================================================================= */

/* `slm xload` with no name/total → -1 (usage). */
static void test_slm_shell_xload_missing_args(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen` with no total → -1 (still missing args). */
static void test_slm_shell_xload_missing_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen abc` → -1 (parse_uint rejects non-digits). */
static void test_slm_shell_xload_invalid_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen abc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen 0` → -1 (total must be > 0; otherwise we'd
 * pmm_alloc_pages(0) and stream into a zero-size buffer). */
static void test_slm_shell_xload_zero_total(void)
{
    rust_slm_test_reset();
    int ret = shell_execute("slm xload qwen 0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* `slm xload qwen <too-large>` → -1 (cap check via
 * rust_slm_max_gguf_bytes; declines BEFORE pmm_alloc_pages, so we
 * don't actually try to grab a 4 GB buddy block in the test.) */
static void test_slm_shell_xload_oversize_total(void)
{
    rust_slm_test_reset();
    /* uint32 max is 4294967295; the registry cap is 2 GiB, so this
     * is comfortably above and exercises the cap branch. */
    int ret = shell_execute("slm xload qwen 4000000000");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    TEST_ASSERT_EQUAL_UINT32(0u, rust_slm_count());
}

/* ============================================================================
 * Prompt path: capturing session + scripted logits
 * ========================================================================= */

/* Ids in rust_slm_test_build_cli_fixture: bytes below 256, then these. */
#define CLI_EOS       300u  /* tokenizer.ggml.eos_token_id */
#define CLI_ENDOFTEXT 509u
#define CLI_IM_END    511u

#define SLM_CAP_SIZE 2048

static struct {
    char     buf[SLM_CAP_SIZE];
    size_t   len;
    uint32_t flushes;
    uint32_t bad_flushes;    /* flushed with a partial UTF-8 char at the end */
    uint32_t tries;          /* try_read_char calls */
    uint32_t ctrl_c_at_try;  /* deliver Ctrl-C on this call; 0 = never */
    const char *rx;          /* bytes for try_read_char */
    bool     saw_output;     /* any write since arming: the prompt is live */
} cap;

static uint8_t cli_fixture_buf[16384];
static struct shell_io slm_io;
static struct shell_session *slm_sess;

static int cap_read_char(struct shell_io *io)
{
    (void)io;
    return -1;
}

static int cap_try_read_char(struct shell_io *io)
{
    (void)io;
    cap.tries++;
    if (cap.ctrl_c_at_try != 0 && cap.tries == cap.ctrl_c_at_try) {
        return 0x03;
    }
    if (cap.rx && *cap.rx) {
        return (uint8_t)*cap.rx++;
    }
    return -1;
}

static void cap_write(struct shell_io *io, const char *buf, size_t len)
{
    (void)io;
    for (size_t i = 0; i < len && cap.len + 1 < SLM_CAP_SIZE; i++) {
        cap.buf[cap.len++] = buf[i];
    }
    cap.buf[cap.len] = '\0';
    cap.saw_output = true;
}

static void cap_flush(struct shell_io *io)
{
    (void)io;
    /* A flush that ends in a UTF-8 lead or continuation byte would show the
     * user half a character; the fixture only ever splits U+4F60. */
    if (cap.len > 0) {
        uint8_t last = (uint8_t)cap.buf[cap.len - 1];
        if (last == 0xE4 || last == 0xBD) {
            cap.bad_flushes++;
        }
    }
    cap.flushes++;
}

static bool cap_is_open(struct shell_io *io)
{
    (void)io;
    return true;
}

static void cap_clear(void)
{
    cap.len = 0;
    cap.buf[0] = '\0';
    cap.flushes = 0;
    cap.bad_flushes = 0;
    cap.tries = 0;
    cap.ctrl_c_at_try = 0;
    cap.rx = NULL;
    cap.saw_output = false;
}

/* Bind a capturing session and load the fixture. Macros because Unity's
 * asserts `return` from the enclosing function. */
#define SLM_PROMPT_SETUP() \
    do { \
        rust_slm_test_reset(); \
        for (uint32_t i_ = 0; i_ < 4u; i_++) { \
            rust_slm_session_close(i_); \
        } \
        size_t size_ = 0; \
        TEST_ASSERT_EQUAL_INT(0, rust_slm_test_build_cli_fixture( \
            cli_fixture_buf, sizeof(cli_fixture_buf), &size_)); \
        TEST_ASSERT_EQUAL_INT(0, rust_slm_load((const uint8_t *)"cli", \
            cli_fixture_buf, size_)); \
        cap_clear(); \
        memset(&slm_io, 0, sizeof(slm_io)); \
        slm_io.read_char = cap_read_char; \
        slm_io.try_read_char = cap_try_read_char; \
        slm_io.write = cap_write; \
        slm_io.flush = cap_flush; \
        slm_io.is_open = cap_is_open; \
        slm_sess = shell_session_alloc(); \
        TEST_ASSERT_NOT_NULL(slm_sess); \
        slm_sess->io = &slm_io; \
        shell_session_bind(task_current(), slm_sess); \
    } while (0)

static void slm_prompt_teardown(void)
{
    rust_slm_test_script(NULL, 0, 0);
    shell_session_unbind(task_current());
    slm_sess->io = NULL;
    shell_session_free(slm_sess);
    for (uint32_t i = 0; i < 4u; i++) {
        rust_slm_session_close(i);
    }
    rust_slm_test_reset();
}

#define SCRIPT(looping, ...) \
    do { \
        static const uint32_t ids_[] = { __VA_ARGS__ }; \
        TEST_ASSERT_EQUAL_INT(0, rust_slm_test_script( \
            ids_, sizeof(ids_) / sizeof(ids_[0]), (looping))); \
    } while (0)

/* Session 0, greedy so the scripted peak wins, enough ctx for ChatML. */
#define LAUNCH_GREEDY() \
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm launch 0 --sampler greedy --ctx 256"))

/* ---- slm launch ---- */

static void test_slm_launch_gpu_cpu_is_stored_and_reported(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("slm launch 0 --ctx 8 --gpu cpu --threads 3"));

    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info(0, &info));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_CPU, info.gpu_mode);
    TEST_ASSERT_EQUAL_UINT32(3u, info.threads);
    TEST_ASSERT_EQUAL_UINT32(0u, info.gpu_ops);

    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "backend=CPU(--gpu cpu)"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "threads=3(stored; forward is single-threaded)"));
    TEST_ASSERT_NULL(strstr(cap.buf, "M5.3.2"));
    slm_prompt_teardown();
}

static void test_slm_launch_defaults_to_gpu_auto(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm launch 0 --ctx 8"));
    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info(0, &info));
    TEST_ASSERT_EQUAL_UINT32(SLM_GPU_AUTO, info.gpu_mode);
    TEST_ASSERT_EQUAL_UINT32(4u, info.threads);
    TEST_ASSERT_EQUAL_UINT32(rust_slm_gpu_tier_ops(), info.gpu_ops);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "backend="));
    slm_prompt_teardown();
}

static void test_slm_launch_rejects_bad_gpu_and_threads(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm launch 0 --ctx 8 --gpu maybe"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm launch 0 --ctx 8 --threads 0"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm launch 0 --ctx 8 --threads x"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm launch 0 --ctx 8 --threads 65"));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_stats(0, &st));
    slm_prompt_teardown();
}

static void test_slm_launch_gpu_on_requires_a_gpu_tier(void)
{
    SLM_PROMPT_SETUP();
    int rc = shell_execute("slm launch 0 --ctx 8 --gpu on");
    if (rust_slm_gpu_tier_ops() == 0) {
        TEST_ASSERT_EQUAL_INT(-1, rc);
        TEST_ASSERT_NOT_NULL(strstr(cap.buf, "no op has a GPU tier"));
    } else {
        TEST_ASSERT_EQUAL_INT(0, rc);
    }
    slm_prompt_teardown();
}

/* ---- slm prompt: text and options ---- */

/* The old path split on spaces, cut at SHELL_MAX_ARGS and kept the opening
 * quote. With the fixture's one-token-per-byte vocab, tokens_in is the
 * exact length of what reached the tokenizer. */
static void test_slm_prompt_long_quoted_text_arrives_whole(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, CLI_IM_END);
#define LONG_TEXT "one two three four five six seven eight nine ten " \
                  "eleven twelve thirteen fourteen fifteen sixteen seventeen"
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw \"" LONG_TEXT "\""));

    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    TEST_ASSERT_EQUAL_UINT32(sizeof(LONG_TEXT) - 1u, st.tokens_in);
    slm_prompt_teardown();
}

static void test_slm_prompt_too_many_unquoted_words_is_an_error(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(-1, shell_execute(
        "slm prompt 0 --raw a b c d e f g h i j k l m n o p"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "Too many arguments"));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    TEST_ASSERT_EQUAL_UINT32(0u, st.prompts_completed);
    slm_prompt_teardown();
}

/* Default: the Qwen ChatML template. --raw: the text alone, with specials
 * parsed to single ids. */
static void test_slm_prompt_renders_chatml_by_default(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 hi"));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    /* <|im_start|>system\n<68-byte Qwen system prompt><|im_end|>\n
     * <|im_start|>user\nhi<|im_end|>\n<|im_start|>assistant\n */
    uint32_t bytes = 7u + 68u + 1u + 5u + 2u + 1u + 10u;
    TEST_ASSERT_EQUAL_UINT32(bytes + 3u + 2u, st.tokens_in);
    slm_prompt_teardown();
}

static void test_slm_prompt_raw_parses_specials(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("slm prompt 0 --raw \"<|im_start|>hi<|im_end|>\""));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    TEST_ASSERT_EQUAL_UINT32(4u, st.tokens_in);
    slm_prompt_teardown();
}

static void test_slm_prompt_unescapes_newlines(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw \"a\\nb\""));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    TEST_ASSERT_EQUAL_UINT32(3u, st.tokens_in);
    slm_prompt_teardown();
}

static void test_slm_prompt_max_option_both_positions(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();

    SCRIPT(0, 'a', 'b', 'c', 'd');
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw --max 2 hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "ab\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=max"));

    cap_clear();
    SCRIPT(0, 'x', 'y', 'z');
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 \"hi there\" --raw --max 1"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "x\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=1 "));

    /* `--` makes dashes text. */
    cap_clear();
    SCRIPT(0, CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw -- --max"));
    SlmStatsC st;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_stats(0, &st));
    TEST_ASSERT_EQUAL_UINT32(2u + 8u + 5u, st.tokens_in); /* hi + "hi there" + --max */

    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm prompt 0 --bogus hi"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm prompt 0 --max"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm prompt 0 --raw"));
    slm_prompt_teardown();
}

/* ---- EOS ---- */

/* Id 2 was the hard-coded EOS: it must generate on here, and the id the GGUF
 * names must stop generation. */
static void test_slm_prompt_stops_on_gguf_eos_not_two(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', 2, 'b', CLI_EOS, 'c');
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "a\x02" "b\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=3 "));
    slm_prompt_teardown();
}

static void test_slm_prompt_stops_on_named_end_tokens(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', CLI_IM_END, 'b');
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=1 "));

    cap_clear();
    SCRIPT(0, 'c', CLI_ENDOFTEXT, 'd');
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=1 "));
    slm_prompt_teardown();
}

/* ---- Streaming ---- */

static void test_slm_prompt_flushes_per_token_and_keeps_utf8_whole(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    /* U+4F60 = E4 BD A0, one byte per token, between two ASCII tokens. */
    SCRIPT(0, 'a', 0xE4, 0xBD, 0xA0, 'b', CLI_IM_END);
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));

    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "a\xE4\xBD\xA0" "b\r\n"));
    TEST_ASSERT_NULL(strstr(cap.buf, "\xEF\xBF\xBD"));
    /* a, the completed character, b. */
    TEST_ASSERT_EQUAL_UINT32(3u, cap.flushes);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.bad_flushes);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=5 "));
    slm_prompt_teardown();
}

static void test_slm_prompt_truncated_utf8_prints_replacement_char(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', 0xE4, CLI_IM_END);
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "a\xEF\xBF\xBD\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=2 "));
    slm_prompt_teardown();
}

/* ---- Stopping ---- */

static void test_slm_prompt_ctrl_c_stops_and_session_takes_next_prompt(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(1, 'a');
    cap.ctrl_c_at_try = 20u;
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw --max 5000 hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "^C\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=stopped"));
    TEST_ASSERT_NULL(strstr(cap.buf, "tokens=5000"));

    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info(0, &info));
    TEST_ASSERT_EQUAL_UINT32(1u, info.stopped);
    TEST_ASSERT_EQUAL_UINT32(0u, info.busy);

    /* No `slm reset` needed. */
    cap_clear();
    SCRIPT(0, 'z', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "z\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_session_info(0, &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.stopped);
    slm_prompt_teardown();
}

static void test_slm_prompt_ctrl_c_interrupts_prefill(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(1, 'a');
    cap.ctrl_c_at_try = 2u;
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw abcdefgh"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=stopped"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "tokens=0 "));
    slm_prompt_teardown();
}

static void test_slm_prompt_keeps_typeahead_for_the_next_command(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', CLI_IM_END);
    cap.rx = "ls\r";
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));
    TEST_ASSERT_EQUAL_UINT32(3u, slm_sess->read_prefetch_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp(slm_sess->read_prefetch, "ls\r", 3));
    slm_prompt_teardown();
}

/* A second shell session, as a second task. While the first runs a prompt
 * that would otherwise produce 20000 tokens, each verb must answer. The old
 * code held the shell mutex and the session lock for the whole decode, so
 * every call here would have waited for the prompt to end by itself. */
static volatile int  second_stats_rc;
static volatile int  second_close_rc;
static volatile int  second_reset_rc;
static volatile int  second_unload_rc;
static volatile int  second_stop_rc;
static volatile bool second_saw_busy;
static volatile bool second_done;

static void second_session_entry(void *arg)
{
    (void)arg;
    while (!cap.saw_output) {
        yield();
    }
    SlmSessionInfoC info;
    if (rust_slm_session_info(0, &info) == 0) {
        second_saw_busy = info.busy != 0;
    }
    second_stats_rc  = shell_execute("slm stats 0");
    second_close_rc  = shell_execute("slm close 0");
    second_reset_rc  = shell_execute("slm reset 0");
    second_unload_rc = shell_execute("slm unload 0");
    second_stop_rc   = shell_execute("slm stop 0");
    second_done = true;
}

static void test_slm_stop_from_second_session_during_prompt(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(1, 'a');
    cap_clear();
    second_stats_rc = second_close_rc = second_reset_rc = -99;
    second_unload_rc = second_stop_rc = -99;
    second_saw_busy = false;
    second_done = false;

    struct task *t = task_create("slm_second", second_session_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    scheduler_add_task_to_cpu(t, task_current()->assigned_cpu);

    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw --max 20000 hi"));
    TEST_ASSERT_NULL(strstr(cap.buf, "stop=max"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=stopped"));

    for (int i = 0; i < 100000 && !second_done; i++) {
        yield();
    }
    TEST_ASSERT_TRUE(second_done);
    TEST_ASSERT_TRUE(second_saw_busy);
    TEST_ASSERT_EQUAL_INT(0, second_stats_rc);
    TEST_ASSERT_EQUAL_INT(-1, second_close_rc);
    TEST_ASSERT_EQUAL_INT(-1, second_reset_rc);
    TEST_ASSERT_EQUAL_INT(-1, second_unload_rc);
    TEST_ASSERT_EQUAL_INT(0, second_stop_rc);

    /* Everything refused above still works now it's idle. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm reset 0"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm close 0"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm unload 0"));
    slm_prompt_teardown();
}

static void test_slm_stop_idle_session_says_so(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm stop 0"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "no prompt running"));

    /* That stop must not cancel the next prompt. */
    cap_clear();
    SCRIPT(0, 'a', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=eos"));

    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stop 3"));
    slm_prompt_teardown();
}

/* ---- Context ---- */

static void test_slm_prompt_too_long_for_ctx_says_so(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm launch 0 --sampler greedy --ctx 4"));
    SCRIPT(0, 'a');
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw abcdefgh"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop=ctx-full"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "context full"));
    slm_prompt_teardown();
}

/* ---- Unknown session ---- */

static void test_slm_prompt_unknown_session_fails_cleanly(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm prompt 3 hi"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "no such session"));
    slm_prompt_teardown();
}

/* ============================================================================
 * Suite Runner
 * ========================================================================= */

int test_suite_slm_shell(void)
{
    UnityBegin("SLM Shell Verb Tests");

    RUN_TEST(test_slm_shell_load_invalid_path);
    RUN_TEST(test_slm_shell_status_with_no_models);
    RUN_TEST(test_slm_shell_dispatch_unknown_verb);
    RUN_TEST(test_slm_shell_no_verb_prints_usage);
    RUN_TEST(test_slm_shell_info_empty_slot);
    RUN_TEST(test_slm_shell_list_empty_registry);
    RUN_TEST(test_slm_shell_xload_missing_args);
    RUN_TEST(test_slm_shell_xload_missing_total);
    RUN_TEST(test_slm_shell_xload_invalid_total);
    RUN_TEST(test_slm_shell_xload_zero_total);
    RUN_TEST(test_slm_shell_xload_oversize_total);
    RUN_TEST(test_slm_launch_gpu_cpu_is_stored_and_reported);
    RUN_TEST(test_slm_launch_defaults_to_gpu_auto);
    RUN_TEST(test_slm_launch_rejects_bad_gpu_and_threads);
    RUN_TEST(test_slm_launch_gpu_on_requires_a_gpu_tier);
    RUN_TEST(test_slm_prompt_long_quoted_text_arrives_whole);
    RUN_TEST(test_slm_prompt_too_many_unquoted_words_is_an_error);
    RUN_TEST(test_slm_prompt_renders_chatml_by_default);
    RUN_TEST(test_slm_prompt_raw_parses_specials);
    RUN_TEST(test_slm_prompt_unescapes_newlines);
    RUN_TEST(test_slm_prompt_max_option_both_positions);
    RUN_TEST(test_slm_prompt_stops_on_gguf_eos_not_two);
    RUN_TEST(test_slm_prompt_stops_on_named_end_tokens);
    RUN_TEST(test_slm_prompt_flushes_per_token_and_keeps_utf8_whole);
    RUN_TEST(test_slm_prompt_truncated_utf8_prints_replacement_char);
    RUN_TEST(test_slm_prompt_ctrl_c_stops_and_session_takes_next_prompt);
    RUN_TEST(test_slm_prompt_ctrl_c_interrupts_prefill);
    RUN_TEST(test_slm_prompt_keeps_typeahead_for_the_next_command);
    RUN_TEST(test_slm_stop_from_second_session_during_prompt);
    RUN_TEST(test_slm_stop_idle_session_says_so);
    RUN_TEST(test_slm_prompt_too_long_for_ctx_says_so);
    RUN_TEST(test_slm_prompt_unknown_session_fails_cleanly);

    return (int)UnityEnd();
}
