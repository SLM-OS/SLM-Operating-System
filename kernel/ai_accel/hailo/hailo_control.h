/*
 * hailo_control.h — Hailo firmware control-channel wire format.
 *
 * Ported from HailoRT's common/include/control_protocol.h (MIT
 * license; see docs/reference/hailort-control-protocol.h). We
 * carry only the subset the kernel actually sends today: IDENTIFY
 * for version-probe (#281 tier-1 milestone), WRITE/READ_MEMORY
 * and CONFIG_STREAM / OPEN_STREAM for the Phase 5.2 CCW streaming
 * follow-up. The full opcode table is in the reference file.
 *
 * Wire model:
 *
 *   driver -> fw: BAR4[0..len] = [md5(payload)(16B)] [buffer_len(4B)]
 *                               [payload: common_header + opcode +
 *                                         parameter_count + params]
 *                 BAR4[raise_ready_offset] = FW_ACCESS_APP_CPU_CONTROL_MASK
 *
 *   fw -> driver: BAR4[0x640..0x640+16] = response md5
 *                 BAR4[0x640+16..] = buffer_len + response payload
 *                 BCS_ISTATUS_HOST (BAR0 offset 0x018C) SW_IRQ bit set
 *
 * Each request/response is stamped with an MD5 of its payload bytes
 * (excluding the md5 header itself). Firmware verifies on receive
 * and stamps on transmit. The kernel mirrors what HailoRT does.
 */

#ifndef AI_ACCEL_HAILO_CONTROL_H
#define AI_ACCEL_HAILO_CONTROL_H

#include "hailo.h"
#include "md5.h"

/* Control-channel constants — mirror HailoRT control_protocol.h. */
#define HAILO_CONTROL_MAX_BUFFER_LENGTH        1500u
#define HAILO_CONTROL_REQUEST_RESPONSE_OFFSET  0x640u  /* BAR4 offset */
#define HAILO_CONTROL_PROTOCOL_VERSION         2u

/*
 * Device-side address the firmware expects ATR[0] to point at
 * post-boot for Hailo-8's control request/response region. Matches
 * Linux's PCIE_CONTROL_SECTION_ADDRESS_H8 in hailo-pcie-common.c.
 * hailo_pcie_is_firmware_loaded polls ATR[0].trsl_addr_lo for this
 * value; our boot path uses the older ATR[1] magic and may leave
 * ATR[0] pointing at the last firmware-upload page, so we
 * explicitly retarget to this value at the start of every control
 * operation.
 */
#define HAILO_CONTROL_SECTION_ADDR_H8  0x60000000u
#define HAILO_CONTROL_MAX_BOARD_NAME_LENGTH    32u
#define HAILO_CONTROL_MAX_SERIAL_NUMBER_LENGTH 16u
#define HAILO_CONTROL_MAX_PART_NUMBER_LENGTH   16u
#define HAILO_CONTROL_MAX_PRODUCT_NAME_LENGTH  42u

/*
 * Doorbell masks written to raise_ready_offset on BAR4. Bit 0
 * triggers the APP CPU (CPU 0) control handler; bit 1 triggers
 * the CORE CPU (CPU 1) handler. All our requests go to APP CPU.
 */
#define HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK  (1u << 0)
#define HAILO_FW_ACCESS_CORE_CPU_CONTROL_MASK (1u << 1)

/*
 * BAR0 offsets for the host-side interrupt-status register.
 * Bits 24..31 encode per-source SW IRQs shifted up from the
 * `hailo_pcie_nnc_sw_interrupt_masks` enum; bits 0..15 are VDMA
 * channel interrupts. The control-response "done" bit is
 * FW_CONTROL_IRQ (0x04) in the SW field — waiting on any non-zero
 * bit is wrong because firmware notifications and VDMA transfers
 * fire other bits and would race ahead of our request. Write-1-
 * to-clear semantics per `read_and_clear_reg` in hailo-pcie-common.c.
 */
#define HAILO_BCS_ISTATUS_HOST                   0x018Cu
#define HAILO_BSC_IMASK_HOST                     0x0188u
#define HAILO_BCS_ISTATUS_HOST_SW_IRQ_MASK       0xFF000000u
#define HAILO_BCS_ISTATUS_HOST_SW_IRQ_SHIFT      24u
#define HAILO_BCS_ISTATUS_HOST_VDMA_SRC_MASK     0x000000FFu
#define HAILO_BCS_ISTATUS_HOST_VDMA_DEST_MASK    0x0000FF00u
#define HAILO_BSC_ISTATUS_HOST_MASK                              \
    (HAILO_BCS_ISTATUS_HOST_SW_IRQ_MASK |                        \
     HAILO_BCS_ISTATUS_HOST_VDMA_SRC_MASK |                      \
     HAILO_BCS_ISTATUS_HOST_VDMA_DEST_MASK)
#define HAILO_PCIE_NNC_FW_CONTROL_IRQ            0x04u
#define HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT    \
    (HAILO_PCIE_NNC_FW_CONTROL_IRQ << HAILO_BCS_ISTATUS_HOST_SW_IRQ_SHIFT)

/* Subset of HailoRT's HAILO_CONTROL_OPCODE_*. Add more as the
 * kernel learns to send them. */
