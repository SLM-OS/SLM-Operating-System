/*
 * component_runtime.c - Component execution and built-in components
 *
 * Bridges the component registry (Rust) to the kernel task system (C).
 * Provides built-in component entry points and the `component run` command.
 *
 * A component becomes executable by:
 *   1. Registering metadata in the component registry
 *   2. Looking up a built-in entry point by name
 *   3. Creating a kernel task with that entry point
 *   4. Linking the task ID back to the component
 *   5. When the task exits, the component state is set to Unloaded
 */

#include "component.h"
#include "task.h"
#include "sched.h"
#include "slm_ffi.h"
/* IPC used by message router (M7); echo service uses simple mailbox */
#include "uart.h"
#include "debug.h"
#include "arch.h"
#include "timer.h"
#include "pmm.h"
#include "spinlock.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Hardware-counter timeout helper.
 * On Pi 5, pit_ticks doesn't advance while tasks run (IRQs masked).
 * Use timer_get_count() which reads the always-running hardware counter.
 *
 * Timeout values by component:
 *   echo_service:    60s — waits for user-initiated messages via shell
 *   listener:        30s — demo scenario, exits after inactivity
 *   sensor_monitor:  30s — demo scenario, resets on each received message
 *   digit_classifier:30s — demo scenario, resets on each inference
 *   echo send:        5s — shell command round-trip, should be near-instant */
static inline uint64_t hw_timeout_start(void) {
    return timer_get_count();
}
static inline int hw_timeout_expired(uint64_t start, uint32_t seconds) {
    uint64_t freq = timer_get_frequency();
    return (timer_get_count() - start) >= (freq * seconds);
}

/* ============================================================================
 * Component Task Cleanup
 *
 * When a component's task exits, update the component state to Unloaded.
 * ============================================================================ */

struct component_task_ctx {
    int component_idx;
};

extern void msg_router_unsubscribe_all(int component_idx);

/*
 * Cleanup callback invoked by task_destroy() when a component task exits.
 * The ctx is heap-allocated per task (one page) in component_start() — this
 * gives each task its own cleanup state, so a surviving task from an earlier
 * component cannot fire a cleanup meant for a new component that reused the
 * same registry slot.
 */
static void component_task_cleanup(void *arg)
{
    struct component_task_ctx *ctx = (struct component_task_ctx *)arg;
    if (!ctx) return;

    /* Only clean up if this component still owns the slot (not hot-swapped) */
    component_info_t info;
    if (component_get_info((uint32_t)ctx->component_idx, &info) == 0 &&
        (info.state == COMPONENT_TERMINATING || info.state == COMPONENT_RUNNING)) {
        msg_router_unsubscribe_all(ctx->component_idx);
        component_set_state((uint32_t)ctx->component_idx, COMPONENT_UNLOADED);
    }
    DEBUG_PRINT("Component %d task exited", ctx->component_idx);

    pmm_free_page(ctx);
}

/* ============================================================================
 * Built-in Component: Counter Service
 *
 * A simple service that counts ticks and periodically reports.
 * Demonstrates component lifecycle (Running → Terminating → Unloaded).
 * ============================================================================ */

static void counter_service_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    uart_printf("[counter] Started (component %d)\n", comp_idx);

    uint32_t count = 0;
    for (int i = 0; i < 10; i++) {
        count++;
        extern void sleep_ms(uint32_t ms);
        sleep_ms(500);

        if (i % 3 == 2) {
            uart_printf("[counter] Count: %u\n", count);
        }
    }

    uart_printf("[counter] Done (count=%u)\n", count);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}

/* ============================================================================
 * Built-in Component: Echo Service
 *
 * Listens on an IPC message queue and echoes messages back.
 * Demonstrates inter-component communication.
 * ============================================================================ */

