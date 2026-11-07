/*
 * slm_runner.c — the `slm-runner` component and the `slm ask` bus client.
 *
 * The runner subscribes to /slm/prompt and /slm/stop and answers each prompt
 * as one fresh conversation through the session FFI: one /slm/token message
 * per generated token that has bytes, then one /slm/done with the counters.
 * docs/tutorials/slm-component.md has the message formats and lifecycle.
 *
 * Delivery: tokens go out with msg_router_publish_reliable, so a slow reader
 * holds the decoder back instead of losing tokens. A reader that stalls for
 * SLM_RUNNER_PUBLISH_TIMEOUT_MS costs that reader the token; /slm/done counts
 * it in dropped=, and the rest of that prompt waits only
 * SLM_RUNNER_STALLED_TIMEOUT_MS per token, so a dead subscriber can't wedge
 * the service.
 *
 * The runner runs in-kernel at the shell's priority. It stays up until `slm
 * runner stop`, and it doesn't need a model to start: it picks one per prompt.
 *
 * NOTE on -mgeneral-regs-only: rust_slm_session_open takes f32 by value
 * (AAPCS64 V0/V1), which the kernel's default FP-disable forbids the C
 * compiler from materialising. CMakeLists.txt compiles this file without
 * -mgeneral-regs-only, the same isolation as kernel/src/slm_shell.c.
 */

#include "component.h"
#include "slm_ffi.h"
#include "slm_runner.h"
#include "shell.h"
#include "string.h"
#include "task.h"
#include "timer.h"
#include "uart.h"
#include "sched.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * External symbols (msg_router)
 * ============================================================================ */

extern int  msg_router_subscribe(const char *topic_name, int component_idx);
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);
extern int  msg_router_publish_nowait(const uint8_t *topic_name, const uint8_t *data);

/* ============================================================================
 * State
 * ============================================================================ */

/* A subscriber gets this long to make room for each message. */
#define SLM_RUNNER_PUBLISH_TIMEOUT_MS 30000u
/* ...and this long per message once it has already missed that once. */
#define SLM_RUNNER_STALLED_TIMEOUT_MS 1000u
/* Idle poll of the mailbox. A sleep, not a yield loop: an idle service
 * shouldn't take every other time slice on CPU 0 from the shell. */
#define SLM_RUNNER_IDLE_POLL_MS 10u
/* How long `slm ask` waits for the runner to take the prompt. */
#define SLM_ASK_PUBLISH_TIMEOUT_MS 5000u
/* Covers the session open: a KV cache, and under --gpu on a GPU program. */
#define SLM_RUNNER_START_TIMEOUT_MS 60000u
/* Past a publish stalled on a dead subscriber, a slow forward step and more. */
#define SLM_RUNNER_STOP_TIMEOUT_MS 60000u

static const slm_runner_config_t SLM_RUNNER_DEFAULT_CONFIG = {
    .model_handle   = SLM_RUNNER_MODEL_LATEST,
    .max_ctx        = 4096u,
    .sampler_kind   = SLM_SAMPLER_TOP_K_TOP_P,
    .temperature    = 0.7f,
    .top_k          = 40u,
    .top_p          = 0.9f,
    .seed           = 1u,
    .gpu_mode       = SLM_GPU_AUTO,
    .threads        = 4u,
    .max_new_tokens = 256u,
    .prompt_flags   = 0u,
};

static slm_runner_config_t g_cfg = SLM_RUNNER_DEFAULT_CONFIG;

/* Written by the runner task, read by shell tasks. */
static struct {
    uint32_t running;          /* atomics */
    uint32_t exit_requested;
    uint32_t busy;
    volatile int32_t  comp_idx;
    volatile int32_t  session;
    volatile int32_t  model;
    volatile uint32_t prompts;
    volatile uint32_t dropped;
    uint64_t model_seq;
    uint32_t publish_timeout_ms;
} g_runner = {
    .comp_idx = -1,
    .session = -1,
    .model = -1,
    .publish_timeout_ms = SLM_RUNNER_PUBLISH_TIMEOUT_MS,
};

/* The prompt being handled. There's one runner. */
static char g_prompt[MSG_ROUTER_MAX_PAYLOAD + 1];

