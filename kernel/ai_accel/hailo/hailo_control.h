/*
 * hailo_control.h — Hailo firmware control-channel wire format.
 *
 * Ported from HailoRT's common/include/control_protocol.h (MIT
 * license; see ~/slmos-ref/hailo/hailort-control-protocol.h). We
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
 * the CORE CPU (CPU 1) handler. Tier-1/2/3 opcodes (IDENTIFY,
 * WRITE_MEMORY, CONFIG_STREAM, …) all target APP CPU. Context-
 * switch opcodes (SET_NETWORK_GROUP_HEADER=0x20,
 * SET_CONTEXT_INFO=0x21) target CORE CPU.
 */
#define HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK  (1u << 0)
#define HAILO_FW_ACCESS_CORE_CPU_CONTROL_MASK (1u << 1)
/* DRIVER_SHUTDOWN: signal fw that the host driver is releasing
 * the device. Linux writes this from hailo_disable_interrupts on
 * release. Lets fw clear "active driver" state so the next boot
 * starts from a known fresh baseline. SOFT_RESET: ask fw to
 * re-init in place without re-uploading the fw blob. Both
 * defined to match hailo-ioctl-common.h:36-41 (NNC interrupt
 * mask enum). #253 (2026-04-23): added so SLM-OS can mirror
 * Linux's clean-shutdown signaling. */
#define HAILO_FW_ACCESS_DRIVER_SHUTDOWN_MASK  (1u << 2)
#define HAILO_FW_ACCESS_SOFT_RESET_MASK       (1u << 3)

/*
 * Which firmware CPU the opcode targets. Used by the transport to
 * pick the doorbell mask. Mirrors hailort's CPU_ID_APP_CPU /
 * CPU_ID_CORE_CPU enum; kept local to avoid dragging in the full
 * hailort header stack.
 */
enum hailo_control_cpu {
    HAILO_CTRL_CPU_APP  = 0,
    HAILO_CTRL_CPU_CORE = 1,
};

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
/* Per-channel VDMA interrupt enable registers (hailo-pcie-common.c:20-21).
 * Reference arms 0xFFFFFFFF into both during `hailo_pcie_enable_interrupts`. */
#define HAILO_BCS_SOURCE_INTERRUPT_PER_CHANNEL   0x0400u
#define HAILO_BCS_DESTINATION_INTERRUPT_PER_CHANNEL 0x0500u
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
/* Mirrors HAILO_PCIE_BOOT_IRQ in pcie_common.h (sw bit 0x2). Raised
 * by fw after a successful boot. Linux's hailo_pcie_read_interrupt
 * read-and-clears it from BCS_ISTATUS_HOST as part of normal IRQ
 * dispatch; SLM-OS polls ATR[1] for the FW_LOADED magic instead, so
 * the bit stays asserted unless we W1C it explicitly. */
#define HAILO_PCIE_BOOT_IRQ                      0x02u
#define HAILO_BCS_ISTATUS_HOST_BOOT_IRQ_BIT      \
    (HAILO_PCIE_BOOT_IRQ << HAILO_BCS_ISTATUS_HOST_SW_IRQ_SHIFT)

/* Subset of HailoRT's HAILO_CONTROL_OPCODE_*. Add more as the
 * kernel learns to send them. */
enum hailo_control_opcode {
    HAILO_CONTROL_OPCODE_IDENTIFY                             = 0x00,
    HAILO_CONTROL_OPCODE_WRITE_MEMORY                         = 0x01,
    HAILO_CONTROL_OPCODE_READ_MEMORY                          = 0x02,
    HAILO_CONTROL_OPCODE_CONFIG_STREAM                        = 0x03,
    HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER = 0x20,
    HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO      = 0x21,
    HAILO_CONTROL_OPCODE_CHANGE_CONTEXT_SWITCH_STATUS         = 0x25,
    HAILO_CONTROL_OPCODE_CORE_IDENTIFY                        = 0x2A,
    HAILO_CONTROL_OPCODE_GET_DEVICE_INFORMATION               = 0x33,
    HAILO_CONTROL_OPCODE_RUN_BIST_TEST                        = 0x3C,
    HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS = 0x47,
    HAILO_CONTROL_OPCODE_GET_HW_CONSTS                        = 0x48,
    HAILO_CONTROL_OPCODE_CHANGE_HW_INFER_STATUS               = 0x4A,
    /* Full table in ~/slmos-ref/hailo/hailort-control-protocol.h. */
};

#ifdef HAILO_HYP_A_RPC
/* CHANGE_HW_INFER_STATUS state values (mirrors HailoRT
 * CONTROL_PROTOCOL__hw_infer_state_t). hyp-A was disconfirmed
 * (2026-05-08); declarations are gated behind HAILO_HYP_A_RPC so the
 * dead reference RPC doesn't ship in production. */