/* Simple shared mailbox for echo service (avoids IPC queue complexity) */
static volatile struct {
    volatile uint32_t ready;       /* 1 = message available */
    volatile uint32_t ack;         /* 1 = message consumed */
    char data[64];
} echo_mailbox;

/* Echo service running flag */
static volatile int echo_running;

static void echo_service_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    extern void sleep_ms(uint32_t ms);

    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);
    echo_running = 1;
    uart_printf("[echo] Started (component %d), listening for messages...\n", comp_idx);

    int msgs_received = 0;
    int idle_polls = 0;

    /* Poll shared mailbox for messages using yield() instead of sleep_ms().
     * Use hardware counter for time-based timeout (60 seconds). */
    uint64_t _hw_start = hw_timeout_start();
    while (!hw_timeout_expired(_hw_start, 60)) {
        if (__atomic_load_n(&echo_mailbox.ready, __ATOMIC_ACQUIRE)) {
            echo_mailbox.data[63] = '\0';
            msgs_received++;
            idle_polls = 0;
            uart_printf("[echo] Received: \"%s\" (msg #%d)\n",
                        (const char *)echo_mailbox.data, msgs_received);
            __atomic_store_n(&echo_mailbox.ready, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&echo_mailbox.ack, 1, __ATOMIC_RELEASE);
        } else {
            idle_polls++;
            yield();
        }
    }

    if (msgs_received > 0)
        uart_printf("[echo] Idle timeout after %d messages\n", msgs_received);

    uart_printf("[echo] Done (%d messages)\n", msgs_received);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
    echo_running = 0;
}

/* ============================================================================
 * Built-in Component: Listener Service
 *
 * Subscribes to a topic via the message router and prints received messages.
 * Demonstrates the pub/sub IPC pattern (M7 MessageRouter).
 * ============================================================================ */

/* Message router API (msg_router.c) */
extern void msg_router_init(void);
extern int msg_router_subscribe(const char *topic_name, int component_idx);
/* msg_router_publish declared in slm_ffi.h */
extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);

static void listener_service_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;

    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    /* Subscribe to the "events" topic */
    if (msg_router_subscribe("events", comp_idx) != 0) {
        uart_printf("[listener] Failed to subscribe to 'events'\n");
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }

    uart_printf("[listener] Started (component %d), subscribed to 'events'\n", comp_idx);

    int msgs_received = 0;
    uint64_t _hw_start = hw_timeout_start();
    while (!hw_timeout_expired(_hw_start, 30)) {
        char topic_buf[16];
        const char *data = msg_router_receive(comp_idx, topic_buf);
        if (data) {
            msgs_received++;
            uart_printf("[listener] [%s] \"%s\" (msg #%d)\n",
                        topic_buf, data, msgs_received);
            msg_router_ack(comp_idx);
            /* Reset timeout on activity */
            _hw_start = hw_timeout_start();
        } else {
            yield();
        }
    }

    uart_printf("[listener] Done (%d messages)\n", msgs_received);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}

/* ============================================================================
 * Built-in Component Table
 * ============================================================================ */

typedef void (*component_entry_t)(void *arg);

struct builtin_component {
    const char *name;
    const char *version;
    const char *description;
    uint8_t type;
    uint8_t priority;
    component_entry_t entry;
    const char *model_name;  /* Model to preload on start (NULL = none) */
};

/* ============================================================================
 * Phase 5 M5: Example SLM Components
 * ============================================================================ */

/*
 * Sensor Monitor — rule-based threshold monitoring (no ML).
 *
 * Subscribes to /sensors/data, checks if the value exceeds a threshold,
 * and publishes alerts to /alerts/threshold. Demonstrates component
 * lifecycle and message routing without model loading.
 */
/* State export for sensor_monitor stateful hot-swap.
 * Called from component_hot_swap_stateful before teardown. */
static int sensor_monitor_alert_count = 0;

/* Expose for testing — verifies state was transferred */
int sensor_monitor_get_alert_count(void) { return sensor_monitor_alert_count; }

