/*
 * kernel/drivers/bpmp/mrq.c - MRQ round-trip transport
 *
 * Formats an mrq_request + payload into the TX IVC frame, rings the BPMP
 * doorbell, polls the RX channel until BPMP writes a response, then
 * copies the payload out and returns the mrq_response.err code from
 * the wire.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "mrq.h"
#include "ivc.h"
#include "hsp.h"
#include "debug.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Response header format (docs/reference/linux-bpmp-abi.h:492).
 * `.err` is int32 at offset 0 of the frame payload, followed by a
 * flags word (4 B), then the response-specific payload. */
#define MRQ_RESP_ERR_OFFSET     0x00
#define MRQ_RESP_HDR_BYTES      8

/* How long to wait for a response in microseconds. 100 ms is generous
 * for simple MRQs (MRQ_PING round-trips in single-digit microseconds
 * on a live BPMP) but required for MRQs that do hardware work like
 * clock enable. */
#define MRQ_POLL_TIMEOUT_US     100000

/* How long to wait for the handshake — can be longer post-kexec when
 * BPMP is mid-transaction with Linux. */
#define MRQ_HANDSHAKE_TIMEOUT_US  500000

static bool              g_mrq_ready;
static struct ivc_channel g_tx;
static struct ivc_channel g_rx;

/* Callback thunks so ivc_handshake doesn't need an HSP dependency. */
static int  ring_bpmp_doorbell(void)  { return hsp_ring_bpmp(); }
static bool bpmp_has_notified(void)   { return hsp_bpmp_has_notified_ccplex(); }
static void ack_bpmp_notification(void) { hsp_ack_bpmp_notification(); }

int mrq_init(uintptr_t tx_sram, uintptr_t rx_sram)
{
    if (tx_sram == 0 || rx_sram == 0) {
        return -1;
    }

    g_tx.base = (volatile uint8_t *)tx_sram;
    g_rx.base = (volatile uint8_t *)rx_sram;

    INFO("BPMP/IVC: TX@0x%lx RX@0x%lx (pre-handshake)",
         (unsigned long)tx_sram, (unsigned long)rx_sram);
    INFO("BPMP/IVC: initial TX state=0x%08x count=%u (rx-count observed by us=%u)",
         (unsigned)ivc_peek_tx_state(&g_tx),
         (unsigned)ivc_peek_tx_count(&g_tx),
         (unsigned)ivc_peek_rx_count(&g_tx));
    INFO("BPMP/IVC: initial RX state=0x%08x count=%u (rx-count observed by us=%u)",
         (unsigned)ivc_peek_tx_state(&g_rx),
         (unsigned)ivc_peek_tx_count(&g_rx),
         (unsigned)ivc_peek_rx_count(&g_rx));

    int rc = ivc_handshake(&g_tx, &g_rx,
                           ring_bpmp_doorbell,
                           bpmp_has_notified,
                           ack_bpmp_notification,
                           MRQ_HANDSHAKE_TIMEOUT_US);
    if (rc != 0) {
        WARN("BPMP/IVC: handshake failed rc=%d", rc);
        return rc;
    }

    g_mrq_ready = true;
    return 0;
}

bool mrq_is_ready(void) { return g_mrq_ready; }

int mrq_send(uint32_t mrq,
             const void *tx_data, uint32_t tx_len,
             void *rx_data, uint32_t rx_len,
             int32_t *err_out)
{
    if (!g_mrq_ready) {
        return -1;
    }

    /* Frame must start from an empty TX side. Normally the previous
     * round-trip cleaned up; if not, something is wedged. */
    if (!ivc_tx_is_empty(&g_tx)) {
        WARN("BPMP: TX busy at start of send (mrq=%u)", (unsigned)mrq);
        return -2;
    }

    /* Flags: request an ACK from BPMP (protocol-level, not the IVC
     * counter ACK) and ring the doorbell on our behalf if BPMP supports
     * it. Both flags are safe to set universally per linux-bpmp.c. */
    uint32_t flags = IVC_FLAG_DO_ACK | IVC_FLAG_RING_DB;

    int rc = ivc_tx_commit(&g_tx, mrq, flags, tx_data, tx_len);
    if (rc != 0) {
        WARN("BPMP: ivc_tx_commit failed rc=%d for mrq=%u", rc, (unsigned)mrq);
        return -3;
    }

    /* Kick BPMP. */
    rc = hsp_ring_bpmp();
    if (rc != 0) {
        WARN("BPMP: doorbell ring failed rc=%d (enable not set?)", rc);
        /* Not strictly fatal — BPMP may still poll the channel — but
         * treat as an error since the legacy driver silently ignored. */
        return -3;
    }

    /* Poll for response. BPMP will write to RX frame, bump RX.tx_count,
     * and (if we get interrupts wired later) ring the CCPLEX doorbell. */
    uint32_t remaining = MRQ_POLL_TIMEOUT_US;
    while (remaining > 0) {
        if (hsp_bpmp_has_notified_ccplex()) {
            hsp_ack_bpmp_notification();
        }

        if (ivc_rx_has_frame(&g_rx)) {
            break;
        }

        for (volatile uint32_t i = 0; i < 100; i++) { }
        remaining--;
    }

    if (remaining == 0) {
        WARN("BPMP: timeout waiting for response to mrq=%u (TX=%u RX=%u)",
             (unsigned)mrq,
             (unsigned)ivc_peek_tx_count(&g_tx),
             (unsigned)ivc_peek_tx_count(&g_rx));
        return -4;
    }

    /* Response frame: layout mirrors mrq_response struct.
     *   offset 0  int32  err
     *   offset 4  uint32 flags
     *   offset 8+ payload (MRQ-specific)
     *
     * We treat the per-MRQ payload as starting at offset 8 within the
     * frame data area. Callers pass rx_data buffers sized for the
     * expected response payload (e.g. 4 bytes for MRQ_CLK responses). */
    uint8_t frame[128];
    size_t got = 0;
    uint32_t resp_mrq = 0;
    rc = ivc_rx_consume(&g_rx, &resp_mrq, frame, sizeof(frame), &got);
    if (rc != 0) {
        WARN("BPMP: ivc_rx_consume failed rc=%d", rc);
        return -5;
    }

    int32_t wire_err = 0;
    if (got >= sizeof(int32_t)) {
        wire_err = (int32_t)((uint32_t)frame[0] |
                             ((uint32_t)frame[1] << 8) |
                             ((uint32_t)frame[2] << 16) |
                             ((uint32_t)frame[3] << 24));
    }
    if (err_out) {
        *err_out = wire_err;
    }

    if (rx_data && rx_len > 0) {
        /* Skip mrq_response header; copy the payload. */
        size_t avail = (got > MRQ_RESP_HDR_BYTES) ? (got - MRQ_RESP_HDR_BYTES) : 0;
        size_t n = (avail < rx_len) ? avail : rx_len;
        const uint8_t *src = frame + MRQ_RESP_HDR_BYTES;
        uint8_t *dst = (uint8_t *)rx_data;
        for (size_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
        /* Zero-fill the rest for callers that don't check got. */
        for (size_t i = n; i < rx_len; i++) {
            dst[i] = 0;
        }
    }

    return 0;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
