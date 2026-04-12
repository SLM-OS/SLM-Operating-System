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
    (void)L;
    yield();
    return 0;
}

/**
 * slm.version() - Get SLM-OS version string
 */
static int l_version(lua_State *L) {
    lua_pushstring(L, "SLM-OS " SLM_VERSION);
    return 1;
}

/**
 * slm.cpu_count() - Get number of CPUs
 */
static int l_cpu_count(lua_State *L) {
    lua_pushinteger(L, MAX_CPUS);
    return 1;
}

/**
 * slm.cpu_id() - Get current CPU ID
 */
static int l_cpu_id(lua_State *L) {
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
    lua_pushinteger(L, (lua_Integer)component_count());
    return 1;
}

/**
 * slm.component_list() - Get list of all components
 * Returns array of tables: {{name, version, type, priority, state, task_id}, ...}
 */
static int l_component_list(lua_State *L) {
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
extern int sensor_monitor_export_state(uint8_t *buf, uint32_t max_size);
static int l_component_hot_swap_stateful(lua_State *L) {
    const char *old_name = luaL_checkstring(L, 1);
    const char *new_name = luaL_checkstring(L, 2);

    /* Select export function based on component name */
    component_state_export_fn export_fn = NULL;
    /* Simple name check for sensor_monitor */
    if (old_name[0] == 's' && old_name[7] == 'm') {
        export_fn = sensor_monitor_export_state;
    }

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
    int idx = rust_model_load_builtin_mnist();
    lua_pushinteger(L, idx);
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
    const char *topic = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    int delivered = msg_router_publish((const uint8_t *)topic, (const uint8_t *)data);
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
    lua_pushstring(L, sched_get_policy());
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
    /* Message routing */
    {"msg_publish", l_msg_publish},
    /* Scheduler */
    {"sched_policy", l_sched_policy},
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
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        uart_printf("Lua error: %s\n", msg ? msg : "(unknown)");
        lua_pop(L, 1);
    }
    return status;
}

int lua_slm_dofile(lua_State *L, const char *filename) {
    if (!L || !filename) return -1;

    char buf[4096];
    int len = vfs_read_path(filename, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
        uart_printf("lua: cannot open %s\n", filename);
        return -1;
    }
    buf[len] = '\0';

    int status = luaL_loadbufferx(L, buf, (size_t)len, filename, NULL);
    if (status == LUA_OK)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        uart_printf("Lua error: %s\n", msg ? msg : "(unknown)");
        lua_pop(L, 1);
    }
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
