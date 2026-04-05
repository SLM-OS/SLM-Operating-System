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
    /* TODO: Implement file loading via VFS */
    (void)L;
    uart_printf("File loading not yet implemented: %s\n", filename);
    return -1;
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
