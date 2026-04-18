/**
 * lua_slm.c - Lua Integration for SLM-OS
 *
 * Provides Lua scripting support with kernel API bindings.
 * Creates the 'slm' table with functions to access kernel features.
 */

#include "lua_slm.h"
#include "debug.h"
#include "timer.h"
#include "pmm.h"
#include "sched.h"
#include "task.h"
#include "shell.h"
#include "vfs.h"
#include "component.h"
#include "slm_ffi.h"
#include "sched_policy.h"
#include "ipc.h"
#include "smp.h"
#include "string.h"
#if defined(ENABLE_NETWORKING)
#include "shell_io_tcp.h"
#include "tcp_shell_server.h"
#include "shell_session.h"   /* MAX_TCP_SHELL_SESSIONS */
#include "net.h"
#endif
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#endif
#if defined(CONFIG_AI_SCHEDULER)
#include "ai_types.h"   /* ai_sched_action, ai_decode_action — #211 review fix */
#endif

/* Lua headers - note: these may include stdio.h from newlib */
#include "../lib/lua/src/lua.h"
#include "../lib/lua/src/lauxlib.h"
#include "../lib/lua/src/lualib.h"

/* Version string */
#define SLM_VERSION "0.1.0"

/* ============================================================================
 * SLM-OS Kernel Bindings
 * ============================================================================ */

/**
 * slm.print(msg) - Print message to console
 */
static int l_print(lua_State *L) {
    if (!L) return 0;
    int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) shell_printf("\t");
        if (lua_isstring(L, i)) {
            shell_printf("%s", lua_tostring(L, i));
        } else if (lua_isnil(L, i)) {
            shell_printf("nil");
        } else if (lua_isboolean(L, i)) {
            shell_printf("%s", lua_toboolean(L, i) ? "true" : "false");
        } else if (lua_isnumber(L, i)) {
            lua_Number n = lua_tonumber(L, i);
            shell_printf("%d", (int)n);
        } else {
            shell_printf("%s: %p", luaL_typename(L, i), lua_topointer(L, i));
        }
    }
    shell_printf("\n");
    return 0;
}

/**
 * slm.uptime() - Get system uptime in milliseconds
 */
static int l_uptime(lua_State *L) {
    if (!L) return 0;
    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();
    uint64_t ms = count / (freq / 1000);
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

/**
 * slm.mem_stats() - Get memory statistics
 * Returns table: {total_kb, free_kb, used_kb}
 */
static int l_mem_stats(lua_State *L) {
    if (!L) return 0;
    size_t total_pages = pmm_get_total_pages();
    size_t free_pages = pmm_get_free_pages();
    size_t used_pages = total_pages - free_pages;

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)(total_pages * 4));
    lua_setfield(L, -2, "total_kb");

    lua_pushinteger(L, (lua_Integer)(free_pages * 4));
    lua_setfield(L, -2, "free_kb");

    lua_pushinteger(L, (lua_Integer)(used_pages * 4));
    lua_setfield(L, -2, "used_kb");

    return 1;
}

/**
 * slm.tasks() - Get list of tasks
 * Returns array of tables: {{id, name, state, cpu}, ...}
 */
static int l_tasks(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    int idx = 1;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_get(i);
        if (t == NULL || t->state == TASK_TERMINATED) continue;

        lua_newtable(L);

        lua_pushinteger(L, t->id);
        lua_setfield(L, -2, "id");

        lua_pushstring(L, t->name);
        lua_setfield(L, -2, "name");

        const char *state_str;
        switch (t->state) {
            case TASK_READY: state_str = "ready"; break;
            case TASK_RUNNING: state_str = "running"; break;
            case TASK_BLOCKED: state_str = "blocked"; break;
            case TASK_TERMINATED: state_str = "terminated"; break;
            default: state_str = "unknown"; break;
        }
        lua_pushstring(L, state_str);
        lua_setfield(L, -2, "state");

        lua_pushinteger(L, t->assigned_cpu);
        lua_setfield(L, -2, "cpu");

        lua_pushinteger(L, t->priority);
        lua_setfield(L, -2, "priority");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/* Forward-declared: defined alongside the msg_subscribe machinery
 * further down the file (#207). */
static void lua_msg_drain(lua_State *L);

/**
 * slm.sleep(ms) - Sleep for milliseconds (busy wait)
 */
static int l_sleep(lua_State *L) {
    if (!L) return 0;
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms > 0) {
        /* Busy wait - proper sleep would require scheduler support */
        uint64_t start = timer_get_count();
        uint64_t freq = timer_get_frequency();
        uint64_t ticks = (uint64_t)ms * (freq / 1000);
        while ((timer_get_count() - start) < ticks) {
            yield();  /* Let other tasks run while waiting */
            lua_msg_drain(L);  /* Dispatch msg_subscribe callbacks (#207) */
        }
    }
    return 0;
}

/**
 * slm.yield() - Yield CPU to scheduler
 *
 * Also drains any pending msg_subscribe callbacks (#207) so scripts that
 * yield periodically see subscriber dispatches without calling
 * slm.msg_drain() explicitly.
 */
static int l_yield(lua_State *L) {
    if (!L) return 0;
    yield();
    lua_msg_drain(L);
    return 0;
}

/**
 * slm.version() - Get SLM-OS version string
 */
static int l_version(lua_State *L) {
    if (!L) return 0;
    lua_pushstring(L, "SLM-OS " SLM_VERSION);
    return 1;
}

/**
 * slm.cpu_count() - Get number of CPUs
 */
static int l_cpu_count(lua_State *L) {
    if (!L) return 0;
    lua_pushinteger(L, MAX_CPUS);
    return 1;
}

/**
 * slm.cpu_id() - Get current CPU ID
 */
static int l_cpu_id(lua_State *L) {
    if (!L) return 0;
#if defined(PLATFORM_X86_64)
    lua_pushinteger(L, 0);
#else
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    lua_pushinteger(L, (lua_Integer)(mpidr & 0xFF));
#endif
    return 1;
}

/* ============================================================================
 * Component Management Bindings
 * ============================================================================ */

/**
 * slm.component_count() - Get number of registered components
 */
static int l_component_count(lua_State *L) {
    if (!L) return 0;
    lua_pushinteger(L, (lua_Integer)component_count());
    return 1;
}

/**
 * slm.component_list() - Get list of all components
 * Returns array of tables: {{name, version, type, priority, state, task_id}, ...}
 */
static int l_component_list(lua_State *L) {
    if (!L) return 0;
    uint32_t count = component_count();
    lua_createtable(L, (int)count, 0);

    int idx = 1;
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_info_t info;
        if (component_get_info(i, &info) != 0)
            continue;

        lua_createtable(L, 0, 7);

        lua_pushstring(L, (const char *)info.name);
        lua_setfield(L, -2, "name");

        lua_pushstring(L, (const char *)info.version);
        lua_setfield(L, -2, "version");

        lua_pushstring(L, component_type_name((component_type_t)info.component_type));
        lua_setfield(L, -2, "type");

        lua_pushinteger(L, info.priority);
        lua_setfield(L, -2, "priority");

        lua_pushstring(L, component_state_name((component_state_t)info.state));
        lua_setfield(L, -2, "state");

        lua_pushinteger(L, info.task_id);
        lua_setfield(L, -2, "task_id");

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "index");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.component_find(name) - Find component by name
 * Returns index or nil if not found
 */
