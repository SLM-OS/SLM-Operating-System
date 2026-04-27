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
#include "shell.h"
#include "shell_io.h"

/* Maximum number of non-console sessions (e.g. TCP). Kept fixed so the
 * session/task/ring-buffer footprint stays statically bounded. Each slot
 * costs roughly 64 KB task stack + 8 KB ring buffers = ~72 KB, so 16 TCP
 * sessions reserve about 1.125 MB before any future per-session Lua state. */
#define MAX_TCP_SHELL_SESSIONS 16

struct task;

/* Forward-declare without pulling in <lua.h>. The field is void * so
 * this header stays lua-agnostic; lua_shell.c casts when it assigns. */
struct lua_State;

/* Default terminal size if NAWS hasn't been negotiated. Matches the
 * venerable 80x24 fallback for a VT100. */
#define SHELL_DEFAULT_COLS 80
#define SHELL_DEFAULT_ROWS 24

/* Max terminal-type string (mirrors telnet.h's TELNET_TTYPE_MAX but
 * this header doesn't pull in telnet.h). */
#define SHELL_TERM_TYPE_MAX 32

struct shell_xput_session {
    bool     active;
    char     path[VFS_MAX_PATH];
    uint32_t expected_size;
    uint32_t received_size;
    uint32_t checksum;
};

/* Per-session command-history ring. Sized at 32 × 128 = 4 KB of payload
 * plus three byte-sized counters; the entire suite (16 TCP slots +
 * console) fits in ~64 KB of BSS. Indexed newest-first by browse
 * cursor: cursor 0 selects the most recently added entry, cursor
 * count-1 selects the oldest, cursor -1 means the user is on the
 * live edit buffer (no recall). See docs/archive/plans/shell-command-history-plan.md. */
struct shell_history {
    char     entries[SHELL_HISTORY_DEPTH][SHELL_HISTORY_LINE_MAX];
    uint8_t  count;     /* valid entries (saturates at SHELL_HISTORY_DEPTH) */
    uint8_t  head;      /* slot the next add() overwrites (modulo DEPTH) */
    int8_t   cursor;    /* -1 = live edit buffer; 0..count-1 = recalled */
};

struct shell_session {
    uint32_t         id;                  /* 0 = console; 1..N = TCP */
    struct shell_io *io;                  /* input/output backend */
    char             cwd[VFS_MAX_PATH];   /* current working directory */
    struct task     *owner_task;          /* task running this session (NULL if unbound) */
    bool             in_use;              /* pool slot occupancy */

    /* Per-session Lua interpreter slot. Lazily allocated on first
     * `lua` command use in this session and torn down when the
     * session closes. Access via void * to avoid pulling <lua.h>
     * into this header. */
    void            *lua;

    /* Terminal metadata.
     *
     * For TCP sessions these are populated from telnet NAWS /
     * TERMINAL-TYPE negotiation. For the console session they stay
     * at the 80x24 defaults (UART has no way to learn window size).
     * Commands like `top` use these dimensions to lay out output;
     * term_type lets commands decide whether to emit ANSI colours.
     * Updated from net_pump context on subneg, read by the shell
     * task — volatile on the 16-bit fields so the reader doesn't
     * need a barrier.
     *
     * term_type is a multi-byte buffer without a lock. In practice
     * clients send a single TERMINAL-TYPE IS early in the session
     * and never update it, so readers observe a stable string. A
     * pathological client that re-sends TTYPE mid-session could
     * cause a torn read on one iteration; the next read sees a
     * consistent string. Callers that care (e.g. `top` checking
     * "xterm*") may want a `strncpy` into a local buffer before
     * acting on the contents. */
    volatile uint16_t window_cols;
    volatile uint16_t window_rows;
    char              term_type[SHELL_TERM_TYPE_MAX];

    /* Interrupt request (telnet IAC IP / Ctrl+C). Set from the
     * net_pump tcp_recv callback when a telnet client sends IP;
     * long-running shell commands can poll shell_interrupt_requested()
     * and bail out cleanly. shell_read_line also treats this like a
     * ^C at the prompt. Cleared by shell_clear_interrupt() once the
     * command has acted on it. */
    volatile bool     interrupt_requested;

    /* Per-session framed upload state for `xput`. Scoped to the shell
     * connection so concurrent operators cannot clobber each other's
     * transfers. */
    struct shell_xput_session xput;

    /* Per-session command history (#434). The REPL line reader pushes
     * each non-blank, non-duplicate command in here, and up / down
     * arrow recall walks the ring. Console + each TCP session has its
     * own copy so two operators editing in parallel cannot see each
     * other's recall buffer. */
    struct shell_history history;
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

/* True if the current session has a pending interrupt request that
 * hasn't been cleared yet. Long-running commands should poll this
 * at convenient yield points and bail out early when set. */
bool shell_interrupt_requested(void);

/* Clear the current session's interrupt flag. Call after acting on
 * a detected interrupt so the next one can re-fire. */
void shell_clear_interrupt(void);

#endif /* SHELL_SESSION_H */
