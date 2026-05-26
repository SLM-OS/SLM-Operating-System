/*
 * inference_device_hailo.c — Hailo-8 NPU backend for inference_device.h.
 *
 * Plumbs the Hailo driver (hef_parser / hef_header / hailo_control /
 * hailo_cs_translator / hailo_infer) through the common
 * inference_device vtable so scheduler and Lua bindings can route
 * inference to the NPU the same way they route to the CPU MLP.
 *
 * load_model (Phase 6.6, #179) drives the full context-switch load
 * sequence:
 *   1. Parse HEF outer header + proto body (hef_parser).
 *   2. Pick largest-pad input/output for slot sizing.
 *   3. Claim a slot.
 *   4. context_switch_load: allocate CCW + boundary DMA buffers +
 *      descriptor lists, translate HEF → wire-action contexts, and
 *      ship the 6-step RPC sequence firmware expects
 *      (RESET → SET_NETWORK_GROUP_HEADER → 4 × SET_CONTEXT_INFO →
 *      ENABLED).
 *   5. Populate slot->cfg + shapes for run().
 *
 * run() currently calls hailo_infer_run, which allocates its own
 * short-lived tensor + desc list per call. That mismatches the
 * boundary descriptor lists load_model bound to firmware via
 * OpenBoundary actions; firmware expects the boundary-channel IOVAs
 * it was told about in ACTIVATION, not whatever run() just allocated.
 * Wiring run() to reuse load_model's boundary lists (or switching to
 * a zero-copy submit path that targets them) is a follow-up — this
 * backend is today "load works end-to-end on real hardware with
 * hypothesis validation for #180; inference submit runs in QEMU only
 * via the mock auto-advance path."
 *
 * Model slots are a fixed-size table (HAILO_MAX_MODELS=4), handed
 * out as handles `idx + 1` so INF_BUILTIN_HANDLE (=0) stays reserved.
 * No dynamic allocation; all state is in BSS + DMA coherent regions
 * managed by hailo_tensor/hailo_vdma.
 */

#include "inference_device.h"
#include "hailo.h"
#include "hailo_control.h"
#include "hailo_cs_translator.h"
#include "hailo_infer.h"
#include "hailo_internal.h"
#include "hailo_tensor.h"
#include "hailo_trace.h"
#include "hailo_vdma.h"
#include "hef_header.h"
#include "hef_parser.h"
#include "spinlock.h"
#include "debug.h"
#include "timer.h"
#include "uart.h"
#include <string.h>

/* Firmware debug log layout (from ~/slmos-ref/hailo/hailo-fw-operation.c:18-21
 * and hailo-fw-operation.h:11).
 *
 *   BAR4[0x2000]                 APP CPU  { host_offset, chip_offset } + 4088 B data
 *   BAR4[0x2000 + 0x1000]        CORE CPU { host_offset, chip_offset } + 4088 B data
 *
 * The buffer is circular: `chip_offset` advances as fw writes; `host_offset`
 * is where the host has read up to. Data between them is "ready to read".
 * Both are 32-bit, naturally aligned at the start of each per-CPU window. */
#define HAILO_FW_LOG_APP_CPU_OFFSET   (8u * 1024u)     /* 0x2000 */
#define HAILO_FW_LOG_BUF_SIZE         (4u * 1024u)     /* per-CPU */
#define HAILO_FW_LOG_CORE_CPU_OFFSET  (HAILO_FW_LOG_APP_CPU_OFFSET + HAILO_FW_LOG_BUF_SIZE)
#define HAILO_FW_LOG_HEADER_SIZE      8u
#define HAILO_FW_LOG_DATA_SIZE        (HAILO_FW_LOG_BUF_SIZE - HAILO_FW_LOG_HEADER_SIZE)

/* D2H notification buffer (from hailo-fw-operation.c:19,
 * hailort-d2h_events.h). Layout:
 *   u16 is_buffer_in_use
 *   u16 buffer_len
 *   u8  buffer[0x370]
 *
 * When fw has a notification pending (is_buffer_in_use=1), buffer[] holds
 * a D2H_EVENT_MESSAGE_t starting with a D2H_EVENT_HEADER_t {version,
 * sequence, priority, module_id, event_id, parameter_count, payload_length}
 * — each 32-bit — followed by a typed body per event_id. Event id 12 is
 * CONTEXT_SWITCH_RUN_TIME_ERROR, which carries {exit_status, batch_index,
 * context_index, action_index, application_index}. If fw stalled the
 * inference pipeline it tells us exactly which action/context blew up
 * via this buffer. */
#define HAILO_D2H_NOTIFICATION_OFFSET 0x0c80u
#define HAILO_D2H_EVENT_MAX_SIZE      0x370u

/* Dump one fw CPU's debug log ring via BAR4. `label` is a short tag
 * ("APP"/"CORE") prepended to each line. Prints up to HAILO_FW_LOG_DATA_SIZE
 * bytes as raw ASCII with \n → \r\n translation so serial-console parsers
 * stay happy. Ignores the header's host/chip_offset pair and just dumps
 * the whole ring — on a healthy boot the circular buffer has wrapped at
 * least once so contents are always up-to-date. Diagnostic-only; strip
 * once the Phase 8 inference-submit blocker lifts. */
static void hailo_fw_dump_log(uint32_t cpu_base_offset, const char *label)
{
    if (!hailo_platform || !hailo_platform->bar4_read) return;

    uint32_t header[2] = { 0, 0 };
    hailo_platform->bar4_read(cpu_base_offset, header, sizeof(header));
    uart_printf("[fwlog:%s] header host_offset=%u chip_offset=%u\r\n",
                label, (unsigned)header[0], (unsigned)header[1]);

    static uint32_t log_buf[HAILO_FW_LOG_DATA_SIZE / 4];
    hailo_platform->bar4_read(cpu_base_offset + HAILO_FW_LOG_HEADER_SIZE,
                              log_buf, sizeof(log_buf));
    const char *p = (const char *)log_buf;
    uart_printf("[fwlog:%s] --- begin ---\r\n", label);
    char line[128];
    size_t l = 0;
    for (size_t i = 0; i < sizeof(log_buf); i++) {
        char c = p[i];
        if (c == 0) {
            if (l > 0) { line[l] = 0; uart_printf("%s\r\n", line); l = 0; }
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n' || l + 2 >= sizeof(line)) {
            line[l] = 0;
            uart_printf("%s\r\n", line);
            l = 0;
            continue;
        }
        if (c < 0x20 || c > 0x7E) c = '.';
        line[l++] = c;
    }
    if (l > 0) { line[l] = 0; uart_printf("%s\r\n", line); }
    uart_printf("[fwlog:%s] --- end ---\r\n", label);
}

/* Public wrapper: dump CORE + APP fw debug log rings. Externally
 * declared (via `extern`) from kernel/ai_accel/hailo/hailo_shell.c
 * so the `hailo fwlog` shell command can drive it on demand —
 * including immediately post-boot to see what fw says about its
 * own initialisation, before any RPCs have run. */
void hailo_fw_dump_logs(void)
{
    hailo_fw_dump_log(HAILO_FW_LOG_CORE_CPU_OFFSET, "CORE");
    hailo_fw_dump_log(HAILO_FW_LOG_APP_CPU_OFFSET,  "APP");
}

/* Hex variant: same buffers as hailo_fw_dump_logs but emits raw bytes
 * 16 per line. The fw log format is opaque binary (HailoRT's driver
 * just copies bytes to userspace; the decoder lives in libhailort).
 * The printable-replaced view in hailo_fw_dump_log is misleading
 * because it merges multi-byte tokens at byte boundaries. The hex
 * view preserves the bytes faithfully so a Hailo support engineer
 * can decode them. Bounded by `max_bytes` to keep serial output
 * tractable; pass 0 for the full ring (4088 B). */
static void hailo_fw_dump_log_hex(uint32_t cpu_base_offset,
                                  const char *label,
                                  uint32_t max_bytes)
{
    if (!hailo_platform || !hailo_platform->bar4_read) return;

    uint32_t header[2] = { 0, 0 };
    hailo_platform->bar4_read(cpu_base_offset, header, sizeof(header));
    uart_printf("[fwloghex:%s] header host_offset=%u chip_offset=%u\r\n",
                label, (unsigned)header[0], (unsigned)header[1]);

    uint32_t cap = max_bytes > 0 ? max_bytes : HAILO_FW_LOG_DATA_SIZE;
    if (cap > HAILO_FW_LOG_DATA_SIZE) cap = HAILO_FW_LOG_DATA_SIZE;

    /* Read into a stack buffer in 256 B chunks to keep stack usage
     * bounded — the full 4088 B ring on the kernel stack would push
     * us close to the 16 KB ceiling under a deep call chain. */
    uint8_t chunk[256];
    uint32_t printed = 0;
    while (printed < cap) {
        uint32_t take = (cap - printed) > sizeof(chunk)
                        ? (uint32_t)sizeof(chunk) : (cap - printed);
        hailo_platform->bar4_read(cpu_base_offset + HAILO_FW_LOG_HEADER_SIZE
                                  + printed, chunk, take);
        for (uint32_t i = 0; i < take; i += 16) {
            uart_printf("[fwloghex:%s] [%04x]", label, printed + i);
            uint32_t row = (take - i) > 16 ? 16 : (take - i);
            for (uint32_t j = 0; j < row; j++) {
                uart_printf(" %02x", chunk[i + j]);
            }
            uart_printf("\r\n");
        }
        printed += take;
    }
}

void hailo_fw_dump_logs_hex(uint32_t max_bytes)
{
    hailo_fw_dump_log_hex(HAILO_FW_LOG_CORE_CPU_OFFSET, "CORE", max_bytes);
    hailo_fw_dump_log_hex(HAILO_FW_LOG_APP_CPU_OFFSET,  "APP",  max_bytes);
}

/* Decode event_id to a short name so the output doesn't need a cross-
 * reference to d2h_events.h. Order matches D2H_EVENT_ID_t. */
static const char *d2h_event_name(uint32_t event_id)
{
    switch (event_id) {
    case 0:  return "ETHERNET_RX_ERROR";
    case 1:  return "HOST_INFO";
    case 2:  return "TEMPERATURE_ALARM";
    case 3:  return "CLOSED_STREAMS";
    case 4:  return "OVERCURRENT_ALERT";
    case 5:  return "LCU_ECC_CORRECTABLE";
    case 6:  return "LCU_ECC_UNCORRECTABLE";
    case 7:  return "CPU_ECC_ERROR";
    case 8:  return "CPU_ECC_FATAL";
    case 9:  return "CS_BREAKPOINT_REACHED";
    case 10: return "CLOCK_CHANGED";
    case 11: return "HW_INFER_DONE";
    case 12: return "CS_RUN_TIME_ERROR";
    case 13: return "NN_CORE_CRC_ERROR";
    case 14: return "THROTTLING_CHANGE";
    default: return "UNKNOWN";
    }
}

/* Decode a bit position in CPU_ECC's `memory_bitmap` to a block name.
 * Mapping is CONTROL_PROTOCOL__bist_top_mem_block_t in
 * ../slmos-reference-cache/hailo/hailort-control-protocol.h:1222-1252; CPU_ECC events use the
 * same enum (confirmed by hailort-control.cpp:test_chip_memories
 * iterating 0..NUM_MEM_BLOCKS over CONTROL_PROTOCOL__BIST_TOP_WHITELIST).
 * The trailing _N suffix in the source enum is the physical block
 * index in the SoC; the bit position is the enum ordinal. */
static const char *top_mem_block_name(unsigned bit)
{
    switch (bit) {
    case 0:  return "CRYPTO_1";
    case 1:  return "L4_0_2";
    case 2:  return "L4_1_3";
    case 3:  return "L4_2_4";
    case 4:  return "L4_3_5";
    case 5:  return "SAGE1_CPU_6";
    case 6:  return "SAGE1_CPU_FAST_BUS_7";
    case 7:  return "SAGE1_DEBUG_8";
    case 8:  return "SAGE1_ETH_9";
    case 9:  return "SAGE1_FLASH_10";
    case 10: return "SAGE1_H264_11";
    case 11: return "SAGE1_ISP_12";
    case 12: return "SAGE1_MIPI_RX_13";
    case 13: return "SAGE1_MIPI_TX_14";
    case 14: return "SAGE1_PCIE_15";
    case 15: return "SAGE1_SDIO_16";
    case 16: return "SAGE1_SOFTMAX_17";
    case 17: return "SAGE1_USB_18";
    case 18: return "SAGE1_19";
    case 19: return "SUB_SERVER0_20";
    case 20: return "SUB_SERVER1_21";
    case 21: return "SUB_SERVER2_22";
    case 22: return "SUB_SERVER3_23";
    case 23: return "SUB_SERVER4_24";
    case 24: return "SUB_SERVER5_25";
    case 25: return "SUB_SERVER6_26";
    case 26: return "SUB_SERVER7_27";
    default: return "RESERVED";
    }
}

/* ACK the pending notification by writing 0 to the is_buffer_in_use +
 * buffer_len u32 at offset 0. Fw is then free to overwrite the buffer
 * with the next queued event. */
static void hailo_fw_ack_d2h_notification(void)
{
    if (!hailo_platform || !hailo_platform->bar4_write) return;
    uint32_t zero = 0;
    hailo_platform->bar4_write(HAILO_D2H_NOTIFICATION_OFFSET, &zero,
                               sizeof(zero));
    hailo_platform->mb();
}

/* Dump the D2H notification buffer. If fw has raised a
 * CONTEXT_SWITCH_RUN_TIME_ERROR (or similar) we see it here as a
 * typed D2H_EVENT_MESSAGE_t. `is_buffer_in_use=0` means no pending
 * notification. Returns true if a notification was pending (and the
 * buffer has been left in_use — caller may want to ACK). */
static bool hailo_fw_dump_d2h_notification_once(void)
{
    if (!hailo_platform || !hailo_platform->bar4_read) return false;

    /* 32-bit read so bar4_read's alignment check passes. is_buffer_in_use
     * is the low 16 bits, buffer_len is the high 16. */
    uint32_t first_word = 0;
    hailo_platform->bar4_read(HAILO_D2H_NOTIFICATION_OFFSET, &first_word,
                              sizeof(first_word));
    uint16_t in_use  = (uint16_t)(first_word & 0xFFFFu);
    uint16_t buf_len = (uint16_t)(first_word >> 16);
    uart_printf("[d2h] notification: in_use=%u buffer_len=%u\r\n",
                (unsigned)in_use, (unsigned)buf_len);

    if (!in_use || buf_len == 0 || buf_len > HAILO_D2H_EVENT_MAX_SIZE) {
        return in_use ? true : false;
    }

    /* D2H_EVENT_HEADER_t is 7 × u32 = 28 bytes. */
    uint32_t hdr[7] = { 0 };
    uint32_t hdr_bytes = buf_len < sizeof(hdr) ? buf_len : sizeof(hdr);
    /* Round hdr_bytes up to 4-byte multiple for bar4_read alignment. */
    hdr_bytes = (hdr_bytes + 3u) & ~3u;
    hailo_platform->bar4_read(HAILO_D2H_NOTIFICATION_OFFSET + 4u, hdr,
                              hdr_bytes);
    uart_printf("[d2h] header: version=%u seq=%u priority=%u module_id=%u "
                "event_id=%u(%s) param_count=%u payload_len=%u\r\n",
                (unsigned)hdr[0], (unsigned)hdr[1], (unsigned)hdr[2],
                (unsigned)hdr[3], (unsigned)hdr[4],
                d2h_event_name(hdr[4]),
                (unsigned)hdr[5], (unsigned)hdr[6]);

    /* Read up to 16 B of body. Only the first `payload_len` bytes are
     * the actual event payload; everything past that is stale fw-side
     * notification-buffer contents from prior events. We still read 16
     * to keep the bar4 alignment + length predictable, but we cap the
     * printed dword count to ceil(payload_len/4) so readers don't get
     * misled by trailing garbage (the bytes that vary per session
     * because they're whatever the previous notification left behind).
     */
    if (buf_len > sizeof(hdr)) {
        uint32_t body[4] = { 0 };
        hailo_platform->bar4_read(HAILO_D2H_NOTIFICATION_OFFSET + 4u
                                      + sizeof(hdr),
                                  body, sizeof(body));
        uint32_t payload_len  = hdr[6];
        uint32_t payload_dwords = (payload_len + 3u) / 4u;
        if (payload_dwords > 4u) payload_dwords = 4u;
        switch (payload_dwords) {
        case 0:
            /* Header claims zero payload; nothing to print but still
             * reachable since buf_len > 28 (header + alignment slack). */
            break;
        case 1:
            uart_printf("[d2h] body: 0x%08x (payload_len=%u)\r\n",
                        (unsigned)body[0], (unsigned)payload_len);
            break;
        case 2:
            uart_printf("[d2h] body: 0x%08x 0x%08x (payload_len=%u)\r\n",
                        (unsigned)body[0], (unsigned)body[1],
                        (unsigned)payload_len);
            break;
        case 3:
            uart_printf("[d2h] body: 0x%08x 0x%08x 0x%08x "
                        "(payload_len=%u)\r\n",
                        (unsigned)body[0], (unsigned)body[1],
                        (unsigned)body[2], (unsigned)payload_len);
            break;
        default:
            uart_printf("[d2h] body: 0x%08x 0x%08x 0x%08x 0x%08x "
                        "(payload_len=%u)\r\n",
                        (unsigned)body[0], (unsigned)body[1],
                        (unsigned)body[2], (unsigned)body[3],
                        (unsigned)payload_len);
            break;
        }

        if (hdr[4] == 7 || hdr[4] == 8) {
            /* CPU_ECC_ERROR / CPU_ECC_FATAL — payload is one u32
             * memory_bitmap. Decode the set bits to block names.
             * On AI HAT+ on Pi 5 with v4.23 fw the persistently-
             * observed value is 0x00001000 = bit 12 = SAGE1_MIPI_RX_13,
             * which is unused on this configuration (no camera) — fw's
             * boot-time ECC scrub flags it but it's not on the
             * inference data path (those are SAGE1_PCIE / SUB_SERVER*).
             * If a future trace shows OTHER bits set the decoder makes
             * the block immediately legible without cross-referencing
             * control-protocol.h. */
            uint32_t bitmap = body[0];
            uart_printf("[d2h] CPU_ECC: memory_bitmap=0x%08x",
                        (unsigned)bitmap);
            if (bitmap == 0) {
                uart_printf(" (no blocks flagged)\r\n");
            } else {
                uart_printf(" blocks=");
                bool first = true;
                for (unsigned bit = 0; bit < 32u; bit++) {
                    if (bitmap & (1u << bit)) {
                        uart_printf("%s%s", first ? "" : ",",
                                    top_mem_block_name(bit));
                        first = false;
                    }
                }
                uart_printf("\r\n");
            }
        } else if (hdr[4] == 12) {
            /* CONTEXT_SWITCH_RUN_TIME_ERROR. Body: {exit_status,
             * batch_index, context_index (u16), action_index (u16),
             * application_index (u8)}. Packed, 13 bytes total. */
            uint32_t exit_status  = body[0];
            uint32_t batch_index  = body[1];
            uint16_t context_idx  = (uint16_t)(body[2] & 0xFFFFu);
            uint16_t action_idx   = (uint16_t)(body[2] >> 16);
            uint8_t  app_idx      = (uint8_t)(body[3] & 0xFFu);
            uart_printf("[d2h] CS_RUNTIME_ERROR: exit_status=0x%08x "
                        "batch=%u ctx=%u action=%u app=%u\r\n",
                        (unsigned)exit_status, (unsigned)batch_index,
                        (unsigned)context_idx, (unsigned)action_idx,
                        (unsigned)app_idx);
        }
    }
    return true;
}