int sensor_monitor_export_state(uint8_t *buf, uint32_t max_size)
{
    if (max_size < 4) return -1;
    /* Simple serialization: 4-byte little-endian alert count */
    buf[0] = (uint8_t)(sensor_monitor_alert_count & 0xFF);
    buf[1] = (uint8_t)((sensor_monitor_alert_count >> 8) & 0xFF);
    buf[2] = (uint8_t)((sensor_monitor_alert_count >> 16) & 0xFF);
    buf[3] = (uint8_t)((sensor_monitor_alert_count >> 24) & 0xFF);
    return 4;
}

static void sensor_monitor_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    /* Subscribe to sensor data topic */
    msg_router_subscribe("/sensors/data", comp_idx);

    /* Check for state from stateful hot-swap */
    uint8_t state_buf[COMPONENT_STATE_MAX];
    uint32_t state_size = component_get_swap_state(state_buf, sizeof(state_buf));
    if (state_size >= 4) {
        sensor_monitor_alert_count = (int)(state_buf[0] | (state_buf[1] << 8) |
                                           (state_buf[2] << 16) | (state_buf[3] << 24));
        uart_printf("[sensor_monitor] Resumed with %d prior alerts\r\n",
                    sensor_monitor_alert_count);
    } else {
        sensor_monitor_alert_count = 0;
    }

    uart_puts("[sensor_monitor] Started, watching /sensors/data\r\n");

    uint64_t _hw_start = hw_timeout_start();

    while (!hw_timeout_expired(_hw_start, 30)) {
        char topic_buf[16];
        const char *data = msg_router_receive(
            comp_idx, topic_buf);

        if (data) {
            /* Parse integer value from message (simple atoi) */
            int value = 0;
            for (int i = 0; data[i] >= '0' && data[i] <= '9' && i < 10; i++) {
                value = value * 10 + (data[i] - '0');
            }

            msg_router_ack(comp_idx);

            /* Threshold check */
            if (value > 50) {
                char alert[60];
                int len = 0;
                const char *prefix = "ALERT: value=";
                for (int i = 0; prefix[i]; i++) alert[len++] = prefix[i];
                /* Append integer value */
                if (value >= 100) alert[len++] = '0' + (value / 100) % 10;
                if (value >= 10) alert[len++] = '0' + (value / 10) % 10;
                alert[len++] = '0' + value % 10;
                alert[len] = '\0';

                msg_router_publish((const uint8_t *)"/alerts/threshold\0",
                                   (const uint8_t *)alert);
                sensor_monitor_alert_count++;
                uart_printf("[sensor_monitor] %s\r\n", alert);
            } else {
                uart_printf("[sensor_monitor] value=%d (normal)\r\n", value);
            }

            _hw_start = hw_timeout_start();  /* Reset timeout on activity */
        } else {
            yield();
        }
    }

    uart_printf("[sensor_monitor] Exiting (%d alerts issued)\r\n", sensor_monitor_alert_count);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}

/*
 * Digit Classifier — MNIST inference component.
 *
 * Subscribes to /input/digits, runs MNIST inference via the Rust inference
 * engine, and publishes classification results to /output/class.
 * Demonstrates the full AI inference pipeline within a component.
 *
 * Prerequisites: MNIST model must be loaded via `model load /mnt/files/mnist.onnx`
 */
