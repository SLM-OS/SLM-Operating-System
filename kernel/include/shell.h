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
 * Command category — drives the grouping in `help` output.
 *
 * Pick the closest fit when adding a new command. Order of values
 * here is also the order categories appear in `help`. cmd_help
 * sorts entries alphabetically *within* each category, so the order
 * of registration does not matter — pick any spot in the per-category
 * block of builtin_commands[] / external_commands[].
 */
typedef enum {
    SHELL_CAT_SHELL,        /* Shell session control: clear, help, reboot */
    SHELL_CAT_FILESYSTEM,   /* File / directory ops: ls, cd, cat, write, ... */
    SHELL_CAT_SYSINFO,      /* Read-only system info: mem, cpu, uptime, ... */
    SHELL_CAT_PROCESS,      /* Tasks, scheduling, benchmarks, AI runtime */
    SHELL_CAT_COMPONENTS,   /* Component lifecycle + IPC: component, msg */
    SHELL_CAT_SCRIPTING,    /* Lua, ELF programs */
    SHELL_CAT_NETWORK,      /* TCP/IP stack and apps: net, ping, http, ... */
    SHELL_CAT_HARDWARE,     /* Device control + diagnostics: peek, gpu, ... */
    SHELL_CAT_COUNT,        /* Sentinel: number of real categories */
} shell_cmd_category_t;

/*
 * Command definition.
 *
 * `mutates = true` if any of the command's subcommands modify global
 * kernel state that lacks its own lock (task lifecycle, active
 * scheduler / eviction policy, component registry, network config).
 * Such commands are serialized by the dispatcher so two concurrent
 * shell sessions cannot race each other's mutations. Read-only
 * commands and commands that mutate through an already-locked
 * subsystem (VFS, PMM, etc.) are `mutates = false` and bypass the
 * lock.
 *
 * `category` controls grouping in `help`. See shell_cmd_category_t
 * above. cmd_help groups by category and alphabetizes within each
 * group, so the array does not need to be sorted at registration
 * time — but the convention enforced by the comment block above
 * builtin_commands[] in shell.c is to keep the array grouped +
 * alphabetized too, so a human reading the registration list sees
 * the same ordering as the help output.
 */
typedef struct {
    const char *name;                   /* Command name */
    shell_handler_t handler;            /* Handler function */
    const char *help;                   /* Short help text */
    bool mutates;                       /* True if serialization required */
    /* REQUIRED — explicit `.category = SHELL_CAT_<X>` is mandatory.
     * A 4-field positional initializer or designated init that omits
     * .category will silently zero-init this to SHELL_CAT_SHELL (the
     * first enum value) and the command will appear under the Shell
     * heading in `help` regardless of where it actually belongs. */
    shell_cmd_category_t category;      /* Grouping for `help` output */
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
 *
 * Returns when the current session's I/O reports EOF (shell_read_line
 * returning -1) — used by TCP session tasks to exit cleanly on peer
 * disconnect. The console session never hits EOF, so for the UART
 * shell task this is effectively a no-return loop.
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

/* Temporarily release the shell mutation lock if the current task holds it.
 * Intended for long-running mutating handlers that need to block on input,
 * then reacquire before touching unlocked global state again. Returns true
 * when the lock was released and must later be resumed. */
bool shell_mutation_pause(void);

/* Reacquire the shell mutation lock after shell_mutation_pause() returned
 * true. Safe to call with false (no-op). */
void shell_mutation_resume(bool paused);

/*
 * Read one line from the current session's I/O with basic line editing
 * (backspace, Ctrl+C). Echoes input as it arrives and terminates on
 * CR/LF. Output is NUL-terminated.
 *
 * Returns the number of characters read (excluding the terminator)
 * on success, 0 if the user pressed Ctrl+C to cancel, or -1 if the
 * session closed (peer disconnect on a TCP session). Callers that
 * loop should break out on -1.
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

/* ============================================================================
 * Per-session command history (#434)
 *
 * Up arrow recalls the previous command in the running session; down
 * arrow walks back toward the live edit buffer. Storage lives on
 * shell_session->history (see shell_session.h). The line-edit loop
 * used by the REPL is shell_read_command(); it emits the prompt,
 * runs the ESC-sequence parser, and calls into the APIs below.
 *
 * Out of scope: reverse search, prefix search, persistence across
 * reboot, in-line cursor movement, multi-line commands. See
 * docs/shell-command-history-plan.md.
 * ============================================================================ */

#define SHELL_HISTORY_DEPTH    32
#define SHELL_HISTORY_LINE_MAX 128

struct shell_session;  /* Defined in shell_session.h. */

/*
 * Capture a freshly entered command into the session's ring buffer.
 * Skips empty / whitespace-only lines and exact duplicates of the
 * most recent entry. Truncates at SHELL_HISTORY_LINE_MAX - 1 chars
 * (NUL preserved). Always resets the browse cursor to "live buffer"
 * so the next up arrow starts from the most recent entry.
 *
 * Safe to call with NULL session or NULL line — both no-op.
 */
void shell_history_add(struct shell_session *s, const char *line);

/*
 * Step the cursor toward older entries and return the entry now under
 * the cursor. Returns NULL when the cursor cannot move (history empty
 * or already at the oldest entry). The returned pointer aliases storage
 * inside the session's ring; callers must copy before issuing another
 * shell_history_add() against the same session.
 */
const char *shell_history_prev(struct shell_session *s);

/*
 * Step the cursor toward newer entries / the live edit buffer. Returns
 * NULL when the cursor is already on the live buffer (no movement). When
 * the cursor was at the most recent entry and a step would land back on
 * the live buffer, returns an empty string sentinel — the caller treats
 * that as "redraw the prompt with no recalled text".
 */
const char *shell_history_next(struct shell_session *s);

/*
 * Force the browse cursor back to the live edit buffer. Called by the
 * line-edit loop after a Ctrl+C cancellation so the next prompt does
 * not resume the previous browse position.
 */
void shell_history_reset_cursor(struct shell_session *s);

/*
 * REPL line reader with prompt + history. Emits `prompt` itself, then
 * reads one line into `buf`, honoring backspace, Ctrl+C, and the up /
 * down arrow recall sequences. Returns the same length / 0 / -1 codes
 * as shell_read_line().
 *
 * Callers that need a plain line read without history (e.g. Lua's
 * slm.read_line) should keep using shell_read_line(); only the REPL
 * uses this entry point.
 */
int shell_read_command(const char *prompt, char *buf, int max_len);

#endif /* SHELL_H */
