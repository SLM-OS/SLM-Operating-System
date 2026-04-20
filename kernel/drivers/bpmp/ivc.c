/*
 * kernel/drivers/bpmp/ivc.c - IVC channel protocol on Tegra234 SYSRAM
 *
 * Runs the Sync → Ack → Established handshake that the old
 * kernel/drivers/bpmp.c skipped. That older code treated offset 0x04
 * of each IVC region as `r_count` and overwrote it during bpmp_init;
 * in the real protocol 0x04 is tx.state, and rewriting it without the
 * handshake ping-pong causes BPMP firmware to reject every subsequent
 * MRQ (symptom: TF-A RAS Uncorrectable Error, rc=-1 on all send_mrq
 * calls — #190).
 *
 * SYSRAM is mapped Normal Non-Cacheable. All MMIO-style accesses go
 * through the volatile pointer in struct ivc_channel. Barriers order
 * writes with respect to the HSP doorbell write that wakes BPMP's R5.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "ivc.h"
#include "debug.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint32_t ivc_read32(const struct ivc_channel *c, uint32_t off)
{
    return *(volatile uint32_t *)(c->base + off);
}

static inline void ivc_write32(struct ivc_channel *c, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(c->base + off) = val;
}

/* Memory barriers are needed between shared-memory writes and the doorbell
 * write that notifies BPMP. NC memory removes the cache coherence problem
 * but not the ordering problem. */
static inline void ivc_dmb(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline void ivc_dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t ivc_peek_tx_count(const struct ivc_channel *c)
{
    return ivc_read32(c, IVC_OFFSET_TX_COUNT);
}

uint32_t ivc_peek_tx_state(const struct ivc_channel *c)
{
    return ivc_read32(c, IVC_OFFSET_TX_STATE);
}

uint32_t ivc_peek_rx_count(const struct ivc_channel *c)
{
    return ivc_read32(c, IVC_OFFSET_RX_COUNT);
}

bool ivc_tx_is_empty(const struct ivc_channel *tx)
{
    uint32_t tx_count = ivc_read32(tx, IVC_OFFSET_TX_COUNT);
    uint32_t rx_count = ivc_read32(tx, IVC_OFFSET_RX_COUNT);
    return tx_count == rx_count;
}

bool ivc_rx_has_frame(const struct ivc_channel *rx)
{
    uint32_t tx_count = ivc_read32(rx, IVC_OFFSET_TX_COUNT);
    uint32_t rx_count = ivc_read32(rx, IVC_OFFSET_RX_COUNT);
    return tx_count != rx_count;
}

/*
 * Handshake protocol (condensed from linux-tegra-ivc.c:tegra_ivc_notified):
 *
 *   On each incoming doorbell, the receiver compares (my tx.state, peer's
 *   tx.state) and takes one transition per call. Both ends follow the same
 *   state machine — it's symmetric.
 *
 *   (us=Sync, them=Established)   BPMP sees us as Sync while they were
 *                                 Established. BPMP clears its own
 *                                 rx.count on the RX channel, writes
 *                                 its tx.state = Ack, rings us back.
 *   (us=Sync, them=Ack)           We observe peer's Ack. Zero our tx.count,
 *                                 write tx.state = Established, ring.
 *   (us=Sync, them=Sync)          Both reset simultaneously. Zero our
 *                                 tx.count, write tx.state = Ack, ring.
 *   (us=Ack, them=Established)    Peer has finished its reset. Write our
 *                                 tx.state = Established (no doorbell — peer
 *                                 is ready).
 *
 * For a post-kexec resync from SLM-OS:
 *
 *   1. Write our tx.count = 0 and tx.state = Sync. Ring BPMP.
 *   2. Wait for peer (BPMP) tx.state to become Ack — BPMP has seen our
 *      Sync, cleared its own rx.count on the RX channel, and signalled
 *      that it has done so. The "RX channel" we read for the peer's
 *      state is the BPMP→CPU IVC region, where BPMP's tx.state lives
 *      at IVC_OFFSET_TX_STATE.
 *   3. Write our own rx.count = 0 on the RX channel (our ack counter
 *      for BPMP's outbound frames — we own it). Write tx.state =
 *      Established on TX. Ring BPMP.
 *   4. BPMP completes its ack transition to Established. Not strictly
 *      required to observe — the protocol permits us to start sending
 *      frames once we're in Established locally.
 *
 * Importantly: "peer Established" is the initial starting condition post-
 * kexec (BPMP kept running and its state is whatever Linux last observed).
 * We must not treat that as a completed handshake — it means BPMP hasn't
 * reacted to us at all. The discriminating signal is "peer transitioned
 * to Ack", which only happens after BPMP processes our Sync.
 */
int ivc_handshake(struct ivc_channel *tx, struct ivc_channel *rx,
                  int (*ring_doorbell)(void),
                  bool (*doorbell_pending)(void),
                  void (*ack_doorbell)(void),
                  uint32_t timeout_us)
{
    if (tx == NULL || rx == NULL || ring_doorbell == NULL) {
        return -1;
    }

    /* Step 1 — signal reset intent. Only we write tx.count and tx.state
     * on the TX channel; BPMP owns rx.count there. */
    ivc_write32(tx, IVC_OFFSET_TX_COUNT, 0);
    ivc_write32(tx, IVC_OFFSET_TX_STATE, IVC_STATE_SYNC);
    ivc_dsb();

    int rc = ring_doorbell();
    if (rc != 0) {
        WARN("IVC: initial doorbell ring failed rc=%d", rc);
        return rc;
    }

    /* Step 2 — wait for peer to acknowledge our Sync by going to Ack.
     * Starting condition is usually peer=Established (from Linux era).
     * We are looking for the transition Established → Ack, which only
     * happens after BPMP processes our doorbell. */
    uint32_t remaining = timeout_us;
    uint32_t peer_state;
    bool rang_second = false;

    while (remaining > 0) {
        if (doorbell_pending && doorbell_pending()) {
            ack_doorbell();
        }

        peer_state = ivc_read32(rx, IVC_OFFSET_TX_STATE);
        if (peer_state == IVC_STATE_ACK) {
            break;
        }

        /* Handle the symmetric-reset edge case: if BPMP also went to Sync
         * (it shouldn't post-kexec but the protocol allows it), we need
         * to transition to Ack ourselves. */
        if (peer_state == IVC_STATE_SYNC && !rang_second) {
            /* Our response to (us=Sync, them=Sync): go to Ack and ring. */
            ivc_write32(tx, IVC_OFFSET_TX_STATE, IVC_STATE_ACK);
            ivc_dsb();
            (void)ring_doorbell();
            rang_second = true;
        }

        for (volatile uint32_t i = 0; i < 100; i++) { }
        remaining--;
    }

    if (remaining == 0) {
        WARN("IVC: peer did not transition to Ack within %u us (peer state=%u)",
             (unsigned)timeout_us, (unsigned)ivc_read32(rx, IVC_OFFSET_TX_STATE));
        return -2;
    }

    /* Step 3 — peer is in Ack. Clear our own ack counter on the RX
     * channel (we own rx.count there). Then transition us to Established
     * and ring BPMP one more time so it can complete its own transition. */
    ivc_write32(rx, IVC_OFFSET_RX_COUNT, 0);
    ivc_write32(tx, IVC_OFFSET_TX_STATE, IVC_STATE_ESTABLISHED);
    ivc_dsb();

    rc = ring_doorbell();
    if (rc != 0) {
        WARN("IVC: final doorbell ring failed rc=%d", rc);
        return rc;
    }

    INFO("IVC: handshake complete (us=Established, peer observed Ack)");
    return 0;
}

int ivc_tx_commit(struct ivc_channel *tx, uint32_t mrq, uint32_t flags,
                  const void *data, size_t len)
{
    if (tx == NULL || len > IVC_DATA_MAX) {
        return -1;
    }

    /* Must be idle before starting a new frame. */
    if (!ivc_tx_is_empty(tx)) {
        return -2;
    }

    /* Verify we're in Established; otherwise the handshake is incomplete
     * and the frame will be silently dropped. */
    if (ivc_read32(tx, IVC_OFFSET_TX_STATE) != IVC_STATE_ESTABLISHED) {
        return -3;
    }

    /* Write the frame's mrq/flags header. */
    ivc_write32(tx, IVC_OFFSET_FRAME_MRQ,   mrq);
    ivc_write32(tx, IVC_OFFSET_FRAME_FLAGS, flags);

    /* Copy payload byte-wise for simplicity. The BPMP side uses natural
     * alignment; we could tighten to 64-bit MMIO writes like edk2-nvidia
     * does but for a ~20-byte MRQ_CLK payload the overhead is negligible
     * and byte writes are safer for struct offsets that aren't 64-aligned. */
    const uint8_t *src = (const uint8_t *)data;
    volatile uint8_t *dst = tx->base + IVC_OFFSET_FRAME_DATA;
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }

    /* Ordering barrier before the counter increment. Readers check
     * tx.count to decide whether to consume the frame, so the frame
     * body must hit memory first. */
    ivc_dmb();
    uint32_t new_count = ivc_read32(tx, IVC_OFFSET_TX_COUNT) + 1;
    ivc_write32(tx, IVC_OFFSET_TX_COUNT, new_count);
    ivc_dsb();

    return 0;
}