/* Post-failure drain cap. After a submit times out we want the
 * most recent fw-side error event, not full history — 8 is enough
 * to flush whatever fw posted while we waited. The previous
 * pre-submit drain (cap 128) was removed when notification
 * handling moved to the FW_NOTIFICATION_IRQ path; see the comment
 * at the former call site in hailo_backend_run. */
#define HAILO_D2H_POST_FAIL_DRAIN_CAP    8u

/* OUT boundary-channel descriptor pre-fill depth. Linux's HailoRT
 * issues 8 sequential single-desc OUTPUT launch_transfer calls
 * before the first INPUT submit (Pi OS wire capture, 2026-04-22:
 * ~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-vdma-mnist-pi5.txt). If fw pre-
 * fetches OUT descriptors ahead of num_avail for pipelining and
 * trips on zero entries at [1..N], pre-filling N descriptors with
 * the same buffer keeps the prefetch window valid even though we
 * only consume one per inference. 8 matches HailoRT's observed
 * count exactly. Track in the Phase 8 follow-up to remove or
 * formalize once the actual mechanism is understood. */
#define HAILO_BOUNDARY_OUT_PREFETCH_DEPTH 8u

/* Phase-boundary wall-clock gaps observed in HailoRT's instrumented
 * MMIO trace (~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-mmio-trace-*-pi5.txt).
 * HailoRT leaves these gaps between major load-sequence RPCs — on
 * SLM-OS we insert matching udelays under HAILO_WIRE_DEBUG to test
 * whether the gaps themselves are load-bearing for #253. They are
 * not (disconfirmed 2026-04-23), but the instrumentation stays for
 * future bisect work. */
#define HAILO_HAILORT_GAP_POST_CLEAR_APPS_US   5000u /* HailoRT: 4.5 ms */
#define HAILO_HAILORT_GAP_POST_DYNAMIC_US      3000u /* HailoRT: 2.8 ms */
#define HAILO_HAILORT_GAP_POST_SETTLE_PINGS_US 2000u /* HailoRT: 1.6 ms */

/* #682 hyp-Q (2026-05-09): minimum-gap pre-RPC pacing for every
 * CORE-CPU control call in the load sequence. Each CORE-CPU RPC
 * touches SAGE1_ISP and fires CPU_ECC_FATAL if fw's async post-RPC
 * init hasn't finished. Linux's HailoRT achieves the gap incidentally
 * via user-space scheduling latency (seconds between RPCs from
 * vstreams library) — we shouldn't replicate the sketch with a fixed
 * unconditional sleep. Instead track the absolute timestamp of the
 * last RPC and only wait the difference needed to reach the floor.
 *
 * Floor calibration (initial guess): 50 ms. The boot-time ECC fix
 * needed 500 ms after BOOT_IRQ; load-time RPCs likely need less
 * because fw's SAGE1_ISP zero-init is mostly done by then. Bisect
 * down once the load completes cleanly without ECCs. The previous
 * uniform 10 ms unconditional delay did NOT suppress load-time
 * ECCs, so 10 ms is known too short. Tracking: #761. */
/* 250 µs floor: matches HailoRT v4.23's measured inter-RPC gap of
 * ~100-300 µs from the MNIST wire capture
 * (~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-wire-capture-mnist-pi5.txt).
 * The 50 ms floor previously used here was justified by "Linux's
 * HailoRT achieves the gap incidentally via user-space scheduling
 * latency (seconds between RPCs from vstreams library)" — that
 * premise is contradicted by the wire capture, which shows Linux
 * fires RPCs ~200 µs apart, not seconds apart. The original 50 vs
 * 200 ms bisect (8b2145ad) never tested below 50 ms; it just compared
 * two large values that were both already 100× slower than Linux. */
#define HAILO_CORE_CPU_SETTLE_FLOOR_US       250u /* 250 us — matches HailoRT */
/*
 * Serialization: this anchor is touched only from `context_switch_load`
 * and `hailo_backend_run`, both of which run under the inference-layer
 * slot lock (one model load + one inference at a time). Every writer
 * also holds `control_lock` for the duration of its CORE-CPU RPC, so
 * the read-modify-stamp dance below is implicitly single-threaded.
 * Do not call `hailo_core_cpu_settle()` from a path that bypasses both
 * locks without revisiting this assumption.
 */
static uint64_t hailo_core_cpu_settle_anchor_ticks = 0;

static bool hailo_core_cpu_settle_armed = false;

static inline void hailo_core_cpu_settle(void)
{
    if (!hailo_platform || !hailo_platform->udelay) return;

    uint64_t freq = timer_get_frequency();
    if (!freq) {
        /* Timer not ready — fall back to unconditional floor wait. */
        hailo_platform->udelay(HAILO_CORE_CPU_SETTLE_FLOOR_US);
        return;
    }

    /* First call after boot: the 500 ms post-BOOT_IRQ settle in
     * `hailo_boot` already gave fw plenty of idle, so don't add
     * another floor wait here. Just arm the anchor for subsequent
     * calls. Use an explicit boolean rather than (anchor_ticks == 0)
     * because CNTPCT_EL0 wraps to zero ~292 years after reset on a
     * 1 GHz timer, which is theoretically a sentinel collision —
     * unreachable on real hardware but trivially avoidable. */
    if (hailo_core_cpu_settle_armed) {
        uint64_t now = timer_get_count();
        if (now > hailo_core_cpu_settle_anchor_ticks) {
            uint64_t elapsed_us = (now - hailo_core_cpu_settle_anchor_ticks)
                                  * 1000000ULL / freq;
            if (elapsed_us < HAILO_CORE_CPU_SETTLE_FLOOR_US) {
                uint32_t wait_us = HAILO_CORE_CPU_SETTLE_FLOOR_US -
                                   (uint32_t)elapsed_us;
                hailo_platform->udelay(wait_us);
            }
        }
    }

    /* Stamp anchor at the start of this RPC. Slightly less precise
     * than stamping at end (RPC-duration is rolled into the gap),
     * but RPC durations are <5 ms (CLEAR_APPS is the longest); the
     * floor is much larger so this is fine. */
    hailo_core_cpu_settle_anchor_ticks = timer_get_count();
    hailo_core_cpu_settle_armed = true;
}

/* Drain pending notifications: for each one, dump it, ACK the buffer,
 * then delay a little so fw can write the next queued event before we
 * re-check. Bounded by `max_events` so we don't loop forever on a fw
 * that re-writes the same event repeatedly. */
void hailo_fw_drain_d2h_notifications(uint32_t max_events)
{
    for (uint32_t i = 0; i < max_events; i++) {
        bool had = hailo_fw_dump_d2h_notification_once();
        if (!had) {
            uart_printf("[d2h] drain: %u events processed, buffer now "
                        "empty\r\n", (unsigned)i);
            return;
        }
        hailo_fw_ack_d2h_notification();
        /* Give fw a moment to write the next event. */
        if (hailo_platform->udelay) {
            hailo_platform->udelay(5000u);  /* 5 ms */
        }
    }
    uart_printf("[d2h] drain: hit max_events=%u cap; more may be queued\r\n",
                (unsigned)max_events);
}

/*
 * IRQ-delivery counters for the notification path. Updated from
 * three call sites (control_msi_handler, wait_for_response,
 * hailo_control_drain_pending_irqs) via hailo_fw_handle_d2h_notification.
 * Read by `hailo state` and tests.
 *
 * Plain uint32_t with __atomic ops — increments race against each
 * other when (e.g.) the MSI fires on CPU N while a polled drain
 * runs on CPU 0. The counts are diagnostic only; eventual
 * consistency is fine.
 */
static uint32_t hailo_notification_irq_count = 0;
static uint32_t hailo_notification_polled_count = 0;

void hailo_irq_delivery_get_counts(struct hailo_irq_delivery_counts *out)
{
    if (!out) return;
    out->notification_irq    = __atomic_load_n(&hailo_notification_irq_count,
                                               __ATOMIC_ACQUIRE);
    out->notification_polled = __atomic_load_n(&hailo_notification_polled_count,
                                               __ATOMIC_ACQUIRE);
}

/*
 * Mirrors Linux's firmware_notification_irq_handler. Reads one
 * event, prints + decodes (existing dump_once helper), then writes
 * zero to in_use so fw can post the next event. Idempotent on an
 * empty buffer — dump_once returns false, we still touch the
 * counter so verification captures still show this path ran.
 *
 * Called from BOTH the MSI ISR (from_irq=true) and the polled
 * fallback paths in wait_for_response /
 * hailo_control_drain_pending_irqs (from_irq=false). The bool tags
 * which counter to bump; otherwise the two call sites are
 * indistinguishable behavior-wise.
 */
void hailo_fw_handle_d2h_notification(bool from_irq)
{
    bool had = hailo_fw_dump_d2h_notification_once();
    if (had) {
        hailo_fw_ack_d2h_notification();
    }
    if (from_irq) {
        __atomic_fetch_add(&hailo_notification_irq_count, 1u,
                           __ATOMIC_ACQ_REL);
    } else {
        __atomic_fetch_add(&hailo_notification_polled_count, 1u,
                           __ATOMIC_ACQ_REL);
    }
}

/* #361 settle pings (2026-05-06): APP-CPU IDENTIFY + GET_DEVICE_
 * INFORMATION pair issued before each CORE-CPU RPC during HEF load.
 * Mirrors HailoRT v4.23's wire-capture cadence (which interleaves
 * APP-CPU pings between every major CORE step). Best-effort: failures
 * are logged and load continues — pings are not protocol-required
 * (every load-RPC return code stays 0 with or without them).
 *
 * Hardware A/B history on pi-5-1:
 *   - On `hailo ctxsmoke full` (synthetic contexts), pings reproducibly
 *     silence the CPU_ECC_FATAL fw fires at CHANGE_STATUS(ENABLED).
 *   - On real `hailo load /mnt/files/user.hef sched` (MNIST), the load
 *     succeeds whether pings fire or are stubbed to no-op — production
 *     load's inter-step DMA + parser work is apparently long enough
 *     that the ENABLED-time event doesn't trip. Pings don't HURT the
 *     production path (still rc=0 everywhere) and matching HailoRT is
 *     cheap insurance against future fw versions, so they stay on. */
static void context_switch_settle_pings(const char *where)
{
    struct hailo_control_identify_response idr;
    int irc = hailo_control_identify(&idr);
    if (irc != HAILO_OK) {
        WARN("hailo backend: pre-%s IDENTIFY rc=%d (continuing)",
             where, irc);
    }
    uint32_t gdi_len = 0;
    int drc = hailo_control_get_device_information(&gdi_len);
    if (drc != HAILO_OK) {
        WARN("hailo backend: pre-%s GET_DEVICE_INFORMATION rc=%d "
             "(continuing)", where, drc);
    }
}

/* -------------------------------------------------------------------------- */
/* Model slot table                                                            */
/* -------------------------------------------------------------------------- */

#define HAILO_MAX_MODELS 4

struct hailo_model_slot {
    bool                      in_use;
    /* Audit F-07 (2026-04-24): inflight_runs lets free_model refuse
     * to tear down a slot whose DMA buffers are currently being used
     * by hailo_backend_run on another CPU. Mutated only under
     * slots_lock; run() inc/dec around its per-call work. */
    uint32_t                  inflight_runs;
    struct hailo_infer_config cfg;
    /* Pad-derived input/output tensor shapes for validate-on-run.
     * Stored as H x W x C to match the HEF pad layout exactly so the
     * caller's inference_tensor_t.shape can be compared 1:1. */
    uint16_t input_shape[3];
    uint16_t output_shape[3];

    /* Context-switch load-time resources (Phase 6.6, #179). Kept in
     * the slot so free_model can release them; load allocates, run()
     * reads nothing from these directly (today). The boundary desc
     * lists were bound to firmware's VDMA channels via ACTIVATION's
     * OpenBoundary actions; a future run() refactor that reuses them
     * instead of allocating fresh lists per-call is a follow-up. */
    bool                          cs_loaded;
    struct hailo_tensor           ccw_tensor;
    struct hailo_vdma_desc_list   ccw_list;
    /* #253 Phase 8: optional second cfg channel for HEFs that split
     * their CCWs across two cfg_channel_index values (MNIST does).
     * cs_loaded HEFs without a split leave these zeroed and the
     * load path skips the dual-channel upload. */
    struct hailo_tensor           ccw_tensor_1;
    struct hailo_vdma_desc_list   ccw_list_1;
    uint16_t                      ccw_num_avail_0;   /* pri chan */
    uint16_t                      ccw_num_avail_1;   /* aux chan */
    bool                          ccw_has_second_channel;
    struct hailo_tensor           boundary_in_tensor;
    struct hailo_vdma_desc_list   boundary_in_list;
    struct hailo_tensor           boundary_out_tensor;
    struct hailo_vdma_desc_list   boundary_out_list;
};

static struct hailo_model_slot slots[HAILO_MAX_MODELS];

/*
 * Serialises slot-table mutations (in_use flag, cfg/shape init and
 * zero-out). load_model / free_model / shutdown all take this lock;
 * run() does NOT — it reads slot->cfg after load_model's IRQ-safe
 * unlock has stored the full config. Readers see either "slot
 * in_use=false" (run bails) or a fully-initialised cfg, never a torn
 * write.
 *
 * IRQ-disabling lock flavour is mandatory because run() is reachable
 * from scheduler-policy paths that can execute with IRQs disabled.
 */
static spinlock_t slots_lock = SPINLOCK_INIT;

/* Phase 8 progress tracker: updated at each cs_load stage, readable by
 * shell command `hailo stage` after a wedge. uart_puts diagnostics
 * corrupt mid-print on real HEFs for reasons unknown, so we use a
 * simple DRAM global instead — the shell recovers post-wedge and can
 * dump this to pinpoint which stage was last entered. Values:
 *   0    = idle / pre-cs_load
 *   10   = entered cs_load, ccw bounds ok
 *   11   = ccw_tensor allocated
 *   12   = ccw memcpy + prepare_for_device done
 *   13   = ccw desc_list allocated + programmed
 *   20   = boundary IN tensor done
 *   21   = boundary OUT tensor done
 *   40   = before translate_application_header
 *   41   = before translate_contexts
 *   42   = translate done
 *   50   = before CHANGE_STATUS(RESET)
 *   51   = RESET ok
 *   52   = before SET_NETWORK_GROUP_HEADER
 *   53   = NG_HEADER ok
 *   60+ix = before SET_CONTEXT_INFO(ix)  (ix = 0..3)
 *   61+ix = SET_CONTEXT_INFO(ix) ok
 *   70   = before CHANGE_STATUS(ENABLED)
 *   71   = ENABLED ok (load_model success)
 */
static volatile int cs_load_stage = 0;

static inline void cs_load_stage_set(int stage)
{
    cs_load_stage = stage;
    __asm__ volatile("dmb ish" ::: "memory");
}

