/*
 * hailo_control.c — Hailo firmware control-channel transport.
 *
 * This is the bottom half of #281: the RPC that lets the kernel ask
 * the booted firmware to run commands (IDENTIFY, READ/WRITE_MEMORY,
 * CONFIG_STREAM, ...). hailo_boot already puts the on-device
 * firmware in a state where it's listening on ATR[0]+BAR4 for
 * requests; this file implements the protocol HailoRT would use
 * otherwise.
 *
 * See hailo_control.h for the wire format and HailoRT references.
 *
 * Design notes:
 *
 * - We bypass the dev_read/dev_write helpers in hailo_core.c. Those
 *   save/retarget/access/restore ATR[0], which was necessary during
 *   boot (we pointed ATR[0] at different device-side pages for each
 *   upload step). Post-boot the firmware has configured ATR[0] to
 *   route BAR4[0..] at its request buffer and BAR4[0x640..] at its
 *   response buffer; we just write/read BAR4 offsets directly.
 *
 * - All transfers go through hailo_platform->bar4_write / bar4_read.
 *   These are dword-aligned on Pi 5 — our request buffer is already
 *   aligned (md5 is 16 B, len is 4 B, payload is at least 4 B of
 *   common_header each field u32).
 *
 * - Polling the response: we don't rely on MSI yet. Read
 *   BCS_ISTATUS_HOST on BAR0; the register is write-1-to-clear
 *   (matching hailo_pcie_common.c:read_and_clear_reg on the Linux
 *   side), not read-and-clear, so after observing any firing bits
 *   we write them back to clear. Multiple sources multiplex on
 *   this register — FW_CONTROL_IRQ (0x04<<24), FW notification
 *   (0x02<<24), VDMA channel IRQs in the low 16 bits. We wait
 *   specifically for the FW_CONTROL bit and clear+ignore the
 *   rest; waiting on "any non-zero" races ahead of the response
 *   on a quiet device. A future MSI path can route each source
 *   separately.
 *
 * - Concurrency: the transport is protected by a spinlock. Today
 *   all callers live on CPU 0 (shell command `hailo fw`), but
 *   future inference submit from an AI scheduler policy could
 *   call from any CPU, and the three statics below (sequence
 *   counter, IRQ-armed flag, on-stack-too-large req/resp buffers)
 *   are shared. Taking the lock also sequences request/response
 *   round-trips against the firmware, which only services one
 *   control-channel command at a time.
 */

#include "hailo.h"
#include "hailo_control.h"
#include "hailo_internal.h"
#include "hailo_trace.h"
#include "hef_parser.h"
#include "debug.h"
#include "md5.h"
#include "spinlock.h"
#include "uart.h"
#include <stddef.h>
#include <string.h>

/* Phase 8 diagnostic: last firmware error status captured by
 * control_check_response_header. Zero until the first RPC that
 * returned a non-zero major_status. Exposed via hailo shell
 * `hailo last_err` for post-wedge triage. */
volatile uint32_t hailo_control_last_err_major  = 0;
volatile uint32_t hailo_control_last_err_minor  = 0;
volatile uint32_t hailo_control_last_err_opcode = 0;

/*
 * The Hailo firmware marshals every scalar in the common header, the
 * parameter_count, and the response status/length fields via htonl /
 * ntohl (see hailort-control_protocol.cpp:199). On a little-endian
 * host these must be byteswapped before being handed to the firmware
 * and re-swapped on receive. firmware_version (major/minor/revision)
 * is the one exception — HailoRT memcpys it verbatim, so it stays
 * native.
 */
static inline uint32_t hailo_cpu_to_be32(uint32_t v)
{
    return __builtin_bswap32(v);
}
static inline uint32_t hailo_be32_to_cpu(uint32_t v)
{
    return __builtin_bswap32(v);
}

/* Sequence counter. Firmware echoes this back in the response header
 * so a response can be correlated with a request; we check it against
 * what we sent. Incremented atomically per send so hailo_control_*
 * callers that race in the request-build phase (before taking
 * control_lock inside send_recv) can't produce colliding sequence
 * numbers — otherwise two CPUs could read the same pre-increment
 * value, send distinct requests with identical seq, and each mistake
 * the other's echoed-back response for their own. */
static uint32_t control_sequence = 0;

static inline uint32_t control_next_sequence(void)
{
    return __atomic_fetch_add(&control_sequence, 1, __ATOMIC_RELAXED);
}

/*
 * Serializes the whole transport: sequence counter, IRQ-armed flag,
 * the static req/resp wire buffers, and the round-trip against
 * firmware (which services one control command at a time). Plain
 * `spin_lock` — NOT `spin_lock_irqsave` — because wait_for_response
 * polls for up to `timeout_us` and holding IRQs off that long would
 * starve the timer tick. No IRQ handler takes this lock, so the
 * non-irqsave form is safe.
 */
static spinlock_t control_lock = SPINLOCK_INIT;

/*
 * Build the on-wire request bytes: [md5(16)][buffer_len(4)][payload].
 * Returns the total bytes written into `out`. Caller sizes `out` to
 * at least sizeof(struct hailo_control_wire_hdr) + payload_len.
 */
static size_t build_request_wire(uint8_t *out,
                                 const void *payload,
                                 uint32_t payload_len)
{
    /* Wire layout: [md5(16)][buffer_len(4)][payload].
     *
     * The MD5 is computed over the PAYLOAD BYTES ONLY — not over
     * [buffer_len || payload]. This matches HailoRT's
     * VdmaDevice::fw_interact_impl:
     *     MD5_Init(&ctx);
     *     MD5_Update(&ctx, request_buffer, request_size);
     *     MD5_Final(request_md5, &ctx);
     * where request_buffer/size are exactly the payload. Including
     * the buffer_len prefix in the hash would silently mismatch
     * firmware's expected value on every request. */
    size_t off = 0;
    uint8_t md5[MD5_DIGEST_LENGTH];
    md5_compute(payload, payload_len, md5);

    memcpy(out + off, md5, MD5_DIGEST_LENGTH); off += MD5_DIGEST_LENGTH;
    memcpy(out + off, &payload_len, sizeof(uint32_t)); off += sizeof(uint32_t);
    memcpy(out + off, payload, payload_len); off += payload_len;
    return off;
}

/*
 * MSI-driven response signal. Set by the control MSI handler when
 * firmware writes to the response mailbox and raises its
 * FW_CONTROL_IRQ bit. wait_for_response acquires + clears this
 * atomically so one MSI == one response consumed.
 *
 * MSI is the path HailoRT uses. Polling still works as a fallback
 * (platforms that don't implement register_irq, or cases where we
 * call hailo_control_* before the MSI handler is registered), but
 * MSI delivers the firmware-ready signal with no scheduling
 * latency — critical for CORE-CPU context-switch opcodes that
 * return only after firmware completes async action processing
 * (see docs/pi5-ai-hat-plan.md §6.4 for the CORE-CPU-async backdrop).
 */
static volatile uint32_t control_msi_pending = 0;

static void control_msi_handler(void *ctx)
{
    (void)ctx;
    /* Snapshot ISTATUS so the per-channel and aggregate acks below
     * see a consistent view. Order matches reference
     * hailo_pcie_read_interrupt (hailo-pcie-common.c:443-448): read
     * the per-channel SRC/DST registers FIRST when their aggregate
     * bits are set, then W1C the aggregate ISTATUS. Reversing the
     * order would race fw — if the aggregate is cleared first and
     * fw flips a per-channel bit before we read it, the next IRQ
     * sees the per-channel bit but no aggregate, and we'd ignore
     * it.
     *
     * mb() after the final W1C ensures the MMIO write is globally
     * visible before control_msi_pending is set. Without it, a
     * polling-path ISTATUS read on another CPU (in wait_for_response's
     * fallback loop) could see the bit still set and W1C it again —
     * harmless but wasteful — and the explicit barrier pairs cleanly
     * with the atomic_store_release that follows. */
    uint32_t istatus = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST);

    /* Phase 8 #253: ack per-channel VDMA IRQ registers on every IRQ.
     * Reference hailo_pcie_read_interrupt reads + clears
     * BCS_SOURCE_INTERRUPT_PER_CHANNEL when the SRC aggregate bit is
     * set and BCS_DESTINATION_INTERRUPT_PER_CHANNEL when the DEST
     * aggregate bit is set. These are W1C status registers tracking
     * which individual channels have fired since the last host read.
     *
     * Prior to this fix, SLM-OS only cleared the top-level
     * ISTATUS_HOST aggregate bits and never touched the per-channel
     * registers. The ftrace/kprobe capture of hailo_pci on Pi OS
     * running MNIST showed ri (read_interrupt) firing hundreds of
     * times per inference and consistently reading both per-channel
     * registers — fw evidently expects the host to ack at this
     * granularity. Without it, fw's internal completion state
     * machine gets stuck and num_proc stops advancing on boundary
     * channels, matching the Phase 8 stall symptom (ch=2 frozen
     * with 0 register changes across the full wait window).
     *
     * Skipping the W1C when the read returns 0 avoids issuing a
     * no-op MMIO that the PCIe RC still pays a round-trip for. */
    uint32_t src_bits = 0;
    uint32_t dst_bits = 0;
    if (istatus & HAILO_BCS_ISTATUS_HOST_VDMA_SRC_MASK) {
        src_bits = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL);
        if (src_bits != 0) {
            hailo_platform->write32(
                HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL,
                src_bits);
        }
    }
    if (istatus & HAILO_BCS_ISTATUS_HOST_VDMA_DEST_MASK) {
        dst_bits = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL);
        if (dst_bits != 0) {
            hailo_platform->write32(
                HAILO_BAR_CONFIG,
                HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL,
                dst_bits);
        }
    }

    /* Aggregate ISTATUS_HOST W1C last (see ordering note above). */
    if (istatus != 0) {
        hailo_platform->write32(
            HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST, istatus);
    }
    hailo_platform->mb();

    if (istatus & HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT) {
        __atomic_store_n(&control_msi_pending, 1, __ATOMIC_RELEASE);
    }

    /* FW_NOTIFICATION_IRQ: fw posted an event to the D2H
     * notification buffer at BAR4+0x0c80. Mirrors Linux's
     * firmware_notification_irq_handler dispatch. The handler is
     * ISR-safe (single read + single ACK, no udelay/no loop); if
     * fw has more events queued it re-raises this bit and the next
     * MSI fires us again. Without this, the bit gets W1C'd above
     * and the buffer is never read — every notification (ECC,
     * CONTEXT_SWITCH_RUN_TIME_ERROR, etc.) is dropped silently. */
    if (istatus & HAILO_BCS_ISTATUS_HOST_FW_NOTIFICATION_BIT) {
        hailo_fw_handle_d2h_notification(true);
    }

    /* IRQ trace: emit one line per handler invocation with the
     * snapshotted ISTATUS + per-channel aggregates. SPI is reported
     * as 0 (the platform shim doesn't currently pass it through
     * ctx); the trace consumer can identify by mech=IRQ + phase.
     *
     * IRQ-context UART note: hailo_trace_emit_irq calls uart_printf,
     * which on Pi 5 / Jetson uses an IRQ-disable-only lock (no
     * cross-CPU spinlock — see kernel/CLAUDE.md "UART Lock on Pi 5
     * / Jetson"), so this is safe to call from the MSI handler. The
     * emit blocks for the duration of the serial drain (~hundreds
     * of bytes at 115200 baud = several ms) and lengthens the ISR
     * accordingly — acceptable for diagnostics, not for production. */
    if (hailo_trace_active(HAILO_TRACE_MECH_IRQ))
        hailo_trace_emit_irq(0u, istatus, src_bits, dst_bits);
}

/*
 * Wait for firmware to signal it's produced a response. Prefers the
 * MSI-driven flag when available (minimum latency, 0us overhead);
 * falls back to polling BCS_ISTATUS_HOST for platforms or early-boot
 * phases where MSI isn't wired up.
 *
 * Either path returns HAILO_OK as soon as the firmware-control
 * signal appears, or HAILO_ERR_TIMEOUT after `timeout_us`.
 */