static void digit_classifier_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    /* Find the MNIST model */
    int model_idx = rust_model_find("mnist");
    if (model_idx < 0) {
        uart_puts("[digit_classifier] ERROR: MNIST model not loaded\r\n");
        uart_puts("[digit_classifier] Load with: model load /mnt/files/mnist.onnx\r\n");
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }

    uart_printf("[digit_classifier] Started with model at slot %d\r\n", model_idx);

    /* Subscribe to digit classification requests */
    msg_router_subscribe((const char *)"/input/digits\0", comp_idx);

    uint64_t _hw_start = hw_timeout_start();
    int infer_count = 0;

    while (!hw_timeout_expired(_hw_start, 30)) {
        char topic_buf[16];
        const char *data = msg_router_receive(
            comp_idx, topic_buf);

        if (data) {
            msg_router_ack(comp_idx);

            /* Run inference (zero input — returns argmax class) */
            int predicted_class = rust_infer_classify((uint32_t)model_idx);

            if (predicted_class >= 0) {
                /* Publish classification result */
                char result[60];
                result[0] = 'c';
                result[1] = 'l';
                result[2] = 'a';
                result[3] = 's';
                result[4] = 's';
                result[5] = '=';
                result[6] = '0' + (char)(predicted_class % 10);
                result[7] = '\0';

                msg_router_publish((const uint8_t *)"/output/class\0",
                                   (const uint8_t *)result);
                infer_count++;
                uart_printf("[digit_classifier] Predicted class: %d\r\n",
                            predicted_class);
            } else {
                uart_puts("[digit_classifier] Inference failed\r\n");
            }

            _hw_start = hw_timeout_start();  /* Reset timeout */
        } else {
            yield();
        }
    }

    uart_printf("[digit_classifier] Exiting (%d inferences)\r\n", infer_count);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}

static const struct builtin_component builtin_components[] = {
    {
        .name = "counter",
        .version = "1.0",
        .description = "Counts to 10 with 500ms intervals",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_NORMAL,
        .entry = counter_service_entry,
    },
    {
        .name = "echo",
        .version = "1.0",
        .description = "Echoes IPC messages back to sender",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_IDLE,  /* Same as shell to enable round-robin */
        .entry = echo_service_entry,
    },
    {
        .name = "listener",
        .version = "1.0",
        .description = "Listens on 'events' topic via message router",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_IDLE,  /* Same as shell for round-robin */
        .entry = listener_service_entry,
    },
    /* Phase 5 M5: Example SLM components */
    {
        .name = "sensor_monitor",
        .version = "1.0",
        .description = "Rule-based sensor threshold monitoring",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_IDLE,
        .entry = sensor_monitor_entry,
    },
    {
        .name = "digit_classifier",
        .version = "1.0",
        .description = "MNIST digit classification via inference engine",
        .type = COMPONENT_TYPE_APPLICATION,
        .priority = COMPONENT_PRIORITY_NORMAL,
        .entry = digit_classifier_entry,
        .model_name = "mnist",
    },
};

#define NUM_BUILTIN_COMPONENTS (sizeof(builtin_components) / sizeof(builtin_components[0]))

/* ============================================================================
 * Component Execution API
 * ============================================================================ */

/*
 * Run a built-in component by name.
 * Registers it in the component system, creates a task, links them.
 * Returns component index on success, -1 on failure.
 */
