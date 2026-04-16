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
 * System:
 *   slm.print(msg)                    - Print message to console
 *   slm.uptime()                      - Get system uptime in milliseconds
 *   slm.mem_stats()                   - Get memory statistics table
 *   slm.tasks()                       - Get list of tasks
 *   slm.sleep(ms)                     - Sleep for milliseconds
 *   slm.yield()                       - Yield CPU to scheduler
 *   slm.version()                     - Get SLM-OS version string
 *   slm.cpu_count()                   - Get number of CPUs
 *   slm.cpu_id()                      - Get current CPU ID
 *
 * Component management:
 *   slm.component_count()             - Number of registered components
 *   slm.component_list()              - List all components (array of tables)
 *   slm.component_find(name)          - Find component by name (index or nil)
 *   slm.component_run(name)           - Run built-in component (index or nil)
 *   slm.component_hot_swap(old, new)  - Hot-swap component (index or nil)
 *
 * Model memory:
 *   slm.model_stats()                 - Pool stats {weights={...}, workspace={...}}
 *
 * Message subscriptions (#207):
 *   slm.msg_subscribe(topic, fn)      - Register callback, returns handle or nil
 *   slm.msg_unsubscribe(handle)       - Remove subscription, returns bool
 *   slm.msg_drain()                   - Manually dispatch pending callbacks
 *
 * Extended model:
 *   slm.model_list()                  - Array of loaded models
 *   slm.model_info(index)             - Detailed model info or nil
 *   slm.model_bench(index, iters)     - Run inference benchmark (prints to UART)
 *   slm.model_load(path [, name])     - Load ONNX from VFS (#209)
 *   slm.infer_stats()                 - Inference stats (count, min/max/last ns)
 *   slm.gpu_status()                  - GPU info {available, name, device, ...}
 *
 * Scheduler:
 *   slm.sched_policy()                - Current policy name
 *   slm.sched_stats()                 - {task_count, ready_count, ctx_switches, ticks}
 *   slm.sched_set_policy(name)        - Switch policy, returns bool
 *   slm.sched_policy_list()           - Array of {name, active}
 *   slm.ai_sched_stats()              - AI scheduler stats or nil
 *   slm.ai_sched_decision(task_id)    - {core, priority_adj, preempt, raw} or nil (#211)
 *   slm.task_migrate(id, cpu)         - Move task to CPU, returns bool (#210)
 *   slm.task_create(name, fn)         - Spawn Lua-defined task, returns id or nil (#208)
 *   slm.task_kill(id)                 - Terminate task, returns bool (#208)
 *   slm.task_set_priority(id, p)      - Set priority 0-7, returns bool (#208)
 *   slm.task_pin(id, cpu)             - Pin task to CPU (-1 clears), returns bool (#208)
 *
 * CPU / Memory:
 *   slm.cpu_info()                    - Per-CPU state
 *   slm.vmm_stats()                   - VMM stats (ARM64) or nil on x86-64
 *   slm.ipc_stats()                   - IPC stats
 *
 * Eviction (requires CONFIG_AI_EVICTION):
 *   slm.eviction_policy()             - Current policy name or nil
 *   slm.eviction_set_policy(name)     - Switch policy, returns bool
 *   slm.eviction_stats()              - Detailed stats table or nil
 *
 * Shell integration:
 *   slm.read_line()                   - Read one line from UART (blocks)
 *   slm.shell_exec(cmd)               - Run a shell command, return exit code
 */

#endif /* LUA_SLM_H */