enum hailo_hw_infer_state {
    HAILO_HW_INFER_STATE_START = 0,
    HAILO_HW_INFER_STATE_STOP  = 1,
};

/* CHANGE_HW_INFER_STATUS boundary_channel_mode values (mirrors
 * HailoRT CONTROL_PROTOCOL__boundary_channel_mode_t). */
enum hailo_boundary_channel_mode {
    HAILO_BOUNDARY_CHANNEL_MODE_DESC = 0,
    HAILO_BOUNDARY_CHANNEL_MODE_CCB  = 1,
};
#endif /* HAILO_HYP_A_RPC */

/* CONTROL_PROTOCOL__communication_type_t values.
 * Mirrored from hailort-control-protocol.h:1522. We only use PCIE
 * today (the AI HAT+ is a PCIe endpoint); the others are defined
 * here only so the field has a named value in traces. */
enum hailo_communication_type {
    HAILO_COMMUNICATION_TYPE_UDP       = 0,
    HAILO_COMMUNICATION_TYPE_MIPI      = 1,
    HAILO_COMMUNICATION_TYPE_PCIE      = 2,
    HAILO_COMMUNICATION_TYPE_INTER_CPU = 3,
};

/* CONTROL_PROTOCOL__pcie_dataflow_type_t values for input streams
 * (hailort-control-protocol.h:528). Output streams don't use this
 * enum — their variant carries `desc_page_size` instead. Type 1
 * (CFG flow channel) is reserved for firmware's own CCW upload
 * path and is not a valid user-facing option. */
enum hailo_pcie_dataflow_type {
    HAILO_PCIE_DATAFLOW_TYPE_CONTINUOUS = 0,
    HAILO_PCIE_DATAFLOW_TYPE_BURST      = 2,
};

/*
 * WRITE_MEMORY and READ_MEMORY are chunked at the HailoRT level;
 * a single control request can carry at most 1024 bytes of data.
 * The kernel-level write/read helpers split larger transfers
 * transparently. Matches CONTROL__MAX_WRITE_MEMORY_CHUNK_SIZE in
 * hailort-control.hpp:26.
 */
#define HAILO_CONTROL_MAX_MEMORY_CHUNK 1024u

/*
 * Sanity cap on a single hailo_control_{write,read}_memory call.
 * Chosen generously relative to realistic callers (CCW uploads
 * for Hailo-8 models are typically under 16 MB; the largest
 * compiled Hailo Model Zoo entry is yolov5m at ~17 MB total HEF,
 * of which the weight section is smaller). The cap exists to
 * stop pathological UINT32_MAX-ish inputs reaching the transport
 * and issuing millions of doorbells at ~100 µs each — not to
 * constrain real callers. Raise if a future model needs more.
 */
#define HAILO_CONTROL_MAX_MEMORY_TRANSFER (32u * 1024u * 1024u)

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
 * Internally serialized by control_lock (see hailo_control.c), and
 * the sequence counter is incremented atomically, so concurrent
 * callers across CPUs are safe. Callers must still respect the
 * boot-vs-control ATR[0] contract: never issue hailo_control_*
 * while hailo_boot is mid-flight, because hailo_core.c's atr0_lock
 * and control_lock are separate locks and do not interlock.
 */
int hailo_control_send_recv(const void *req_payload,
                            uint32_t    req_len,
                            void       *resp_payload,
                            uint32_t    resp_capacity,
                            uint32_t   *resp_len,
                            uint32_t    timeout_us);

/*
 * Variant that targets the CORE CPU instead of the APP CPU. Needed
 * for the context-switch opcodes (SET_NETWORK_GROUP_HEADER=0x20,
 * SET_CONTEXT_INFO=0x21); every other opcode still uses the APP-CPU
 * default above. Identical semantics otherwise — same control_lock,
 * same MD5, same response polling — only the doorbell mask differs.
 */
int hailo_control_send_recv_cpu(enum hailo_control_cpu cpu_id,
                                const void *req_payload,
                                uint32_t    req_len,
                                void       *resp_payload,
                                uint32_t    resp_capacity,
                                uint32_t   *resp_len,
                                uint32_t    timeout_us);

/*
 * High-level helpers.
 */
int hailo_control_identify(struct hailo_control_identify_response *out);

