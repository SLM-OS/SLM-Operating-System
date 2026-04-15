/*
 * shell.h - Debug Shell for SLM-OS
 *
 * Minimal command-line interface for system inspection and debugging.
 * Runs as a dedicated task, reading from UART and dispatching commands.
 */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* Shell prompt */
#define SHELL_PROMPT        "slmos> "

/*
 * Command handler function type.
 * Returns 0 on success, negative on error.
 */
typedef int (*shell_handler_t)(int argc, char *argv[]);

/*
 * Command definition.
 */
typedef struct {
    const char *name;           /* Command name */
    shell_handler_t handler;    /* Handler function */
    const char *help;           /* Short help text */
} shell_cmd_t;

/*
 * Initialize the shell subsystem.
 * Registers built-in commands.
 */
void shell_init(void);

/*
 * Start the shell task.
 * Creates a task that runs the shell loop on CPU 0.
 * Should be called after scheduler is running.
 */
void shell_start(void);

/*
 * Shell main loop (runs in shell task).
 * Reads input, parses commands, dispatches to handlers.
 * Does not return.
 */
void shell_run(void);

/*
 * Register an external command.
 * Returns 0 on success, -1 if command table is full.
 */
int shell_register_command(const shell_cmd_t *cmd);

/*
 * Execute a command string directly.
 * Useful for scripting or testing.
 * Returns command's return value, or -1 if command not found.
 */
int shell_execute(const char *cmdline);

/*
 * Read one line from the UART with basic line editing (backspace, Ctrl+C).
 * Echoes input as it arrives and terminates on CR/LF. Output is NUL-terminated.
 * Returns the number of characters read (excluding the terminator).
 *
 * This is the line reader that the shell REPL uses internally; exposed so Lua
 * scripting (slm.read_line) and other callers can prompt the user without
 * duplicating the implementation.
 */
int shell_read_line(char *buf, int max_len);

#endif /* SHELL_H */