static int wait_for_response(uint32_t timeout_us)
{
    const uint32_t poll_interval_us = 100;
    uint32_t elapsed = 0;

    while (elapsed < timeout_us) {
        /* MSI path: handler consumed the IRQ + set the pending flag.
         * Caller clears this flag before firing the doorbell, so any
         * pending=1 observed here reflects firmware's response to
         * THIS RPC. */
        if (__atomic_load_n(&control_msi_pending, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&control_msi_pending, 0, __ATOMIC_RELEASE);
            return HAILO_OK;
        }
        /* Polling fallback: still check ISTATUS in case the MSI
         * wasn't registered (stub platform / pre-register_irq init).
         * If MSI handler also fires it will race with this read; the
         * handler's W1C + our flag-based return is idempotent — both
         * paths converge on "return HAILO_OK once we've observed the
         * firmware-control signal at least once". */
        uint32_t istatus = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST);
        if (istatus != 0) {
            /* Handle FW_NOTIFICATION BEFORE the W1C below. If a
             * notification was posted while we were polling for a
             * FW_CONTROL response, the W1C clears the bit and Linux's
             * model has nothing to re-raise it on (notifications are
             * edge-triggered per buffer state). Reading + ACKing here
             * drains the buffer so fw can post the next event. */
            if (istatus & HAILO_BCS_ISTATUS_HOST_FW_NOTIFICATION_BIT) {
                hailo_fw_handle_d2h_notification(false);
            }
            hailo_platform->write32(
                HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST, istatus);
            if (istatus & HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT) {
                return HAILO_OK;
            }
        }
        hailo_platform->udelay(poll_interval_us);
        elapsed += poll_interval_us;
    }
    return HAILO_ERR_TIMEOUT;
}

/*
 * One-shot post-boot init for the control channel. Runs once on the
 * first hailo_control_send_recv_locked call after firmware is up
 * and does three things in order:
 *
 *   1. Point ATR[0] at the Hailo-8 control-request section
 *      (device address 0x60000000). The firmware sets this up on a
 *      "clean" driver path (Linux polls for it in
 *      hailo_pcie_is_firmware_loaded); our boot path uses the older
 *      ATR[1] magic and may leave ATR[0] pointing at the last
 *      firmware upload window (core_fw_header at 0xA0000).
 *
 *   2. Arm interrupts: OR BSC_ISTATUS_HOST_MASK into BSC_IMASK_HOST
 *      so firmware's FW_CONTROL_IRQ is latchable, then W1C any stale
 *      bits so we start from a known-quiet state. Matches
 *      hailo_pcie_enable_interrupts in the Linux driver.
 *
 *   3. Register the MSI handler (hailo_platform->register_irq) so
 *      wait_for_response's MSI-driven fast path works. Platforms
 *      without register_irq (stub / test mock) stay on the polling
 *      fallback; failure here is non-fatal.
 *
 * Was originally three independent one-shot flags; consolidated into
 * a single control_post_boot_init_done gate because the three steps
 * ALWAYS run together (any caller that needs one needs all three)
 * and three separate booleans added noise without corresponding
 * flexibility.
 */
static bool control_post_boot_init_done = false;
static bool control_irq_masks_armed = false;

/* Track whether the MSI handler is bound. Separate flag from
 * control_post_boot_init_done because pre-boot MSI registration can
 * happen *before* the post-boot init runs. control_post_boot_init
 * checks this flag and skips MSI registration if already done.
 *
 * Concurrency: this and control_irq_masks_armed are plain bool
 * flags with no lock. That's safe under the current caller model
 * because the setters all run from single-threaded init paths:
 *   - hailo_boot() at device init (no other threads)
 *   - control_post_boot_init() fires on first FW_CONTROL RPC, which
 *     runs under control_lock, serializing any concurrent callers.
 *   - hailo_control_signal_driver_shutdown() fires from cmd_reboot,
 *     which runs on the shell task before psci_system_reset — no
 *     concurrent state changes possible.
 * Any future caller that could race one of these setters would need
 * to promote the flag to atomic. */
static bool control_msi_registered = false;

int hailo_control_register_msi_for_boot(void)
{
    if (control_msi_registered) return HAILO_OK;
    if (!hailo_platform || !hailo_platform->register_irq) {
        /* No MSI infrastructure on this platform — silently skip.
         * Polling fallback will still work. */
        return HAILO_OK;
    }

    int rc = hailo_platform->register_irq(control_msi_handler, NULL);
    if (rc != HAILO_OK) {
        WARN("hailo: pre-boot MSI registration failed (%d); polling fallback", rc);
        /* Non-fatal: the existing ATR[1] poll in hailo_boot still works.
         * Mark as "registered" so post-boot init doesn't retry. */
    }
    control_msi_registered = true;
    return HAILO_OK;
}

int hailo_control_signal_driver_shutdown(void)
{
    if (!hailo_platform || !hailo_platform->bar4_write) return HAILO_ERR_NODEV;
    if (hailo_get_state() != HAILO_STATE_RUNNING) return HAILO_OK;

    hailo_trace_set_phase(HAILO_TRACE_PHASE_TEARDOWN);

    /* Mirror Linux's finalize_doorbell write: doorbell at
     * raise_ready_offset (0x1684) with FW_ACCESS_DRIVER_SHUTDOWN_MASK
     * (0x4) so fw can clear active-driver state. Best-effort: any
     * failure here is logged and ignored — we're tearing down. */
    uint32_t val = HAILO_FW_ACCESS_DRIVER_SHUTDOWN_MASK;
    hailo_platform->bar4_write(hailo_fw_addrs_hailo8.raise_ready_offset,
                               &val, sizeof(val));
    if (hailo_platform->mb) hailo_platform->mb();
    INFO("hailo: signaled DRIVER_SHUTDOWN to fw");
    return HAILO_OK;
}

void hailo_control_dump_irq_state(const char *label)
{
    if (!hailo_platform || !hailo_platform->read32) return;
    uint32_t imask = hailo_platform->read32(HAILO_BAR_CONFIG,
                                            HAILO_BSC_IMASK_HOST);
    uint32_t istat = hailo_platform->read32(HAILO_BAR_CONFIG,
                                            HAILO_BCS_ISTATUS_HOST);
    uint32_t per_src = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL);
    uint32_t per_dst = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL);
    uart_printf("[irq-state] %s: IMASK=0x%08x ISTATUS=0x%08x "
                "PER_SRC=0x%08x PER_DST=0x%08x\r\n",
                label ? label : "(none)",
                (unsigned)imask, (unsigned)istat,
                (unsigned)per_src, (unsigned)per_dst);
}

/*
 * #682 hyp-X-1 (2026-05-09): drain stale per-channel VDMA IRQ
 * pending bits + aggregate ISTATUS bits the same way Linux's
 * hailo_pcie_read_interrupt does (hailo-pcie-common.c:443-460).
 * Linux's MNIST trace shows the host issues this exact drain
 * sequence at vaddr+0x18c, +0x400 (and +0x500 if DEST set)
 * IMMEDIATELY before the first boundary IN avail bump. SLM-OS
 * leaves CFG-channel SRC pending bits stale after load completes
 * (PER_SRC=0x00000003 from ch=0+1 servicing during fw context-info
 * upload), and the trace from e8976dfc on pi-5-1 shows ch=2 wedges
 * indefinitely while these stale bits sit there.
 *
 * Mirrors control_msi_handler's drain logic with one extra
 * guard: the FW_CONTROL_BIT is preserved (excluded from the
 * aggregate W1C) and, if set, control_msi_pending is raised. This
 * keeps the contract that one fw FW_CONTROL signal always reaches
 * one wait_for_response wake — so a polling RPC waiter on another
 * CPU doesn't lose a notification we happened to drain.
 */
void hailo_control_drain_pending_irqs(const char *label)
{
    if (!hailo_platform || !hailo_platform->read32 ||
        !hailo_platform->write32) return;

    uint32_t istatus_pre = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST);
    uint32_t src_pre = 0, dst_pre = 0;

    if (istatus_pre & HAILO_BCS_ISTATUS_HOST_VDMA_SRC_MASK) {
        src_pre = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL);
        if (src_pre != 0) {
            hailo_platform->write32(
                HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL,
                src_pre);
        }
    }
    if (istatus_pre & HAILO_BCS_ISTATUS_HOST_VDMA_DEST_MASK) {
        dst_pre = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL);
        if (dst_pre != 0) {
            hailo_platform->write32(
                HAILO_BAR_CONFIG,
                HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL,
                dst_pre);
        }
    }

    /* Read+ACK the D2H notification buffer BEFORE the aggregate
     * W1C below. If a notification was posted, clearing the bit
     * without reading the buffer drops the event silently —
     * matches the wait_for_response polling-fallback fix. */
    if (istatus_pre & HAILO_BCS_ISTATUS_HOST_FW_NOTIFICATION_BIT) {
        hailo_fw_handle_d2h_notification(false);
    }

    uint32_t to_clear = istatus_pre & ~HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT;
    if (to_clear != 0) {
        hailo_platform->write32(
            HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST, to_clear);
    }
    if (istatus_pre & HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT) {
        __atomic_store_n(&control_msi_pending, 1, __ATOMIC_RELEASE);
    }
    if (hailo_platform->mb) hailo_platform->mb();

    /* Polled-drain IRQ trace. The MSI handler also emits an IRQ
     * trace at its tail, but on fast-fw RPCs the polling path
     * (wait_for_response → this drain) clears ISTATUS before the
     * handler runs. Without this emit, polled drains would be
     * invisible in the trace while interrupt-delivered IRQs would
     * appear — confusing asymmetry. spi=0 marks this as a poll
     * rather than a hardware IRQ delivery. */
    if ((src_pre | dst_pre | istatus_pre) != 0u &&
        hailo_trace_active(HAILO_TRACE_MECH_IRQ)) {
        hailo_trace_emit_irq(0u, istatus_pre, src_pre, dst_pre);
    }

    uint32_t istatus_post = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST);
    uint32_t src_post = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL);
    uint32_t dst_post = hailo_platform->read32(
        HAILO_BAR_CONFIG, HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL);
    uart_printf("[irq-drain] %s: ISTATUS pre=0x%08x post=0x%08x "
                "PER_SRC pre=0x%08x post=0x%08x "
                "PER_DST pre=0x%08x post=0x%08x\r\n",
                label ? label : "(none)",
                (unsigned)istatus_pre, (unsigned)istatus_post,
                (unsigned)src_pre, (unsigned)src_post,
                (unsigned)dst_pre, (unsigned)dst_post);
}

int hailo_control_arm_irq_masks(void)
{
    if (control_irq_masks_armed) return HAILO_OK;
    if (!hailo_platform || !hailo_platform->write32 || !hailo_platform->read32) {
        return HAILO_ERR_NODEV;
    }

    /* Mirrors hailo_pcie_enable_interrupts (hailo-pcie-common.c:867):
     * arm IMASK_HOST then W1C any stale ISTATUS bits, then arm ALL
     * 32 SRC + 32 DST per-channel IRQ masks. Linux does this BEFORE
     * the fw trigger so the device boots into a fully-armed IRQ
     * state. */
    uint32_t mask = hailo_platform->read32(HAILO_BAR_CONFIG,
                                           HAILO_BSC_IMASK_HOST);
    mask |= HAILO_BSC_ISTATUS_HOST_MASK;
    hailo_platform->write32(HAILO_BAR_CONFIG, HAILO_BSC_IMASK_HOST, mask);
    hailo_platform->write32(HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST,
                            0xFFFFFFFFu);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL,
                            0xFFFFFFFFu);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL,
                            0xFFFFFFFFu);
    hailo_platform->mb();

    control_irq_masks_armed = true;
    return HAILO_OK;
}

/* #682 hyp-N (2026-05-09): mirror Linux's hailo_disable_interrupts
 * after the BOOT_IRQ ack. The Pi OS boot-phase MMIO trace shows
 * IMASK_HOST written to 0 right after the post-fw-load ISTATUS W1C;
 * Linux then idles in D3hot until first open() and re-arms IMASK at
 * that point. SLM-OS leaves IMASK armed continuously, which means our
 * MSI handler can fire on fw-internal events (CPU_ECC notifications,
 * boundary IRQ aggregates, etc.) during the post-boot/pre-load idle
 * window and silently W1C them. If fw is sensitive to host IRQ state
 * during that window — e.g., expects to W1C its own SAGE init bits
 * without competition from a host handler — disarming gives it the
 * same conditions Linux does.
 *
 * Also used under HAILO_IRQ_CYCLE_AT_BOOT to wrap a disable→re-enable
 * cycle around the post-boot D3hot transition (replaces the older
 * hailo_control_disable_imask name from PR #695).
 *
 * The arm flag is cleared too; control_post_boot_init's existing call
 * to hailo_control_arm_irq_masks (early-returns when the flag is set)
 * will re-arm on the first FW_CONTROL RPC, symmetric to Linux's
 * re-enable on open(). */
