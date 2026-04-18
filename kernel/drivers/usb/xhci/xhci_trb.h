/*
 * xhci_trb.h - xHCI Transfer Request Block definitions
 *
 * Phase 3A Step 4: the subset needed to issue NO_OP commands and
 * consume command-completion events. Transfer TRBs (Setup/Data/Status
 * for control, bulk, interrupt) land in Step 6.
 */

#ifndef XHCI_TRB_H
#define XHCI_TRB_H

#include <stdint.h>

/*
 * Every TRB is 16 bytes / 4 dwords. Fields are little-endian as stored
 * in memory and read by the controller; we never byte-swap on ARM64.
 */
struct xhci_trb {
    uint32_t param_lo;   /* offset 0:  TRB-type-specific parameter */
    uint32_t param_hi;   /* offset 4 */
    uint32_t status;     /* offset 8:  completion code / xfer length */
    uint32_t control;    /* offset 12: type, cycle, flags */
} __attribute__((packed));
_Static_assert(sizeof(struct xhci_trb) == 16, "TRB must be 16 bytes");

/* -------------------------------------------------------------------------- */
/* Control-dword layout                                                        */
/* -------------------------------------------------------------------------- */

#define XHCI_TRB_CYCLE              (1u << 0)   /* cycle bit */
#define XHCI_TRB_ENT                (1u << 1)   /* evaluate next TRB */
#define XHCI_TRB_ISP                (1u << 2)   /* interrupt on short packet */
#define XHCI_TRB_NO_SNOOP           (1u << 3)
#define XHCI_TRB_CH                 (1u << 4)   /* chain bit */
#define XHCI_TRB_IOC                (1u << 5)   /* interrupt on completion */
#define XHCI_TRB_IDT                (1u << 6)   /* immediate data */
#define XHCI_TRB_TC                 (1u << 1)   /* Toggle Cycle (Link TRB only) */
#define XHCI_TRB_TYPE_SHIFT         10
#define XHCI_TRB_TYPE_MASK          (0x3Fu << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TYPE(type)         ((uint32_t)(type) << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TYPE_GET(ctrl)     (((ctrl) >> XHCI_TRB_TYPE_SHIFT) & 0x3F)

/* Slot ID occupies bits 31:24 of control dword on Command Completion events. */
#define XHCI_TRB_SLOT_SHIFT         24
#define XHCI_TRB_SLOT_GET(ctrl)     (((ctrl) >> XHCI_TRB_SLOT_SHIFT) & 0xFF)

/* Completion code occupies bits 31:24 of the status dword on event TRBs. */
#define XHCI_CC_SHIFT               24
#define XHCI_CC_MASK                (0xFFu << XHCI_CC_SHIFT)
#define XHCI_CC_GET(stat)           (((stat) >> XHCI_CC_SHIFT) & 0xFF)

/* Completion codes — only the ones we act on today. */
#define XHCI_CC_SUCCESS             1
#define XHCI_CC_DATA_BUF_ERR        2
#define XHCI_CC_BABBLE              3
#define XHCI_CC_USB_TRANSACTION_ERR 4
#define XHCI_CC_TRB_ERR             5
#define XHCI_CC_STALL               6
#define XHCI_CC_RESOURCE_ERR        7
#define XHCI_CC_BANDWIDTH_ERR       8
#define XHCI_CC_NO_SLOTS_AVAIL      9
#define XHCI_CC_SHORT_PACKET        13

/* -------------------------------------------------------------------------- */
/* TRB Types                                                                   */
/* -------------------------------------------------------------------------- */

/* Transfer TRBs (used in Step 6+). */
#define XHCI_TRB_NORMAL             1
#define XHCI_TRB_SETUP_STAGE        2
#define XHCI_TRB_DATA_STAGE         3
#define XHCI_TRB_STATUS_STAGE       4
#define XHCI_TRB_ISOCH              5
#define XHCI_TRB_LINK               6
#define XHCI_TRB_EVENT_DATA         7
#define XHCI_TRB_NOOP               8   /* transfer-ring no-op */

/* Command TRBs. */
#define XHCI_TRB_CMD_ENABLE_SLOT    9
#define XHCI_TRB_CMD_DISABLE_SLOT   10
#define XHCI_TRB_CMD_ADDRESS_DEVICE 11
#define XHCI_TRB_CMD_CONFIGURE_EP   12
#define XHCI_TRB_CMD_EVAL_CONTEXT   13
#define XHCI_TRB_CMD_RESET_EP       14
#define XHCI_TRB_CMD_STOP_EP        15
#define XHCI_TRB_CMD_SET_TR_DEQ     16
#define XHCI_TRB_CMD_RESET_DEVICE   17
#define XHCI_TRB_CMD_NOOP           23  /* command-ring no-op */

/* Event TRBs. */
#define XHCI_TRB_EVT_TRANSFER       32
#define XHCI_TRB_EVT_CMD_COMPLETION 33
#define XHCI_TRB_EVT_PORT_STATUS    34
#define XHCI_TRB_EVT_BANDWIDTH      35
#define XHCI_TRB_EVT_DOORBELL       36
#define XHCI_TRB_EVT_HOST           37

#endif /* XHCI_TRB_H */
