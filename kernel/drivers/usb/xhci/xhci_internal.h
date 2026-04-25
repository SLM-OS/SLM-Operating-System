/*
 * xhci_internal.h - state + helpers shared between xhci.c, xhci_device.c,
 *                   and xhci_xfer.c.
 *
 * Phase 3A Steps 5-7 split the driver across three .c files to keep
 * any single TU below the 1000-line threshold noted in xhci.c's
 * file-level comment. This header is the shared ABI between them.
 *
 * Not a public interface — kernel/include/ never sees this.
 */

#ifndef XHCI_INTERNAL_H
#define XHCI_INTERNAL_H

#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "xhci_trb.h"
#include "xhci_ctx.h"
#include "usb.h"
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Per-device HCD state                                                        */
/* -------------------------------------------------------------------------- */

/*
 * One of these attaches to `struct usb_device.hcd_private` for the
 * lifetime of the device. Phase 3A supports a single root device, so
 * the state lives in a file-scope pool (xhci_device.c) rather than
 * being ncmem_alloc'd per-device.
 *
 * `dev_ctx` / `input_ctx` are NC-memory byte buffers accessed through
 * xhci_ctx.h helpers — never overlaid with a C struct — so the CSZ=1
 * 64-byte-context padding doesn't poison the field offsets.
 */
struct xhci_device {
    bool                valid;
    bool                adopted_inherited;
    uint8_t             slot_id;
    uint8_t             root_port;       /* 1-based per xHCI §4.19 */
    void               *dev_ctx;         /* written to DCBAA[slot] */
    void               *input_ctx;       /* command input — reused for CONFIGURE_EP */
    void               *controller_devctx; /* retained Linux output ctx, if any */
    uintptr_t           dev_ctx_phys;
    uintptr_t           input_ctx_phys;
    uintptr_t           controller_devctx_phys;

    /*
     * One transfer ring per DCI we've enabled. Slot 0 is unused; EP0
     * lives at DCI 1; non-control endpoints at DCI 2..31. Rings are
     * allocated lazily in device_open (EP0) and endpoint_configure
     * (others); freed in device_close.
     */
    struct xhci_ring   *ep_rings[32];
};

/* -------------------------------------------------------------------------- */
/* Driver-wide state defined in xhci.c                                         */
/* -------------------------------------------------------------------------- */

extern bool                    xhci_live;
extern struct xhci_caps        xhci_caps_cached;
extern volatile uint8_t       *xhci_cap_base;
extern volatile uint8_t       *xhci_op_base;
extern volatile uint8_t       *xhci_rt_base;
extern volatile uint8_t       *xhci_db_base;
extern struct xhci_ring        xhci_cmd_ring;
extern struct xhci_event_ring  xhci_evt_ring;
extern uint64_t               *xhci_dcbaa;
extern uintptr_t               xhci_inherited_slot1_devctx_raw_phys;
extern uintptr_t               xhci_inherited_slot1_ep0_deq_phys;
extern bool                    xhci_inherited_slot1_ctx_valid;
extern uint32_t                xhci_inherited_slot1_slot_ctx_dw[4];
extern uint32_t                xhci_inherited_slot1_ep0_ctx_dw[8];
extern uintptr_t               xhci_inherited_slot3_devctx_phys;
extern uintptr_t               xhci_inherited_slot3_devctx_raw_phys;
extern uintptr_t               xhci_inherited_slot3_ep0_deq_phys;
extern bool                    xhci_inherited_slot3_ctx_valid;
extern uint32_t                xhci_inherited_slot3_slot_ctx_dw[4];
extern uint32_t                xhci_inherited_slot3_ep0_ctx_dw[8];
extern uint32_t                xhci_inherited_slot3_ep_ctx_dw[8][8];

struct xhci_ctrl_diag {
    bool      valid;
    bool      adopted;
    bool      completed;
    bool      timed_out;
    uint8_t   slot_id;
    uint8_t   dci;
    uint8_t   request;
    uint8_t   request_type;
    uint8_t   stage;
    uint8_t   cc;
    uint16_t  value;
    uint16_t  index;
    uint16_t  length;
    int32_t   status;
    uint32_t  actual;
    uint32_t  residual;
    uintptr_t trb_phys;
    uintptr_t next_trb_phys;
    uint32_t  enqueue;
    uint32_t  pcs;
};