const char *slm_stop_reason_name(uint32_t reason)
{
    switch (reason) {
    case SLM_STOP_EOS:          return "eos";
    case SLM_STOP_MAX_TOKENS:   return "max";
    case SLM_STOP_USER:         return "stopped";
    case SLM_STOP_CALLBACK:     return "callback";
    case SLM_STOP_CONTEXT_FULL: return "ctx-full";
    case SLM_STOP_NO_LOGITS:    return "no-logits";
    case SLM_STOP_EMPTY_PROMPT: return "empty";
    default:                    return "none";
    }
}

/* The flag says the task got started. A task killed from outside never clears
 * it, but its cleanup does move the component on from RUNNING. */
static bool slm_runner_alive(void)
{
    if (!__atomic_load_n(&g_runner.running, __ATOMIC_ACQUIRE)) {
        return false;
    }
    component_info_t info;
    int32_t idx = g_runner.comp_idx;
    return idx >= 0 && component_get_info((uint32_t)idx, &info) == 0 &&
           info.state == COMPONENT_RUNNING;
}

/* ============================================================================
 * Token stream
 * ============================================================================ */

/* Per-prompt context handed to the callbacks. */
typedef struct {
    int      comp_idx;        /* -1: called directly, nobody to poll for */
    uint32_t tokens;          /* model tokens */
    uint32_t msgs;            /* /slm/token messages published */
    uint32_t dropped;         /* of those, lost to a stalled subscriber */
    uint32_t timeout_ms;
} slm_runner_token_ctx_t;

static void slm_runner_publish_tracked(slm_runner_token_ctx_t *ctx,
                                       const char *topic, const char *text)
{
    msg_publish_stats_t st = {0};
    msg_router_publish_reliable((const uint8_t *)topic, (const uint8_t *)text,
                                ctx->timeout_ms, &st);
    if (st.dropped > 0) {
        ctx->dropped += (uint32_t)st.dropped;
    }
    if ((st.dropped > 0 || st.unacked > 0) &&
        ctx->timeout_ms > SLM_RUNNER_STALLED_TIMEOUT_MS) {
        ctx->timeout_ms = SLM_RUNNER_STALLED_TIMEOUT_MS;
    }
}

/*
 * Publish bytes as NUL-terminated messages, split to fit and never inside a
 * UTF-8 sequence. Every byte goes out except NULs, which a C-string bus can't
 * carry.
 */
static void slm_runner_publish_token_bytes(slm_runner_token_ctx_t *ctx,
                                           const uint8_t *bytes, size_t len)
{
    char buf[MSG_ROUTER_MAX_PAYLOAD + 1];

    while (len > 0) {
        size_t n = len;
        if (n > MSG_ROUTER_MAX_PAYLOAD) {
            n = MSG_ROUTER_MAX_PAYLOAD;
            /* bytes[n] starts the next message: move back until it's not a
             * continuation byte. A UTF-8 sequence is at most 4 bytes, so a
             * longer run is malformed and gets split where it stands. */
            size_t back = 0;
            while (back < 4 && (bytes[n - back] & 0xC0) == 0x80) {
                back++;
            }
            if (back < 4) {
                n -= back;
            }
        }
        size_t out = 0;
        for (size_t i = 0; i < n; i++) {
            if (bytes[i] != '\0') {
                buf[out++] = (char)bytes[i];
            }
        }
        buf[out] = '\0';
        if (out > 0) {
            slm_runner_publish_tracked(ctx, SLM_TOPIC_TOKEN, buf);
            ctx->msgs++;
        }
        bytes += n;
        len -= n;
    }
}

/* Test hook: publish one token's bytes as the callback would. */
uint32_t slm_runner_test_publish_bytes(const uint8_t *bytes, size_t len)
{
    slm_runner_token_ctx_t ctx = {
        .comp_idx = -1,
        .timeout_ms = g_runner.publish_timeout_ms,
    };
    slm_runner_publish_token_bytes(&ctx, bytes, len);
    return ctx.msgs;
}

