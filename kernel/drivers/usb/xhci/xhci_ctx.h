/*
 * xhci_ctx.h - xHCI Slot / Endpoint / Input context layouts + helpers
 *
 * Phase 3A Step 5+ of #266. Spec references are to xHCI 1.2 §6.2:
 *
 *   Slot Context         §6.2.2  (32 bytes baseline)
 *   Endpoint Context     §6.2.3  (32 bytes baseline)
 *   Input Control Ctx    §6.2.5.1 (32 bytes baseline)
 *   Input Context        §6.2.5  (Input Control Ctx + Slot Ctx + 31 EP Ctx)
 *   Device Context       §6.2.1  (Slot Ctx + 31 EP Ctx)
 *
 * The controller reports a "Context Size" bit in HCCPARAMS1:
 *   CSZ=0 — each context is exactly the 32-byte baseline above
 *   CSZ=1 — each context occupies 64 bytes (the spec layout plus
 *           32 bytes of reserved padding at the tail)
 *
 * Tegra234 reports CSZ=1. The driver stores contexts in a raw byte
 * buffer and uses `xhci_ctx_slot_ctx()` / `xhci_ctx_ep_ctx()` to
 * address them by DCI; those helpers multiply by 32 or 64 based on
 * `ctx_64`. Callers never see a C struct overlaid on the HW memory,
 * which keeps the CSZ=1 padding from poisoning the field offsets.
 *
 * DCI (Device Context Index) convention (§6.2.3):
 *   0 — Slot Context (not an endpoint)
 *   1 — EP0 bidirectional control
 *   2·N     — EP N OUT                (for N ≥ 1)
 *   2·N + 1 — EP N IN                 (for N ≥ 1)
 *   total — 32 contexts (slot + 31 endpoint)
 */

#ifndef XHCI_CTX_H
#define XHCI_CTX_H

#include <stdint.h>
#include <stdbool.h>

/* Context byte sizes. */
#define XHCI_CTX_SIZE_32            32U
#define XHCI_CTX_SIZE_64            64U

/* Device Context = Slot Context + 31 Endpoint Contexts = 32 contexts. */
#define XHCI_NUM_CONTEXTS           32U

/* Input Context = Input Control Ctx + Device Context = 33 contexts. */
#define XHCI_INPUT_NUM_CONTEXTS     33U

/* DCI helpers. */
#define XHCI_DCI_SLOT               0U
#define XHCI_DCI_EP0                1U
static inline unsigned xhci_dci_ep(uint8_t ep_address)
{
    /* bEndpointAddress: low 4 bits = ep number (1..15),
     * bit 7 = direction (0=OUT, 1=IN). DCI = 2*N + direction. */
    unsigned n   = ep_address & 0x0F;
    unsigned dir = (ep_address & 0x80) ? 1U : 0U;
    return (2U * n) + dir;
}

/* Return the byte size of one context given the controller's CSZ bit. */
static inline uint32_t xhci_ctx_stride(bool ctx_64)
{
    return ctx_64 ? XHCI_CTX_SIZE_64 : XHCI_CTX_SIZE_32;
}

/* Byte offset of the DCI-th context within a Device Context. */
static inline uint32_t xhci_ctx_dev_offset(unsigned dci, bool ctx_64)
{
    return dci * xhci_ctx_stride(ctx_64);
}

/* Byte offset of the DCI-th context within an Input Context. Input
 * Control Context is at DCI 0-equivalent; Slot Ctx at DCI 1-equivalent
 * from the buffer start, i.e. (DCI + 1) * stride. */
static inline uint32_t xhci_ctx_in_offset(unsigned dci, bool ctx_64)
{
    return (dci + 1U) * xhci_ctx_stride(ctx_64);
}

/* Total byte size of a Device Context buffer. */
static inline uint32_t xhci_ctx_dev_bytes(bool ctx_64)
{
    return XHCI_NUM_CONTEXTS * xhci_ctx_stride(ctx_64);
}

/* Total byte size of an Input Context buffer. */
static inline uint32_t xhci_ctx_in_bytes(bool ctx_64)
{
    return XHCI_INPUT_NUM_CONTEXTS * xhci_ctx_stride(ctx_64);
}

/* -------------------------------------------------------------------------- */
/* Slot Context (§6.2.2) — first 32 bytes                                     */
/* -------------------------------------------------------------------------- */

/* DW0 */
#define XHCI_SLOT_DW0_ROUTE_MASK    0x000FFFFFU             /* bits 19:0 */
#define XHCI_SLOT_DW0_SPEED_SHIFT   20
#define XHCI_SLOT_DW0_SPEED_MASK    (0xFU << XHCI_SLOT_DW0_SPEED_SHIFT)
#define XHCI_SLOT_DW0_MTT           (1U  << 25)
#define XHCI_SLOT_DW0_HUB           (1U  << 26)
#define XHCI_SLOT_DW0_CTXENT_SHIFT  27
#define XHCI_SLOT_DW0_CTXENT_MASK   (0x1FU << XHCI_SLOT_DW0_CTXENT_SHIFT)

/* DW1 */
#define XHCI_SLOT_DW1_ROOT_PORT_SHIFT  16
#define XHCI_SLOT_DW1_ROOT_PORT_MASK   (0xFFU << XHCI_SLOT_DW1_ROOT_PORT_SHIFT)

