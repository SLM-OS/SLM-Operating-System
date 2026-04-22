/**
 * lua_shell.c - Lua Shell Commands for SLM-OS
 *
 * Provides 'lua' command for interactive scripting and script execution.
 */

#include "lua_slm.h"
#include "shell.h"
#include "shell_session.h"
#include "debug.h"

#include "../lib/lua/src/lua.h"      /* lua_newtable, lua_rawseti, lua_setglobal */
#include <stddef.h>

/**
 * lua [script] - Run Lua script or enter REPL
 *
 * Usage:
 *   lua              - Enter interactive REPL
 *   lua -e "code"    - Execute code directly
 *   lua <filename>   - Run script from filesystem
 */
static int cmd_lua(int argc, char *argv[]) {
    struct shell_session *sess = shell_session_current();
    bool persistent_repl = (sess && sess->id != 0 && argc == 1);
    bool close_on_return = !persistent_repl;
    lua_State *L = (persistent_repl && sess) ? (lua_State *)sess->lua : NULL;
    if (persistent_repl && sess && !L) {
        L = lua_slm_newstate();
        if (L) {
            sess->lua = L;
        }
    }
    if (!persistent_repl) {
        L = lua_slm_newstate();
    }
    if (L == NULL) {
        shell_printf("Failed to initialize Lua\n");
        return -1;
    }

    /* Each invocation gets a fresh `arg` view. Persistent per-session
     * Lua states must not leak argv from a prior script or -e run. */
    lua_pushnil(L);
    lua_setglobal(L, "arg");

    if (argc == 1) {
        /* No arguments - enter REPL */
        lua_slm_repl(L);
    } else if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'e') {
        /* -e "code" - execute code */
        lua_slm_dostring(L, argv[2]);
    } else if (argc >= 2) {
        /* Filename + optional positional args. Populate Lua's `arg`
         * global with argv[2..argc-1] so scripts like demo_hailo.lua
         * can read `arg[1]`, `arg[2]`, ... just like a standalone
         * Lua interpreter. arg[0] holds the script path. */
        lua_newtable(L);
        for (int i = 1; i < argc; i++) {
            lua_pushstring(L, argv[i]);
            lua_rawseti(L, -2, i - 1);  /* arg[0] = script path,
                                           arg[1..] = extra args */
        }
        lua_setglobal(L, "arg");
        if (lua_slm_dofile(L, argv[1]) != 0) {
            /* File loading failed - try as inline code */
            lua_slm_dostring(L, argv[1]);
        }
    } else {
        shell_printf("Usage:\n");
        shell_printf("  lua                        - Enter interactive REPL\n");
        shell_printf("  lua -e \"code\"              - Execute code directly\n");
        shell_printf("  lua <filename> [args...]   - Run script with positional args\n");
    }

    if (close_on_return) {
        lua_slm_close(L);
    }

    return 0;
}

/* Command registration */
static const shell_cmd_t lua_commands[] = {
    {"lua", cmd_lua, "Lua scripting (REPL or script)", true},
};

void lua_shell_init(void) {
    for (size_t i = 0; i < sizeof(lua_commands) / sizeof(lua_commands[0]); i++) {
        shell_register_command(&lua_commands[i]);
    }
    lua_slm_init();
}