int hailo_backend_get_cs_load_stage(void)
{
    return cs_load_stage;
}

/* Extern-callable setter for the translator (different compilation
 * unit) to mark sub-stage progress inside
 * hailo_cs_translate_contexts. Phase 8 diagnostic only. */
void cs_load_stage_set_raw(int stage)
{
    cs_load_stage_set(stage);
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static int hailo_err_to_inf(int rc)
{
    switch (rc) {
    case HAILO_OK:          return INF_OK;
    case HAILO_ERR_INVAL:   return INF_ERR_INVAL;
    case HAILO_ERR_NODEV:   return INF_ERR_NODEV;
    case HAILO_ERR_NOMEM:   return INF_ERR_NOMEM;
    case HAILO_ERR_TIMEOUT: return INF_ERR_TIMEOUT;
    default:                return INF_ERR_NOSUPPORT;
    }
}

/* Padded dim falls back to the unpadded one when zero. HEFs emit the
 * padded value only when it differs from the base shape. */
static uint32_t pad_dim(uint32_t padded, uint32_t base)
{
    return padded ? padded : base;
}

/* Compute byte count for an H x W x C INT8 tensor. The Hailo device
 * operates on INT8 after DFC quantization, so every pad byte is one
 * element. Returns 0 if any dim is zero (invalid pad). */
static uint32_t pad_bytes(const struct hef_pad_info *p)
{
    if (!p->has_tensor_shape) return 0;
    uint32_t h = pad_dim(p->padded_height,   p->height);
    uint32_t w = pad_dim(p->padded_width,    p->width);
    uint32_t c = pad_dim(p->padded_features, p->features);
    if (!h || !w || !c) return 0;
    /* Overflow check — each pad is realistically KB-scale, but guard
     * anyway so a malicious HEF can't walk through the uint32 size. */
    uint64_t bytes = (uint64_t)h * w * c;
    if (bytes > UINT32_MAX) return 0;
    return (uint32_t)bytes;
}

/* Pick the largest input and output pads across the HEF's pad list.
 * Multi-head vision models have several pads in each direction; we
 * size the DMA buffer to the biggest. Returns 0 on success (and
 * populates the output pointers), -1 if either direction is empty. */
static int pick_largest_pads(const struct hef_info *info,
                             const struct hef_pad_info **in_pad_out,
                             const struct hef_pad_info **out_pad_out,
                             uint32_t *in_bytes_out,
                             uint32_t *out_bytes_out)
{
    const struct hef_pad_info *in_pad = NULL;
    const struct hef_pad_info *out_pad = NULL;
    uint32_t input_bytes = 0, output_bytes = 0;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *p = &info->pads[i];
        uint32_t b = pad_bytes(p);
        if (!b) continue;
        if (p->is_input) {
            if (b > input_bytes) { input_bytes = b; in_pad = p; }
        } else {
            if (b > output_bytes) { output_bytes = b; out_pad = p; }
        }
    }
    if (!input_bytes || !output_bytes || !in_pad || !out_pad) return -1;
    *in_pad_out    = in_pad;
    *out_pad_out   = out_pad;
    *in_bytes_out  = input_bytes;
    *out_bytes_out = output_bytes;
    return 0;
}

/* Upload the HEF's CCW weight stream (best-effort).
 *
 * For v0/v1 HEFs whose CCWs are inline `write_data_ccw` actions,
 * `hailo_control_upload_ccw` writes payloads to sequential firmware
 * addresses — the original Phase 5.3 path.
 *
 * For v2+ HEFs (DFC 3.33.1 scheduler_mlp_pi5.hef), CCWs are
 * `write_data_ccw_ptr` actions pointing into the separate CCWS
 * block. Real delivery on v2+ goes through the
 * CONTEXT_SWITCH_SET_CONTEXT_INFO opcode — firmware's context
 * switcher pulls CCW payloads from host-side DMA buffers. That's a
 * separate major subsystem (not yet implemented); our
 * WRITE_MEMORY-based uploader gets rejected by firmware.
 *
 * Treat failure as non-fatal so the policy path stays exercisable
 * (hailo_infer_run will then time out cleanly when firmware never
 * produces output). This lets `bench sched-policy` run through the
 * chain and observe the timeout, which is the correct signal until
 * the context-switch protocol lands. */
/* -------------------------------------------------------------------------- */
/* Context-switch load path (Phase 6.6, #179)                                   */
/*                                                                              */
/* Replaces the prior "best-effort" WRITE_MEMORY + CONFIG_STREAM flow (which    */
/* firmware v4.23 rejects for v2+ HEFs) with the full 6-step context-switch    */
/* sequence HailoRT uses:                                                        */
/*                                                                              */
/*   1. Allocate + program the CCW DMA buffer + descriptor list; copy weights  */
/*      from the HEF's ccws_offset region into the buffer.                      */
/*   2. Allocate + program boundary input / output DMA buffers + desc lists    */
/*      sized to the HEF's pad shapes.                                           */
/*   3. Build hailo_cs_translate_cfg referencing all three descriptor list     */
/*      IOVAs.                                                                   */
/*   4. Translate application header + 4 context buffers.                       */
/*   5. RPCs: CHANGE_CONTEXT_SWITCH_STATUS(RESET) → SET_NETWORK_GROUP_HEADER   */
/*      → 4 × SET_CONTEXT_INFO → CHANGE_CONTEXT_SWITCH_STATUS(ENABLED).         */
/*   6. Store tensor + list handles in the slot so free_model can reclaim.      */
/*                                                                              */
/* On failure anywhere in the sequence, we unwind whatever allocations already  */
/* landed and return without marking cs_loaded. free_model treats !cs_loaded   */
/* slots as having no context-switch resources to release.                      */
/* -------------------------------------------------------------------------- */

/* CCW + boundary page-size defaults shared between allocation and
 * translate_cfg. The MVP hardcodes both to match ctxsmoke's
 * shell-command sizing on pi-5-1. The matching config-VDMA-channel
 * default lives in hailo_cs_translator.h alongside the boundary-
 * channel offsets and their compile-time range guards. */
#define HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE   512u
/* HailoRT v4.23 wire capture on pi-5-1 shows boundary channels use
 * desc_page_size=512 (pios_ACTIVATION.bin / pios_DYNAMIC.bin: low-16
 * of OpenBoundary.host_buffer_info.desc_page_size = 0x0200). 4096
 * is within the HW 4 KB cap but fw appears to cache the
 * (desc_page_size, total_desc_count) pair declared in
 * OpenBoundary and silently refuse transfers whose host-side geometry
 * disagrees. Matching HailoRT's chosen values byte-for-byte is the
 * safest default. */
#define HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE   512u
/* Output-side boundary desc page size. HailoRT v4.23 on MNIST
 * allocates the output boundary descriptor list at page_size=64
 * (MIN_VDMA_DESCRIPTOR_BUFFER_SIZE) while the input uses 512. See
 * pios_DYNAMIC.bin host_buffer_info.desc_page_size fields for
 * ACTIVATE_BOUNDARY_INPUT vs ACTIVATE_BOUNDARY_OUTPUT — the LE
 * bytes at abs offset 0x22-0x23 of the DYNAMIC stream are
 * 0x40 0x00 (=64) for OUTPUT and 0x00 0x02 (=512) for INPUT.
 * The asymmetry lets tiny output tensors (16 B for MNIST) use the
 * smallest possible descriptor granularity without overcommitting
 * SG table space. Firmware cross-validates the declared page size
 * against the descriptor list's own header and silently wedges
 * the D2H pipe (num_proc stays 0 forever) on a mismatch.
 * Byte-verified on pi-5-1 #253. */
#define HAILO_CS_DEFAULT_BOUNDARY_OUTPUT_PAGE_SIZE  64u
/* HailoRT allocates a 32-entry boundary SG list on v4.23 (host_buffer_
 * info.total_desc_count = 32). Our desc_count_for() rounds up to a
 * power of two anyway, so for MNIST-scale transfers (one 784 B frame
 * in a 512 B page = 2 descs) the floor pushes us to 32 explicitly. */
#define HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT  32u

/* Round `bytes` up to `page_size` and return the descriptor count
 * needed to cover the region, rounded UP to the next power of two
 * (hailo_vdma_desc_list_alloc requires a power-of-2 count — the
 * VDMA engine walks the ring via (index & mask) and non-power-of-2
 * counts would corrupt on wraparound). Minimum 2
 * (HAILO_VDMA_MIN_DESC_COUNT). The VDMA engine's num_avail /
 * num_proc counters are 16-bit, so the maximum count is 65536.
 * Returns 0 if a legitimate round-up exceeds that ceiling — the
 * caller must treat 0 as "HEF too large" (HAILO_ERR_INVAL) rather
 * than silently under-programming the descriptor list. */
static uint32_t desc_count_for(uint32_t bytes, uint16_t page_size)
{
    if (page_size == 0) return 2;
    uint32_t n = (bytes + page_size - 1) / page_size;
    if (n < 2) return 2;
    /* Round up to next power of two, capped at 65536 (the VDMA
     * ring's counter width). If the rounded-up value would exceed
     * the cap, signal failure rather than saturate. */
    uint32_t p = 2;
    while (p < n) {
        if (p >= 0x10000u) return 0;
        p <<= 1;
    }
    return p;
}

/* Release context-switch load resources. Safe to call on a slot with
 * cs_loaded=false (each tensor/list alloc is zero-initialised until
 * populated, and hailo_*_free handles zeroed inputs). */
static void context_switch_unwind(struct hailo_model_slot *slot)
{
    hailo_vdma_desc_list_free(&slot->boundary_out_list);
    hailo_tensor_free(&slot->boundary_out_tensor);
    hailo_vdma_desc_list_free(&slot->boundary_in_list);
    hailo_tensor_free(&slot->boundary_in_tensor);
    if (slot->ccw_has_second_channel) {
        hailo_vdma_desc_list_free(&slot->ccw_list_1);
        hailo_tensor_free(&slot->ccw_tensor_1);
        slot->ccw_has_second_channel = false;
    }
    hailo_vdma_desc_list_free(&slot->ccw_list);
    hailo_tensor_free(&slot->ccw_tensor);
    slot->cs_loaded = false;
}

/* Walk ccw_actions[] and copy every action with a matching
 * cfg_channel_index into the tensor buffer `dst` at consecutive
 * offsets, bounded by `dst_bytes`. Used by the dual-cfg-channel
 * path in context_switch_load to split MNIST's 28 CCW actions
 * into a small (cfg_channel_index=1) buffer and a bulk
 * (cfg_channel_index=0) buffer.
 *
 * Bounds-checks every copy against the source CCWS block (via
 * outer->ccws_size) so a malformed HEF with a large
 * data_offset_in_blob can't OOB-read past the PMM-allocated model
 * blob. Destination bounds stop copying early — any remaining
 * bytes past dst_bytes are silently skipped (the caller sized the
 * tensor to the summed ch_bytes total, so this is defensive
 * against the tally itself overflowing).
 *
 * Returns copied-byte count on success, a HAILO_ERR_* on a source
 * bounds-check failure (in which case *dst contents are undefined
 * and the caller should propagate the error up). */
static int copy_ccws_for_cfg_channel(
    const struct hef_info *info,
    const struct hef_outer_header *outer,
    const void *model,
    uint32_t select_cfg_channel_index,
    uint8_t *dst, uint32_t dst_bytes)
{
    const uint8_t *ccws_base = (const uint8_t *)model + outer->ccws_offset;
    uint32_t off = 0;
    for (uint32_t i = 0; i < info->ccw_action_count; i++) {
        const struct hef_ccw_action *a = &info->ccw_actions[i];
        if (a->cfg_channel_index != select_cfg_channel_index) continue;
        if (off + a->data_size > dst_bytes) break;   /* dst-bounded */
        if (a->data_offset_in_blob > outer->ccws_size
         || a->data_size > outer->ccws_size - a->data_offset_in_blob) {
            WARN("hailo backend: CCW action %u (ch=%u) out-of-bounds "
                 "(offset=%u size=%u ccws_size=%lu)",
                 i, select_cfg_channel_index,
                 a->data_offset_in_blob, a->data_size,
                 (unsigned long)outer->ccws_size);
            return HAILO_ERR_INVAL;
        }
        memcpy(dst + off, ccws_base + a->data_offset_in_blob, a->data_size);
        off += a->data_size;
    }
    return (int)off;
}

#ifdef HAILO_WIRE_DEBUG
/* #682 hyp-7 diagnostic: snapshot fw's BAR4 SRAM (the ATR0-mapped
 * 16 KB control window) at pre-IN-submit and post-timeout, then
 * dword-diff. If proc never advances AND BAR4 contents are unchanged,
 * fw is genuinely doing nothing during the 500 ms wait. If BAR4
 * changes despite proc=0, fw is actively touching memory we don't
 * normally inspect — pinpointing the changed offsets gives the next
 * lead.
 *
 * Static BSS allocation (32 KB) avoids stack pressure and works on
 * any platform where bar4_read is implemented.
 */
#define HAILO_BAR4_SNAP_SIZE 0x4000u
static uint8_t hailo_bar4_snap_pre[HAILO_BAR4_SNAP_SIZE];
static uint8_t hailo_bar4_snap_post[HAILO_BAR4_SNAP_SIZE];

static void hailo_bar4_snap(uint8_t *out)
{
    if (!hailo_platform || !hailo_platform->bar4_read) return;
    hailo_platform->bar4_read(0, out, HAILO_BAR4_SNAP_SIZE);
}

static void hailo_bar4_diff_print(const char *label)
{
    const uint32_t *a = (const uint32_t *)(const void *)hailo_bar4_snap_pre;
    const uint32_t *b = (const uint32_t *)(const void *)hailo_bar4_snap_post;
    size_t words = HAILO_BAR4_SNAP_SIZE / 4;
    int diffs = 0;
    /* Region buckets so we can see the shape at a glance:
     *  +0x0000..0x063F  control RPC
     *  +0x0640..0x0C7F  control response
     *  +0x0C80..0x1FFF  d2h ring + undocumented state
     *  +0x2000..0x2FFF  APP CPU debug log (header + data)
     *  +0x3000..0x3FFF  CORE CPU debug log
     */
    int n_ctrl = 0, n_resp = 0, n_d2h = 0, n_app_log = 0, n_core_log = 0;
    /* Print every diff OUTSIDE the debug log regions (those are
     * fw printf spam, expected). Inside the debug log regions
     * just count. */
    for (size_t i = 0; i < words; i++) {
        if (a[i] != b[i]) {
            unsigned off = (unsigned)(i * 4);
            bool in_app_log  = (off >= 0x2000u && off < 0x3000u);
            bool in_core_log = (off >= 0x3000u && off < 0x4000u);
            if (in_app_log) {
                n_app_log++;
            } else if (in_core_log) {
                n_core_log++;
            } else if (off < 0x640u) {
                n_ctrl++;
            } else if (off < 0xC80u) {
                n_resp++;
            } else {
                n_d2h++;
            }

            if (!in_app_log && !in_core_log) {
                uart_printf("[bar4-diff] %s +0x%04x: 0x%08x -> 0x%08x\r\n",
                            label, off,
                            (unsigned)a[i], (unsigned)b[i]);
            }
            diffs++;
        }
    }
    uart_printf("[bar4-diff] %s buckets ctrl=%d resp=%d d2h+state=%d "
                "app_log=%d core_log=%d  total=%d\r\n",
                label, n_ctrl, n_resp, n_d2h,
                n_app_log, n_core_log, diffs);
}
#endif /* HAILO_WIRE_DEBUG */

/* The full 6-step load sequence. Called from load_model after the
 * slot has been claimed and shapes stored. Returns HAILO_OK on
 * success (slot->cs_loaded=true, resources owned by the slot) or a
 * HAILO_ERR_* on any failure (caller must release the slot). */