/* Test hook: how long a subscriber gets to make room. 0 restores the default. */
void slm_runner_test_set_publish_timeout(uint32_t ms)
{
    g_runner.publish_timeout_ms = ms != 0 ? ms : SLM_RUNNER_PUBLISH_TIMEOUT_MS;
}

static int32_t slm_runner_token_cb(void *user, uint32_t token_id,
                                   const uint8_t *bytes, size_t bytes_len)
{
    slm_runner_token_ctx_t *ctx = (slm_runner_token_ctx_t *)user;

    /* A token that ended inside a UTF-8 sequence, or a special, has no bytes
     * yet. Subscribers get no empty message; it still counts as a token. */
    slm_runner_publish_token_bytes(ctx, bytes, bytes_len);
    if (token_id != SLM_TOKEN_ID_FLUSH) {
        ctx->tokens++;
    }
    return __atomic_load_n(&g_runner.exit_requested, __ATOMIC_ACQUIRE) ? 0 : 1;
}

/* Before every forward step: has someone asked this prompt to stop? Reads
 * /slm/stop alone, so a queued prompt stays queued. */
static int32_t slm_runner_poll_cb(void *user)
{
    slm_runner_token_ctx_t *ctx = (slm_runner_token_ctx_t *)user;
    if (ctx->comp_idx >= 0 &&
        msg_router_receive_topic(ctx->comp_idx, SLM_TOPIC_STOP) != NULL) {
        msg_router_ack(ctx->comp_idx);
        return 1;
    }
    return __atomic_load_n(&g_runner.exit_requested, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/* ============================================================================
 * Sessions
 * ============================================================================ */

/* Returns the session id, -1, or this when --gpu on has nothing to run on. */
#define SLM_RUNNER_NO_GPU_PROGRAM (-2)

static int slm_runner_open_session(uint32_t handle, uint32_t max_ctx)
{
    int32_t session = rust_slm_session_open(handle, max_ctx,
                                            g_cfg.sampler_kind,
                                            g_cfg.temperature,
                                            g_cfg.top_k,
                                            g_cfg.top_p,
                                            g_cfg.seed);
    if (session < 0) {
        uart_puts("[slm-runner] ERROR: rust_slm_session_open failed\r\n");
        return -1;
    }
    /* The same call `slm launch --gpu` makes. */
    int32_t crc = rust_slm_session_configure((uint32_t)session, g_cfg.gpu_mode,
                                             g_cfg.threads);
    if (crc == SLM_ERR_GPU_PROGRAM) {
        SlmSessionGpuC g;
        uart_printf("[slm-runner] ERROR: --gpu on, but no GPU program: %s\r\n",
                    rust_slm_session_gpu_info((uint32_t)session, &g) == 0 ?
                    g.note : "unknown");
        rust_slm_session_close((uint32_t)session);
        return SLM_RUNNER_NO_GPU_PROGRAM;
    }
    if (crc != 0) {
        uart_puts("[slm-runner] ERROR: could not apply --gpu/--threads\r\n");
        rust_slm_session_close((uint32_t)session);
        return -1;
    }
    return (int)session;
}

/* Which model the configuration names right now. On failure sets *err. */
static int slm_runner_pick_model(uint32_t *handle, uint64_t *seq, const char **err)
{
    if (g_cfg.model_handle != SLM_RUNNER_MODEL_LATEST) {
        *handle = (uint32_t)g_cfg.model_handle;
        *seq = rust_slm_load_seq(*handle);
        if (*seq == 0) {
            *err = "bad-handle";
            return -1;
        }
        return 0;
    }
    if (rust_slm_latest_loaded(handle, seq) != 0) {
        *err = "no-model";
        return -1;
    }
    return 0;
}

/*
 * Open a session over the configured model with a caller-chosen `max_ctx`.
 * Returns the session id, or -1, logging why. The caller owns the session.
 * Unit tests pass a small `max_ctx` since their Rust heap is tiny.
 */
int slm_runner_setup_session_with_ctx(uint32_t max_ctx)
{
    uint32_t handle = 0;
    uint64_t seq = 0;
    const char *err = NULL;
    if (slm_runner_pick_model(&handle, &seq, &err) != 0) {
        uart_printf("[slm-runner] ERROR: %s\r\n", err);
        uart_puts("[slm-runner] Load a model with: slm load <path> "
                  "or slm xload <name> <bytes>\r\n");
        return -1;
    }
    return slm_runner_open_session(handle, max_ctx);
}

int slm_runner_setup_session(void)
{
    return slm_runner_setup_session_with_ctx(g_cfg.max_ctx);
}

static void slm_runner_close_session(void)
{
    if (g_runner.session >= 0) {
        rust_slm_session_close((uint32_t)g_runner.session);
    }
    g_runner.session = -1;
    g_runner.model = -1;
}

/* Make sure the open session is on the model the configuration names now.
 * Reopens after a newer load or a reload of the same slot: a session can't
 * outlive its model's weights. */
static int slm_runner_ensure_session(const char **err)
{
    uint32_t handle = 0;
    uint64_t seq = 0;
    if (slm_runner_pick_model(&handle, &seq, err) != 0) {
        slm_runner_close_session();
        return -1;
    }
    if (g_runner.session >= 0 && g_runner.model == (int32_t)handle &&
        g_runner.model_seq == seq) {
        return 0;
    }
    slm_runner_close_session();
    int session = slm_runner_open_session(handle, g_cfg.max_ctx);
    if (session < 0) {
        *err = session == SLM_RUNNER_NO_GPU_PROGRAM ? "no-gpu-program" : "open-failed";
        return -1;
    }
    g_runner.session = session;
    g_runner.model = (int32_t)handle;
    g_runner.model_seq = seq;
    return 0;
}

/* ============================================================================
 * One prompt
 * ============================================================================ */

/*
 * Run a prompt through an open session, streaming tokens to /slm/token and
 * publishing /slm/done on every path. Returns what rust_slm_prompt_ex
 * returned: 0 if it ran, -1 on a decoder error, SLM_ERR_BUSY if a prompt
 * from `slm prompt` was in the way.
 */
static int slm_runner_run_prompt(uint32_t session_id, const char *prompt,
                                 size_t prompt_len, int comp_idx,
                                 uint32_t *tokens_emitted_out)
{
    slm_runner_token_ctx_t cb_ctx = {
        .comp_idx = comp_idx,
        .timeout_ms = g_runner.publish_timeout_ms,
    };

    int32_t rc = rust_slm_prompt_ex(session_id, (const uint8_t *)prompt,
                                    prompt_len, g_cfg.prompt_flags,
                                    g_cfg.max_new_tokens,
                                    slm_runner_token_cb, slm_runner_poll_cb,
                                    &cb_ctx);

    SlmStatsC stats = {0};
    if (rc != 0 || rust_slm_stats(session_id, &stats) != 0) {
        memset(&stats, 0, sizeof(stats));
    }

    g_runner.prompts = g_runner.prompts + 1u;
    g_runner.dropped = g_runner.dropped + cb_ctx.dropped;
    /* Before the publish: a client that sees /slm/done may ask again at once. */
    __atomic_store_n(&g_runner.busy, 0u, __ATOMIC_RELEASE);

    /* msgs= counts the /slm/token messages before this one, so a client can
     * tell it got them all. */
    char done[192];
    uart_snprintf(done, sizeof(done),
                  "rc=%d stop=%s tokens_in=%u tokens_out=%u msgs=%u dropped=%u "
                  "ttft_ms=%lu decode_ms=%lu session=%u prompts=%u",
                  (int)rc,
                  slm_stop_reason_name(stats.last_stop_reason),
                  stats.tokens_in,
                  cb_ctx.tokens,
                  cb_ctx.msgs,
                  cb_ctx.dropped,
                  (unsigned long)(stats.last_ttft_ns / 1000000ull),
                  (unsigned long)(stats.last_decode_ns / 1000000ull),
                  session_id,
                  (unsigned)g_runner.prompts);
    slm_runner_publish_tracked(&cb_ctx, SLM_TOPIC_DONE, done);

    if (tokens_emitted_out) {
        *tokens_emitted_out = cb_ctx.tokens;
    }
    return (int)rc;
}

/* Direct entry for tests: no bus input, nobody polled for /slm/stop. */
int slm_runner_handle_prompt(uint32_t session_id,
                             const char *prompt,
                             size_t prompt_len,
                             uint32_t *tokens_emitted_out)
{
    return slm_runner_run_prompt(session_id, prompt, prompt_len, -1,
                                 tokens_emitted_out);
}

/* A prompt off the bus: pick the model, start a fresh conversation, run it. */
static void slm_runner_serve_prompt(int comp_idx, const char *prompt, size_t prompt_len)
{
    const char *err = NULL;
    __atomic_store_n(&g_runner.busy, 1u, __ATOMIC_RELEASE);

    if (slm_runner_ensure_session(&err) != 0) {
        slm_runner_token_ctx_t ctx = {
            .comp_idx = comp_idx,
            .timeout_ms = g_runner.publish_timeout_ms,
        };
        char done[128];
        g_runner.prompts = g_runner.prompts + 1u;
        __atomic_store_n(&g_runner.busy, 0u, __ATOMIC_RELEASE);
        uart_printf("[slm-runner] prompt refused: %s\r\n", err);
        uart_snprintf(done, sizeof(done),
                      "rc=-1 err=%s stop=none tokens_in=0 tokens_out=0 msgs=0 "
                      "dropped=0 prompts=%u",
                      err, (unsigned)g_runner.prompts);
        slm_runner_publish_tracked(&ctx, SLM_TOPIC_DONE, done);
        return;
    }

    /* Each prompt is its own conversation. A shared session on a bus would
     * leak one client's text into another's reply, and fill up for good. */
    rust_slm_session_reset((uint32_t)g_runner.session);

    uint32_t tokens = 0;
    int rc = slm_runner_run_prompt((uint32_t)g_runner.session, prompt, prompt_len,
                                   comp_idx, &tokens);
    uart_printf("[slm-runner] prompt #%u rc=%d tokens=%u\r\n",
                (unsigned)g_runner.prompts, rc, tokens);
}

/* ============================================================================
 * Component entry point — registered in kernel/src/component_runtime.c's
 * builtin_components[] table.
 * ============================================================================ */

void slm_runner_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    if (msg_router_subscribe(SLM_TOPIC_PROMPT, comp_idx) != 0 ||
        msg_router_subscribe(SLM_TOPIC_STOP, comp_idx) != 0) {
        uart_puts("[slm-runner] ERROR: subscribe failed\r\n");
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }

    /* A task killed from outside left its fields behind. Its session is still
     * leased to the prompt it died in, and can't be closed. */
    g_runner.session = -1;
    g_runner.model = -1;
    g_runner.prompts = 0;
    g_runner.dropped = 0;
    g_runner.comp_idx = comp_idx;
    __atomic_store_n(&g_runner.busy, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_runner.exit_requested, 0u, __ATOMIC_RELEASE);

    /* Open a session now if there's a model, so a bad configuration or an
     * out-of-memory KV cache shows at start. Without one, keep going: the
     * model can come later and each prompt picks its own. Running is set
     * after, so `slm runner start` reports the session it got. */
    const char *err = NULL;
    if (slm_runner_ensure_session(&err) != 0) {
        uart_printf("[slm-runner] no session yet: %s\r\n", err);
    }
    __atomic_store_n(&g_runner.running, 1u, __ATOMIC_RELEASE);
    uart_printf("[slm-runner] Started (component %d, session %d, model %d), "
                "subscribed to " SLM_TOPIC_PROMPT "\r\n",
                comp_idx, (int)g_runner.session, (int)g_runner.model);

    while (!__atomic_load_n(&g_runner.exit_requested, __ATOMIC_ACQUIRE)) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        const char *data = msg_router_receive(comp_idx, topic);
        if (!data) {
            task_sleep_ms(SLM_RUNNER_IDLE_POLL_MS);
            continue;
        }
        if (strcmp(topic, SLM_TOPIC_PROMPT) != 0) {
            /* /slm/stop with nothing running. */
            msg_router_ack(comp_idx);
            continue;
        }

        /* Copy before the ack releases the mailbox to the next publisher. */
        size_t prompt_len = strlen(data);
        memcpy(g_prompt, data, prompt_len + 1u);
        msg_router_ack(comp_idx);

        slm_runner_serve_prompt(comp_idx, g_prompt, prompt_len);
    }

    uart_printf("[slm-runner] Exiting (%u prompts handled)\r\n",
                (unsigned)g_runner.prompts);
    slm_runner_close_session();
    __atomic_store_n(&g_runner.running, 0u, __ATOMIC_RELEASE);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}

