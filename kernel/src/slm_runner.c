/*
 * slm_runner.c — Phase SLM M8 `slm-runner` component.
 *
 * Subscribes to /slm/prompt, runs each prompt through the M5.2 SLM
 * session FFI (rust_slm_session_open / rust_slm_prompt), publishes one
 * /slm/token message per generated token, and ends every prompt with a
 * /slm/done message carrying per-prompt telemetry counters.
 *
 * The runner expects an SLM to already be loaded (`slm load <path>`).
 * When the registry is empty at startup it logs a friendly hint and
 * exits, so the operator gets a clear next-step rather than a silent
 * idle.
 *
 * M8.1 limitation: the M5.2 decoder runs on placeholder zero weights
 * and returns -1 for non-empty prompts. The runner still publishes
 * /slm/done with the current telemetry snapshot in that case so
 * subscribers can observe completion. Real text generation lands with
 * M5.3.
 *
 * NOTE on -mgeneral-regs-only: rust_slm_session_open and rust_slm_prompt
 * take f32 by value (AAPCS64 V0/V1) which the kernel's default
 * FP-disable forbids the C compiler from materialising. CMakeLists.txt
 * compiles this file without -mgeneral-regs-only — same isolation
 * pattern as kernel/src/slm_shell.c. No other kernel TU in this
 * library is allowed to do FP arithmetic.
 */

#include "component.h"
#include "slm_ffi.h"
#include "uart.h"
#include "sched.h"

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * External symbols (component_runtime.c + msg_router)
 * ============================================================================ */

extern int  msg_router_subscribe(const char *topic_name, int component_idx);
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
/* msg_router_publish is declared in slm_ffi.h. */

/* Hardware-counter timeout helpers — duplicated from
 * component_runtime.c to keep this TU standalone and avoid leaking
 * private inline helpers across the FP boundary. timer_get_count()
 * and timer_get_frequency() are integer-only, so they compile fine
 * here even without -mgeneral-regs-only relaxed. */
#include "timer.h"
static inline uint64_t hw_timeout_start(void) {
    return timer_get_count();
}
static inline int hw_timeout_expired(uint64_t start, uint32_t seconds) {
    uint64_t freq = timer_get_frequency();
    return (timer_get_count() - start) >= (freq * seconds);
}

/* ============================================================================
 * Token callback + per-prompt context
 * ============================================================================ */

/* Per-prompt context handed to the token callback. */
typedef struct {
    int      comp_idx;
    uint32_t session_id;
    uint32_t tokens_emitted;
} slm_runner_token_ctx_t;

/*
 * Token callback invoked by the Rust decoder once per generated token.
 *
 * Publishes the UTF-8 token bytes on /slm/token. The mailbox copies
 * the bytes (capped at MAX_MSG_LEN), so the callback may return
 * immediately after the publish call without retaining the pointer —
 * matching the lifetime contract documented on RustSlmTokenCb.
 *
 * Yields after each publish so subscriber tasks get a chance to drain
 * their mailbox before the next token arrives. Without a yield, a
 * fast decoder can overwrite an unacknowledged slot under the
 * router's latest-wins semantics and the subscriber misses tokens.
 */
static int32_t slm_runner_token_cb(
    void *user,
    uint32_t token_id,
    const uint8_t *bytes,
    size_t bytes_len)
{
    (void)token_id;
    slm_runner_token_ctx_t *ctx = (slm_runner_token_ctx_t *)user;

    /* Stage the token bytes into a NUL-terminated buffer.
     *
     * The router copies up to MAX_MSG_LEN bytes and looks for a NUL
     * terminator; the M5.2 decoder hands over &str.as_bytes() which
     * is NOT NUL-terminated. The 60-byte cap matches MAX_MSG_LEN in
     * runtime/src/msg_router.rs minus one for the terminator. */
    uint8_t buf[60];
    size_t n = bytes_len;
    if (n > sizeof(buf) - 1) {
        n = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = bytes[i];
    }
    buf[n] = '\0';

    msg_router_publish((const uint8_t *)"/slm/token", buf);
    if (ctx) {
        ctx->tokens_emitted++;
    }

    /* Cooperative yield so a subscriber gets a chance to ack the
     * mailbox before the decoder writes the next token. */
    yield();

    return 1;  /* continue generating */
}

/* ============================================================================
 * Public helpers (also exercised by kernel/tests/test_slm_runner.c)
 * ============================================================================ */

/*
 * Open a session over the runner's slot 0 with caller-chosen
 * `max_ctx`. The sampler params come from the spec demo defaults
 * (temperature 0.7 + top_p 0.9 + top_k 40). Returns the session id
 * (>= 0) on success, -1 when the registry is empty (operator hasn't
 * run `slm load` yet) or the Rust-side session_open call fails. Logs
 * a friendly hint on the empty-registry path so the failure mode is
 * self-explanatory.
 *
 * Factored out so unit tests (which run with a tiny Rust heap) can
 * pass a small `max_ctx` without modifying the runner's production
 * defaults. The integer-typed signature also keeps the call site
 * reachable from -mgeneral-regs-only test TUs.
 */
