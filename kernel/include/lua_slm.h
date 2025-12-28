/**
 * lua_slm.h - Lua Integration for SLM-OS
 *
 * Provides Lua scripting support with kernel API bindings.
 */

#ifndef LUA_SLM_H
#define LUA_SLM_H

#include <stddef.h>
#include <stdbool.h>

/* Forward declaration */
struct lua_State;
typedef struct lua_State lua_State;

/**
 * Initialize the Lua subsystem.
 * Must be called once before using Lua.
 */
void lua_slm_init(void);

/**
 * Create a new Lua state with SLM-OS bindings.
 * Returns NULL on failure.
 */
lua_State *lua_slm_newstate(void);

/**
 * Close a Lua state and free resources.
 */
void lua_slm_close(lua_State *L);

/**
 * Execute a Lua script from a string.
 * Returns 0 on success, non-zero on error.
 */
int lua_slm_dostring(lua_State *L, const char *script);

/**
 * Execute a Lua script from a file.
 * Returns 0 on success, non-zero on error.
 */
int lua_slm_dofile(lua_State *L, const char *filename);

/**
 * Run the Lua REPL (Read-Eval-Print Loop).
 * Blocks until user exits (Ctrl+D or 'exit').
 */
void lua_slm_repl(lua_State *L);

/**
 * Get the last error message from Lua.
 */
const char *lua_slm_geterror(lua_State *L);

/**
 * Register shell commands for Lua.
 */
void lua_shell_init(void);

/* ============================================================================
 * SLM-OS Lua API (available to scripts as 'slm' table)
 * ============================================================================
 *
 * slm.print(msg)       - Print message to console
 * slm.uptime()         - Get system uptime in milliseconds
 * slm.mem_stats()      - Get memory statistics table
 * slm.tasks()          - Get list of tasks
 * slm.sleep(ms)        - Sleep for milliseconds
 * slm.yield()          - Yield CPU to scheduler
 * slm.version()        - Get SLM-OS version string
 */

#endif /* LUA_SLM_H */