/*
 * Write `data_length` bytes from `data` into firmware memory at
 * device-side `address`. Transfers larger than
 * HAILO_CONTROL_MAX_MEMORY_CHUNK are split internally into 1 KB
 * chunks matching HailoRT's write_memory_chunk loop.
 *
 * Rejects with HAILO_ERR_INVAL if `data` is NULL, `data_length`
 * is zero, `data_length` exceeds HAILO_CONTROL_MAX_MEMORY_TRANSFER,
 * or `address + data_length` wraps past UINT32_MAX.
 *
 * Returns HAILO_OK on full success, HAILO_ERR_INVAL on arg
 * rejection, or the first propagated error from the transport if
 * a chunk fails mid-flight. On partial failure earlier chunks may
 * have already been committed on the device — callers who need
 * all-or-nothing semantics must implement that on top.
 */
int hailo_control_write_memory(uint32_t address,
                               const void *data,
                               uint32_t data_length);

/*
 * Read `data_length` bytes from firmware memory at device-side
 * `address` into `data`. Same chunking / validation rules as
 * write_memory.
 *
 * Returns HAILO_OK, HAILO_ERR_INVAL, HAILO_ERR_TIMEOUT (firmware
 * did not respond), or HAILO_ERR_BAD_FIRMWARE (short response).
 * On partial failure the destination buffer has been populated
 * through the last-successful chunk; bytes past that point are
 * untouched. Callers who need defined-on-failure output should
 * pre-zero the buffer or treat any rc != HAILO_OK as the whole
 * read being invalid.
 */
int hailo_control_read_memory(uint32_t address,
                              void *data,
                              uint32_t data_length);

/* Forward declaration — hef_parser.h pulls in hef.pb.h which is
 * large; callers that need the full struct should include
 * hef_parser.h themselves. */
struct hef_info;

/*
 * Upload every WriteDataCcw action recorded in `info->ccw_actions`
 * to the device via chained WRITE_MEMORY calls.
 *
 * `blob_base` points at the original `.hef` protobuf blob the
 * parser was given; each action's data is found at
 * `blob_base + action.data_offset_in_blob`. `device_base_addr` is
 * the starting target address on the device; each action writes
 * at `device_base_addr + cumulative_bytes_so_far`. The caller
 * sources this address from an earlier CONFIG_STREAM response
 * (Phase 5.3+: the CFG channel firmware publishes when a stream
 * is opened; the CCW upload is then a fire-and-forget sequence
 * of appends).
 *
 * Actions beyond HEF_PARSER_MAX_CCW_ACTIONS were not stored by
 * the parser (see `ccw_actions_truncated`); this function
 * therefore refuses to run against a truncated info — a truncated
 * model can't be correctly uploaded from the captured subset.
 *
 * Returns HAILO_OK on full success (all recorded actions written),
 * HAILO_ERR_INVAL on null args or truncated info, or the first
 * non-OK rc from WRITE_MEMORY — in which case earlier actions
 * have ALREADY been committed on the device (the caller must
 * reset the stream / re-upload from scratch if they need
 * all-or-nothing semantics). On success, `*out_bytes_uploaded`
 * (if non-NULL) carries the total byte count — a sanity check
 * against info->ccw_total_bytes.
 */
/*
 * `ccws_base` supplies the byte source for v2+ write_data_ccw_ptr
 * actions (those whose is_ccw_ptr flag is true in hef_ccw_action).
 * It should point at the start of the HEF's CCWS block — i.e.
 * (hef_file_base + outer.ccws_offset). Pass NULL if the HEF only
 * uses v0/v1 write_data_ccw actions (all actions have is_ccw_ptr
 * false); the upload will still work but v2+ actions referencing
 * a NULL ccws_base return HAILO_ERR_INVAL early. blob_base remains
 * the proto-body base for v0/v1 actions.
 *
 * `blob_size` and `ccws_size` bound the respective source buffers.
 * Any action whose [data_offset_in_blob, +data_size) range escapes
 * the appropriate bound returns HAILO_ERR_INVAL before any
 * WRITE_MEMORY fires. This closes an info-leak vector: without the
 * bound, a malformed HEF could cause us to forward post-buffer
 * kernel memory to firmware. Pass 0 only when the corresponding
 * base is NULL (e.g. ccws_size=0 when ccws_base=NULL).
 */
int hailo_control_upload_ccw(const struct hef_info *info,
                             const void *blob_base, size_t blob_size,
                             const void *ccws_base, size_t ccws_size,
                             uint32_t device_base_addr,
                             uint64_t *out_bytes_uploaded);

/*
 * nn_stream_config carries per-stream buffering parameters that
 * firmware's NN engine needs to size its descriptor rings. All
 * values come from the `.hef` — the kernel doesn't invent them.
 * Mirrors CONTROL_PROTOCOL__nn_stream_config_t
 * (hailort-control-protocol.h:451); see also the comment at the
 * HailoRT packer on the odd
 * `htons(params->periph_buffers_per_frame)` on what is declared
 * as a u32 field. We replicate that exactly — the HailoRT
 * truncation-via-htons is part of the firmware-facing contract.
 */