static int context_switch_load(struct hailo_model_slot *slot,
                               const struct hef_info *info,
                               const void *model,
                               size_t     model_size,
                               const struct hef_outer_header *outer,
                               const struct hef_pad_info *in_pad,
                               const struct hef_pad_info *out_pad)
{
    int rc;

    /* Bound the CCWS region against the caller-provided blob size.
     * hef_parse_outer_header validates (magic, version, proto range)
     * but does not bound the CCWS region — a malformed HEF with
     * ccws_offset+ccws_size overflowing into the caller's buffer
     * would OOB-read via the memcpy below. Reject such HEFs here. */
    if (outer->ccws_size > 0) {
        if (outer->ccws_offset > model_size
         || outer->ccws_size > model_size - outer->ccws_offset) {
            WARN("hailo backend: HEF CCWS out of bounds "
                 "(offset=%lu size=%lu blob=%lu)",
                 (unsigned long)outer->ccws_offset,
                 (unsigned long)outer->ccws_size,
                 (unsigned long)model_size);
            return HAILO_ERR_INVAL;
        }
    }
    cs_load_stage_set(10);

    /* Step 1: CCW buffer + descriptor list.
     *
     * The MNIST HEF (and likely other real HEFs) tags each CCW action
     * with a cfg_channel_index of 0 or 1. HailoRT opens BOTH cfg
     * channels and DMAs each channel's payloads to its own desc list.
     * Our old single-channel code lumped everything onto the default
     * cfg VDMA channel (HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL=1), so
     * the ~56 KB tagged for logical channel 0 never reached the NN
     * core — fw's TRIGGER_SEQUENCER had nothing to program, clusters
     * never armed, boundary credits never issued (Phase 8 submit
     * blocker, #253).
     *
     * Fix: tally per-channel byte/action counts, allocate two
     * tensors + desc lists (channel 0 = packed_vdma=0 with the bulk
     * microcode, channel 1 = packed_vdma=1 with the secondary
     * payload), and copy each action's payload from the CCWS blob
     * into its matching channel buffer at contiguous offsets.
     *
     * HEFs with a single cfg_channel_index (synthetic tests, or
     * simpler topologies) keep the legacy single-channel path —
     * ccw_has_second_channel stays false. */
    uint32_t ch_bytes[2]  = { 0, 0 };
    uint32_t ch_count[2]  = { 0, 0 };
    const struct hef_info *hef = info;
    for (uint32_t i = 0; i < hef->ccw_action_count; i++) {
        uint32_t ci = hef->ccw_actions[i].cfg_channel_index;
        if (ci <= 1) {
            /* Saturating add — a malformed HEF could otherwise wrap
             * our u32 accumulator and make `use_dual` or the
             * downstream alloc compute undersized buffers. Cap at
             * UINT32_MAX so the tensor allocator refuses cleanly via
             * its existing size guard. */
            uint32_t ds = hef->ccw_actions[i].data_size;
            if (ch_bytes[ci] > UINT32_MAX - ds) {
                ch_bytes[ci] = UINT32_MAX;
            } else {
                ch_bytes[ci] += ds;
            }
            ch_count[ci]++;
        }
    }
    bool use_dual = (ch_count[0] > 0 && ch_count[1] > 0);
    uint32_t ccw_bytes;
    if (use_dual) {
        /* Primary (ccw_tensor) holds cfg_channel_index=1 on PCIe
         * channel 1 (packed=1). Real data size only — fw stops at
         * LAST_DESC_CTRL and padded microcode can trip
         * CPU_ECC_FATAL on the NN-core side. */
        ccw_bytes = ch_bytes[1];
    } else {
        ccw_bytes = (outer->ccws_size > 0) ? outer->ccws_size
                                           : HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE;
    }
    if (ccw_bytes == 0) {
        ccw_bytes = HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE;
    }

    /* Audit F-01 (2026-04-24): CCW tensor + desc list now use the
     * low-DMA allocators (`_low` variants). The hard-bounded ceiling
     * in pi5_dma_alloc_common will fail loudly if the request can't
     * be satisfied below 1 GB. CCW worked from low memory in earlier
     * traces, so this codifies that observation as a contract instead
     * of leaving it to allocator luck. */
    rc = hailo_tensor_alloc_low(ccw_bytes, &slot->ccw_tensor);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CCW tensor alloc failed (rc=%d, bytes=%u)",
             rc, ccw_bytes);
        goto fail;
    }
    cs_load_stage_set(11);

    if (use_dual) {
        /* Primary (ccw_tensor) carries cfg_channel_index=1 data on
         * PCIe channel 1 (packed_vdma=1). Secondary (ccw_tensor_1,
         * allocated below) carries cfg_channel_index=0 bulk data on
         * PCIe channel 0 (packed_vdma=0). See earlier comment. */
        memset(slot->ccw_tensor.cpu_addr, 0, ccw_bytes);
        int copied = copy_ccws_for_cfg_channel(hef, outer, model,
                                               /*cfg_channel_index=*/1,
                                               slot->ccw_tensor.cpu_addr,
                                               ccw_bytes);
        if (copied < 0) { rc = copied; goto fail; }
    } else if (outer->ccws_size > 0) {
        const uint8_t *ccws_base = (const uint8_t *)model + outer->ccws_offset;
        memcpy(slot->ccw_tensor.cpu_addr, ccws_base, outer->ccws_size);
    } else {
        memset(slot->ccw_tensor.cpu_addr, 0, ccw_bytes);
    }
    hailo_tensor_prepare_for_device(&slot->ccw_tensor);
    cs_load_stage_set(12);

    uint32_t ccw_desc_count =
        desc_count_for(ccw_bytes, HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE);
    if (ccw_desc_count == 0) {
        WARN("hailo backend: CCW region too large for VDMA desc list (%u bytes)",
             ccw_bytes);
        rc = HAILO_ERR_INVAL;
        goto fail;
    }
    rc = hailo_vdma_desc_list_alloc_low(ccw_desc_count,
                                        HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE,
                                        /*circular=*/false, &slot->ccw_list);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CCW desc_list alloc failed (rc=%d)", rc);
        goto fail;
    }
    int programmed = hailo_vdma_program_buffer(&slot->ccw_list, 0,
                                               slot->ccw_tensor.iova,
                                               ccw_bytes, /*data_id=*/0);
    if (programmed < 0) {
        WARN("hailo backend: CCW program_buffer failed (rc=%d)", programmed);
        rc = HAILO_ERR_IO;
        goto fail;
    }
    uint16_t ccw_num_avail = (uint16_t)programmed;   /* N descs to submit */
    slot->ccw_num_avail_0 = ccw_num_avail;
    cs_load_stage_set(13);

    /* Step 1b: allocate and program the SECOND cfg-channel buffer
     * carrying the cfg_channel_index=0 bulk payload. Targets PCIe
     * channel 0 (packed_vdma=0) per HailoRT's wire mapping. For
     * MNIST this is ~55 KB of NN-core microcode that the sequencers
     * need before TRIGGER_SEQUENCER can arm the clusters. */
    uint16_t ccw_num_avail_bulk = 0;
    if (use_dual) {
        /* Real CCW byte count. fw stops at the descriptor with
         * LAST_DESC_CTRL flagged (109 descs for 55792 bytes at
         * 512 page), so padding past the real data would have fw
         * pull zero microcode into the NN core at the tail and
         * trip CPU_ECC_FATAL. bytes_in_pattern (advertised to fw
         * via ACTIVATE_CFG_CHANNEL) is the walk-boundary, still
         * advertised as 56320 to match HailoRT. */
        uint32_t bulk_bytes = ch_bytes[0];
        if (bulk_bytes == 0) {
            bulk_bytes = HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE;
        }
        rc = hailo_tensor_alloc_low(bulk_bytes, &slot->ccw_tensor_1);
        if (rc != HAILO_OK) {
            WARN("hailo backend: CCW bulk tensor alloc failed (rc=%d, bytes=%u)",
                 rc, bulk_bytes);
            goto fail;
        }
        /* Arm the unwind flag as soon as ccw_tensor_1 is allocated so
         * any `goto fail` between here and the desc-list programming
         * below still frees the tensor. context_switch_unwind's
         * hailo_tensor_free / hailo_vdma_desc_list_free are NULL-safe,
         * so partially-initialised state (tensor but no list) cleans
         * up correctly. */
        slot->ccw_has_second_channel = true;
        memset(slot->ccw_tensor_1.cpu_addr, 0, bulk_bytes);
        int copied_bulk = copy_ccws_for_cfg_channel(
            hef, outer, model, /*cfg_channel_index=*/0,
            slot->ccw_tensor_1.cpu_addr, bulk_bytes);
        if (copied_bulk < 0) { rc = copied_bulk; goto fail; }
        hailo_tensor_prepare_for_device(&slot->ccw_tensor_1);
        uint32_t bulk_dc =
            desc_count_for(bulk_bytes, HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE);
        if (bulk_dc == 0) {
            WARN("hailo backend: CCW bulk region too large (%u bytes)", bulk_bytes);
            rc = HAILO_ERR_INVAL;
            goto fail;
        }
        rc = hailo_vdma_desc_list_alloc_low(bulk_dc,
                                            HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE,
                                            /*circular=*/false, &slot->ccw_list_1);
        if (rc != HAILO_OK) {
            WARN("hailo backend: CCW bulk desc_list alloc failed (rc=%d)", rc);
            goto fail;
        }
        int prog1 = hailo_vdma_program_buffer(&slot->ccw_list_1, 0,
                                              slot->ccw_tensor_1.iova,
                                              bulk_bytes, /*data_id=*/0);
        if (prog1 < 0) {
            WARN("hailo backend: CCW bulk program_buffer failed (rc=%d)", prog1);
            rc = HAILO_ERR_IO;
            goto fail;
        }
        ccw_num_avail_bulk = (uint16_t)prog1;
        slot->ccw_num_avail_1 = ccw_num_avail_bulk;
        /* ccw_has_second_channel was set immediately after the
         * tensor alloc above so the unwind path catches partial init. */
    }

    /* Step 2: boundary tensors + desc lists — only for pads the HEF
     * marked as stream-bound. Tests HEFs without edge_layers have
     * has_stream_info=false and skip the boundary allocation. */
    uint64_t boundary_in_iova  = 0;
    uint32_t boundary_in_desc_count = 0;
    if (in_pad->has_stream_info && in_pad->core_bytes_per_buffer) {
        /* Phase 8 fix: size to FULL frame bytes, not just a single
         * periph-buffer. For ResNet-18 at 224×224×3, the HEF declares
         * core_bytes_per_buffer=672 and core_buffers_per_frame=224 —
         * a single buffer is one row; a full frame is 150528 bytes.
         * Sizing the boundary DMA tensor to just 672 bytes caused
         * firmware to reject ACTIVATION with
         * HAILO_DATAFLOW_STATUS_INVALID_RECEIVE_COMMUNICATION
         * (0x40130016) because the programmed descriptor list and
         * the frame-size-embedded OpenBoundary action disagreed.
         * Earlier test HEFs were single-buffer (buffers_per_frame=1)
         * so this path wasn't hit. */
        uint32_t bpb    = in_pad->core_bytes_per_buffer;
        uint32_t bpf    = in_pad->core_buffers_per_frame
                            ? in_pad->core_buffers_per_frame : 1u;
        /* Size the DMA buffer to the PERIPH frame (h*w*features for
         * INT8 inputs), not the core-padded size. HailoRT declares
         * host_buffer_info.bytes_in_pattern = periph_frame on the
         * wire; host-side DMA must transfer exactly that many bytes
         * or fw's periph layer waits for a partial tail that never
         * arrives. For MNIST: core bpb*bpf = 32*28 = 896, periph =
         * 28*28*1 = 784. Pre-2026-04-22 we allocated 896 and the
         * submit hung with device-side avail stuck at 0. */
        uint32_t in_bytes = in_pad->has_tensor_shape
            ? (uint32_t)in_pad->height * in_pad->width * in_pad->features
            : bpb * bpf;
        if (in_bytes == 0) in_bytes = bpb * bpf;  /* shape-less fallback */

        /* Phase 8 boundary-submit probe (2026-04-23 — see
         * ~/slmos-ref/derivatives/notes/hailort-trace-findings-vdma-2026-04-23.md):
         * boundary IN/OUT tensors + desc lists go through the low-
         * bias allocator so their IOVAs land in the bottom of
         * physical RAM. Platforms without dma_alloc_low silently
         * fall through to the default path, so the call is safe
         * cross-platform. Per-allocation argument — no global state,
         * safe under concurrent load_model calls. */
        rc = hailo_tensor_alloc_low(in_bytes, &slot->boundary_in_tensor);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary IN tensor alloc failed (rc=%d)", rc);
            goto fail;
        }
        memset(slot->boundary_in_tensor.cpu_addr, 0, in_bytes);
        hailo_tensor_prepare_for_device(&slot->boundary_in_tensor);

        boundary_in_desc_count =
            desc_count_for(in_bytes, HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE);
        if (boundary_in_desc_count == 0) {
            WARN("hailo backend: boundary IN region too large for VDMA desc list (%u)",
                 in_bytes);
            rc = HAILO_ERR_INVAL;
            goto fail;
        }
        /* Floor at HailoRT's chosen 32-entry ring so the host_buffer_
         * info.total_desc_count field we declare to fw matches what
         * HailoRT would have declared (pios_DYNAMIC.bin). */
        if (boundary_in_desc_count < HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT) {
            boundary_in_desc_count = HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT;
        }
        rc = hailo_vdma_desc_list_alloc_low(boundary_in_desc_count,
                                            HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE,
                                            /*circular=*/false,
                                            &slot->boundary_in_list);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary IN desc_list alloc failed (rc=%d)", rc);
            goto fail;
        }
        int prog = hailo_vdma_program_buffer(&slot->boundary_in_list, 0,
                                             slot->boundary_in_tensor.iova,
                                             in_bytes,
                                             HAILO_VDMA_HOST_DMA_DATA_ID);
        if (prog < 0) {
            WARN("hailo backend: boundary IN program_buffer failed (rc=%d)", prog);
            rc = HAILO_ERR_IO;
            goto fail;
        }
        boundary_in_iova = slot->boundary_in_list.iova;
    }
    cs_load_stage_set(20);

    uint64_t boundary_out_iova  = 0;
    uint32_t boundary_out_desc_count = 0;
    if (out_pad->has_stream_info && out_pad->core_bytes_per_buffer) {
        /* Same full-frame sizing as the input path. */
        uint32_t obpb    = out_pad->core_bytes_per_buffer;
        uint32_t obpf    = out_pad->core_buffers_per_frame
                             ? out_pad->core_buffers_per_frame : 1u;
        uint32_t out_bytes = obpb * obpf;
        rc = hailo_tensor_alloc_low(out_bytes, &slot->boundary_out_tensor);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary OUT tensor alloc failed (rc=%d)", rc);
            goto fail;
        }
        memset(slot->boundary_out_tensor.cpu_addr, 0, out_bytes);
        hailo_tensor_prepare_for_device(&slot->boundary_out_tensor);

        boundary_out_desc_count =
            desc_count_for(out_bytes,
                           HAILO_CS_DEFAULT_BOUNDARY_OUTPUT_PAGE_SIZE);
        if (boundary_out_desc_count == 0) {
            WARN("hailo backend: boundary OUT region too large for VDMA desc list (%u)",
                 out_bytes);
            rc = HAILO_ERR_INVAL;
            goto fail;
        }
        /* Floor at HailoRT's chosen 32-entry ring — same reason as
         * the input side. */
        if (boundary_out_desc_count < HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT) {
            boundary_out_desc_count = HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT;
        }
        rc = hailo_vdma_desc_list_alloc_low(boundary_out_desc_count,
                                            HAILO_CS_DEFAULT_BOUNDARY_OUTPUT_PAGE_SIZE,
                                            /*circular=*/false,
                                            &slot->boundary_out_list);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary OUT desc_list alloc failed (rc=%d)", rc);
            goto fail;
        }
        int prog = hailo_vdma_program_buffer(&slot->boundary_out_list, 0,
                                             slot->boundary_out_tensor.iova,
                                             out_bytes,
                                             HAILO_VDMA_HOST_DMA_DATA_ID);
        if (prog < 0) {
            WARN("hailo backend: boundary OUT program_buffer failed (rc=%d)", prog);
            rc = HAILO_ERR_IO;
            goto fail;
        }
        boundary_out_iova = slot->boundary_out_list.iova;
    }
    cs_load_stage_set(21);

    /* Step 3: translate_cfg. The boundary IOVA fields are zero if the
     * HEF has no boundary edge of that direction; translate_activation
     * skips OpenBoundary emission for pads without has_stream_info,
     * so the two views stay consistent. */
    struct hailo_cs_translate_cfg tcfg = {
        .config_vdma_channel              = HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL,
        /* HailoRT's wire uses stream_index = packed_vdma_channel_id
         * (packed=1 sidx=1, packed=0 sidx=0). Single-channel path
         * retains sidx=0; dual-channel primary (packed=1, small
         * payload = cfg_channel_index=1 on HailoRT's side) uses
         * sidx=1. */
        .config_stream_index              = slot->ccw_has_second_channel
                                                ? 1u : 0u,
        .ccw_desc_list_iova               = slot->ccw_list.iova,
        .ccw_desc_page_size               = HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE,
        /* HailoRT advertises total_desc_count / bytes_in_pattern on
         * ACTIVATE_CFG_CHANNEL independently of our local desc list
         * size (which rounds up to power-of-2). For the MNIST HEF
         * the exact numbers are: bulk = 110 descs / 56320 bytes,
         * small = 7 descs / 1024 bytes. Matching those byte-for-
         * byte removes another set of wire diffs vs pios_
         * PRELIMINARY.bin. Non-dual loads keep the old behaviour. */
        .ccw_total_desc_count             = slot->ccw_has_second_channel
                                                ? 7u : ccw_desc_count,
        .ccw_bytes_in_pattern             = slot->ccw_has_second_channel
                                                ? 1024u : 0u,
        /* FETCH_CFG_CHANNEL_DESCRIPTORS count fw is instructed to
         * issue for the bulk microcode pull. HailoRT picks 109
         * (the real data size, not the padded list size). */
        .ccw_fetch_bulk_desc_count        = slot->ccw_has_second_channel
                                                ? 109u : 0u,
        /* Second cfg channel (bulk microcode on PCIe channel 0,
         * sidx=0). Left zero unless this HEF's CCWs split across 2
         * cfg_channel_index values — see use_dual above. */
        .cfg_channel_1_packed_vdma        = 0u,
        .cfg_channel_1_stream_index       = 0u,
        .cfg_channel_1_desc_list_iova     = slot->ccw_has_second_channel
                                                ? slot->ccw_list_1.iova : 0u,
        .cfg_channel_1_total_desc_count   = slot->ccw_has_second_channel
                                                ? 126u : 0u,
        .cfg_channel_1_bytes_in_pattern   = slot->ccw_has_second_channel
                                                ? 56320u : 0u,
        .boundary_input_desc_list_iova    = boundary_in_iova,
        .boundary_input_total_desc_count  = boundary_in_desc_count,
        .boundary_output_desc_list_iova   = boundary_out_iova,
        .boundary_output_total_desc_count = boundary_out_desc_count,
        .boundary_desc_page_size          = HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE,
        .boundary_output_desc_page_size   =
            HAILO_CS_DEFAULT_BOUNDARY_OUTPUT_PAGE_SIZE,
    };

    /* Step 4: translate. */
    cs_load_stage_set(40);
    struct hailo_cs_application_header hdr;
    rc = hailo_cs_translate_application_header(info, &tcfg, &hdr);
    if (rc != HAILO_OK) {
        WARN("hailo backend: translate_application_header failed (rc=%d)", rc);
        goto fail;
    }
    /* bufs is ~2 KB — large but still fits on the 16 KB kernel stack
     * alongside hef_info (~1.2 KB) and the caller's frame. A prior
     * version used a file-scope static here and claimed slots_lock
     * protection, but slots_lock is released before context_switch_load
     * runs (load_model only holds it to claim the slot index) — two
     * concurrent loads would have raced through a shared static.
     * Stack-local is the simplest fix and keeps this function re-
     * entrant. Tensor + desc_list allocations earlier in this call
     * already consume kernel-stack frame; the extra 2 KB is budgeted. */
    cs_load_stage_set(41);
    struct hailo_cs_context_buffers cs_bufs;
    rc = hailo_cs_translate_contexts(info, &tcfg, &cs_bufs);
    if (rc != HAILO_OK) {
        WARN("hailo backend: translate_contexts failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(42);

    /* Step 5: RPC sequence. Each must succeed; abort on any failure.
     *
     * Phase 8 fix: inserted CLEAR_CONFIGURED_APPS + GET_HW_CONSTS
     * between RESET and SET_NETWORK_GROUP_HEADER. `hailo ctxsmoke`
     * does this handshake and its SET_CONTEXT_INFO calls return
     * rc=0. load_model skipped it and every real-HEF load failed
     * at SET_CONTEXT_INFO(ACTIVATION) with major=0x40130016
     * (HAILO_DATAFLOW_STATUS_INVALID_RECEIVE_COMMUNICATION). The
     * memory note for the CORE CPU 0xFFFFFFFF wedge resolution
     * (2026-04-20) confirmed this: v4.23 firmware requires the
     * pre-configure handshake before accepting network-group-
     * level setup. */
    cs_load_stage_set(50);
    /* Pre-RESET settle: pings (#361) warm up fw state via APP-CPU
     * IDENTIFY + GET_DEVICE_INFO; the wall-clock floor (#682 hyp-Q)
     * then guarantees a minimum gap to the RESET RPC. The two are
     * complementary — pings give fw a chance to do init work; the
     * floor catches paths where the pings themselves return faster
     * than the floor. */
    context_switch_settle_pings("RESET");
    /* HailoRT v4.23 issues GDI TWICE before RESET (mobilenet wire
     * capture at t=393.529161 + 393.529358, 197 us apart, between the
     * initial IDENTIFY and CHANGE_STATUS(RESET) at 393.529554). The
     * shared settle_pings helper only emits 1 GDI; add a second here
     * to match the reference cadence specifically pre-RESET. */
    {
        uint32_t gdi_len = 0;
        int drc2 = hailo_control_get_device_information(&gdi_len);
        if (drc2 != HAILO_OK) {
            WARN("hailo backend: pre-RESET second GDI rc=%d (continuing)",
                 drc2);
        }
    }
    hailo_core_cpu_settle();
    rc = hailo_control_change_context_switch_status(
            HAILO_CS_STATE_RESET,
            HAILO_CS_IGNORE_APPLICATION_INDEX,
            /*batch_size=*/0, /*batch_count=*/0);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CHANGE_CONTEXT_SWITCH_STATUS(RESET) failed (rc=%d)", rc);
        goto fail;
    }
#ifdef HAILO_WIRE_DEBUG
    /* #682 checkpoint 2: IN ch=2 base register right after RESET RPC.
     * RESET should clear all channel state — if avail!=0 here, RESET
     * is not actually clearing the host-side mirror. */
    hailo_vdma_dump_channel_regs(2, "[682-cp2] post RESET");
    /* #253 (2026-04-23) ECC bisect: drain D2H mailbox between every
     * step of the load so we can see exactly which RPC triggers the
     * CPU_ECC_FATAL event (memory_bitmap=0x1000). The drain is cheap
     * (~5 ms when empty) but 6× per load adds ~30 ms per load in
     * default builds — hence the HAILO_WIRE_DEBUG gate. */
    uart_printf("[bisect] post RESET:\r\n");
    hailo_fw_drain_d2h_notifications(2);
    hailo_vdma_snap_channels("post-RESET");
#endif
    cs_load_stage_set(51);

    /* Pre-configure handshake. CLEAR_CONFIGURED_APPS drops any apps
     * the firmware had registered from a prior load; GET_HW_CONSTS
     * reads hardware constants firmware needs to have handy before it
     * can validate subsequent context-switch bytes.
     *
     * No APP-CPU pings between RESET and CLEAR_APPS. HailoRT v4.23's
     * MobileNet wire capture
     * (~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-wire-capture-mobilenet.txt)
     * shows CLEAR_APPS issued IMMEDIATELY after CHANGE_STATUS(RESET) at
     * t=393.529554/393.529681 — 127 µs apart, no RPC between. The
     * settle_pings call previously here was a SLM-OS-specific
     * divergence (#1003 disconfirmed it as the SAGE1_MIPI_RX_13 ECC
     * trigger, but it's still an unjustified divergence). The wall-
     * clock floor from the prior `hailo_core_cpu_settle()` is also
     * dropped — HailoRT's 127 µs gap is far below SLM-OS's
     * HAILO_CORE_CPU_SETTLE_FLOOR_US, so the floor wasn't matching the
     * reference either. */
    /* #682 (2026-05-09): time CLEAR_APPS — Linux's response is 4.4 ms
     * (longest single RPC in init); if SLM-OS's is much shorter, fw
     * isn't doing the same internal work (likely SAGE init). */
    uint64_t t_clear_start = timer_get_count();
    rc = hailo_control_context_switch_clear_configured_apps();
    uint64_t t_clear_end = timer_get_count();
    uint64_t clear_us = (t_clear_end - t_clear_start) * 1000000ULL
                         / timer_get_frequency();
    uart_printf("[hailo] CLEAR_CONFIGURED_APPS rc=%d latency=%lu us "
                "(Linux baseline: ~4400 us)\r\n",
                rc, (unsigned long)clear_us);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CLEAR_CONFIGURED_APPS failed (rc=%d)", rc);
        goto fail;
    }