static int l_component_find(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = component_find(name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_run(name) - Run a built-in component
 * Returns index or nil on failure
 */
static int l_component_run(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = component_run(name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_hot_swap(old_name, new_name) - Hot-swap a component
 * Returns new index or nil on failure
 */
static int l_component_hot_swap(lua_State *L) {
    if (!L) return 0;
    const char *old_name = luaL_checkstring(L, 1);
    const char *new_name = luaL_checkstring(L, 2);
    int idx = component_hot_swap(old_name, new_name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_hot_swap_stateful(old_name, new_name) - Stateful hot-swap
 * Exports state from old component, transfers to new.
 * Currently supports sensor_monitor (transfers alert count).
 * Returns new index or nil on failure.
 */
/* Export function registry for stateful hot-swap.
 * Components register their export callback here. */
struct state_export_entry {
    const char *name;
    component_state_export_fn fn;
};

extern int sensor_monitor_export_state(uint8_t *buf, uint32_t max_size);

static const struct state_export_entry export_registry[] = {
    { "sensor_monitor", sensor_monitor_export_state },
    { NULL, NULL }
};

static component_state_export_fn find_export_fn(const char *name)
{
    for (int i = 0; export_registry[i].name; i++) {
        const char *a = name;
        const char *b = export_registry[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return export_registry[i].fn;
    }
    return NULL;
}

static int l_component_hot_swap_stateful(lua_State *L) {
    if (!L) return 0;
    const char *old_name = luaL_checkstring(L, 1);
    const char *new_name = luaL_checkstring(L, 2);

    component_state_export_fn export_fn = find_export_fn(old_name);

    int idx = component_hot_swap_stateful(old_name, new_name, export_fn);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/* ============================================================================
 * Model Memory Bindings
 * ============================================================================ */

/**
 * slm.model_stats() - Get model memory pool statistics
 * Returns table: {weights={...}, workspace={...}}
 */
static int l_model_stats(lua_State *L) {
    if (!L) return 0;
    RustPoolStats w = rust_weight_pool_stats();
    RustPoolStats ws = rust_workspace_pool_stats();

    lua_createtable(L, 0, 2);

    /* weights sub-table */
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)w.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    lua_pushinteger(L, (lua_Integer)w.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)w.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)w.shared_blocks);
    lua_setfield(L, -2, "shared_blocks");
    lua_pushinteger(L, (lua_Integer)w.peak_usage);
    lua_setfield(L, -2, "peak_usage");
    lua_setfield(L, -2, "weights");

    /* workspace sub-table */
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)ws.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    lua_pushinteger(L, (lua_Integer)ws.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)ws.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)ws.shared_blocks);
    lua_setfield(L, -2, "shared_blocks");
    lua_pushinteger(L, (lua_Integer)ws.peak_usage);
    lua_setfield(L, -2, "peak_usage");
    lua_setfield(L, -2, "workspace");

    return 1;
}

/* ============================================================================
 * Model Inference Bindings
 * ============================================================================ */

/**
 * slm.model_infer(index) - Run inference on a loaded model
 * Returns predicted class (integer) or -1 on error
 */
static int l_model_infer(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    int result = rust_infer_classify((uint32_t)idx);
    lua_pushinteger(L, result);
    return 1;
}

/**
 * slm.model_find(name) - Find a model by name
 * Returns model index or -1 if not found
 */
static int l_model_find(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = rust_model_find(name);
    lua_pushinteger(L, idx);
    return 1;
}

/**
 * slm.model_load_mnist() - Load the built-in MNIST model
 * Returns model index or -1 on failure
 */
static int l_model_load_mnist(lua_State *L) {
    if (!L) return 0;
    int idx = rust_model_load_builtin_mnist();
    lua_pushinteger(L, idx);
    return 1;
}

/**
 * slm.model_pin(index) - Pin a model to prevent LRU eviction
 * Returns 0 on success, -1 on error
 */
static int l_model_pin(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, rust_model_pin((uint32_t)idx));
    return 1;
}

/**
 * slm.model_unpin(index) - Unpin a model (allow LRU eviction)
 * Returns 0 on success, -1 on error
 */
static int l_model_unpin(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, rust_model_unpin((uint32_t)idx));
    return 1;
}

/**
 * slm.model_preload(name) - Preload a model in a background task (#64)
 * Currently only "mnist" is supported for async preload.
 * Returns 0 on success (preload started), -1 on error.
 */
static int l_model_preload(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);

    /* Delegate to shell_execute which drives the preload_task_entry
     * background task. This reuses the shell's preload infrastructure
     * without duplicating the task-spawn logic. */
    char cmd[64];
    extern int uart_snprintf(char *buf, size_t size, const char *fmt, ...);
    uart_snprintf(cmd, sizeof(cmd), "model preload %s", name);
    int rc = shell_execute(cmd);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.model_preload_wait(name [, timeout_ms]) - Wait for async preload (#64)
 * Returns model index (>=0) on success, negative on error/timeout.
 */
static int l_model_preload_wait(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    uint32_t timeout = 5000;
    if (lua_gettop(L) >= 2) {
        timeout = (uint32_t)luaL_checkinteger(L, 2);
    }
    extern int model_preload_wait(const char *name, uint32_t timeout_ms);
    int rc = model_preload_wait(name, timeout);
    lua_pushinteger(L, rc);
    return 1;
}

/* ============================================================================
 * Message Router Bindings
 * ============================================================================ */

/**
 * slm.msg_publish(topic, data) - Publish a message to a topic
 * Returns number of subscribers that received the message
 */
static int l_msg_publish(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    int delivered = msg_router_publish((const uint8_t *)topic, (const uint8_t *)data);
    lua_pushinteger(L, delivered);
    return 1;
}

/**
 * slm.msg_publish_priority(topic, data, priority) - Publish with priority
 * priority: 0 = normal, higher = more urgent
 * Returns number of subscribers that received the message
 */
static int l_msg_publish_priority(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    int prio = (int)luaL_checkinteger(L, 3);
    if (prio < 0) prio = 0;
    if (prio > 255) prio = 255;
    int delivered = msg_router_publish_priority(
        (const uint8_t *)topic, (const uint8_t *)data, (uint8_t)prio);
    lua_pushinteger(L, delivered);
    return 1;
}

/* ============================================================================
 * Lua Message Subscriptions (#207)
 * ============================================================================
 *
 * Lua scripts register callbacks against msg_router topics; messages are
 * dispatched on the shell task's stack at yield / sleep / read_line. The
 * callbacks are not truly concurrent — option (1) from the issue — but
 * match the existing single-threaded Lua model. A single sentinel
 * component_idx (LUA_MSG_SUB_IDX) represents the Lua mailbox at the
 * router; the Lua side tracks which callbacks care about which topics.
 */

/* Sentinel component id for the Lua subscriber pool.
 *
 * Must be in [0, MAX_COMPONENTS=32) — msg_router_ack() rejects indices
 * outside that range so we cannot use an "obviously large" sentinel
 * like 1000. We pick the top of the range (31) so it sits above the
 * real component slots (0..COMPONENT_MAX_COUNT-1 = 0..15). If
 * COMPONENT_MAX_COUNT ever grows to where it might collide with the
 * sentinel, the static_assert below traps it at build time — PR #217
 * review flagged the silent-collision risk. If that assert ever fires,
 * bump MSG_ROUTER's MAX_COMPONENTS in runtime/src/msg_router.rs and
 * move the sentinel into the freshly-opened range. */
#define LUA_MSG_SUB_IDX  31
_Static_assert(LUA_MSG_SUB_IDX > COMPONENT_MAX_COUNT,
               "LUA_MSG_SUB_IDX must not collide with native component slots; "
               "grow MAX_COMPONENTS in runtime/src/msg_router.rs first");
_Static_assert(LUA_MSG_SUB_IDX < 32,
               "LUA_MSG_SUB_IDX must be < msg_router's MAX_COMPONENTS "
               "or msg_router_ack silently drops acks");

/* Maximum concurrent Lua subscriptions across all active lua_States.
 * Subscriptions are lightweight (one Lua registry slot + ~20 bytes) so
 * the cap is mostly a sanity ceiling. */
#define LUA_MSG_MAX_SUBS 16

/* Topic buffer in the router — keep in sync with TOPIC_NAME_LEN in
 * runtime/src/msg_router.rs. */
#define LUA_MSG_TOPIC_LEN 16

extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);

struct lua_msg_sub {
    int handle;                         /* Caller-visible id (1, 2, ...) */
    int ref;                            /* luaL_ref slot for the callback */
    lua_State *L;                       /* State owning ref (for cleanup) */
    char pattern[LUA_MSG_TOPIC_LEN];    /* Subscribed topic or wildcard */
    uint8_t wildcard;                   /* 1 if pattern ends in '/*' */
    uint8_t prefix_len;                 /* Pattern length excl. trailing '*' */
    uint8_t active;
};