struct xhci_cmd_diag {
    bool      valid;
    bool      completed;
    bool      timed_out;
    uint8_t   type;
    uint8_t   cc;
    uint8_t   slot_id;
    uint32_t  usbsts;
    uintptr_t trb_phys;
};

extern struct xhci_ctrl_diag   xhci_last_ctrl_diag;
extern struct xhci_cmd_diag    xhci_last_cmd_diag;

/* -------------------------------------------------------------------------- */
/* Low-level MMIO helpers exported by xhci.c                                   */
/* -------------------------------------------------------------------------- */

uint32_t xhci_op_r32(uint32_t off);
void     xhci_op_w32(uint32_t off, uint32_t val);
void     xhci_tegra_restore_context(const char *why);
void     xhci_dump_runtime_state(const char *tag);
void     xhci_dump_runtime_state_slot(const char *tag, uint8_t slot_id);

/* Doorbell at `db_index` (0 = command, 1..MaxSlots = device slot). */
void xhci_ring_doorbell(uint8_t db_index, uint8_t target);

/* -------------------------------------------------------------------------- */
/* Command + event ring interface (xhci.c implements)                          */
/* -------------------------------------------------------------------------- */

/*
 * Submit one command TRB and block up to `timeout_ms` for its
 * matching Command Completion event. On success returns 0 and fills
 * `*cc_out` with the completion code + `*slot_out` with the slot id
 * reported on the event (meaningful for ENABLE_SLOT). Either pointer
 * may be NULL.
 *
 * Events unrelated to the pending command are fully consumed during
 * the wait — Transfer Events dispatch to xhci_xfer_on_transfer_event,
 * everything else is logged and dropped.
 */
int xhci_cmd_submit_and_wait(const struct xhci_trb *cmd,
                             uint8_t *cc_out,
                             uint8_t *slot_out,
                             uint32_t timeout_ms);

/* Drain every currently-pending event from the event ring. Returns
 * the number of events consumed. Safe to call with no pending events. */
int xhci_event_ring_drain(void);
void xhci_defer_post_short_noop(uint8_t slot_id);
void xhci_run_deferred_probes(void);

/* -------------------------------------------------------------------------- */
/* URB dispatch hooks (xhci_xfer.c implements)                                 */
/* -------------------------------------------------------------------------- */

void xhci_xfer_on_transfer_event(const struct xhci_trb *evt);

/* -------------------------------------------------------------------------- */
/* Port decode (xhci_device.c implements, tests use directly)                  */
/* -------------------------------------------------------------------------- */

/* Decode a PORTSC register value into (connected, speed). Returns
 * true iff the input represents a valid USB 2.0 high/full/low-speed
 * port state the driver recognises. SuperSpeed (speed ID 4) returns
 * true with USB_SPEED_SUPER; higher / reserved IDs return false. */
bool xhci_decode_portsc(uint32_t portsc, bool *connected,
                        enum usb_speed *speed);
void xhci_dump_port_state(void);
void xhci_dump_device_state(void);

/* -------------------------------------------------------------------------- */
/* HCD op table — defined in xhci.c, ops split across .c files                 */
/* -------------------------------------------------------------------------- */

extern const struct usb_hcd xhci_hcd;

int   xhci_hcd_start(void);
bool  xhci_hcd_port_status(uint8_t port, bool *connected, enum usb_speed *speed);
int   xhci_hcd_port_reset(uint8_t port);
int   xhci_hcd_device_open(struct usb_device *dev);
void  xhci_hcd_device_close(struct usb_device *dev);
int   xhci_hcd_endpoint_configure(struct usb_device *dev,
                                  const struct usb_endpoint *ep);
int   xhci_hcd_submit_urb(struct usb_urb *urb);
int   xhci_hcd_cancel_urb(struct usb_urb *urb);
void  xhci_hcd_poll(void);

#endif /* XHCI_INTERNAL_H */
