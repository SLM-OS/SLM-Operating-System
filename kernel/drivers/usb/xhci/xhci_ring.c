/*
 * xhci_ring.c - NC-memory-backed command / event ring primitives.
 *
 * Phase 3A Step 4 of #266. Producer ring (commands, transfers) +
 * consumer ring (events). Single-segment, 64-byte aligned. Cycle
 * bit handling follows xHCI 1.2 §4.9.
 *
 * Structure: `*_init` variants operate on caller-supplied memory and
 * are platform-agnostic — unit tests (`kernel/tests/test_xhci_ring.c`)
 * drive them with stack-allocated TRB arrays on every platform the
 * test harness builds for. The `*_alloc` wrappers are Jetson-only
 * because they pull ncmem.
 */

#include "xhci_ring.h"
#include "debug.h"

#include <string.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Platform-agnostic ring primitives                                           */
/* -------------------------------------------------------------------------- */

int xhci_ring_init(struct xhci_ring *r, struct xhci_trb *trbs,
                   uintptr_t trbs_phys, uint32_t num_trbs)
{
    if (r == NULL || trbs == NULL || num_trbs < 4)
        return -1;

    memset(trbs, 0, (size_t)num_trbs * sizeof(struct xhci_trb));

    r->trbs        = trbs;
    r->phys        = trbs_phys;
    r->num_trbs    = num_trbs;
    r->enqueue     = 0;
    r->cycle_state = 1;

    /*
     * Place a Link TRB at the last slot pointing back to the first.
     * TC (Toggle Cycle) = 1 so the HC flips its Consumer Cycle State
     * on traversal; our enqueue path does the same for PCS.
     *
     * Note: the Link TRB's cycle bit is NOT set here — it's a consumer
     * marker that we never need to toggle on the producer side; the
     * Link TRB sits "between" iterations. The HC writes no data to
     * the Link TRB; it just reads param_lo/hi to know where to jump.
     */
    struct xhci_trb *link = &trbs[num_trbs - 1];
    link->param_lo = (uint32_t)(trbs_phys & 0xFFFFFFFFu);
    link->param_hi = (uint32_t)(trbs_phys >> 32);
    link->status   = 0;
    link->control  = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC;

    return 0;
}

int xhci_event_ring_init(struct xhci_event_ring *r, struct xhci_trb *trbs,
                         uintptr_t trbs_phys, uint32_t num_trbs)
{
    if (r == NULL || trbs == NULL || num_trbs < 4)
        return -1;

    memset(trbs, 0, (size_t)num_trbs * sizeof(struct xhci_trb));

    r->trbs        = trbs;
    r->phys        = trbs_phys;
    r->num_trbs    = num_trbs;
    r->dequeue     = 0;
    r->cycle_state = 1;
    return 0;
}

struct xhci_trb *xhci_ring_enqueue(struct xhci_ring *r,
                                   const struct xhci_trb *t)
{
    if (r == NULL || t == NULL || r->trbs == NULL)
        return NULL;

    /*
     * If the enqueue slot IS the Link TRB, flip the Link's cycle bit
     * so the HC follows it, wrap the enqueue index to 0, and toggle
     * PCS. Then fall through to write the incoming TRB at slot 0.
     */
    if (r->enqueue == r->num_trbs - 1) {
        struct xhci_trb *link = &r->trbs[r->enqueue];
        uint32_t ctrl = link->control & ~XHCI_TRB_CYCLE;
        ctrl |= (r->cycle_state & 1);
        link->control = ctrl;
        r->enqueue = 0;
        r->cycle_state ^= 1;
    }

    struct xhci_trb *slot = &r->trbs[r->enqueue];
    /* Write the payload first, then the control dword with the
     * cycle bit matching PCS. The HC uses the cycle bit to decide
     * the TRB is "ready", so this store order must hold all the
     * way through to DRAM — NC memory makes that free. */
    slot->param_lo = t->param_lo;
    slot->param_hi = t->param_hi;
    slot->status   = t->status;
    uint32_t ctrl  = t->control & ~XHCI_TRB_CYCLE;
    ctrl |= (r->cycle_state & 1);
    slot->control  = ctrl;

    r->enqueue++;
    return slot;
}

bool xhci_event_ring_peek(struct xhci_event_ring *r, struct xhci_trb *out)
{
    if (r == NULL || out == NULL || r->trbs == NULL)
        return false;

    struct xhci_trb *slot = &r->trbs[r->dequeue];
    if ((slot->control & XHCI_TRB_CYCLE) != (r->cycle_state & 1))
        return false;   /* no event yet */

    /* Copy out, then advance. Reading all four dwords here is fine;
     * the HC won't touch them again until we lap it. */
    out->param_lo = slot->param_lo;
    out->param_hi = slot->param_hi;
    out->status   = slot->status;
    out->control  = slot->control;

    r->dequeue++;
    if (r->dequeue >= r->num_trbs) {
        r->dequeue = 0;
        r->cycle_state ^= 1;
    }
    return true;
}

uintptr_t xhci_event_ring_dequeue_phys(const struct xhci_event_ring *r)
{
    if (r == NULL || r->trbs == NULL)
        return 0;
    return r->phys + (uintptr_t)r->dequeue * sizeof(struct xhci_trb);
}

/* -------------------------------------------------------------------------- */
/* ncmem-backed allocation wrappers (Jetson only)                              */
/* -------------------------------------------------------------------------- */

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "ncmem.h"

int xhci_ring_alloc(struct xhci_ring *r, uint32_t num_trbs)
{
    size_t bytes = (size_t)num_trbs * sizeof(struct xhci_trb);
    void  *mem   = ncmem_alloc(bytes, 64);
    if (mem == NULL) {
        WARN("xhci_ring: ncmem_alloc(%zu) failed", bytes);
        return -1;
    }
    return xhci_ring_init(r, (struct xhci_trb *)mem, (uintptr_t)mem, num_trbs);
}

int xhci_event_ring_alloc(struct xhci_event_ring *r, uint32_t num_trbs)
{
    size_t bytes = (size_t)num_trbs * sizeof(struct xhci_trb);
    void  *mem   = ncmem_alloc(bytes, 64);
    if (mem == NULL) {
        WARN("xhci_event_ring: ncmem_alloc(%zu) failed", bytes);
        return -1;
    }
    return xhci_event_ring_init(r, (struct xhci_trb *)mem,
                                (uintptr_t)mem, num_trbs);
}

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/* No ncmem on non-Jetson platforms. Callers that need allocation
 * are expected to drive xhci_ring_init with their own buffer. */
int xhci_ring_alloc(struct xhci_ring *r, uint32_t n)            { (void)r; (void)n; return -1; }
int xhci_event_ring_alloc(struct xhci_event_ring *r, uint32_t n) { (void)r; (void)n; return -1; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