static struct lua_msg_sub lua_msg_subs[LUA_MSG_MAX_SUBS];
static int lua_msg_next_handle = 1;
static int lua_msg_router_subscribed = 0;  /* Lazy msg_router registration */

/* Match a Lua subscription pattern against an actual delivered topic.
 * Exact match OR (for patterns ending in "/*") prefix match up to the '*'. */
static int lua_msg_topic_matches(const struct lua_msg_sub *sub, const char *topic)
{
    if (sub->wildcard) {
        for (uint8_t i = 0; i < sub->prefix_len; i++) {
            if (topic[i] == '\0' || topic[i] != sub->pattern[i]) return 0;
        }
        return 1;
    }
    /* Exact */
    for (uint8_t i = 0; i < LUA_MSG_TOPIC_LEN; i++) {
        if (sub->pattern[i] != topic[i]) return 0;
        if (sub->pattern[i] == '\0') return 1;
    }
    return 1;
}

/* Drain pending router messages for the Lua subscriber pool and invoke
 * matching callbacks on the given state. Safe to call at any yield point;
 * any error raised by a callback is swallowed (logged to UART) so one
 * bad subscriber cannot wedge the drain loop. */
static void lua_msg_drain(lua_State *L)
{
    if (!L || !lua_msg_router_subscribed) return;

    /* The drain cap bounds work done per yield/sleep/read_line so a
     * flooded router mailbox cannot starve the main Lua thread. PR #217
     * review raised concerns about message loss under high throughput.
     * Messages are NOT lost — the router holds the next one until the
     * next drain — but we do introduce latency up to one drain cycle
     * per LUA_MSG_DRAIN_BATCH messages. 256 is tuned to be much larger
     * than realistic demo load while still cheap enough that we return
     * to the caller within a few milliseconds at worst. Scripts with
     * genuinely high subscriber fan-in should call slm.msg_drain() in
     * a tight loop themselves. */
#define LUA_MSG_DRAIN_BATCH 256
    for (int guard = 0; guard < LUA_MSG_DRAIN_BATCH; guard++) {
        char topic_buf[LUA_MSG_TOPIC_LEN];
        const char *data = msg_router_receive(LUA_MSG_SUB_IDX, topic_buf);
        if (!data) break;

        for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
            struct lua_msg_sub *s = &lua_msg_subs[i];
            if (!s->active || s->L != L) continue;
            if (!lua_msg_topic_matches(s, topic_buf)) continue;

            lua_rawgeti(L, LUA_REGISTRYINDEX, s->ref);
            lua_pushstring(L, topic_buf);
            lua_pushstring(L, data);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(L, -1);
                shell_printf("[lua msg_subscribe] callback error: %s\n",
                            err ? err : "(unknown)");
                lua_pop(L, 1);
            }
        }

        msg_router_ack(LUA_MSG_SUB_IDX);
    }
}

/**
 * slm.msg_subscribe(topic, fn) - Register a Lua callback for a topic.
 *
 * The callback is invoked as `fn(topic, data)` on the next drain point
 * (`slm.yield`, `slm.sleep`, `slm.read_line`) with pending matching
 * messages. Topics ending in "/*" match any topic with that prefix.
 *
 * Returns a subscription handle (integer >= 1) on success, nil if the
 * Lua subscription pool is full (LUA_MSG_MAX_SUBS).
 */
static int l_msg_subscribe(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Copy pattern into fixed buffer, compute wildcard metadata */
    char buf[LUA_MSG_TOPIC_LEN];
    size_t tlen = 0;
    while (topic[tlen] && tlen < LUA_MSG_TOPIC_LEN - 1) {
        buf[tlen] = topic[tlen];
        tlen++;
    }
    buf[tlen] = '\0';

    int wildcard = 0;
    uint8_t prefix_len = (uint8_t)tlen;
    if (tlen >= 2 && buf[tlen - 1] == '*') {
        wildcard = 1;
        prefix_len = (uint8_t)(tlen - 1);  /* everything before the '*' */
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        if (!lua_msg_subs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        lua_pushnil(L);
        return 1;
    }

    /* Reference the callback (arg 2 is on top after the checks) */
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    struct lua_msg_sub *s = &lua_msg_subs[slot];
    s->handle = lua_msg_next_handle++;
    s->ref = ref;
    s->L = L;
    for (size_t i = 0; i < LUA_MSG_TOPIC_LEN; i++) s->pattern[i] = buf[i];
    s->wildcard = (uint8_t)wildcard;
    s->prefix_len = prefix_len;
    s->active = 1;

    /* msg_router_subscribe is NOT idempotent — each call consumes a
     * fresh subscriber slot on the topic and delivers a copy of every
     * message into its own mailbox. Dedup at the Lua layer so a single
     * topic only has one router subscription no matter how many Lua
     * callbacks want it. */
    int already_subscribed_at_router = 0;
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        if (i == slot || !lua_msg_subs[i].active) continue;
        int eq = 1;
        for (int k = 0; k < LUA_MSG_TOPIC_LEN; k++) {
            if (lua_msg_subs[i].pattern[k] != s->pattern[k]) { eq = 0; break; }
            if (s->pattern[k] == '\0') break;
        }
        if (eq) { already_subscribed_at_router = 1; break; }
    }
    if (!already_subscribed_at_router) {
        msg_router_subscribe((const uint8_t *)buf, LUA_MSG_SUB_IDX);
    }
    lua_msg_router_subscribed = 1;

    lua_pushinteger(L, (lua_Integer)s->handle);
    return 1;
}

/**
 * slm.msg_unsubscribe(handle) - Remove a Lua subscription.
 *
 * Returns true on success, false if the handle is unknown. The router-side
 * subscription is left in place — msg_router does not expose per-topic
 * unsubscribe — but once no Lua sub matches, the drain loop stops
 * delivering the message to Lua (and ack discards it from the mailbox).
 */