int hailo_control_disarm_irq_masks(void)
{
    if (!hailo_platform || !hailo_platform->write32) {
        return HAILO_ERR_NODEV;
    }
    hailo_platform->write32(HAILO_BAR_CONFIG, HAILO_BSC_IMASK_HOST, 0u);
    if (hailo_platform->mb) hailo_platform->mb();
    control_irq_masks_armed = false;
    return HAILO_OK;
}

static void control_post_boot_init(void)
{
    if (control_post_boot_init_done) return;

    /* ATR[0] retarget. Originally called every RPC, which is
     * wasteful (the window doesn't move) and creates a theoretical
     * race where firmware's response-write could land mid-reprogram.
     * Hardware-verified identical behavior with one-shot vs per-RPC
     * on pi-5-1 fw v4.23. */
    uint32_t atr0 = HAILO_ATR_BASE;
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_PARAM,
                            HAILO_ATR_PARAM(0));
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_SRC, 0u);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_LO,
                            HAILO_CONTROL_SECTION_ADDR_H8);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_HI, 0u);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_PARAM,
                            HAILO_ATR_TRSL_AXI);
    hailo_platform->mb();

    /* Arm interrupts. Delegated to hailo_control_arm_irq_masks so
     * both the pre-boot path (hailo_boot) and this post-boot path
     * share identical register writes and flag gating. The helper
     * is idempotent — if the pre-boot path already armed masks,
     * this call is a cheap early-return. */
    (void)hailo_control_arm_irq_masks();

    /* Register the MSI handler — non-fatal on failure. Skip if the
     * boot path already registered it via
     * hailo_control_register_msi_for_boot. */
    if (hailo_platform->register_irq && !control_msi_registered) {
        int rc = hailo_platform->register_irq(control_msi_handler, NULL);
        if (rc != HAILO_OK) {
            WARN("hailo: control MSI registration failed (%d); polling fallback", rc);
            /* Mark done anyway — retry wouldn't help, and the
             * polling fallback stays available. */
        }
        control_msi_registered = true;
    }

    control_post_boot_init_done = true;
}

/*
 * BSS footprint summary for this file (kernel/ai_accel/hailo/hailo_control.c):
 *
 *   control_req_wire             ~1520 B  (transport request)
 *   control_resp_wire            ~1500 B  (transport response)
 *   read_memory wire / resp      ~ 270 B  (per-opcode scratch, defined
 *   write_memory wire             ~ 280 B  below in hailo_control_*
 *   config_stream wire            ~ 240 B  helpers)
 *   set_network_group_header      ~ 230 B
 *   set_context_info wire         ~1024 B
 *   change_status wire            ~ 230 B
 *   get_hw_consts resp            ~ 264 B
 *
 * Total per-file BSS reservation is roughly 5–6 KB across the
 * transport buffers and roughly another ~3–4 KB across the per-opcode
 * scratches. Chosen over stack allocation because the 16 KB kernel
 * stack can't comfortably carry the per-call transient scratch on top
 * of the boot-path frames already on it. Chosen over heap allocation
 * because this file runs on Pi 5 only and the simpler static layout
 * is easier to audit. Only reachable post-boot once firmware is
 * running, so the BSS pressure is paid once for the kernel lifetime.
 * If a future port adds another platform, revisit this budget — the
 * static-buffer count grows with opcode count and the linear-scan
 * cost is already noticeable in `nm | grep control_`.
 */
static uint8_t control_req_wire[sizeof(struct hailo_control_wire_hdr)
                              + HAILO_CONTROL_MAX_BUFFER_LENGTH];
static uint8_t control_resp_wire[HAILO_CONTROL_MAX_BUFFER_LENGTH];

/*
 * Shared arg-validation used by both the public and locked entry
 * points. Returns HAILO_OK on success, or the appropriate error
 * code; writes 0 into *resp_len on every error path so callers
 * don't read stale bytes.
 */
static int control_validate_send_recv_args(const void *req_payload,
                                           uint32_t    req_len,
                                           void       *resp_payload,
                                           uint32_t    resp_capacity,
                                           uint32_t   *resp_len)
{
    if (resp_len) *resp_len = 0;
    if (!hailo_platform || hailo_get_state() != HAILO_STATE_RUNNING) {
        return HAILO_ERR_NODEV;
    }
    if (!req_payload || req_len == 0
     || req_len > HAILO_CONTROL_MAX_BUFFER_LENGTH) {
        return HAILO_ERR_INVAL;
    }
    if (!resp_payload || !resp_len
     || resp_capacity > HAILO_CONTROL_MAX_BUFFER_LENGTH) {
        return HAILO_ERR_INVAL;
    }
    return HAILO_OK;
}

/*
 * Send/receive with control_lock already held. Used by the chunked
 * WRITE_MEMORY / READ_MEMORY helpers, which need to populate their
 * static BSS request structs under the same lock they'll run the
 * I/O under (otherwise the populate-then-call pattern would race).
 * Plain callers should use hailo_control_send_recv; this is a
 * private helper.
 */
static int hailo_control_send_recv_locked(enum hailo_control_cpu cpu_id,
                                          const void *req_payload,
                                          uint32_t    req_len,
                                          void       *resp_payload,
                                          uint32_t    resp_capacity,
                                          uint32_t   *resp_len,
                                          uint32_t    timeout_us)
{
    /* One-shot post-boot init: ATR[0] retarget + IMASK arm + MSI
     * handler registration. Gated so subsequent RPCs skip the setup. */
    control_post_boot_init();

    /* Clear stale BCS_ISTATUS_HOST bits AND control_msi_pending
     * BEFORE firing the new doorbell. Ordering matters:
     *
     *   1. W1C ISTATUS first, so any deferred IRQ in the GIC that
     *      hasn't reached the handler yet will, when it does fire,
     *      read ISTATUS=0 and be a no-op — it cannot re-set
     *      control_msi_pending.
     *   2. Then clear control_msi_pending. Any prior RPC's handler
     *      that already ran between step 1 and this clear would have
     *      seen ISTATUS=0 and done nothing, so pending=1 here is
     *      strictly from a handler run BEFORE step 1 — the "stale
     *      pending from the previous RPC" case we need to clear.
     *   3. mb() pairs the two stores with everything after.
     *
     * Clearing pending AFTER the doorbell is tempting (it narrows
     * the "stale pending" window) but opens a much worse race: on
     * a fast firmware response, the MSI handler runs between the
     * doorbell and the clear, sets pending=1, then we clobber it
     * back to 0. wait_for_response would then see ISTATUS=0 (the
     * handler W1C'd it) and pending=0, falling through to a full
     * timeout_us busy-wait. Firmware response latency is ~µs and
     * our instruction window between doorbell and clear is ~ns, so
     * the clobber race is unlikely in practice — but the pre-
     * doorbell ordering above is race-free under the documented
     * ISTATUS level-hold semantics and is strictly simpler. */
    uint32_t stale = hailo_platform->read32(HAILO_BAR_CONFIG,
                                            HAILO_BCS_ISTATUS_HOST);
    if (stale != 0) {
        hailo_platform->write32(HAILO_BAR_CONFIG,
                                HAILO_BCS_ISTATUS_HOST, stale);
        hailo_platform->mb();
    }
    /* __ATOMIC_RELEASE already orders this store before the
     * subsequent request/doorbell writes; no additional mb() needed.
     * The mb() after the ISTATUS W1C above is separate — it pairs
     * the MMIO write with the atomic store that follows. */
    __atomic_store_n(&control_msi_pending, 0, __ATOMIC_RELEASE);

    size_t wire_len = build_request_wire(control_req_wire, req_payload, req_len);

    /* Write request to BAR4 at offset 0. Firmware has ATR[0]
     * configured to land this in its request buffer. dword-
     * aligned length required by the platform shim's bar4_write. */
    size_t aligned_len = (wire_len + 3u) & ~(size_t)3u;
    hailo_platform->bar4_write(0, control_req_wire, aligned_len);
    hailo_platform->mb();

    /* Ring the doorbell: APP CPU for ordinary opcodes, CORE CPU
     * for context-switch opcodes. raise_ready_offset (0x1684 on
     * Hailo-8) is a direct BAR4 offset — the Linux driver writes
     * to it via `resources->fw_access` which is BAR4, and that's
     * how the firmware picks up the "request ready" event. */
    uint32_t doorbell_val = (cpu_id == HAILO_CTRL_CPU_CORE)
                                ? HAILO_FW_ACCESS_CORE_CPU_CONTROL_MASK
                                : HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK;
    hailo_platform->bar4_write(hailo_fw_addrs_hailo8.raise_ready_offset,
                               &doorbell_val, sizeof(doorbell_val));
    hailo_platform->mb();

    /* RPC tx trace. The request common header is 16 bytes:
     *   [version(4)][flags(4)][sequence(4)][opcode(4)] (all BE32)
     * so the opcode's low byte is at offset 15 of req_payload. The
     * 16-byte size is pinned by the _Static_assert next to
     * `struct hailo_control_common_header` in hailo_control.h.
     * md5 lives in control_req_wire bytes 0..15; first 8 bytes are
     * enough to identify the RPC across a capture without bloating
     * the line. */
    if (hailo_trace_active(HAILO_TRACE_MECH_RPC)) {
        uint8_t op = 0;
        if (req_payload && req_len >= 16) {
            const uint8_t *p = (const uint8_t *)req_payload;
            op = p[15];  /* low byte of BE32 opcode at offset 12..15 */
        }
        hailo_trace_emit_rpc_tx(op, req_len, (uint8_t)doorbell_val,
                                control_req_wire);
    }

    /* TODO(#332): wait_for_response is a udelay-polled busy wait.
     * For #281 tier-1 (shell-driven IDENTIFY) this is fine — the
     * lone caller on CPU 0 just waits. For Phase 5.3+ inference
     * submit, this should either yield() between polls or route
     * through the future MSI path so CPU 0 isn't burned for up to
     * a full timeout_us. Picked up during Phase 7 — surface via
     * `gh issue list --label sub:ai-runtime`. */
    int rc = wait_for_response(timeout_us);
    if (rc != HAILO_OK) goto out;

    /* Read response header (md5 + buffer_len) from BAR4+0x640. */
    struct {
        uint8_t  md5[MD5_DIGEST_LENGTH];
        uint32_t buffer_len;
    } __attribute__((packed)) resp_hdr;
    hailo_platform->bar4_read(HAILO_CONTROL_REQUEST_RESPONSE_OFFSET,
                              &resp_hdr, sizeof(resp_hdr));

    /* buffer_len is written by firmware as native LE (same host
     * endianness contract as the payload body) — no byteswap here.
     * The scalar fields *inside* the payload (opcode/sequence/status)
     * are big-endian and get swapped below. */
    if (resp_hdr.buffer_len == 0
     || resp_hdr.buffer_len > HAILO_CONTROL_MAX_BUFFER_LENGTH) {
        WARN("hailo: control response has bad buffer_len=%u",
             resp_hdr.buffer_len);
        rc = HAILO_ERR_BAD_FIRMWARE;
        goto out;
    }

    uint32_t read_len = resp_hdr.buffer_len;
    size_t aligned_read = (read_len + 3u) & ~(size_t)3u;
    hailo_platform->bar4_read(
        HAILO_CONTROL_REQUEST_RESPONSE_OFFSET + sizeof(resp_hdr),
        control_resp_wire, aligned_read);

    /* Verify response MD5 is computed over the payload bytes only
     * (same pattern as the request side; HailoRT's check in
     * VdmaDevice::fw_interact_impl hashes response_buffer alone). */
    uint8_t check[MD5_DIGEST_LENGTH];
    md5_compute(control_resp_wire, read_len, check);
    if (memcmp(check, resp_hdr.md5, MD5_DIGEST_LENGTH) != 0) {
        WARN("hailo: control response MD5 mismatch");
        rc = HAILO_ERR_BAD_FIRMWARE;
        goto out;
    }

    /* Copy into caller's buffer (capped at resp_capacity). */
    uint32_t copy_len = read_len < resp_capacity ? read_len : resp_capacity;
    memcpy(resp_payload, control_resp_wire, copy_len);
    *resp_len = copy_len;
    rc = HAILO_OK;

    /* RPC rx trace. Response common header is 16 B (same layout as
     * request) followed by an 8-B response_status:
     *   [version(4)][flags(4)][sequence(4)][opcode(4)] [major(4)][minor(4)]
     * all BE32. Decode major+minor from bytes 16..23 of the response
     * payload when the readback is at least header-sized.
     *
     * major/minor are u32 on the wire, but the running fw populates
     * only the low byte of each (all known HAILO_COMMON_STATUS_*
     * codes fit in u8). The trace deliberately passes only that low
     * byte — narrowing the line by 6 chars per field. If a future fw
     * starts populating the upper 24 bits, widen the trace fields
     * to u32 here AND in hailo_trace_emit_rpc_rx; the low-byte read
     * is intentional truncation, not a placeholder. */
    if (hailo_trace_active(HAILO_TRACE_MECH_RPC)) {
        uint8_t major = 0, minor = 0;
        if (read_len >= 24) {
            major = control_resp_wire[19]; /* low byte of major BE32 @ 16 */
            minor = control_resp_wire[23]; /* low byte of minor BE32 @ 20 */
        }
        hailo_trace_emit_rpc_rx(major, minor, copy_len);
    }

out:
    return rc;
}

