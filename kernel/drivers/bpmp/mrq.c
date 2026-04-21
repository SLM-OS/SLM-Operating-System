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
#include "timer.h"      /* timer_busy_wait_us — truthful polling cadence */
#include "spinlock.h"   /* serialize concurrent MRQ callers */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/*
 * Serialises concurrent callers of mrq_send. A single MRQ round-trip
 * is commit → ring → poll-until-response → consume; the shared
 * g_tx/g_rx channel state can't tolerate a second commit partway
 * through. This lock is held for the full round-trip, which is OK
 * because the whole path is bounded by MRQ_POLL_TIMEOUT_US (100 ms)
 * and no sleep/alloc happens inside it.
 */
static spinlock_t g_mrq_lock = SPINLOCK_INIT;

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

    /* Serialise the whole request/response round-trip. Concurrent
     * callers (e.g. driver-owned clock gates from different CPUs)
     * would otherwise corrupt the shared g_tx/g_rx state. */
    irq_flags_t flags_irq = spin_lock_irqsave(&g_mrq_lock);

    /* Frame must start from an empty TX side. Normally the previous
     * round-trip cleaned up; if not, something is wedged. */
    if (!ivc_tx_is_empty(&g_tx)) {
        WARN("BPMP: TX busy at start of send (mrq=%u)", (unsigned)mrq);
        spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
        return -2;
    }

    /* Flags: request an ACK from BPMP (protocol-level, not the IVC
     * counter ACK) and ring the doorbell on our behalf if BPMP supports
     * it. Both flags are safe to set universally per linux-bpmp.c. */
    uint32_t flags = IVC_FLAG_DO_ACK | IVC_FLAG_RING_DB;

    int rc = ivc_tx_commit(&g_tx, mrq, flags, tx_data, tx_len);
    if (rc != 0) {
        WARN("BPMP: ivc_tx_commit failed rc=%d for mrq=%u", rc, (unsigned)mrq);
        spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
        return -3;
    }

    /* Kick BPMP. */
    rc = hsp_ring_bpmp();
    if (rc != 0) {
        WARN("BPMP: doorbell ring failed rc=%d (enable not set?)", rc);
        /* Not strictly fatal — BPMP may still poll the channel — but
         * treat as an error since the legacy driver silently ignored. */
        spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
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

        timer_busy_wait_us(1);
        remaining--;
    }

    if (remaining == 0) {
        WARN("BPMP: timeout waiting for response to mrq=%u (TX=%u RX=%u)",
             (unsigned)mrq,
             (unsigned)ivc_peek_tx_count(&g_tx),
             (unsigned)ivc_peek_tx_count(&g_rx));
        spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
        return -4;
    }

    /*
     * Wire layout on response. Linux's struct mb_data (one struct for
     * both directions):
     *   offset 0x80 (IVC frame MRQ)   int32_t code   — on response, this
     *                                                  is the BPMP err
     *                                                  (0 = OK, <0 = errno)
     *   offset 0x84 (IVC frame FLAGS) uint32_t flags
     *   offset 0x88 (IVC frame DATA)  MRQ-specific payload
     *
     * ivc_rx_consume writes the code field into *out_mrq and copies the
     * DATA area into our frame[] buffer. So frame[0..] is the reply
     * payload directly, with no extra header to skip.
     */
    uint8_t frame[IVC_DATA_MAX];
    size_t got = 0;
    uint32_t resp_code = 0;
    rc = ivc_rx_consume(&g_rx, &resp_code, frame, sizeof(frame), &got);
    if (rc != 0) {
        WARN("BPMP: ivc_rx_consume failed rc=%d", rc);
        spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
        return -5;
    }

    if (err_out) {
        *err_out = (int32_t)resp_code;
    }

    if (rx_data && rx_len > 0) {
        size_t n = (got < rx_len) ? got : rx_len;
        uint8_t *dst = (uint8_t *)rx_data;
        for (size_t i = 0; i < n; i++) {
            dst[i] = frame[i];
        }
        /* Zero-fill the rest for callers that don't check got. */
        for (size_t i = n; i < rx_len; i++) {
            dst[i] = 0;
        }
    }

    spin_unlock_irqrestore(&g_mrq_lock, flags_irq);
    return 0;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