/* ============================================================================
 * Control
 * ============================================================================ */

void slm_runner_default_config(slm_runner_config_t *cfg)
{
    *cfg = SLM_RUNNER_DEFAULT_CONFIG;
}

int slm_runner_configure(const slm_runner_config_t *cfg)
{
    if (slm_runner_alive()) {
        return -1;
    }
    g_cfg = *cfg;
    return 0;
}

static uint64_t ms_to_counts(uint32_t ms)
{
    return (timer_get_frequency() * ms) / 1000u;
}

int slm_runner_start(void)
{
    if (slm_runner_alive()) {
        return -1;
    }
    /* Left set only by a task killed from outside. */
    __atomic_store_n(&g_runner.running, 0u, __ATOMIC_RELEASE);
    if (component_run("slm-runner") < 0) {
        return -1;
    }
    /* A sleep gets the new task on the CPU whatever the caller's priority. */
    uint64_t start = timer_get_count();
    while (!__atomic_load_n(&g_runner.running, __ATOMIC_ACQUIRE)) {
        if (timer_get_count() - start >= ms_to_counts(SLM_RUNNER_START_TIMEOUT_MS)) {
            return -1;
        }
        task_sleep_ms(1);
    }
    return 0;
}

int slm_runner_stop(void)
{
    if (!slm_runner_alive()) {
        return -1;
    }
    int32_t idx = g_runner.comp_idx;
    __atomic_store_n(&g_runner.exit_requested, 1u, __ATOMIC_RELEASE);
    int32_t session = g_runner.session;
    if (session >= 0) {
        rust_slm_stop((uint32_t)session);
    }

    /* Wait for the task's cleanup, not just the flag: it unsubscribes by
     * component index, so a start before then would lose the new runner's
     * subscriptions. */
    uint64_t start = timer_get_count();
    for (;;) {
        component_info_t info;
        if (component_get_info((uint32_t)idx, &info) != 0 ||
            info.state == COMPONENT_UNLOADED) {
            return 0;
        }
        if (timer_get_count() - start >= ms_to_counts(SLM_RUNNER_STOP_TIMEOUT_MS)) {
            return -2;
        }
        task_sleep_ms(1);
    }
}

