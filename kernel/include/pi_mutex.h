/*
 * pi_mutex.h - Priority-Inheriting Mutex for SLM-OS
 *
 * Implements priority inheritance to prevent priority inversion.
 * When a high-priority task blocks on a mutex held by a lower-priority
 * task, the holder's priority is temporarily boosted.
 *
 * Unlike spinlocks (which disable IRQs), pi_mutex allows preemption
 * while waiting, making priority inheritance meaningful.
 */

#ifndef PI_MUTEX_H
#define PI_MUTEX_H

#include <stdbool.h>
#include <stdint.h>
#include "spinlock.h"

/* Forward declaration */
struct task;

/*
 * Priority-inheriting mutex.
 *
 * Uses a spinlock internally to protect the mutex state. A contended
 * locker blocks on the mutex's wait queue (#93). The inherited priority
 * lives in task->pi_priority (#1103) and is dropped when the holder releases
 * the last PI mutex it holds, so a task holding several can stay boosted a
 * little longer than strictly needed, but never less. Inheritance is one
 * level: a boosted holder blocked on another mutex does not pass it on.
 */
typedef struct {
    spinlock_t guard;           /* Protects mutex state */
    struct task *owner;         /* Current owner (NULL if unlocked) */
    struct task *wait_head;     /* Head of blocked-waiter FIFO (#93) */
    struct task *wait_tail;     /* Tail of blocked-waiter FIFO */
    volatile uint8_t locked;    /* 1 if locked, 0 if unlocked */
} pi_mutex_t;

#define PI_MUTEX_INIT { \
    .guard = SPINLOCK_INIT, \
    .owner = NULL, \
    .wait_head = NULL, \
    .wait_tail = NULL, \
    .locked = 0 \
}

/*
 * Initialize a priority-inheriting mutex.
 */
void pi_mutex_init(pi_mutex_t *mutex);

/*
 * Acquire a priority-inheriting mutex.
 *
 * If the mutex is held by a lower-priority task, that task's
 * priority is boosted to match the caller's priority.
 *
 * Blocks until the mutex is acquired.
 */
void pi_mutex_lock(pi_mutex_t *mutex);

/*
 * Try to acquire a priority-inheriting mutex without blocking.
 *
 * Returns: 1 if acquired, 0 if mutex was already held.
 */
int pi_mutex_trylock(pi_mutex_t *mutex);

/*
 * Release a priority-inheriting mutex.
 *
 * If this was the last PI mutex the owner holds, drops its inherited priority
 * and recomputes its effective priority. If tasks are waiting, hands the mutex
 * to the highest-priority one and wakes it; the waiters left behind set its
 * inherited priority.
 */
void pi_mutex_unlock(pi_mutex_t *mutex);

/*
 * True iff the mutex is currently held by the calling task.
 *
 * Intended for non-recursive callers that want to skip a nested
 * acquire without changing the lock discipline. Safe to call from
 * any context; reads the owner field under the guard spinlock to
 * avoid tearing against concurrent lock/unlock.
 */
bool pi_mutex_held_by_self(pi_mutex_t *mutex);

/*
 * Check if a priority inversion was detected.
 *
 * Returns: 1 if priority inheritance was applied, 0 otherwise.
 *
 * This is set during pi_mutex_lock() if the caller had to
 * boost the owner's priority.
 */
int pi_mutex_inversion_detected(void);

/*
 * Get count of priority inversions detected since boot.
 */
uint32_t pi_mutex_inversion_count(void);

#endif /* PI_MUTEX_H */
