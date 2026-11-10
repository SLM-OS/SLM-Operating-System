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
#include "../include/smp.h"
#include "../include/timer.h"
#include "../include/uart.h"

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

/* QEMU has no channel, so --gpu on has nothing to run on and says so, and
 * no session is left behind. */
static void test_slm_launch_gpu_on_requires_a_program(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm launch 0 --ctx 8 --gpu on"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "no GPU program"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "UEFI-direct"));
    SlmSessionInfoC info;
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_session_info(0, &info));
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

/* ---- summary line ---- */

/* total is ttft plus decode. It printed decode alone, so a CPU prompt, whose
 * prefill is most of the time, showed ttft=73100 ms total=7331 ms. */
static void test_slm_prompt_summary_total_includes_ttft(void)
{
    SLM_PROMPT_SETUP();
    SlmStatsC st;
    memset(&st, 0, sizeof(st));
    st.last_ttft_ns = 73100ull * 1000000ull;
    st.last_decode_ns = 7331ull * 1000000ull;
    st.last_stop_reason = SLM_STOP_MAX_TOKENS;
    slm_prompt_print_summary(&st, 4, 12);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "[slm] ttft=73100 ms  decode="));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, " total=80431 ms tokens=4 bytes=12 stop=max"));

    /* And end to end: never below ttft. */
    cap_clear();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', 'b', CLI_EOS);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    const char *t = strstr(cap.buf, "ttft=");
    const char *tot = strstr(cap.buf, "total=");
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_NOT_NULL(tot);
    unsigned long ttft = 0, total = 0;
    for (t += 5; *t >= '0' && *t <= '9'; t++) {
        ttft = ttft * 10u + (unsigned long)(*t - '0');
    }
    for (tot += 6; *tot >= '0' && *tot <= '9'; tot++) {
        total = total * 10u + (unsigned long)(*tot - '0');
    }
    TEST_ASSERT_TRUE(total >= ttft);
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

/* ---- slm stats ---- */

static void test_slm_stats_no_session(void)
{
    SLM_PROMPT_SETUP();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "Usage: slm stats"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats x"));
    cap_clear();
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats 0"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "not open"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats 0 --profile"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats 0 --follow"));
    SlmTelemetryC t;
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_telemetry(0, 0, &t));
    TEST_ASSERT_EQUAL_INT(-1, rust_slm_telemetry_profile(0, 1));
    slm_prompt_teardown();
}

/* A session with no prompt yet shows zeros and no invented rate. */
static void test_slm_stats_before_any_prompt(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm stats 0 --verbose"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "prompts=0  tokens_in=0  tokens_out=0"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "ttft     : 0 ms"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "decode   : 0 tokens, 0 ms total, 0.0 tok/s"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "itl      : none yet"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "stop     : "));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats 0 --bogus"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("slm stats 0 --follow x"));
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm stats 0 --follow"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "no prompt running"));
    slm_prompt_teardown();
}

static void test_slm_stats_formats_every_section(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(0, 'a', 'b', 'c', CLI_IM_END);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw hi"));
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm stats 0 --verbose --profile --gpu --power"));
    static const char *const want[] = {
        "prompts=1  tokens_in=2  tokens_out=3",
        "ttft     : ", "prefill  : 2 tokens", "decode   : 3 tokens", "stop     : eos",
        "kv_used  : ", "peak mem : ", "ops/token: ", "oov      : 0 text pieces",
        "sampler retries: n/a",
        "itl      : min ", "tokens\r\n",
        "profile  : on, 0 tokens timed", "rolling  : last ",
        "clocks   : EMC", "op tiers :", "matmul-attn", "lm_head",
        "gpu busy : n/a (not on a GPU program)",
        "mma issue time / matmul wall (derived): n/a (no op at tier 1)",
        "tensor-core share of matmul+GQA MACs (derived): n/a (no op at tier 1)",
        "SM tensor-pipe counter: n/a",
        "power    : n/a", "temperatures only",
        "backend=", "threads  : ",
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        if (!strstr(cap.buf, want[i])) {
            TEST_FAIL_MESSAGE(want[i]);
        }
    }
    /* --profile stays armed until turned off, and the off form says so. */
    SlmTelemetryC t;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_telemetry(0, 0, &t));
    TEST_ASSERT_EQUAL_UINT32(1u, t.profile);
    cap_clear();
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm stats 0 --profile off"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "profile  : off"));
    TEST_ASSERT_EQUAL_INT(0, rust_slm_telemetry(0, 0, &t));
    TEST_ASSERT_EQUAL_UINT32(0u, t.profile);
    slm_prompt_teardown();
}

/* GPU time is split at the first token, so the decode figure can be read
 * against the decode wall (it once included the prefill step and exceeded it). */
