/*
 * shell_session.c - Session pool and per-task session lookup
 *
 * Provides the console session (always present, UART-backed) and a
 * fixed pool for future TCP sessions. The bind-to-task mapping lets
 * shell_puts/shell_printf discover which session is running without
 * threading a shell_session* through every command handler.
 *
 * The task_id -> session pointer map is write-sparingly (only on
 * session create/destroy) and read hot (every shell_printf). Pointer
 * writes/reads are atomic on our 64-bit targets, so no lock is needed
 * to serialize map reads against writes.
 */

#include "shell_session.h"
#include "shell_io.h"
#include "task.h"
#include "config.h"
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

void shell_session_init(void)
{
    if (console_session_ready) {
        return;
    }

    console_session.id         = 0;
    console_session.io         = shell_io_uart();
    console_session.cwd[0]     = '/';
    console_session.cwd[1]     = '\0';
    console_session.owner_task = NULL;
    console_session.in_use     = true;
    console_session.lua        = NULL;

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
            s->lua        = NULL;
            s->cwd[0]     = '/';
            s->cwd[1]     = '\0';
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
    irq_flags_t flags = spin_lock_irqsave(&pool_lock);
    s->in_use     = false;
    s->io         = NULL;
    s->owner_task = NULL;
    s->lua        = NULL;
    spin_unlock_irqrestore(&pool_lock, flags);
}
