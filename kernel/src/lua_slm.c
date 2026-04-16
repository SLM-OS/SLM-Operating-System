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
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
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
        if (i > 1) uart_printf("\t");
        if (lua_isstring(L, i)) {
            uart_printf("%s", lua_tostring(L, i));
        } else if (lua_isnil(L, i)) {
            uart_printf("nil");
        } else if (lua_isboolean(L, i)) {
            uart_printf("%s", lua_toboolean(L, i) ? "true" : "false");
        } else if (lua_isnumber(L, i)) {
            lua_Number n = lua_tonumber(L, i);
            uart_printf("%d", (int)n);
        } else {
            uart_printf("%s: %p", luaL_typename(L, i), lua_topointer(L, i));
        }
    }
    uart_printf("\n");
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
        }
    }
    return 0;
}

/**
 * slm.yield() - Yield CPU to scheduler
 */
static int l_yield(lua_State *L) {
    if (!L) return 0;
    yield();
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
    {"model_unpin", l_model_unpin},
    /* Message routing */
    {"msg_publish", l_msg_publish},
    {"msg_publish_priority", l_msg_publish_priority},
    /* Scheduler */
    {"sched_policy", l_sched_policy},
    {"sched_stats", l_sched_stats},
    {"sched_set_policy", l_sched_set_policy},
    {"sched_policy_list", l_sched_policy_list},
    {"ai_sched_stats", l_ai_sched_stats},
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
    {"infer_stats", l_infer_stats},
    {"gpu_status", l_gpu_status},
    /* Shell integration */
    {"read_line", l_read_line},
    {"shell_exec", l_shell_exec},
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

void lua_slm_init(void) {
    if (lua_initialized) return;
    lua_initialized = 1;
    uart_printf("Lua 5.4 scripting initialized\n");
}

lua_State *lua_slm_newstate(void) {
    if (!lua_initialized) {
        lua_slm_init();
    }

    /* Create Lua state with default allocator (uses our malloc) */
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        uart_printf("Failed to create Lua state\n");
        return NULL;
    }

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
        lua_close(L);
        /* Reset the Lua heap to eliminate fragmentation between sessions.
         * Safe because only one Lua state exists at a time. */
        extern void heap_reset(void);
        heap_reset();
    }
}

int lua_slm_dostring(lua_State *L, const char *script) {
    if (!L) return -1;
    int saved_top = lua_gettop(L);
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        uart_printf("Lua error: %s\n", msg ? msg : "(unknown)");
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
        uart_printf("lua: cannot open %s\n", filename);
        return -1;
    }
    if (len >= (int)sizeof(buf) - 1) {
        uart_printf("lua: %s exceeds %d bytes\n", filename, (int)sizeof(buf) - 1);
        return -1;
    }
    buf[len] = '\0';

    int status = luaL_loadbufferx(L, buf, (size_t)len, filename, NULL);
    if (status == LUA_OK)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        uart_printf("Lua error: %s\n", msg ? msg : "(unknown)");
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

    uart_printf("Lua 5.4 REPL - type 'exit' to quit\n");
    uart_printf(">>> ");

    while (1) {
        int c = uart_getc();
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            uart_printf("\n");
            buffer[pos] = '\0';

            /* Check for exit command */
            if (pos == 4 && buffer[0] == 'e' && buffer[1] == 'x' &&
                buffer[2] == 'i' && buffer[3] == 't') {
                uart_printf("Exiting Lua REPL\n");
                break;
            }

            /* Check for Ctrl+D (EOF) */
            if (pos == 0) {
                /* Empty line - just show prompt again */
                uart_printf(">>> ");
                continue;
            }

            /* Execute the line */
            lua_slm_dostring(L, buffer);

            /* Reset for next line */
            pos = 0;
            uart_printf(">>> ");
        } else if (c == 0x03) {
            /* Ctrl+C - cancel current line */
            uart_printf("^C\n>>> ");
            pos = 0;
        } else if (c == 0x04) {
            /* Ctrl+D - exit */
            uart_printf("^D\nExiting Lua REPL\n");
            break;
        } else if (c == 0x7f || c == 0x08) {
            /* Backspace */
            if (pos > 0) {
                pos--;
                uart_printf("\b \b");
            }
        } else if (c >= 32 && c < 127 && pos < REPL_BUFFER_SIZE - 1) {
            /* Printable character */
            buffer[pos++] = (char)c;
            uart_putc((char)c);
        }
    }
}
