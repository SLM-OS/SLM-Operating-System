/*
 * steal_deque.c - Bounded deque for per-CPU work-stealing run queues
 *
 * Issue #59, Phase A. See kernel/include/steal_deque.h for semantics.
 *
 * Implementation notes:
 *   - Indices grow monotonically (no wraparound reset). The modulo
 *     STEAL_DEQUE_CAPACITY happens only at array access time. This is
 *     lifted straight from the Chase-Lev paper — it makes owner/thief
 *     races straightforward to reason about in a future lock-free port.
 *   - Capacity MUST be a power of two; the mask (`CAP - 1`) avoids a
 *     divide instruction.
 *   - All entry points are **lockless internally** — the caller is
 *     responsible for serializing access. sched.c holds
 *     `steal_deque_lock[cpu]` (cacheable) across every push / pop /
 *     steal / remove. The deque itself lives in NC memory on
 *     PLATFORM_HAS_NC_MEMORY, where an embedded spinlock would be
 *     neutered by SPINLOCK_SKIP_LOCKING.
 */

#include "steal_deque.h"
#include "task.h"

#include <stdint.h>

_Static_assert((STEAL_DEQUE_CAPACITY & (STEAL_DEQUE_CAPACITY - 1)) == 0,
               "STEAL_DEQUE_CAPACITY must be a power of two");

#define DEQUE_MASK (STEAL_DEQUE_CAPACITY - 1)

void steal_deque_init(steal_deque_t *d)
{
    d->bottom = 0;
    d->top = 0;
    for (uint32_t i = 0; i < STEAL_DEQUE_CAPACITY; i++) {
        d->buf[i] = (struct task *)0;
        d->gen_buf[i] = 0;
    }
}

int steal_deque_push(steal_deque_t *d, struct task *t)
{
    uint32_t size = d->bottom - d->top;
    if (size >= STEAL_DEQUE_CAPACITY) {
        return -1;
    }

    uint32_t idx = d->bottom & DEQUE_MASK;
    d->buf[idx] = t;
    /* Capture the task's current generation (#139). The task pointer
     * this slot holds is this-task's-life-number `t->generation` —
     * any later task_destroy + recycle will bump `t->generation` so
     * the still-in-deque entry becomes distinguishable as stale. */
    d->gen_buf[idx] = t->generation;
    d->bottom++;

    return 0;
}

struct task *steal_deque_pop(steal_deque_t *d, uint32_t *out_gen)
{
    /* Skip NULL slots left behind by `steal_deque_remove` — those were
     * live entries at push time but the task has been terminated or
     * recycled. Not advancing past them would let the caller see a
     * NULL and mistake the deque for empty, while actual live tasks
     * sit deeper in the live range. */
    while (d->bottom != d->top) {
        d->bottom--;
        uint32_t idx = d->bottom & DEQUE_MASK;
        struct task *t = d->buf[idx];
        uint32_t gen = d->gen_buf[idx];
        d->buf[idx] = (struct task *)0;
        d->gen_buf[idx] = 0;
        if (t) {
            if (out_gen) *out_gen = gen;
            return t;
        }
    }

    return (struct task *)0;
}

struct task *steal_deque_steal(steal_deque_t *d, uint32_t *out_gen)
{
    /* Same NULL-skip as pop. Bounded by (bottom - top) probes. */
    while (d->top != d->bottom) {
        uint32_t idx = d->top & DEQUE_MASK;
        struct task *t = d->buf[idx];
        uint32_t gen = d->gen_buf[idx];
        d->buf[idx] = (struct task *)0;
        d->gen_buf[idx] = 0;
        d->top++;
        if (t) {
            if (out_gen) *out_gen = gen;
            return t;
        }
    }

    return (struct task *)0;
}

uint32_t steal_deque_size(const steal_deque_t *d)
{
    return d->bottom - d->top;
}

int steal_deque_is_empty(const steal_deque_t *d)
{
    return d->bottom == d->top;
}

int steal_deque_remove(steal_deque_t *d, struct task *t)
{
    if (t == (struct task *)0)
        return 0;

    int cleared = 0;
    /* Walk the live range [top, bottom). Monotonic indices; mask at
     * access time. A matching slot is cleared to NULL in-place rather
     * than compacted — the steal and pop paths skip NULL slots, so
     * leaving a gap costs one extra probe at steal time but avoids
     * moving elements (which would require updating both top and
     * bottom atomically).
     *
     * Exit on first match: a task pointer should appear at most once.
     * If a user pushes the same pointer twice (a bug), only the first
     * hit is cleared; the second will be surfaced by a later steal and
     * rejected by the state validation in sched_try_steal. */
    for (uint32_t i = d->top; i != d->bottom; i++) {
        if (d->buf[i & DEQUE_MASK] == t) {
            d->buf[i & DEQUE_MASK] = (struct task *)0;
            d->gen_buf[i & DEQUE_MASK] = 0;
            cleared = 1;
            break;
        }
    }

    return cleared;
}