struct hailo_nn_stream_config {
    uint16_t core_bytes_per_buffer;
    uint16_t core_buffers_per_frame;
    uint16_t periph_bytes_per_buffer;
    uint32_t periph_buffers_per_frame;   /* upstream htons'es this u32 */
    uint16_t feature_padding_payload;
    uint32_t buffer_padding_payload;     /* upstream htons'es this u32 */
    uint16_t buffer_padding;
    bool     is_core_hw_padding_config_in_dfc;
};

/*
 * CONFIG_STREAM (opcode 0x03, PCIe variants) params. We only
 * expose the PCIe variants of CONFIG_STREAM on SLM-OS because the
 * Hailo-8 on AI HAT+ is a PCIe endpoint; UDP / MIPI / INTER_CPU
 * are for other SKUs. `is_input` selects between:
 *   - true  → pcie_input:  `pcie_dataflow_type` (enum
 *     hailo_pcie_dataflow_type).
 *   - false → pcie_output: `desc_page_size` (u16, passed as-is to
 *     firmware per the HailoRT convention — NOT byte-swapped).
 */
struct hailo_stream_pcie_config {
    uint8_t                           stream_index;
    bool                              is_input;
    bool                              skip_nn_stream_config;
    struct hailo_nn_stream_config     nn_stream_config;
    uint8_t                           pcie_channel_index;
    union {
        uint8_t  pcie_dataflow_type;   /* input  — enum hailo_pcie_dataflow_type */
        uint16_t desc_page_size;       /* output */
    };
};

/*
 * Configure a PCIe stream and obtain the dataflow_manager_id the
 * firmware assigns. The manager id is required by subsequent
 * OPEN_STREAM / CLOSE_STREAM calls (Phase 5.4). Either direction
 * is valid; pick via `cfg->is_input`.
 *
 * Returns HAILO_OK (out_dataflow_manager_id populated),
 * HAILO_ERR_INVAL (null / bad args), HAILO_ERR_NODEV (device not
 * RUNNING), HAILO_ERR_TIMEOUT (firmware didn't respond),
 * HAILO_ERR_BAD_FIRMWARE (short / malformed response), or
 * HAILO_ERR_IO (firmware returned a non-zero status — caller
 * should check kernel log for major/minor codes).
 */
int hailo_control_config_stream_pcie(
    const struct hailo_stream_pcie_config *cfg,
    uint8_t *out_dataflow_manager_id);

/*
 * Constants mirroring hailort's context-switch protocol, kept here
 * so callers don't have to drag in the whole hailort header set.
 *
 * IMPORTANT: MAX_CFG_CHANNELS is 4 in firmware v4.23 (running on the
 * AI HAT+ in the lab), NOT the 24 the cached reference header
 * ~/slmos-ref/hailo/hailort-control-protocol.h shows for newer releases.
 * The application_header_t wire size is 32 bytes on v4.23; firmware
 * rejects any other length with
 * CONTROL_PROTOCOL_STATUS_INVALID_CONTEXT_SWITCH_APP_HEADER_LENGTH
 * (major=0x40030060). See docs/pi5-ai-hat-plan.md §6.3.
 */
#define HAILO_CS_MAX_CFG_CHANNELS         4u    /* v4.23 firmware */
#define HAILO_CS_MAX_VDMA_ENGINES         3u    /* CONTROL_PROTOCOL__MAX_VDMA_ENGINES_COUNT */
#define HAILO_CS_MAX_CONTEXT_SIZE         4096u /* CONTROL_PROTOCOL__MAX_CONTEXT_SIZE */

/* `external_action_list_address` sentinel for "no DDR backing — use
 * control-channel action lists". HailoRT calls this
 * CONTEXT_SWITCH_DEFS__INVALID_DDR_CONTEXTS_BUFFER_ADDRESS. 0 would
 * be interpreted as a valid DDR pointer → firmware rejects. */
#define HAILO_CS_NO_DDR_ACTION_LIST       0xFFFFFFFFu

/* Context_type values for SET_CONTEXT_INFO. Mirrors
 * CONTROL_PROTOCOL__context_switch_context_type_t. */
enum hailo_cs_context_type {
    HAILO_CS_CONTEXT_TYPE_PRELIMINARY      = 0,
    HAILO_CS_CONTEXT_TYPE_DYNAMIC          = 1,
    HAILO_CS_CONTEXT_TYPE_BATCH_SWITCHING  = 2,
    HAILO_CS_CONTEXT_TYPE_ACTIVATION       = 3,
};

