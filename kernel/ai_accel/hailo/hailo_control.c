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
#include "debug.h"
#include "md5.h"
#include "spinlock.h"
#include <stddef.h>
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

/*
 * BSS footprint: two large static wire buffers (req ~1520 B + resp
 * 1500 B ≈ 3 KB total). Chosen over stack allocation because the
 * 16 KB kernel stack can't comfortably carry 3 KB of transient
 * scratch on every control call — boot-path callers already use a
 * big chunk of it. Chosen over heap allocation because this file
 * runs on Pi 5 only and the simpler static layout is easier to
 * audit. Only reachable post-boot once firmware is running.
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
static int hailo_control_send_recv_locked(const void *req_payload,
                                          uint32_t    req_len,
                                          void       *resp_payload,
                                          uint32_t    resp_capacity,
                                          uint32_t   *resp_len,
                                          uint32_t    timeout_us)
{
    /* Unmask interrupts (once) and ensure ATR[0] routes BAR4[0..]
     * and BAR4[0x640..] to the firmware's request / response
     * buffers. */
    control_arm_interrupts();
    control_retarget_atr0();

    size_t wire_len = build_request_wire(control_req_wire, req_payload, req_len);

    /* Write request to BAR4 at offset 0. Firmware has ATR[0]
     * configured to land this in its request buffer. dword-
     * aligned length required by the platform shim's bar4_write. */
    size_t aligned_len = (wire_len + 3u) & ~(size_t)3u;
    hailo_platform->bar4_write(0, control_req_wire, aligned_len);
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

    /* TODO: wait_for_response is a udelay-polled busy wait. For
     * #281 tier-1 (shell-driven IDENTIFY) this is fine — the lone
     * caller on CPU 0 just waits. For Phase 5.3+ inference submit,
     * this should either yield() between polls or route through
     * the future MSI path so CPU 0 isn't burned for up to a full
     * timeout_us. */
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
    int rc = control_validate_send_recv_args(req_payload, req_len,
                                             resp_payload, resp_capacity,
                                             resp_len);
    if (rc != HAILO_OK) return rc;

    spin_lock(&control_lock);
    rc = hailo_control_send_recv_locked(req_payload, req_len,
                                        resp_payload, resp_capacity,
                                        resp_len, timeout_us);
    spin_unlock(&control_lock);
    return rc;
}

void hailo_control_reset_state_for_tests(void)
{
    __atomic_store_n(&control_sequence, 0, __ATOMIC_RELAXED);
    control_irq_armed = false;
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
        rc = hailo_control_send_recv_locked(&control_mem_write_req, req_len,
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
        rc = hailo_control_send_recv_locked(&control_mem_read_req,
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
        rc = hailo_control_send_recv_locked(&control_config_stream_req, req_len,
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