#ifdef HAILO_WIRE_DEBUG
    uart_printf("[bisect] post CLEAR_CONFIGURED_APPS:\r\n");
    hailo_fw_drain_d2h_notifications(2);
    hailo_vdma_snap_channels("post-CLEAR_APPS");
    /* #253 (2026-04-23): HailoRT's wire capture shows a 4.5 ms wall-
     * clock gap after CLEAR_APPS before the next RPC. Try matching. */
    if (hailo_platform && hailo_platform->udelay) {
        hailo_platform->udelay(HAILO_HAILORT_GAP_POST_CLEAR_APPS_US);
    }
#endif
    cs_load_stage_set(52);

    /* #361 (2026-05-06): HailoRT v4.23 calls GET_HW_CONSTS four times
     * during HEF load (three direct callers in resource_manager_
     * builder.cpp + one whose source we haven't pinned). SLM-OS calls
     * it once. Hypothesis: does mirroring the 4× pattern stop the
     * CPU_ECC_FATAL events fw fires on RPCs after RESET? Tested via
     * `hailo ctxsmoke full hwc4` — disconfirmed. CPU_ECC_FATAL still
     * fires at CHANGE_STATUS(ENABLED) and additional ECC events fire
     * during the GET_HW_CONSTS sequence itself. The count is not
     * load-bearing; next concrete delta is APP-CPU settle pings. */
    context_switch_settle_pings("GET_HW_CONSTS");
    uint32_t hw_consts_len = 0;
    /* HailoRT v4.23 calls GET_HW_CONSTS FOUR times back-to-back during
     * init. Verified across both wire captures
     * (~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-wire-capture-{mnist-pi5,mobilenet}.txt):
     * 4 occurrences of `00 00 00 48` at common_header opcode position,
     * back-to-back with no other CORE/APP RPCs interleaved.
     *
     * Previous comment claimed "SIX times" citing a "Pi OS inference
     * trace 2026-05-09" — that source isn't reproducible from the
     * checked-in capture files; the actual wire evidence is 4×. The
     * `_iter < 4u` count below is bounded by the wire-capture
     * ground truth. */
    for (uint32_t hw_consts_iter = 0; hw_consts_iter < 4u; hw_consts_iter++) {
        hailo_core_cpu_settle();
        uint64_t t_hw_start = timer_get_count();
        rc = hailo_control_get_hw_consts(&hw_consts_len);
        uint64_t t_hw_end = timer_get_count();
        uint64_t hw_us = (t_hw_end - t_hw_start) * 1000000ULL
                         / timer_get_frequency();
        uart_printf("[hailo] GET_HW_CONSTS [%u/4] rc=%d resp_len=%u "
                    "latency=%lu us\r\n",
                    (unsigned)(hw_consts_iter + 1u), rc,
                    (unsigned)hw_consts_len, (unsigned long)hw_us);
        if (rc != HAILO_OK) {
            WARN("hailo backend: GET_HW_CONSTS [%u/4] failed (rc=%d)",
                 (unsigned)(hw_consts_iter + 1u), rc);
            goto fail;
        }
#ifdef HAILO_WIRE_DEBUG
        /* Phase 8 #253 / #682: dump the raw response body for off-line
         * decode. Runs AFTER t_hw_end so the ~21 ms of UART output
         * doesn't contaminate the latency measurement (the Linux
         * baseline is ~165 µs per call; mixing the dump into the timed
         * window made our calls look 100× slower than they actually
         * were). HailoRT v4.23 control_protocol.h declares the body as
         * fifo_word_granularity_bytes (u32 BE) + max_periph_buffers_per_frame
         * (u16) + max_periph_bytes_per_buffer (u16) + max_acceptable_bytes
         * (u16) + outbound_data_stream_size (u32) + should_optimize_credits
         * (u8) + default_initial_credit_size (u32), each as a 4-B
         * BE-length-prefixed param. Only the last iteration is dumped
         * to keep the boot log short — fw response is identical across
         * the 4 calls per the HailoRT wire capture. */
        if (hw_consts_iter == 3u && hw_consts_len > 0) {
            const uint8_t *body = NULL;
            uint32_t       body_cap = 0;
            hailo_control_get_hw_consts_response_body(&body, &body_cap);
            uint32_t body_len = hw_consts_len < body_cap
                                ? hw_consts_len : body_cap;
            uart_printf("[hw_consts] response body %u bytes:\r\n",
                        (unsigned)body_len);
            for (uint32_t i = 0; body && i < body_len; i += 16) {
                uart_printf("[hw_consts] [%03x]:", (unsigned)i);
                uint32_t end = (i + 16 > body_len) ? body_len : (i + 16);
                for (uint32_t j = i; j < end; j++) {
                    uart_printf(" %02x", body[j]);
                }
                uart_printf("\r\n");
            }
        }
#endif
    }
#ifdef HAILO_WIRE_DEBUG
    uart_printf("[bisect] post GET_HW_CONSTS:\r\n");
    hailo_fw_drain_d2h_notifications(2);
    hailo_vdma_snap_channels("post-GET_HW_CONSTS");
#endif
    cs_load_stage_set(53);

    context_switch_settle_pings("SET_NETWORK_GROUP_HEADER");
    hailo_core_cpu_settle();
    rc = hailo_control_set_network_group_header(&hdr);
    if (rc != HAILO_OK) {
        WARN("hailo backend: SET_NETWORK_GROUP_HEADER failed (rc=%d)", rc);
        goto fail;
    }
#ifdef HAILO_WIRE_DEBUG
    uart_printf("[bisect] post SET_NETWORK_GROUP_HEADER:\r\n");
    hailo_fw_drain_d2h_notifications(2);
    hailo_vdma_snap_channels("post-NET_GROUP_HDR");
#endif
    cs_load_stage_set(53);

    const struct {
        enum hailo_cs_context_type type;
        const uint8_t *bytes;
        uint32_t       len;
        const char    *name;
    } ctxs[] = {
        { HAILO_CS_CONTEXT_TYPE_ACTIVATION,      cs_bufs.activation,
          (uint32_t)cs_bufs.activation_len,      "ACTIVATION" },
        { HAILO_CS_CONTEXT_TYPE_BATCH_SWITCHING, cs_bufs.batch_switching,
          (uint32_t)cs_bufs.batch_switching_len, "BATCH_SWITCHING" },
        { HAILO_CS_CONTEXT_TYPE_PRELIMINARY,     cs_bufs.preliminary,
          (uint32_t)cs_bufs.preliminary_len,     "PRELIMINARY" },
        { HAILO_CS_CONTEXT_TYPE_DYNAMIC,         cs_bufs.dynamic,
          (uint32_t)cs_bufs.dynamic_len,         "DYNAMIC" },
    };
    /* #253 diagnostic: surface per-context action-list lengths so we
     * can tell at a glance whether e.g. DYNAMIC is empty (just the
     * APPLICATION_CHANGE_INTERRUPT tail, ~5 bytes) vs populated with
     * compute-driving actions (ENABLE_LCU, AllowInputDataflow, ...).
     * An empty DYNAMIC would mean fw has no recipe for driving the
     * compute side of the inference, which stalls boundary submits. */
    uart_printf("[cs] ctx bytes: activation=%u batch_switching=%u "
                "preliminary=%u dynamic=%u; hef ctx_actions_count=%u "
                "action_count[0]=%u allow_input_dataflow_count=%u "
                "enable_lcu_count=%u trigger_seq_count=%u "
                "wait_seq_count=%u disable_lcu_count=%u\r\n",
                (unsigned)cs_bufs.activation_len,
                (unsigned)cs_bufs.batch_switching_len,
                (unsigned)cs_bufs.preliminary_len,
                (unsigned)cs_bufs.dynamic_len,
                (unsigned)info->context_actions_count,
                (unsigned)(info->context_actions_count > 0
                    ? info->context_actions[0].action_count : 0),
                (unsigned)info->allow_input_dataflow_count,
                (unsigned)info->enable_lcu_count,
                (unsigned)info->trigger_sequencer_count,
                (unsigned)info->wait_sequencer_count,
                (unsigned)info->disable_lcu_count);
    if (info->context_actions_count > 0) {
        uart_printf("[cs] dynamic action_types[0..]: ");
        for (uint32_t i = 0; i < info->context_actions[0].action_count
                                 && i < 16; i++) {
            uart_printf("%u ", (unsigned)info->context_actions[0].action_types[i]);
        }
        uart_printf("\r\n");
    }
    for (uint32_t i = 0; i < info->allow_input_dataflow_count && i < 4; i++) {
        const struct hef_allow_input_dataflow_action *a =
            &info->allow_input_dataflow_actions[i];
        uart_printf("[cs] allow_input_dataflow[%u]: ctx=%u sys_index=%u\r\n",
                    (unsigned)i, (unsigned)a->context_index,
                    (unsigned)a->sys_index);
    }
#ifdef HAILO_WIRE_DEBUG
    /* Dump each CS context for byte-level diff against
     * ~/slmos-ref/derivatives/hailort-traces/pios_{ACTIVATION,BATCH_SWITCHING,PRELIMINARY,
     * DYNAMIC}.bin. Gated behind HAILO_WIRE_DEBUG (CMake option,
     * default ON while Phase 8 #253 submit blocker is open). Release
     * kernels built with HAILO_WIRE_DEBUG=OFF skip this block. */
    {
        const struct { const char *tag; const uint8_t *buf; uint32_t len; } ctxdumps[] = {
            { "act", cs_bufs.activation,      cs_bufs.activation_len      },
            { "bs",  cs_bufs.batch_switching, cs_bufs.batch_switching_len },
            { "pre", cs_bufs.preliminary,     cs_bufs.preliminary_len     },
            { "dyn", cs_bufs.dynamic,         cs_bufs.dynamic_len         },
        };
        for (uint32_t c = 0; c < sizeof(ctxdumps)/sizeof(ctxdumps[0]); c++) {
            uint32_t dump_len = ctxdumps[c].len;   /* full context */
            for (uint32_t off = 0; off < dump_len; off += 16) {
                uart_printf("[cs] %s[%03x]:", ctxdumps[c].tag, (unsigned)off);
                for (uint32_t j = 0; j < 16 && off + j < dump_len; j++) {
                    uart_printf(" %02x", ctxdumps[c].buf[off + j]);
                }
                uart_printf("\r\n");
            }
        }
    }
