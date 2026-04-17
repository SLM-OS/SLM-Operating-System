/*
 * shell_session.h - Per-session shell state
 *
 * A shell_session ties a shell REPL to a specific shell_io backend and
 * carries per-session state (cwd, Lua interpreter, future peer info).
 * The console session is created at boot; additional sessions are
 * allocated from a fixed pool when a TCP connection is accepted
 * (Phase 1.3, future).
 *
 * Every shell task has exactly one session bound to it via
 * shell_session_bind(); shell_session_current() returns that session
 * for the running task. Background tasks (net_pump, idle, driver ISRs)
 * have no session and must use uart_* directly.
 */

#ifndef SHELL_SESSION_H
#define SHELL_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "vfs.h"
#include "shell_io.h"

struct task;

struct shell_session {
    uint32_t         id;                  /* 0 = console; 1..N = TCP */
    struct shell_io *io;                  /* input/output backend */
    char             cwd[VFS_MAX_PATH];   /* current working directory */
    struct task     *owner_task;          /* task running this session (NULL if unbound) */
    bool             in_use;              /* pool slot occupancy */
};

/* Get the singleton console session (UART-backed). Always non-NULL
 * once the shell subsystem has been initialized. */
struct shell_session *shell_session_console(void);

/* Initialize the console session. Must be called once before any
 * shell_session_current() call. Safe to call multiple times — only
 * the first call has effect. */
void shell_session_init(void);

/* Bind a session to a task so shell_session_current() finds it when
 * that task is running. */
void shell_session_bind(struct task *t, struct shell_session *s);

/* Remove any binding for this task. */
void shell_session_unbind(struct task *t);

/* Return the session bound to the currently running task, or the
 * console session if no binding exists. Never returns NULL once
 * shell_session_init() has run. Before that (early boot) it may
 * return NULL. */
struct shell_session *shell_session_current(void);

#endif /* SHELL_SESSION_H */