int slm_runner_setup_session_with_ctx(uint32_t max_ctx)
{
    if (rust_slm_count() == 0) {
        uart_puts("[slm-runner] ERROR: no SLM loaded\r\n");
        uart_puts("[slm-runner] Load one with: slm load /mnt/files/<model>.gguf\r\n");
        return -1;
    }

    int32_t session = rust_slm_session_open(
        /* model_handle = */ 0u,
        max_ctx,
        /* sampler_kind = */ SLM_SAMPLER_TOP_K_TOP_P,
        /* temperature  = */ 0.7f,
        /* top_k        = */ 40u,
        /* top_p        = */ 0.9f,
        /* seed         = */ 0u);
    if (session < 0) {
        uart_puts("[slm-runner] ERROR: rust_slm_session_open failed\r\n");
        return -1;
    }
    return (int)session;
}

/*
 * Production entry: opens a session with the spec's max_ctx ceiling
 * (4096). Thin wrapper over `slm_runner_setup_session_with_ctx`.
 */
int slm_runner_setup_session(void)
{
    return slm_runner_setup_session_with_ctx(4096u);
}

/*
 * Run a single prompt through the session, streaming tokens to
 * /slm/token and publishing /slm/done on completion. Factored out of
 * slm_runner_entry so unit tests can exercise the dispatch path
 * without spinning up a task or waiting on the receive loop's idle
 * timeout.
 *
 * Returns the rc that rust_slm_prompt produced (0 on success, -1 on
 * decoder error). The /slm/done message is published on every path
 * so subscribers always observe per-prompt completion.
 */
int slm_runner_handle_prompt(uint32_t session_id,
                             const char *prompt,
                             size_t prompt_len,
                             uint32_t *tokens_emitted_out)
{
    slm_runner_token_ctx_t cb_ctx = {
        .comp_idx = -1,
        .session_id = session_id,
        .tokens_emitted = 0,
    };

    int32_t rc = rust_slm_prompt(
        session_id,
        (const uint8_t *)prompt,
        prompt_len,
        /* max_new_tokens = */ 256u,
        slm_runner_token_cb,
        &cb_ctx);

    SlmStatsC stats = {0};
    rust_slm_stats(session_id, &stats);

    char done_msg[60];
    uart_snprintf(done_msg, sizeof(done_msg),
                  "rc=%d tokens_out=%u prompts=%u",
                  (int)rc, cb_ctx.tokens_emitted, stats.prompts_completed);
    msg_router_publish((const uint8_t *)"/slm/done",
                       (const uint8_t *)done_msg);

    if (tokens_emitted_out) {
        *tokens_emitted_out = cb_ctx.tokens_emitted;
    }
    return (int)rc;
}

/* ============================================================================
 * Component entry point — registered in kernel/src/component_runtime.c's
 * builtin_components[] table.
 * ============================================================================ */

void slm_runner_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    int sess_rc = slm_runner_setup_session();
    if (sess_rc < 0) {
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }
    uint32_t session_id = (uint32_t)sess_rc;

    if (msg_router_subscribe("/slm/prompt", comp_idx) != 0) {
        uart_puts("[slm-runner] ERROR: subscribe /slm/prompt failed\r\n");
        rust_slm_session_close(session_id);
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }

    uart_printf("[slm-runner] Started (component %d, session %u), "
                "subscribed to /slm/prompt\r\n",
                comp_idx, session_id);

    uint32_t prompts_handled = 0;
    uint64_t hw_start = hw_timeout_start();

    while (!hw_timeout_expired(hw_start, 60)) {
        char topic_buf[16];
        const char *data = msg_router_receive(comp_idx, topic_buf);

        if (!data) {
            yield();
            continue;
        }

        /* Copy the prompt out of the mailbox before ack so the slot
         * can be released for the next publisher. The router caps
         * payloads at MAX_MSG_LEN (60); a 64-byte buffer covers it. */
        char prompt[64];
        size_t prompt_len = 0;
        for (size_t i = 0; i < sizeof(prompt) - 1; i++) {
            char c = data[i];
            if (c == '\0') break;
            prompt[i] = c;
            prompt_len++;
        }
        prompt[prompt_len] = '\0';
        msg_router_ack(comp_idx);

        /* Dispatch to the decoder + publish /slm/done with telemetry.
         * The helper publishes /slm/done on every path so subscribers
         * always observe per-prompt completion, even when the M5.2
         * decoder rejects a non-empty prompt with rc=-1. */
        uint32_t tokens_emitted = 0;
        int rc = slm_runner_handle_prompt(session_id, prompt, prompt_len,
                                          &tokens_emitted);

        prompts_handled++;
        uart_printf("[slm-runner] prompt #%u rc=%d tokens=%u\r\n",
                    prompts_handled, rc, tokens_emitted);

        hw_start = hw_timeout_start();  /* reset idle timeout on activity */
    }

    uart_printf("[slm-runner] Exiting (%u prompts handled)\r\n",
                prompts_handled);
    rust_slm_session_close(session_id);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}
