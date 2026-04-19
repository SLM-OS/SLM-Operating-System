/*
 * xhci_attach.h - Hot-plug state machine for #309 (post-kexec re-plug
 *                  workaround).
 *
 * Extracted from xhci_hcd_port_status so the pure state-transition
 * logic can be unit-tested without a live xHCI controller. The .c
 * side owns the MMIO read, the enumeration flag, and the log prints;
 * this header owns the decision of "what does usb_core see?" given
 * (current phase, raw PORTSC CCS+speed).
 *
 *   STALE          Pre-kexec device seen at init. CCS=1 but device
 *                  state is whatever Linux left — first EP0 control
 *                  transfer fails cc=4. Hide until unplug.
 *   WAIT_RECONNECT Device has been unplugged (or was never present
 *                  at init). Report disconnected until CCS rises.
 *   FRESH          Clean attach observed; pass through raw CCS/speed
 *                  to usb_core. Stays in FRESH thereafter — a future
 *                  disconnect reports disconnected but does not
 *                  re-arm the stale-hide logic.
 *
 * See issue #309 for the permanent fix that eliminates the re-plug
 * requirement.
 */

#ifndef XHCI_ATTACH_H
#define XHCI_ATTACH_H

#include "usb.h"

#include <stdbool.h>

enum xhci_attach_phase {
    XHCI_ATTACH_STALE = 0,
    XHCI_ATTACH_WAIT_RECONNECT,
    XHCI_ATTACH_FRESH,
};

struct xhci_attach_result {
    enum xhci_attach_phase next_state;
    bool                   report_connected;
    enum usb_speed         report_speed;
    bool                   transitioned;   /* true iff next_state != in */
};

/*
 * Pure state-transition step. Caller passes the current phase plus
 * the raw (connected, speed) decoded from PORTSC; gets back the new
 * phase and the (connected, speed) that should be surfaced to
 * usb_core. No MMIO, no logging, no globals.
 */
static inline struct xhci_attach_result
xhci_attach_step(enum xhci_attach_phase state,
                 bool raw_connected,
                 enum usb_speed raw_speed)
{
    struct xhci_attach_result r = {
        .next_state       = state,
        .report_connected = false,
        .report_speed     = USB_SPEED_UNKNOWN,
        .transitioned     = false,
    };

    switch (state) {
    case XHCI_ATTACH_STALE:
        if (!raw_connected) {
            r.next_state   = XHCI_ATTACH_WAIT_RECONNECT;
            r.transitioned = true;
        }
        /* Hide the stale device regardless of raw CCS. */
        return r;

    case XHCI_ATTACH_WAIT_RECONNECT:
        if (raw_connected) {
            r.next_state       = XHCI_ATTACH_FRESH;
            r.transitioned     = true;
            r.report_connected = true;
            r.report_speed     = raw_speed;
        }
        /* Otherwise report disconnected and stay in WAIT_RECONNECT. */
        return r;

    case XHCI_ATTACH_FRESH:
        /* Pass through raw CCS/speed. Once FRESH, stay FRESH — a
         * disconnect reports disconnected but does not re-arm the
         * stale-hide logic (the dongle has already been enumerated
         * at least once, so any future attach is trustworthy). */
        r.report_connected = raw_connected;
        r.report_speed     = raw_speed;
        return r;
    }

    /* Unreachable — enum exhausted. Defensive: report disconnected. */
    return r;
}

#endif /* XHCI_ATTACH_H */