int ivc_rx_consume(struct ivc_channel *rx, uint32_t *out_mrq,
                   void *out_data, size_t max_len, size_t *got_len)
{
    if (rx == NULL) {
        return -1;
    }

    if (!ivc_rx_has_frame(rx)) {
        return -2;
    }

    /* Invalidate the implicit read-cache: we need BPMP's latest write
     * to the frame. NC memory makes this a no-op for caches, but DSB
     * ensures we don't use speculatively-reordered loads. */
    ivc_dsb();

    if (out_mrq) {
        *out_mrq = ivc_read32(rx, IVC_OFFSET_FRAME_MRQ);
    }

    size_t copy_len = max_len;
    if (copy_len > IVC_DATA_MAX) copy_len = IVC_DATA_MAX;

    uint8_t *dst = (uint8_t *)out_data;
    const volatile uint8_t *src = rx->base + IVC_OFFSET_FRAME_DATA;
    if (dst) {
        for (size_t i = 0; i < copy_len; i++) {
            dst[i] = src[i];
        }
    }

    if (got_len) {
        *got_len = copy_len;
    }

    /* Acknowledge consumption by advancing rx.count. */
    ivc_dmb();
    uint32_t new_count = ivc_read32(rx, IVC_OFFSET_RX_COUNT) + 1;
    ivc_write32(rx, IVC_OFFSET_RX_COUNT, new_count);
    ivc_dsb();

    return 0;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