/*
 * Public send/receive entry point. Validates, takes control_lock,
 * dispatches to the locked core, releases. No IRQ handler takes
 * this lock, so plain spin_lock — not spin_lock_irqsave — is
 * correct; irqsave is deliberately avoided so wait_for_response
 * can poll for up to timeout_us without starving the timer tick.
 */
int hailo_control_send_recv(const void *req_payload,
                            uint32_t    req_len,
                            void       *resp_payload,
                            uint32_t    resp_capacity,
                            uint32_t   *resp_len,
                            uint32_t    timeout_us)
{
    return hailo_control_send_recv_cpu(HAILO_CTRL_CPU_APP,
                                       req_payload, req_len,
                                       resp_payload, resp_capacity,
                                       resp_len, timeout_us);
}

int hailo_control_send_recv_cpu(enum hailo_control_cpu cpu_id,
                                const void *req_payload,
                                uint32_t    req_len,
                                void       *resp_payload,
                                uint32_t    resp_capacity,
                                uint32_t   *resp_len,
                                uint32_t    timeout_us)
{
    int rc = control_validate_send_recv_args(req_payload, req_len,
                                             resp_payload, resp_capacity,
                                             resp_len);
    if (rc != HAILO_OK) return rc;

    spin_lock(&control_lock);
    rc = hailo_control_send_recv_locked(cpu_id,
                                        req_payload, req_len,
                                        resp_payload, resp_capacity,
                                        resp_len, timeout_us);
    spin_unlock(&control_lock);
    return rc;
}

void hailo_control_reset_state_for_tests(void)
{
    /* Clear every static that gates idempotency — the tests re-boot
     * the mock device between cases and expect each init path
     * (sequence counter, MSI-pending latch, per-boot IRQ mask
     * arming, MSI handler registration, post-boot init pipeline)
     * to fire cleanly. Missing any one of these when a new static
     * is added causes spurious test failures where the second
     * test's control_setup_running skips a write or registration
     * because the first test already set the flag. Add new statics
     * here as they're introduced. */
    __atomic_store_n(&control_sequence, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&control_msi_pending, 0, __ATOMIC_RELAXED);
    control_post_boot_init_done = false;
    control_irq_masks_armed     = false;
    control_msi_registered      = false;
}

int hailo_control_identify(struct hailo_control_identify_response *out)
{
    if (!out) return HAILO_ERR_INVAL;

    uint32_t sequence = control_next_sequence();

    /* IDENTIFY has an empty payload body (parameter_count = 0).
     * The request is: [common_header][parameter_count=0]. All fields
     * go on the wire in big-endian (see control_protocol__pack_*). */
    struct {
        struct hailo_control_common_header common;
        uint32_t                           parameter_count;
    } __attribute__((packed)) req;

    req.common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    req.common.flags    = 0;   /* ack bit clear; swap of zero is zero */
    req.common.sequence = hailo_cpu_to_be32(sequence);
    req.common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_IDENTIFY);
    req.parameter_count = 0;   /* swap of zero is zero */

    /* Response wire layout:
     *   [common_header(16)][status(8)][parameter_count(4)][body].
     * HailoRT's parse_response reads past a CONTROL_PROTOCOL__payload_t
     * (which is `parameter_count` + flexible `parameters[]`) before
     * pointing `identify_response` at `payload->parameters`. For
     * IDENTIFY the parameter_count is 0 and the body sits directly
     * after it, so we need the explicit 4 B gap here. Without it,
     * every field in `body` reads 4 B earlier than it should and
     * fw_version.major picks up fw_version_length (0x0C000000). */
    struct {
        struct hailo_control_response_header          header;
        uint32_t                                      parameter_count;
        struct hailo_control_identify_response        body;
    } __attribute__((packed)) resp;

    uint32_t resp_len = 0;
    int rc = hailo_control_send_recv(&req, sizeof(req),
                                     &resp, sizeof(resp),
                                     &resp_len,
                                     /* timeout */ 1000000u /* 1 s */);
    if (rc != HAILO_OK) return rc;

    if (resp_len < sizeof(resp)) {
        WARN("hailo: IDENTIFY response truncated (%u < %u)",
             resp_len, (unsigned)sizeof(resp));
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* Response scalars are big-endian — unswap before checking. */
    uint32_t opcode = hailo_be32_to_cpu(resp.header.common.opcode);
    uint32_t major  = hailo_be32_to_cpu(resp.header.status.major_status);
    uint32_t minor  = hailo_be32_to_cpu(resp.header.status.minor_status);

    if (opcode != HAILO_CONTROL_OPCODE_IDENTIFY) {
        WARN("hailo: IDENTIFY response wrong opcode (got 0x%x)", opcode);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    if (major != 0) {
        WARN("hailo: IDENTIFY failed (major=%u minor=%u)", major, minor);
        return HAILO_ERR_IO;
    }

    /* fw_version (major/minor/revision) is memcpy'd raw by HailoRT —
     * firmware writes it in native LE, so leave the body alone. */
    memcpy(out, &resp.body, sizeof(*out));
    return HAILO_OK;
}

/*
 * Common post-send validation for WRITE/READ_MEMORY. Both opcodes
 * share the response-header layout and the status check; only the
 * body shape differs. `resp_len` is what send_recv wrote; `opcode`
 * is the request opcode we expect the firmware to echo back.
 */
static int control_check_response_header(
    const struct hailo_control_response_header *hdr,
    uint32_t resp_len,
    uint32_t expected_opcode,
    const char *op_name)
{
    if (resp_len < sizeof(*hdr)) {
        WARN("hailo: %s response truncated (%u < %u)",
             op_name, resp_len, (unsigned)sizeof(*hdr));
        return HAILO_ERR_BAD_FIRMWARE;
    }
    uint32_t opcode = hailo_be32_to_cpu(hdr->common.opcode);
    uint32_t major  = hailo_be32_to_cpu(hdr->status.major_status);
    uint32_t minor  = hailo_be32_to_cpu(hdr->status.minor_status);
    /* Check status BEFORE opcode-echo: firmware's rejection path
     * leaves the echoed opcode as 0xFFFFFFFF (observed on pi-5-1
     * with a deliberately minimal CONFIG_STREAM request) rather
     * than mirroring back the request opcode. Treat that as a
     * firmware-error rather than a transport-level protocol
     * violation — the status fields carry the actual reason and
     * HAILO_ERR_IO is the right signal to the caller. True
     * protocol violations (opcode is any other value AND status
     * says success) still fall through as BAD_FIRMWARE. */
    if (major != 0) {
        /* Phase 8: stash last firmware-error status so shell can dump
         * it post-wedge via `hailo last_err`. uart_printf WARN output
         * gets corrupted on real HEFs for reasons not yet understood,
         * so an observable global is the reliable diagnostic channel. */
        hailo_control_last_err_major = major;
        hailo_control_last_err_minor = minor;
        hailo_control_last_err_opcode = opcode;
        WARN("hailo: %s failed (major=0x%x minor=0x%x opcode_echo=0x%x)",
             op_name, major, minor, opcode);
        return HAILO_ERR_IO;
    }
    if (opcode != expected_opcode) {
        WARN("hailo: %s response wrong opcode "
             "(got 0x%x resp_len=%u)", op_name, opcode, resp_len);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    return HAILO_OK;
}

/*
 * Shared file-scope scratch for the WRITE/READ_MEMORY chunk wire
 * structs. Previously built on the stack (~1 KB per call, on top
 * of send_recv's own frame and the 1.5 KB control_req_wire BSS
 * copy). Moving to BSS keeps the kernel-stack footprint bounded
 * so Phase 5.3 can stack additional chunk helpers without compounding
 * the overhead. Access is serialized by control_lock; the chunk
 * helpers take the lock before writing here and pass the populated
 * buffer through hailo_control_send_recv_locked so the lock is
 * held continuously from populate through I/O.
 */
struct control_write_memory_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;
    uint32_t address_length;
    uint32_t address;
    uint32_t data_length;
    uint8_t  data[HAILO_CONTROL_MAX_MEMORY_CHUNK];
} __attribute__((packed));

struct control_read_memory_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;
    uint32_t address_length;
    uint32_t address;
    uint32_t data_count_length;
    uint32_t data_count;
} __attribute__((packed));

struct control_read_memory_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;
    uint32_t data_length;
    uint8_t  data[HAILO_CONTROL_MAX_MEMORY_CHUNK];
} __attribute__((packed));

static struct control_write_memory_req_wire control_mem_write_req;
static struct control_read_memory_req_wire  control_mem_read_req;
static struct control_read_memory_resp_wire control_mem_read_resp;

/*
 * One-chunk WRITE_MEMORY round-trip. HailoRT's
 * CONTROL_PROTOCOL__pack_write_memory_request packs:
 *   [common_header(16)] [parameter_count=2(4)]
 *   [address_length=4(4)] [address(4)]
 *   [data_length(4)] [data(data_length)]
 * Every scalar is big-endian; `data` is raw bytes. Caller
 * enforces chunk_size <= HAILO_CONTROL_MAX_MEMORY_CHUNK.
 */
static int control_write_memory_chunk(uint32_t address,
                                      const uint8_t *data,
                                      uint32_t chunk_size)
{
    /* Lock spans populate + I/O so the static scratch isn't raced. */
    spin_lock(&control_lock);

    control_mem_write_req.common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    control_mem_write_req.common.flags    = 0;
    control_mem_write_req.common.sequence = hailo_cpu_to_be32(control_next_sequence());
    control_mem_write_req.common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_WRITE_MEMORY);
    control_mem_write_req.parameter_count = hailo_cpu_to_be32(2u);
    control_mem_write_req.address_length  = hailo_cpu_to_be32(sizeof(control_mem_write_req.address));
    control_mem_write_req.address         = hailo_cpu_to_be32(address);
    control_mem_write_req.data_length     = hailo_cpu_to_be32(chunk_size);
    memcpy(control_mem_write_req.data, data, chunk_size);

    /* Only the prefix up through `data[chunk_size]` travels on the
     * wire. Pack the full struct size minus the unused trailing data. */
    uint32_t req_len = (uint32_t)(offsetof(struct control_write_memory_req_wire,
                                            data)
                                  + chunk_size);

    /* Response is just header + status (no body). */
    struct hailo_control_response_header resp;
    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_mem_write_req, req_len,
                                             &resp, sizeof(resp), &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_APP,
                                            &control_mem_write_req, req_len,
                                            &resp, sizeof(resp), &resp_len,
                                            /* 1 s */ 1000000u);
    }
    spin_unlock(&control_lock);
    if (rc != HAILO_OK) return rc;

    return control_check_response_header(&resp, resp_len,
                                         HAILO_CONTROL_OPCODE_WRITE_MEMORY,
                                         "WRITE_MEMORY");
}

