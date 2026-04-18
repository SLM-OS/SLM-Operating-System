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
 *   BCS_ISTATUS_HOST on BAR0; the read auto-clears the register, so
 *   any non-zero SW_IRQ bits mean "something is ready". Hailo's
 *   firmware sets distinct SW_IRQ bits per source; we treat any
 *   non-zero as "control response ready" because the only request
 *   in flight at a time is ours. A future MSI path can differentiate.
 */

#include "hailo.h"
#include "hailo_control.h"
#include "hailo_internal.h"
#include "debug.h"
#include "md5.h"
#include <string.h>

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
 * what we sent. Incremented per send. */
static uint32_t control_sequence = 0;

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
 * Poll BCS_ISTATUS_HOST for the firmware-control SW IRQ bit
 * (HAILO_PCIE_NNC_FW_CONTROL_IRQ shifted into the SW_IRQ field),
 * yielding (via udelay) between reads. Returns HAILO_OK as soon as
 * that specific bit appears, or HAILO_ERR_TIMEOUT after
 * `timeout_us` elapsed without it. Other sources (notifications,
 * VDMA transfers) get cleared as they arrive so they don't hide
 * the one we're waiting for, but are otherwise ignored — a future
 * MSI path can route them separately.
 */
static int wait_for_response(uint32_t timeout_us)
{
    const uint32_t poll_interval_us = 100;
    uint32_t elapsed = 0;

    while (elapsed < timeout_us) {
        uint32_t istatus = hailo_platform->read32(
            HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST);
        if (istatus != 0) {
            /* Write-1-to-clear whichever bits fired. */
            hailo_platform->write32(
                HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST, istatus);
            if (istatus & HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT) {
                return HAILO_OK;
            }
            /* Non-control source fired — keep polling for ours. */
        }
        hailo_platform->udelay(poll_interval_us);
        elapsed += poll_interval_us;
    }
    return HAILO_ERR_TIMEOUT;
}

/*
 * Point ATR[0] at the Hailo-8 control-request section (device
 * address 0x60000000). The firmware sets this up post-boot on a
 * "clean" driver path (Linux polls for it in
 * hailo_pcie_is_firmware_loaded); our boot path uses the older
 * ATR[1] magic and may leave ATR[0] pointing at the last firmware
 * upload window (core_fw_header at 0xA0000). Force the correct
 * value before every control-channel op. Idempotent — if firmware
 * already set it, this is a no-op.
 */
static void control_retarget_atr0(void)
{
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
}

/*
 * One-shot interrupt setup. The firmware won't latch bits into
 * BCS_ISTATUS_HOST until the matching IMASK bits are enabled —
 * hailo_pcie_enable_interrupts in the Linux driver does exactly
 * this before any fw_control round-trip. Safe to call every
 * request (the mask OR-in is idempotent) but only the first call
 * matters on real hardware. Also clears any stale bits in the
 * ISTATUS / per-channel counter registers so we start each
 * request from a known-quiet state.
 */
static bool control_irq_armed = false;
static void control_arm_interrupts(void)
{
    if (control_irq_armed) return;
    uint32_t mask = hailo_platform->read32(HAILO_BAR_CONFIG,
                                           HAILO_BSC_IMASK_HOST);
    mask |= HAILO_BSC_ISTATUS_HOST_MASK;
    hailo_platform->write32(HAILO_BAR_CONFIG, HAILO_BSC_IMASK_HOST, mask);
    hailo_platform->write32(HAILO_BAR_CONFIG, HAILO_BCS_ISTATUS_HOST,
                            0xFFFFFFFFu);
    hailo_platform->mb();
    control_irq_armed = true;
}

int hailo_control_send_recv(const void *req_payload,
                            uint32_t    req_len,
                            void       *resp_payload,
                            uint32_t    resp_capacity,
                            uint32_t   *resp_len,
                            uint32_t    timeout_us)
{
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

    /* Unmask interrupts (once) and ensure ATR[0] routes BAR4[0..]
     * and BAR4[0x640..] to the firmware's request / response
     * buffers. */
    control_arm_interrupts();
    control_retarget_atr0();

    /*
     * Assemble the wire bytes on the stack. The largest Hailo-8
     * control request the kernel will ever send is ~128 B (CONFIG
     * CONTEXT SWITCH), so the MAX_BUFFER_LENGTH-sized buffer below
     * is generous but not remotely close to the 16 KB stack cap.
     */
    static uint8_t req_wire[sizeof(struct hailo_control_wire_hdr)
                          + HAILO_CONTROL_MAX_BUFFER_LENGTH];
    size_t wire_len = build_request_wire(req_wire, req_payload, req_len);

    /* Write request to BAR4 at offset 0. Firmware has ATR[0]
     * configured to land this in its request buffer. dword-
     * aligned length required by the platform shim's bar4_write. */
    size_t aligned_len = (wire_len + 3u) & ~(size_t)3u;
    hailo_platform->bar4_write(0, req_wire, aligned_len);
    hailo_platform->mb();

    /* Ring the doorbell: APP CPU control. raise_ready_offset
     * (0x1684 on Hailo-8) is a direct BAR4 offset — the Linux
     * driver writes to it via `resources->fw_access` which is
     * BAR4, and that's how the firmware picks up the "request
     * ready" event. */
    uint32_t doorbell_val = HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK;
    hailo_platform->bar4_write(hailo_fw_addrs_hailo8.raise_ready_offset,
                               &doorbell_val, sizeof(doorbell_val));
    hailo_platform->mb();

    int rc = wait_for_response(timeout_us);
    if (rc != HAILO_OK) return rc;

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
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* Read the response payload. */
    static uint8_t resp_wire[HAILO_CONTROL_MAX_BUFFER_LENGTH];
    uint32_t read_len = resp_hdr.buffer_len;
    size_t aligned_read = (read_len + 3u) & ~(size_t)3u;
    hailo_platform->bar4_read(
        HAILO_CONTROL_REQUEST_RESPONSE_OFFSET + sizeof(resp_hdr),
        resp_wire, aligned_read);

    /* Verify response MD5 is computed over the payload bytes only
     * (same pattern as the request side; HailoRT's check in
     * VdmaDevice::fw_interact_impl hashes response_buffer alone). */
    uint8_t check[MD5_DIGEST_LENGTH];
    md5_compute(resp_wire, read_len, check);
    if (memcmp(check, resp_hdr.md5, MD5_DIGEST_LENGTH) != 0) {
        WARN("hailo: control response MD5 mismatch");
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* Copy into caller's buffer (capped at resp_capacity). */
    uint32_t copy_len = read_len < resp_capacity ? read_len : resp_capacity;
    memcpy(resp_payload, resp_wire, copy_len);
    *resp_len = copy_len;
    return HAILO_OK;
}

void hailo_control_reset_state_for_tests(void)
{
    control_sequence  = 0;
    control_irq_armed = false;
}

int hailo_control_identify(struct hailo_control_identify_response *out)
{
    if (!out) return HAILO_ERR_INVAL;

    uint32_t sequence = control_sequence++;

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