static int l_msg_unsubscribe(lua_State *L) {
    if (!L) return 0;
    lua_Integer handle = luaL_checkinteger(L, 1);
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        struct lua_msg_sub *s = &lua_msg_subs[i];
        if (s->active && s->handle == (int)handle) {
            luaL_unref(s->L, LUA_REGISTRYINDEX, s->ref);
            s->active = 0;
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

/**
 * slm.msg_drain() - Manually poll pending messages and invoke callbacks.
 *
 * Normally scripts don't need to call this — drains happen automatically
 * at slm.yield / slm.sleep / slm.read_line. Exposed for scripts that
 * compute without yielding and want to tick the subscriber pipeline.
 */
static int l_msg_drain(lua_State *L) {
    if (!L) return 0;
    lua_msg_drain(L);
    return 0;
}

/* Teardown helper — release all Lua refs belonging to this state and
 * ask the router to drop the Lua mailbox if no Lua sub remains. Called
 * from lua_slm_close. */
static void lua_msg_subs_cleanup(lua_State *L) {
    int any_left = 0;
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        struct lua_msg_sub *s = &lua_msg_subs[i];
        if (!s->active) continue;
        if (s->L == L) {
            luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
            s->active = 0;
        } else {
            any_left = 1;
        }
    }
    if (!any_left) {
        msg_router_unsubscribe_all(LUA_MSG_SUB_IDX);
        lua_msg_router_subscribed = 0;
    }
}

/* ============================================================================
 * Scheduler Bindings
 * ============================================================================ */

/**
 * slm.sched_policy() - Get current scheduler policy name
 * Returns string
 */
static int l_sched_policy(lua_State *L) {
    if (!L) return 0;
    lua_pushstring(L, sched_get_policy());
    return 1;
}

/**
 * slm.sched_stats() - Get scheduler statistics
 * Returns table: {task_count, ready_count, context_switches, timer_ticks, policy}
 */
static int l_sched_stats(lua_State *L) {
    if (!L) return 0;
    struct sched_stats stats;
    scheduler_get_stats(&stats);

    lua_createtable(L, 0, 5);

    lua_pushinteger(L, (lua_Integer)stats.task_count);
    lua_setfield(L, -2, "task_count");

    lua_pushinteger(L, (lua_Integer)stats.ready_count);
    lua_setfield(L, -2, "ready_count");

    lua_pushinteger(L, (lua_Integer)stats.context_switches);
    lua_setfield(L, -2, "context_switches");

    lua_pushinteger(L, (lua_Integer)stats.timer_ticks);
    lua_setfield(L, -2, "timer_ticks");

    lua_pushstring(L, sched_get_policy());
    lua_setfield(L, -2, "policy");

    return 1;
}

/**
 * slm.sched_set_policy(name) - Switch scheduler policy by name
 * Returns true on success, false on failure (unknown policy name)
 */
static int l_sched_set_policy(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    const struct sched_policy_ops *p = sched_find_policy(name);
    if (!p) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int rc = sched_set_policy(p);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.sched_policy_list() - List available scheduler policies
 * Returns array of tables: {{name, active}, ...}
 */
static int l_sched_policy_list(lua_State *L) {
    if (!L) return 0;
    int count = sched_policy_count();
    const char *current = sched_get_policy();

    lua_createtable(L, count, 0);

    int idx = 1;
    for (int i = 0; i < count; i++) {
        const struct sched_policy_ops *p = sched_policy_get(i);
        if (!p) continue;

        lua_createtable(L, 0, 2);
        lua_pushstring(L, p->name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, strcmp(p->name, current) == 0);
        lua_setfield(L, -2, "active");
        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.ai_sched_stats() - Get AI scheduler statistics (if CONFIG_AI_SCHEDULER enabled)
 * Returns table: {decisions, fallbacks, avg_latency_ns, histogram={...}}
 * Returns nil if AI scheduler is not compiled in.
 */
static int l_ai_sched_stats(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_SCHEDULER)
    extern void sched_ai_get_stats(const char *, uint32_t *, uint32_t *,
                                   uint64_t *, const uint32_t **, int *);

    const char *policy = sched_get_policy();
    uint32_t decisions = 0, fallbacks = 0;
    uint64_t avg_lat = 0;
    const uint32_t *hist = NULL;
    int n_actions = 0;

    sched_ai_get_stats(policy, &decisions, &fallbacks, &avg_lat, &hist, &n_actions);

    lua_createtable(L, 0, 5);

    lua_pushstring(L, policy);
    lua_setfield(L, -2, "policy");

    lua_pushinteger(L, (lua_Integer)decisions);
    lua_setfield(L, -2, "decisions");

    lua_pushinteger(L, (lua_Integer)fallbacks);
    lua_setfield(L, -2, "fallbacks");

    lua_pushinteger(L, (lua_Integer)avg_lat);
    lua_setfield(L, -2, "avg_latency_ns");

    if (hist && n_actions > 0) {
        lua_createtable(L, n_actions, 0);
        for (int i = 0; i < n_actions; i++) {
            lua_pushinteger(L, (lua_Integer)hist[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "histogram");
    }
#else
    lua_pushnil(L);
#endif
    return 1;
}

/* ============================================================================
 * Lua-defined task trampoline (#208)
 * ============================================================================
 *
 * slm.task_create(name, fn) dumps `fn` to Lua bytecode, stashes the blob in
 * a PMM-backed buffer, and spawns a kernel task whose entry point creates
 * a fresh lua_State, loads the bytecode, calls the function, and exits.
 * Isolates per-task state (no shared upvalues) at the cost of an extra
 * lua_State per task — the shared Lua heap tracks live-state count so
 * heap_reset only fires when every state is closed.
 *
 * Context pool is static; no malloc in the binding hot path.
 */

#define LUA_TASK_MAX_CTX 4        /* bumped if demos need more concurrency */
#define LUA_TASK_NAME_LEN 32

struct lua_task_ctx {
    uint8_t active;
    char name[LUA_TASK_NAME_LEN];
    uint8_t *bytecode;
    size_t bytecode_len;
    size_t bytecode_pages;
};

static struct lua_task_ctx lua_task_ctxs[LUA_TASK_MAX_CTX];

/* Accumulator for lua_dump — writes bytecode into a PMM-backed buffer as
 * lua_dump streams chunks. Tracks a current offset and a capacity; grows
 * the buffer by re-allocating a bigger region when needed. */
struct lua_dump_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t pages;
    int oom;
};

static int lua_dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    struct lua_dump_buf *b = (struct lua_dump_buf *)ud;
    if (b->oom) return 1;

    size_t need = b->len + sz;
    if (need > b->cap) {
        /* Double the buffer (or round up to multiple of page size). */
        size_t new_pages = b->pages * 2;
        if (new_pages < (need + 4095) / 4096) new_pages = (need + 4095) / 4096;
        if (new_pages < 1) new_pages = 1;
        uint8_t *nb = (uint8_t *)pmm_alloc_pages(new_pages);
        if (!nb) { b->oom = 1; return 1; }
        for (size_t i = 0; i < b->len; i++) nb[i] = b->data[i];
        if (b->data) pmm_free_pages(b->data, b->pages);
        b->data = nb;
        b->cap = new_pages * 4096;
        b->pages = new_pages;
    }
    const uint8_t *src = (const uint8_t *)p;
    for (size_t i = 0; i < sz; i++) b->data[b->len + i] = src[i];
    b->len += sz;
    return 0;
}

static void lua_task_entry(void *arg)
{
    struct lua_task_ctx *ctx = (struct lua_task_ctx *)arg;
    lua_State *L = lua_slm_newstate();
    if (!L) {
        shell_printf("[lua task %s] failed to create state\n", ctx->name);
        goto cleanup;
    }

    if (luaL_loadbuffer(L, (const char *)ctx->bytecode, ctx->bytecode_len,
                        ctx->name) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        shell_printf("[lua task %s] error: %s\n",
                    ctx->name, err ? err : "(unknown)");
    }

    lua_slm_close(L);

cleanup:
    if (ctx->bytecode) pmm_free_pages(ctx->bytecode, ctx->bytecode_pages);
    ctx->active = 0;
    task_exit();
}

/**
 * slm.task_create(name, fn) - Spawn a new kernel task running a Lua function.
 *
 * `fn` is serialized to Lua bytecode via `lua_dump` and executed in a fresh
 * lua_State on the new task's stack. Returns the task id (>=1) on success
 * or nil on failure (full pool, allocation failure, bad arguments).
 *
 * The new state has no access to upvalues or globals from the caller —
 * it starts empty with the same `slm` module and standard libs as the
 * shell REPL. Scripts that need to pass data in must use the msg_router,
 * shared files, or arguments encoded into `fn`'s captured upvalues before
 * the `lua_dump` (which dumps bytecode only — upvalues are NOT captured).
 */
static int l_task_create(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Grab a ctx slot before doing any allocation. */
    struct lua_task_ctx *ctx = NULL;
    for (int i = 0; i < LUA_TASK_MAX_CTX; i++) {
        if (!lua_task_ctxs[i].active) { ctx = &lua_task_ctxs[i]; break; }
    }
    if (!ctx) {
        lua_pushnil(L);
        return 1;
    }

    /* Copy name. */
    size_t nlen = 0;
    while (name[nlen] && nlen < LUA_TASK_NAME_LEN - 1) {
        ctx->name[nlen] = name[nlen];
        nlen++;
    }
    ctx->name[nlen] = '\0';

    /* Serialize the function via lua_dump. Start with 1 page (4 KB) which
     * handles any realistic short function; writer will grow as needed. */
    struct lua_dump_buf b = {0};
    b.pages = 1;
    b.cap = 4096;
    b.data = (uint8_t *)pmm_alloc_pages(1);
    if (!b.data) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushvalue(L, 2);
    int rc = lua_dump(L, lua_dump_writer, &b, /*strip=*/1);
    lua_pop(L, 1);  /* pop the function copy */

    if (rc != 0 || b.oom) {
        if (b.data) pmm_free_pages(b.data, b.pages);
        lua_pushnil(L);
        return 1;
    }

    ctx->bytecode = b.data;
    ctx->bytecode_len = b.len;
    ctx->bytecode_pages = b.pages;
    ctx->active = 1;

    struct task *t = task_create_with_priority(ctx->name, lua_task_entry,
                                               ctx, TASK_PRIORITY_NORMAL);
    if (!t) {
        pmm_free_pages(ctx->bytecode, ctx->bytecode_pages);
        ctx->active = 0;
        lua_pushnil(L);
        return 1;
    }

    scheduler_add_task(t);

    lua_pushinteger(L, (lua_Integer)t->id);
    return 1;
}

/**
 * slm.task_kill(task_id) - Terminate a task.
 *
 * Wraps scheduler_remove_task + task_destroy. Refuses to kill the
 * currently running task (would race with the scheduler on its own stack)
 * and refuses the idle task id 0. Returns true on success, false on any
 * refusal (unknown id, idle, self, terminated).
 */
static int l_task_kill(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    if (task_id <= 0) {           /* 0 is idle, negative is invalid */
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Don't kill the task that's asking — task_exit is the right path for
     * self-termination and it doesn't return. */
    if (t == task_current()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    scheduler_remove_task(t);
    task_destroy(t);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * slm.task_set_priority(task_id, priority) - Change a task's priority.
 *
 * Priority must be in [0, 7] (TASK_PRIORITY_IDLE..TASK_PRIORITY_CRITICAL).
 * Returns true on success, false on argument errors.
 */
static int l_task_set_priority(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer prio = luaL_checkinteger(L, 2);

    if (task_id < 0 || prio < 0 || prio > 7) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    task_set_priority(t, (uint8_t)prio);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * slm.task_pin(task_id, cpu) - Pin a task to a specific CPU.
 *
 * Wraps sched_set_task_affinity. Pass -1 (or `nil`) as the second argument
 * to clear affinity. Returns true on success, false on argument errors.
 */
static int l_task_pin(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer cpu = luaL_checkinteger(L, 2);

    if (task_id < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Negative -> clear affinity. Positive -> must be a valid cpu id. */
    uint32_t target;
    if (cpu < 0) {
        target = CPU_AFFINITY_ANY;
    } else if (cpu >= (lua_Integer)cpu_count) {
        lua_pushboolean(L, 0);
        return 1;
    } else {
        target = (uint32_t)cpu;
    }

    int rc = sched_set_task_affinity(t, target);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.task_migrate(task_id, target_cpu) - Move a task to a specific CPU
 *
 * Wraps sched_migrate_task. Returns true on success, false if the task
 * cannot be migrated (unknown id, running, affinity conflict, target out
 * of range). Scripts can poll slm.tasks() to see the CPU assignment
 * change after the migrate.
 */
static int l_task_migrate(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer target_cpu = luaL_checkinteger(L, 2);

    /* Task IDs are a monotonic uint32_t counter (see next_task_id in
     * kernel/sched/task.c) — NOT bounded by MAX_TASKS, which only
     * sizes the task_table array. Just reject negative values and
     * out-of-range CPUs. task_get() returns NULL for unknown ids.
     * Note: lua_Integer is 32-bit signed here (LUA_32BITS=1) so we
     * cannot compare against UINT32_MAX — it would wrap to -1. */
    if (task_id < 0 ||
        target_cpu < 0 || target_cpu >= (lua_Integer)cpu_count) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int rc = sched_migrate_task(t, (uint32_t)target_cpu);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.ai_sched_decision(task_id) - Get the last AI scheduler decision for a task.
 *
 * Returns a table: {core, priority_adj, preempt, raw}. Returns nil when
 *   - CONFIG_AI_SCHEDULER is off
 *   - the task id is unknown or terminated
 *   - the AI policy has not run on this task yet (last_ai_action == -1)
 *
 * priority_adj is 0/1/2 (none/boost/reduce). preempt is 0/1. raw is the
 * packed action index used inside the AI scheduler for histogram keys.
 */
static int l_ai_sched_decision(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_SCHEDULER)
    lua_Integer task_id = luaL_checkinteger(L, 1);
    if (task_id < 0) {
        lua_pushnil(L);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED || t->last_ai_action < 0) {
        lua_pushnil(L);
        return 1;
    }

    /* Decode via the canonical ai_decode_action (ai_types.h). Originally
     * inlined here to keep the Lua library isolated from AI headers, but
     * the PR #217 review flagged that as a silent-break hazard if the
     * encoding ever changes — ai_types.h is plain C with no heavy deps
     * so the include is free. */
    struct ai_sched_action act;
    ai_decode_action(t->last_ai_action, &act);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)act.core_assignment);
    lua_setfield(L, -2, "core");

    lua_pushinteger(L, (lua_Integer)act.priority_adj);
    lua_setfield(L, -2, "priority_adj");

    lua_pushinteger(L, (lua_Integer)act.preempt);
    lua_setfield(L, -2, "preempt");

    lua_pushinteger(L, (lua_Integer)t->last_ai_action);
    lua_setfield(L, -2, "raw");
#else
    (void)luaL_checkinteger(L, 1);  /* still validate arg shape */
    lua_pushnil(L);
#endif
    return 1;
}

/* ============================================================================
 * CPU Info Bindings
 * ============================================================================ */

/**
 * slm.cpu_info() - Get per-CPU information
 * Returns table: {online_count, total_count, current_cpu, cpus=[{id, isolated, ticks, schedules}, ...]}
 */
static int l_cpu_info(lua_State *L) {
    if (!L) return 0;
    /* Declared as pointer (into NC memory) on PLATFORM_HAS_NC_MEMORY
     * platforms, as a plain BSS array elsewhere — must extern-declare
     * in the matching shape or subscripting dereferences garbage. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    extern volatile uint32_t *sched_diag_tick;
    extern volatile uint32_t *sched_diag_schedule;
#else
    extern volatile uint32_t sched_diag_tick[];
    extern volatile uint32_t sched_diag_schedule[];
#endif

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)cpus_online);
    lua_setfield(L, -2, "online_count");

    lua_pushinteger(L, (lua_Integer)cpu_count);
    lua_setfield(L, -2, "total_count");

#if defined(PLATFORM_X86_64)
    lua_pushinteger(L, 0);
#else
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    lua_pushinteger(L, (lua_Integer)(mpidr & 0xFF));
#endif
    lua_setfield(L, -2, "current_cpu");

    /* Per-CPU array */
    uint32_t n = cpu_count;
    if (n > MAX_CPUS) n = MAX_CPUS;
    lua_createtable(L, (int)n, 0);
    for (uint32_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 4);

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "id");

        lua_pushboolean(L, sched_is_core_isolated(i));
        lua_setfield(L, -2, "isolated");

        lua_pushinteger(L, sched_diag_tick ? (lua_Integer)sched_diag_tick[i] : 0);
        lua_setfield(L, -2, "ticks");

        lua_pushinteger(L, sched_diag_schedule ? (lua_Integer)sched_diag_schedule[i] : 0);
        lua_setfield(L, -2, "schedules");

        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "cpus");

    return 1;
}

/* ============================================================================
 * VMM Stats Bindings (ARM64 only — x86-64 build omits vmm.c)
 * ============================================================================ */

/**
 * slm.vmm_stats() - Get virtual memory statistics
 * Returns table: {l1_tables, l2_tables, blocks_mapped, bytes_mapped}
 * Returns nil on x86-64 (no VMM).
 */
static int l_vmm_stats(lua_State *L) {
    if (!L) return 0;
#if defined(PLATFORM_X86_64)
    lua_pushnil(L);
#else
    struct vmm_stats stats;
    vmm_get_stats(&stats);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)stats.l1_tables);
    lua_setfield(L, -2, "l1_tables");

    lua_pushinteger(L, (lua_Integer)stats.l2_tables);
    lua_setfield(L, -2, "l2_tables");

    lua_pushinteger(L, (lua_Integer)stats.blocks_mapped);
    lua_setfield(L, -2, "blocks_mapped");

    lua_pushinteger(L, (lua_Integer)stats.bytes_mapped);
    lua_setfield(L, -2, "bytes_mapped");
#endif
    return 1;
}

/* ============================================================================
 * IPC Stats Bindings
 * ============================================================================ */

/**
 * slm.ipc_stats() - Get IPC statistics
 * Returns table: {queue_count, buffer_count, msgs_sent, msgs_recv}
 */
static int l_ipc_stats(lua_State *L) {
    if (!L) return 0;
    struct ipc_stats stats;
    ipc_get_stats(&stats);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)stats.queue_count);
    lua_setfield(L, -2, "queue_count");

    lua_pushinteger(L, (lua_Integer)stats.buffer_count);
    lua_setfield(L, -2, "buffer_count");

    lua_pushinteger(L, (lua_Integer)stats.total_msgs_sent);
    lua_setfield(L, -2, "msgs_sent");

    lua_pushinteger(L, (lua_Integer)stats.total_msgs_recv);
    lua_setfield(L, -2, "msgs_recv");

    return 1;
}

/* ============================================================================
 * Eviction Policy Bindings (gated on CONFIG_AI_EVICTION)
 * ============================================================================ */

/**
 * slm.eviction_policy() - Get current eviction policy name
 * Returns string, or nil if eviction is disabled.
 */
static int l_eviction_policy(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_EVICTION)
    uint8_t buf[64];
    size_t n = rust_eviction_policy_name(buf, sizeof(buf));
    if (n > 0)
        lua_pushlstring(L, (const char *)buf, n);
    else
        lua_pushnil(L);
#else
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.eviction_set_policy(name) - Switch eviction policy by name
 * Returns true on success, false on failure (unknown name / feature off).
 */
static int l_eviction_set_policy(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int32_t rc = rust_eviction_policy_set((const uint8_t *)name);
    lua_pushboolean(L, rc == 0);
#else
    (void)name;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_stats() - Get eviction statistics
 * Returns table with {policy, enabled, weight_evictions, workspace_evictions,
 *                     weight_allocated/total, workspace_allocated/total,
 *                     snapshot_candidates, expert_weights_bp}
 * Returns nil if eviction feature is disabled.
 */
static int l_eviction_stats(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_EVICTION)
    RustEvictionStats stats;
    int32_t rc = rust_eviction_get_stats(&stats);
    if (rc != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 10);

    lua_pushboolean(L, stats.feature_enabled);
    lua_setfield(L, -2, "enabled");

    /* Policy name */
    uint8_t pbuf[64];
    size_t pn = rust_eviction_policy_name(pbuf, sizeof(pbuf));
    if (pn > 0)
        lua_pushlstring(L, (const char *)pbuf, pn);
    else
        lua_pushstring(L, "unknown");
    lua_setfield(L, -2, "policy");

    lua_pushinteger(L, (lua_Integer)stats.weight_evictions);
    lua_setfield(L, -2, "weight_evictions");

    lua_pushinteger(L, (lua_Integer)stats.workspace_evictions);
    lua_setfield(L, -2, "workspace_evictions");

    lua_pushinteger(L, (lua_Integer)stats.weight_allocated);
    lua_setfield(L, -2, "weight_allocated");

    lua_pushinteger(L, (lua_Integer)stats.weight_total);
    lua_setfield(L, -2, "weight_total");

    lua_pushinteger(L, (lua_Integer)stats.workspace_allocated);
    lua_setfield(L, -2, "workspace_allocated");

    lua_pushinteger(L, (lua_Integer)stats.workspace_total);
    lua_setfield(L, -2, "workspace_total");

    lua_pushinteger(L, (lua_Integer)stats.snapshot_candidates);
    lua_setfield(L, -2, "snapshot_candidates");

    /* CACHEUS expert weights (basis points: 0..10000) */
    if (stats.cacheus_expert_count > 0) {
        uint32_t n = stats.cacheus_expert_count;
        if (n > 5) n = 5;
        lua_createtable(L, (int)n, 0);
        for (uint32_t i = 0; i < n; i++) {
            lua_pushinteger(L, (lua_Integer)stats.expert_weights_bp[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, "expert_weights_bp");
    }
#else
    lua_pushnil(L);
#endif
    return 1;
}

/* ============================================================================
 * Extended Model Bindings
 * ============================================================================ */

/**
 * slm.model_list() - List all loaded models
 * Returns array of tables: {{index, name, format, params, weight_size, nodes}, ...}
 */
static int l_model_list(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    int idx = 1;
    /* Registry has 8 slots; skip invalid ones. Same loop bound as the
     * `model list` shell handler in shell_exec.c. */
    for (uint32_t i = 0; i < 8; i++) {
        RustModelInfo info;
        if (rust_model_get_info(i, &info) != 0) continue;

        lua_createtable(L, 0, 6);

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "index");

        lua_pushstring(L, (const char *)info.name);
        lua_setfield(L, -2, "name");

        const char *fmt;
        switch (info.format) {
            case 0: fmt = "GGUF"; break;
            case 1: fmt = "ONNX"; break;
            case 2: fmt = "Raw"; break;
            default: fmt = "unknown"; break;
        }
        lua_pushstring(L, fmt);
        lua_setfield(L, -2, "format");

        lua_pushinteger(L, (lua_Integer)info.param_count);
        lua_setfield(L, -2, "params");

        lua_pushinteger(L, (lua_Integer)info.weight_size);
        lua_setfield(L, -2, "weight_size");

        lua_pushinteger(L, (lua_Integer)info.node_count);
        lua_setfield(L, -2, "nodes");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.model_info(index) - Get detailed info for one model
 * Returns table or nil if index invalid.
 */
static int l_model_info(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);

    RustModelInfo info;
    if (rust_model_get_info((uint32_t)idx, &info) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 9);

    lua_pushinteger(L, (lua_Integer)idx);
    lua_setfield(L, -2, "index");

    lua_pushstring(L, (const char *)info.name);
    lua_setfield(L, -2, "name");

    const char *fmt;
    switch (info.format) {
        case 0: fmt = "GGUF"; break;
        case 1: fmt = "ONNX"; break;
        case 2: fmt = "Raw"; break;
        default: fmt = "unknown"; break;
    }
    lua_pushstring(L, fmt);
    lua_setfield(L, -2, "format");

    lua_pushinteger(L, (lua_Integer)info.param_count);
    lua_setfield(L, -2, "params");

    lua_pushinteger(L, (lua_Integer)info.weight_size);
    lua_setfield(L, -2, "weight_size");

    lua_pushinteger(L, (lua_Integer)info.workspace_size);
    lua_setfield(L, -2, "workspace_size");

    lua_pushinteger(L, (lua_Integer)info.node_count);
    lua_setfield(L, -2, "nodes");

    lua_pushinteger(L, (lua_Integer)info.input_count);
    lua_setfield(L, -2, "inputs");

    lua_pushinteger(L, (lua_Integer)info.output_count);
    lua_setfield(L, -2, "outputs");

    return 1;
}

/**
 * slm.model_bench(index, iterations) - Run inference benchmark
 * Results are printed to UART. Returns 0 on success, -1 on error.
 */
static int l_model_bench(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    int iters = (int)luaL_optinteger(L, 2, 10);
    if (iters < 1) iters = 1;
    int rc = rust_infer_bench((uint32_t)idx, (uint32_t)iters);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.model_load(path [, name]) - Load an ONNX model from the VFS.
 *
 * Mirrors the `model load <path>` shell command (kernel/src/shell_sys.c)
 * so scripts can exercise the eviction demo with different models. If
 * `name` is omitted the model name is derived from the filename
 * (last path component, extension stripped).
 *
 * Returns the registry index (>=0) on success, -1 on any failure
 * (path too long, file not found, not a file, empty, out of memory,
 * ONNX parse error). The file buffer is freed before return regardless
 * of success — rust_model_load copies into its own registry storage.
 */
static int l_model_load(lua_State *L) {
    if (!L) return 0;
    const char *path = luaL_checkstring(L, 1);

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0 || info.type != 0 || info.size == 0) {
        lua_pushinteger(L, -1);
        return 1;
    }

    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        lua_pushinteger(L, -1);
        return 1;
    }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        lua_pushinteger(L, -1);
        return 1;
    }

    /* Derive model name: explicit arg 2 wins; otherwise use the
     * last path component, extension stripped (matches shell's
     * `model load` behavior). */
    char derived[32];
    const char *name = luaL_optstring(L, 2, NULL);
    if (!name) {
        const char *base = resolved;
        for (const char *p = resolved; *p; p++)
            if (*p == '/') base = p + 1;
        size_t n = 0;
        for (const char *p = base; *p && *p != '.' && n < 31; p++)
            derived[n++] = *p;
        derived[n] = '\0';
        name = derived;
    }

    int result = rust_model_load(name, buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);

    lua_pushinteger(L, result);
    return 1;
}

/**
 * slm.infer_stats() - Get inference performance statistics
 * Returns table: {total, total_ns, min_ns, max_ns, last_ns, errors}
 * Returns nil on error (e.g., stats unavailable).
 */
static int l_infer_stats(lua_State *L) {
    if (!L) return 0;
    RustInferStats stats;
    int rc = rust_infer_stats(&stats);
    if (rc != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 6);

    lua_pushinteger(L, (lua_Integer)stats.total_inferences);
    lua_setfield(L, -2, "total");

    lua_pushinteger(L, (lua_Integer)stats.total_time_ns);
    lua_setfield(L, -2, "total_ns");

    lua_pushinteger(L, (lua_Integer)stats.min_time_ns);
    lua_setfield(L, -2, "min_ns");

    lua_pushinteger(L, (lua_Integer)stats.max_time_ns);
    lua_setfield(L, -2, "max_ns");

    lua_pushinteger(L, (lua_Integer)stats.last_time_ns);
    lua_setfield(L, -2, "last_ns");

    lua_pushinteger(L, (lua_Integer)stats.errors);
    lua_setfield(L, -2, "errors");

    return 1;
}

/**
 * slm.gpu_status() - Get GPU subsystem status
 * Returns table: {available, name, device, compute_ready, unified_memory, memory_size}
 */
static int l_gpu_status(lua_State *L) {
    if (!L) return 0;

    int avail = slm_gpu_available();

    lua_createtable(L, 0, 6);

    lua_pushboolean(L, avail);
    lua_setfield(L, -2, "available");

    if (avail) {
        RustGpuInfo info;
        if (slm_gpu_get_info(&info) == 0) {
            lua_pushstring(L, (const char *)info.name);
            lua_setfield(L, -2, "name");

            lua_pushstring(L, (const char *)info.device);
            lua_setfield(L, -2, "device");

            lua_pushboolean(L, info.compute_ready);
            lua_setfield(L, -2, "compute_ready");

            lua_pushboolean(L, info.unified_memory);
            lua_setfield(L, -2, "unified_memory");

            lua_pushinteger(L, (lua_Integer)info.memory_size);
            lua_setfield(L, -2, "memory_size");
        }
    }

    return 1;
}

/* ============================================================================
 * Shell Integration Bindings
 * ============================================================================ */

/**
 * slm.read_line() - Read one line from the shell's UART input.
 *
 * Blocks until the user presses Enter (or Ctrl+C, which yields an empty line).
 * Returns the line as a string with the trailing newline stripped.
 *
 * Used by interactive Lua scripts (menus, prompts) so they can read user
 * input without each script reimplementing line editing.
 */
static int l_read_line(lua_State *L) {
    if (!L) return 0;
    /* Drain pending msg_subscribe callbacks before blocking on UART (#207)
     * so scripts that wait at a menu prompt still see their subscribers. */
    lua_msg_drain(L);
    char buf[SHELL_MAX_LINE];
    int n = shell_read_line(buf, (int)sizeof(buf));
    if (n < 0) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, buf, (size_t)n);
    }
    return 1;
}

