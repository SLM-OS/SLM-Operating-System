/**
 * lua_shell.c - Lua Shell Commands for SLM-OS
 *
 * Provides 'lua' command for interactive scripting and script execution.
 */

#include "lua_slm.h"
#include "shell.h"
#include "debug.h"

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
    lua_State *L = lua_slm_newstate();
    if (L == NULL) {
        uart_printf("Failed to initialize Lua\n");
        return -1;
    }

    if (argc == 1) {
        /* No arguments - enter REPL */
        lua_slm_repl(L);
    } else if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'e') {
        /* -e "code" - execute code */
        lua_slm_dostring(L, argv[2]);
    } else if (argc == 2) {
        /* Filename - try to run script */
        if (lua_slm_dofile(L, argv[1]) != 0) {
            /* File loading not yet implemented - try as inline code */
            lua_slm_dostring(L, argv[1]);
        }
    } else {
        uart_printf("Usage:\n");
        uart_printf("  lua              - Enter interactive REPL\n");
        uart_printf("  lua -e \"code\"    - Execute code directly\n");
        uart_printf("  lua <filename>   - Run script from file\n");
    }

    lua_slm_close(L);
    return 0;
}

/* Command registration */
static const shell_cmd_t lua_commands[] = {
    {"lua", cmd_lua, "Lua scripting (REPL or script)"},
};

void lua_shell_init(void) {
    for (size_t i = 0; i < sizeof(lua_commands) / sizeof(lua_commands[0]); i++) {
        shell_register_command(&lua_commands[i]);
    }
    lua_slm_init();
}