int hailo_control_write_memory(uint32_t address,
                               const void *data,
                               uint32_t data_length)
{
    if (!data || data_length == 0) return HAILO_ERR_INVAL;
    /* Sanity cap. Generous relative to any realistic caller
     * (CCW uploads are model-sized, typically <16 MB); mostly a
     * guard against accidental UINT32_MAX / overflow patterns
     * reaching the transport and issuing millions of doorbells. */
    if (data_length > HAILO_CONTROL_MAX_MEMORY_TRANSFER) return HAILO_ERR_INVAL;
    /* Reject the address+length wraparound pattern. Without this
     * check the chunk loop would advance past UINT32_MAX and wrap
     * to low device addresses. */
    if (address + data_length < address) return HAILO_ERR_INVAL;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = data_length;
    uint32_t cur_addr  = address;

    while (remaining > 0) {
        uint32_t chunk = remaining < HAILO_CONTROL_MAX_MEMORY_CHUNK
                        ? remaining
                        : HAILO_CONTROL_MAX_MEMORY_CHUNK;
        int rc = control_write_memory_chunk(cur_addr, p, chunk);
        if (rc != HAILO_OK) return rc;
        p         += chunk;
        cur_addr  += chunk;
        remaining -= chunk;
    }
    return HAILO_OK;
}

/*
 * One-chunk READ_MEMORY round-trip. HailoRT's
 * CONTROL_PROTOCOL__pack_read_memory_request packs:
 *   [common_header(16)] [parameter_count=2(4)]
 *   [address_length=4(4)] [address(4)]
 *   [data_count_length=4(4)] [data_count(4)]
 * Response layout:
 *   [response_header(24)] [parameter_count(4)]
 *   [data_length(4)] [data(data_length, raw bytes)]
 * data_length is BE; data is raw (memcpy'd by HailoRT).
 */
static int control_read_memory_chunk(uint32_t address,
                                     uint8_t *data,
                                     uint32_t chunk_size)
{
    spin_lock(&control_lock);

    control_mem_read_req.common.version    = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    control_mem_read_req.common.flags      = 0;
    control_mem_read_req.common.sequence   = hailo_cpu_to_be32(control_next_sequence());
    control_mem_read_req.common.opcode     = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_READ_MEMORY);
    control_mem_read_req.parameter_count   = hailo_cpu_to_be32(2u);
    control_mem_read_req.address_length    = hailo_cpu_to_be32(sizeof(control_mem_read_req.address));
    control_mem_read_req.address           = hailo_cpu_to_be32(address);
    control_mem_read_req.data_count_length = hailo_cpu_to_be32(sizeof(control_mem_read_req.data_count));
    control_mem_read_req.data_count        = hailo_cpu_to_be32(chunk_size);

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_mem_read_req,
                                             sizeof(control_mem_read_req),
                                             &control_mem_read_resp,
                                             sizeof(control_mem_read_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_APP,
                                            &control_mem_read_req,
                                            sizeof(control_mem_read_req),
                                            &control_mem_read_resp,
                                            sizeof(control_mem_read_resp),
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }

    /* control_mem_read_resp is a file-scope static so the response
     * data survives releasing the lock, but we must finish reading
     * out of it before another chunk runs. Snapshot length fields
     * and copy out under the lock. */
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    /*
     * control_mem_read_resp.header sits at offset 0 inside the
     * packed struct and is therefore as-aligned as the aggregate
     * itself. GCC's -Waddress-of-packed-member doesn't track that,
     * so copy the header out by value rather than passing a
     * pointer into the packed aggregate.
     */
    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_mem_read_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_READ_MEMORY,
                                       "READ_MEMORY");
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    /* Need at least enough response bytes to cover the fixed
     * prefix plus the `chunk_size` data bytes firmware claims to
     * have written. */
    uint32_t fixed = (uint32_t)(offsetof(struct control_read_memory_resp_wire,
                                         data));
    if (resp_len < fixed + chunk_size) {
        WARN("hailo: READ_MEMORY response short (%u < %u)",
             resp_len, fixed + chunk_size);
        spin_unlock(&control_lock);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    uint32_t data_length = hailo_be32_to_cpu(control_mem_read_resp.data_length);
    if (data_length != chunk_size) {
        WARN("hailo: READ_MEMORY returned %u bytes, expected %u",
             data_length, chunk_size);
        spin_unlock(&control_lock);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    memcpy(data, control_mem_read_resp.data, chunk_size);
    spin_unlock(&control_lock);
    return HAILO_OK;
}

int hailo_control_read_memory(uint32_t address,
                              void *data,
                              uint32_t data_length)
{
    if (!data || data_length == 0) return HAILO_ERR_INVAL;
    if (data_length > HAILO_CONTROL_MAX_MEMORY_TRANSFER) return HAILO_ERR_INVAL;
    if (address + data_length < address) return HAILO_ERR_INVAL;

    uint8_t *p = (uint8_t *)data;
    uint32_t remaining = data_length;
    uint32_t cur_addr  = address;

    while (remaining > 0) {
        uint32_t chunk = remaining < HAILO_CONTROL_MAX_MEMORY_CHUNK
                        ? remaining
                        : HAILO_CONTROL_MAX_MEMORY_CHUNK;
        int rc = control_read_memory_chunk(cur_addr, p, chunk);
        if (rc != HAILO_OK) return rc;
        p         += chunk;
        cur_addr  += chunk;
        remaining -= chunk;
    }
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Phase 5.3: CCW upload from parsed hef_info                                  */
/* -------------------------------------------------------------------------- */

int hailo_control_upload_ccw(const struct hef_info *info,
                             const void *blob_base, size_t blob_size,
                             const void *ccws_base, size_t ccws_size,
                             uint32_t device_base_addr,
                             uint64_t *out_bytes_uploaded)
{
    if (out_bytes_uploaded) *out_bytes_uploaded = 0;
    if (!info || !blob_base) return HAILO_ERR_INVAL;
    /* Truncated info means the parser hit HEF_PARSER_MAX_CCW_ACTIONS
     * and stopped storing — we can't upload the full model from a
     * partial list. Raising the cap and re-parsing is the fix. */
    if (info->ccw_actions_truncated) return HAILO_ERR_INVAL;

    /* Overflow guard on the running target address. Prevents a
     * malformed HEF (or a caller passing a device_base_addr close
     * to UINT32_MAX) from wrapping mid-upload and landing later
     * actions at low device addresses. */
    if ((uint64_t)device_base_addr + info->ccw_total_bytes
        > (uint64_t)UINT32_MAX) {
        return HAILO_ERR_INVAL;
    }

    const uint8_t *blob = (const uint8_t *)blob_base;
    const uint8_t *ccws = (const uint8_t *)ccws_base;
    uint32_t cur_addr   = device_base_addr;
    uint64_t total      = 0;

    for (uint32_t i = 0; i < info->ccw_action_count; i++) {
        const struct hef_ccw_action *a = &info->ccw_actions[i];
        if (a->data_size == 0) continue;   /* nothing to write */

        /* Bounds-check the action's source range against the
         * appropriate buffer. Without this, a malformed HEF could
         * walk data_offset_in_blob + data_size off the end of
         * blob/ccws and leak post-buffer kernel memory to firmware
         * via WRITE_MEMORY. 64-bit arithmetic avoids overflow in
         * the `end` computation (data_offset_in_blob + data_size
         * are both u32, sum fits in u64). */
        uint64_t end = (uint64_t)a->data_offset_in_blob + a->data_size;
        const uint8_t *src;
        if (a->is_ccw_ptr) {
            if (!ccws) {
                WARN("hailo: CCW upload skipped action %u: is_ccw_ptr "
                     "but ccws_base is NULL", i);
                return HAILO_ERR_INVAL;
            }
            if (end > ccws_size) {
                WARN("hailo: CCW action %u out of CCWS bounds "
                     "(offset=%u size=%u ccws_size=%zu)",
                     i, a->data_offset_in_blob, a->data_size, ccws_size);
                return HAILO_ERR_INVAL;
            }
            src = ccws + a->data_offset_in_blob;
        } else {
            if (end > blob_size) {
                WARN("hailo: CCW action %u out of blob bounds "
                     "(offset=%u size=%u blob_size=%zu)",
                     i, a->data_offset_in_blob, a->data_size, blob_size);
                return HAILO_ERR_INVAL;
            }
            src = blob + a->data_offset_in_blob;
        }

        int rc = hailo_control_write_memory(cur_addr, src, a->data_size);
        if (rc != HAILO_OK) {
            WARN("hailo: CCW upload failed at action %u (rc=%d, "
                 "address=0x%x, size=%u)", i, rc, cur_addr, a->data_size);
            return rc;
        }
        cur_addr += a->data_size;   /* already checked not to wrap */
        total    += a->data_size;
    }

    if (out_bytes_uploaded) *out_bytes_uploaded = total;
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* CONFIG_STREAM (opcode 0x03, PCIe variant)                                   */
/* -------------------------------------------------------------------------- */

/*
 * Wire-format structs matching what HailoRT's
 * control_protocol__pack_config_stream_base_request +
 * pack_config_stream_pcie_{input,output}_request produce.
 *
 * Every uint32_t `*_length` field is big-endian on the wire. Every
 * uint16_t / uint32_t scalar inside nn_stream_config is byte-
 * swapped with htons (including the two u32 fields — matches the
 * HailoRT quirk; firmware is authoritative). u8 fields and the
 * packed PCIe variant payloads are raw bytes.
 */
struct hailo_ns_nn_stream_config_wire {
    uint16_t core_bytes_per_buffer;
    uint16_t core_buffers_per_frame;
    uint16_t periph_bytes_per_buffer;
    uint32_t periph_buffers_per_frame;   /* htons on a u32 — see note */
    uint16_t feature_padding_payload;
    uint32_t buffer_padding_payload;     /* htons on a u32 — see note */
    uint16_t buffer_padding;
    bool     is_core_hw_padding_config_in_dfc;
} __attribute__((packed));

/* Fixed prefix of the CONFIG_STREAM request payload, common to
 * every transport variant. The `communication_params` bytes and
 * their length prefix follow this prefix; they differ by variant. */
struct hailo_ns_config_stream_prefix_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;            /* BE, = 7 */
    uint32_t stream_index_length;        /* BE, = 1 */
    uint8_t  stream_index;
    uint32_t is_input_length;            /* BE, = 1 */
    uint8_t  is_input;
    uint32_t communication_type_length;  /* BE, = 4 */
    uint32_t communication_type;         /* BE */
    uint32_t skip_nn_stream_config_length; /* BE, = 1 */
    uint8_t  skip_nn_stream_config;
    uint32_t nn_stream_config_length;    /* BE, = sizeof(nn) */
    struct hailo_ns_nn_stream_config_wire nn_stream_config;
    uint32_t communication_params_length; /* BE, = sizeof(variant) */
} __attribute__((packed));

struct hailo_ns_pcie_input_wire {
    uint8_t pcie_channel_index;
    uint8_t pcie_dataflow_type;
} __attribute__((packed));

/* HailoRT does NOT byteswap desc_page_size at pack time — the u16
 * rides the wire in native LE, matching the memcpy'd-raw
 * convention used for firmware_version. Keep the field native. */
struct hailo_ns_pcie_output_wire {
    uint8_t  pcie_channel_index;
    uint16_t desc_page_size;
} __attribute__((packed));

/* Full request = prefix + variant. Size depends on direction;
 * both variants fit in the same BSS buffer. Input variant is 2 B
 * so the 3 B output variant is the worst case. */
struct hailo_ns_config_stream_req_wire {
    struct hailo_ns_config_stream_prefix_wire prefix;
    /* Worst-case variant bytes (sizeof(output_wire) = 3). Pad one
     * byte for safe indexing via offsetof + sizeof. */
    uint8_t  variant[4];
} __attribute__((packed));

/*
 * Response: [response_header(24)] [parameter_count(4)]
 *           [dataflow_manager_id_length(4)] [dataflow_manager_id(1)]
 */
struct hailo_ns_config_stream_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;
    uint32_t dataflow_manager_id_length;
    uint8_t  dataflow_manager_id;
} __attribute__((packed));

static struct hailo_ns_config_stream_req_wire  control_config_stream_req;
static struct hailo_ns_config_stream_resp_wire control_config_stream_resp;

int hailo_control_config_stream_pcie(
    const struct hailo_stream_pcie_config *cfg,
    uint8_t *out_dataflow_manager_id)
{
    if (!cfg || !out_dataflow_manager_id) return HAILO_ERR_INVAL;
    /* Bool-as-int guard: firmware reads these as single bytes.
     * The C bool type is 0/1 by construction, so no clamping is
     * needed. We just forbid absurd pcie_channel_index values. */
    if (cfg->pcie_channel_index >= 16) return HAILO_ERR_INVAL;

    spin_lock(&control_lock);

    uint32_t variant_len;
    if (cfg->is_input) {
        variant_len = (uint32_t)sizeof(struct hailo_ns_pcie_input_wire);
    } else {
        variant_len = (uint32_t)sizeof(struct hailo_ns_pcie_output_wire);
    }

    /* Populate the shared prefix. */
    struct hailo_ns_config_stream_prefix_wire *p = &control_config_stream_req.prefix;
    p->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    p->common.flags    = 0;
    p->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    p->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_CONFIG_STREAM);
    p->parameter_count = hailo_cpu_to_be32(7u);

    p->stream_index_length   = hailo_cpu_to_be32(sizeof(p->stream_index));
    p->stream_index          = cfg->stream_index;

    p->is_input_length       = hailo_cpu_to_be32(sizeof(p->is_input));
    p->is_input              = cfg->is_input ? 1u : 0u;

    p->communication_type_length = hailo_cpu_to_be32(sizeof(p->communication_type));
    p->communication_type    = hailo_cpu_to_be32(HAILO_COMMUNICATION_TYPE_PCIE);

    p->skip_nn_stream_config_length = hailo_cpu_to_be32(sizeof(p->skip_nn_stream_config));
    p->skip_nn_stream_config = cfg->skip_nn_stream_config ? 1u : 0u;

    p->nn_stream_config_length = hailo_cpu_to_be32(sizeof(p->nn_stream_config));
    /* htons on every field, including the u32 members that HailoRT
     * also htons — see struct comment. */
    p->nn_stream_config.core_bytes_per_buffer    = __builtin_bswap16(cfg->nn_stream_config.core_bytes_per_buffer);
    p->nn_stream_config.core_buffers_per_frame   = __builtin_bswap16(cfg->nn_stream_config.core_buffers_per_frame);
    p->nn_stream_config.periph_bytes_per_buffer  = __builtin_bswap16(cfg->nn_stream_config.periph_bytes_per_buffer);
    p->nn_stream_config.periph_buffers_per_frame = __builtin_bswap16((uint16_t)cfg->nn_stream_config.periph_buffers_per_frame);
    p->nn_stream_config.feature_padding_payload  = __builtin_bswap16(cfg->nn_stream_config.feature_padding_payload);
    p->nn_stream_config.buffer_padding_payload   = __builtin_bswap16((uint16_t)cfg->nn_stream_config.buffer_padding_payload);
    p->nn_stream_config.buffer_padding           = __builtin_bswap16(cfg->nn_stream_config.buffer_padding);
    p->nn_stream_config.is_core_hw_padding_config_in_dfc
        = cfg->nn_stream_config.is_core_hw_padding_config_in_dfc ? 1u : 0u;

    p->communication_params_length = hailo_cpu_to_be32(variant_len);

    /* Write the variant bytes into control_config_stream_req.variant,
     * which begins immediately after the prefix on the wire. */
    if (cfg->is_input) {
        struct hailo_ns_pcie_input_wire v = {
            .pcie_channel_index = cfg->pcie_channel_index,
            .pcie_dataflow_type = cfg->pcie_dataflow_type,
        };
        memcpy(control_config_stream_req.variant, &v, sizeof(v));
    } else {
        struct hailo_ns_pcie_output_wire v = {
            .pcie_channel_index = cfg->pcie_channel_index,
            .desc_page_size     = cfg->desc_page_size, /* native LE */
        };
        memcpy(control_config_stream_req.variant, &v, sizeof(v));
    }

    uint32_t req_len = (uint32_t)(offsetof(struct hailo_ns_config_stream_req_wire,
                                           variant)
                                  + variant_len);

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_config_stream_req, req_len,
                                             &control_config_stream_resp,
                                             sizeof(control_config_stream_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_APP,
                                            &control_config_stream_req, req_len,
                                            &control_config_stream_resp,
                                            sizeof(control_config_stream_resp),
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    /* Copy the header out of the packed response struct before
     * checking, to dodge -Waddress-of-packed-member. */
    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_config_stream_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_CONFIG_STREAM,
                                       "CONFIG_STREAM");
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    if (resp_len < sizeof(control_config_stream_resp)) {
        WARN("hailo: CONFIG_STREAM response short (%u < %u)",
             resp_len, (unsigned)sizeof(control_config_stream_resp));
        spin_unlock(&control_lock);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    *out_dataflow_manager_id = control_config_stream_resp.dataflow_manager_id;
    spin_unlock(&control_lock);
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Context-switch: SET_NETWORK_GROUP_HEADER (opcode 0x20, CORE CPU).          */
/* -------------------------------------------------------------------------- */

/* Wire mirror of CONTROL_PROTOCOL__application_header_t for
 * firmware v4.23 (32 bytes). HailoRT's #pragma pack(1) applies to
 * the whole struct region, so bools ride as 1-byte and there's no
 * inter-field padding.
 *
 *   u16(2) + 3 bools(3) + bool(1) + u8(1) + 2×u16(4) + u32(4)
 *   + 3×u32(12) + u8(1) + 4×u8(4) = 32 bytes.
 *
 * Firmware rejects any other size with
 * CONTROL_PROTOCOL_STATUS_INVALID_CONTEXT_SWITCH_APP_HEADER_LENGTH
 * (major=0x40030060). The newer upstream reference header at
 * ~/slmos-ref/hailo/hailort-control-protocol.h:883-894 shows the 53-byte
 * layout (4 bools + 24 cfg channels) — that's a newer fw release,
 * not what pi-5-1 ships.
 */
struct hailo_cs_application_header_wire {
    uint16_t dynamic_contexts_count;       /* native LE */
    uint8_t  preliminary_run_asap;
    uint8_t  batch_register_config;
    uint8_t  can_fast_batch_switch;
    uint8_t  is_abbale_supported;
    uint8_t  networks_count;
    uint16_t csm_buffer_size;              /* native LE */
    uint16_t batch_size;                   /* native LE */
    uint32_t external_action_list_address; /* native LE */
    uint32_t boundary_channels_bitmap[HAILO_CS_MAX_VDMA_ENGINES]; /* native LE */
    uint8_t  config_channels_count;
    uint8_t  config_channel_packed_id[HAILO_CS_MAX_CFG_CHANNELS];
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_application_header_wire) == 32,
               "v4.23 application_header wire size must be 32 bytes");

struct hailo_cs_set_ngh_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;              /* BE, = 1 */
    uint32_t application_header_length;    /* BE, = 32 (v4.23) */
    struct hailo_cs_application_header_wire application_header;
} __attribute__((packed));