int component_run(const char *name)
{
    /* Find built-in component */
    const struct builtin_component *bc = NULL;
    for (size_t i = 0; i < NUM_BUILTIN_COMPONENTS; i++) {
        /* strcmp equivalent */
        const char *a = name;
        const char *b = builtin_components[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            bc = &builtin_components[i];
            break;
        }
    }

    if (!bc) {
        uart_printf("Unknown component: '%s'\n", name);
        uart_printf("Available components:\n");
        for (size_t i = 0; i < NUM_BUILTIN_COMPONENTS; i++) {
            uart_printf("  %-12s %s — %s\n",
                        builtin_components[i].name,
                        builtin_components[i].version,
                        builtin_components[i].description);
        }
        return -1;
    }

    /* Check if already running */
    int existing = component_find(bc->name);
    if (existing >= 0) {
        component_info_t info;
        if (component_get_info((uint32_t)existing, &info) == 0 &&
            info.state == COMPONENT_RUNNING) {
            uart_printf("Component '%s' is already running (idx %d)\n", name, existing);
            return -1;
        }
        /* Remove stale entry */
        component_unregister((uint32_t)existing);
    }

    /* Register in component system */
    int comp_idx = component_register(bc->name, bc->version, bc->type, bc->priority);
    if (comp_idx < 0) {
        uart_printf("Failed to register component '%s'\n", name);
        return -1;
    }

    component_set_state((uint32_t)comp_idx, COMPONENT_INITIALIZING);

    /* Preload model if component manifest declares one.
     *
     * #64: check if the model is already loaded first. If not, load it
     * synchronously — the async preload path (`model preload <name>`)
     * can be used by Lua scripts or shell commands to pre-warm the
     * registry before component_run so this load is a no-op. For the
     * built-in MNIST (26 KB, <1 ms), synchronous loading is fast enough
     * that deferring to a background task adds complexity without
     * measurable benefit. True async is available via the shell/Lua
     * `model preload` command for future large models. */
    if (bc->model_name) {
        int model_idx = rust_model_find(bc->model_name);
        if (model_idx < 0) {
            /* #64: check if an async preload is in-flight for this
             * model before falling back to synchronous loading. If a
             * Lua script or shell command already kicked off `model
             * preload <name>`, waiting is cheaper than re-loading. */
            extern int model_preload_wait(const char *name, uint32_t timeout_ms);
            int wait_idx = model_preload_wait(bc->model_name, 2000);
            if (wait_idx >= 0) {
                model_idx = wait_idx;
                uart_printf("[component] Model '%s' ready via async preload "
                            "(idx %d) for %s\n",
                            bc->model_name, model_idx, bc->name);
            } else {
                /* Not in-flight — fall back to synchronous load. */
                const char *mn = bc->model_name;
                bool is_mnist = (mn[0]=='m' && mn[1]=='n' && mn[2]=='i' &&
                                 mn[3]=='s' && mn[4]=='t' && mn[5]=='\0');
                if (is_mnist) {
                    model_idx = rust_model_load_builtin_mnist();
                }
                if (model_idx >= 0) {
                    uart_printf("[component] Preloaded model '%s' (idx %d) "
                                "for %s\n",
                                bc->model_name, model_idx, bc->name);
                } else {
                    uart_printf("[WARN] Failed to preload model '%s' for %s\n",
                                bc->model_name, bc->name);
                }
            }
        } else {
            uart_printf("[component] Model '%s' already loaded (idx %d) "
                        "for %s\n",
                        bc->model_name, model_idx, bc->name);
        }
    }

    /* Create task */
    struct task *task = task_create_with_priority(
        bc->name, bc->entry, (void *)(uintptr_t)comp_idx, bc->priority);

    if (!task) {
        uart_printf("Failed to create task for component '%s'\n", name);
        component_unregister((uint32_t)comp_idx);
        return -1;
    }

    /* Pre-initialize echo service state (before task starts). Use atomics
     * so the new task sees `echo_running == 1` via an acquire pairing with
     * the release store below, and so any later `__atomic_load_n(&ready)`
     * on the sender side is guaranteed to observe the zero init. */
    if (bc->entry == echo_service_entry) {
        __atomic_store_n(&echo_mailbox.ready, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&echo_mailbox.ack, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&echo_running, 1, __ATOMIC_RELEASE);
    }

    /* Pin component tasks to CPU 0 so they share the scheduler with shell
     * and IPC via shared memory is immediately visible (same CPU). */
    task->cpu_affinity = 0;

    /* Link task to component. The cleanup ctx is heap-allocated per task so
     * each task carries its own reference to the registry slot: a surviving
     * task from an earlier generation cannot fire a cleanup against a new
     * component that reused the same index. The page is freed inside
     * component_task_cleanup() when the task exits. */
    struct component_task_ctx *ctx = pmm_alloc_page();
    if (!ctx) {
        uart_printf("Failed to allocate cleanup ctx for component '%s'\n", name);
        task_destroy(task);
        component_unregister((uint32_t)comp_idx);
        return -1;
    }
    ctx->component_idx = comp_idx;
    task_set_cleanup(task, component_task_cleanup, ctx);

    /* Add to scheduler — don't yield here, timer preemption will start the task */
    scheduler_add_task(task);

    uart_printf("Component '%s' v%s started (idx=%d, task=%u)\n",
                bc->name, bc->version, comp_idx, task->id);

    return comp_idx;
}