static void test_slm_stats_gpu_busy_splits_prefill(void)
{
    SLM_PROMPT_SETUP();
    SlmTelemetryC t;
    memset(&t, 0, sizeof(t));
    t.decode_ns = 1210000000ull;
    t.gpu_decode_steps = 62;
    t.gpu_decode_ns = 1200010000ull;
    t.gpu_prefill_steps = 1;
    t.gpu_prefill_ns = 28680000ull;
    t.gpu_last_ns = 20290000ull;
    t.gpu_evict_us = 63u * 52u;
    cap_clear();
    slm_stats_print_gpu_busy(&t);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf,
        "gpu busy : decode 1200.01 ms over 62 steps (decode wall 1210 ms), "
        "prefill 28.68 ms over 1 steps"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "last step 20.29 ms; evict 52 us per step"));
    slm_prompt_teardown();
}

/* Decode DRAM bandwidth (ADR 0005): plan bytes per forward pass times decode
 * forward passes (itl_count, not emitted tokens), against 32 bytes per EMC
 * clock. 982 MB x 256 passes in 5.16 s at 3199 MHz is 48.7 GB/s, 47.59 % of
 * 102.3 GB/s; with 257 emitted tokens the figure must not change. */
static void test_slm_stats_dram_bw_against_peak(void)
{
    SLM_PROMPT_SETUP();
    SlmTelemetryC t;
    memset(&t, 0, sizeof(t));
    t.weight_bytes = 979000000ull;
    t.kv_read_bytes = 3000000ull;
    t.decode_tokens = 257u;
    t.itl_count = 256u;
    t.decode_ns = 5160000000ull;
    cap_clear();
    slm_stats_print_dram_bw(&t, 3199000000ull);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf,
        "dram bw  : decode 48.7 GB/s = 47.59 % of 102.3 GB/s LPDDR5 peak"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "target llama.cpp parity 41.27 %: met"));
    /* 30 GB/s is below llama.cpp's fraction. */
    t.decode_ns = 8380000000ull;
    cap_clear();
    slm_stats_print_dram_bw(&t, 3199000000ull);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "29.9 GB/s = 29.30 %"));
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, ": not met"));
    memset(&t, 0, sizeof(t));
    cap_clear();
    slm_stats_print_dram_bw(&t, 3199000000ull);
    TEST_ASSERT_NOT_NULL(strstr(cap.buf, "dram bw  : decode n/a"));
    slm_prompt_teardown();
}

/* A figure read in a second session during a prompt is live: it advances
 * between two reads, and neither a plain read nor --follow stops the prompt. */
static volatile uint64_t live_first;
static volatile uint64_t live_second;
static volatile uint32_t live_running;
static volatile int      live_stats_rc;
static volatile int      live_follow_rc;
static volatile bool     live_saw_header;
static volatile bool     live_done;

/* #1202 instrumentation (coherency run 2, C1): the helper records its CPU
 * and a CNTPCT stamp at each stage, so a timeout shows how far it got and
 * when. Stages: entry, saw output, stats done, 2000 yields done, stop done,
 * follow done, flag set. */
enum { LIVE_ST_ENTRY, LIVE_ST_SAW, LIVE_ST_STATS, LIVE_ST_Y2000, LIVE_ST_STOP,
       LIVE_ST_FOLLOW, LIVE_ST_DONE, LIVE_ST_N };
static volatile uint64_t live_st_ts[LIVE_ST_N];
static volatile uint32_t live_st_cpu[LIVE_ST_N];

static inline void live_stage(int st)
{
    live_st_ts[st] = timer_get_count();
    live_st_cpu[st] = cpu_id();
}

static void live_session_entry(void *arg)
{
    (void)arg;
    live_stage(LIVE_ST_ENTRY);
    while (!cap.saw_output) {
        yield();
    }
    live_stage(LIVE_ST_SAW);
    SlmTelemetryC t;
    if (rust_slm_telemetry(0, 0, &t) == 0) {
        live_first = t.decode_tokens;
        live_running = t.running;
    }
    /* Same output sink as the prompt, whose stream fills the capture quickly,
     * so make room first. */
    shell_session_bind(task_current(), slm_sess);
    cap_clear();
    live_stats_rc = shell_execute("slm stats 0 --verbose --profile --gpu");
    live_saw_header = strstr(cap.buf, "[prompt running: live]") != NULL;
    live_stage(LIVE_ST_STATS);
    for (int i = 0; i < 2000; i++) {
        yield();
    }
    live_stage(LIVE_ST_Y2000);
    if (rust_slm_telemetry(0, 0, &t) == 0) {
        live_second = t.decode_tokens;
    }
    (void)shell_execute("slm stop 0");
    live_stage(LIVE_ST_STOP);
    live_follow_rc = shell_execute("slm stats 0 --follow 1");
    live_stage(LIVE_ST_FOLLOW);
    shell_session_unbind(task_current());
    live_stage(LIVE_ST_DONE);
    live_done = true;
}