struct hailo_cs_set_ngh_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;              /* BE, = 0 */
} __attribute__((packed));

static struct hailo_cs_set_ngh_req_wire  control_set_ngh_req;
static struct hailo_cs_set_ngh_resp_wire control_set_ngh_resp;

int hailo_control_set_network_group_header(
    const struct hailo_cs_application_header *header)
{
    if (!header) return HAILO_ERR_INVAL;
    if (header->config_channels_count > HAILO_CS_MAX_CFG_CHANNELS) return HAILO_ERR_INVAL;

    spin_lock(&control_lock);

    struct hailo_cs_set_ngh_req_wire *r = &control_set_ngh_req;
    memset(r, 0, sizeof(*r));

    r->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->common.flags    = 0;
    r->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER);
    r->parameter_count = hailo_cpu_to_be32(1u);
    r->application_header_length =
        hailo_cpu_to_be32((uint32_t)sizeof(r->application_header));

    /* Field-for-field copy from host struct to packed wire struct.
     * Scalars stay native LE; only the outer length/parameter_count
     * prefix is BE (as above). */
    r->application_header.dynamic_contexts_count  = header->dynamic_contexts_count;
    r->application_header.preliminary_run_asap    = header->preliminary_run_asap     ? 1u : 0u;
    r->application_header.batch_register_config   = header->batch_register_config    ? 1u : 0u;
    r->application_header.can_fast_batch_switch   = header->can_fast_batch_switch    ? 1u : 0u;
    r->application_header.is_abbale_supported     = header->is_abbale_supported      ? 1u : 0u;
    r->application_header.networks_count          = header->networks_count;
    r->application_header.csm_buffer_size         = header->csm_buffer_size;
    r->application_header.batch_size              = header->batch_size;
    r->application_header.external_action_list_address =
        header->external_action_list_address;
    memcpy(r->application_header.boundary_channels_bitmap,
           header->boundary_channels_bitmap,
           sizeof(r->application_header.boundary_channels_bitmap));
    r->application_header.config_channels_count   = header->config_channels_count;
    memcpy(r->application_header.config_channel_packed_id,
           header->config_channel_packed_id,
           sizeof(r->application_header.config_channel_packed_id));

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_set_ngh_req, sizeof(*r),
                                             &control_set_ngh_resp,
                                             sizeof(control_set_ngh_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_CORE,
                                            &control_set_ngh_req, sizeof(*r),
                                            &control_set_ngh_resp,
                                            sizeof(control_set_ngh_resp),
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_set_ngh_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER,
                                       "SET_NETWORK_GROUP_HEADER");
    spin_unlock(&control_lock);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Context-switch: SET_CONTEXT_INFO (opcode 0x21, CORE CPU).                  */
/* -------------------------------------------------------------------------- */

/* Fixed prefix before context_network_data. All length fields are
 * BE on the wire; the u8 payload bytes they precede are 1-byte and
 * stored native. Per reference: ~/slmos-ref/hailo/hailort-control-protocol.h
 * lines 969-978 and -control_protocol.cpp:1162-1211. */
struct hailo_cs_set_ctx_info_req_prefix_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;                    /* BE, = 4 */
    uint32_t is_first_chunk_per_context_length;  /* BE, = 1 */
    uint8_t  is_first_chunk_per_context;
    uint32_t is_last_chunk_per_context_length;   /* BE, = 1 */
    uint8_t  is_last_chunk_per_context;
    uint32_t context_type_length;                /* BE, = 1 */
    uint8_t  context_type;
    uint32_t context_network_data_length;        /* BE, = N */
    /* context_network_data[N] follows here */
} __attribute__((packed));

/* 16 (common) + 4 (parameter_count) + 19 (four length+u8 pairs, where
 * the last length stands alone with data_length following as part of
 * the variable tail) = 39 bytes. */
_Static_assert(sizeof(struct hailo_cs_set_ctx_info_req_prefix_wire) == 39,
               "SET_CONTEXT_INFO prefix must be 39 bytes on the wire");

/* Full request buffer: fixed prefix + up to HAILO_CS_CONTEXT_CHUNK_MAX_BYTES
 * bytes of action data. Allocated in BSS; single request in flight
 * at a time (serialized by control_lock). */
struct hailo_cs_set_ctx_info_req_wire {
    struct hailo_cs_set_ctx_info_req_prefix_wire prefix;
    uint8_t  context_network_data[HAILO_CS_CONTEXT_CHUNK_MAX_BYTES];
} __attribute__((packed));

struct hailo_cs_set_ctx_info_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                    /* BE, = 0 */
} __attribute__((packed));

static struct hailo_cs_set_ctx_info_req_wire  control_set_ctx_info_req;
static struct hailo_cs_set_ctx_info_resp_wire control_set_ctx_info_resp;