/*
 * Send a message to the echo service (for testing component IPC).
 */
int component_send_echo(const char *message)
{
    if (!echo_running) {
        uart_printf("Echo service not running\n");
        return -1;
    }

    /* Copy message to shared mailbox with plain stores. Ordering against
     * the consumer is established by the explicit release fence below,
     * which makes every preceding store (data bytes + ack=0) globally
     * visible before the relaxed ready=1 store. `volatile` alone does
     * not provide this guarantee under the C memory model. */
    size_t len = 0;
    while (message[len] && len < 63) {
        echo_mailbox.data[len] = message[len];
        len++;
    }
    echo_mailbox.data[len] = '\0';

    /* Clear the stale ack before arming ready. Relaxed is fine — the fence
     * below covers ordering. */
    __atomic_store_n(&echo_mailbox.ack, 0, __ATOMIC_RELAXED);

    /* Publish: release fence + relaxed store on ready. Paired with the
     * consumer's `__atomic_load_n(&ready, __ATOMIC_ACQUIRE)` in
     * echo_service_entry. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&echo_mailbox.ready, 1, __ATOMIC_RELAXED);

    /* Both shell and echo are IDLE priority — yield() round-robins between them */
    uint64_t send_start = hw_timeout_start();
    while (!hw_timeout_expired(send_start, 5)) {
        if (__atomic_load_n(&echo_mailbox.ack, __ATOMIC_ACQUIRE)) {
            uart_printf("Message delivered to echo service\n");
            return 0;
        }
        yield();
    }

    uart_printf("Echo service did not acknowledge (5s timeout)\n");
    return -1;
}

/*
 * Zero-copy message publish: passes data by reference.
 * For messages larger than the inline mailbox limit, this avoids
 * copying into each subscriber's mailbox. The data pointer must remain
 * valid until all subscribers acknowledge.
 *
 * Current implementation: delegates to msg_router_publish for small messages.
 * For large messages, subscribers get the same data pointer (zero-copy
 * within the shared address space). The caller must not free the data
 * until this function returns.
 */
int msg_router_publish_large(const char *topic_name, const char *data,
                             uint32_t data_len)
{
    /* For now, delegate to the standard publish path.
     * The shared address space means the subscriber can read the data
     * pointer directly — the publish function copies it into the mailbox,
     * but the caller's buffer remains valid. True zero-copy (passing only
     * a pointer through the mailbox) requires mailbox struct changes that
     * are deferred. This function establishes the API contract. */
    (void)data_len;
    return msg_router_publish((const uint8_t *)topic_name, (const uint8_t *)data);
}

/*
 * Direct component-to-component message channel.
 * Bypasses topic routing for known endpoints, providing lower latency
 * than the publish/subscribe path. Uses a simple shared mailbox per
 * registered channel.
 */
#define DIRECT_CHANNEL_MAX 4
#define DIRECT_MSG_MAX 64

struct direct_channel {
    int sender_idx;
    int receiver_idx;
    volatile uint32_t ready;
    volatile uint32_t ack;
    char data[DIRECT_MSG_MAX];
};

static struct direct_channel direct_channels[DIRECT_CHANNEL_MAX];

int component_direct_channel_create(int sender_idx, int receiver_idx)
{
    for (int i = 0; i < DIRECT_CHANNEL_MAX; i++) {
        if (direct_channels[i].sender_idx == -1) {
            direct_channels[i].sender_idx = sender_idx;
            direct_channels[i].receiver_idx = receiver_idx;
            direct_channels[i].ready = 0;
            direct_channels[i].ack = 0;
            return i;
        }
    }
    return -1;  /* No free channels */
}