/* DW3 */
#define XHCI_SLOT_DW3_ADDR_MASK     0xFFU
#define XHCI_SLOT_DW3_STATE_SHIFT   27
#define XHCI_SLOT_DW3_STATE_MASK    (0x1FU << XHCI_SLOT_DW3_STATE_SHIFT)

/* Speed encoding in the Slot Context DW0 matches PORTSC speed bits
 * (xHCI §7.2.2.1.1 Table 7-13). USB 2 values: 1=Full, 2=Low, 3=High. */

/* -------------------------------------------------------------------------- */
/* Endpoint Context (§6.2.3) — first 32 bytes                                 */
/* -------------------------------------------------------------------------- */

/* DW0 */
#define XHCI_EP_DW0_STATE_MASK      0x7U
#define XHCI_EP_DW0_MULT_SHIFT      8
#define XHCI_EP_DW0_INTERVAL_SHIFT  16
#define XHCI_EP_DW0_INTERVAL_MASK   (0xFFU << XHCI_EP_DW0_INTERVAL_SHIFT)

/* DW1 */
#define XHCI_EP_DW1_CERR_SHIFT      1
#define XHCI_EP_DW1_CERR_MASK       (0x3U << XHCI_EP_DW1_CERR_SHIFT)
#define XHCI_EP_DW1_EPTYPE_SHIFT    3
#define XHCI_EP_DW1_EPTYPE_MASK     (0x7U << XHCI_EP_DW1_EPTYPE_SHIFT)
#define XHCI_EP_DW1_MAXBURST_SHIFT  8
#define XHCI_EP_DW1_MAXBURST_MASK   (0xFFU << XHCI_EP_DW1_MAXBURST_SHIFT)
#define XHCI_EP_DW1_MAXPKT_SHIFT    16
#define XHCI_EP_DW1_MAXPKT_MASK     (0xFFFFU << XHCI_EP_DW1_MAXPKT_SHIFT)

/* EP Type values (§6.2.3 Table 6-9). */
#define XHCI_EP_TYPE_ISOC_OUT       1U
#define XHCI_EP_TYPE_BULK_OUT       2U
#define XHCI_EP_TYPE_INT_OUT        3U
#define XHCI_EP_TYPE_CONTROL        4U
#define XHCI_EP_TYPE_ISOC_IN        5U
#define XHCI_EP_TYPE_BULK_IN        6U
#define XHCI_EP_TYPE_INT_IN         7U

/* DW2: low 32 of TR Dequeue Pointer | DCS (bit 0).
 * DW3: high 32 of TR Dequeue Pointer. */

/* DW4 */
#define XHCI_EP_DW4_AVG_TRB_LEN_MASK  0xFFFFU

/* -------------------------------------------------------------------------- */
/* Input Control Context (§6.2.5.1) — first 32 bytes                          */
/* -------------------------------------------------------------------------- */

/* DW0 = Drop Context Flags (bits 31:2 = D31..D2; D0/D1 reserved-0). */
/* DW1 = Add Context Flags  (bits 31:0 = A31..A0). A0=Slot, A1=EP0. */
#define XHCI_INPUT_ADD_SLOT         (1U << 0)
#define XHCI_INPUT_ADD_EP(dci)      (1U << (dci))    /* dci is 1..31 */

/* -------------------------------------------------------------------------- */
/* Field accessors — treat the context buffer as a raw u32 array               */
/* -------------------------------------------------------------------------- */

/*
 * The HC DMAs both Device and Input Contexts as little-endian. On
 * ARM64 we never byte-swap. Callers read / write via these inline
 * helpers which take a byte-addressable pointer plus a DWORD index.
 */
static inline uint32_t *xhci_ctx_dwords(void *ctx_bytes)
{
    return (uint32_t *)ctx_bytes;
}

/* Pointer to the Slot Context DW[i] inside a Device Context buffer. */
static inline uint32_t *xhci_dev_slot_dw(void *dev_ctx, unsigned i)
{
    return &xhci_ctx_dwords(dev_ctx)[i];
}

/* Pointer to EP[dci] DW[i] inside a Device Context buffer. dci is 1..31. */
static inline uint32_t *xhci_dev_ep_dw(void *dev_ctx, unsigned dci,
                                       unsigned i, bool ctx_64)
{
    uint8_t *base = (uint8_t *)dev_ctx + xhci_ctx_dev_offset(dci, ctx_64);
    return &xhci_ctx_dwords(base)[i];
}

/* Pointer to the Input Control Context DW[i]. */
static inline uint32_t *xhci_in_control_dw(void *in_ctx, unsigned i)
{
    return &xhci_ctx_dwords(in_ctx)[i];
}

/* Pointer to the Slot Context DW[i] inside an Input Context buffer. */
static inline uint32_t *xhci_in_slot_dw(void *in_ctx, unsigned i, bool ctx_64)
{
    uint8_t *base = (uint8_t *)in_ctx + xhci_ctx_stride(ctx_64);
    return &xhci_ctx_dwords(base)[i];
}

/* Pointer to EP[dci] DW[i] inside an Input Context buffer. dci is 1..31. */
static inline uint32_t *xhci_in_ep_dw(void *in_ctx, unsigned dci,
                                      unsigned i, bool ctx_64)
{
    uint8_t *base = (uint8_t *)in_ctx + xhci_ctx_in_offset(dci, ctx_64);
    return &xhci_ctx_dwords(base)[i];
}

#endif /* XHCI_CTX_H */