/**
 * slm.shell_exec(cmd) - Run a shell command string and return its exit code.
 *
 * Dispatches `cmd` through the same parser the shell REPL uses, so anything
 * the user could type (e.g. "bench smp", "tasks", "run test") works from
 * Lua. Output goes to UART as it normally would. Returns the command's
 * integer return value, or -1 if the command was not found / line too long.
 *
 * Validates the command length at the binding boundary so a too-long input
 * raises a clean Lua error (with arg position) instead of falling through
 * to shell_execute's UART warning.
 */
static int l_shell_exec(lua_State *L) {
    if (!L) return 0;
    size_t len;
    const char *cmd = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len < SHELL_MAX_LINE, 1,
                  "command exceeds SHELL_MAX_LINE");
    int rc = shell_execute(cmd);
    lua_pushinteger(L, rc);
    return 1;
}

#if defined(ENABLE_NETWORKING)
/* ============================================================================
 * slm.telnetd_* — Phase 3 Lua bindings for the TCP shell daemon
 *
 * Flat namespace (slm.telnetd_start, slm.telnetd_stop, ...) to match
 * the project's existing slm.component_* / slm.msg_* / slm.sched_*
 * pattern. The plan (§3.4) shows a nested slm.telnetd.{start,stop,...}
 * form; flattening is a deliberate deviation to keep Lua convention
 * consistent across the bindings.
 * ============================================================================ */