int hailo_control_set_context_info_chunk(
    enum hailo_cs_context_type context_type,
    bool                       is_first_chunk,
    bool                       is_last_chunk,
    const void                *network_data,
    uint32_t                   network_data_len)
{
    if (network_data_len > HAILO_CS_CONTEXT_CHUNK_MAX_BYTES) return HAILO_ERR_INVAL;
    if (network_data_len > 0 && !network_data) return HAILO_ERR_INVAL;

    spin_lock(&control_lock);

    struct hailo_cs_set_ctx_info_req_wire *r = &control_set_ctx_info_req;
    memset(&r->prefix, 0, sizeof(r->prefix));

    r->prefix.common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->prefix.common.flags    = 0;
    r->prefix.common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->prefix.common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO);
    r->prefix.parameter_count = hailo_cpu_to_be32(4u);

    r->prefix.is_first_chunk_per_context_length =
        hailo_cpu_to_be32(sizeof(r->prefix.is_first_chunk_per_context));
    r->prefix.is_first_chunk_per_context = is_first_chunk ? 1u : 0u;

    r->prefix.is_last_chunk_per_context_length =
        hailo_cpu_to_be32(sizeof(r->prefix.is_last_chunk_per_context));
    r->prefix.is_last_chunk_per_context = is_last_chunk ? 1u : 0u;

    r->prefix.context_type_length = hailo_cpu_to_be32(sizeof(r->prefix.context_type));
    r->prefix.context_type        = (uint8_t)context_type;

    r->prefix.context_network_data_length = hailo_cpu_to_be32(network_data_len);
    if (network_data_len > 0) {
        memcpy(r->context_network_data, network_data, network_data_len);
    }

    uint32_t req_len = (uint32_t)(sizeof(r->prefix) + network_data_len);

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_set_ctx_info_req, req_len,
                                             &control_set_ctx_info_resp,
                                             sizeof(control_set_ctx_info_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        /* 10s timeout on SET_CONTEXT_INFO. Longer than other CORE-CPU
         * opcodes because firmware's CORE task processes each context's
         * action list asynchronously — the response comes back only
         * AFTER that work completes. On pi-5-1 fw v4.23 with a tight
         * 1s timeout, ACTIVATION returned rc=0 but the next
         * SET_CONTEXT_INFO (BATCH_SWITCHING) timed out because the
         * CORE task was still finalizing BURST_CREDITS_TASK_RESET
         * from ACTIVATION. A longer timeout lets the natural sequence
         * complete. Full MSI-driven response routing is the long-term
         * fix; this bump unblocks single-MLP loads today. */
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_CORE,
                                            &control_set_ctx_info_req, req_len,
                                            &control_set_ctx_info_resp,
                                            sizeof(control_set_ctx_info_resp),
                                            &resp_len,
                                            /* 10 s */ 10000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_set_ctx_info_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO,
                                       "SET_CONTEXT_INFO");
    spin_unlock(&control_lock);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Context-switch: CHANGE_CONTEXT_SWITCH_STATUS (opcode 0x25, CORE CPU).      */
/* -------------------------------------------------------------------------- */

/* Wire layout: parameter_count=4 with four length+value pairs.
 * state_machine_status (u8), application_index (u8),
 * dynamic_batch_size (u16, native LE), batch_count (u16, native LE). */
struct hailo_cs_change_status_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;                /* BE, = 4 */
    uint32_t state_machine_status_length;    /* BE, = 1 */
    uint8_t  state_machine_status;
    uint32_t application_index_length;       /* BE, = 1 */
    uint8_t  application_index;
    uint32_t dynamic_batch_size_length;      /* BE, = 2 */
    uint16_t dynamic_batch_size;             /* native LE */
    uint32_t batch_count_length;             /* BE, = 2 */
    uint16_t batch_count;                    /* native LE */
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_change_status_req_wire) == 42,
               "CHANGE_CONTEXT_SWITCH_STATUS wire must be 42 bytes");

struct hailo_cs_change_status_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE, = 0 */
} __attribute__((packed));

static struct hailo_cs_change_status_req_wire  control_change_status_req;
static struct hailo_cs_change_status_resp_wire control_change_status_resp;

int hailo_control_change_context_switch_status(
    enum hailo_cs_state state,
    uint8_t             application_index,
    uint16_t            dynamic_batch_size,
    uint16_t            batch_count)
{
    spin_lock(&control_lock);

    struct hailo_cs_change_status_req_wire *r = &control_change_status_req;
    memset(r, 0, sizeof(*r));

    r->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->common.flags    = 0;
    r->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS);
    r->parameter_count = hailo_cpu_to_be32(4u);

    r->state_machine_status_length = hailo_cpu_to_be32(sizeof(r->state_machine_status));
    r->state_machine_status        = (uint8_t)state;
    r->application_index_length    = hailo_cpu_to_be32(sizeof(r->application_index));
    r->application_index           = application_index;
    r->dynamic_batch_size_length   = hailo_cpu_to_be32(sizeof(r->dynamic_batch_size));
    r->dynamic_batch_size          = dynamic_batch_size;
    r->batch_count_length          = hailo_cpu_to_be32(sizeof(r->batch_count));
    r->batch_count                 = batch_count;

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_change_status_req, sizeof(*r),
                                             &control_change_status_resp,
                                             sizeof(control_change_status_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_CORE,
                                            &control_change_status_req, sizeof(*r),
                                            &control_change_status_resp,
                                            sizeof(control_change_status_resp),
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_change_status_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS,
                                       "CHANGE_CONTEXT_SWITCH_STATUS");
    spin_unlock(&control_lock);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Context-switch: CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS (0x47, CORE CPU).     */
/* GET_HW_CONSTS (0x48, CORE CPU).                                            */
/* -------------------------------------------------------------------------- */

/* Both are empty-body requests. Wire layout matches IDENTIFY:
 *   [common_header(16)][parameter_count=0(4)] = 20 bytes.
 * Responses: CLEAR has no body (just header+parameter_count=0),
 * GET_HW_CONSTS returns a hw_consts struct that SLM-OS doesn't
 * currently consume. */
struct hailo_cs_empty_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;                /* BE, = 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_empty_req_wire) == 20,
               "empty-body request wire must be 20 bytes");

struct hailo_cs_clear_apps_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE, = 0 */
} __attribute__((packed));

/* HailoRT's CONTROL_PROTOCOL__get_hw_consts_response_t packs a
 * handful of u32/u16 fields; observed 51 B on fw v4.23. The 128 B
 * cap is defense-in-depth — control_validate_send_recv_args already
 * rejects responses whose framed buffer_len exceeds the caller's
 * buffer, but a conservative ceiling avoids silent truncation if a
 * future fw revision grows the struct past what this buffer holds. */
struct hailo_cs_hw_consts_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE */
    uint8_t  body[128];
} __attribute__((packed));

/* CORE_IDENTIFY response: parameter_count=1, one param carrying
 * firmware_version_t = {major, minor, revision} as three u32s (12 B).
 * Framed wire: header(12) + parameter_count(4) + length(4) + body(12)
 * = 32 bytes. 64 B ceiling is defensive. */
struct hailo_cs_core_identify_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE */
    uint8_t  body[64];
} __attribute__((packed));

static struct hailo_cs_empty_req_wire        control_clear_apps_req;
static struct hailo_cs_clear_apps_resp_wire  control_clear_apps_resp;
static struct hailo_cs_empty_req_wire        control_hw_consts_req;
static struct hailo_cs_hw_consts_resp_wire   control_hw_consts_resp;
static struct hailo_cs_empty_req_wire        control_core_identify_req;
static struct hailo_cs_core_identify_resp_wire control_core_identify_resp;

/* Shared implementation for empty-body CORE-CPU RPCs (0x47, 0x48,
 * and any future `parameter_count=0`-only opcode). Caller owns the
 * request + response wire buffers; this helper handles header pack,
 * lock, send, response-header validation, and unlock. `resp_buf`
 * must begin with a `struct hailo_control_response_header`.
 * `out_resp_len` is optional; set to the received payload byte
 * count on success. */
static int control_send_empty_body_core_rpc(uint32_t opcode,
                                            const char *op_name,
                                            struct hailo_cs_empty_req_wire *req,
                                            void *resp_buf,
                                            size_t resp_buf_size,
                                            uint32_t *out_resp_len)
{
    spin_lock(&control_lock);

    memset(req, 0, sizeof(*req));
    req->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    req->common.flags    = 0;
    req->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    req->common.opcode   = hailo_cpu_to_be32(opcode);
    req->parameter_count = 0;

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(req, sizeof(*req),
                                             resp_buf, resp_buf_size,
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_CORE,
                                            req, sizeof(*req),
                                            resp_buf, resp_buf_size,
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, resp_buf, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len, opcode, op_name);
    if (rc == HAILO_OK && out_resp_len) {
        *out_resp_len = resp_len;
    }
    spin_unlock(&control_lock);
    return rc;
}

int hailo_control_context_switch_clear_configured_apps(void)
{
    return control_send_empty_body_core_rpc(
        HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS,
        "CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS",
        &control_clear_apps_req,
        &control_clear_apps_resp,
        sizeof(control_clear_apps_resp),
        NULL);
}

int hailo_control_get_hw_consts(uint32_t *out_response_len)
{
    return control_send_empty_body_core_rpc(
        HAILO_CONTROL_OPCODE_GET_HW_CONSTS,
        "GET_HW_CONSTS",
        &control_hw_consts_req,
        &control_hw_consts_resp,
        sizeof(control_hw_consts_resp),
        out_response_len);
}

/* Accessor for the most recent GET_HW_CONSTS response body. Lets a
 * caller dump or decode the response *after* its timing window has
 * closed — the previous in-function dump (5 lines × ~63 chars at
 * 115200 baud) added ~21 ms to any externally-bracketed measurement,
 * which masked the real RPC latency. The static BSS buffer here is
 * single-writer (control_lock-serialized inside
 * hailo_control_get_hw_consts) and stable until the next
 * GET_HW_CONSTS call. */
void hailo_control_get_hw_consts_response_body(const uint8_t **out_body,
                                               uint32_t       *out_capacity)
{
    if (out_body)     *out_body     = control_hw_consts_resp.body;
    if (out_capacity) *out_capacity = (uint32_t)sizeof(control_hw_consts_resp.body);
}

int hailo_control_core_identify(uint32_t *out_response_len)
{
    return control_send_empty_body_core_rpc(
        HAILO_CONTROL_OPCODE_CORE_IDENTIFY,
        "CORE_IDENTIFY",
        &control_core_identify_req,
        &control_core_identify_resp,
        sizeof(control_core_identify_resp),
        out_response_len);
}

/* GET_DEVICE_INFORMATION (opcode 0x33, APP_CPU). Empty-body
 * request, returns a ~143-byte device info struct on fw v4.23.
 * SLM-OS does not currently parse the body — the RPC is used
 * purely as a HailoRT-style fw-settled handshake between load
 * steps. See hailo_control.h comment for the #253 motivation. */
struct hailo_cs_device_info_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE */
    uint8_t  body[256];                      /* observed 143 B; 256 is defensive */
} __attribute__((packed));

static struct hailo_cs_empty_req_wire        control_device_info_req;
static struct hailo_cs_device_info_resp_wire control_device_info_resp;

int hailo_control_get_device_information(uint32_t *out_response_len)
{
    spin_lock(&control_lock);

    struct hailo_cs_empty_req_wire *r = &control_device_info_req;
    memset(r, 0, sizeof(*r));
    r->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->common.flags    = 0;
    r->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_GET_DEVICE_INFORMATION);
    r->parameter_count = 0;

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(r, sizeof(*r),
                                             &control_device_info_resp,
                                             sizeof(control_device_info_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_APP,
                                            r, sizeof(*r),
                                            &control_device_info_resp,
                                            sizeof(control_device_info_resp),
                                            &resp_len,
                                            /* 1 s */ 1000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_device_info_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_GET_DEVICE_INFORMATION,
                                       "GET_DEVICE_INFORMATION");
    if (rc == HAILO_OK && out_response_len) *out_response_len = resp_len;
    spin_unlock(&control_lock);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* RUN_BIST_TEST (opcode 0x3C, APP CPU). 5-parameter request.                 */
/* -------------------------------------------------------------------------- */

/* Wire layout matches CONTROL_PROTOCOL__pack_run_bist_test_request:
 *   parameter_count = 5
 *   is_top_test_length BE u32 (=1) + is_top_test u8
 *   top_bypass_bitmap_length BE u32 (=4) + top_bypass_bitmap BE u32
 *   cluster_index_length BE u32 (=1) + cluster_index u8
 *   cluster_bypass_bitmap_0_length BE u32 (=4) + cluster_bypass_bitmap_0 BE u32
 *   cluster_bypass_bitmap_1_length BE u32 (=4) + cluster_bypass_bitmap_1 BE u32
 * Total wire body after common_header: 4 + 5 + 8 + 5 + 8 + 8 = 38 bytes. */
struct hailo_cs_run_bist_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;                /* BE, = 5 */
    uint32_t is_top_test_length;             /* BE, = 1 */
    uint8_t  is_top_test;
    uint32_t top_bypass_bitmap_length;       /* BE, = 4 */
    uint32_t top_bypass_bitmap;              /* BE u32 */
    uint32_t cluster_index_length;           /* BE, = 1 */
    uint8_t  cluster_index;
    uint32_t cluster_bypass_bitmap_0_length; /* BE, = 4 */
    uint32_t cluster_bypass_bitmap_0;        /* BE u32 */
    uint32_t cluster_bypass_bitmap_1_length; /* BE, = 4 */
    uint32_t cluster_bypass_bitmap_1;        /* BE u32 */
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_run_bist_req_wire) == 54,
               "RUN_BIST_TEST request wire must be 54 bytes "
               "(16 hdr + 4 pcount + 38 body)");