#endif /* HAILO_WIRE_DEBUG */

    for (uint32_t i = 0; i < sizeof(ctxs) / sizeof(ctxs[0]); i++) {
        cs_load_stage_set(60 + (int)i * 2);     /* 60, 62, 64, 66 per context */
        context_switch_settle_pings(ctxs[i].name);
        hailo_core_cpu_settle();
        rc = hailo_control_set_context_info(ctxs[i].type,
                                            ctxs[i].bytes, ctxs[i].len);
        if (rc != HAILO_OK) {
            /* Encode (rpc_index, rc) so `hailo stage` shows both.
             * 800_000 + ctx_index*1000 + |rc| — e.g.,
             * 800004 = SET_CONTEXT_INFO(0) returned rc=-4 (TIMEOUT). */
            int abs_rc = (rc < 0) ? -rc : rc;
            cs_load_stage_set(800000 + (int)i * 1000 + abs_rc);
            WARN("hailo backend: SET_CONTEXT_INFO(%s) failed (rc=%d)",
                 ctxs[i].name, rc);
            goto fail;
        }
#ifdef HAILO_WIRE_DEBUG
        uart_printf("[bisect] post SET_CONTEXT_INFO(%s):\r\n", ctxs[i].name);
        hailo_fw_drain_d2h_notifications(2);
        {
            const char *snap_labels[] = {
                "post-CTX_ACTIVATION", "post-CTX_BATCH_SWITCHING",
                "post-CTX_PRELIMINARY", "post-CTX_DYNAMIC"
            };
            hailo_vdma_snap_channels(
                (i < 4u) ? snap_labels[i] : "post-CTX_?");
        }
#endif
        cs_load_stage_set(61 + (int)i * 2);

    }

#ifdef HAILO_WIRE_DEBUG
    /* #253 (2026-04-23): 2.8 ms wall-clock gap in HailoRT's wire
     * capture between the 4th SET_CONTEXT_INFO (DYNAMIC) and the next
     * RPC. Candidate: fw's CORE task is still finishing DYNAMIC's
     * AllowInputDataflow action-list processing (the action that
     * arms the boundary dataflow scheduler). Firing CHANGE_STATUS
     * (ENABLED) before that completes may leave the scheduler in an
     * incomplete state and never issue boundary credits. Tested and
     * disconfirmed — kept under HAILO_WIRE_DEBUG for future bisects. */
    if (hailo_platform && hailo_platform->udelay) {
        hailo_platform->udelay(HAILO_HAILORT_GAP_POST_DYNAMIC_US);
    }
#endif

    /* #682 hyp-F (2026-05-08): Pi OS HailoRT kprobe shows 4 APP-CPU
     * settle pings (GDI, ID, GDI, ID) BETWEEN SET_CONTEXT_INFO(DYNAMIC)
     * and CHANGE_STATUS(ENABLED). Tested unconditionally on pi-5-1 —
     * disconfirmed (ch=2 still stalls with proc=0). Reverted. */

    cs_load_stage_set(70);                      /* about to CHANGE_STATUS(ENABLED) */
    context_switch_settle_pings("CHANGE_STATUS_ENABLED");
    /* Pi OS wire capture (2026-04-22, HailoRT v4.23.0 MNIST on pi-5-1
     * instrumented driver — see ~/slmos-ref/hailo/hailort-v4.23.0-wire-
     * capture-mnist-pi5.txt, CHANGE_STATUS #2 body):
     *   state=ENABLED, app_index=0, batch_size=0, batch_count=0
     * batch_size=0 = CONTROL_PROTOCOL__IGNORE_DYNAMIC_BATCH_SIZE (use
     * pre-configured). batch_count=0 = CONTROL_PROTOCOL__INIFINITE_
     * BATCH_COUNT — fw will keep processing submits until CHANGE_STATUS
     * (RESET). Previously SLM-OS sent (1,1), which told fw "process
     * exactly 1 batch of 1 and stop"; fw's action-list processor then
     * gated the boundary dataflow on a state machine transition that
     * never fired from our host writes, leaving num_proc pinned at 0
     * on the boundary channels. */
    hailo_core_cpu_settle();
    rc = hailo_control_change_context_switch_status(
            HAILO_CS_STATE_ENABLED,
            /*application_index=*/0,
            /*batch_size=*/0, /*batch_count=*/0);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CHANGE_CONTEXT_SWITCH_STATUS(ENABLED) failed (rc=%d)", rc);
        goto fail;
    }
#ifdef HAILO_WIRE_DEBUG
    /* #682 checkpoint 3: IN ch=2 base register right after
     * CHANGE_STATUS(ENABLED). If avail becomes non-zero between
     * checkpoints 2 and 3, the ACTIVATION/ENABLED firmware path
     * is programming the host-side base register on our behalf. */
    hailo_vdma_dump_channel_regs(2, "[682-cp3] post ENABLED");
    uart_printf("[bisect] post CHANGE_STATUS(ENABLED):\r\n");
    hailo_fw_drain_d2h_notifications(2);
    hailo_vdma_snap_channels("post-ENABLED");

    /* #253 (2026-04-23): HailoRT's wire capture shows 2× (GET_DEVICE_INFO
     * + IDENTIFY) interleaved after CHANGE_STATUS(ENABLED) and before the
     * first boundary submit. SLM-OS previously went straight from ENABLED
     * to CCW upload + submit. Tested this pattern in case fw needs a
     * specific settle handshake before accepting boundary credits —
     * disconfirmed but kept here so the bisect infrastructure stays
     * intact for future investigations. */
    {
        struct hailo_control_identify_response idr;
        uint32_t gdi_len = 0;
        int r1 = hailo_control_get_device_information(&gdi_len);
        int r2 = hailo_control_identify(&idr);
        int r3 = hailo_control_get_device_information(&gdi_len);
        int r4 = hailo_control_identify(&idr);
        uart_printf("[settle] post-ENABLED pings: GDI=%d ID=%d GDI=%d ID=%d\r\n",
                    r1, r2, r3, r4);
        uart_printf("[bisect] post-ENABLED settle pings:\r\n");
        hailo_fw_drain_d2h_notifications(2);
    }
    /* #253 (2026-04-23): 1.6 ms wall-clock gap in HailoRT between the
     * last settle ping and the next CHANGE_STATUS. Match it. */
    if (hailo_platform && hailo_platform->udelay) {
        hailo_platform->udelay(HAILO_HAILORT_GAP_POST_SETTLE_PINGS_US);
    }
#endif /* HAILO_WIRE_DEBUG */

    /* #253: CHANGE_STATUS(ENABLED) returns synchronously but fw's
     * action-list processing (ACTIVATION → BATCH_SWITCHING →
     * PRELIMINARY → DYNAMIC) continues asynchronously. Fw programs
     * the CFG channel's regs (CONTROL=START, depth, iova) while
     * processing ACTIVATION/PRELIMINARY — before ENABLED the channel
     * is all zeros, writes don't stick.
     *
     * Poll CFG ch base_dword until CONTROL=START lights up, then
     * write num_avail = ccw_num_avail so the engine fetches the
     * programmed descriptors. Wait for num_proc to catch up: when
     * it equals num_avail the CCW DMA pull is complete, which is
     * what PRELIMINARY's FETCH_CFG_CHANNEL_DESCRIPTORS was waiting
     * for. Only then is fw's inference pipeline ready to process
     * the first boundary submit. Skipping this step leaves fw
     * blocked behind unloaded CCWs and every H2D submit times out
     * with num_proc=0. */
    cs_load_stage_set(72);
    {
        /* Drain BOTH cfg channels when we're in dual-channel mode.
         * PCIe channel 0 carries the bulk microcode, channel 1 the
         * small secondary payload — both need num_avail written and
         * num_proc polled before fw's PRELIMINARY can finish. Order
         * doesn't matter (separate VDMA engines) but wait for the
         * bulk channel first so the NN-core microcode lands before
         * fw's TRIGGER_SEQUENCER runs. */
        if (slot->ccw_has_second_channel) {
            /* On the bulk channel fw's PRELIMINARY itself issues two
             * FETCH_CFG_CHANNEL_DESCRIPTORS REPEATED groups (1 desc
             * handshake + bulk_desc_count-1 for the microcode), so
             * the channel DMA is driven by those actions rather than
             * by a host-side num_avail write. Just wait for the
             * channel's num_proc to reach bulk_desc_count — that's
             * the signal the microcode has fully landed and the
             * NN-core sequencers have their program loaded. */
            const uint8_t bulk_ch = 0;
            int arm0 = hailo_vdma_channel_wait_armed(bulk_ch, 500000u);
            if (arm0 != HAILO_OK) {
                WARN("hailo backend: CFG bulk ch=%u never armed (rc=%d)",
                     bulk_ch, arm0);
                cs_load_stage_set(830072);
                rc = arm0;
                goto fail;
            }
            int sub0 = hailo_vdma_channel_wait_proc(
                bulk_ch, ccw_num_avail_bulk, 2000000u);
            if (sub0 != HAILO_OK) {
                WARN("hailo backend: CFG bulk wait_proc (target=%u) "
                     "failed (rc=%d)", (unsigned)ccw_num_avail_bulk, sub0);
                cs_load_stage_set(830000 + (sub0 < 0 ? -sub0 : sub0));
                rc = sub0;
                goto fail;
            }
        }

        const uint8_t cfg_ch = HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL;
        int arm_rc = hailo_vdma_channel_wait_armed(cfg_ch, 500000u /* 500 ms */);
        if (arm_rc != HAILO_OK) {
            WARN("hailo backend: CFG channel %u never armed after "
                 "ENABLED (rc=%d)", cfg_ch, arm_rc);
            cs_load_stage_set(830072);
            rc = arm_rc;
            goto fail;
        }
        cs_load_stage_set(73);

        int sub_rc = hailo_vdma_submit_and_wait(
            cfg_ch, ccw_num_avail, 2000000u /* 2 s */);
        if (sub_rc != HAILO_OK) {
            WARN("hailo backend: CFG channel submit (avail=%u) "
                 "failed (rc=%d)", (unsigned)ccw_num_avail, sub_rc);
            cs_load_stage_set(830000 + (sub_rc < 0 ? -sub_rc : sub_rc));
            rc = sub_rc;
            goto fail;
        }
        cs_load_stage_set(74);
    }

#ifdef HAILO_WIRE_DEBUG
    uart_printf("[bisect] post CCW DMA pull:\r\n");
    hailo_fw_drain_d2h_notifications(2);

    /* #682 hypothesis-1: read back per-channel IRQ-enable registers
     * after fw has executed ACTIVATE_BOUNDARY_INPUT/OUTPUT and the
     * full PRELIMINARY arming sequence. Compare to the post-boot-arm
     * baseline — if any of the 32 SRC/DST bits flipped, fw is
     * clearing them on us. */
    hailo_control_dump_irq_state("post-load");
    /* #682 hypothesis-6: re-check the trained link speed after the
     * full load completes. A step-down between post-boot-arm and
     * post-load indicates the link drops during context-switch RPCs. */
    hailo_platform_log_link_state("post-load");
#endif

    /* #682 hyp-F2 (2026-05-08): Pi OS HailoRT kprobe placed the
     * second set of (GDI, ID, GDI, ID) AFTER CCW upload and BEFORE
     * the first inference. Tested at this position — also
     * disconfirmed (ch=2 still stalls). Reverted. */

    cs_load_stage_set(71);
    INFO("hailo backend: context-switch load OK (CCW=%u B, IN=%u B, OUT=%u B)",
         ccw_bytes,
         in_pad->has_stream_info  ? in_pad->core_bytes_per_buffer  : 0u,
         out_pad->has_stream_info ? out_pad->core_bytes_per_buffer : 0u);
    slot->cs_loaded = true;
    return HAILO_OK;

fail:
    context_switch_unwind(slot);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* ops                                                                         */
/* -------------------------------------------------------------------------- */

static int hailo_backend_init(struct inference_device *dev)
{
    (void)dev;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);
    return INF_OK;
}

static void hailo_backend_shutdown(struct inference_device *dev)
{
    (void)dev;
    /* Two-phase: copy out the DMA resource handles under the lock +
     * mark slots free, then release them outside the lock. Calling
     * hailo_*_free under spin_lock_irqsave is unsafe — the platform
     * dma_free may acquire another lock, trigger an IPI, or walk a
     * refcount that takes the PMM lock. Holding a kernel-wide IRQ-
     * disabled critical section across unbounded allocator work is
     * what to avoid; the out-of-lock path keeps the same ordering
     * guarantees (in_use=false is visible to other CPUs via the
     * unlock's release semantics before we free). */
    struct hailo_model_slot stash[HAILO_MAX_MODELS];
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memcpy(stash, slots, sizeof(stash));
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);

    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (stash[i].in_use && stash[i].cs_loaded) {
            context_switch_unwind(&stash[i]);
        }
    }
}

