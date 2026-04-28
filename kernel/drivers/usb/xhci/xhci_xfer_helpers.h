/*
 * xhci_xfer_helpers.h — Pure-logic helpers extracted from xhci_xfer.c
 * so they can be unit-tested independent of the Jetson-only HCD code
 * the rest of that translation unit is gated under.
 */

#ifndef XHCI_XFER_HELPERS_H
#define XHCI_XFER_HELPERS_H

#include <stdint.h>

/*
 * Compute urb->actual_length for a Transfer Event.
 *
 * The xHCI Transfer-Event "TRB Transfer Length" field reports the
 * residual byte count NOT transferred for the TRB that completed
 * (xHCI §6.4.2.1). The same "requested - residual" rule covers both
 * stages an IOC/ISP event may fire from on a control transfer:
 *
 *   - Status Stage success (Status Stage carries no payload, so the
 *     event reports residual=0): actual = requested - 0 = requested.
 *   - Data Stage short packet (Data Stage TRB sets ISP, firing an
 *     event with residual=N-M for an M-of-N byte short read):
 *     actual = requested - (N-M) = M.
 *
 * Pre-#316 the driver short-circuited this to "actual = requested
 * if cc=SUCCESS else 0", which silently over-reported on short-packet
 * control-IN transfers because the URB completed via the Status Stage
 * SUCCESS event after the Data Stage SHORT_PACKET event was being
 * dropped as "unmatched" (the slot was only registered against the
 * Status TRB physical address). The slot-lookup half of that bug is
 * fixed by tracking all three TRB phys addresses; this helper pins
 * the formula half so a regression in xhci_xfer.c's accounting can
 * be caught by a unit test.
 *
 * Defensive: a malformed event with residual > requested clamps to
 * 0 rather than wrapping into a huge unsigned value.
 */
static inline uint32_t xhci_xfer_actual_from_residual(uint32_t requested,
                                                      uint32_t residual)
{
    return (residual <= requested) ? (requested - residual) : 0U;
}

#endif /* XHCI_XFER_HELPERS_H */
