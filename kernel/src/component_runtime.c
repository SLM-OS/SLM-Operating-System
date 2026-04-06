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
#include "ipc.h"
#include "uart.h"
#include "debug.h"
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

static void component_task_cleanup(void *arg)
{
    struct component_task_ctx *ctx = (struct component_task_ctx *)arg;
    if (ctx) {
        component_set_state((uint32_t)ctx->component_idx, COMPONENT_UNLOADED);
        DEBUG_PRINT("Component %d task exited → Unloaded", ctx->component_idx);
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

/* Global echo queue — accessible to message senders (volatile for cross-CPU visibility) */
volatile struct msg_queue *echo_service_queue;

static void echo_service_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;

    /* Create IPC queue for receiving messages */
    struct msg_queue *q = msg_queue_create(8, 64);
    echo_service_queue = q;
    __asm__ volatile("mfence" ::: "memory");  /* Ensure visible to other CPUs */
    if (!q) {
        uart_printf("[echo] Failed to create message queue\n");
        component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
        return;
    }

    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);
    uart_printf("[echo] Started (component %d), listening for messages...\n", comp_idx);

    char buf[64];
    int msgs_received = 0;
    int idle_polls = 0;
    extern void sleep_ms(uint32_t ms);

    /* Poll for messages — non-blocking recv with sleep between polls */
    while (idle_polls < 40) {  /* ~20 seconds max idle */
        int ret = msg_recv(q, buf, 0);  /* Non-blocking */
        if (ret == IPC_OK) {
            buf[63] = '\0';
            msgs_received++;
            idle_polls = 0;  /* Reset idle counter on message */
            uart_printf("[echo] Received: \"%s\" (msg #%d)\n", buf, msgs_received);
        } else {
            idle_polls++;
            sleep_ms(500);  /* Poll every 500ms */
        }
    }

    if (msgs_received > 0) {
        uart_printf("[echo] Idle timeout after %d messages\n", msgs_received);
    }

    uart_printf("[echo] Done (%d messages)\n", msgs_received);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
    echo_service_queue = NULL;
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
        .priority = COMPONENT_PRIORITY_NORMAL,
        .entry = echo_service_entry,
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

    /* Link task to component */
    /* Store cleanup context on the task */
    static struct component_task_ctx cleanup_ctxs[COMPONENT_MAX_COUNT];
    cleanup_ctxs[comp_idx].component_idx = comp_idx;
    task_set_cleanup(task, component_task_cleanup, &cleanup_ctxs[comp_idx]);

    /* Add to scheduler */
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
    struct msg_queue *q = (struct msg_queue *)echo_service_queue;
    if (!q) {
        uart_printf("Echo service not running\n");
        return -1;
    }

    char buf[64];
    size_t len = 0;
    while (message[len] && len < 63) {
        buf[len] = message[len];
        len++;
    }
    buf[len] = '\0';

    int ret = msg_send(q, buf, 0);
    if (ret != IPC_OK) {
        uart_printf("Failed to send message (err=%d)\n", ret);
        return -1;
    }

    return 0;
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