/*
 * Host-facing mirror of CONTROL_PROTOCOL__application_header_t.
 * Field order matches the wire struct exactly so callers can
 * populate this in natural C style without worrying about the
 * packed encoding. hailo_control_set_network_group_header copies
 * into a packed wire buffer before transmission.
 *
 * `external_action_list_address` must be 0 for the control-channel
 * (non-DDR) action-list path — the only path SLM-OS implements in
 * Phase 6.3. `boundary_channels_bitmap` is a bit-per-VDMA-channel
 * per engine; callers OR in the channels they plan to drive for
 * this network group.
 */
struct hailo_cs_application_header {
    uint16_t dynamic_contexts_count;
    /* INFER_FEATURE_LIST_t — 3 bools on v4.23, all default-false.
     * split_allow_input_action exists only in newer firmware. */
    bool     preliminary_run_asap;
    bool     batch_register_config;
    bool     can_fast_batch_switch;
    /* VALIDATION_FEATURE_LIST_t — 1 bool, default false. */
    bool     is_abbale_supported;
    uint8_t  networks_count;
    uint16_t csm_buffer_size;
    uint16_t batch_size;
    /* Pass HAILO_CS_NO_DDR_ACTION_LIST (0xFFFFFFFF) for the
     * control-channel action-list path — Phase 6.3 only supports
     * this path. Zero is interpreted as a valid DDR pointer by
     * firmware and will be rejected. */
    uint32_t external_action_list_address;
    uint32_t boundary_channels_bitmap[HAILO_CS_MAX_VDMA_ENGINES];
    uint8_t  config_channels_count;
    uint8_t  config_channel_packed_id[HAILO_CS_MAX_CFG_CHANNELS];
};

/*
 * SET_NETWORK_GROUP_HEADER (opcode 0x20, CPU_ID_CORE_CPU). Declares
 * a network group to the firmware's context switcher before any
 * per-context action list is sent. The `application_header` is
 * memcpy'd raw (native LE) onto the wire after a BE
 * application_header_length prefix.
 *
 * Returns HAILO_OK on success, HAILO_ERR_INVAL on null arg or
 * out-of-range counts, HAILO_ERR_IO if firmware returned non-zero
 * status (caller checks kernel log for major/minor), or
 * HAILO_ERR_TIMEOUT / HAILO_ERR_BAD_FIRMWARE from the transport.
 */
int hailo_control_set_network_group_header(
    const struct hailo_cs_application_header *header);

/*
 * Maximum context_network_data bytes per SET_CONTEXT_INFO chunk.
 *
 * Per hailort's CONTROL_PROTOCOL__CONTEXT_NETWORK_DATA_SINGLE_CONTROL_MAX_SIZE:
 *   MAX_CONTROL_LENGTH(1500) - offsetof(request_t, parameters)(20)
 *     - sizeof(context_switch_set_context_info_request_t)(19)
 *   = 1461 bytes.
 *
 * offsetof(parameters) = 16 (common header) + 4 (parameter_count) = 20.
 * sizeof-request = 4+1+4+1+4+1+4 = 19 (four BE lengths plus three u8
 * payload slots for is_first / is_last / context_type; the final
 * context_network_data_length is part of the prefix, the payload
 * itself is the [0] flex-array tail).
 */
#define HAILO_CS_CONTEXT_CHUNK_MAX_BYTES  1461u

/*
 * SET_CONTEXT_INFO (opcode 0x21, CPU_ID_CORE_CPU). Sends one chunk
 * of a context's pre-encoded action stream to firmware. The action
 * bytes come from the HEF's contexts[].metadata — see the
 * hailort-context_switch_defs.h reference and Phase 6.3d plumbing.
 *
 * `context_type` selects preliminary/dynamic/batch-switching/
 * activation (see enum hailo_cs_context_type). Chunking is driven
 * by the caller: set is_first_chunk=true on the opening call for a
 * given context and is_last_chunk=true on the closing call; single-
 * chunk contexts set both.
 *
 * `network_data` points at the chunk bytes; `network_data_len` must
 * be <= HAILO_CS_CONTEXT_CHUNK_MAX_BYTES. Returns HAILO_OK, or
 * HAILO_ERR_INVAL on null/oversize, or HAILO_ERR_IO / TIMEOUT /
 * BAD_FIRMWARE from the transport.
 */
int hailo_control_set_context_info_chunk(
    enum hailo_cs_context_type context_type,
    bool                       is_first_chunk,
    bool                       is_last_chunk,
    const void                *network_data,
    uint32_t                   network_data_len);

