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
/* IPC used by message router (M7); echo service uses simple mailbox */
#include "uart.h"
#include "debug.h"
#include "arch.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Component Task Cleanup
 *
 * When a component's task exits, update the component state to Unloaded.
 * ============================================================================ */

struct component_task_ctx {
    int component_idx;
};

extern void msg_router_unsubscribe_all(int component_idx);

static void component_task_cleanup(void *arg)
{
    struct component_task_ctx *ctx = (struct component_task_ctx *)arg;
    if (ctx) {
        /* Only clean up if this component still owns the slot (not hot-swapped) */
        component_info_t info;
        if (component_get_info((uint32_t)ctx->component_idx, &info) == 0 &&
            (info.state == COMPONENT_TERMINATING || info.state == COMPONENT_RUNNING)) {
            msg_router_unsubscribe_all(ctx->component_idx);
            component_set_state((uint32_t)ctx->component_idx, COMPONENT_UNLOADED);
        }
        DEBUG_PRINT("Component %d task exited", ctx->component_idx);
    }
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
     * Use pit_ticks for time-based timeout (60 seconds). */
    extern volatile uint64_t pit_ticks;
    uint64_t timeout_tick = pit_ticks + 6000;  /* 60s at 100 Hz */
    while (pit_ticks < timeout_tick) {
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
    extern volatile uint64_t pit_ticks;
    uint64_t timeout_tick = pit_ticks + 6000;  /* 60s at 100 Hz */

    while (pit_ticks < timeout_tick) {
        char topic_buf[16];
        const char *data = msg_router_receive(comp_idx, topic_buf);
        if (data) {
            msgs_received++;
            uart_printf("[listener] [%s] \"%s\" (msg #%d)\n",
                        topic_buf, data, msgs_received);
            msg_router_ack(comp_idx);
            /* Reset timeout on activity */
            timeout_tick = pit_ticks + 6000;
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
};

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

    /* Create task */
    struct task *task = task_create_with_priority(
        bc->name, bc->entry, (void *)(uintptr_t)comp_idx, bc->priority);

    if (!task) {
        uart_printf("Failed to create task for component '%s'\n", name);
        component_unregister((uint32_t)comp_idx);
        return -1;
    }

    /* Pre-initialize echo service state (before task starts) */
    if (bc->entry == echo_service_entry) {
        echo_mailbox.ready = 0;
        echo_mailbox.ack = 0;
        echo_running = 1;  /* Set BEFORE task starts so send sees it immediately */
    }

    /* Pin component tasks to CPU 0 so they share the scheduler with shell
     * and IPC via shared memory is immediately visible (same CPU). */
    task->cpu_affinity = 0;

    /* Link task to component */
    static struct component_task_ctx cleanup_ctxs[COMPONENT_MAX_COUNT];
    cleanup_ctxs[comp_idx].component_idx = comp_idx;
    task_set_cleanup(task, component_task_cleanup, &cleanup_ctxs[comp_idx]);

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

    /* Copy message to shared mailbox */
    size_t len = 0;
    while (message[len] && len < 63) {
        ((volatile char *)echo_mailbox.data)[len] = message[len];
        len++;
    }
    ((volatile char *)echo_mailbox.data)[len] = '\0';

    /* Signal echo service and wait for acknowledgment.
     * Use yield() + pit_ticks timeout (5 seconds). */
    __atomic_store_n(&echo_mailbox.ack, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&echo_mailbox.ready, 1, __ATOMIC_RELEASE);

    /* Both shell and echo are IDLE priority — yield() round-robins between them */
    extern volatile uint64_t pit_ticks;
    uint64_t send_timeout = pit_ticks + 500;  /* 5s at 100 Hz */
    while (pit_ticks < send_timeout) {
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
