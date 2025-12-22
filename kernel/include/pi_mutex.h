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

#include <stdint.h>
#include "spinlock.h"

/* Forward declaration */
struct task;

/*
 * Priority-inheriting mutex.
 *
 * Uses a spinlock internally to protect the mutex state,
 * but the actual wait is done with a spin loop that allows
 * the scheduler to preempt (IRQs enabled).
 */
typedef struct {
    spinlock_t guard;           /* Protects mutex state */
    struct task *owner;         /* Current owner (NULL if unlocked) */
    uint8_t owner_original_pri; /* Owner's priority before inheritance */
    volatile uint8_t locked;    /* 1 if locked, 0 if unlocked */
} pi_mutex_t;

#define PI_MUTEX_INIT { \
    .guard = SPINLOCK_INIT, \
    .owner = NULL, \
    .owner_original_pri = 0, \
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
 * Restores the owner's priority to its original value
 * (or the highest of any remaining waiters if we implement
 * a proper wait queue in the future).
 */
void pi_mutex_unlock(pi_mutex_t *mutex);

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