static int hailo_backend_load_model(struct inference_device *dev,
                                    const void *model, size_t size,
                                    inference_model_handle_t *out)
{
    (void)dev;

    if (!model || size == 0 || !out) return INF_ERR_INVAL;
    *out = INF_INVALID_HANDLE;

    hailo_trace_set_phase(HAILO_TRACE_PHASE_MODEL_LOAD);

    /* Firmware must be booted before we can feed any model. */
    if (hailo_get_state() != HAILO_STATE_RUNNING) return INF_ERR_NODEV;

    /* 1. Outer header — validates magic + version + proto bounds. */
    struct hef_outer_header outer;
    int rc = hef_parse_outer_header(model, size, &outer);
    if (rc != HEF_OK) {
        uart_printf("[hailo] load_model: outer parse failed rc=%d\r\n", rc);
        return INF_ERR_BAD_MODEL;
    }
    uart_printf("[hailo] load_model: outer v=%u proto=%u@%u ccws=%lu@%lu (blob=%lu)\r\n",
                (unsigned)outer.version,
                (unsigned)outer.proto_size,
                (unsigned)outer.proto_offset,
                (unsigned long)outer.ccws_size,
                (unsigned long)outer.ccws_offset,
                (unsigned long)size);

    /* 2. Proto body — extract pad shapes for the first network group.
     * struct hef_info is ~1.2 KB; the 16 KB kernel stack has room. */
    struct hef_info info;
    const uint8_t *proto = (const uint8_t *)model + outer.proto_offset;
    rc = hef_parse_body(proto, outer.proto_size, &info);
    if (rc != HEF_OK) {
        uart_printf("[hailo] load_model: body parse failed rc=%d\r\n", rc);
        return INF_ERR_BAD_MODEL;
    }

    uart_printf("[hailo] load_model: parsed ngs=%u ops=%u pads=%u ccws=%u\r\n",
                (unsigned)info.network_group_count,
                (unsigned)info.op_count,
                (unsigned)info.pad_count,
                (unsigned)info.ccw_action_count);

    /* The parser silently truncates at HEF_PARSER_MAX_CCW_ACTIONS (256)
     * and sets info.ccw_actions_truncated; a truncated load would stall
     * at inference with missing microcode and no direct symptom pointing
     * at the cause. Refuse the load instead so the error is obvious. */
    if (info.ccw_actions_truncated) {
        WARN("hailo backend: HEF has more CCW actions than HEF_PARSER_"
             "MAX_CCW_ACTIONS; load aborted (count=%u)",
             (unsigned)info.ccw_action_count);
        return INF_ERR_BAD_MODEL;
    }

    /* 3. Pick the largest pads in each direction. See pick_largest_pads
     * for the multi-head rationale.
     *
     * Audit F-05 (2026-04-24): the previous synthetic-pad fallback
     * (1000-byte ImageNet shape with invented sys_index) is removed.
     * If pick_largest_pads fails on a real HEF, the right fix is at
     * the parser layer, not a runtime invention — synthetic transport
     * geometry can produce OPEN_BOUNDARY_OUTPUT context bytes whose
     * sys_index doesn't match anything firmware can route, masking
     * the parser bug as a "no output ever" runtime symptom. */
    const struct hef_pad_info *in_pad, *out_pad;
    uint32_t input_bytes, output_bytes;
    if (pick_largest_pads(&info, &in_pad, &out_pad, &input_bytes, &output_bytes) != 0) {
        uart_printf("[hailo] load_model: pick_largest_pads failed "
                    "(pad_count=%u). HEF parser must surface both an "
                    "input pad and an output pad; refusing load.\r\n",
                    (unsigned)info.pad_count);
        for (uint32_t i = 0; i < info.pad_count; i++) {
            const struct hef_pad_info *p = &info.pads[i];
            uart_printf("  pad[%u] dir=%s sys=%u bytes=%u shape=%ux%ux%u\r\n",
                        (unsigned)i,
                        p->is_input ? "in" : "out",
                        (unsigned)p->sys_index,
                        (unsigned)pad_bytes(p),
                        (unsigned)p->height, (unsigned)p->width,
                        (unsigned)p->features);
        }
        return INF_ERR_BAD_MODEL;
    }
    uart_printf("[hailo] load_model: pads in=%u bytes out=%u bytes\r\n",
                (unsigned)input_bytes, (unsigned)output_bytes);

    /* Stream parameters — prefer HEF-derived values (Phase 6.2b),
     * fall back to placeholders that work under the mock but time
     * out on real hardware. Channel indices are host-chosen; we use
     * 0 for input and 1 for output (standard convention).
     *
     * data_id mirrors the HEF's sys_index — firmware uses it to
     * route DMA to the correct on-chip buffer. Without the HEF
     * value the kernel sends 0, which the NPU rejects with a
     * stream-not-configured status.
     *
     * page_size is core_bytes_per_buffer for the input and the
     * periph side for the output; without HEF values we fall back
     * to 512 B which only happens to work for models whose real
     * buffer size is a multiple of it.
     *
     * Computed before the slot is claimed so the lock-protected
     * region below stays short. */
    uint8_t  in_data_id   = in_pad->has_stream_info  ? (uint8_t) in_pad->sys_index
                                                     : 0;
    uint8_t  out_data_id  = out_pad->has_stream_info ? (uint8_t)out_pad->sys_index
                                                     : 0;
    uint16_t in_page_size = in_pad->has_stream_info && in_pad->core_bytes_per_buffer
                              ? (uint16_t)in_pad->core_bytes_per_buffer : 512;
    uint16_t out_page_size = out_pad->has_stream_info && out_pad->core_bytes_per_buffer
                               ? (uint16_t)out_pad->core_bytes_per_buffer : 512;

    /* Pre-compute the input/output shape values from in_pad/out_pad
     * before taking slots_lock. pad_dim is `max(padded, raw)` today,
     * but hoisting these reads keeps the lock-protected critical
     * region as straight-line stores even if pad_dim ever grows
     * non-trivial logic. */
    uint16_t in_shape0  = (uint16_t)pad_dim(in_pad->padded_height,   in_pad->height);
    uint16_t in_shape1  = (uint16_t)pad_dim(in_pad->padded_width,    in_pad->width);
    uint16_t in_shape2  = (uint16_t)pad_dim(in_pad->padded_features, in_pad->features);
    uint16_t out_shape0 = (uint16_t)pad_dim(out_pad->padded_height,   out_pad->height);
    uint16_t out_shape1 = (uint16_t)pad_dim(out_pad->padded_width,    out_pad->width);
    uint16_t out_shape2 = (uint16_t)pad_dim(out_pad->padded_features, out_pad->features);

    /* 4. Claim a slot AND populate cfg/shape atomically.
     *
     * Previously the populate happened outside the lock with only the
     * in_use=true flip protected. That left a window where
     * hailo_backend_in_use_slots() / hailo_backend_model_sizes() (which
     * also iterate slots under slots_lock) could observe in_use=true
     * with cfg.input_bytes / output_bytes still zero from the prior
     * occupant. context_switch_load remains outside the lock — its
     * descriptor allocation and HEF translation are too long to hold
     * a spinlock across. */
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    int idx = -1;
    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (!slots[i].in_use) {
            slots[i].in_use = true;
            slots[i].cfg = (struct hailo_infer_config){
                .input_bytes      = input_bytes,
                .output_bytes     = output_bytes,
                /* Boundary channels MUST match the packed_vdma the CS
                 * translator emits in OPEN_BOUNDARY_{INPUT,OUTPUT}_CHANNEL
                 * — see hailo_cs_translator.h. Hardcoded 0/1 here pre-#682
                 * armed different host VDMA registers than the channels
                 * the fw was told to wait on. */
                .input_channel    = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL +
                                              HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET),
                .output_channel   = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL +
                                              HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET),
                .input_data_id    = in_data_id,
                .output_data_id   = out_data_id,
                .input_page_size  = in_page_size,
                .output_page_size = out_page_size,
                /* Deliberately tight: scheduler-policy path (ai_hailo)
                 * invokes run() from contexts that may have IRQs
                 * disabled. A 500 ms poll would stall the CPU and drop
                 * timer ticks. A real Hailo-8 MLP inference completes
                 * in microseconds; 10 ms was the original 500x safety
                 * margin but bumping to 500 ms for real-HEF bring-up —
                 * first inference may include one-time CCW upload
                 * latency and we don't yet know the real variance. */
                .timeout_us       = 500000,     /* 500 ms */
            };
            slots[i].input_shape[0]  = in_shape0;
            slots[i].input_shape[1]  = in_shape1;
            slots[i].input_shape[2]  = in_shape2;
            slots[i].output_shape[0] = out_shape0;
            slots[i].output_shape[1] = out_shape1;
            slots[i].output_shape[2] = out_shape2;
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&slots_lock, flags);
    if (idx < 0) return INF_ERR_FULL;

    /* 5. Context-switch load: replaces the prior best-effort
     * WRITE_MEMORY + CONFIG_STREAM path. Allocates VDMA desc lists
     * for CCW + boundary I/O, translates HEF → context-switch wire
     * bytes, and ships the 4-context sequence firmware needs. On
     * failure the slot is released so the caller can retry. */
    int csrc = context_switch_load(&slots[idx], &info, model, size, &outer,
                                   in_pad, out_pad);
    if (csrc != HAILO_OK) {
        irq_flags_t f = spin_lock_irqsave(&slots_lock);
        memset(&slots[idx], 0, sizeof(slots[idx]));
        spin_unlock_irqrestore(&slots_lock, f);
        return hailo_err_to_inf(csrc);
    }

    /* Handle numbering: 1..HAILO_MAX_MODELS. INF_BUILTIN_HANDLE (=0)
     * stays reserved for backends with compiled-in weights. */
    *out = (inference_model_handle_t)(idx + 1);
    return INF_OK;
}

static int hailo_backend_run(struct inference_device *dev,
                             inference_model_handle_t h,
                             const inference_tensor_t *in,
                             inference_tensor_t *out)
{
    (void)dev;

    if (!in || !out || !in->data || !out->data) return INF_ERR_INVAL;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return INF_ERR_INVAL;

    hailo_trace_set_phase(HAILO_TRACE_PHASE_INFERENCE);

    struct hailo_model_slot *slot = &slots[h - 1];
    /* Audit F-07: take a slot reference under the lock. From here
     * until inflight_runs is decremented at the bottom, free_model
     * will refuse and return BUSY rather than tearing down the DMA
     * buffers, descriptor lists, and parsed HEF state we're about
     * to touch. */
    irq_flags_t run_flags = spin_lock_irqsave(&slots_lock);
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, run_flags);
        return INF_ERR_INVAL;
    }
    slot->inflight_runs++;
    spin_unlock_irqrestore(&slots_lock, run_flags);

#ifdef HAILO_WIRE_DEBUG
    /* #682 checkpoint 4: IN ch=2 base register at runmodel entry,
     * before any program_buffer or submit work. Pinpoints whether
     * avail=2 already at function entry (load left it) vs gets
     * introduced by code between here and submit_and_wait. */
    hailo_vdma_dump_channel_regs(2, "[682-cp4] runmodel entry");
#endif

    int run_rc;
    #define HAILO_RUN_RETURN(rc_) do { run_rc = (rc_); goto run_release; } while (0)

    /* Hailo operates on INT8 tensors after DFC quantization. FP32
     * callers need a pre/post quantization layer — that's the
     * ai_policy_hailo wrapper's job, not this backend's. */
    if (in->dtype != INF_DTYPE_INT8 || out->dtype != INF_DTYPE_INT8) {
        HAILO_RUN_RETURN(INF_ERR_BAD_TENSOR);
    }
    if (in->n_elems != slot->cfg.input_bytes
     || out->n_elems != slot->cfg.output_bytes) {
        HAILO_RUN_RETURN(INF_ERR_BAD_TENSOR);
    }

    /* #338: submit via the slot's load-bound boundary tensors +
     * descriptor lists rather than allocating fresh ones. Firmware
     * was told about these specific IOVAs in ACTIVATION's
     * OpenBoundaryInput/Output actions; using different buffers at
     * submit time would either be rejected by firmware or cause the
     * NN engine to DMA from the zero-initialized load-time buffers.
     *
     * If context_switch_load didn't allocate boundary resources
     * (synthetic HEF without has_stream_info pads), fall back to
     * the legacy hailo_infer_run path — it allocates its own
     * buffers and runs self-contained. That path is used by
     * `hailo infer <size>` shell diagnostics that don't load a HEF. */
    if (!slot->cs_loaded || slot->boundary_in_tensor.cpu_addr == NULL
                         || slot->boundary_out_tensor.cpu_addr == NULL) {
        int rc = hailo_infer_run(&slot->cfg, in->data, out->data, NULL);
        HAILO_RUN_RETURN(hailo_err_to_inf(rc));
    }

    /* Bounds already checked against slot->cfg.input_bytes/output_bytes
     * above; the boundary tensors were sized from the same pad values
     * (core_bytes_per_buffer) at load time so the memcpy target has
     * matching capacity. Defensive re-check since cfg and tensor
     * sizing come from different sources and mismatches would be
     * memory-safety-critical, not just functional bugs. */
    if (in->n_elems  > slot->boundary_in_tensor.tensor_bytes
     || out->n_elems > slot->boundary_out_tensor.tensor_bytes) {
        WARN("hailo backend: tensor size > boundary buffer (in=%u/%u, out=%u/%u)",
             in->n_elems,  slot->boundary_in_tensor.tensor_bytes,
             out->n_elems, slot->boundary_out_tensor.tensor_bytes);
        HAILO_RUN_RETURN(INF_ERR_BAD_TENSOR);
    }

    /* The VDMA channel indices that receive these submits must be
     * the ones ACTIVATION opened. translate_activation packed them
     * as config_vdma + BOUNDARY_INPUT_CHANNEL_OFFSET (1) and
     * BOUNDARY_OUTPUT_CHANNEL_OFFSET (2). The low nibble of
     * packed_vdma_channel_id is the channel number — same value
     * both sides need. */
    uint8_t in_channel  = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                                  + HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET);
    uint8_t out_channel = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                                  + HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET);

    /* #253 CORE-CPU liveness probe: issue an empty-body CORE_IDENTIFY
     * right before the submit. If this returns rc=0 quickly, fw's
     * CORE-CPU RPC thread is alive and the submit blocker is inside
     * the inference-task / burst-credits state machine. If it times
     * out, the whole CORE CPU is wedged (e.g. inference task crashed
     * and starved the RPC thread). Strip once the blocker lifts. */
    {
        uint64_t t_probe_start = timer_get_count();
        uint32_t resp_len = 0;
        int probe_rc = hailo_control_core_identify(&resp_len);
        uint64_t t_probe_end = timer_get_count();
        uint64_t probe_us = (t_probe_end - t_probe_start) * 1000000ULL
                             / timer_get_frequency();
        uart_printf("[hailo] run: pre-submit CORE_IDENTIFY rc=%d "
                    "resp_len=%u latency=%lu us\r\n",
                    probe_rc, (unsigned)resp_len,
                    (unsigned long)probe_us);
    }

    /* #682 (2026-05-11): the unconditional pre-submit drain that
     * used to live here was perturbing fw's channel state machine
     * on every submit. The drain wrote zero to BAR4+0x0c80 (the
     * in_use flag) regardless of whether fw had actually posted a
     * notification — Linux hailo_pci's IRQ-driven path never
     * touches the notification buffer unless FW_NOTIFICATION_IRQ
     * fires. With FW_NOTIFICATION_IRQ now handled in
     * control_msi_handler and the polling-fallback paths
     * (wait_for_response, hailo_control_drain_pending_irqs), the
     * buffer is read+ACK'd at delivery time, which is the same
     * model Linux uses. Stale boot-time ECC notifications are
     * caught by the 500 ms post-BOOT_IRQ settle (see memory
     * hailo_post_bootirq_settle_fixes_ecc.md) and the FW_NOTIFICATION
     * IRQ path, not by submit-boundary draining. */

    /* Copy caller's input into the pre-allocated DMA buffer +
     * cache-clean so the device picks up the fresh bytes.
     * Corresponding invalidate+copy for output happens after submit. */
    memcpy(slot->boundary_in_tensor.cpu_addr, in->data, in->n_elems);
    hailo_tensor_prepare_for_device(&slot->boundary_in_tensor);

    /* Re-program the input/output descriptor lists against the fresh
     * tensor bytes for this inference. The desc list structure is
     * shared with firmware (bound via OpenBoundary at load time) but
     * the per-submit num_avail signal tells firmware how many pages
     * to consume from the current buffer contents.
     *
     * Do NOT call channel_start here — firmware owns the boundary
     * channels after context-switch ACTIVATION+ENABLED. An early
     * experiment re-issued channel_start each inference which wrote
     * ABORT_PAUSE (then START) into the CONTROL byte; this clobbered
     * firmware's channel state and num_proc never advanced on IN.
     * Reference behaviour (hailo-vdma-common.c:hailo_vdma_start_channel)
     * is a one-time setup during resource manager activation, not
     * per-transfer. */
    /* Program the FULL boundary-tensor size, not in->n_elems. Firmware
     * was told bytes_in_pattern = bpb*bpf (the HEF's periph-aligned
     * frame size) in OpenBoundary at load time. A partial program at
     * run time would describe a shorter frame than the one firmware
     * expects, and firmware silently refuses to advance num_proc on
     * the boundary channel. The caller's payload (in->n_elems) was
     * memcpy'd into the tensor above; positions beyond it stay at the
     * zero-fill from load_model, giving firmware a periph-padded frame
     * matching OpenBoundary's declaration. */
    int programmed = hailo_vdma_program_buffer(
        &slot->boundary_in_list, 0,
        slot->boundary_in_tensor.iova,
        slot->boundary_in_tensor.tensor_bytes,
        HAILO_VDMA_HOST_DMA_DATA_ID);
    if (programmed < 0) {
        HAILO_RUN_RETURN(INF_ERR_NOSUPPORT);
    }
    uint16_t in_num_avail = (uint16_t)programmed;

    programmed = hailo_vdma_program_buffer(
        &slot->boundary_out_list, 0,
        slot->boundary_out_tensor.iova,
        slot->boundary_out_tensor.tensor_bytes,
        HAILO_VDMA_HOST_DMA_DATA_ID);
    if (programmed < 0) {
        HAILO_RUN_RETURN(INF_ERR_NOSUPPORT);
    }
    uint16_t out_num_avail = (uint16_t)programmed;

#ifdef HAILO_WIRE_DEBUG
    /* Phase 8 #253: dump programmed descriptors + channel regs so we
     * can compare byte-for-byte against HailoRT's reference output.
     * Runs once per hailo_backend_run call; `hailo runmodel <h> 1`
     * gives exactly one dump per inference. Gated behind
     * HAILO_WIRE_DEBUG; OFF builds skip.
     *
     * Also dumps the CONFIG channel (ch 1) so we can see whether the
     * CCW upload actually advanced num_proc to total_desc_count — if
     * it's still at 0, PRELIMINARY's FETCH_CFG_CHANNEL_DESCRIPTORS
     * never completed, which would stall the whole inference
     * pipeline even though SET_CONTEXT_INFO(PRELIMINARY) returned
     * rc=0 (the RPC acknowledges "got it", not "done"). */
    hailo_vdma_dump_channel_regs(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL,
                                 "CFG pre-submit");
    hailo_vdma_dump_desc_list(&slot->boundary_in_list,  "IN",  4);
    hailo_vdma_dump_channel_regs(in_channel,  "IN pre-submit");
    hailo_vdma_dump_desc_list(&slot->boundary_out_list, "OUT", 4);
    hailo_vdma_dump_channel_regs(out_channel, "OUT pre-submit");
#endif /* HAILO_WIRE_DEBUG */

    /* PHASE 8 KEY FINDING #2 (2026-04-22, instrumented hailo_pci on
     * Pi OS yolov6n — see ~/slmos-ref/derivatives/notes/hailort-trace-findings-2026
     * -04-22.md): HailoRT pre-arms each output channel with N
     * SEPARATE launch_transfer calls, each one programming desc[i]
     * and bumping num_avail from i to i+1. Across 8 calls fw's
     * channel-side num_avail register sees 1, 2, 3, 4, 5, 6, 7, 8
     * as eight distinct MMIO writes — not a single 0→8 jump.
     *
     * Hypothesis: firmware's boundary-credit state machine processes
     * num_avail bumps incrementally and won't recognize a multi-
     * credit jump in one MMIO write. PR #350's "program desc[1..7]
     * + write num_avail=1 once" placed valid descriptors but fw
     * never saw the credits arrive in the way it expects. This loop
     * mirrors the HailoRT pattern exactly: one program_buffer +
     * one num_avail bump per pre-armed credit, OUT first, IN last.
     *
     * out_num_avail from program_buffer above is the count for ONE
     * descriptor; we ignore it here and pre-arm
     * HAILO_BOUNDARY_OUT_PREFETCH_DEPTH credits sequentially. */
#ifdef HAILO_WIRE_DEBUG
    hailo_vdma_snap_channels("pre-OUT-prefetch");
#endif
    for (uint16_t i = 0; i < HAILO_BOUNDARY_OUT_PREFETCH_DEPTH; i++) {
        if (i > 0) {
            (void)hailo_vdma_program_buffer(
                &slot->boundary_out_list, i,
                slot->boundary_out_tensor.iova,
                slot->boundary_out_tensor.tensor_bytes,
                HAILO_VDMA_HOST_DMA_DATA_ID);
        }
        (void)hailo_vdma_write_num_avail(out_channel, (uint16_t)(i + 1));
    }
#ifdef HAILO_WIRE_DEBUG
    hailo_vdma_snap_channels("post-OUT-prefetch");
