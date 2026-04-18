/*
 * xhci_ring.h - command / transfer / event ring state + helpers.
 *
 * Phase 3A Step 4: command ring (producer) + event ring (consumer)
 * backed by a single NC-memory segment each. Transfer rings reuse
 * the producer state machine in Step 6.
 */

#ifndef XHCI_RING_H
#define XHCI_RING_H

#include "xhci_trb.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * A producer ring (command ring, transfer ring). One segment with a
 * Link TRB at the end that points back to the start. The cycle bit
 * toggles every time we traverse the Link.
 */
struct xhci_ring {
    struct xhci_trb *trbs;        /* NC-memory virtual pointer */
    uintptr_t        phys;        /* physical == virtual on NC */
    uint32_t         num_trbs;    /* including the Link TRB */
    uint32_t         enqueue;     /* producer index */
    uint8_t          cycle_state; /* PCS — starts at 1 */
};

/*
 * The event ring is consumer-only from the software side. The HC
 * writes events and advances its own producer; we advance the
 * dequeue index and toggle our ECS when we hit the end of a segment.
 * Phase 3A uses a single segment, so no link handling on this side.
 */
struct xhci_event_ring {
    struct xhci_trb *trbs;
    uintptr_t        phys;
    uint32_t         num_trbs;
    uint32_t         dequeue;
    uint8_t          cycle_state; /* ECS — starts at 1 */
};

/*
 * Initialise a producer ring on a caller-supplied buffer. Zero it,
 * write the Link TRB at the end pointing back at the first TRB.
 * `trbs_phys` is the physical / DMA-visible address; on Jetson with
 * NC memory that equals the virtual pointer.
 *
 * Returns 0 on success, -1 on bad args.
 */
int  xhci_ring_init(struct xhci_ring *r, struct xhci_trb *trbs,
                    uintptr_t trbs_phys, uint32_t num_trbs);

/*
 * Allocate NC memory via ncmem_alloc + call xhci_ring_init on it.
 * On OOM returns -1 and leaves *r zero. Jetson-only path; non-Jetson
 * platforms should drive xhci_ring_init directly with their own
 * allocator (e.g. unit tests use a stack-allocated TRB array).
 */
int  xhci_ring_alloc(struct xhci_ring *r, uint32_t num_trbs);

/*
 * Initialise an event ring on a caller-supplied buffer. Zero it,
 * set dequeue to 0 and cycle state to 1.
 */
int  xhci_event_ring_init(struct xhci_event_ring *r, struct xhci_trb *trbs,
                          uintptr_t trbs_phys, uint32_t num_trbs);

/* ncmem-backed variant for Jetson runtime. */
int  xhci_event_ring_alloc(struct xhci_event_ring *r, uint32_t num_trbs);

/*
 * Append a TRB to a producer ring. `t` is the TRB with its parameter,
 * status, and type bits already filled in; the cycle bit is OR'd in
 * here to match the ring's current PCS. If the enqueue pointer is
 * currently on the Link TRB, wrap via the Link and toggle PCS
 * before writing.
 *
 * Returns the virtual address of the TRB as placed in the ring
 * (which equals the physical address for completion-event
 * correlation on NC memory). NULL if the ring is full — we don't
 * track a consumer pointer yet in Phase 3A, so "full" just means
 * wrapping back to the link TRB, which is always permitted here.
 */
struct xhci_trb *xhci_ring_enqueue(struct xhci_ring *r,
                                   const struct xhci_trb *t);

/*
 * Look at the TRB at the current event-ring dequeue pointer. If its
 * cycle bit matches our ECS, an event is pending; copy it into
 * *out, advance the dequeue index (toggling ECS on wrap), and return
 * true. Otherwise return false — caller spins or returns.
 *
 * Does NOT write ERDP. The caller is responsible for batching event
 * consumption and posting a final ERDP update.
 */
bool xhci_event_ring_peek(struct xhci_event_ring *r, struct xhci_trb *out);

/*
 * Dequeue-pointer physical address for programming the interrupter's
 * ERDP register. The hardware expects the address of the NEXT TRB to
 * be consumed, with the EHB bit set in the low dword.
 */
uintptr_t xhci_event_ring_dequeue_phys(const struct xhci_event_ring *r);

#endif /* XHCI_RING_H */