/* Microseconds from CNTPCT stamp a to b; -1 when b was never written (0)
 * or precedes a. */
static long long c1_us(uint64_t a, uint64_t b)
{
    uint64_t f = timer_get_frequency();
    if (b == 0 || b < a || f == 0) {
        return -1;
    }
    return (long long)((b - a) * 1000000ull / f);
}

static void test_slm_stats_live_from_second_session(void)
{
    SLM_PROMPT_SETUP();
    LAUNCH_GREEDY();
    SCRIPT(1, 'a');
    cap_clear();
    live_first = live_second = 0;
    live_running = 0;
    live_stats_rc = live_follow_rc = -99;
    live_saw_header = false;
    live_done = false;
    for (int st = 0; st < LIVE_ST_N; st++) {
        live_st_ts[st] = 0;
        live_st_cpu[st] = 0xFFu;
    }

    struct task *t = task_create("slm_live", live_session_entry, NULL);
    TEST_ASSERT_NOT_NULL(t);
    uint32_t waiter_cpu0 = cpu_id();
    uint64_t t_dispatch = timer_get_count();
    scheduler_add_task_to_cpu(t, task_current()->assigned_cpu);
    /* The looping script fills the capture buffer, so the stop reason is
     * checked from the telemetry below, not the text. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("slm prompt 0 --raw --max 200000 hi"));
    uint64_t t_loop = timer_get_count();
    int yields = 0;
    for (int i = 0; i < 100000 && !live_done; i++) {
        yield();
        yields++;
    }
    uint64_t t_end = timer_get_count();
    bool done_at_bound = live_done;
    uint32_t waiter_cpu1 = cpu_id();
    uint32_t h_state = (uint32_t)t->state;
    uint32_t h_assigned = t->assigned_cpu;
    uint64_t h_switches = t->switches;

    /* #1202 instrumentation: on a timeout, keep yielding for up to 1 s of
     * wall clock and record whether the flag is set 1, 10 and 100 ms after
     * expiry (load-acquire), and when it first turned true. */
    int rp[3] = { -1, -1, -1 };
    long long first_true_us = -1;
    if (!done_at_bound) {
        static const long long marks_us[3] = { 1000, 10000, 100000 };
        int next = 0;
        for (;;) {
            long long dt = c1_us(t_end, timer_get_count());
            bool v = __atomic_load_n(&live_done, __ATOMIC_ACQUIRE);
            if (v && first_true_us < 0) {
                first_true_us = dt;
            }
            while (next < 3 && dt >= marks_us[next]) {
                rp[next++] = v ? 1 : 0;
            }
            if (next >= 3 && (first_true_us >= 0 || dt >= 1000000)) {
                break;
            }
            yield();
        }
    }
    /* Cost of a bare yield here, after the wait: 1000 yields timed. */
    uint64_t tb0 = timer_get_count();
    for (int i = 0; i < 1000; i++) {
        yield();
    }
    long long bare1000_us = c1_us(tb0, timer_get_count());
    long long loop_us = c1_us(t_loop, t_end);
    uart_printf("[C1-1202] live %s waiter_cpu=%u->%u helper_cpu=%u..%u "
                "assigned=%u state=%u switches=%llu yields=%d/100000 "
                "loop_us=%lld dispatch_to_end_us=%lld ns_per_yield=%lld "
                "bare_ns_per_yield=%lld\n",
                done_at_bound ? "PASS" : "FAIL",
                (unsigned)waiter_cpu0, (unsigned)waiter_cpu1,
                (unsigned)live_st_cpu[LIVE_ST_ENTRY],
                (unsigned)live_st_cpu[LIVE_ST_DONE],
                (unsigned)h_assigned, (unsigned)h_state,
                (unsigned long long)h_switches, yields, loop_us,
                c1_us(t_dispatch, t_end),
                yields > 0 ? loop_us * 1000 / yields : -1,
                bare1000_us >= 0 ? bare1000_us : -1);
    uart_printf("[C1-1202] live stages_us(from dispatch; loop at %lld, "
                "expiry at %lld) entry=%lld@%u saw=%lld@%u stats=%lld@%u "
                "y2000=%lld@%u stop=%lld@%u follow=%lld@%u done=%lld@%u\n",
                c1_us(t_dispatch, t_loop), c1_us(t_dispatch, t_end),
                c1_us(t_dispatch, live_st_ts[LIVE_ST_ENTRY]),
                (unsigned)live_st_cpu[LIVE_ST_ENTRY],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_SAW]),
                (unsigned)live_st_cpu[LIVE_ST_SAW],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_STATS]),
                (unsigned)live_st_cpu[LIVE_ST_STATS],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_Y2000]),
                (unsigned)live_st_cpu[LIVE_ST_Y2000],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_STOP]),
                (unsigned)live_st_cpu[LIVE_ST_STOP],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_FOLLOW]),
                (unsigned)live_st_cpu[LIVE_ST_FOLLOW],
                c1_us(t_dispatch, live_st_ts[LIVE_ST_DONE]),
                (unsigned)live_st_cpu[LIVE_ST_DONE]);
    if (!done_at_bound) {
        uart_printf("[C1-1202] live FAIL repoll 1ms=%d 10ms=%d 100ms=%d "
                    "first_true_after_expiry_us=%lld stats_rc=%d follow_rc=%d "
                    "first=%llu second=%llu running=%u saw_header=%d\n",
                    rp[0], rp[1], rp[2], first_true_us,
                    (int)live_stats_rc, (int)live_follow_rc,
                    (unsigned long long)live_first,
                    (unsigned long long)live_second,
                    (unsigned)live_running, live_saw_header ? 1 : 0);
    }
    TEST_ASSERT_TRUE(done_at_bound);
    TEST_ASSERT_EQUAL_UINT32(1u, live_running);
    TEST_ASSERT_EQUAL_INT(0, live_stats_rc);
    TEST_ASSERT_EQUAL_INT(0, live_follow_rc);
    TEST_ASSERT_TRUE(live_second > live_first);
    TEST_ASSERT_TRUE(live_saw_header);
    /* The prompt's own figures agree with what the second session saw. */
    SlmTelemetryC s;
    TEST_ASSERT_EQUAL_INT(0, rust_slm_telemetry(0, 0, &s));
    TEST_ASSERT_EQUAL_UINT32(0u, s.running);
    TEST_ASSERT_TRUE(s.decode_tokens >= live_second);
    TEST_ASSERT_EQUAL_UINT32(SLM_STOP_USER, s.stop_reason);
    slm_prompt_teardown();
}

