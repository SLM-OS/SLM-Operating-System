/*
 * xhci_trb_build.h - TRB builder helpers (pure logic, testable).
 *
 * Headed off into its own header so both xhci_xfer.c (production) and
 * kernel/tests/test_xhci_xfer.c (host tests) link against the same
 * inline definitions. Zero state — every function writes one caller-
 * supplied struct xhci_trb from its arguments.
 *
 * References are to xHCI 1.2 §6.4.1 and §6.4.2.
 */

#ifndef XHCI_TRB_BUILD_H
#define XHCI_TRB_BUILD_H

#include "xhci_trb.h"
#include "usb.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * Setup Stage TRB (§6.4.1.2.1). The 8-byte SETUP packet goes into
 * param_lo/param_hi as immediate data (IDT=1). `status` = Interrupter
 * Target (31:22) | reserved | TRB Transfer Length (16:0) = 8. The
 * control dword carries the TRT field (17:16), IDT bit, the cycle
 * bit (bit 0), and the TRB type (15:10).
 *
 * `trt_field` encodes the transfer direction of the Data Stage (if
 * any): 0 = No Data, 2 = OUT, 3 = IN.
 */
static inline void xhci_build_setup_stage(struct xhci_trb *out,
                                          const struct usb_setup_packet *setup,
                                          uint32_t trt_field,
                                          uint8_t cycle)
{
    memset(out, 0, sizeof(*out));
    memcpy(&out->param_lo, setup, sizeof(*setup));
    out->status  = 8U;
    out->control = XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) |
                   XHCI_TRB_IDT |
                   (trt_field << 16) |
                   ((uint32_t)cycle & 0x1U);
}

/*
 * Data Stage TRB (§6.4.1.2.2). `param` is the data buffer physical
 * address. `status` carries the TRB Transfer Length. DIR bit (bit 16
 * of the control dword) is set for IN, cleared for OUT. ISP is set
 * so a short-packet IN raises an event.
 */
static inline void xhci_build_data_stage(struct xhci_trb *out,
                                         uintptr_t buf_phys,
                                         uint32_t length,
                                         bool in,
                                         uint8_t cycle)
{
    memset(out, 0, sizeof(*out));
    out->param_lo = (uint32_t)(buf_phys & 0xFFFFFFFFu);
    out->param_hi = (uint32_t)(buf_phys >> 32);
    out->status   = length & 0x1FFFFU;
    uint32_t dir  = in ? (1u << 16) : 0u;
    out->control  = XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) |
                    dir | XHCI_TRB_ISP |
                    ((uint32_t)cycle & 0x1U);
}

/*
 * Status Stage TRB (§6.4.1.2.3). No data transfer; `in` is the
 * direction of the Status Stage *handshake* packet, which is
 * opposite to the Data Stage direction (or IN for a no-data control
 * transfer per §4.11.2.2). IOC is set so this TRB's completion
 * event references the whole control transfer.
 */
static inline void xhci_build_status_stage(struct xhci_trb *out,
                                           bool in,
                                           uint8_t cycle)
{
    memset(out, 0, sizeof(*out));
    uint32_t dir = in ? (1u << 16) : 0u;
    out->control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) |
                   dir | XHCI_TRB_IOC |
                   ((uint32_t)cycle & 0x1U);
}

/*
 * Normal TRB (§6.4.1.1) for bulk / interrupt transfers. Phase 3A
 * always sets IOC so the single TRB corresponds to one URB; ISP is
 * set so a short IN terminates the TRB and raises an event with
 * residual byte count.
 */
static inline void xhci_build_normal(struct xhci_trb *out,
                                     uintptr_t buf_phys,
                                     uint32_t length,
                                     uint8_t cycle)
{
    memset(out, 0, sizeof(*out));
    out->param_lo = (uint32_t)(buf_phys & 0xFFFFFFFFu);
    out->param_hi = (uint32_t)(buf_phys >> 32);
    out->status   = length & 0x1FFFFU;
    out->control  = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) |
                    XHCI_TRB_ISP | XHCI_TRB_IOC |
                    ((uint32_t)cycle & 0x1U);
}

#endif /* XHCI_TRB_BUILD_H */