/**
 * slm.telnetd_start([port]) — bring the listener up.
 * Returns 0 on success, negative on failure.
 */
static int l_telnetd_start(lua_State *L) {
    if (!L) return 0;
    int port = luaL_optinteger(L, 1, 2323);
    if (port < 1 || port > 65535) {
        return luaL_argerror(L, 1, "port must be 1..65535");
    }
    if (!net_is_up()) {
        if (net_init() != 0) {
            lua_pushinteger(L, -1);
            return 1;
        }
    }
    int rc = tcp_shell_server_start((uint16_t)port);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.telnetd_stop() — stop accepting new connections.
 * Returns true if the listener was running, false if already stopped.
 */
static int l_telnetd_stop(lua_State *L) {
    if (!L) return 0;
    bool was_running = tcp_shell_server_running();
    if (was_running) {
        tcp_shell_server_stop();
    }
    lua_pushboolean(L, was_running);
    return 1;
}

/**
 * slm.telnetd_status() — returns a table with daemon state.
 *   { running, port, accepted, active, max }
 */
static int l_telnetd_status(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    lua_pushboolean(L, tcp_shell_server_running());
    lua_setfield(L, -2, "running");

    lua_pushinteger(L, (lua_Integer)tcp_shell_server_port());
    lua_setfield(L, -2, "port");

    lua_pushinteger(L, (lua_Integer)tcp_shell_server_accepted());
    lua_setfield(L, -2, "accepted");

    lua_pushinteger(L, (lua_Integer)shell_io_tcp_active_count());
    lua_setfield(L, -2, "active");

    lua_pushinteger(L, (lua_Integer)MAX_TCP_SHELL_SESSIONS);
    lua_setfield(L, -2, "max");

    return 1;
}

/* Closure over the lua_State so the foreach visitor can push into it. */
struct telnetd_sessions_ctx {
    lua_State *L;
    int        idx;   /* next Lua array index */
};

static bool telnetd_sessions_visitor(const struct tcp_session_info *info, void *c) {
    struct telnetd_sessions_ctx *sc = c;
    lua_State *L = sc->L;

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info->session_id);
    lua_setfield(L, -2, "id");

    lua_pushinteger(L, (lua_Integer)info->peer_ip);
    lua_setfield(L, -2, "peer_ip");

    lua_pushinteger(L, (lua_Integer)info->peer_port);
    lua_setfield(L, -2, "peer_port");

    lua_pushinteger(L, (lua_Integer)info->connected_at);
    lua_setfield(L, -2, "connected_at");

    lua_rawseti(L, -2, sc->idx++);
    return true;
}