/*
 * Convenience wrapper that chunks a whole context's action bytes
 * into HAILO_CS_CONTEXT_CHUNK_MAX_BYTES slices and issues the
 * sequence of SET_CONTEXT_INFO calls with the is_first/is_last flags
 * set automatically. Zero-length contexts still fire one
 * is_first=is_last=true call with an empty payload — firmware treats
 * that as "context with no actions", which is valid for synthetic
 * contexts but unusual in practice.
 *
 * Returns HAILO_OK on success; the first non-OK rc from any chunk
 * otherwise. Earlier chunks have already landed — caller must treat
 * partial failure as a full re-load (Phase 6.3e will add that path).
 */
int hailo_control_set_context_info(
    enum hailo_cs_context_type context_type,
    const void                *network_data,
    uint32_t                   network_data_len);

/* State-machine targets for CHANGE_CONTEXT_SWITCH_STATUS (opcode 0x25,
 * CPU_ID_CORE_CPU). Mirrors CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_t. */
enum hailo_cs_state {
    HAILO_CS_STATE_RESET   = 0,
    HAILO_CS_STATE_ENABLED = 1,
};

/* IGNORE_NETWORK_GROUP_INDEX per hailort-control.cpp:2054 — used
 * with STATE_RESET where the application_index field is meaningless. */
#define HAILO_CS_IGNORE_APPLICATION_INDEX  255u

/*
 * CHANGE_CONTEXT_SWITCH_STATUS (opcode 0x25, CPU_ID_CORE_CPU).
 * Drives the firmware's context-switch state machine between RESET
 * (idle, ready to accept a new network group) and ENABLED (running).
 * Must be called with RESET BEFORE SET_NETWORK_GROUP_HEADER; firmware
 * rejects header/context writes with major=0x40030060 otherwise (this
 * is what the 2026-04-19 ctxsmoke hardware probe returned on pi-5-1).
 *
 * `application_index` is the network-group index (0 for the first
 * and only loaded NG); pass HAILO_CS_IGNORE_APPLICATION_INDEX on
 * RESET. `dynamic_batch_size` / `batch_count` are inference-time
 * params — ignored on RESET, honored on ENABLED (0 / 0 for a simple
 * "enable with loaded batch size" pattern).
 *
 * Returns HAILO_OK, HAILO_ERR_IO on firmware non-zero status, or
 * transport errors.
 */
int hailo_control_change_context_switch_status(
    enum hailo_cs_state state,
    uint8_t             application_index,
    uint16_t            dynamic_batch_size,
    uint16_t            batch_count);

/*
 * CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS (opcode 0x47, CPU_ID_CORE_CPU).
 * Empty-body request. Clears firmware's internal bookkeeping for
 * previously-configured network groups so the next SET_NETWORK_GROUP_
 * HEADER / SET_CONTEXT_INFO sequence starts from a clean state.
 *
 * Wire capture (#180 diagnostic, 2026-04-19): HailoRT calls this
 * between CHANGE_CONTEXT_SWITCH_STATUS(RESET) and GET_HW_CONSTS. We
 * were skipping it entirely, which leaves fw v4.23's context-switch
 * state machine with stale bookkeeping and causes any non-empty
 * BATCH_SWITCHING action list to walk into uninitialized memory.
 */
int hailo_control_context_switch_clear_configured_apps(void);

/*
 * GET_HW_CONSTS (opcode 0x48, CPU_ID_CORE_CPU). Empty-body request;
 * firmware responds with a packed struct of hardware constants.
 * HailoRT fetches this in fill_activation_config_recepies and
 * fill_batch_switching_context_edge_layers to size internal
 * structures.
 *
 * For Phase 6.8, the returned constants are not consumed by SLM-OS
 * (we hardcode reasonable Hailo-8 defaults). The call is made for
 * the side-effect of completing firmware's pre-configure handshake.
 * `out_response_len` is set to the number of response body bytes
 * received so callers can optionally inspect; pass NULL to ignore.
 */
int hailo_control_get_hw_consts(uint32_t *out_response_len);

/*
 * Accessor for the static BSS response body of the most recent
 * GET_HW_CONSTS call. Useful for HAILO_WIRE_DEBUG dumps that need to
 * run *after* the caller's timing window has closed — any uart_printf
 * inside hailo_control_get_hw_consts itself would be bracketed by the
 * caller's timer reads and skew measurements (51 B body → 5 hex lines
 * → ~21 ms at 115200 baud). The buffer is `out_capacity` bytes; the
 * meaningful prefix is the `*out_response_len` returned by the most
 * recent get_hw_consts call.
 *
 * Both pointers may be NULL if the caller wants only one. The buffer
 * remains valid until the next GET_HW_CONSTS call (writer is serialized
 * under control_lock).
 */
void hailo_control_get_hw_consts_response_body(const uint8_t **out_body,
                                               uint32_t       *out_capacity);

