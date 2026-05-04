/*
 * shell_session.c - Session pool and per-task session lookup
 *
 * Provides the console session (always present, UART-backed) and a
 * fixed pool for TCP sessions. The bind-to-task mapping lets
 * shell_puts/shell_printf discover which session is running without
 * threading a shell_session* through every command handler.
 *
 * Concurrency model for sessions_by_task[]:
 *   - Each task only writes its own slot (via shell_session_bind or
 *     shell_session_unbind from the task that will own / has owned
 *     the binding).
 *   - Each task's shell_* wrappers read only its own slot (via
 *     task_current() -> id).
 *   - Aligned pointer reads/writes are atomic on both targets (ARM64
 *     and x86-64 guarantee single-machine-word accesses tear-free).
 *   - Therefore no lock is required between a read and a write of
 *     the same slot, and cross-slot accesses never collide.
 *
 *   The TCP pool (tcp_session_pool) is serialized with pool_lock
 *   because its allocation/free path runs on net_pump while the
 *   teardown path runs on the shell task.
 */

#include "shell_session.h"
#include "shell_io.h"
#include "task.h"
#include "config.h"
#include "lua_slm.h"
#include "spinlock.h"
#include "string.h"

#include <stddef.h>

/* Map from task id to the session bound to that task. Indexed directly
 * by task->id; slots for non-shell tasks stay NULL. */
static struct shell_session *sessions_by_task[MAX_TASKS];

/* Singleton console session (UART-backed). */
static struct shell_session console_session;
static bool                  console_session_ready;

/* Pool for non-console (currently TCP) sessions. id field is the slot
 * index + 1 so 0 stays reserved for the console. Alloc walks the pool
 * looking for !in_use slots; a spinlock serializes alloc/free so the
 * accept callback (net_pump ctx) and a session-teardown path on the
 * shell task can't race. */
static struct shell_session tcp_session_pool[MAX_TCP_SHELL_SESSIONS];
static spinlock_t            pool_lock = SPINLOCK_INIT;

static void session_reset_defaults(struct shell_session *s)
{
    s->cwd[0]             = '/';
    s->cwd[1]             = '\0';
    s->lua                = NULL;
    s->window_cols        = SHELL_DEFAULT_COLS;
    s->window_rows        = SHELL_DEFAULT_ROWS;
    s->term_type[0]       = '\0';
    s->interrupt_requested = false;
    memset(&s->xput, 0, sizeof(s->xput));
    /* Explicit "no LittleFS handle held" sentinel — `0` is a valid
     * file-handle value, so we can't rely on the memset above. */
    s->xput.fd = -1;

    /* Wipe any stale recall ring left behind from a previous occupant
     * of this pool slot, then put the cursor on the live edit buffer
     * so the next up arrow starts from the most recent entry rather
     * than mid-browse (#434). */
    memset(&s->history, 0, sizeof(s->history));
    s->history.cursor = -1;

    /* Drop any prefetched-but-unconsumed input bytes from the prior
     * occupant. A new session starts with an empty prefetch and
     * shell_read_command refills on first read (#597). */
    s->read_prefetch_pos = 0;
    s->read_prefetch_len = 0;
}

void shell_session_init(void)
{
    if (console_session_ready) {
        return;
    }

    console_session.id         = 0;
    console_session.io         = shell_io_uart();
    console_session.owner_task = NULL;
    console_session.in_use     = true;
    session_reset_defaults(&console_session);

    console_session_ready = true;
}

struct shell_session *shell_session_console(void)
{
    if (!console_session_ready) {
        shell_session_init();
    }
    return &console_session;
}

void shell_session_bind(struct task *t, struct shell_session *s)
{
    if (!t) {
        return;
    }
    if (t->id >= MAX_TASKS) {
        return;
    }
    sessions_by_task[t->id] = s;
    if (s) {
        s->owner_task = t;
    }
}

void shell_session_unbind(struct task *t)
{
    if (!t || t->id >= MAX_TASKS) {
        return;
    }
    struct shell_session *s = sessions_by_task[t->id];
    sessions_by_task[t->id] = NULL;
    if (s && s->owner_task == t) {
        s->owner_task = NULL;
    }
}

struct shell_session *shell_session_current(void)
{
    struct task *t = task_current();
    if (t && t->id < MAX_TASKS) {
        struct shell_session *s = sessions_by_task[t->id];
        if (s) {
            return s;
        }
    }
    /* No binding — fall back to the console session. Lazily initialize
     * it if needed so test harnesses that call shell_execute without
     * a preceding shell_init() still get a valid session. */
    if (!console_session_ready) {
        shell_session_init();
    }
    return &console_session;
}

struct shell_session *shell_session_alloc(void)
{
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    for (uint32_t i = 0; i < MAX_TCP_SHELL_SESSIONS; i++) {
        struct shell_session *s = &tcp_session_pool[i];
        if (!s->in_use) {
            s->in_use     = true;
            s->id         = i + 1;   /* 0 reserved for console */
            s->io         = NULL;
            s->owner_task = NULL;
            session_reset_defaults(s);
            spin_unlock_irqrestore(&pool_lock, flags);
            return s;
        }
    }
    spin_unlock_irqrestore(&pool_lock, flags);
    return NULL;
}

void shell_session_free(struct shell_session *s)
{
    if (!s || s == &console_session) {
        return;
    }
    /* Close any held xput fd before tearing down the session.
     * Without this, a peer that opens an `xput begin` and
     * disconnects before `xput finish` (or before a chunk-error
     * recovery path runs) leaks one of the LFS_SLM_MAX_FILES = 4
     * LittleFS file handles per failed session. After 4 such
     * teardowns the mount is wedged and every subsequent open
     * returns LFS_ERR_NOMEM. Run this OUTSIDE pool_lock — the
     * close path takes the LFS mount's own spinlock and we
     * don't want to nest. */
    shell_xput_session_close_for(s);
    if (s->lua) {
        lua_slm_close((lua_State *)s->lua);
        s->lua = NULL;
    }
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    s->in_use     = false;
    s->io         = NULL;
    s->owner_task = NULL;
    spin_unlock_irqrestore(&pool_lock, flags);
}

bool shell_interrupt_requested(void)
{
    struct shell_session *s = shell_session_current();
    return s && s->interrupt_requested;
}

void shell_clear_interrupt(void)
{
    struct shell_session *s = shell_session_current();
    if (s) {
        s->interrupt_requested = false;
    }
}