enum hailo_control_opcode {
    HAILO_CONTROL_OPCODE_IDENTIFY       = 0x00,
    /* HAILO_CONTROL_OPCODE_WRITE_MEMORY = 0x01, (Phase 5.2 — CCW) */
    /* HAILO_CONTROL_OPCODE_READ_MEMORY  = 0x02, (Phase 5.2)       */
    /* HAILO_CONTROL_OPCODE_CONFIG_STREAM = 0x03, (Phase 5.2)      */
    /* Full table in docs/reference/hailort-control-protocol.h.    */
};

/*
 * Common header shared by request and response. Byte-level layout
 * is fixed by the firmware (HailoRT uses #pragma pack(push, 1));
 * we use explicit uint32_ts and static_assert the total size.
 */
struct hailo_control_common_header {
    uint32_t version;    /* always HAILO_CONTROL_PROTOCOL_VERSION */
    uint32_t flags;      /* bit 0 = ack; rest reserved / must be 0 */
    uint32_t sequence;   /* driver-chosen, increments per request */
    uint32_t opcode;     /* enum hailo_control_opcode */
};
_Static_assert(sizeof(struct hailo_control_common_header) == 16,
               "control header must be 16 bytes on the wire");

struct hailo_control_request_header {
    struct hailo_control_common_header common;
};

struct hailo_control_response_status {
    uint32_t major_status;
    uint32_t minor_status;
};

struct hailo_control_response_header {
    struct hailo_control_common_header common;
    struct hailo_control_response_status status;
};
_Static_assert(sizeof(struct hailo_control_response_header) == 24,
               "control response header must be 24 bytes on the wire");

/*
 * Wire-format wrapper prepended to every payload before it goes on
 * BAR4: md5(payload) + buffer_len + payload. Matches the layout
 * that hailo_pcie_write_firmware_control writes (request_size =
 * sizeof(md5) + sizeof(buffer_len) + buffer_len).
 */
struct hailo_control_wire_hdr {
    uint8_t  md5[MD5_DIGEST_LENGTH];
    uint32_t buffer_len;
    /* payload bytes follow */
};

/*
 * IDENTIFY response body. Each field's length is echoed alongside
 * the field itself — HailoRT uses these as a runtime check; we
 * rely only on fw_version for our version-probe.
 */
struct hailo_control_firmware_version {
    uint32_t major;
    uint32_t minor;
    uint32_t revision;
};
_Static_assert(sizeof(struct hailo_control_firmware_version) == 12,
               "firmware_version is 12 bytes on the wire");

/*
 * __packed is load-bearing: product_number[42] is not 4-byte
 * aligned, so without packing the compiler tacks on 2 bytes of
 * trailing padding to align any hypothetical follow-up uint32_t
 * field. That pads sizeof() from the 162 bytes firmware actually
 * sends up to 164, and the response-length check rejects the
 * real response as truncated.
 */
struct hailo_control_identify_response {
    uint32_t protocol_version_length;
    uint32_t protocol_version;
    uint32_t fw_version_length;
    struct hailo_control_firmware_version fw_version;
    uint32_t logger_version_length;
    uint32_t logger_version;
    uint32_t board_name_length;
    uint8_t  board_name[HAILO_CONTROL_MAX_BOARD_NAME_LENGTH];
    uint32_t device_architecture_length;
    uint32_t device_architecture;
    uint32_t serial_number_length;
    uint8_t  serial_number[HAILO_CONTROL_MAX_SERIAL_NUMBER_LENGTH];
    uint32_t part_number_length;
    uint8_t  part_number[HAILO_CONTROL_MAX_PART_NUMBER_LENGTH];
    uint32_t product_number_length;
    uint8_t  product_number[HAILO_CONTROL_MAX_PRODUCT_NAME_LENGTH];
} __attribute__((packed));
/*
 * 162 bytes is the wire size firmware actually sends. Without
 * __packed, trailing padding after product_number[42] grows the
 * struct to 164 and the length check in hailo_control_identify
 * rejects a valid response as truncated. This static_assert makes
 * removing the __packed attribute a compile-time error.
 */
_Static_assert(sizeof(struct hailo_control_identify_response) == 162,
               "identify_response must be 162 bytes on the wire "
               "(did someone drop __attribute__((packed))?)");

/*
 * Low-level transport.
 *
 * Writes `req_payload` (req_len bytes) to BAR4 at offset 0 wrapped
 * in [md5 + len + payload], rings the ready doorbell, polls
 * BCS_ISTATUS_HOST for the SW_IRQ bit (up to timeout_us), reads
 * the response wire-format from BAR4+0x640, verifies the response
 * MD5, copies the payload into resp_payload (up to resp_capacity
 * bytes), and writes the actual size into *resp_len.
 *
 * Returns HAILO_OK, HAILO_ERR_TIMEOUT (no response), or
 * HAILO_ERR_BAD_FIRMWARE (MD5 mismatch / impossible buffer_len).
 *
 * Single-threaded by contract — hailo_boot's ATR[0] invariant
 * still applies: only one CPU at a time drives the control channel.
 */
int hailo_control_send_recv(const void *req_payload,
                            uint32_t    req_len,
                            void       *resp_payload,
                            uint32_t    resp_capacity,
                            uint32_t   *resp_len,
                            uint32_t    timeout_us);

/*
 * High-level helpers. Currently just IDENTIFY; more land as
 * needed.
 */
int hailo_control_identify(struct hailo_control_identify_response *out);

/*
 * Reset internal control-channel state (sequence counter and the
 * "IMASK already armed" flag). Only used by unit tests to isolate
 * each send_recv round from the last. Safe to call at any time.
 */
void hailo_control_reset_state_for_tests(void);

#endif /* AI_ACCEL_HAILO_CONTROL_H */
