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
#include <stddef.h>
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
 * Read one line from the current session's I/O with basic line editing
 * (backspace, Ctrl+C). Echoes input as it arrives and terminates on
 * CR/LF. Output is NUL-terminated. Returns the number of characters
 * read (excluding the terminator).
 *
 * This is the line reader that the shell REPL uses internally; exposed
 * so Lua scripting (slm.read_line) and other callers can prompt the
 * user without duplicating the implementation.
 */
int shell_read_line(char *buf, int max_len);

/* ============================================================================
 * Per-session I/O wrappers
 *
 * These route through shell_session_current()->io. Use them inside
 * command handlers and anywhere shell output is produced. Kernel logs
 * (driver INFO/WARN, panic handlers, background tasks) should continue
 * to use uart_* directly so they always reach the physical console.
 * ============================================================================ */

/* Write a null-terminated string to the current session's I/O. */
void shell_puts(const char *s);

/* Write a single character to the current session's I/O. */
void shell_putc(char c);

/* printf-style formatted write to the current session's I/O.
 * Same format specifiers as uart_printf. */
int  shell_printf(const char *fmt, ...);

/* Blocking read of one byte (0..255) from the current session's I/O.
 * Returns -1 if the session has closed. */
int  shell_getc(void);

/* Non-blocking read. Returns a byte (0..255) or -1 if no data is
 * available right now (or the session has closed). */
int  shell_try_getc(void);

#endif /* SHELL_H */