/* Response layout: header + parameter_count + opaque body. Public docs
 * don't enumerate the response struct; the caller's job is to dump
 * whatever bytes come back so we can pattern-match against the BIST
 * top/cluster enums. */
struct hailo_cs_run_bist_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                /* BE */
    uint8_t  body[256];
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_run_bist_resp_wire) ==
                   sizeof(struct hailo_control_response_header) + 4 + 256,
               "RUN_BIST_TEST response wire layout drifted — check "
               "header struct + parameter_count + body sizing");

static struct hailo_cs_run_bist_req_wire  control_run_bist_req;
static struct hailo_cs_run_bist_resp_wire control_run_bist_resp;

int hailo_control_run_bist_test(bool     is_top_test,
                                uint32_t top_bypass_bitmap,
                                uint8_t  cluster_index,
                                uint32_t cluster_bypass_bitmap_0,
                                uint32_t cluster_bypass_bitmap_1,
                                uint8_t *out_resp,
                                uint32_t out_resp_cap,
                                uint32_t *out_resp_len)
{
    spin_lock(&control_lock);

    struct hailo_cs_run_bist_req_wire *r = &control_run_bist_req;
    memset(r, 0, sizeof(*r));

    r->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->common.flags    = 0;
    r->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_RUN_BIST_TEST);
    r->parameter_count = hailo_cpu_to_be32(5u);

    r->is_top_test_length             = hailo_cpu_to_be32(sizeof(r->is_top_test));
    r->is_top_test                    = is_top_test ? 1u : 0u;
    r->top_bypass_bitmap_length       = hailo_cpu_to_be32(sizeof(r->top_bypass_bitmap));
    r->top_bypass_bitmap              = hailo_cpu_to_be32(top_bypass_bitmap);
    r->cluster_index_length           = hailo_cpu_to_be32(sizeof(r->cluster_index));
    r->cluster_index                  = cluster_index;
    r->cluster_bypass_bitmap_0_length = hailo_cpu_to_be32(sizeof(r->cluster_bypass_bitmap_0));
    r->cluster_bypass_bitmap_0        = hailo_cpu_to_be32(cluster_bypass_bitmap_0);
    r->cluster_bypass_bitmap_1_length = hailo_cpu_to_be32(sizeof(r->cluster_bypass_bitmap_1));
    r->cluster_bypass_bitmap_1        = hailo_cpu_to_be32(cluster_bypass_bitmap_1);

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(r, sizeof(*r),
                                             &control_run_bist_resp,
                                             sizeof(control_run_bist_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        /* BIST is genuinely slow — fw scribbles patterns into memory
         * and reads them back. Give it 5 s; fw normally completes in
         * well under that. */
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_APP,
                                            r, sizeof(*r),
                                            &control_run_bist_resp,
                                            sizeof(control_run_bist_resp),
                                            &resp_len,
                                            /* 5 s */ 5000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_run_bist_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_RUN_BIST_TEST,
                                       "RUN_BIST_TEST");
    if (rc == HAILO_OK) {
        /* Copy out the post-header body bytes for the caller to dump. */
        if (out_resp && out_resp_cap > 0) {
            uint32_t body_off = (uint32_t)sizeof(struct hailo_control_response_header);
            uint32_t body_len = (resp_len > body_off) ? (resp_len - body_off) : 0;
            if (body_len > out_resp_cap) body_len = out_resp_cap;
            if (body_len > 0) {
                memcpy(out_resp,
                       (const uint8_t *)&control_run_bist_resp + body_off,
                       body_len);
            }
            if (out_resp_len) *out_resp_len = body_len;
        } else if (out_resp_len) {
            *out_resp_len = 0;
        }
    }
    spin_unlock(&control_lock);
    return rc;
}

int hailo_control_set_context_info(
    enum hailo_cs_context_type context_type,
    const void                *network_data,
    uint32_t                   network_data_len)
{
    if (network_data_len > 0 && !network_data) return HAILO_ERR_INVAL;

    /* Zero-length context: single chunk with is_first=is_last=true
     * and empty payload. Firmware interprets this as a context with
     * no actions — legal but rare. */
    if (network_data_len == 0) {
        return hailo_control_set_context_info_chunk(
            context_type, /*is_first=*/true, /*is_last=*/true, NULL, 0);
    }

    const uint8_t *p = (const uint8_t *)network_data;
    uint32_t remaining = network_data_len;
    bool is_first = true;

    while (remaining > 0) {
        uint32_t chunk = (remaining > HAILO_CS_CONTEXT_CHUNK_MAX_BYTES)
                             ? HAILO_CS_CONTEXT_CHUNK_MAX_BYTES
                             : remaining;
        bool is_last = (chunk == remaining);
        int rc = hailo_control_set_context_info_chunk(
            context_type, is_first, is_last, p, chunk);
        if (rc != HAILO_OK) return rc;
        p         += chunk;
        remaining -= chunk;
        is_first   = false;
    }
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* CHANGE_HW_INFER_STATUS (opcode 0x4A, CORE CPU). #682 hyp-A.                */
/*                                                                            */
/* Gated behind HAILO_HYP_A_RPC. hyp-A was disconfirmed (2026-05-08) and      */
/* the RPC has zero in-tree callers; the wire layout + _Static_asserts are    */
/* preserved here as a build-flag-enabled reference for any future fw rev    */
/* that may honour it. See header for the full investigation history.        */
/* -------------------------------------------------------------------------- */

#ifdef HAILO_HYP_A_RPC

/* Per-channel info entry. Mirrors HailoRT's
 * CONTROL_PROTOCOL__hw_infer_channel_info_t (4 bytes packed). */
struct hailo_hw_infer_channel_info {
    uint8_t  channel_index;
    uint8_t  engine_index;
    uint16_t desc_programed;            /* native LE */
} __attribute__((packed));

/* HailoRT's CONTROL_PROTOCOL__MAX_TOTAL_CHANNEL_COUNT
 * = MAX_VDMA_CHANNELS_PER_ENGINE(40) * MAX_VDMA_ENGINES_COUNT(3) = 120. */
#define HAILO_HW_INFER_MAX_TOTAL_CHANNEL_COUNT 120u

/* Mirrors hw_infer_channels_info_t: array of 120 channel_info
 * (4 B each = 480 B) + 1-byte channel_count = 481 B total. */
struct hailo_hw_infer_channels_info {
    struct hailo_hw_infer_channel_info channel_info[HAILO_HW_INFER_MAX_TOTAL_CHANNEL_COUNT];
    uint8_t channel_count;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_hw_infer_channels_info) == 481,
               "hw_infer_channels_info wire size must be 481 bytes");

/* Wire layout: parameter_count=6 with six length+value pairs.
 *   1: hw_infer_state (u8)
 *   2: application_index (u8)
 *   3: dynamic_batch_size (u16, native LE)
 *   4: batch_count (u16, native LE)
 *   5: channels_info (481 B blob, native LE per-field)
 *   6: boundary_channel_mode (u8)
 */
struct hailo_cs_change_hw_infer_status_req_wire {
    struct hailo_control_common_header common;
    uint32_t parameter_count;                    /* BE, = 6 */
    uint32_t hw_infer_state_length;              /* BE, = 1 */
    uint8_t  hw_infer_state;
    uint32_t application_index_length;           /* BE, = 1 */
    uint8_t  application_index;
    uint32_t dynamic_batch_size_length;          /* BE, = 2 */
    uint16_t dynamic_batch_size;                 /* native LE */
    uint32_t batch_count_length;                 /* BE, = 2 */
    uint16_t batch_count;                        /* native LE */
    uint32_t channels_info_length;               /* BE, = 481 */
    struct hailo_hw_infer_channels_info channels_info;
    uint32_t boundary_channel_mode_length;       /* BE, = 1 */
    uint8_t  boundary_channel_mode;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_change_hw_infer_status_req_wire) ==
               16 /* common */ + 4 /* param_count */ +
               (4+1) + (4+1) + (4+2) + (4+2) + (4+481) + (4+1),
               "CHANGE_HW_INFER_STATUS wire size mismatch");

/* Response: parameter_count=1 carrying hw_only_infer_results_t
 * (bool infer_done + uint32_t infer_cycles, 5 B packed). The
 * 32 B ceiling is defensive — header(12) + parameter_count(4) +
 * length(4) + body(5) = 25 bytes, leaving slack for any framing
 * variation. */
struct hailo_cs_change_hw_infer_status_resp_wire {
    struct hailo_control_response_header header;
    uint32_t parameter_count;                    /* BE */
    uint8_t  body[32];
} __attribute__((packed));

static struct hailo_cs_change_hw_infer_status_req_wire   control_change_hw_infer_req;
static struct hailo_cs_change_hw_infer_status_resp_wire  control_change_hw_infer_resp;

int hailo_control_change_hw_infer_status(
    enum hailo_hw_infer_state         state,
    uint8_t                           app_idx,
    uint16_t                          dynamic_batch_size,
    uint16_t                          batch_count,
    enum hailo_boundary_channel_mode  boundary_mode)
{
    spin_lock(&control_lock);

    struct hailo_cs_change_hw_infer_status_req_wire *r = &control_change_hw_infer_req;
    memset(r, 0, sizeof(*r));

    r->common.version  = hailo_cpu_to_be32(HAILO_CONTROL_PROTOCOL_VERSION);
    r->common.flags    = 0;
    r->common.sequence = hailo_cpu_to_be32(control_next_sequence());
    r->common.opcode   = hailo_cpu_to_be32(HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS);
    r->parameter_count = hailo_cpu_to_be32(6u);

    r->hw_infer_state_length        = hailo_cpu_to_be32(sizeof(r->hw_infer_state));
    r->hw_infer_state               = (uint8_t)state;
    r->application_index_length     = hailo_cpu_to_be32(sizeof(r->application_index));
    r->application_index            = app_idx;
    r->dynamic_batch_size_length    = hailo_cpu_to_be32(sizeof(r->dynamic_batch_size));
    r->dynamic_batch_size           = dynamic_batch_size;
    r->batch_count_length           = hailo_cpu_to_be32(sizeof(r->batch_count));
    r->batch_count                  = batch_count;
    r->channels_info_length         = hailo_cpu_to_be32(sizeof(r->channels_info));
    /* channels_info already zeroed by memset above — channel_count=0,
     * all channel_info entries zero. Whether fw accepts that is part
     * of the experimental signal. */
    r->boundary_channel_mode_length = hailo_cpu_to_be32(sizeof(r->boundary_channel_mode));
    r->boundary_channel_mode        = (uint8_t)boundary_mode;

    uint32_t resp_len = 0;
    int rc = control_validate_send_recv_args(&control_change_hw_infer_req, sizeof(*r),
                                             &control_change_hw_infer_resp,
                                             sizeof(control_change_hw_infer_resp),
                                             &resp_len);
    if (rc == HAILO_OK) {
        rc = hailo_control_send_recv_locked(HAILO_CTRL_CPU_CORE,
                                            &control_change_hw_infer_req, sizeof(*r),
                                            &control_change_hw_infer_resp,
                                            sizeof(control_change_hw_infer_resp),
                                            &resp_len,
                                            /* 5 s — generous: HW-only START
                                             * may run inference before
                                             * acking */ 5000000u);
    }
    if (rc != HAILO_OK) {
        spin_unlock(&control_lock);
        return rc;
    }

    struct hailo_control_response_header hdr_copy;
    memcpy(&hdr_copy, &control_change_hw_infer_resp.header, sizeof(hdr_copy));
    rc = control_check_response_header(&hdr_copy, resp_len,
                                       HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS,
                                       "CHANGE_HW_INFER_STATUS");
    spin_unlock(&control_lock);
    return rc;
}

#endif /* HAILO_HYP_A_RPC */
