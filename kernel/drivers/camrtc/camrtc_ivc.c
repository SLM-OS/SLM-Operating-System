/*
 * camrtc_ivc.c — Tegra-IVC ring transport for the RCE channels
 * `camrtc_ch_setup_*` binds. Header doc in `kernel/include/camrtc_ivc.h`
 * carries the wire-format reference.
 */

#include "camrtc_ivc.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "camrtc.h"
#include "debug.h"
#include "platform.h"
#include "timer.h"

/* ---- Queue header layout (matches docs/reference/linux-tegra-ivc.c) ---- */

/* TX half (writer-owned): count + state + 56 bytes of pad. */
#define IVC_HDR_TX_COUNT_OFF      0u
#define IVC_HDR_TX_STATE_OFF      4u
/* RX half (reader-owned): count + 60 bytes of pad. */
#define IVC_HDR_RX_COUNT_OFF      64u
/* Frames start immediately after the 128-byte header. */
#define IVC_HDR_SIZE              128u

/* Per-direction state (writer-side). 0 == ESTABLISHED matches the
 * zero-init convention SLM-OS uses to skip the SYNC handshake. */
#define IVC_STATE_ESTABLISHED     0u
#define IVC_STATE_SYNC            1u
#define IVC_STATE_ACK             2u

/* SS[0] bit layout for VM↔FW notifications (camrtc_commands.h):
 *   bits 31..16 = VM→FW (AP sets to wake RCE)
 *   bits 15..0  = FW→VM (RCE sets to wake AP)
 *   IVC group occupies the low 8 bits of each half. */
#define CAMRTC_HSP_SS_VM_SHIFT    16u
#define CAMRTC_HSP_SS_IVC_MASK    0xFFu

/* SS register window for the camera-RTCPU HSP block. SS[0] base lives
 * at HSP_BASE + 0x10000 (common region) + num_sm * 0x8000 (8 SMs per
 * `rcediag` DIMENSIONING dump = 0x40000) = HSP_BASE + 0x50000.
 * Per-SS register offsets (from docs/reference/linux-tegra-hsp.c):
 *   +0x00 SHRD_SEM_STATUS
 *   +0x04 SHRD_SEM_SET (write-1-to-set)
 *   +0x08 SHRD_SEM_CLR (write-1-to-clear) */
#define HSP_COMMON_REGION_SIZE    0x10000u
#define HSP_SM_SIZE               0x8000u
#define HSP_NUM_SM_RCE            8u
#define HSP_SS_SIZE               0x10000u
#define HSP_SS0_BASE \
    (TEGRA234_RCE_HSP_BASE + HSP_COMMON_REGION_SIZE \
     + HSP_NUM_SM_RCE * HSP_SM_SIZE)
#define HSP_SS_SHRD_SEM_SET       0x4u

/* ---- MMIO accessors (mirror camrtc.c) ---- */

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void mmio_write32(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- Header field accessors ----
 *
 * The headers live in cacheable kernel-linear memory shared with RCE.
 * RCE is dma-coherent per L4T DT, so a `dsb sy` after a write or
 * before a read is sufficient to bridge the AP↔RCE visibility gap. */

static inline uint32_t hdr_load_u32(uintptr_t hdr, uint32_t off)
{
    /* The IVC region lives in the NC mapping (see
     * `CAMRTC_CTRL_REGION_PHYS` in camrtc.c) so reads bypass cache
     * and hit DRAM directly. A DSB before the read is the only
     * ordering needed — no DC CIVAC required. */
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint32_t *)(hdr + off);
}

