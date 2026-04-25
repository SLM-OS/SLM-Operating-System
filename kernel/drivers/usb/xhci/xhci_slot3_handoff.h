/*
 * xhci_slot3_handoff.h - Linux -> SLM-OS handoff block for the retained
 * full-speed slot-3 path on Jetson xHCI.
 *
 * Linux writes this block into SLM-OS's NC memory just before kexec.
 * SLM-OS reads it during xHCI init so it can recover the controller's
 * live slot-3 device-context pointer even when the inherited DCBAA
 * itself lives above SLM-OS's directly probeable RAM aperture.
 */

#ifndef XHCI_SLOT3_HANDOFF_H
#define XHCI_SLOT3_HANDOFF_H

#include <stdint.h>

#define XHCI_SLOT3_HANDOFF_MAGIC  0x58483348U  /* "XH3H" */
#define XHCI_SLOT3_HANDOFF_VERSION 1U
#define XHCI_SLOT3_HANDOFF_CTX_DCIS 7U
#define XHCI_SLOT3_HANDOFF_EP_DWORDS 8U
#define XHCI_SLOT3_HANDOFF_PAYLOAD_U64S \
    (4U + 4U + (XHCI_SLOT3_HANDOFF_CTX_DCIS * XHCI_SLOT3_HANDOFF_EP_DWORDS))
#define XHCI_SLOT3_HANDOFF_PHYS   0xBDE00000ULL
#define XHCI_SLOT3_DEVCXT_MIRROR_PHYS 0xBDE01000ULL
#define XHCI_SLOT3_INPUT_CTX_PHYS     0xBDE02000ULL
#define XHCI_SLOT3_EP0_RING_PHYS      0xBDE03000ULL
#define XHCI_SLOT3_EP0_RING_SIZE      0x00002000ULL
#define XHCI_SLOT3_RESERVED_BYTES     0x00005000ULL
#define XHCI_SLOT3_EP0_RING_TRBS      64U
#define XHCI_SLOT3_EP0_RING_BYTES     (XHCI_SLOT3_EP0_RING_TRBS * 16U)

struct xhci_slot3_handoff {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_id;
    uint32_t root_port;
    uint64_t dcbaap;
    uint64_t devctx_phys;
    uint64_t ep0_deq_phys;
    uint32_t slot_ctx_dw[4];
    uint32_t ep0_ctx_dw[8];
};

_Static_assert(sizeof(struct xhci_slot3_handoff) == 88,
               "xhci_slot3_handoff layout changed");

#endif /* XHCI_SLOT3_HANDOFF_H */
