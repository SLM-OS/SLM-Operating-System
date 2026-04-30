/*
 * camrtc_ivc.h — Tegra-IVC ring transport over the CH_SETUP-bound
 * RCE channels (capture-control, capture, etc.).
 *
 * Once `camrtc_ch_setup_capture_control()` returns 0, RCE has bound
 * a (rx, tx) ring pair to a (group, service) tuple. This header
 * exposes the bare-minimum API SLM-OS needs to send a request frame
 * and read the response frame for that channel — the foundation for
 * `CAPTURE_PHY_STREAM_OPEN_REQ`, `CAPTURE_CSI_STREAM_SET_CONFIG_REQ`,
 * and the rest of `../slmos-reference-cache/tegra-l4t/l4t-camrtc-capture-messages.h`.
 *
 * Wire-format notes (cross-checked against
 * `../slmos-reference-cache/linux/linux-tegra-ivc.c`):
 *
 *   - Each queue is 128 B header + nframes * frame_size bytes of
 *     contiguous frame slots. The header is split into two 64-B
 *     halves so cache-coherency traffic doesn't false-share between
 *     the writer and the reader:
 *       offset 0..63   "tx half" — owned by the queue's writer.
 *                      Field 0..3:  count   (u32, monotonic, wraps)
 *                      Field 4..7:  state   (TEGRA_IVC_STATE_*)
 *                      Field 8..63: reserved
 *       offset 64..127 "rx half" — owned by the queue's reader.
 *                      Field 64..67: count  (u32, monotonic, wraps)
 *                      Field 68..127: reserved
 *
 *   - For the "rx queue" (RCE→AP), RCE is the writer (owns bytes
 *     0..63) and AP is the reader (owns bytes 64..127). For the
 *     "tx queue" (AP→RCE), AP is the writer and RCE is the reader.
 *
 *   - Slot index for the next frame to write/read is
 *     `(local_count) % nframes`. Queue-empty when writer.count ==
 *     reader.count; queue-full when (writer.count - reader.count)
 *     >= nframes. Counters are u32 so subtraction handles wrap-
 *     around naturally.
 *
 *   - State machine: writer's `state` is one of SYNC (1) / ACK (2) /
 *     ESTABLISHED (0). `camrtc_ivc_init` drives the SYNC→ACK→EST
 *     handshake matching L4T `tegra_ivc_reset` + `tegra_ivc_notified`:
 *     write local=SYNC + notify (kickoff), then poll-and-transition
 *     with 1 ms inter-poll spacing until both sides observe EST.
 *     The spacing is load-bearing — without it, RCE's heartbeat
 *     watchdog trips on IRQ-msg flood.
 *
 *   - Notification: when AP advances a write count and the queue
 *     transitioned from empty→non-empty, AP must wake RCE. The wake
 *     channel is the SS[0] shared semaphore (group bits 31:16,
 *     IVC mask 0xFF), and an additional `CAMRTC_HSP_IRQ` mailbox
 *     message. SLM-OS issues both to mirror L4T's
 *     `camrtc_hsp_vm_group_ring`.
 *
 *   - Memory ordering: the IVC region lives in the NC mapping at
 *     0xBDFE0000 (`CAMRTC_CTRL_REGION_PHYS` in camrtc.c). AP writes
 *     bypass L1/L2 and hit DRAM immediately, so `dsb sy` is the
 *     only ordering needed — no cache maintenance (DC CVAC/CIVAC).
 *     AP→RCE writes get a write-then-DSB before counter bump; AP
 *     reads issue DSB before counter check.
 *
 * Concurrency: NOT thread-safe. Same constraint as `camrtc_send_msg`
 * — single shell-context caller today.
 *
 * Jetson-only (`PLATFORM_JETSON_ORIN_NANO`); other platforms link
 * stubs that return -1 from every function.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Channel state. Populated by `camrtc_ivc_init`; thereafter the
 * struct is opaque to callers — fields can move without breaking
 * the API. Lives in the caller's storage (typically a file-scope
 * static) because PMM allocation isn't needed for fixed-channel
 * use cases.
 */
struct camrtc_ivc_channel {
    /* Queue base addresses (physical, identity-mapped in the
     * kernel linear map). rx is RCE→AP, tx is AP→RCE. */
    uintptr_t rx_queue;
    uintptr_t tx_queue;

