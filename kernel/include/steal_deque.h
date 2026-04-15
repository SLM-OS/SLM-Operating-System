/*
 * steal_deque.h - Bounded deque for per-CPU work-stealing run queues
 *
 * Issue #59, Phase A data structure + unit tests; wired into the
 * scheduler by Phase B.
 *
 * Semantics:
 *   - Owner CPU pushes and pops at the "bottom" end (LIFO — cache-warm).
 *   - Thief CPUs steal at the "top" end (FIFO — oldest task).
 *   - Capacity is fixed and must be a power of two (cheap modulo).
 *   - **Lockless internally** — callers are responsible for serializing
 *     every entry point externally. The scheduler uses
 *     `steal_deque_lock[MAX_CPUS]` (cacheable) for this; the deque
 *     itself lives in NC memory on PLATFORM_HAS_NC_MEMORY where an
 *     embedded spinlock would be neutered by SPINLOCK_SKIP_LOCKING.
 *     Tests hold no lock and rely on single-thread execution.
 *
 * The deque stores struct task pointers; a NULL return signals empty.
 */

#ifndef STEAL_DEQUE_H
#define STEAL_DEQUE_H

#include <stdint.h>

struct task; /* forward declaration */

#define STEAL_DEQUE_CAPACITY 32  /* must be power of 2 and >= MAX_TASKS */

typedef struct {
    struct task *buf[STEAL_DEQUE_CAPACITY];
    /* Parallel array of generation captures (#139). gen_buf[i] is the
     * value of buf[i]->generation at push time. Steal / pop return
     * this alongside the pointer so the validator in sched_try_steal
     * can detect an ABA — if the current task->generation no longer
     * matches, the slot points at a recycled logical task and must
     * be discarded even if the state / assigned_cpu / affinity checks
     * would otherwise pass. */
    uint32_t gen_buf[STEAL_DEQUE_CAPACITY];
    uint32_t bottom;      /* next free slot on owner side */
    uint32_t top;         /* next slot to steal on thief side */
} steal_deque_t;

/* Initialize an empty deque. Caller must hold the appropriate external
 * lock if initialization can race with concurrent access. */
void steal_deque_init(steal_deque_t *d);

/*
 * Owner-side push (at bottom). Returns 0 on success, -1 if full.
 * Caller must hold the external steal-deque lock for this deque.
 */
int steal_deque_push(steal_deque_t *d, struct task *t);

/*
 * Owner-side pop (from bottom, LIFO). Returns the task or NULL if empty.
 * Intended for the owning CPU's schedule() fast path.
 * Caller must hold the external steal-deque lock.
 *
 * If `out_gen` is non-NULL and the return is non-NULL, the captured
 * generation counter for the returned task is written there. Callers
 * who need ABA detection (sched_try_steal) pass a real pointer;
 * callers that only need the task pointer pass NULL.
 */
struct task *steal_deque_pop(steal_deque_t *d, uint32_t *out_gen);

/*
 * Thief-side steal (from top, FIFO). Returns the task or NULL if empty.
 * Callable from any CPU. Caller must hold the external steal-deque
 * lock. `out_gen` semantics as for steal_deque_pop.
 */
struct task *steal_deque_steal(steal_deque_t *d, uint32_t *out_gen);

/* Current count of tasks in the deque (racy without external lock). */
uint32_t steal_deque_size(const steal_deque_t *d);

/* True if the deque has no tasks (racy without external lock). */
int steal_deque_is_empty(const steal_deque_t *d);

/*
 * Best-effort remove of a specific task pointer from the deque. O(n)
 * scan from top to bottom; sets the matching slot to NULL if found.
 * Callers: `scheduler_terminate_task` and anywhere else a task pointer
 * is about to become stale (task_destroy, task_table recycle), to
 * prevent an ABA race where a thief steals a NULL-cleared slot and
 * a recycled task pointer reuses that slot before the thief validates.
 *
 * The steal path skips NULL slots during traversal, so a NULL'd slot
 * is effectively gone.
 *
 * Returns the number of slots cleared (0 or 1). Caller must hold the
 * external steal-deque lock.
 */
int steal_deque_remove(steal_deque_t *d, struct task *t);

#endif /* STEAL_DEQUE_H */