/**
 * slm.telnetd_sessions() — returns an array of per-session tables.
 *   { { id=N, peer_ip=N (nbo), peer_port=N, connected_at=ms }, ... }
 */
static int l_telnetd_sessions(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);
    struct telnetd_sessions_ctx sc = { .L = L, .idx = 1 };
    shell_io_tcp_foreach(telnetd_sessions_visitor, &sc);
    return 1;
}

/**
 * slm.telnetd_kick(id) — force-disconnect one session.
 * Returns true if a matching session was found.
 */
static int l_telnetd_kick(lua_State *L) {
    if (!L) return 0;
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id < 0 || id > 0xFFFFFFFF) {
        return luaL_argerror(L, 1, "id out of range");
    }
    bool kicked = shell_io_tcp_kick((uint32_t)id);
    lua_pushboolean(L, kicked);
    return 1;
}
#endif /* ENABLE_NETWORKING */

/* SLM library functions */
static const luaL_Reg slm_lib[] = {
    {"print", l_print},
    {"uptime", l_uptime},
    {"mem_stats", l_mem_stats},
    {"tasks", l_tasks},
    {"sleep", l_sleep},
    {"yield", l_yield},
    {"version", l_version},
    {"cpu_count", l_cpu_count},
    {"cpu_id", l_cpu_id},
    /* Component management */
    {"component_count", l_component_count},
    {"component_list", l_component_list},
    {"component_find", l_component_find},
    {"component_run", l_component_run},
    {"component_hot_swap", l_component_hot_swap},
    {"component_hot_swap_stateful", l_component_hot_swap_stateful},
    /* Model memory and inference */
    {"model_stats", l_model_stats},
    {"model_find", l_model_find},
    {"model_infer", l_model_infer},
    {"model_load_mnist", l_model_load_mnist},
    {"model_pin", l_model_pin},
    {"model_preload", l_model_preload},
    {"model_preload_wait", l_model_preload_wait},
    {"model_unpin", l_model_unpin},
    /* Message routing */
    {"msg_publish", l_msg_publish},
    {"msg_publish_priority", l_msg_publish_priority},
    {"msg_subscribe", l_msg_subscribe},
    {"msg_unsubscribe", l_msg_unsubscribe},
    {"msg_drain", l_msg_drain},
    /* Scheduler */
    {"sched_policy", l_sched_policy},
    {"sched_stats", l_sched_stats},
    {"sched_set_policy", l_sched_set_policy},
    {"sched_policy_list", l_sched_policy_list},
    {"ai_sched_stats", l_ai_sched_stats},
    {"ai_sched_decision", l_ai_sched_decision},
    {"task_migrate", l_task_migrate},
    {"task_create", l_task_create},
    {"task_kill", l_task_kill},
    {"task_set_priority", l_task_set_priority},
    {"task_pin", l_task_pin},
    /* CPU info */
    {"cpu_info", l_cpu_info},
    /* Memory / VMM / IPC */
    {"vmm_stats", l_vmm_stats},
    {"ipc_stats", l_ipc_stats},
    /* Eviction */
    {"eviction_policy", l_eviction_policy},
    {"eviction_set_policy", l_eviction_set_policy},
    {"eviction_stats", l_eviction_stats},
    /* Extended model bindings */
    {"model_list", l_model_list},
    {"model_info", l_model_info},
    {"model_bench", l_model_bench},
    {"model_load", l_model_load},
    {"infer_stats", l_infer_stats},
    {"gpu_status", l_gpu_status},
    /* Shell integration */
    {"read_line", l_read_line},
    {"shell_exec", l_shell_exec},
#if defined(ENABLE_NETWORKING)
    /* TCP shell daemon (Phase 3) */
    {"telnetd_start",    l_telnetd_start},
    {"telnetd_stop",     l_telnetd_stop},
    {"telnetd_status",   l_telnetd_status},
    {"telnetd_sessions", l_telnetd_sessions},
    {"telnetd_kick",     l_telnetd_kick},
#endif
    {NULL, NULL}
};