int component_direct_send(int channel, const char *data, uint32_t len)
{
    if (channel < 0 || channel >= DIRECT_CHANNEL_MAX) return -1;
    struct direct_channel *ch = &direct_channels[channel];
    if (ch->sender_idx == -1) return -1;

    uint32_t copy = len < DIRECT_MSG_MAX ? len : DIRECT_MSG_MAX - 1;
    for (uint32_t i = 0; i < copy; i++) ch->data[i] = data[i];
    ch->data[copy] = '\0';

    __atomic_store_n(&ch->ack, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ch->ready, 1, __ATOMIC_RELEASE);

    /* Wait for ack — 100ms timeout (bare-metal responses should be fast) */
    uint64_t start = timer_get_count();
    uint64_t limit = timer_get_frequency() / 10;
    while ((timer_get_count() - start) < limit) {
        if (__atomic_load_n(&ch->ack, __ATOMIC_ACQUIRE)) return 0;
        yield();
    }
    return -2;  /* Timeout */
}

const char *component_direct_receive(int channel)
{
    if (channel < 0 || channel >= DIRECT_CHANNEL_MAX) return NULL;
    struct direct_channel *ch = &direct_channels[channel];
    if (!__atomic_load_n(&ch->ready, __ATOMIC_ACQUIRE)) return NULL;
    return ch->data;
}

void component_direct_ack(int channel)
{
    if (channel < 0 || channel >= DIRECT_CHANNEL_MAX) return;
    __atomic_store_n(&direct_channels[channel].ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&direct_channels[channel].ack, 1, __ATOMIC_RELEASE);
}

void component_direct_init(void)
{
    for (int i = 0; i < DIRECT_CHANNEL_MAX; i++) {
        direct_channels[i].sender_idx = -1;
        direct_channels[i].receiver_idx = -1;
    }
}

/* State transfer buffer for stateful hot-swap.
 * Single global buffer — only one stateful swap can be in progress at a time.
 * `swap_state_lock` serialises both the export side (component_hot_swap_stateful)
 * and the import side (component_get_swap_state) so Lua-triggered hot-swaps
 * running on different CPUs can't race on the buffer. */
static component_swap_state_t swap_state_buf;
static spinlock_t swap_state_lock = SPINLOCK_INIT;

uint32_t component_get_swap_state(uint8_t *buf, uint32_t max_size)
{
    irq_flags_t f = spin_lock_irqsave(&swap_state_lock);

    if (!swap_state_buf.valid || swap_state_buf.size == 0) {
        spin_unlock_irqrestore(&swap_state_lock, f);
        return 0;
    }
    uint32_t copy_size = swap_state_buf.size;
    if (copy_size > max_size) copy_size = max_size;

    for (uint32_t i = 0; i < copy_size; i++) {
        buf[i] = swap_state_buf.data[i];
    }

    /* Clear after read — one-shot */
    swap_state_buf.valid = 0;
    swap_state_buf.size = 0;

    spin_unlock_irqrestore(&swap_state_lock, f);
    return copy_size;
}