#endif
    /* `out_num_avail` from program_buffer above is the desc count fw
     * is expected to *consume* for one inference (== 1 for our single-
     * desc OUT). Keep that as the wait target — even though we
     * primed 8 credits via the loop, only ONE descriptor's worth of
     * data lands per input we submit. wait_proc(target=1) is the
     * right downstream poll. */

#ifdef HAILO_WIRE_DEBUG
    /* #682 hypothesis-1 (disconfirmed 2026-05-07): readback of
     * PER_SRC/PER_DST/ISTATUS at boot-arm, post-load, pre-IN-submit
     * showed fw sets PER_SRC bits 0/1 for CFG channels but never
     * sets bit 2 for IN ch=2 or PER_DST bit 16 for OUT ch=16.
     * A fix-attempt that wrote (1<<in_channel) into PER_SRC via RMW
     * found the registers behave as write-1-to-clear interrupt status:
     * the write returned PER_SRC=0 (clobbered fw's bits 0/1) and also
     * cleared ISTATUS bit 0. So PER_SRC/PER_DST are "channels with an
     * IRQ pending" status, not host-settable enables — boundary IN
     * ch=2 not appearing means fw never raised an IRQ for it, which
     * means fw never tried to fetch; root cause is upstream of the
     * channel-arming/IRQ layer. */
    hailo_control_dump_irq_state("pre-IN-submit");
    /* #682 hypothesis-6: link-state immediately before submit. If
     * the trained speed/width is anything other than what was
     * captured at post-boot-arm, the link re-trained between RPCs. */
    hailo_platform_log_link_state("pre-IN-submit");
    /* #682 hyp-W: dump (read-only) then W1C-clear (real config-space
     * write — mutates EP state) the bridge error bits so the
     * post-timeout dump shows only errors that fired *during* the
     * boundary submit window. Disambiguates stale boot-time UR/MA
     * from runtime UR/MA. The clear is the only `HAILO_WIRE_DEBUG`
     * site that mutates device state; if you're debugging an
     * unexpected post-submit DEVSTA value with WIRE_DEBUG enabled,
     * remember this clear ran first. */
    hailo_platform_dump_bridge_errors("pre-IN-submit");
    hailo_platform_clear_bridge_errors("pre-IN-submit");
    /* #682 hyp-X-1 (DISCONFIRMED 2026-05-09): mirror Linux's
     * hailo_pcie_read_interrupt drain at pre-IN-submit. Linux MNIST
     * trace (~/slmos-ref/derivatives/hailort-traces/
     *  hailort-v4.23.0-mnist-inference-pios-pi5.txt:3181-3186) shows
     * the host issues a read+W1C of BCS_ISTATUS_HOST + per-channel
     * SRC/DST registers immediately before the boundary IN avail
     * bump. SLM-OS arrives here with stale PER_SRC bits for ch=0+1
     * (CFG channels from load) + ISTATUS.BOOT_IRQ still latched.
     * The drain clears them cleanly (0x02800001→0x00000000) but the
     * ch=2 wedge is unchanged: dev_proc stays 0, dev_base[31:16]
     * never advances to 0x001f. Pending host-side IRQ acks are not
     * the gating factor for fw's ch=2 prep. Helper kept wired here
     * for cleaner post-timeout state correlation; cheap (4-6 MMIO
     * ops) and does no harm. */
    hailo_control_drain_pending_irqs("pre-IN-submit");
    /* #682 hypothesis-7: BAR4 SRAM baseline before submit. Diff'd
     * against post-timeout snapshot to detect fw activity hiding
     * outside our normal trace points. */
    hailo_bar4_snap(hailo_bar4_snap_pre);
    /* #682 hypothesis-A (2026-05-08): RPC was rejected by fw with
     * status 0x400300ca/0x40000001 (CORE-CPU). HW-only benchmark mode
     * requires special HEF state; not applicable to streaming inference.
     * Disabled — kept here for the bisect record. */

    /* #682 hypothesis-E (2026-05-08): host-side AP→SR ctrl cycle on
     * ch=2 just before the avail bump. Disconfirmed — ctrl writes
     * land cleanly (read-back showed 0x01→0x02→0x01) but the avail
     * bump that followed still failed to advance proc. Channel state
     * is healthy from host's view; the wedge is upstream of the host
     * channel registers, in fw or device internals. */
#endif

    /* #682 hyp-H2 (2026-05-08, DISCONFIRMED): mirrored HailoRT v4.23's
     * 4-RPC settle ping set (GDI, IDENTIFY × 2) between OUT prefetch
     * and IN submit, per Pi OS kprobe trace at
     * ~/slmos-ref/derivatives/hailort-traces/
     * hailort-v4.23.0-irq-rpc-resnet50-pi5.txt. All 4 pings returned
     * rc=0 but ch=2 still wedged at proc=0, dev_base=0x00022801. The
     * settle pings are not the gating factor for fw boundary
     * processing. */

#ifdef HAILO_WIRE_DEBUG
    hailo_vdma_snap_channels("pre-IN-submit");
#endif

    int rc;
    uint64_t t_in_submit  = timer_get_count();
    rc = hailo_vdma_submit_and_wait(in_channel, in_num_avail,
                                    slot->cfg.timeout_us);
    uint64_t t_in_done = timer_get_count();
#ifdef HAILO_WIRE_DEBUG
    hailo_vdma_snap_channels("post-IN-submit");
#endif
    if (rc != HAILO_OK) {
        uart_printf("[hailo] run: IN submit_and_wait rc=%d (avail=%u)\r\n",
                    rc, (unsigned)in_num_avail);
        /* #253 (2026-04-23): read back desc[0..7] status fields from
         * DRAM. fw writes DESC_DONE / DESC_ERROR into the low byte
         * when it processes a descriptor; the value tells us whether
         * fw ever even tried to fetch our descriptors:
         *   status==0 → fw never touched it (upstream problem:
         *               channel arming, num_avail latch, scheduler
         *               not assigning credits)
         *   DONE      → fw fetched + processed (problem is downstream)
         *   ERROR     → fw fetched but DMA-faulted (IOVA / inbound)
         * This splits "fw never tried" from "fw tried and failed",
         * which num_proc==0 alone can't distinguish. */
        hailo_vdma_dump_desc_status(&slot->boundary_in_list, "IN", 8);
        hailo_vdma_dump_desc_status(&slot->boundary_out_list, "OUT", 8);
#ifdef HAILO_WIRE_DEBUG
        /* #682 hypothesis-1: post-timeout register read-back. If bits
         * cleared during the 500 ms poll window, fw is dynamically
         * disabling them — completes the four-point trail. */
        hailo_control_dump_irq_state("post-timeout");
        /* #682 hypothesis-6: link-state after the 500 ms timeout. A
         * step-down here would mean the link was dropping during the
         * window we expected fw to fetch descriptors. */
        hailo_platform_log_link_state("post-timeout");
        /* #682 hyp-U: dump RC bridge + endpoint error registers. If
         * fw issued a transaction the BCM2712 RC dropped, the EP's
         * Device Status will have UR/Non-Fatal/Fatal set; if the
         * RC's AXI bus saw a read error, AXI_READ_ERROR_DATA will
         * differ from the 0xFFFFFFFF seed. */
        hailo_platform_dump_bridge_errors("post-timeout");
        /* #682 hypothesis-7: BAR4 SRAM snapshot after the 500 ms wait.
         * Diff against pre-submit baseline. Any changed dword tells
         * us where fw is active during a stall that proc=0 alone
         * makes look idle. */
        hailo_bar4_snap(hailo_bar4_snap_post);
        hailo_bar4_diff_print("post-timeout");
#endif
        /* #253: dump fw debug log + D2H notification buffer on submit
         * failure. The D2H notification contains CONTEXT_SWITCH_RUN_TIME_ERROR
         * events that carry {exit_status, context_idx, action_idx} — exactly
         * what we want to know when fw's inference pipeline stalled. The
         * debug log is compact binary so less immediately useful, kept
         * for offset-advancement signals (chip_offset > last seen == fw
         * still writing). */
        hailo_fw_drain_d2h_notifications(HAILO_D2H_POST_FAIL_DRAIN_CAP);
        hailo_fw_dump_log(HAILO_FW_LOG_CORE_CPU_OFFSET, "CORE");
        hailo_fw_dump_log(HAILO_FW_LOG_APP_CPU_OFFSET,  "APP");
        goto run_out;
    }
    uart_printf("[hailo] run: IN ok in %lu us\r\n",
                (unsigned long)((t_in_done - t_in_submit) * 1000000ULL
                                / timer_get_frequency()));

    /* OUTPUT num_avail was written pre-INPUT (see earlier comment on
     * HailoRT's host-side submit ordering). At this point fw should
     * have produced the output data and the channel's num_proc
     * should equal num_avail. Poll for that without re-writing
     * num_avail. */
    uint64_t t_out_submit = timer_get_count();
    rc = hailo_vdma_channel_wait_proc(out_channel, out_num_avail,
                                      slot->cfg.timeout_us);
    uint64_t t_out_done = timer_get_count();
    if (rc != HAILO_OK) {
        uart_printf("[hailo] run: OUT wait_proc rc=%d (target=%u)\r\n",
                    rc, (unsigned)out_num_avail);
        goto run_out;
    }
    uart_printf("[hailo] run: OUT ok in %lu us\r\n",
                (unsigned long)((t_out_done - t_out_submit) * 1000000ULL
                                / timer_get_frequency()));

    hailo_tensor_prepare_for_host(&slot->boundary_out_tensor);
    memcpy(out->data, slot->boundary_out_tensor.cpu_addr, out->n_elems);
    rc = HAILO_OK;

run_out:
    /* Do NOT stop the boundary channels on success — firmware expects
     * them live for subsequent inferences. On error, leaving them
     * running is also safe: the next submit will fail the same way
     * until the model is freed (which tears the channels down via
     * context_switch_unwind → hailo_backend_free_model). */
    run_rc = hailo_err_to_inf(rc);

run_release:
    /* Audit F-07: drop the slot reference taken at function entry.
     * Every code path between `inflight_runs++` and this label takes
     * the matching decrement once — invariant `inflight_runs > 0` on
     * entry to this block. WARN-and-skip on underflow rather than
     * silently absorbing it: a hit here means a missing inc/dec pair
     * elsewhere and the right response is a visible log, not a
     * paper-over. */
    {
        irq_flags_t rel_flags = spin_lock_irqsave(&slots_lock);
        if (slot->inflight_runs == 0) {
            spin_unlock_irqrestore(&slots_lock, rel_flags);
            WARN("hailo backend: run_release with inflight_runs==0 "
                 "(inc/dec pairing bug; slot=%u)",
                 (unsigned)(h - 1));
        } else {
            slot->inflight_runs--;
            spin_unlock_irqrestore(&slots_lock, rel_flags);
        }
    }
    #undef HAILO_RUN_RETURN
    return run_rc;
}

static int hailo_backend_free_model(struct inference_device *dev,
                                    inference_model_handle_t h)
{
    (void)dev;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return INF_ERR_INVAL;
    struct hailo_model_slot *slot = &slots[h - 1];
    /* Two-phase: under the lock, copy out the DMA handles + clear
     * the slot; outside the lock, free the DMA resources. Holding
     * spin_lock_irqsave across dma_free would block other CPUs on
     * any allocator-internal contention and — depending on the
     * platform's free path — risk nesting locks held at interrupt
     * priority. The lock ordering is preserved: any concurrent
     * load_model scanning for a free slot sees in_use=false only
     * after the slot is fully zeroed. */
    struct hailo_model_slot stash;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return INF_ERR_INVAL;
    }
    /* Audit F-07: refuse the free if a run() is in flight on this
     * slot. Returning EBUSY (mapped to INF_ERR_BUSY) is correct
     * behavior — the caller can retry after the inference completes,
     * or the slot can stay loaded until the run releases its
     * reference. We do NOT spin-wait under the IRQ-disabling lock. */
    if (slot->inflight_runs > 0) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return INF_ERR_BUSY;
    }
    stash = *slot;
    memset(slot, 0, sizeof(*slot));
    spin_unlock_irqrestore(&slots_lock, flags);

    if (stash.cs_loaded) {
        context_switch_unwind(&stash);
    }
    return INF_OK;
}

/* -------------------------------------------------------------------------- */
/* Test introspection + test fixture helper                                    */
/*                                                                              */
/* Always present (not ENABLE_BOOT_TESTS-gated) so test_hailo.c — which is     */
/* linked into both test and non-test kernel builds on non-x86 — always       */
/* resolves. The cost is 16 bytes of code in production kernels and a         */
/* byte-level dispatcher that no non-test caller ever hits.                    */
/* -------------------------------------------------------------------------- */

uint32_t hailo_backend_in_use_slots(void)
{
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    uint32_t n = 0;
    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (slots[i].in_use)
            n++;
    }
    spin_unlock_irqrestore(&slots_lock, flags);
    return n;
}

/* Audit F-07: test-only inflight_runs poke. Lets a unit test simulate
 * "run() is in flight" without actually firing the full inference
 * path, so the free_model BUSY contract can be exercised in QEMU.
 * Returns the previous count, or UINT32_MAX for an out-of-range or
 * unloaded handle. */
uint32_t hailo_backend_test_set_inflight(
    inference_model_handle_t h, uint32_t count)
{
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return UINT32_MAX;
    struct hailo_model_slot *slot = &slots[h - 1];
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return UINT32_MAX;
    }
    uint32_t prev = slot->inflight_runs;
    slot->inflight_runs = count;
    spin_unlock_irqrestore(&slots_lock, flags);
    return prev;
}

/* Test-only: expose load-time boundary IOVAs so run-path tests can
 * assert that submit happens via the same IOVAs firmware was told
 * about in ACTIVATION. Out-of-range / unloaded handle returns 0 for
 * both. */
void hailo_backend_get_boundary_iovas_for_tests(
    inference_model_handle_t h, uint64_t *in_iova, uint64_t *out_iova)
{
    if (in_iova)  *in_iova  = 0;
    if (out_iova) *out_iova = 0;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return;
    struct hailo_model_slot *slot = &slots[h - 1];
    if (!slot->in_use || !slot->cs_loaded) return;
    if (in_iova)  *in_iova  = slot->boundary_in_list.iova;
    if (out_iova) *out_iova = slot->boundary_out_list.iova;
}

/* Public helper for callers (e.g. the Lua shell binding) that need
 * to size a tensor buffer against a loaded model without going
 * through inference_run. Returns HAILO_OK and fills both out-params
 * on success, HAILO_ERR_INVAL for an out-of-range / unloaded
 * handle. Safe to call with either pointer NULL (skips that output).
 *
 * Takes slots_lock so a concurrent hailo_backend_free_model can't
 * zero cfg.input_bytes / cfg.output_bytes between our in_use check
 * and the reads. Matches hailo_backend_in_use_slots' locking model
 * (writers mutate slot state under slots_lock, readers that care
 * about consistency must take it too). */
int hailo_backend_model_sizes(inference_model_handle_t h,
                              uint32_t *in_bytes,
                              uint32_t *out_bytes)
{
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return HAILO_ERR_INVAL;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    struct hailo_model_slot *slot = &slots[h - 1];
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return HAILO_ERR_INVAL;
    }
    uint32_t in  = slot->cfg.input_bytes;
    uint32_t out = slot->cfg.output_bytes;
    spin_unlock_irqrestore(&slots_lock, flags);
    if (in_bytes)  *in_bytes  = in;
    if (out_bytes) *out_bytes = out;
    return HAILO_OK;
}

/* Public helper: report the maximum number of concurrent models the
 * backend can hold. Static at compile time today (HAILO_MAX_MODELS),
 * but exposed as a function so callers (slm.hailo.status) don't need
 * to duplicate the constant. */
uint32_t hailo_backend_slots_max(void)
{
    return (uint32_t)HAILO_MAX_MODELS;
}

void hailo_backend_reset_slots_for_tests(void)
{
    /* Same two-phase dance as hailo_backend_shutdown — see that
     * function for the rationale (dma_free outside spin_lock). */
    struct hailo_model_slot stash[HAILO_MAX_MODELS];
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memcpy(stash, slots, sizeof(stash));
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);

    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (stash[i].in_use && stash[i].cs_loaded) {
            context_switch_unwind(&stash[i]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Registration                                                                */
/* -------------------------------------------------------------------------- */

static const struct inference_device_ops hailo_ops = {
    .name       = "hailo-8",
    .caps       = INF_CAP_INT8 | INF_CAP_LOAD_MODEL | INF_CAP_DMA,
    .init       = hailo_backend_init,
    .shutdown   = hailo_backend_shutdown,
    .load_model = hailo_backend_load_model,
    .run        = hailo_backend_run,
    .free_model = hailo_backend_free_model,
};

static struct inference_device hailo_device = {
    .ops = &hailo_ops,
};

/*
 * Called once from platform init, AFTER the Hailo platform shim has
 * installed ops (hailo_init succeeded). Registering before that leaves
 * the backend present but load_model returns INF_ERR_NODEV until the
 * firmware is booted — which is the intended behavior (scheduler can
 * look up the device by name at any time, but actually using it
 * requires the hardware to be live).
 */
int inference_device_hailo_register(void)
{
    return inference_device_register(&hailo_device);
}
