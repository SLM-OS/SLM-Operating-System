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

/* Maximum number of non-console sessions (e.g. TCP). Chosen small to
 * keep per-session state (stack + Lua interpreter slot + ring buffers)
 * bounded. Bump with care: each slot costs roughly 64 KB task stack +
 * 8 KB ring buffers = ~72 KB. */
#define MAX_TCP_SHELL_SESSIONS 2

struct task;

/* Forward-declare without pulling in <lua.h>. The field is void * so
 * this header stays lua-agnostic; lua_shell.c casts when it assigns. */
struct lua_State;

struct shell_session {
    uint32_t         id;                  /* 0 = console; 1..N = TCP */
    struct shell_io *io;                  /* input/output backend */
    char             cwd[VFS_MAX_PATH];   /* current working directory */
    struct task     *owner_task;          /* task running this session (NULL if unbound) */
    bool             in_use;              /* pool slot occupancy */

    /* Per-session Lua interpreter slot. Unused today — the `lua`
     * command still creates and tears down a fresh state per
     * invocation (see kernel/src/lua_shell.c). When Lua starts being
     * driven from a long-running remote session, the `lua` command
     * will lazily allocate this on first use and tear it down when
     * the session closes. Access via void * to avoid pulling <lua.h>
     * into this header. */
    void            *lua;
};

/* Get the singleton console session (UART-backed). Always non-NULL
 * once the shell subsystem has been initialized. */
struct shell_session *shell_session_console(void);

/* Allocate a session from the TCP pool. Returns NULL if the pool is
 * exhausted. The returned session has id > 0, cwd set to "/", and
 * io/owner_task left NULL for the caller to populate. */
struct shell_session *shell_session_alloc(void);

/* Return a session previously obtained from shell_session_alloc to
 * the pool. Safe to call with NULL or the console session (no-op in
 * both cases). The caller is responsible for closing the shell_io
 * before freeing. */
void shell_session_free(struct shell_session *s);

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
 * console session if no binding exists. Lazily initializes the
 * console session so test harnesses that call into shell command
 * dispatch without first calling shell_init() still get a valid
 * session. Never returns NULL. */
struct shell_session *shell_session_current(void);

#endif /* SHELL_SESSION_H */