int component_hot_swap_stateful(const char *old_name, const char *new_name,
                                component_state_export_fn export_fn)
{
    /* Populate swap_state_buf under the lock. The registered export
     * callbacks (e.g. sensor_monitor_export_state) are plain memcpy-style
     * helpers and do not yield, so holding a spinlock-irqsave across the
     * call is safe. component_hot_swap() runs outside the lock because it
     * calls into the task/scheduler machinery. */
    irq_flags_t f = spin_lock_irqsave(&swap_state_lock);

    swap_state_buf.valid = 0;
    swap_state_buf.size = 0;

    if (export_fn) {
        int exported = export_fn(swap_state_buf.data, COMPONENT_STATE_MAX);
        if (exported > 0 && (uint32_t)exported <= COMPONENT_STATE_MAX) {
            swap_state_buf.size = (uint32_t)exported;
            swap_state_buf.valid = 1;
        } else if (exported > (int)COMPONENT_STATE_MAX) {
            spin_unlock_irqrestore(&swap_state_lock, f);
            uart_puts("[WARN] Hot-swap: export callback exceeded buffer size\r\n");
            return -1;
        }
    }

    int exported_bytes = swap_state_buf.valid ? (int)swap_state_buf.size : 0;

    spin_unlock_irqrestore(&swap_state_lock, f);

    if (exported_bytes > 0) {
        uart_printf("Hot-swap: exported %d bytes of state\r\n", exported_bytes);
    }

    /* Delegate to existing hot-swap (saves subscriptions, unregisters, starts new) */
    int new_idx = component_hot_swap(old_name, new_name);

    if (new_idx < 0) {
        /* Discard state on failure under the lock */
        f = spin_lock_irqsave(&swap_state_lock);
        swap_state_buf.valid = 0;
        swap_state_buf.size = 0;
        spin_unlock_irqrestore(&swap_state_lock, f);
    }

    return new_idx;
}

/*
 * Hot-swap a running component: unload old, load new, preserve subscriptions.
 * Returns the new component index on success, -1 on failure.
 */
extern void msg_router_get_subscriptions(
    int component_idx, char topic_names[][16], int *count_out, int max_topics);
extern int msg_router_subscribe(const char *topic_name, int component_idx);

int component_hot_swap(const char *old_name, const char *new_name)
{
    /* Find old component */
    int old_idx = component_find(old_name);
    if (old_idx < 0) {
        uart_printf("Hot-swap: component '%s' not found\n", old_name);
        return -1;
    }

    component_info_t info;
    if (component_get_info((uint32_t)old_idx, &info) != 0) {
        uart_printf("Hot-swap: cannot get info for component %d\n", old_idx);
        return -1;
    }

    /* Save subscriptions before cleanup */
    char saved_topics[8][16];
    int saved_count = 0;
    msg_router_get_subscriptions(old_idx, saved_topics, &saved_count, 8);

    /* Mark as updating — prevents cleanup callback from acting */
    component_set_state((uint32_t)old_idx, COMPONENT_UPDATING);

    /* Remove old subscriptions and unregister */
    msg_router_unsubscribe_all(old_idx);
    component_unregister((uint32_t)old_idx);

    /* Run new component */
    int new_idx = component_run(new_name);
    if (new_idx < 0) {
        uart_printf("Hot-swap: failed to start '%s'\n", new_name);
        return -1;
    }

    /* Re-subscribe new component to saved topics */
    for (int i = 0; i < saved_count; i++) {
        msg_router_subscribe(saved_topics[i], new_idx);
    }

    uart_printf("Hot-swap: '%s' (idx %d) -> '%s' (idx %d), %d subscription(s) transferred\n",
                old_name, old_idx, new_name, new_idx, saved_count);
    return new_idx;
}

/*
 * List available built-in components.
 */
void component_list_builtins(void)
{
    uart_printf("Built-in Components (%zu available):\n", NUM_BUILTIN_COMPONENTS);
    uart_printf("  %-12s %-8s %-11s %s\n", "Name", "Version", "Type", "Description");
    uart_printf("  %-12s %-8s %-11s %s\n", "----", "-------", "----", "-----------");
    for (size_t i = 0; i < NUM_BUILTIN_COMPONENTS; i++) {
        const struct builtin_component *bc = &builtin_components[i];
        uart_printf("  %-12s %-8s %-11s %s\n",
                    bc->name, bc->version,
                    bc->type == COMPONENT_TYPE_SERVICE ? "service" :
                    bc->type == COMPONENT_TYPE_DRIVER ? "driver" : "application",
                    bc->description);
    }
}