/* The whole per-token cost with stats disabled, on this CPU under TCG, against
 * 0.1 percent of the 23 ms bar. rust_slm_telemetry_token_cost_ns uses a scratch
 * block; the same check on the host is in telemetry.rs, with the no-heap one. */
static void test_slm_stats_disabled_cost_is_not_measurable(void)
{
    uint64_t ns = rust_slm_telemetry_token_cost_ns();
    TEST_ASSERT_TRUE(ns < 23000u);
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
    RUN_TEST(test_slm_launch_gpu_on_requires_a_program);
    RUN_TEST(test_slm_prompt_long_quoted_text_arrives_whole);
    RUN_TEST(test_slm_prompt_too_many_unquoted_words_is_an_error);
    RUN_TEST(test_slm_prompt_renders_chatml_by_default);
    RUN_TEST(test_slm_prompt_raw_parses_specials);
    RUN_TEST(test_slm_prompt_unescapes_newlines);
    RUN_TEST(test_slm_prompt_max_option_both_positions);
    RUN_TEST(test_slm_prompt_summary_total_includes_ttft);
    RUN_TEST(test_slm_prompt_stops_on_gguf_eos_not_two);
    RUN_TEST(test_slm_prompt_stops_on_named_end_tokens);
    RUN_TEST(test_slm_prompt_flushes_per_token_and_keeps_utf8_whole);
    RUN_TEST(test_slm_prompt_truncated_utf8_prints_replacement_char);
    RUN_TEST(test_slm_prompt_ctrl_c_stops_and_session_takes_next_prompt);
    RUN_TEST(test_slm_prompt_ctrl_c_interrupts_prefill);
    RUN_TEST(test_slm_prompt_keeps_typeahead_for_the_next_command);
    RUN_TEST(test_slm_stop_from_second_session_during_prompt);
    RUN_TEST(test_slm_stats_no_session);
    RUN_TEST(test_slm_stats_before_any_prompt);
    RUN_TEST(test_slm_stats_formats_every_section);
    RUN_TEST(test_slm_stats_gpu_busy_splits_prefill);
    RUN_TEST(test_slm_stats_dram_bw_against_peak);
    RUN_TEST(test_slm_stats_live_from_second_session);
    RUN_TEST(test_slm_stats_disabled_cost_is_not_measurable);
    RUN_TEST(test_slm_stop_idle_session_says_so);
    RUN_TEST(test_slm_prompt_too_long_for_ctx_says_so);
    RUN_TEST(test_slm_prompt_unknown_session_fails_cleanly);

    return (int)UnityEnd();
}