/*
 * CORE_IDENTIFY (opcode 0x2A, CPU_ID_CORE_CPU). Empty-body liveness
 * probe. Firmware responds with its fw_version ({major, minor,
 * revision} u32s). If the CORE CPU's RPC thread is alive, it responds
 * in single-digit microseconds; if the CORE CPU is wedged (e.g. the
 * inference task crashed and starved the RPC thread) the call times
 * out.
 *
 * Added 2026-04-21 for the Phase 8 #253 investigation: it lets the
 * submit path check "is CORE CPU still listening?" immediately before
 * writing num_avail. A live response here but a frozen num_proc after
 * submit would localize the blocker to the inference task / BURST_
 * CREDITS_TASK state rather than the whole CORE CPU.
 *
 * `out_response_len` is set to the number of response body bytes on
 * success; pass NULL to ignore.
 */
int hailo_control_core_identify(uint32_t *out_response_len);

/*
 * GET_DEVICE_INFORMATION (opcode 0x33, CPU_ID_APP_CPU). Empty-body
 * probe; firmware responds with a ~143-byte struct describing device
 * state. HailoRT calls this multiple times during load (pre-RESET,
 * post-CLEAR_APPS, post-SET_CONTEXT_INFO, post-ENABLED) as a
 * fw-settled / liveness handshake. SLM-OS does not strictly need the
 * response content — this wrapper just fires the RPC and checks
 * rc=0.
 *
 * Added 2026-04-23 for the Phase 8 #253 investigation: HailoRT's
 * wire capture shows 7 of these interspersed through the load
 * sequence; SLM-OS sends none. Mirroring HailoRT's cadence is one
 * of the cheapest ways to rule "post-ENABLED settling" in or out as
 * the cause of the boundary-submit stall.
 *
 * `out_response_len` is set on success; pass NULL to ignore.
 */
int hailo_control_get_device_information(uint32_t *out_response_len);

/*
 * RUN_BIST_TEST (opcode 0x3C, CPU_ID_APP_CPU). Memory built-in
 * self-test. The chip's BIST whitelist is bits 2..5 of the top
 * memory bitmap (the four L4 SRAM banks); all other bits are
 * silently ignored even when un-bypassed.
 *
 * Added 2026-04-25 for the Phase 8 #253 CPU_ECC investigation
 * (`memory_bitmap=0x00001000` = bit 12 = SAGE1_ISP per the BIST
 * top-block enum). Running BIST itself only exercises the L4
 * banks, so it cannot directly probe bit 12 — but the response's
 * pass/fail layout is the strongest available reference for how
 * fw numbers chip-side memory blocks. If the response shows a
 * bit-flagged result vector that lines up with the BIST enum, the
 * CPU_ECC bitmap-to-block mapping is confirmed by analogy.
 *
 * Caller passes the request bitmap fields verbatim; this wrapper
 * marshals them and dumps the raw response payload to `out_resp`
 * (truncated to `out_resp_cap`). `out_resp_len` is the actual
 * response payload size on the wire (post-common-header).
 *
 * BIST is destructive: fw scribbles patterns into memory then
 * checks them. Plan to reboot the chip after running.
 */
int hailo_control_run_bist_test(bool     is_top_test,
                                uint32_t top_bypass_bitmap,
                                uint8_t  cluster_index,
                                uint32_t cluster_bypass_bitmap_0,
                                uint32_t cluster_bypass_bitmap_1,
                                uint8_t *out_resp,
                                uint32_t out_resp_cap,
                                uint32_t *out_resp_len);

/*
 * Pre-boot interrupt-mask arming. Linux's hailo_pcie_enable_interrupts
 * (called from hailo_activate_board BEFORE load_firmware) writes the
 * IMASK_HOST + per-channel SRC/DST IRQ masks before triggering the
 * fw boot, so fw boots with all IRQ infrastructure already armed. We
 * previously only did this lazily on the first FW_CONTROL RPC, after
 * fw was already running. Phase 8 #253: hypothesis is fw initializes
 * differently when IRQ masks are/aren't armed at boot time. This
 * function lets the boot path call it before triggering fw.
 *
 * Idempotent: if interrupts have already been armed, a re-call is
 * cheap (the writes are the same value). MSI handler registration is
 * NOT done here — that lives in control_post_boot_init since it
 * needs fw to be RUNNING (handler may receive responses).
 */
int hailo_control_arm_irq_masks(void);