void slm_runner_get_status(slm_runner_status_t *out)
{
    bool alive = slm_runner_alive();
    out->running = alive ? 1u : 0u;
    out->busy = alive ? __atomic_load_n(&g_runner.busy, __ATOMIC_ACQUIRE) : 0u;
    out->component_idx = alive ? g_runner.comp_idx : -1;
    out->session = alive ? g_runner.session : -1;
    out->model = alive ? g_runner.model : -1;
    out->gpu_mode = g_cfg.gpu_mode;
    out->prompts = g_runner.prompts;
    out->dropped = g_runner.dropped;
}

/* ============================================================================
 * `slm ask`: a client that only uses the bus
 * ============================================================================ */

static uint32_t g_ask_claimed;

/* Value of `key` (with its =) in a /slm/done line, or `fallback`. */
static long slm_done_field(const char *done, const char *key, long fallback)
{
    const char *p = strstr(done, key);
    if (!p) {
        return fallback;
    }
    p += strlen(key);
    bool negative = false;
    if (*p == '-') {
        negative = true;
        p++;
    }
    if (*p < '0' || *p > '9') {
        return fallback;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return negative ? -v : v;
}

static int slm_ask_exchange(const char *text, uint32_t timeout_s)
{
    const int me = SLM_ASK_COMPONENT_IDX;

    /* Subscribe before publishing, or the first tokens have nowhere to go. */
    if (msg_router_subscribe(SLM_TOPIC_TOKEN, me) != 0 ||
        msg_router_subscribe(SLM_TOPIC_DONE, me) != 0) {
        shell_puts("slm ask: can't subscribe (topic full?)\r\n");
        return -1;
    }

    msg_publish_stats_t st = {0};
    int acked = msg_router_publish_reliable((const uint8_t *)SLM_TOPIC_PROMPT,
                                            (const uint8_t *)text,
                                            SLM_ASK_PUBLISH_TIMEOUT_MS, &st);
    if (acked <= 0) {
        shell_printf("slm ask: nobody took the prompt (subscribers=%d "
                     "unread=%d)\r\n", (int)st.targets, (int)st.unacked);
        return -1;
    }

    uint32_t received = 0;
    bool stop_sent = false;
    char done[MSG_ROUTER_MAX_PAYLOAD + 1];
    uint64_t timeout = timer_get_frequency() * timeout_s;
    uint64_t last_msg = timer_get_count();

    for (;;) {
        char topic[MSG_ROUTER_TOPIC_LEN];
        const char *data = msg_router_receive(me, topic);
        if (data) {
            last_msg = timer_get_count();
            if (strcmp(topic, SLM_TOPIC_DONE) == 0) {
                strncpy(done, data, sizeof(done) - 1u);
                done[sizeof(done) - 1u] = '\0';
                msg_router_ack(me);
                break;
            }
            for (const char *p = data; *p; p++) {
                shell_putc(*p);
            }
            shell_flush();
            received++;
            msg_router_ack(me);
            continue;
        }

        if (!stop_sent && shell_session_poll_interrupt()) {
            /* Nowait: the reply keeps coming until the runner sees this, and
             * only this loop can drain it. */
            msg_router_publish_nowait((const uint8_t *)SLM_TOPIC_STOP,
                                      (const uint8_t *)"stop");
            shell_puts("^C");
            stop_sent = true;
        }
        if (!slm_runner_alive()) {
            shell_puts("\r\nslm ask: the runner stopped before answering\r\n");
            return -1;
        }
        if (timer_get_count() - last_msg >= timeout) {
            msg_router_publish_nowait((const uint8_t *)SLM_TOPIC_STOP,
                                      (const uint8_t *)"stop");
            shell_printf("\r\nslm ask: no message for %lu s, giving up\r\n",
                         (unsigned long)timeout_s);
            return -1;
        }
        /* A yield, not a sleep. The runner waits for this ack before its next
         * forward step, so a sleep here would cost the decoder a scheduler
         * tick per token. */
        yield();
    }

    shell_puts("\r\n[slm done] ");
    shell_puts(done);
    shell_puts("\r\n");

    long rc = slm_done_field(done, "rc=", -1);
    long msgs = slm_done_field(done, "msgs=", -1);
    if ((long)received != msgs) {
        shell_printf("slm ask: LOST tokens: received %lu of %ld messages\r\n",
                     (unsigned long)received, msgs);
        return -1;
    }
    shell_printf("[slm ask] received %lu of %ld token messages\r\n",
                 (unsigned long)received, msgs);
    return rc == 0 ? 0 : -1;
}

int slm_runner_ask(const char *text, uint32_t timeout_s)
{
    if (strlen(text) > MSG_ROUTER_MAX_PAYLOAD) {
        shell_printf("slm ask: prompt is %lu bytes, the bus takes %u\r\n",
                     (unsigned long)strlen(text), MSG_ROUTER_MAX_PAYLOAD);
        return -1;
    }
    if (!slm_runner_alive()) {
        shell_puts("slm ask: no runner (`slm runner start` first)\r\n");
        return -1;
    }
    /* No request ids on the bus: one exchange at a time, or replies mix. */
    if (__atomic_load_n(&g_runner.busy, __ATOMIC_ACQUIRE)) {
        shell_puts("slm ask: the runner is answering a prompt; try again when "
                   "it's done\r\n");
        return -1;
    }
    if (__atomic_exchange_n(&g_ask_claimed, 1u, __ATOMIC_ACQUIRE)) {
        shell_puts("slm ask: another `slm ask` is running\r\n");
        return -1;
    }
    int rc = slm_ask_exchange(text, timeout_s);
    msg_router_unsubscribe_all(SLM_ASK_COMPONENT_IDX);
    __atomic_store_n(&g_ask_claimed, 0u, __ATOMIC_RELEASE);
    return rc;
}
