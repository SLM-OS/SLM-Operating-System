/*
 * slm_runner.h — the slm-runner component and its bus client.
 *
 * docs/tutorials/slm-component.md describes the topics and message formats.
 *
 * Holds the additions to the message router's C surface this feature needs.
 * The router's other declarations are scattered across the kernel's TUs.
 */

#ifndef SLM_RUNNER_H
#define SLM_RUNNER_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Message router additions (runtime/src/msg_router.rs)
 * ============================================================================ */

/* Largest payload a publish accepts, excluding the NUL. Pinned against
 * msg_router_max_payload() by kernel/tests/test_msg_router.c. */
#define MSG_ROUTER_MAX_PAYLOAD 511u

/* Mirrors MsgPublishStats. */
typedef struct {
    int32_t targets;  /* subscribers the message was addressed to */
    int32_t acked;    /* took it and acked, or unsubscribed */
    int32_t dropped;  /* mailbox stayed full for the timeout: message lost */
    int32_t unacked;  /* landed, not read in time: not lost */
} msg_publish_stats_t;

extern uint32_t msg_router_max_payload(void);

/*
 * Publish with back-pressure: wait up to timeout_ms per subscriber for room,
 * then again for the ack. Returns the subscribers that acked, -1 on a bad
 * argument or oversized payload. Blocks: task context, IRQs on, no lock held.
 */
extern int msg_router_publish_reliable(const uint8_t *topic_name,
                                       const uint8_t *data,
                                       uint32_t timeout_ms,
                                       msg_publish_stats_t *stats);

/* msg_router_receive for one exact topic. Ack with msg_router_ack. */
extern const char *msg_router_receive_topic(int component_idx,
                                            const char *topic_name);

/* ============================================================================
 * Registry additions (runtime/src/slm/registry.rs)
 *
 * Both take the registry lock, which a running prompt holds for each forward
 * step: never from an IRQs-off path (#1093).
 * ============================================================================ */

/* Most recently loaded SLM still loaded. 0, or -1 if none is. */
extern int rust_slm_latest_loaded(uint32_t *handle, uint64_t *seq);

/* Load sequence number of a slot, 0 if empty. A reload gets a new number. */
extern uint64_t rust_slm_load_seq(uint32_t index);

/* ============================================================================
 * Runner
 * ============================================================================ */

#define SLM_TOPIC_PROMPT "/slm/prompt"
#define SLM_TOPIC_TOKEN  "/slm/token"
#define SLM_TOPIC_DONE   "/slm/done"
#define SLM_TOPIC_STOP   "/slm/stop"

/* Router component slot of the `slm ask` client: above native components,
 * Lua states and telemetryd (see tcp_telemetry_server.c). */
#define SLM_ASK_COMPONENT_IDX 50

/* model_handle value meaning "the most recently loaded model". */
#define SLM_RUNNER_MODEL_LATEST (-1)

/* The session options of `slm launch`, plus what only the runner has. Only
 * TUs built with FP enabled may touch the float members. */
typedef struct {
    int32_t  model_handle;   /* slot, or SLM_RUNNER_MODEL_LATEST */
    uint32_t max_ctx;
    uint32_t sampler_kind;   /* SLM_SAMPLER_* */
    float    temperature;
    uint32_t top_k;
    float    top_p;
    uint64_t seed;
    uint32_t gpu_mode;       /* SLM_GPU_* */
    uint32_t threads;
    uint32_t max_new_tokens;
    uint32_t prompt_flags;   /* 0 (chat template) or SLM_PROMPT_RAW */
} slm_runner_config_t;

typedef struct {
    uint32_t running;        /* the task is serving the bus */
    uint32_t busy;           /* handling a prompt now */
    int32_t  component_idx;  /* -1 when stopped */
    int32_t  session;        /* -1 until a session is open */
    int32_t  model;          /* slot the session is on, -1 without one */
    uint32_t gpu_mode;       /* configured SLM_GPU_* */
    uint32_t prompts;        /* handled since this runner started */
    uint32_t dropped;        /* /slm/token messages lost since it started */
} slm_runner_status_t;

/* The configuration `component run slm-runner` uses when nothing set one. */
void slm_runner_default_config(slm_runner_config_t *cfg);

/* Store the configuration the next start uses. -1 while the runner runs. */
int slm_runner_configure(const slm_runner_config_t *cfg);

/* Start the component and wait until it serves the bus. 0, or -1. */
int slm_runner_start(void);

/* Stop the prompt it's running, if any, end the task and wait for that.
 * 0, -1 if it wasn't running, -2 if it hasn't exited after 60 s. */
int slm_runner_stop(void);

void slm_runner_get_status(slm_runner_status_t *out);

/*
 * The `slm ask` client: publish `text` on /slm/prompt and print the reply
 * from /slm/token and /slm/done. Ctrl-C publishes /slm/stop. `timeout_s` is
 * how long to go without a message before giving up. Returns 0 if the runner
 * answered with nothing lost, -1 otherwise.
 */
int slm_runner_ask(const char *text, uint32_t timeout_s);

/* Word for SlmStatsC.last_stop_reason. */
const char *slm_stop_reason_name(uint32_t reason);

#endif /* SLM_RUNNER_H */