/**
 * Open the SLM library
 */
static int luaopen_slm(lua_State *L) {
    luaL_newlib(L, slm_lib);
    return 1;
}

/* ============================================================================
 * Lua State Management
 * ============================================================================ */

static int lua_initialized = 0;

/* Active lua_State count. heap_reset() in lua_slm_close must wait until
 * this drops to zero — with Lua-defined tasks (#208) more than one state
 * can be live and resetting the shared heap would pull the rug out from
 * under the surviving state. */
static volatile int lua_state_count = 0;

void lua_slm_init(void) {
    if (lua_initialized) return;
    lua_initialized = 1;
    shell_printf("Lua 5.4 scripting initialized\n");
}

lua_State *lua_slm_newstate(void) {
    if (!lua_initialized) {
        lua_slm_init();
    }

    /* Create Lua state with default allocator (uses our malloc) */
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        shell_printf("Failed to create Lua state\n");
        return NULL;
    }
    lua_state_count++;

    /* Open safe standard libraries */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);

    /* Open SLM library */
    luaL_requiref(L, "slm", luaopen_slm, 1);
    lua_pop(L, 1);

    /* Replace print with our version */
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    return L;
}

void lua_slm_close(lua_State *L) {
    if (L) {
        /* Release any Lua msg_subscribe refs owned by this state (#207)
         * before the state is closed — luaL_unref needs a live state. */
        lua_msg_subs_cleanup(L);
        lua_close(L);
        /* Only reset the shared Lua heap when the last state is gone.
         * With Lua-defined tasks (#208), multiple states can be live at
         * once and a reset would wipe allocations belonging to survivors. */
        if (lua_state_count > 0) lua_state_count--;
        if (lua_state_count == 0) {
            extern void heap_reset(void);
            heap_reset();
        }
    }
}

int lua_slm_dostring(lua_State *L, const char *script) {
    if (!L) return -1;
    int saved_top = lua_gettop(L);
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        shell_printf("Lua error: %s\n", msg ? msg : "(unknown)");
    }
    /* Restore stack to caller's view regardless of outcome: luaL_dostring
     * leaves either the results of the chunk or the error message, and
     * callers of this wrapper don't read them. */
    lua_settop(L, saved_top);
    return status;
}

int lua_slm_dofile(lua_State *L, const char *filename) {
    if (!L || !filename) return -1;
    int saved_top = lua_gettop(L);

    /* Sized to fit the largest embedded demo (~7 KB demo_menu.lua) plus
     * headroom. Lives on the shell task's 64 KB stack; cheap. */
    char buf[16384];
    int len = vfs_read_path(filename, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
        shell_printf("lua: cannot open %s\n", filename);
        return -1;
    }
    if (len >= (int)sizeof(buf) - 1) {
        shell_printf("lua: %s exceeds %d bytes\n", filename, (int)sizeof(buf) - 1);
        return -1;
    }
    buf[len] = '\0';

    int status = luaL_loadbufferx(L, buf, (size_t)len, filename, NULL);
    if (status == LUA_OK)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        shell_printf("Lua error: %s\n", msg ? msg : "(unknown)");
    }
    lua_settop(L, saved_top);
    return status;
}

const char *lua_slm_geterror(lua_State *L) {
    if (lua_gettop(L) > 0 && lua_isstring(L, -1)) {
        return lua_tostring(L, -1);
    }
    return NULL;
}

/* ============================================================================
 * REPL (Read-Eval-Print Loop)
 * ============================================================================ */

#define REPL_BUFFER_SIZE 256

void lua_slm_repl(lua_State *L) {
    static char buffer[REPL_BUFFER_SIZE];
    int pos = 0;

    shell_printf("Lua 5.4 REPL - type 'exit' to quit\n");
    shell_printf(">>> ");

    while (1) {
        int c = shell_getc();
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            shell_printf("\n");
            buffer[pos] = '\0';

            /* Check for exit command */
            if (pos == 4 && buffer[0] == 'e' && buffer[1] == 'x' &&
                buffer[2] == 'i' && buffer[3] == 't') {
                shell_printf("Exiting Lua REPL\n");
                break;
            }

            /* Check for Ctrl+D (EOF) */
            if (pos == 0) {
                /* Empty line - just show prompt again */
                shell_printf(">>> ");
                continue;
            }

            /* Execute the line */
            lua_slm_dostring(L, buffer);

            /* Reset for next line */
            pos = 0;
            shell_printf(">>> ");
        } else if (c == 0x03) {
            /* Ctrl+C - cancel current line */
            shell_printf("^C\n>>> ");
            pos = 0;
        } else if (c == 0x04) {
            /* Ctrl+D - exit */
            shell_printf("^D\nExiting Lua REPL\n");
            break;
        } else if (c == 0x7f || c == 0x08) {
            /* Backspace */
            if (pos > 0) {
                pos--;
                shell_printf("\b \b");
            }
        } else if (c >= 32 && c < 127 && pos < REPL_BUFFER_SIZE - 1) {
            /* Printable character */
            buffer[pos++] = (char)c;
            shell_putc((char)c);
        }
    }
}