static inline void hdr_store_u32(uintptr_t hdr, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(hdr + off) = v;
    /* NC mapping → write reaches DRAM immediately. DSB ensures
     * the write is observable before any subsequent operation
     * (e.g. notify_rce). No DC CVAC required. */
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- Frame slot pointer ----
 *
 * The `which`-th slot in a queue starts at hdr + IVC_HDR_SIZE +
 * which * frame_size. `which` is `count % nframes` for the next
 * unread/unwritten slot. */
static inline uintptr_t slot_addr(uintptr_t queue, uint32_t which,
                                  uint32_t frame_size)
{
    return queue + IVC_HDR_SIZE + which * frame_size;
}

/* ---- byte memcpy (kernel has no <string.h>) ---- */

/* Copy `n` bytes src→dst. Uses 8-byte stores when both pointers and
 * `n` are 8-aligned (the common case for IVC frame copies — slot
 * base is 64-aligned, frame_size is 64-aligned), and falls back to
 * single-byte stores for any unaligned head/tail. NC mapping makes
 * either path land in DRAM directly; the word-at-a-time path is
 * just ~8× fewer store instructions. */
static void byte_copy(volatile uint8_t *dst, const uint8_t *src,
                      uint32_t n)
{
    uintptr_t da = (uintptr_t)dst;
    uintptr_t sa = (uintptr_t)src;
    if (((da | sa | n) & 7u) == 0u) {
        volatile uint64_t *d = (volatile uint64_t *)dst;
        const uint64_t *s = (const uint64_t *)src;
        uint32_t words = n / 8u;
        for (uint32_t i = 0; i < words; i++) d[i] = s[i];
        return;
    }
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

static void byte_zero(volatile uint8_t *dst, uint32_t n)
{
    if ((((uintptr_t)dst | n) & 7u) == 0u) {
        volatile uint64_t *d = (volatile uint64_t *)dst;
        uint32_t words = n / 8u;
        for (uint32_t i = 0; i < words; i++) d[i] = 0;
        return;
    }
    for (uint32_t i = 0; i < n; i++) dst[i] = 0;
}

/* ---- Notification: wake RCE for `group`. ----
 *
 * Mirrors `camrtc_hsp_vm_group_ring` in `docs/reference/
 * l4t-rtcpu-hsp-combo.c:252`: write the group bit shifted into the
 * VM→FW half of SS[0], then send a CAMRTC_HSP_IRQ mailbox message
 * (which `camrtc_send_msg` would otherwise drain as unidirectional).
 *
 * The IRQ-msg path is best-effort — on Jetson today the SS write
 * alone is enough to drive RCE's ISR — but we keep both kicks to
 * mirror L4T's exact wire behaviour. */
static void notify_rce(uint32_t group)
{
    /* SS_SET write. Per the L4T encoding the group value (NOT a
     * bitmask) is masked to IVC_MASK and shifted into the VM half. */
    uint32_t bits = (group & CAMRTC_HSP_SS_IVC_MASK) << CAMRTC_HSP_SS_VM_SHIFT;
    mmio_write32(HSP_SS0_BASE + HSP_SS_SHRD_SEM_SET, bits);

    /* IRQ mailbox kick. Fire-and-forget; param=1 is what L4T uses
     * (`docs/reference/l4t-rtcpu-hsp-combo.c:268`). The kick wakes
     * RCE's mailbox-FULL ISR which then reads SS[0] to learn which
     * IVC group has new traffic — without this the SS_SET write
     * sits unread until RCE's next unrelated wake. */
    (void)camrtc_send_irq(CAMRTC_HSP_IRQ, 1u, 1000u);
}

/* ---- Public API ---- */

int camrtc_ivc_init(struct camrtc_ivc_channel *ch,
                    uintptr_t rx_iova, uintptr_t tx_iova,
                    uint32_t nframes, uint32_t frame_size,
                    uint32_t group)
{
    if (ch == (struct camrtc_ivc_channel *)0
        || rx_iova == 0 || tx_iova == 0
        || nframes == 0 || (nframes & (nframes - 1)) != 0
        || frame_size == 0 || (frame_size & 63u) != 0) {
        WARN("camrtc_ivc: bad init args (nframes=%u frame_size=%u)",
             (unsigned)nframes, (unsigned)frame_size);
        return -1;
    }

    ch->rx_queue   = rx_iova;
    ch->tx_queue   = tx_iova;
    ch->nframes    = nframes;
    ch->frame_size = frame_size;
    ch->group      = group;
    ch->initialised = false;

    /* Zero our half of each queue header. Don't touch RCE's halves
     * — RCE may have written its own tx_state during CH_SETUP and
     * we need to observe it for the SYNC handshake. AP owns
     * tx_iova[0..63] (TX half of AP→RCE) and rx_iova[64..127]
     * (RX half of RCE→AP). */
    byte_zero((volatile uint8_t *)tx_iova, IVC_HDR_SIZE / 2u);
    byte_zero((volatile uint8_t *)(rx_iova + IVC_HDR_SIZE / 2u),
              IVC_HDR_SIZE / 2u);

    /* Drive the SYNC→ACK→EST handshake matching `linux-tegra-ivc.c`
     * `tegra_ivc_reset` + `tegra_ivc_notified`. The earlier all-EST
     * zero-init left RCE silently ignoring frames; an aggressive
     * notify-every-iteration variant tripped RCE's heartbeat
     * watchdog. The shape below mirrors L4T:
     *   - kickoff: write local=SYNC + notify (single shot)
     *   - poll: read both states; if changed since last iter,
     *           apply the transition table ONCE and notify;
     *           sleep 1 ms between polls so RCE's mailbox ISR
     *           doesn't drown.
     */
    hdr_store_u32(tx_iova, IVC_HDR_TX_STATE_OFF, IVC_STATE_SYNC);
    notify_rce(group);

    uint64_t freq = timer_get_frequency();
    uint64_t ticks_per_us = freq / 1000000u;
    if (ticks_per_us == 0) ticks_per_us = 1u;
    uint64_t deadline = timer_get_count()
                      + (uint64_t)100000u * ticks_per_us;

    uint32_t prev_local  = 0xFFu;
    uint32_t prev_remote = 0xFFu;

    for (;;) {
        if (timer_get_count() >= deadline) {
            uint32_t l = hdr_load_u32(tx_iova, IVC_HDR_TX_STATE_OFF);
            uint32_t r = hdr_load_u32(rx_iova, IVC_HDR_TX_STATE_OFF);
            WARN("camrtc_ivc: handshake timeout (local=%u, remote=%u)",
                 (unsigned)l, (unsigned)r);
            return -2;
        }

        uint32_t local  = hdr_load_u32(tx_iova, IVC_HDR_TX_STATE_OFF);
        uint32_t remote = hdr_load_u32(rx_iova, IVC_HDR_TX_STATE_OFF);

        if (local == IVC_STATE_ESTABLISHED
            && remote == IVC_STATE_ESTABLISHED) {
            INFO("camrtc_ivc: handshake EST/EST after kickoff");
            break;
        }

        /* Only act if state has changed since last iteration —
         * otherwise we'd re-notify RCE without anything to ack. */
        if (local == prev_local && remote == prev_remote) {
            timer_busy_wait_us(1000u);
            continue;
        }
        prev_local = local;
        prev_remote = remote;

        /* L4T `tegra_ivc_notified` transition table (subset):
         *   remote==SYNC                  → reset counters; local=ACK
         *   local==SYNC && remote==ACK    → reset counters; local=EST
         *   local==ACK                    → local=EST  (no counter reset)
         *   else                          → no-op (wait)
         */
        if (remote == IVC_STATE_SYNC) {
            hdr_store_u32(tx_iova, IVC_HDR_TX_COUNT_OFF, 0u);
            hdr_store_u32(rx_iova, IVC_HDR_RX_COUNT_OFF, 0u);
            hdr_store_u32(tx_iova, IVC_HDR_TX_STATE_OFF, IVC_STATE_ACK);
            notify_rce(group);
        } else if (local == IVC_STATE_SYNC
                   && remote == IVC_STATE_ACK) {
            hdr_store_u32(tx_iova, IVC_HDR_TX_COUNT_OFF, 0u);
            hdr_store_u32(rx_iova, IVC_HDR_RX_COUNT_OFF, 0u);
            hdr_store_u32(tx_iova, IVC_HDR_TX_STATE_OFF,
                          IVC_STATE_ESTABLISHED);
            notify_rce(group);
        } else if (local == IVC_STATE_ACK) {
            hdr_store_u32(tx_iova, IVC_HDR_TX_STATE_OFF,
                          IVC_STATE_ESTABLISHED);
            notify_rce(group);
        }
        /* else: waiting for remote to advance; just keep polling. */

        timer_busy_wait_us(1000u);
    }

    ch->initialised = true;
    INFO("camrtc_ivc: channel up (group=%u, rx=0x%lx, tx=0x%lx, "
         "nframes=%u, frame_size=%u)",
         (unsigned)group, (unsigned long)rx_iova,
         (unsigned long)tx_iova,
         (unsigned)nframes, (unsigned)frame_size);
    return 0;
}

bool camrtc_ivc_can_send(struct camrtc_ivc_channel *ch)
{
    if (ch == (struct camrtc_ivc_channel *)0 || !ch->initialised) {
        return false;
    }
    /* Writer's count - reader's count >= nframes ⇒ full. The reader
     * counter for our TX queue lives in the queue's RX half (RCE
     * owns it). hdr_load_u32 issues DSB so we observe RCE's latest
     * advance. */
    uint32_t writer = hdr_load_u32(ch->tx_queue, IVC_HDR_TX_COUNT_OFF);
    uint32_t reader = hdr_load_u32(ch->tx_queue, IVC_HDR_RX_COUNT_OFF);
    return (writer - reader) < ch->nframes;
}

bool camrtc_ivc_can_recv(struct camrtc_ivc_channel *ch)
{
    if (ch == (struct camrtc_ivc_channel *)0 || !ch->initialised) {
        return false;
    }
    /* Writer's count != reader's count ⇒ at least one frame
     * available. For our RX queue, RCE is the writer (TX half) and
     * AP is the reader (RX half). */
    uint32_t writer = hdr_load_u32(ch->rx_queue, IVC_HDR_TX_COUNT_OFF);
    uint32_t reader = hdr_load_u32(ch->rx_queue, IVC_HDR_RX_COUNT_OFF);
    return writer != reader;
}

int camrtc_ivc_send(struct camrtc_ivc_channel *ch,
                    const void *payload, uint32_t len)
{
    if (ch == (struct camrtc_ivc_channel *)0 || !ch->initialised
        || payload == (const void *)0 || len > ch->frame_size) {
        return -1;
    }

    uint32_t writer = hdr_load_u32(ch->tx_queue, IVC_HDR_TX_COUNT_OFF);
    uint32_t reader = hdr_load_u32(ch->tx_queue, IVC_HDR_RX_COUNT_OFF);
    if ((writer - reader) >= ch->nframes) {
        return -2;  /* queue full */
    }

    uint32_t slot = writer & (ch->nframes - 1u);
    uintptr_t dst = slot_addr(ch->tx_queue, slot, ch->frame_size);

    /* Copy payload + zero-pad the rest of the slot so RCE sees a
     * deterministic frame regardless of what was in the slot last
     * time around. */
    byte_copy((volatile uint8_t *)dst,
              (const uint8_t *)payload, len);
    if (len < ch->frame_size) {
        byte_zero((volatile uint8_t *)(dst + len),
                  ch->frame_size - len);
    }

    /* Write barrier: frame data must hit DRAM before the count
     * advance. NC mapping makes the byte writes already-visible;
     * DSB just enforces ordering vs the count store. */
    __asm__ volatile("dsb sy" ::: "memory");
    hdr_store_u32(ch->tx_queue, IVC_HDR_TX_COUNT_OFF, writer + 1u);

    /* Empty→non-empty transition? The L4T notify-only-on-transition
     * pattern saves an SS write when the queue was already non-empty
     * (RCE will drain pending frames in one go). */
    if (writer == reader) {
        notify_rce(ch->group);
    }

    return 0;
}

int camrtc_ivc_recv(struct camrtc_ivc_channel *ch,
                    void *buf, uint32_t buf_size, uint32_t *out_len)
{
    if (ch == (struct camrtc_ivc_channel *)0 || !ch->initialised
        || buf == (void *)0) {
        return -1;
    }

    uint32_t writer = hdr_load_u32(ch->rx_queue, IVC_HDR_TX_COUNT_OFF);
    uint32_t reader = hdr_load_u32(ch->rx_queue, IVC_HDR_RX_COUNT_OFF);
    if (writer == reader) {
        return -2;  /* queue empty */
    }

    uint32_t slot = reader & (ch->nframes - 1u);
    uintptr_t src = slot_addr(ch->rx_queue, slot, ch->frame_size);

    uint32_t copy_n = ch->frame_size;
    if (copy_n > buf_size) copy_n = buf_size;

    /* Read barrier: we must observe RCE's writes to this slot
     * (which it ordered before its count advance) before we touch
     * the slot bytes. NC mapping → writes are in DRAM already;
     * DSB enforces ordering. */
    __asm__ volatile("dsb sy" ::: "memory");
    byte_copy((volatile uint8_t *)buf,
              (const uint8_t *)src, copy_n);

    if (out_len != (uint32_t *)0) *out_len = copy_n;

    /* Acknowledge: bump our read counter, kick RCE if the queue
     * transitioned from full→non-full (so RCE can post the next
     * frame). */
    bool was_full = (writer - reader) >= ch->nframes;
    hdr_store_u32(ch->rx_queue, IVC_HDR_RX_COUNT_OFF, reader + 1u);
    if (was_full) {
        notify_rce(ch->group);
    }

    return 0;
}

int camrtc_ivc_recv_wait(struct camrtc_ivc_channel *ch,
                         void *buf, uint32_t buf_size,
                         uint32_t *out_len, uint32_t timeout_us)
{
    if (ch == (struct camrtc_ivc_channel *)0 || !ch->initialised) {
        return -1;
    }

    uint64_t freq = timer_get_frequency();
    uint64_t ticks_per_us = freq / 1000000u;
    if (ticks_per_us == 0) ticks_per_us = 1u;
    uint64_t deadline = timer_get_count()
                      + (uint64_t)timeout_us * ticks_per_us;

    while (!camrtc_ivc_can_recv(ch)) {
        if (timer_get_count() >= deadline) {
            return -2;
        }
        /* No yield — caller is shell context, single-CPU,
         * sub-second timeout. Tight poll. */
    }
    return camrtc_ivc_recv(ch, buf, buf_size, out_len);
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int camrtc_ivc_init(struct camrtc_ivc_channel *ch,
                    uintptr_t rx_iova, uintptr_t tx_iova,
                    uint32_t nframes, uint32_t frame_size,
                    uint32_t group)
{
    (void)ch; (void)rx_iova; (void)tx_iova;
    (void)nframes; (void)frame_size; (void)group;
    return -1;
}

bool camrtc_ivc_can_send(struct camrtc_ivc_channel *ch)
{
    (void)ch; return false;
}
bool camrtc_ivc_can_recv(struct camrtc_ivc_channel *ch)
{
    (void)ch; return false;
}
int camrtc_ivc_send(struct camrtc_ivc_channel *ch,
                    const void *payload, uint32_t len)
{
    (void)ch; (void)payload; (void)len;
    return -1;
}
int camrtc_ivc_recv(struct camrtc_ivc_channel *ch,
                    void *buf, uint32_t buf_size, uint32_t *out_len)
{
    (void)ch; (void)buf; (void)buf_size;
    if (out_len) *out_len = 0;
    return -1;
}
int camrtc_ivc_recv_wait(struct camrtc_ivc_channel *ch,
                         void *buf, uint32_t buf_size,
                         uint32_t *out_len, uint32_t timeout_us)
{
    (void)ch; (void)buf; (void)buf_size; (void)timeout_us;
    if (out_len) *out_len = 0;
    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