/*
 * #682 hyp-N (2026-05-09): mirror Linux's hailo_disable_interrupts
 * after BOOT_IRQ. Writes IMASK_HOST=0 and clears the arm flag so the
 * next call to hailo_control_arm_irq_masks runs the full re-arm
 * sequence (which currently early-returns when the flag is set).
 * Linux disables IMASK after BOOT_IRQ and re-enables on first open();
 * SLM-OS calls this from hailo_boot() right after the BOOT_IRQ ack,
 * and control_post_boot_init re-arms on the first FW_CONTROL RPC.
 *
 * Also used under HAILO_IRQ_CYCLE_AT_BOOT to wrap a disable→re-enable
 * cycle around the post-boot D3hot transition (replaces the older
 * hailo_control_disable_imask name from PR #695). Per-channel SRC/DST
 * IRQ masks are NOT cleared here — Linux's disable path leaves them
 * armed too.
 *
 * Returns HAILO_OK on success, or HAILO_ERR_NODEV if the platform
 * shim isn't wired up (e.g., test stub).
 */
int hailo_control_disarm_irq_masks(void);

/*
 * #682 hypothesis-1 diagnostic. Reads back the four interrupt-state
 * registers (IMASK_HOST, ISTATUS_HOST, BCS_SOURCE_INTERRUPT_PER_CHANNEL,
 * BCS_DESTINATION_INTERRUPT_PER_CHANNEL) and prints them with the
 * supplied label. Used to verify whether the per-channel SRC/DST IRQ
 * enable bits stay set across the load + run sequence, or whether
 * fw / a CPU_ECC event is clearing them on us. The function itself
 * is always compiled; all in-tree call sites are wrapped under
 * `#ifdef HAILO_WIRE_DEBUG` so default builds incur no diagnostic
 * I/O on the load/run hot paths.
 */
void hailo_control_dump_irq_state(const char *label);

#ifdef HAILO_HYP_A_RPC
/*
 * #682 hyp-A (2026-05-08, disconfirmed): CHANGE_HW_INFER_STATUS RPC.
 * Targets CORE CPU. Used by HailoRT's hw-only benchmark mode to start
 * internal inference (where fw generates synthetic input). Tested as
 * a "wake up the DYNAMIC context's APPLICATION_CHANGE_INTERRUPT wait"
 * experiment — fw's pios_DYNAMIC.bin ends with action 0x15 (waiting
 * for an app-change signal). RPC was rejected with status
 * 0x400300ca/0x40000001; HW-only mode requires special HEF state not
 * applicable to streaming inference. Gated behind HAILO_HYP_A_RPC so
 * production builds don't ship the dead RPC; flip the build flag if
 * a future fw version honours it. The wire layout (`_Static_assert`s
 * in the .c file) and the comment trail are why we keep this around.
 *
 * `state`: HAILO_HW_INFER_STATE_START or _STOP.
 * `app_idx`: network group index (0 for our single-NG MNIST).
 * `dynamic_batch_size` / `batch_count`: for the experiment, both 1.
 * `boundary_mode`: HAILO_BOUNDARY_CHANNEL_MODE_DESC for descriptor-
 *   based (matches our setup); _CCB for circular-credit mode.
 * The host-side channels_info struct is sent ALL ZEROS (channel_count=0)
 * since we don't have desc_programed counts to populate; whether fw
 * accepts that is itself the experimental signal.
 */
int hailo_control_change_hw_infer_status(
    enum hailo_hw_infer_state         state,
    uint8_t                           app_idx,
    uint16_t                          dynamic_batch_size,
    uint16_t                          batch_count,
    enum hailo_boundary_channel_mode  boundary_mode);
#endif /* HAILO_HYP_A_RPC */

/*
 * Pre-boot MSI registration. Linux's hailo_pcie_enable_interrupts
 * (called BEFORE load_firmware) does pci_enable_msi + request_irq
 * so MSI is configured by the time fw boots. SLM-OS previously only
 * called register_irq on first FW_CONTROL RPC — long after boot.
 * This function lets the boot path engage MSI early so fw observes
 * a fully-configured interrupt environment when it comes up.
 *
 * Idempotent and non-fatal: on platforms without register_irq or on
 * second call, returns HAILO_OK without re-registering.
 */
int hailo_control_register_msi_for_boot(void);

/*
 * Signal fw that the host driver is shutting down. Writes
 * FW_ACCESS_DRIVER_SHUTDOWN_MASK (0x4) to the raise_ready
 * doorbell so fw can clean up its "active driver" state. Mirrors
 * Linux's hailo_pcie_finalize_doorbell_data path. Safe to call
 * before reboot, before fw teardown, or when releasing the
 * accelerator. No-op if fw is not running.
 */
int hailo_control_signal_driver_shutdown(void);

/*
 * Reset internal control-channel state (sequence counter and the
 * "IMASK already armed" flag). Only used by unit tests to isolate
 * each send_recv round from the last. Safe to call at any time.
 */
void hailo_control_reset_state_for_tests(void);

#endif /* AI_ACCEL_HAILO_CONTROL_H */