    /* Geometry. Both directions share the same nframes/frame_size
     * in the camera-rtcpu protocol (capture-control = 64 × 320). */
    uint32_t  nframes;
    uint32_t  frame_size;

    /* SS[0] group value used for the SS-set wake (1 for the
     * capture-control channel per the L4T DT binding). */
    uint32_t  group;

    /* True after `camrtc_ivc_init` succeeds. */
    bool      initialised;
};

/*
 * Initialise an IVC channel using the rx/tx IOVAs RCE bound during
 * CH_SETUP. Zeros AP's halves of both queue headers (only — RCE's
 * halves carry state RCE wrote during CH_SETUP and must be left
 * intact), then drives the SYNC→ACK→EST handshake with the remote.
 *
 *   ch          Caller-allocated channel state to populate.
 *   rx_iova     Phys address of the RCE→AP queue (the rx_iova
 *               passed to CAMRTC_HSP_CH_SETUP).
 *   tx_iova     Phys address of the AP→RCE queue.
 *   nframes     Frame count per queue (must be a power of two).
 *   frame_size  Bytes per frame (must be a multiple of 64).
 *   group       SS[0] group value (1 for capture-control).
 *
 * Pre-condition: `camrtc_init()` has returned 0 and the caller has
 * already invoked `camrtc_ch_setup_capture_control()` (or its peer
 * for non-control channels) with success.
 *
 * Returns 0 on success, -1 on bad arguments (NULL ch, zero
 * geometry, frame_size not 64-aligned, nframes not power of two),
 * -2 if the SYNC handshake doesn't reach EST/EST within ~100 ms.
 */
int camrtc_ivc_init(struct camrtc_ivc_channel *ch,
                    uintptr_t rx_iova, uintptr_t tx_iova,
                    uint32_t nframes, uint32_t frame_size,
                    uint32_t group);

/*
 * Non-blocking queue-state predicates. Both refresh the relevant
 * peer counter from DRAM via DSB before checking, so the answer
 * reflects the latest RCE-side advance.
 */
bool camrtc_ivc_can_send(struct camrtc_ivc_channel *ch);
bool camrtc_ivc_can_recv(struct camrtc_ivc_channel *ch);

/*
 * Send one frame to RCE. Copies up to `frame_size` bytes from
 * `payload` into the next AP→RCE slot, advances the AP write
 * counter, issues a memory barrier, and wakes RCE via SS[0] +
 * CAMRTC_HSP_IRQ.
 *
 *   ch       Initialised channel.
 *   payload  Source buffer.
 *   len      Bytes to copy. Must be <= frame_size; the remaining
 *            bytes of the slot are zero-padded so RCE sees a clean
 *            frame.
 *
 * Returns 0 on success, -1 on bad args (uninitialised, NULL,
 * len > frame_size), -2 if the queue is full (caller's
 * responsibility to retry or fail).
 */
int camrtc_ivc_send(struct camrtc_ivc_channel *ch,
                    const void *payload, uint32_t len);

/*
 * Receive one frame from RCE. Copies up to `buf_size` bytes from
 * the next RCE→AP slot into `buf`, advances the AP read counter,
 * and acknowledges the frame so RCE can reuse the slot.
 *
 *   ch        Initialised channel.
 *   buf       Destination buffer.
 *   buf_size  Capacity of `buf`. The full `frame_size` is copied
 *             when buf_size >= frame_size; otherwise buf_size
 *             bytes are copied and the remainder is discarded
 *             (and `*out_len` reflects what was written).
 *   out_len   Optional: filled with the number of bytes copied to
 *             `buf` (= min(frame_size, buf_size)).
 *
 * Returns 0 on success, -1 on bad args (uninitialised, NULL),
 * -2 if the queue is empty.
 */
int camrtc_ivc_recv(struct camrtc_ivc_channel *ch,
                    void *buf, uint32_t buf_size, uint32_t *out_len);

/*
 * Blocking variant of `camrtc_ivc_recv`. Polls `camrtc_ivc_can_recv`
 * until a frame is available or `timeout_us` elapses. Useful for
 * the request/response pattern of capture-control (send a REQ,
 * recv the matching RESP).
 *
 * Returns 0 on success, -2 on timeout, other negatives same as
 * `camrtc_ivc_recv`.
 */
int camrtc_ivc_recv_wait(struct camrtc_ivc_channel *ch,
                         void *buf, uint32_t buf_size,
                         uint32_t *out_len, uint32_t timeout_us);
