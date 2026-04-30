/*
 * kernel/drivers/bpmp/ivc.h - Tegra IVC channel protocol (private header)
 *
 * IVC (Inter-VM Communication) is NVIDIA's shared-memory-based message
 * protocol for CCPLEX ↔ BPMP / SCE / SPE / etc. Each direction of the
 * link is a 4 KB SYSRAM region with a fixed header and one 128-byte frame.
 *
 * Header layout (identical for TX and RX regions, 128 bytes total):
 *
 *   offset 0x00  uint32  tx.count   writer-side frame counter
 *   offset 0x04  uint32  tx.state   TEGRA_IVC_STATE_{ESTABLISHED,SYNC,ACK}
 *   offset 0x08  byte[56] pad       rest of writer's 64-byte cacheline
 *   offset 0x40  uint32  rx.count   receiver-side ACK counter
 *   offset 0x44  byte[60] pad       rest of receiver's 64-byte cacheline
 *
 * Frame layout (immediately after header, 128 bytes):
 *
 *   offset 0x80  uint32  mrq        MRQ_* opcode (PING=0, CLK=22, …)
 *   offset 0x84  uint32  flags      BPMP_MAIL_DO_ACK | BPMP_MAIL_RING_DB
 *   offset 0x88  byte[120]          MRQ-specific payload
 *
 * The "owner" of each field alternates: the TX side owns tx.count /
 * tx.state, the RX side owns rx.count. This cacheline isolation is
 * why the BPMP SRAM must be mapped Non-Cacheable in SLM-OS.
 *
 * Tegra234 BPMP channels have num_frames=1 — the protocol is strictly
 * request/response with no pipelining. tx.count - rx.count can only be
 * 0 (idle) or 1 (frame in flight).
 *
 * References:
 *   ../slmos-reference-cache/linux/linux-tegra-ivc.c           (state machine)
 *   ../slmos-reference-cache/tegra-l4t/edk2-nvidia-bpmpipcprivate.h (IVC_CHANNEL layout)
 *   ../slmos-reference-cache/linux/linux-bpmp-abi.h            (mrq_request wire format)
 */

#ifndef DRIVERS_BPMP_IVC_H
#define DRIVERS_BPMP_IVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Field offsets into one direction of the channel.  */
#define IVC_OFFSET_TX_COUNT     0x00
#define IVC_OFFSET_TX_STATE     0x04
#define IVC_OFFSET_RX_COUNT     0x40
#define IVC_OFFSET_FRAME_MRQ    0x80
#define IVC_OFFSET_FRAME_FLAGS  0x84
#define IVC_OFFSET_FRAME_DATA   0x88

/* Total channel size (header + one 128 B frame). */
#define IVC_CHANNEL_SIZE        0x100

/* Maximum payload in one frame (matches Linux MSG_DATA_MIN_SZ). */
#define IVC_DATA_MAX            120
_Static_assert(IVC_DATA_MAX == 120,
               "IVC_DATA_MAX must match Linux MSG_DATA_MIN_SZ "
               "(linux-bpmp-abi.h:536) — BPMP firmware rejects "
               "frames larger than this");

/* Handshake states (see linux-tegra-ivc.c:enum tegra_ivc_state). */
#define IVC_STATE_ESTABLISHED   0
#define IVC_STATE_SYNC          1
#define IVC_STATE_ACK           2

/* mrq_request::flags bits (see linux-bpmp-abi.h). */
#define IVC_FLAG_DO_ACK         (1u << 0)
#define IVC_FLAG_RING_DB        (1u << 1)

/*
 * A single IVC channel, one direction. CPU→BPMP uses one instance;
 * BPMP→CPU uses another. Both point at 4 KB regions in SYSRAM that
 * must already be mapped Normal Non-Cacheable.
 */
struct ivc_channel {
    volatile uint8_t *base;
};

/*
 * Drive the IVC handshake on a TX/RX channel pair.
 *
 * Blocking: sends Sync, waits for BPMP's Ack, transitions to Established.
 * Per linux-tegra-ivc.c the receiver (BPMP) is allowed to zero both
 * sides' counters during this handshake, which is how a post-kexec
 * CCPLEX can reset Linux's stale channel state without corrupting
 * BPMP's internal bookkeeping.
 *
 * Returns 0 on success, negative on timeout/error. Rings the BPMP
 * doorbell between state transitions via the hsp_ring_bpmp() call
 * supplied from mrq.c; ivc itself has no HSP dependency baked in.
 */
int ivc_handshake(struct ivc_channel *tx, struct ivc_channel *rx,
                  int (*ring_doorbell)(void),
                  bool (*doorbell_pending)(void),
                  void (*ack_doorbell)(void),
                  uint32_t timeout_us);

/* True if tx channel can accept a new frame (tx.count == rx.count). */
bool ivc_tx_is_empty(const struct ivc_channel *tx);

/* True if rx channel has a response frame waiting (rx.tx_count != rx.rx_count). */
bool ivc_rx_has_frame(const struct ivc_channel *rx);

/*
 * Build a frame in the TX channel and commit it by incrementing tx.count.
 *   mrq     MRQ opcode.
 *   flags   IVC_FLAG_*.
 *   data    payload bytes (copied into the frame data area).
 *   len     bytes to copy, must be ≤ IVC_DATA_MAX.
 * Returns 0 on success.
 */
int ivc_tx_commit(struct ivc_channel *tx, uint32_t mrq, uint32_t flags,
                  const void *data, size_t len);

/*
 * Consume a pending RX frame:
 *   out_mrq   filled with the response's mrq field (usually the error code).
 *   out_data  copied from the frame's data area.
 *   max_len   size of the out_data buffer.
 *   *got_len  set to `min(max_len, IVC_DATA_MAX)`. NOTE: this is the
 *             number of bytes COPIED into out_data, not the number of
 *             valid bytes in the frame — IVC has no length metadata on
 *             the wire (frames are always a fixed 120 bytes). Callers
 *             should pass rx_len equal to the expected response
 *             payload size for the MRQ they issued.
 * Returns 0 on success. Also increments rx.count so BPMP knows we
 * processed the frame.
 */
int ivc_rx_consume(struct ivc_channel *rx, uint32_t *out_mrq,
                   void *out_data, size_t max_len, size_t *got_len);

/* Raw peeks for diagnostics. */
uint32_t ivc_peek_tx_count(const struct ivc_channel *c);
uint32_t ivc_peek_tx_state(const struct ivc_channel *c);
uint32_t ivc_peek_rx_count(const struct ivc_channel *c);

#endif /* DRIVERS_BPMP_IVC_H */
