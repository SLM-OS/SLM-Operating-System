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
 *   - All entry points take `d->lock` for the whole operation. A
 *     Chase-Lev-style lock-free path can replace `steal_deque_steal` and
 *     the unsynchronized parts of `steal_deque_pop` later without
 *     changing the API.
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
    for (uint32_t i = 0; i < STEAL_DEQUE_CAPACITY; i++)
        d->buf[i] = (struct task *)0;
    spin_init(&d->lock);
}

int steal_deque_push(steal_deque_t *d, struct task *t)
{
    irq_flags_t flags = spin_lock_irqsave(&d->lock);

    uint32_t size = d->bottom - d->top;
    if (size >= STEAL_DEQUE_CAPACITY) {
        spin_unlock_irqrestore(&d->lock, flags);
        return -1;
    }

    d->buf[d->bottom & DEQUE_MASK] = t;
    d->bottom++;

    spin_unlock_irqrestore(&d->lock, flags);
    return 0;
}

struct task *steal_deque_pop(steal_deque_t *d)
{
    irq_flags_t flags = spin_lock_irqsave(&d->lock);

    if (d->bottom == d->top) {
        spin_unlock_irqrestore(&d->lock, flags);
        return (struct task *)0;
    }

    d->bottom--;
    struct task *t = d->buf[d->bottom & DEQUE_MASK];
    d->buf[d->bottom & DEQUE_MASK] = (struct task *)0;

    spin_unlock_irqrestore(&d->lock, flags);
    return t;
}

struct task *steal_deque_steal(steal_deque_t *d)
{
    irq_flags_t flags = spin_lock_irqsave(&d->lock);

    if (d->top == d->bottom) {
        spin_unlock_irqrestore(&d->lock, flags);
        return (struct task *)0;
    }

    struct task *t = d->buf[d->top & DEQUE_MASK];
    d->buf[d->top & DEQUE_MASK] = (struct task *)0;
    d->top++;

    spin_unlock_irqrestore(&d->lock, flags);
    return t;
}

uint32_t steal_deque_size(const steal_deque_t *d)
{
    return d->bottom - d->top;
}

int steal_deque_is_empty(const steal_deque_t *d)
{
    return d->bottom == d->top;
}
