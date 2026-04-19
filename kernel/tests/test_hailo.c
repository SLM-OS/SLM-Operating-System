/*
 * test_hailo.c — Offline unit tests for the Hailo driver core.
 *
 * Exercises hailo_core.c against a fake `hailo_platform_ops` whose
 * read32/write32 operate on an in-memory register array — no
 * PCIe, no hardware, runs on QEMU. Covers:
 *   - hailo_init rejects NULL / incomplete ops
 *   - hailo_probe correctly decodes vendor/device IDs
 *   - ATR[0] programming + BAR4 pass-through to a mocked device SRAM
 *   - hailo_validate_firmware rejects bad magic / oversized code /
 *     truncated blobs, accepts a hand-built valid header
 *
 * No floating-point. Compiles under -mgeneral-regs-only.
 */

#include "unity.h"
#include "../ai_accel/hailo/hailo.h"
#include "../ai_accel/hailo/hailo_control.h"
#include "../ai_accel/hailo/hailo_cs_actions.h"
#include "../ai_accel/hailo/hailo_cs_builder.h"
#include "../ai_accel/hailo/hailo_internal.h"
#include "../ai_accel/hailo/hailo_infer.h"
#include "../ai_accel/hailo/hailo_tensor.h"
#include "../ai_accel/hailo/hailo_vdma.h"
#include "../ai_accel/hailo/hef_parser.h"
#include "../ai_accel/hailo/hef_header.h"
#include "../include/inference_device.h"
#include "../include/md5.h"
#include "../include/uart.h"

/* Backend registration + test hooks from kernel/inference/inference_device_hailo.c. */
extern int inference_device_hailo_register(void);
extern uint32_t hailo_backend_in_use_slots(void);
extern void hailo_backend_reset_slots_for_tests(void);
#include "test_harness.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Mocked platform                                                             */
/* -------------------------------------------------------------------------- */

/*
 * Two register banks: BAR0 (4 KB) for the PLDA bridge view, and
 * a "device SRAM" array that BAR4 reads/writes route through via
 * the ATR[0] window. The mock tracks the programmed ATR[0] target
 * and translates bar4_write/read into the SRAM array at
 * (atr0_target + offset).
 *
 * SRAM base = 0 so every Hailo-8 device-side address fits without
 * offset math. 1 MB covers code (0x60000), app/core FW headers
 * (0xA0000, 0xE0030), and the boot/trigger registers around 0xE0000
 * with room to spare. 1 MB of BSS is acceptable for test builds only.
 */
#define MOCK_BAR0_SIZE  0x4000u
/* BAR2 holds the VDMA channel registers — 32 B per channel ×
 * 16 channels = 512 B; round up to 4 KB for headroom. */
#define MOCK_BAR2_SIZE  0x1000u
#define MOCK_SRAM_BASE  0x00000000u
#define MOCK_SRAM_SIZE  0x00100000u   /* 1 MB — covers all Hailo-8 FW targets */

static uint8_t  mock_bar0[MOCK_BAR0_SIZE];
static uint8_t  mock_bar2[MOCK_BAR2_SIZE];
/* Per-channel "device responds" simulation hooks. When
 * mock_vdma_auto_advance is non-zero, every num_avail write also
 * advances num_proc to match — lets a test drive hailo_vdma_submit_and_wait
 * past its polling loop without a separate "fire completion" step. */
static bool     mock_vdma_auto_advance;
static uint8_t  mock_sram[MOCK_SRAM_SIZE];
static uint64_t mock_atr0_target;
static int      mock_init_calls;

/* Simulated firmware state. hailo_boot writes a 1 to trigger_address;
 * the mock then sets ATR[1]'s loaded magic so the post-trigger poll
 * converges. Tests that want to simulate a stuck-load failure clear
 * mock_fw_sim_set_atr1_magic before calling hailo_boot. */
static bool     mock_fw_sim_enabled;
static bool     mock_fw_sim_set_atr1_magic;   /* default true */
static uint32_t mock_trigger_writes;

/* Simulated control-channel state (Phase 5.2 / #281). Tests that
 * exercise hailo_control_identify and friends set a canned response
 * body here; the mock's bar4_write doorbell-handler picks it up. */
#define MOCK_CONTROL_RESP_MAX 512
static bool     mock_fw_sim_control_enabled;
static uint8_t  mock_fw_sim_control_resp[MOCK_CONTROL_RESP_MAX];
static uint32_t mock_fw_sim_control_resp_len;
static uint32_t mock_control_doorbells;
static uint32_t mock_control_core_doorbells;
static uint32_t mock_control_last_doorbell_val;
/* Sized to hold a full control-channel payload (HAILO_CONTROL_MAX_BUFFER_LENGTH).
 * A WRITE_MEMORY chunk with a 1024 B data body totals 32 B header + 1024 B
 * data = 1056 B on the wire, which exceeded the old 512 B cap and caused
 * the smart-memory handler to silently skip the backing-store copy when
 * WRITE_MEMORY was captured in 1-KB-chunks mode. */
static uint8_t  mock_last_control_request[HAILO_CONTROL_MAX_BUFFER_LENGTH];
static uint32_t mock_last_control_request_len;
/* IRQ arming observability: the transport must unmask interrupts in
 * BSC_IMASK_HOST exactly once per lifetime, and clear any stale bits
 * out of BCS_ISTATUS_HOST before the first send.
 *
 * mock_istatus_one_shot_preload lets tests inject a single non-
 * FW_CONTROL bit into the NEXT BCS_ISTATUS_HOST read to verify the
 * poll loop ignores unrelated sources. It is cleared by mock_read32
 * after being returned once (single-shot semantics) so subsequent
 * reads fall through to the normal mock_bar0-backed path. */
static uint32_t mock_imask_writes;
static uint32_t mock_imask_last_value;
static uint32_t mock_istatus_clears_all;
static uint32_t mock_istatus_one_shot_preload;

/*
 * Smart WRITE_MEMORY / READ_MEMORY simulation. When enabled, the
 * mock parses the captured request's opcode and, for the two
 * memory opcodes, services the round-trip against a small backing
 * store instead of the canned-response path. Lets tests do a real
 * WRITE pattern → READ back → memcmp check.
 *
 * The backing store is byte-addressable with wrap-mod over
 * MOCK_MEMORY_SIZE, so tests can use any firmware-side address
 * without worrying about the actual SRAM layout on real hardware.
 */
#define MOCK_MEMORY_SIZE 4096u
static bool     mock_fw_sim_smart_memory_enabled;
static uint8_t  mock_fw_sim_device_memory[MOCK_MEMORY_SIZE];

/* DMA-mock state used by both mock_reset and the dma_alloc /
 * dma_free / cache_* hooks. Kept at file scope (rather than
 * grouped with mock_ops further down) so mock_reset can touch
 * the counters without needing forward declarations. */
/* Pool is large enough for tensor + VDMA-descriptor-list allocations
 * and aligned to HAILO_VDMA_DESC_LIST_ALIGN (64 KB) — the strictest
 * alignment any Hailo DMA structure requires. Smaller-alignment
 * requests land correctly via the offset math below. */
#define MOCK_DMA_POOL_SIZE (1024u * 1024u)
static alignas(HAILO_VDMA_DESC_LIST_ALIGN) uint8_t mock_dma_pool[MOCK_DMA_POOL_SIZE];
static size_t   mock_dma_next_off;
static bool     mock_dma_force_null;
static uint32_t mock_dma_alloc_calls;
static uint32_t mock_dma_free_calls;
static uint32_t mock_cache_clean_calls;
static uint32_t mock_cache_invalidate_calls;
static size_t   mock_last_cache_clean_size;
static size_t   mock_last_cache_invalidate_size;

/* Smart CONFIG_STREAM simulation. When enabled the mock synthesizes
 * a successful response carrying the supplied dataflow_manager_id,
 * so tests can assert the driver correctly unpacks the BE-wrapped
 * response body. */
static bool    mock_fw_sim_config_stream_enabled;
static uint8_t mock_fw_sim_config_stream_manager_id;

static void mock_reset(void)
{
    memset(mock_bar0, 0, sizeof(mock_bar0));
    memset(mock_bar2, 0, sizeof(mock_bar2));
    mock_vdma_auto_advance = false;
    memset(mock_sram, 0, sizeof(mock_sram));
    mock_atr0_target = 0;
    mock_init_calls  = 0;
    mock_fw_sim_enabled = true;
    mock_fw_sim_set_atr1_magic = true;
    mock_trigger_writes = 0;
    mock_fw_sim_control_enabled = false;
    memset(mock_fw_sim_control_resp, 0, sizeof(mock_fw_sim_control_resp));
    mock_fw_sim_control_resp_len = 0;
    mock_control_doorbells = 0;
    mock_control_core_doorbells = 0;
    mock_control_last_doorbell_val = 0;
    memset(mock_last_control_request, 0, sizeof(mock_last_control_request));
    mock_last_control_request_len = 0;
    mock_imask_writes   = 0;
    mock_imask_last_value = 0;
    mock_istatus_clears_all = 0;
    mock_istatus_one_shot_preload = 0;
    mock_fw_sim_smart_memory_enabled = false;
    memset(mock_fw_sim_device_memory, 0, sizeof(mock_fw_sim_device_memory));
    /* DMA-allocator + cache-hook observability — reset pool and
     * counters so per-test assertions start from zero. */
    memset(mock_dma_pool, 0, sizeof(mock_dma_pool));
    mock_dma_next_off = 0;
    mock_dma_force_null = false;
    mock_dma_alloc_calls = 0;
    mock_dma_free_calls = 0;
    mock_cache_clean_calls = 0;
    mock_cache_invalidate_calls = 0;
    mock_last_cache_clean_size = 0;
    mock_last_cache_invalidate_size = 0;
    mock_fw_sim_config_stream_enabled = false;
    mock_fw_sim_config_stream_manager_id = 0;
    hailo_control_reset_state_for_tests();
}

static int mock_init(void)
{
    mock_init_calls++;
    return HAILO_OK;
}

static uint32_t mock_read32(uint8_t bar, uint32_t offset)
{
    /* Tests that want to verify the poll loop ignores non-FW_CONTROL
     * SW interrupts set mock_istatus_one_shot_preload to a non-zero
     * pattern; return it on the first read of BCS_ISTATUS_HOST, then
     * clear it so subsequent reads fall through to the normal path
     * (zero, unless the control simulator wrote FW_CONTROL_IRQ). */
    if (bar == HAILO_BAR_CONFIG
     && offset == HAILO_BCS_ISTATUS_HOST
     && mock_istatus_one_shot_preload != 0) {
        uint32_t v = mock_istatus_one_shot_preload;
        mock_istatus_one_shot_preload = 0;
        return v;
    }
    if (bar == HAILO_BAR_CONFIG && offset + 4 <= MOCK_BAR0_SIZE) {
        uint32_t v;
        memcpy(&v, &mock_bar0[offset], sizeof(v));
        return v;
    }
    if (bar == HAILO_BAR_VDMA && offset + 4 <= MOCK_BAR2_SIZE) {
        uint32_t v;
        memcpy(&v, &mock_bar2[offset], sizeof(v));
        return v;
    }
    return 0xFFFFFFFFu;
}

static void mock_write32(uint8_t bar, uint32_t offset, uint32_t value)
{
    /* BCS_ISTATUS_HOST is write-1-to-clear on real hardware
     * (see hailo-pcie-common.c:read_and_clear_reg). Stored
     * value becomes stored & ~value. Simulating plain store
     * here would make a 0xFFFFFFFF write-to-clear look like
     * "every bit set" on the next read and the poll loop would
     * immediately succeed with a zeroed response area. */
    if (bar == HAILO_BAR_CONFIG && offset == HAILO_BCS_ISTATUS_HOST) {
        uint32_t cur;
        memcpy(&cur, &mock_bar0[offset], sizeof(cur));
        cur &= ~value;
        memcpy(&mock_bar0[offset], &cur, sizeof(cur));
        if (value == 0xFFFFFFFFu) {
            mock_istatus_clears_all++;
        }
    } else if (bar == HAILO_BAR_CONFIG && offset + 4 <= MOCK_BAR0_SIZE) {
        memcpy(&mock_bar0[offset], &value, sizeof(value));
    }
    /* Observability for the IRQ-arming contract. IMASK must be
     * OR'd with BSC_ISTATUS_HOST_MASK exactly once per lifetime. */
    if (bar == HAILO_BAR_CONFIG && offset == HAILO_BSC_IMASK_HOST) {
        mock_imask_writes++;
        mock_imask_last_value = value;
    }
    /* Track ATR[0].trsl_addr_lo writes for the atr0 translation. */
    if (bar == HAILO_BAR_CONFIG
     && offset == HAILO_ATR_BASE + HAILO_ATR_OFF_TRSL_ADDR_LO) {
        mock_atr0_target = (mock_atr0_target & 0xFFFFFFFF00000000ULL)
                         | (uint64_t)value;
    }
    if (bar == HAILO_BAR_CONFIG
     && offset == HAILO_ATR_BASE + HAILO_ATR_OFF_TRSL_ADDR_HI) {
        mock_atr0_target = (mock_atr0_target & 0xFFFFFFFFULL)
                         | ((uint64_t)value << 32);
    }
    /* BAR2 (VDMA channel regs): plain 32-bit store. The VDMA
     * register block is packed u8/u16 fields, so the driver does
     * read-modify-write sequences — reflected by reads servicing
     * from the same mock_bar2 backing. When auto-advance is on,
     * any write to a channel's NUM_AVAIL (offset 2 within the
     * 4-byte dword at CHANNEL_CONTROL_OFFSET) also bumps NUM_PROC
     * (16-bit value at CHANNEL_NUM_PROC_OFFSET=0x04) to match,
     * simulating the device completing the transfer immediately. */
    if (bar == HAILO_BAR_VDMA && offset + 4 <= MOCK_BAR2_SIZE) {
        memcpy(&mock_bar2[offset], &value, sizeof(value));
        if (mock_vdma_auto_advance
         && (offset & 0x1Fu) == 0) {
            /* Write to the channel-base dword. NUM_AVAIL lives in
             * bits [31:16] of this dword. Mirror it to NUM_PROC
             * (16-bit at offset+0x04) so a subsequent poll of
             * num_proc sees completion. */
            uint16_t num_avail = (uint16_t)(value >> 16);
            memcpy(&mock_bar2[offset + 0x04],
                   &num_avail, sizeof(num_avail));
        }
    }
}

static void mock_bar4_read(uint32_t offset, void *dst, size_t n)
{
    uint64_t dev_addr = mock_atr0_target + offset;
    uint64_t sram_off = (dev_addr - MOCK_SRAM_BASE) % MOCK_SRAM_SIZE;
    if (sram_off + n > MOCK_SRAM_SIZE) return;
    memcpy(dst, &mock_sram[sram_off], n);
}

/* Simulated firmware reaction to a trigger-address write. On real
 * hardware, the boot ROM loads our uploaded FW and firmware then
 * writes HAILO_ATR1_FW_LOADED_MAGIC into ATR[1].trsl_addr_lo to
 * signal "running" back to the driver. Tests can disable the sim
 * (mock_fw_sim_set_atr1_magic = false) to reproduce the load-never-
 * completed timeout path. */
static void mock_simulate_fw_after_trigger(void)
{
    if (!mock_fw_sim_enabled) return;
    if (mock_fw_sim_set_atr1_magic) {
        uint32_t atr1_lo_off = HAILO_ATR_BASE + HAILO_ATR_STRIDE
                             + HAILO_ATR_OFF_TRSL_ADDR_LO;
        uint32_t magic = HAILO_ATR1_FW_LOADED_MAGIC;
        memcpy(&mock_bar0[atr1_lo_off], &magic, sizeof(magic));
    }
}

/*
 * Build an in-memory response echoing the captured request's
 * common header (version + sequence) and opcode, applying status
 * and any trailing body the caller supplied. Used by the smart-
 * memory path to synthesize WRITE/READ_MEMORY responses on the
 * fly. `body` may be NULL iff body_len == 0.
 */
static void mock_build_echo_response(uint8_t *out,
                                     uint32_t *out_len,
                                     uint32_t req_opcode_be,
                                     uint32_t req_sequence_be,
                                     const void *body,
                                     uint32_t body_len)
{
    struct hailo_control_response_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    /* Echo back version + sequence + opcode from the request.
     * status fields are already zeroed via memset → success. */
    memcpy(&hdr.common.version,  mock_last_control_request +  0, 4);
    memcpy(&hdr.common.sequence, mock_last_control_request +  8, 4);
    hdr.common.opcode = req_opcode_be;
    (void)req_sequence_be;   /* already copied above */

    uint32_t off = 0;
    memcpy(out + off, &hdr, sizeof(hdr));           off += sizeof(hdr);
    uint32_t param_count = 0;                       /* no structured parameters */
    memcpy(out + off, &param_count, sizeof(param_count));
    off += sizeof(param_count);
    if (body_len) {
        memcpy(out + off, body, body_len);
        off += body_len;
    }
    *out_len = off;
}

/*
 * Smart handler for WRITE_MEMORY / READ_MEMORY. Returns true iff
 * it produced a response body (written into `resp_out` with
 * `*resp_out_len`). Returns false if smart mode is off or the
 * request opcode isn't one it handles — the caller then falls
 * back to the canned-response path.
 */
static bool mock_smart_memory_handle(uint32_t opcode_native,
                                     uint8_t *resp_out,
                                     uint32_t *resp_out_len)
{
    uint32_t req_opcode_be;
    memcpy(&req_opcode_be, mock_last_control_request + 12, 4);

    /* CONFIG_STREAM is gated by its own flag, independent of the
     * smart-memory toggle. Response shape is:
     *   [response_header(24)][parameter_count=0(4)]
     *   [dataflow_manager_id_length=1(4, BE)]
     *   [dataflow_manager_id(1)] */
    if (opcode_native == HAILO_CONTROL_OPCODE_CONFIG_STREAM
     && mock_fw_sim_config_stream_enabled) {
        struct {
            uint32_t dmid_length_be;
            uint8_t  dataflow_manager_id;
        } __attribute__((packed)) body;
        body.dmid_length_be = __builtin_bswap32(1u);
        body.dataflow_manager_id = mock_fw_sim_config_stream_manager_id;
        mock_build_echo_response(resp_out, resp_out_len,
                                 req_opcode_be, 0, &body, sizeof(body));
        return true;
    }

    if (!mock_fw_sim_smart_memory_enabled) return false;

    if (opcode_native == HAILO_CONTROL_OPCODE_WRITE_MEMORY) {
        /* Request body after the common header:
         *   [parameter_count(4, BE)] [address_length(4, BE)]
         *   [address(4, BE)] [data_length(4, BE)] [data(...)] */
        uint32_t address_be, data_length_be;
        memcpy(&address_be,     mock_last_control_request + 24, 4);
        memcpy(&data_length_be, mock_last_control_request + 28, 4);
        uint32_t address     = __builtin_bswap32(address_be);
        uint32_t data_length = __builtin_bswap32(data_length_be);
        if (data_length <= MOCK_MEMORY_SIZE
         && 32 + data_length <= mock_last_control_request_len) {
            uint32_t slot = address % MOCK_MEMORY_SIZE;
            uint32_t n    = data_length;
            if (slot + n <= MOCK_MEMORY_SIZE) {
                memcpy(&mock_fw_sim_device_memory[slot],
                       mock_last_control_request + 32, n);
            } else {
                uint32_t first = MOCK_MEMORY_SIZE - slot;
                memcpy(&mock_fw_sim_device_memory[slot],
                       mock_last_control_request + 32, first);
                memcpy(&mock_fw_sim_device_memory[0],
                       mock_last_control_request + 32 + first, n - first);
            }
        }
        /* Response: echo header, status=0, param_count=0, no body. */
        mock_build_echo_response(resp_out, resp_out_len,
                                 req_opcode_be, 0, NULL, 0);
        return true;
    }

    if (opcode_native == HAILO_CONTROL_OPCODE_READ_MEMORY) {
        /* Request body:
         *   [parameter_count(4)] [address_length(4)]
         *   [address(4)] [data_count_length(4)] [data_count(4)] */
        uint32_t address_be, data_count_be;
        memcpy(&address_be,    mock_last_control_request + 24, 4);
        memcpy(&data_count_be, mock_last_control_request + 32, 4);
        uint32_t address    = __builtin_bswap32(address_be);
        uint32_t data_count = __builtin_bswap32(data_count_be);
        if (data_count > MOCK_MEMORY_SIZE) data_count = MOCK_MEMORY_SIZE;

        uint8_t body[sizeof(uint32_t) + MOCK_MEMORY_SIZE];
        uint32_t data_length_be = __builtin_bswap32(data_count);
        memcpy(body, &data_length_be, sizeof(uint32_t));
        uint32_t slot = address % MOCK_MEMORY_SIZE;
        if (slot + data_count <= MOCK_MEMORY_SIZE) {
            memcpy(body + sizeof(uint32_t),
                   &mock_fw_sim_device_memory[slot], data_count);
        } else {
            uint32_t first = MOCK_MEMORY_SIZE - slot;
            memcpy(body + sizeof(uint32_t),
                   &mock_fw_sim_device_memory[slot], first);
            memcpy(body + sizeof(uint32_t) + first,
                   &mock_fw_sim_device_memory[0], data_count - first);
        }
        mock_build_echo_response(resp_out, resp_out_len,
                                 req_opcode_be, 0,
                                 body, sizeof(uint32_t) + data_count);
        return true;
    }

    return false;
}

/* Simulated firmware reaction to a control-channel doorbell (post-
 * boot). The driver writes the request bytes to BAR4[0..] and then
 * pokes raise_ready_offset on BAR4 with APP_CPU_CONTROL_MASK; real
 * firmware would pick up the request, process it, and write the
 * response to BAR4[0x640..]. Here we:
 *  1. Capture the request (for test inspection).
 *  2. Dispatch: if smart mode recognizes the opcode, synthesize a
 *     matching response; else emit the test-provided canned body.
 *  3. Write the response with a freshly-computed MD5.
 *  4. Set BCS_ISTATUS_HOST's FW_CONTROL bit so the poll terminates. */
static void mock_simulate_fw_control_response(void)
{
    /* Capture request from BAR4[0..]: the driver writes
     * [md5][len][payload] at BAR4 offset 0, which — via ATR[0]
     * pointed at HAILO_CONTROL_SECTION_ADDR_H8 (0x60000000) and
     * our modular SRAM mapping — lands at mock_sram[0..]. */
    uint64_t req_sram_off = (mock_atr0_target - MOCK_SRAM_BASE) % MOCK_SRAM_SIZE;
    uint32_t req_hdr_size = MD5_DIGEST_LENGTH + sizeof(uint32_t);
    if (req_sram_off + req_hdr_size <= MOCK_SRAM_SIZE) {
        uint32_t payload_len = 0;
        memcpy(&payload_len, &mock_sram[req_sram_off + MD5_DIGEST_LENGTH],
               sizeof(uint32_t));
        if (payload_len > sizeof(mock_last_control_request)) {
            payload_len = sizeof(mock_last_control_request);
        }
        memcpy(mock_last_control_request,
               &mock_sram[req_sram_off + MD5_DIGEST_LENGTH + sizeof(uint32_t)],
               payload_len);
        mock_last_control_request_len = payload_len;
    }

    /* Figure out what response body to emit. Smart-memory mode
     * handles WRITE/READ_MEMORY; everything else falls back to the
     * test-provided canned body. */
    static uint8_t synth_resp[sizeof(struct hailo_control_response_header)
                              + 4   /* parameter_count */
                              + 4   /* data_length     */
                              + MOCK_MEMORY_SIZE];
    uint32_t body_len = 0;
    const uint8_t *body = NULL;

    uint32_t opcode_be = 0;
    if (mock_last_control_request_len >= 16) {
        memcpy(&opcode_be, mock_last_control_request + 12, 4);
    }
    uint32_t opcode_native = __builtin_bswap32(opcode_be);
    uint32_t synth_len = 0;
    if (mock_smart_memory_handle(opcode_native, synth_resp, &synth_len)) {
        body_len = synth_len;
        body     = synth_resp;
    } else if (mock_fw_sim_control_enabled
            && mock_fw_sim_control_resp_len != 0) {
        body_len = mock_fw_sim_control_resp_len;
        body     = mock_fw_sim_control_resp;
    } else {
        /* No canned response, no smart handler — nothing to emit. */
        return;
    }

    /* Build the response wire bytes: [md5 over body][len][body].
     * MD5 covers the payload only; see hailo_control.c for the
     * HailoRT reference. */
    uint8_t md5[MD5_DIGEST_LENGTH];
    md5_compute(body, body_len, md5);

    uint64_t resp_sram_off = ((mock_atr0_target - MOCK_SRAM_BASE)
                              + HAILO_CONTROL_REQUEST_RESPONSE_OFFSET)
                             % MOCK_SRAM_SIZE;
    if (resp_sram_off + MD5_DIGEST_LENGTH + sizeof(uint32_t) + body_len
        > MOCK_SRAM_SIZE) {
        return;
    }
    memcpy(&mock_sram[resp_sram_off], md5, MD5_DIGEST_LENGTH);
    memcpy(&mock_sram[resp_sram_off + MD5_DIGEST_LENGTH],
           &body_len, sizeof(uint32_t));
    memcpy(&mock_sram[resp_sram_off + MD5_DIGEST_LENGTH + sizeof(uint32_t)],
           body, body_len);

    /* Signal "control response ready" by setting the specific
     * FW_CONTROL_IRQ bit in the SW_IRQ field of BCS_ISTATUS_HOST.
     * The driver's poll distinguishes this from notifications,
     * VDMA transfers, etc., so raising any other bit would hang
     * the test (or worse, let it consume the wrong event). */
    uint32_t istatus = HAILO_BCS_ISTATUS_HOST_FW_CONTROL_BIT;
    memcpy(&mock_bar0[HAILO_BCS_ISTATUS_HOST], &istatus, sizeof(istatus));
}

static void mock_bar4_write(uint32_t offset, const void *src, size_t n)
{
    uint64_t dev_addr = mock_atr0_target + offset;
    uint64_t sram_off = (dev_addr - MOCK_SRAM_BASE) % MOCK_SRAM_SIZE;
    /* Wrap-mod SRAM: a real Hailo control request-response address
     * (0x60000000 + small_offset) collides with 0 + small_offset,
     * which is fine for tests because each test wipes the SRAM
     * region it cares about. Boot-path device addresses (app_fw_code
     * at 0x60000, core_fw_header at 0xA0000, boot_status at 0xE0000)
     * are all within MOCK_SRAM_SIZE (1 MB) so modular collapse on
     * them is a no-op. */
    if (sram_off + n <= MOCK_SRAM_SIZE) {
        memcpy(&mock_sram[sram_off], src, n);
    }

    /* Watch for the trigger-address doorbell (hailo_boot path). */
    if (dev_addr == hailo_fw_addrs_hailo8.trigger_address
     && n >= sizeof(uint32_t)) {
        uint32_t v;
        memcpy(&v, src, sizeof(v));
        if (v == HAILO_FW_TRIGGER_VALUE) {
            mock_trigger_writes++;
            mock_simulate_fw_after_trigger();
        }
    }

    /* Watch for the control-channel doorbell (post-boot, #281).
     * Check on the raw BAR4 offset rather than the ATR-translated
     * device address — control_retarget_atr0 re-points ATR[0] at
     * 0x60000000 (Hailo-8 control section), so dev_addr for this
     * write is 0x60000000 + raise_ready_offset, not just
     * raise_ready_offset alone. */
    if (offset == hailo_fw_addrs_hailo8.raise_ready_offset
     && n >= sizeof(uint32_t)) {
        uint32_t v;
        memcpy(&v, src, sizeof(v));
        mock_control_last_doorbell_val = v;
        if (v == HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK) {
            mock_control_doorbells++;
            mock_simulate_fw_control_response();
        } else if (v == HAILO_FW_ACCESS_CORE_CPU_CONTROL_MASK) {
            mock_control_core_doorbells++;
            mock_simulate_fw_control_response();
        }
    }
}

/*
 * Simple bump allocator backing the mock DMA pool. The pool and
 * counters are declared up near mock_reset; the allocator body
 * lives here with the other platform hooks. Reset between tests
 * via mock_reset.
 */
static void *mock_dma_alloc(size_t size, size_t align, uint64_t *iova_out)
{
    mock_dma_alloc_calls++;
    if (mock_dma_force_null) {
        if (iova_out) *iova_out = 0;
        return NULL;
    }
    /* Round the current offset up to the requested alignment. */
    size_t aligned_off = (mock_dma_next_off + align - 1u) & ~(align - 1u);
    if (aligned_off + size > sizeof(mock_dma_pool)) {
        if (iova_out) *iova_out = 0;
        return NULL;
    }
    void *p = &mock_dma_pool[aligned_off];
    mock_dma_next_off = aligned_off + size;
    /* IOVA is the same as the host virtual address in the mock —
     * identity mapping, matching what the Pi 5 NC allocator does. */
    if (iova_out) *iova_out = (uint64_t)(uintptr_t)p;
    return p;
}
static void  mock_dma_free(void *ptr, size_t size, size_t align)
{
    (void)ptr; (void)size; (void)align;
    mock_dma_free_calls++;
    /* Bump allocator never shrinks. Tests that care about resource
     * accounting assert on the free-count instead. */
}
static void  mock_cache_clean(const void *a, size_t n)
{
    (void)a;
    mock_cache_clean_calls++;
    mock_last_cache_clean_size = n;
}
static void  mock_cache_invalidate(void *a, size_t n)
{
    (void)a;
    mock_cache_invalidate_calls++;
    mock_last_cache_invalidate_size = n;
}
static void  mock_mb(void)                             {}
static void  mock_udelay(uint32_t u)                   { (void)u; }

static const struct hailo_platform_ops mock_ops = {
    .name             = "mock",
    .init             = mock_init,
    .shutdown         = NULL,
    .read32           = mock_read32,
    .write32          = mock_write32,
    .bar4_write       = mock_bar4_write,
    .bar4_read        = mock_bar4_read,
    .dma_alloc        = mock_dma_alloc,
    .dma_free         = mock_dma_free,
    .cache_clean      = mock_cache_clean,
    .cache_invalidate = mock_cache_invalidate,
    .mb               = mock_mb,
    .udelay           = mock_udelay,
    .register_irq     = NULL,
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* Seed BAR0 so vendor/device read returns specific values. */
static void seed_ids(uint16_t vendor, uint16_t device)
{
    uint32_t id = (uint32_t)vendor | ((uint32_t)device << 16);
    memcpy(&mock_bar0[HAILO_REG_VENDOR], &id, sizeof(id));
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                       */
/* -------------------------------------------------------------------------- */

/*
 * Negative tests below point `hailo_platform` at stack-local structs
 * to exercise validation failures. Each test must restore the
 * original pointer on exit — otherwise the next test sees a dangling
 * pointer to this function's (now-dead) stack frame and derefs it.
 */
static void test_init_rejects_null_ops(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    hailo_platform = NULL;
    int rc = hailo_init();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
    hailo_platform = saved;
}

static void test_init_rejects_incomplete_ops(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    struct hailo_platform_ops bad = mock_ops;
    bad.read32 = NULL;
    hailo_platform = &bad;
    int rc = hailo_init();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
    hailo_platform = saved;
}

/* Regression: mb() is required (was optional; used by ATR retarget).
 * A caller that forgets it would silently skip the barrier and
 * corrupt the access ordering on strongly-ordered hardware. */
static void test_init_rejects_missing_mb(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    struct hailo_platform_ops bad = mock_ops;
    bad.mb = NULL;
    hailo_platform = &bad;
    int rc = hailo_init();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
    hailo_platform = saved;
}

static void test_init_accepts_complete_ops(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
    int rc = hailo_init();
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
    TEST_ASSERT_EQUAL_INT(1, mock_init_calls);
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_UNINIT,
                          (int)hailo_get_state());
}

static void test_probe_detects_no_device(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
    (void)hailo_init();
    /* BAR0 all-zeros → vendor=0 → no device. */
    uint16_t v = 0xABCD, d = 0xABCD;
    int rc = hailo_probe(&v, &d);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV, rc);
}

static void test_probe_rejects_wrong_vendor(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
    (void)hailo_init();
    seed_ids(0x8086, 0x1234);   /* Intel, not Hailo */
    uint16_t v = 0, d = 0;
    int rc = hailo_probe(&v, &d);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO, rc);
    TEST_ASSERT_EQUAL_HEX16(0x8086, v);
}

static void test_probe_reads_ids_then_boot_status(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
    (void)hailo_init();

    seed_ids(HAILO_PCI_VENDOR_ID, HAILO_PCI_DEVICE_HAILO8);

    /* Seed mock_sram[0] with a known boot_status value and point
     * ATR[0] at boot_status' 4 KB page before calling probe. The
     * core will reprogram ATR[0] itself; to make the read land on
     * our mocked SRAM, use the one-window mock: the dev_read32
     * code will write ATR[0]'s trsl_addr to the page base
     * (0xE0000), then bar4_read offset 0, and the mock translates
     * that to mock_atr0_target (= 0xE0000) + 0 which must hit the
     * SRAM backing store. We shift MOCK_SRAM_BASE's effective
     * rooting by seeding SRAM at offset computed below.
     *
     * Since MOCK_SRAM_BASE is 0x60000, the boot_status read at
     * 0xE0000 lands at sram_off = 0x80000 — past the 0x20000 SRAM
     * size. So the mock's range check rejects the read and
     * bar4_read leaves dst untouched. hailo_probe therefore reads
     * 0 from boot_status and logs it; it does NOT error out on a
     * garbage value (the field is an observability hint, not a
     * gate). Assert the probe still succeeds. */
    uint16_t v = 0, d = 0;
    int rc = hailo_probe(&v, &d);
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
    TEST_ASSERT_EQUAL_HEX16(HAILO_PCI_VENDOR_ID, v);
    TEST_ASSERT_EQUAL_HEX16(HAILO_PCI_DEVICE_HAILO8, d);
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_PROBED,
                          (int)hailo_get_state());
}

static void test_validate_firmware_rejects_short_blob(void)
{
    uint8_t tiny[8] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_validate_firmware(tiny, sizeof(tiny)));
}

static void test_validate_firmware_rejects_bad_magic(void)
{
    struct hailo_firmware_header bad = {
        .magic = 0xDEADBEEF, .code_size = 0x100,
    };
    uint8_t blob[sizeof(bad) + 0x100];
    memcpy(blob, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_validate_firmware(blob, sizeof(blob)));
}

static void test_validate_firmware_rejects_oversize_code(void)
{
    struct hailo_firmware_header bad = {
        .magic = HAILO_FW_MAGIC_HAILO8,
        .code_size = HAILO_FW_MAX_CODE_SIZE + 1,
    };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_validate_firmware(&bad, sizeof(bad)));
}

static void test_validate_firmware_rejects_truncated(void)
{
    struct hailo_firmware_header hdr = {
        .magic = HAILO_FW_MAGIC_HAILO8,
        .code_size = 0x100,
    };
    /* Blob says there are 0x100 bytes of code but only 0x40 present. */
    uint8_t blob[sizeof(hdr) + 0x40] = {0};
    memcpy(blob, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_validate_firmware(blob, sizeof(blob)));
}

static void test_validate_firmware_accepts_minimal_valid(void)
{
    struct hailo_firmware_header hdr = {
        .magic           = HAILO_FW_MAGIC_HAILO8,
        .header_version  = 0,
        .firmware_major  = 1,
        .firmware_minor  = 2,
        .firmware_revision = 3,
        .code_size       = 0x100,
    };
    uint8_t blob[sizeof(hdr) + 0x100] = {0};
    memcpy(blob, &hdr, sizeof(hdr));
    int rc = hailo_validate_firmware(blob, sizeof(blob));
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
}

static void test_validate_firmware_rejects_zero_code_size(void)
{
    struct hailo_firmware_header bad = {
        .magic = HAILO_FW_MAGIC_HAILO8,
        .code_size = 0,
    };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_validate_firmware(&bad, sizeof(bad)));
}

static void test_validate_firmware_rejects_null_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_validate_firmware(NULL, 128));
}

/*
 * Platform init failure must flip state to FAILED and prevent
 * subsequent probes. Uses a platform ops table whose init()
 * returns a negative error.
 */
static int mock_init_fail(void) { return HAILO_ERR_IO; }

static void test_init_propagates_platform_init_failure(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    mock_reset();
    struct hailo_platform_ops failing = mock_ops;
    failing.init = mock_init_fail;
    hailo_platform = &failing;

    int rc = hailo_init();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO, rc);
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED,
                          (int)hailo_get_state());

    /* Probe must not advance from FAILED. */
    uint16_t v = 0, d = 0;
    rc = hailo_probe(&v, &d);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV, rc);
    hailo_platform = saved;
}

static void test_state_str_labels_known_values(void)
{
    /* Guards the shell command's output against silent renames. */
    TEST_ASSERT_EQUAL_STRING("uninit", hailo_state_str(HAILO_STATE_UNINIT));
    TEST_ASSERT_EQUAL_STRING("probed", hailo_state_str(HAILO_STATE_PROBED));
    TEST_ASSERT_EQUAL_STRING("running", hailo_state_str(HAILO_STATE_RUNNING));
    TEST_ASSERT_EQUAL_STRING("failed", hailo_state_str(HAILO_STATE_FAILED));
}

/* -------------------------------------------------------------------------- */
/* Boot tests — exercise the full FW-upload / trigger / poll state machine. */
/* -------------------------------------------------------------------------- */

/* Build a minimal-but-well-formed firmware blob (header + dummy code +
 * cert header + dummy key/content). Returns total size written. All
 * sizes are 4-aligned. */
/*
 * Value that build_fw_blob (the no-core-size wrapper) passes to
 * build_fw_blob_full. Small enough that short tests don't need
 * large buffers. Tests that exercise the multi-chunk core path call
 * build_fw_blob_full directly with a larger size.
 */
#define BUILD_FW_BLOB_DEFAULT_CORE_CODE  4u

static size_t build_fw_blob_full(uint8_t *out, size_t out_cap,
                                 uint32_t code_size,
                                 uint32_t key_size,
                                 uint32_t content_size,
                                 uint32_t core_code_size,
                                 uint32_t fw_major, uint32_t fw_minor,
                                 uint32_t fw_rev)
{
    size_t need = sizeof(struct hailo_firmware_header) + code_size
                + sizeof(struct hailo_fw_cert_header) + key_size + content_size
                + sizeof(struct hailo_firmware_header) + core_code_size;
    if (need > out_cap) return 0;

    struct hailo_firmware_header hdr = {
        .magic = HAILO_FW_MAGIC_HAILO8, .header_version = 0,
        .firmware_major = fw_major, .firmware_minor = fw_minor,
        .firmware_revision = fw_rev, .code_size = code_size,
    };
    struct hailo_fw_cert_header cert = {
        .key_size = key_size, .content_size = content_size,
    };
    struct hailo_firmware_header core_hdr = {
        .magic = HAILO_FW_MAGIC_HAILO8, .header_version = 0,
        .firmware_major = fw_major, .firmware_minor = fw_minor,
        .firmware_revision = fw_rev, .code_size = core_code_size,
    };

    size_t off = 0;
    memcpy(out + off, &hdr, sizeof(hdr));               off += sizeof(hdr);
    for (uint32_t i = 0; i < code_size; i++) out[off++] = (uint8_t)(0x10 + i);
    memcpy(out + off, &cert, sizeof(cert));             off += sizeof(cert);
    for (uint32_t i = 0; i < key_size;     i++) out[off++] = (uint8_t)(0x40 + i);
    for (uint32_t i = 0; i < content_size; i++) out[off++] = (uint8_t)(0x80 + i);
    memcpy(out + off, &core_hdr, sizeof(core_hdr));     off += sizeof(core_hdr);
    for (uint32_t i = 0; i < core_code_size; i++) out[off++] = (uint8_t)(0xC0 + i);
    return off;
}

/*
 * Common-case wrapper: Hailo-8 production firmware carries BOTH app
 * and core sections. This builds both with a small (4 B) core code
 * for tests that don't care about the core payload. Tests that do
 * care use build_fw_blob_full with an explicit core_code_size.
 */
static size_t build_fw_blob(uint8_t *out, size_t out_cap,
                            uint32_t code_size,
                            uint32_t key_size,
                            uint32_t content_size,
                            uint32_t fw_major, uint32_t fw_minor, uint32_t fw_rev)
{
    return build_fw_blob_full(out, out_cap, code_size, key_size, content_size,
                              BUILD_FW_BLOB_DEFAULT_CORE_CODE,
                              fw_major, fw_minor, fw_rev);
}

/* Common setup: ops installed, device probed, mock SRAM seeded with
 * boot_status=UNINIT so a subsequent hailo_boot can start. */
static void boot_setup_probed(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
    seed_ids(HAILO_PCI_VENDOR_ID, HAILO_PCI_DEVICE_HAILO8);
    uint32_t uninit = HAILO_BOOT_STATUS_UNINIT;
    memcpy(&mock_sram[hailo_fw_addrs_hailo8.boot_status - MOCK_SRAM_BASE],
           &uninit, sizeof(uninit));
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_init());
    uint16_t v = 0, d = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_probe(&v, &d));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_PROBED, (int)hailo_get_state());
}

static void test_boot_rejects_wrong_state(void)
{
    /* Directly call boot from UNINIT — must refuse. */
    mock_reset();
    hailo_platform = &mock_ops;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_init());
    /* state stays UNINIT after init */
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, hailo_boot(blob, n));
}

static void test_boot_rejects_bad_magic(void)
{
    boot_setup_probed();
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    ((struct hailo_firmware_header *)blob)->magic = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_missing_cert(void)
{
    boot_setup_probed();
    /* Build header+code only; no cert trailer. validate_firmware
     * accepts this (cert is optional there), but hailo_boot must
     * reject it — Hailo-8 production FW always carries a cert. */
    uint8_t blob[sizeof(struct hailo_firmware_header) + 8];
    struct hailo_firmware_header hdr = {
        .magic = HAILO_FW_MAGIC_HAILO8, .code_size = 8,
    };
    memcpy(blob, &hdr, sizeof(hdr));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
                          hailo_boot(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_cert_oversize(void)
{
    boot_setup_probed();
    uint8_t blob[256];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    /* Poison the cert key_size to exceed the 4 KB bound. */
    struct hailo_fw_cert_header *cert =
        (struct hailo_fw_cert_header *)(blob
            + sizeof(struct hailo_firmware_header) + 4);
    cert->key_size = HAILO_FW_MAX_CERT_KEY + 4u;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_unexpected_boot_status(void)
{
    boot_setup_probed();
    /* Pretend device is past UNINIT — e.g. already in bootloader or
     * running. hailo_boot should refuse rather than corrupt state. */
    uint32_t running = 0x5u;
    memcpy(&mock_sram[hailo_fw_addrs_hailo8.boot_status - MOCK_SRAM_BASE],
           &running, sizeof(running));
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_succeeds_and_uploads_sections(void)
{
    boot_setup_probed();
    uint8_t blob[256];
    size_t n = build_fw_blob(blob, sizeof(blob), 16, 8, 12, 4, 21, 0);
    TEST_ASSERT_TRUE(n != 0);

    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_RUNNING, (int)hailo_get_state());

    /* Trigger doorbell must have been observed exactly once. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_trigger_writes);

    /* Verify each section landed where the device expects. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.boot_fw_header - MOCK_SRAM_BASE],
        blob, sizeof(struct hailo_firmware_header)));

    uint8_t *code_src = blob + sizeof(struct hailo_firmware_header);
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.app_fw_code_ram_base - MOCK_SRAM_BASE],
        code_src, 16));

    uint8_t *cert_start = code_src + 16;
    uint8_t *key_src    = cert_start + sizeof(struct hailo_fw_cert_header);
    uint8_t *content_src = key_src + 8;
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.boot_key_cert - MOCK_SRAM_BASE],
        key_src, 8));
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.boot_cont_cert - MOCK_SRAM_BASE],
        content_src, 12));

    /* Core section: build_fw_blob appends [core_header, core_code(4)]
     * after the cert content. Verify both land at their device-side
     * addresses — core_fw_header (0xA0000) and core_code_ram_base
     * (0xC0000) for Hailo-8. */
    uint8_t *core_hdr_src  = content_src + 12;
    uint8_t *core_code_src = core_hdr_src + sizeof(struct hailo_firmware_header);
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.core_fw_header - MOCK_SRAM_BASE],
        core_hdr_src, sizeof(struct hailo_firmware_header)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(
        &mock_sram[hailo_fw_addrs_hailo8.core_code_ram_base - MOCK_SRAM_BASE],
        core_code_src, 4));
}

static void test_boot_rejects_missing_core_fw(void)
{
    boot_setup_probed();
    /* Build app + cert only — no core firmware trailer. Hailo-8
     * production FW always carries a core section; hailo_boot must
     * reject a truncated blob. Manually build the blob since
     * build_fw_blob always appends a core section. */
    uint8_t blob[128];
    struct hailo_firmware_header app_hdr = {
        .magic = HAILO_FW_MAGIC_HAILO8, .code_size = 4,
    };
    struct hailo_fw_cert_header cert = { .key_size = 4, .content_size = 4 };
    size_t off = 0;
    memcpy(blob + off, &app_hdr, sizeof(app_hdr)); off += sizeof(app_hdr);
    for (int i = 0; i < 4; i++) blob[off++] = 0x10 + i;  /* code */
    memcpy(blob + off, &cert, sizeof(cert));       off += sizeof(cert);
    for (int i = 0; i < 4; i++) blob[off++] = 0x40 + i;  /* key */
    for (int i = 0; i < 4; i++) blob[off++] = 0x80 + i;  /* content */
    /* NO core header or code. */

    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, off));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

/* Helper: return offset of the core firmware header inside a blob
 * produced by build_fw_blob(code_size, key_size, content_size, ...). */
static size_t core_hdr_offset(uint32_t code_size, uint32_t key_size,
                              uint32_t content_size)
{
    return sizeof(struct hailo_firmware_header) + code_size
         + sizeof(struct hailo_fw_cert_header) + key_size + content_size;
}

static void test_boot_rejects_bad_core_magic(void)
{
    boot_setup_probed();
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    struct hailo_firmware_header *core = (struct hailo_firmware_header *)(
        blob + core_hdr_offset(4, 4, 4));
    core->magic = 0xCAFEBABEu;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_core_code_size_zero(void)
{
    boot_setup_probed();
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    struct hailo_firmware_header *core = (struct hailo_firmware_header *)(
        blob + core_hdr_offset(4, 4, 4));
    core->code_size = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_core_code_size_oversize(void)
{
    boot_setup_probed();
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    struct hailo_firmware_header *core = (struct hailo_firmware_header *)(
        blob + core_hdr_offset(4, 4, 4));
    core->code_size = HAILO_FW_MAX_CODE_SIZE + 4u;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_rejects_core_code_truncated(void)
{
    boot_setup_probed();
    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    /* Declare core_code_size larger than the remaining blob — forces
     * the core_end > fw_size truncation check. */
    struct hailo_firmware_header *core = (struct hailo_firmware_header *)(
        blob + core_hdr_offset(4, 4, 4));
    core->code_size = 512u;  /* but only 4 bytes actually follow */
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_fails_when_fw_never_signals_loaded(void)
{
    boot_setup_probed();
    /* Upload succeeds but FW never sets ATR[1]'s loaded magic.
     * Exercises the post-trigger poll-timeout path. */
    mock_fw_sim_set_atr1_magic = false;

    uint8_t blob[128];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 1, 2, 3);
    TEST_ASSERT_TRUE(n != 0);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_FAILED, (int)hailo_get_state());
}

static void test_boot_chunks_large_code(void)
{
    /* Exercise dev_write_chunked: code larger than one ATR window
     * (4 KB) must be uploaded correctly. Use a just-past-one-page
     * code_size so the second chunk is small. A static buffer keeps
     * the 8 KB blob off the test's 16 KB kernel stack. */
    boot_setup_probed();
    const uint32_t big_code = HAILO_ATR_TABLE_SIZE + 32u;  /* 4 KB + 32 B */
    static uint8_t boot_blob[HAILO_ATR_TABLE_SIZE * 2];
    size_t blob_cap = sizeof(struct hailo_firmware_header)
                    + big_code
                    + sizeof(struct hailo_fw_cert_header)
                    + 16u + 16u;
    TEST_ASSERT_TRUE(blob_cap <= sizeof(boot_blob));
    size_t n = build_fw_blob(boot_blob, sizeof(boot_blob),
                             big_code, 16, 16, 1, 0, 0);
    TEST_ASSERT_TRUE(n != 0);

    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_boot(boot_blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_RUNNING, (int)hailo_get_state());

    /* Spot-check first and last byte of the code section landed. */
    uint8_t *code_src = boot_blob + sizeof(struct hailo_firmware_header);
    uint8_t *code_dst = &mock_sram[
        hailo_fw_addrs_hailo8.app_fw_code_ram_base - MOCK_SRAM_BASE];
    TEST_ASSERT_EQUAL_HEX8(code_src[0],             code_dst[0]);
    TEST_ASSERT_EQUAL_HEX8(code_src[big_code - 1],  code_dst[big_code - 1]);
}

static void test_boot_core_chunks_large_code(void)
{
    /* Exercise dev_write_chunked on the CORE code path. Real Hailo-8
     * firmware has a ~58 KB core section (14+ ATR windows); without
     * this test the multi-chunk core upload has zero mock coverage
     * and only the real-hardware boot verifies the path. */
    boot_setup_probed();
    const uint32_t app_code  = 16u;
    const uint32_t key       = 16u;
    const uint32_t content   = 16u;
    const uint32_t big_core  = HAILO_ATR_TABLE_SIZE + 32u;  /* 4 KB + 32 B */
    static uint8_t boot_blob[HAILO_ATR_TABLE_SIZE * 2];
    size_t n = build_fw_blob_full(boot_blob, sizeof(boot_blob),
                                  app_code, key, content, big_core,
                                  1, 0, 0);
    TEST_ASSERT_TRUE(n != 0);

    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_boot(boot_blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_RUNNING, (int)hailo_get_state());

    /* Spot-check first and last byte of the core code landed. */
    size_t core_off = sizeof(struct hailo_firmware_header) + app_code
                    + sizeof(struct hailo_fw_cert_header) + key + content
                    + sizeof(struct hailo_firmware_header);
    uint8_t *core_code_src = boot_blob + core_off;
    uint8_t *core_code_dst = &mock_sram[
        hailo_fw_addrs_hailo8.core_code_ram_base - MOCK_SRAM_BASE];
    TEST_ASSERT_EQUAL_HEX8(core_code_src[0],             core_code_dst[0]);
    TEST_ASSERT_EQUAL_HEX8(core_code_src[big_core - 1],  core_code_dst[big_core - 1]);
}

/* -------------------------------------------------------------------------- */
/* Direct tests for the extracted decode helpers.                              */
/*                                                                             */
/* These bypass hailo_boot and the mock platform entirely, so they isolate     */
/* regressions in the pure bounds / format logic from state-machine bugs.      */
/* Helpers are declared in hailo_internal.h (not the public hailo.h).          */
/* -------------------------------------------------------------------------- */

/* Build a cert-only fragment starting at the given offset inside `out`:
 *   [cert_header(key_size, content_size)][key bytes...][content bytes...]
 * Returns total bytes written starting from cert_off (i.e. fragment size).
 * Key/content are filled with deterministic sentinels so tests can spot-
 * check the returned *out_key / *out_content pointers land on real data. */
static size_t emit_cert_fragment(uint8_t *out, size_t cert_off,
                                 uint32_t key_size, uint32_t content_size)
{
    struct hailo_fw_cert_header cert = {
        .key_size = key_size, .content_size = content_size,
    };
    size_t off = cert_off;
    memcpy(out + off, &cert, sizeof(cert)); off += sizeof(cert);
    for (uint32_t i = 0; i < key_size; i++)     out[off++] = (uint8_t)(0x40 + i);
    for (uint32_t i = 0; i < content_size; i++) out[off++] = (uint8_t)(0x80 + i);
    return off - cert_off;
}

static void test_decode_cert_rejects_null_blob(void)
{
    struct hailo_fw_cert_header cert;
    const uint8_t *key = NULL, *content = NULL;
    size_t cert_end = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_cert(NULL, 128, 0, &cert, &key, &content, &cert_end));
}

/* Four per-slot NULL-out-param tests — split from one bundled test so
 * a regression on any one slot names the specific slot in the failure
 * report instead of masking inside a shared test. */
static void test_decode_cert_rejects_null_cert_out(void)
{
    uint8_t blob[64] = { 0 };
    const uint8_t *key = NULL, *content = NULL;
    size_t cert_end = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_cert(blob, sizeof(blob), 0, NULL, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_null_key_out(void)
{
    uint8_t blob[64] = { 0 };
    struct hailo_fw_cert_header cert;
    const uint8_t *content = NULL;
    size_t cert_end = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, NULL, &content, &cert_end));
}

static void test_decode_cert_rejects_null_content_out(void)
{
    uint8_t blob[64] = { 0 };
    struct hailo_fw_cert_header cert;
    const uint8_t *key = NULL;
    size_t cert_end = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, NULL, &cert_end));
}

static void test_decode_cert_rejects_null_cert_end_out(void)
{
    uint8_t blob[64] = { 0 };
    struct hailo_fw_cert_header cert;
    const uint8_t *key = NULL, *content = NULL;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, NULL));
}

static void test_decode_cert_rejects_header_past_end(void)
{
    /* fw_size must be STRICTLY less than sizeof(cert_header) to fire
     * the "header runs past end" branch. `sizeof(struct
     * hailo_fw_cert_header) == 8`, so fw_size=4 (= sizeof(blob))
     * forces the cert_off + sizeof > fw_size check to fail before
     * the zeroed cert header is even read — otherwise the all-zero
     * header would land on the zero-key/zero-content rejection and
     * pass the assertion for the wrong reason. */
    uint8_t blob[4] = { 0 };
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_zero_key(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_cert_fragment(blob, 0, 0, 4);  /* key_size=0 */
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_zero_content(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_cert_fragment(blob, 0, 4, 0);
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_oversize_key(void)
{
    uint8_t blob[128] = { 0 };
    struct hailo_fw_cert_header bogus = {
        .key_size = HAILO_FW_MAX_CERT_KEY + 4u, .content_size = 4u,
    };
    memcpy(blob, &bogus, sizeof(bogus));
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_oversize_content(void)
{
    uint8_t blob[128] = { 0 };
    struct hailo_fw_cert_header bogus = {
        .key_size = 4u, .content_size = HAILO_FW_MAX_CERT_CONTENT + 4u,
    };
    memcpy(blob, &bogus, sizeof(bogus));
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_misaligned_key(void)
{
    uint8_t blob[128] = { 0 };
    struct hailo_fw_cert_header misaligned = {
        .key_size = 5u, .content_size = 4u,
    };
    memcpy(blob, &misaligned, sizeof(misaligned));
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_misaligned_content(void)
{
    uint8_t blob[128] = { 0 };
    struct hailo_fw_cert_header misaligned = {
        .key_size = 4u, .content_size = 7u,
    };
    memcpy(blob, &misaligned, sizeof(misaligned));
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(blob), 0, &cert, &key, &content, &cert_end));
}

static void test_decode_cert_rejects_truncated_payload(void)
{
    /* Buffer just barely holds the cert header; the declared
     * key_size + content_size trailer runs off the end. Using an
     * explicit fw_size smaller than blob[] isolates the cert-
     * truncation check from blob-sizing tricks. */
    uint8_t blob[64] = { 0 };
    struct hailo_fw_cert_header cert_hdr = {
        .key_size = 16u, .content_size = 16u,   /* needs 40 B total */
    };
    memcpy(blob, &cert_hdr, sizeof(cert_hdr));
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    /* fw_size = sizeof(cert_hdr) + 16: enough for key but not content. */
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, sizeof(cert_hdr) + 16u, 0,
                          &cert, &key, &content, &cert_end));
}

static void test_decode_cert_success_populates_outputs(void)
{
    uint8_t blob[128] = { 0 };
    const uint32_t pre_pad = 16;  /* simulate cert_off non-zero */
    size_t frag_len = emit_cert_fragment(blob, pre_pad, 8, 12);

    struct hailo_fw_cert_header cert;
    const uint8_t *key = NULL, *content = NULL;
    size_t cert_end = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_decode_cert(blob, sizeof(blob), pre_pad,
                          &cert, &key, &content, &cert_end));

    TEST_ASSERT_EQUAL_UINT32(8u,  cert.key_size);
    TEST_ASSERT_EQUAL_UINT32(12u, cert.content_size);
    TEST_ASSERT_EQUAL_PTR(blob + pre_pad + sizeof(cert), key);
    TEST_ASSERT_EQUAL_PTR(key + 8, content);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(pre_pad + frag_len), (uint64_t)cert_end);

    /* Sanity-check the sentinel data shows through the returned
     * pointers — proves out_key / out_content actually index the
     * caller's blob rather than some internal scratch. */
    TEST_ASSERT_EQUAL_HEX8(0x40, key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, content[0]);
}

/* Build a core-fw fragment (header + code) starting at `core_off`.
 * Caller picks magic / header_version / code_size so tests can poison
 * individual fields. */
static size_t emit_core_fragment(uint8_t *out, size_t core_off,
                                 uint32_t magic, uint32_t header_version,
                                 uint32_t code_size, uint32_t code_bytes_present)
{
    struct hailo_firmware_header core = {
        .magic = magic, .header_version = header_version,
        .code_size = code_size,
    };
    size_t off = core_off;
    memcpy(out + off, &core, sizeof(core)); off += sizeof(core);
    for (uint32_t i = 0; i < code_bytes_present; i++)
        out[off++] = (uint8_t)(0xC0 + i);
    return off - core_off;
}

static void test_decode_core_fw_rejects_null_blob(void)
{
    struct hailo_firmware_header core;
    const uint8_t *code = NULL;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_core_fw(NULL, 128, 0, &core, &code));
}

static void test_decode_core_fw_rejects_null_core_hdr_out(void)
{
    uint8_t blob[64] = { 0 };
    const uint8_t *code = NULL;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_core_fw(blob, sizeof(blob), 0, NULL, &code));
}

static void test_decode_core_fw_rejects_null_core_code_out(void)
{
    uint8_t blob[64] = { 0 };
    struct hailo_firmware_header core;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, NULL));
}

static void test_decode_core_fw_rejects_header_past_end(void)
{
    uint8_t blob[8] = { 0 };
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

static void test_decode_core_fw_rejects_bad_magic(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_core_fragment(blob, 0, 0xDEADBEEFu, HAILO_FW_HEADER_VERSION_V0,
                             4u, 4u);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

static void test_decode_core_fw_rejects_unsupported_header_version(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                             HAILO_FW_HEADER_VERSION_V0 + 1u, 4u, 4u);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

static void test_decode_core_fw_rejects_zero_code_size(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                             HAILO_FW_HEADER_VERSION_V0, 0u, 0u);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

static void test_decode_core_fw_rejects_oversize_code(void)
{
    uint8_t blob[128] = { 0 };
    (void)emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                             HAILO_FW_HEADER_VERSION_V0,
                             HAILO_FW_MAX_CORE_CODE_SIZE + 4u, 4u);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

static void test_decode_core_fw_rejects_truncated_code(void)
{
    uint8_t blob[32] = { 0 };
    /* Declares 64 B of code but the blob only has room for 8. */
    (void)emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                             HAILO_FW_HEADER_VERSION_V0, 64u, 8u);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, sizeof(blob), 0, &core, &code));
}

/* Tight-fit aliasing tests: pass fw_size == exact required byte count.
 * These guard the `> fw_size` vs `>= fw_size` boundary on both decoders
 * — a future off-by-one that flips the inequality would make the
 * helpers either reject valid tight-packed blobs or accept blobs with
 * one byte missing. */
static void test_decode_cert_accepts_tight_fit(void)
{
    uint8_t blob[64] = { 0 };
    const uint32_t key_size = 8, content_size = 12;
    size_t frag_len = emit_cert_fragment(blob, 0, key_size, content_size);
    /* frag_len is exactly sizeof(cert_header) + key_size + content_size. */
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_decode_cert(blob, frag_len, 0, &cert, &key, &content, &cert_end));
    TEST_ASSERT_EQUAL_UINT64((uint64_t)frag_len, (uint64_t)cert_end);
}

static void test_decode_cert_rejects_one_byte_short(void)
{
    uint8_t blob[64] = { 0 };
    const uint32_t key_size = 8, content_size = 12;
    size_t frag_len = emit_cert_fragment(blob, 0, key_size, content_size);
    struct hailo_fw_cert_header cert;
    const uint8_t *key, *content;
    size_t cert_end;
    /* fw_size one byte shy — cert_end > fw_size must fire. */
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_cert(blob, frag_len - 1, 0,
                          &cert, &key, &content, &cert_end));
}

static void test_decode_core_fw_accepts_tight_fit(void)
{
    uint8_t blob[128] = { 0 };
    const uint32_t code_size = 16;
    size_t frag_len = emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                                         HAILO_FW_HEADER_VERSION_V0,
                                         code_size, code_size);
    /* frag_len is exactly sizeof(firmware_header) + code_size. */
    struct hailo_firmware_header core;
    const uint8_t *code = NULL;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_decode_core_fw(blob, frag_len, 0, &core, &code));
    TEST_ASSERT_EQUAL_UINT32(code_size, core.code_size);
    /* Verify out_core_code lands past the header, not at the header. */
    TEST_ASSERT_EQUAL_PTR(blob + sizeof(core), code);
}

static void test_decode_core_fw_rejects_one_byte_short(void)
{
    uint8_t blob[128] = { 0 };
    const uint32_t code_size = 16;
    size_t frag_len = emit_core_fragment(blob, 0, HAILO_FW_MAGIC_HAILO8,
                                         HAILO_FW_HEADER_VERSION_V0,
                                         code_size, code_size);
    struct hailo_firmware_header core;
    const uint8_t *code;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_decode_core_fw(blob, frag_len - 1, 0, &core, &code));
}

static void test_decode_core_fw_success_populates_outputs(void)
{
    uint8_t blob[128] = { 0 };
    const uint32_t pre_pad = 24;
    (void)emit_core_fragment(blob, pre_pad, HAILO_FW_MAGIC_HAILO8,
                             HAILO_FW_HEADER_VERSION_V0, 8u, 8u);

    struct hailo_firmware_header core;
    const uint8_t *code = NULL;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_decode_core_fw(blob, sizeof(blob), pre_pad, &core, &code));
    TEST_ASSERT_EQUAL_UINT32(HAILO_FW_MAGIC_HAILO8, core.magic);
    TEST_ASSERT_EQUAL_UINT32(HAILO_FW_HEADER_VERSION_V0, core.header_version);
    TEST_ASSERT_EQUAL_UINT32(8u, core.code_size);
    TEST_ASSERT_EQUAL_PTR(blob + pre_pad + sizeof(core), code);
    TEST_ASSERT_EQUAL_HEX8(0xC0, code[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC7, code[7]);
}

/* -------------------------------------------------------------------------- */
/* Control-channel tests (Phase 5.2 tier 1, #281)                              */
/* -------------------------------------------------------------------------- */

/* Boot the device to state=RUNNING and reset ATR[0] so the control-
 * channel offsets on BAR4 map cleanly onto the mock SRAM
 * (0 → SRAM[0], 0x640 → SRAM[0x640], 0x1684 → SRAM[0x1684]). */
static void control_setup_running(void)
{
    boot_setup_probed();
    static uint8_t blob[256];
    size_t n = build_fw_blob(blob, sizeof(blob), 4, 4, 4, 4, 23, 0);
    TEST_ASSERT_TRUE(n != 0);
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_boot(blob, n));
    TEST_ASSERT_EQUAL_INT((int)HAILO_STATE_RUNNING, (int)hailo_get_state());
    /* Post-boot ATR[0] may point anywhere from the firmware upload
     * path; clear it so the control-channel offsets map cleanly. */
    mock_atr0_target = 0;
}

static void test_control_identify_rejects_when_not_running(void)
{
    boot_setup_probed();   /* state=PROBED, not RUNNING */
    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
                          hailo_control_identify(&resp));
}

static void test_control_identify_rejects_null_out(void)
{
    control_setup_running();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, hailo_control_identify(NULL));
}

static void test_control_identify_happy_path(void)
{
    control_setup_running();

    /* Seed a canned IDENTIFY response: [response_header][identify_body].
     * We fill the fw_version fields and leave the rest zero. */
    struct {
        struct hailo_control_response_header   header;
        uint32_t                               parameter_count;
        struct hailo_control_identify_response body;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    /* Common-header scalars and status fields cross the wire in
     * big-endian — the mock must seed them the same way firmware
     * would. fw_version is memcpy'd raw, so it stays native LE.
     * parameter_count sits between header and body in the wire
     * layout (CONTROL_PROTOCOL__payload_t in HailoRT). */
    fake.header.common.version  = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.sequence = 0;  /* bswap of zero is zero */
    fake.header.common.opcode   = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    fake.header.status.major_status = 0;
    fake.header.status.minor_status = 0;
    fake.parameter_count          = 0;
    fake.body.fw_version.major    = 4;
    fake.body.fw_version.minor    = 23;
    fake.body.fw_version.revision = 0x20000000;

    TEST_ASSERT_TRUE(sizeof(fake) <= MOCK_CONTROL_RESP_MAX);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_control_identify(&resp));

    TEST_ASSERT_EQUAL_UINT32(4,          resp.fw_version.major);
    TEST_ASSERT_EQUAL_UINT32(23,         resp.fw_version.minor);
    TEST_ASSERT_EQUAL_UINT32(0x20000000, resp.fw_version.revision);

    /* The mock captured the request; sanity-check its opcode field. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
    struct hailo_control_common_header captured_hdr;
    TEST_ASSERT_TRUE(mock_last_control_request_len
                     >= sizeof(captured_hdr));
    memcpy(&captured_hdr, mock_last_control_request, sizeof(captured_hdr));
    /* Wire values are big-endian — compare against the swapped form. */
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY),
                             captured_hdr.opcode);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION),
                             captured_hdr.version);
}

static void test_control_identify_timeout_no_response(void)
{
    control_setup_running();

    /* Leave the control simulator disabled — doorbell fires but
     * the mock never writes a response and never sets ISTATUS.
     * hailo_control_send_recv should time out.
     *
     * Use a very small response body to pass the preamble and
     * land in the poll loop quickly; leaving mock_fw_sim_control_
     * enabled == false means mock_simulate_fw_control_response
     * returns early without writing anything. */
    mock_fw_sim_control_enabled = false;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
                          hailo_control_identify(&resp));
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
}

static void test_control_send_recv_cpu_core_rings_core_doorbell(void)
{
    /* Context-switch opcodes target CPU_ID_CORE_CPU, which rings
     * bit 1 of raise_ready_offset instead of bit 0. The transport
     * plumbing (hailo_control_send_recv_cpu) must select the right
     * doorbell mask based on the cpu_id argument. Use the mock's
     * canned response path (same as IDENTIFY test above) to drive
     * a round trip via the CORE path and confirm:
     *   - mock_control_core_doorbells increments
     *   - mock_control_doorbells does NOT (APP path untouched)
     *   - last doorbell value equals CORE mask (1<<1) */
    control_setup_running();

    struct {
        struct hailo_control_response_header   header;
        uint32_t                               parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version  = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode   = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    fake.parameter_count        = 0;

    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    /* Build a minimal well-formed request body — common header +
     * parameter_count (zero params). The transport doesn't care about
     * opcode value for routing; only length validity and doorbell
     * routing are under test. */
    struct {
        struct hailo_control_common_header header;
        uint32_t                           parameter_count;
    } __attribute__((packed)) req;
    memset(&req, 0, sizeof(req));
    req.header.version  = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    req.header.opcode   = __builtin_bswap32(0x20);  /* SET_NETWORK_GROUP_HEADER */
    req.parameter_count = 0;

    uint8_t  resp[64];
    uint32_t resp_len = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_send_recv_cpu(HAILO_CTRL_CPU_CORE,
                                    &req, sizeof(req),
                                    resp, sizeof(resp), &resp_len,
                                    /*timeout_us=*/1000));

    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_core_doorbells);
    TEST_ASSERT_EQUAL_UINT32(HAILO_FW_ACCESS_CORE_CPU_CONTROL_MASK,
                             mock_control_last_doorbell_val);
}

static void test_set_network_group_header_rings_core_doorbell_and_wire(void)
{
    /* SET_NETWORK_GROUP_HEADER (opcode 0x20) targets CPU_ID_CORE_CPU.
     * Seed a minimal success response, pack a known header, and
     * verify:
     *   - request opcode on the wire is 0x20 (BE)
     *   - parameter_count is 1 (BE)
     *   - application_header_length is 53 (BE)
     *   - CORE doorbell fired, APP did not
     *   - the application_header bytes match our packed struct
     *     byte-for-byte (native LE, 53 bytes). */
    control_setup_running();

    struct {
        struct hailo_control_response_header header;
        uint32_t                             parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  =
        __builtin_bswap32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER);
    fake.parameter_count       = 0;
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_cs_application_header h;
    memset(&h, 0, sizeof(h));
    h.dynamic_contexts_count    = 3;
    h.preliminary_run_asap      = true;
    h.networks_count            = 1;
    h.csm_buffer_size           = 0x1234;
    h.batch_size                = 2;
    h.external_action_list_address = HAILO_CS_NO_DDR_ACTION_LIST;
    h.boundary_channels_bitmap[0] = 0x00000005;   /* engine 0, channels 0+2 */
    h.config_channels_count     = 1;
    h.config_channel_packed_id[0] = 0x11;

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_set_network_group_header(&h));

    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_core_doorbells);

    /* Inspect the captured request wire bytes:
     * [common header 16][parameter_count 4][application_header_length 4]
     * [application_header 32]. */
    TEST_ASSERT_TRUE(mock_last_control_request_len >= 16 + 4 + 4 + 32);
    const uint8_t *req = mock_last_control_request;
    uint32_t opcode, param_count, app_len;
    memcpy(&opcode,      req + 12, 4);  /* offset of `opcode` in common header */
    memcpy(&param_count, req + 16, 4);
    memcpy(&app_len,     req + 20, 4);
    TEST_ASSERT_EQUAL_UINT32(
        __builtin_bswap32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_NETWORK_GROUP_HEADER),
        opcode);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(1u),  param_count);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(32u), app_len);

    /* application_header bytes, native LE. Start at offset 24. */
    const uint8_t *ah = req + 24;
    uint16_t dyn_count;
    memcpy(&dyn_count, ah + 0, 2);
    TEST_ASSERT_EQUAL_UINT16(3, dyn_count);
    TEST_ASSERT_EQUAL_UINT8(1, ah[2]);   /* preliminary_run_asap */
    TEST_ASSERT_EQUAL_UINT8(0, ah[3]);   /* batch_register_config */
    TEST_ASSERT_EQUAL_UINT8(0, ah[4]);   /* can_fast_batch_switch */
    TEST_ASSERT_EQUAL_UINT8(0, ah[5]);   /* is_abbale_supported */
    TEST_ASSERT_EQUAL_UINT8(1, ah[6]);   /* networks_count */
    uint16_t csm;
    memcpy(&csm, ah + 7, 2);
    TEST_ASSERT_EQUAL_UINT16(0x1234, csm);
    uint16_t bs;
    memcpy(&bs, ah + 9, 2);
    TEST_ASSERT_EQUAL_UINT16(2, bs);
    uint32_t ext_addr;
    memcpy(&ext_addr, ah + 11, 4);
    TEST_ASSERT_EQUAL_UINT32(HAILO_CS_NO_DDR_ACTION_LIST, ext_addr);
    uint32_t bitmap0;
    memcpy(&bitmap0, ah + 15, 4);
    TEST_ASSERT_EQUAL_UINT32(0x00000005, bitmap0);
    TEST_ASSERT_EQUAL_UINT8(1,    ah[27]);   /* config_channels_count */
    TEST_ASSERT_EQUAL_UINT8(0x11, ah[28]);   /* config_channel_packed_id[0] */
}

static void test_set_network_group_header_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_set_network_group_header(NULL));
}

static void test_set_network_group_header_rejects_bad_config_count(void)
{
    struct hailo_cs_application_header h;
    memset(&h, 0, sizeof(h));
    h.config_channels_count = HAILO_CS_MAX_CFG_CHANNELS + 1;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_set_network_group_header(&h));
}

/* Seed the mock firmware with a minimal SET_CONTEXT_INFO success
 * response so a chunk call completes. Inlined helper because the
 * 4 chunking tests below all need it. */
static void seed_set_context_info_success_response(void)
{
    struct {
        struct hailo_control_response_header header;
        uint32_t                             parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  =
        __builtin_bswap32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO);
    fake.parameter_count       = 0;
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;
}

static void test_set_context_info_chunk_single_wire_layout(void)
{
    /* Single-chunk context (both is_first and is_last true). Verify:
     *   - opcode 0x21 (BE) on the wire
     *   - parameter_count = 4 (BE)
     *   - each length field = 1 except data_length which carries the
     *     actual payload size (BE)
     *   - is_first=1, is_last=1, context_type=PRELIMINARY=0
     *   - payload bytes land at offset 39 and match input byte-wise
     *   - CORE doorbell fired */
    control_setup_running();
    seed_set_context_info_success_response();

    uint8_t payload[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_set_context_info_chunk(
            HAILO_CS_CONTEXT_TYPE_PRELIMINARY,
            /*is_first=*/true, /*is_last=*/true,
            payload, sizeof(payload)));

    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_core_doorbells);

    TEST_ASSERT_TRUE(mock_last_control_request_len >= 39 + sizeof(payload));
    const uint8_t *req = mock_last_control_request;
    uint32_t opcode, param_count;
    memcpy(&opcode,      req + 12, 4);
    memcpy(&param_count, req + 16, 4);
    TEST_ASSERT_EQUAL_UINT32(
        __builtin_bswap32(HAILO_CONTROL_OPCODE_CONTEXT_SWITCH_SET_CONTEXT_INFO),
        opcode);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(4u), param_count);

    /* Per-field length + payload assertions. Offsets from wire layout:
     * 20: is_first_length (4 BE), 24: is_first (u8)
     * 25: is_last_length (4 BE), 29: is_last (u8)
     * 30: ctx_type_length (4 BE), 34: ctx_type (u8)
     * 35: data_length (4 BE), 39: data... */
    uint32_t len;
    memcpy(&len, req + 20, 4); TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(1u), len);
    TEST_ASSERT_EQUAL_UINT8(1, req[24]);
    memcpy(&len, req + 25, 4); TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(1u), len);
    TEST_ASSERT_EQUAL_UINT8(1, req[29]);
    memcpy(&len, req + 30, 4); TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(1u), len);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_CONTEXT_TYPE_PRELIMINARY, req[34]);
    memcpy(&len, req + 35, 4);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32((uint32_t)sizeof(payload)), len);
    TEST_ASSERT_EQUAL_MEMORY(payload, req + 39, sizeof(payload));
}

static void test_set_context_info_chunks_oversize_payload(void)
{
    /* A 3000-byte payload must chunk: 1461 + 1461 + 78 = 3 calls.
     * Each firing the CORE doorbell, with flags is_first=true only
     * on the first call and is_last=true only on the last.
     *
     * Verifying per-call flags requires capturing each request
     * individually; we approximate by:
     *   - checking final doorbell count == 3
     *   - checking the LAST captured request has is_last=1 and size
     *     equal to the residual 78 bytes */
    control_setup_running();
    seed_set_context_info_success_response();

    uint8_t payload[3000];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7u);

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_set_context_info(
            HAILO_CS_CONTEXT_TYPE_DYNAMIC,
            payload, (uint32_t)sizeof(payload)));

    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
    TEST_ASSERT_EQUAL_UINT32(3, mock_control_core_doorbells);

    /* Last captured request: is_first must be 0, is_last must be 1,
     * data_length = 3000 - 1461*2 = 78. */
    const uint8_t *req = mock_last_control_request;
    TEST_ASSERT_EQUAL_UINT8(0, req[24]);   /* is_first_chunk */
    TEST_ASSERT_EQUAL_UINT8(1, req[29]);   /* is_last_chunk */
    uint32_t data_len;
    memcpy(&data_len, req + 35, 4);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(78u), data_len);
    /* The last chunk's payload is payload[1461*2 ..]. */
    TEST_ASSERT_EQUAL_MEMORY(&payload[1461 * 2], req + 39, 78);
}

static void test_set_context_info_zero_length_is_single_chunk(void)
{
    /* A context with no actions still sends exactly one call with
     * is_first=is_last=true and data_length=0. */
    control_setup_running();
    seed_set_context_info_success_response();

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_set_context_info(
            HAILO_CS_CONTEXT_TYPE_ACTIVATION, NULL, 0));

    TEST_ASSERT_EQUAL_UINT32(1, mock_control_core_doorbells);
    const uint8_t *req = mock_last_control_request;
    TEST_ASSERT_EQUAL_UINT8(1, req[24]);
    TEST_ASSERT_EQUAL_UINT8(1, req[29]);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_CONTEXT_TYPE_ACTIVATION, req[34]);
    uint32_t data_len;
    memcpy(&data_len, req + 35, 4);
    TEST_ASSERT_EQUAL_UINT32(0u, data_len);
}

static void test_set_context_info_chunk_rejects_oversize(void)
{
    /* A chunk larger than HAILO_CS_CONTEXT_CHUNK_MAX_BYTES must be
     * rejected at the API boundary — firmware has a hard cap and
     * would reject it anyway, but we catch the error before firing
     * the doorbell. */
    control_setup_running();

    uint8_t payload[HAILO_CS_CONTEXT_CHUNK_MAX_BYTES + 1];
    memset(payload, 0, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_set_context_info_chunk(
            HAILO_CS_CONTEXT_TYPE_PRELIMINARY,
            true, true, payload, (uint32_t)sizeof(payload)));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_core_doorbells);
}

static void test_set_context_info_chunk_rejects_null_with_nonzero_len(void)
{
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_set_context_info_chunk(
            HAILO_CS_CONTEXT_TYPE_DYNAMIC,
            true, true, NULL, 16));
}

/* -------------------------------------------------------------------------- */
/* Phase 6.4a+b: context-switch action-list builder                            */
/* -------------------------------------------------------------------------- */

static void test_cs_builder_append_emits_header_then_body(void)
{
    /* Single FETCH_CCW_BURSTS action: 5-byte common header + 3-byte
     * body = 8 bytes total. Verify layout byte-for-byte. */
    uint8_t buf[32];
    struct hailo_cs_builder b;
    hailo_cs_builder_init(&b, buf, sizeof(buf));

    struct hailo_cs_act_fetch_ccw_bursts body = {
        .ccw_bursts          = 0x1234,
        .config_stream_index = 0x7,
    };
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_FETCH_CCW_BURSTS,
                                &body, sizeof(body)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)8, hailo_cs_builder_size(&b));

    /* [0]=action_type (u8) = 27 (FETCH_CCW_BURSTS)
     * [1..4]=time_stamp (u32 native LE) = 0
     * [5..6]=ccw_bursts (u16 native LE) = 0x1234
     * [7]=config_stream_index (u8) = 7 */
    const uint8_t *d = hailo_cs_builder_data(&b);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_ACT_FETCH_CCW_BURSTS, d[0]);
    uint32_t ts;
    memcpy(&ts, d + 1, 4);
    TEST_ASSERT_EQUAL_UINT32(0, ts);
    uint16_t bursts;
    memcpy(&bursts, d + 5, 2);
    TEST_ASSERT_EQUAL_UINT16(0x1234, bursts);
    TEST_ASSERT_EQUAL_UINT8(7, d[7]);
}

static void test_cs_builder_appends_concatenate(void)
{
    /* Three actions back-to-back: ACTIVATE_CFG_CHANNEL (21 B body),
     * FETCH_CCW_BURSTS (3 B body), DEACTIVATE_CFG_CHANNEL (2 B body).
     * Total with 5-byte common headers: 26 + 8 + 7 = 41 bytes. */
    uint8_t buf[128];
    struct hailo_cs_builder b;
    hailo_cs_builder_init(&b, buf, sizeof(buf));

    struct hailo_cs_act_activate_cfg_channel a1 = {
        .packed_vdma_channel_id = 0x21,
        .config_stream_index    = 0,
        .host_buffer_info = {
            .buffer_type       = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
            .dma_address       = 0x123456789ABCDEF0ull,
            .desc_page_size    = 512,
            .total_desc_count  = 16,
            .bytes_in_pattern  = 0,
        },
    };
    struct hailo_cs_act_fetch_ccw_bursts a2 = { .ccw_bursts = 4, .config_stream_index = 0 };
    struct hailo_cs_act_deactivate_cfg_channel a3 = { .packed_vdma_channel_id = 0x21, .config_stream_index = 0 };

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL, &a1, sizeof(a1)));
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_FETCH_CCW_BURSTS, &a2, sizeof(a2)));
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_DEACTIVATE_CFG_CHANNEL, &a3, sizeof(a3)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)26 + 8 + 7, hailo_cs_builder_size(&b));

    const uint8_t *d = hailo_cs_builder_data(&b);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL,   d[0]);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_ACT_FETCH_CCW_BURSTS,       d[26]);
    TEST_ASSERT_EQUAL_UINT8(HAILO_CS_ACT_DEACTIVATE_CFG_CHANNEL, d[26 + 8]);

    /* host_buffer_info.dma_address starts at offset 5 (common hdr) +
     * 2 (packed_vdma_channel_id + config_stream_index) + 1 (buffer_type)
     * = offset 8. It's a u64 native LE. */
    uint64_t dma_addr;
    memcpy(&dma_addr, d + 8, 8);
    TEST_ASSERT_EQUAL_UINT64(0x123456789ABCDEF0ull, dma_addr);
}

static void test_cs_builder_returns_nomem_on_overflow(void)
{
    uint8_t small[6];   /* Too small for one 5+3=8 byte action. */
    struct hailo_cs_builder b;
    hailo_cs_builder_init(&b, small, sizeof(small));
    struct hailo_cs_act_fetch_ccw_bursts body = { .ccw_bursts = 1, .config_stream_index = 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NOMEM,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_FETCH_CCW_BURSTS, &body, sizeof(body)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)0, hailo_cs_builder_size(&b));
}

static void test_cs_builder_rejects_null_buffer(void)
{
    struct hailo_cs_builder b;
    hailo_cs_builder_init(&b, NULL, 0);
    struct hailo_cs_act_fetch_ccw_bursts body = { .ccw_bursts = 1, .config_stream_index = 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_cs_builder_append(&b, HAILO_CS_ACT_FETCH_CCW_BURSTS, &body, sizeof(body)));
}

static void test_control_send_recv_default_rings_app_doorbell(void)
{
    /* The APP-default path (plain hailo_control_send_recv via
     * hailo_control_identify) rings bit 0. Regression-guards the
     * existing tier-1 opcode callers after the cpu_id plumbing
     * refactor. */
    control_setup_running();

    struct {
        struct hailo_control_response_header   header;
        uint32_t                               parameter_count;
        struct hailo_control_identify_response body;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version  = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode   = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    fake.parameter_count        = 0;
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_control_identify(&resp));
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_core_doorbells);
    TEST_ASSERT_EQUAL_UINT32(HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK,
                             mock_control_last_doorbell_val);
}

/*
 * Exercises the request wire format in full: every field in the
 * common header plus parameter_count. HailoRT's firmware expects
 * big-endian for every scalar; a single missed byteswap breaks
 * the entire RPC. The response path is already covered by
 * test_control_identify_happy_path; this test guards the send
 * side of the same contract.
 */
static void test_control_identify_request_wire_format_is_be(void)
{
    control_setup_running();
    /* Seed a minimal valid response so the send path completes and
     * we can inspect the captured request. */
    struct {
        struct hailo_control_response_header   header;
        uint32_t                               parameter_count;
        struct hailo_control_identify_response body;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_control_identify(&resp));

    /* Every scalar in the request payload must be the bswap32'd
     * form of its native value, including parameter_count=0 (which
     * happens to bswap to 0 — still worth asserting as a guard
     * against someone sneaking a native-endian write into the
     * send path). */
    struct {
        struct hailo_control_common_header common;
        uint32_t                           parameter_count;
    } __attribute__((packed)) captured;
    TEST_ASSERT_TRUE(mock_last_control_request_len >= sizeof(captured));
    memcpy(&captured, mock_last_control_request, sizeof(captured));

    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION),
                             captured.common.version);
    TEST_ASSERT_EQUAL_UINT32(0u, captured.common.flags);
    /* First send since reset → sequence == 0 on the wire. */
    TEST_ASSERT_EQUAL_UINT32(0u, captured.common.sequence);
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY),
                             captured.common.opcode);
    TEST_ASSERT_EQUAL_UINT32(0u, captured.parameter_count);
}

/*
 * Hailo firmware won't latch bits into BCS_ISTATUS_HOST until the
 * matching BSC_IMASK_HOST bits are unmasked — that's what the
 * Linux driver's hailo_pcie_enable_interrupts does. The transport
 * must do the same, once, and clear ISTATUS of any stale bits
 * before the first send. Subsequent sends must be idempotent.
 */
static void test_control_identify_arms_imask_once(void)
{
    control_setup_running();

    /* Canned response so each send_recv completes. */
    struct {
        struct hailo_control_response_header   header;
        uint32_t                               parameter_count;
        struct hailo_control_identify_response body;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_control_identify(&resp));
    TEST_ASSERT_EQUAL_UINT32(1, mock_imask_writes);
    TEST_ASSERT_EQUAL_UINT32(1, mock_istatus_clears_all);
    /* Enablement must include the whole ISTATUS mask so every
     * source we care about gets unmasked. */
    TEST_ASSERT_TRUE(
        (mock_imask_last_value & HAILO_BSC_ISTATUS_HOST_MASK)
        == HAILO_BSC_ISTATUS_HOST_MASK);

    /* Second send with the same process state: IMASK was already
     * armed, so no further writes. */
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_control_identify(&resp));
    TEST_ASSERT_EQUAL_UINT32(1, mock_imask_writes);
    TEST_ASSERT_EQUAL_UINT32(1, mock_istatus_clears_all);
}

/*
 * BCS_ISTATUS_HOST is a shared-line interrupt register: SW_IRQ,
 * VDMA_SRC, and VDMA_DEST bits can fire independently. The
 * transport must wait for the specific FW_CONTROL bit, not any
 * non-zero value, or an unrelated notification will be mistaken
 * for a response and the real response is missed.
 */
static void test_control_identify_ignores_non_fw_control_irq(void)
{
    control_setup_running();

    /* Preload the NOTIFICATION_IRQ bit (0x02 << 24) into ISTATUS
     * so the first poll read sees a non-zero value that is NOT
     * the FW_CONTROL bit. If the transport were still matching on
     * "any non-zero", it would consume this as the completion and
     * read a zeroed response area. */
    mock_istatus_one_shot_preload = (0x02u << HAILO_BCS_ISTATUS_HOST_SW_IRQ_SHIFT);

    /* Leave the control simulator disabled — the real FW_CONTROL
     * bit never fires, so the transport must time out even after
     * observing the notification. */
    mock_fw_sim_control_enabled = false;

    struct hailo_control_identify_response resp;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
                          hailo_control_identify(&resp));
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
    /* The stale bit must have been cleared (write-1-to-clear)
     * so it doesn't wedge the next request. */
    TEST_ASSERT_EQUAL_UINT32(0u, mock_istatus_one_shot_preload);
}

/* -------------------------------------------------------------------------- */
/* WRITE_MEMORY / READ_MEMORY (Phase 5.3, #281 tier-2)                         */
/* -------------------------------------------------------------------------- */

static void test_control_write_memory_rejects_null(void)
{
    control_setup_running();
    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_write_memory(0x100, NULL, 4));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_write_memory(0x100, buf, 0));
}

static void test_control_read_memory_rejects_null(void)
{
    control_setup_running();
    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_read_memory(0x100, NULL, 4));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
                          hailo_control_read_memory(0x100, buf, 0));
}

static void test_control_write_memory_rejects_oversize(void)
{
    /* data_length above HAILO_CONTROL_MAX_MEMORY_TRANSFER must be
     * rejected before any doorbell fires — the cap exists so a
     * UINT32_MAX-ish accidental call can't sit in the chunk loop
     * for minutes burning CPU 0. */
    control_setup_running();
    static uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_write_memory(0x100, buf,
                                   HAILO_CONTROL_MAX_MEMORY_TRANSFER + 1));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_control_read_memory_rejects_oversize(void)
{
    control_setup_running();
    static uint8_t buf[4];
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_read_memory(0x100, buf,
                                  HAILO_CONTROL_MAX_MEMORY_TRANSFER + 1));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_control_write_memory_rejects_address_wrap(void)
{
    /* address + data_length must not wrap past UINT32_MAX. The
     * classic pattern — address near the top of the 32-bit space
     * with a long length — would silently advance past zero into
     * low device addresses partway through the chunk loop. */
    control_setup_running();
    static uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_write_memory(0xFFFFFFFE, buf, 4));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_control_read_memory_rejects_address_wrap(void)
{
    control_setup_running();
    static uint8_t buf[4];
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_read_memory(0xFFFFFFFE, buf, 4));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_control_write_memory_sends_correct_wire(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    const uint8_t pattern[16] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0xCA, 0xFE, 0xBA, 0xBE,
    };
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_write_memory(0x123, pattern, sizeof(pattern)));

    /* One doorbell for one chunk (16 B << 1024 B chunk limit). */
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);

    /* Verify the captured request body: opcode=WRITE_MEMORY (BE),
     * parameter_count=2 (BE), address_length=4 (BE), address (BE),
     * data_length=16 (BE), then the raw pattern. */
    struct hailo_control_common_header hdr;
    memcpy(&hdr, mock_last_control_request, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_OPCODE_WRITE_MEMORY),
                             hdr.opcode);

    uint32_t pcount_be, addr_len_be, addr_be, data_len_be;
    memcpy(&pcount_be,    mock_last_control_request + 16, 4);
    memcpy(&addr_len_be,  mock_last_control_request + 20, 4);
    memcpy(&addr_be,      mock_last_control_request + 24, 4);
    memcpy(&data_len_be,  mock_last_control_request + 28, 4);
    TEST_ASSERT_EQUAL_UINT32(2u, __builtin_bswap32(pcount_be));
    TEST_ASSERT_EQUAL_UINT32(4u, __builtin_bswap32(addr_len_be));
    TEST_ASSERT_EQUAL_UINT32(0x123u, __builtin_bswap32(addr_be));
    TEST_ASSERT_EQUAL_UINT32(16u, __builtin_bswap32(data_len_be));
    TEST_ASSERT_EQUAL_MEMORY(pattern,
                                  mock_last_control_request + 32,
                                  sizeof(pattern));
}

static void test_control_read_memory_returns_device_bytes(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    /* Seed the mock backing store directly; no WRITE traffic yet. */
    uint8_t seed[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    memcpy(&mock_fw_sim_device_memory[0x200 % MOCK_MEMORY_SIZE],
           seed, sizeof(seed));

    uint8_t out[8];
    memset(out, 0xA5, sizeof(out));  /* poison */
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x200, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(seed, out, sizeof(seed));

    /* Verify request wire format. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
    struct hailo_control_common_header hdr;
    memcpy(&hdr, mock_last_control_request, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_OPCODE_READ_MEMORY),
                             hdr.opcode);
    uint32_t addr_be, count_be;
    memcpy(&addr_be,  mock_last_control_request + 24, 4);
    memcpy(&count_be, mock_last_control_request + 32, 4);
    TEST_ASSERT_EQUAL_UINT32(0x200u, __builtin_bswap32(addr_be));
    TEST_ASSERT_EQUAL_UINT32(8u,      __builtin_bswap32(count_be));
}

static void test_control_memory_round_trip(void)
{
    /* End-to-end: WRITE a pattern, READ it back, expect bytes match.
     * Exercises both opcodes AND the mock's backing store — a
     * regression here surfaces bugs in packer, unpacker, or
     * address routing. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t pattern[32];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(0x40 + i);
    }
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_write_memory(0x400, pattern, sizeof(pattern)));

    uint8_t readback[32];
    memset(readback, 0, sizeof(readback));
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x400, readback, sizeof(readback)));

    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sizeof(pattern));
    TEST_ASSERT_EQUAL_UINT32(2, mock_control_doorbells);  /* 1 WRITE + 1 READ */
}

static void test_control_memory_chunks_large_transfer(void)
{
    /* Transfers >1024 B must split into multiple chunks. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t pattern[2500];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 7 + 3);
    }
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_write_memory(0x800, pattern, sizeof(pattern)));
    /* 2500 / 1024 = 2 full chunks + 452-byte remainder → 3 doorbells. */
    TEST_ASSERT_EQUAL_UINT32(3, mock_control_doorbells);

    uint8_t readback[2500];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x800, readback, sizeof(readback)));
    /* 3 WRITE + 3 READ = 6 total doorbells. */
    TEST_ASSERT_EQUAL_UINT32(6, mock_control_doorbells);
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sizeof(pattern));
}

static void test_control_write_memory_rejects_when_not_running(void)
{
    boot_setup_probed();  /* state=PROBED, not RUNNING */
    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
                          hailo_control_write_memory(0x100, buf, 4));
}

static void test_control_read_memory_rejects_when_not_running(void)
{
    boot_setup_probed();
    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
                          hailo_control_read_memory(0x100, buf, 4));
}

/*
 * Firmware-side error propagation: when major_status != 0, the
 * driver must surface HAILO_ERR_IO rather than silently treating
 * the response as success. This was the actual failure mode
 * observed on pi-5-1: major_status=0x40000058 (no active stream
 * context) for arbitrary-address WRITE/READ; we need to be sure
 * we don't swallow that.
 */
static void test_control_write_memory_propagates_fw_error(void)
{
    control_setup_running();

    /* Hand-build a response with non-zero major_status. The mock's
     * smart-memory mode would always return success, so use the
     * canned-response path here. */
    struct {
        struct hailo_control_response_header header;
        uint32_t parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_WRITE_MEMORY);
    fake.header.status.major_status = __builtin_bswap32(0x40000058u);
    fake.header.status.minor_status = __builtin_bswap32(0x40000058u);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    uint8_t buf[4] = { 0x12, 0x34, 0x56, 0x78 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO,
        hailo_control_write_memory(0x200, buf, sizeof(buf)));
}

static void test_control_read_memory_propagates_fw_error(void)
{
    control_setup_running();

    struct {
        struct hailo_control_response_header header;
        uint32_t parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_READ_MEMORY);
    fake.header.status.major_status = __builtin_bswap32(0x40000058u);
    fake.header.status.minor_status = __builtin_bswap32(0x40000058u);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    uint8_t buf[4];
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO,
        hailo_control_read_memory(0x200, buf, sizeof(buf)));
}

/*
 * If firmware echoes back the wrong opcode in the response — e.g.
 * because a prior request is getting picked up — the driver must
 * flag it rather than returning success on a mis-parsed response.
 */
static void test_control_write_memory_rejects_wrong_opcode_echo(void)
{
    control_setup_running();

    struct {
        struct hailo_control_response_header header;
        uint32_t parameter_count;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    /* Echo IDENTIFY opcode (0x00) instead of WRITE_MEMORY (0x01). */
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_IDENTIFY);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    uint8_t buf[4] = { 0 };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_BAD_FIRMWARE,
        hailo_control_write_memory(0x200, buf, sizeof(buf)));
}

/*
 * Exact chunk-boundary: 1024 B should fit in exactly one doorbell,
 * not split into two. Guards against an off-by-one in the
 * remaining-vs-chunk comparison.
 */
static void test_control_write_memory_single_chunk_boundary(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t pattern[HAILO_CONTROL_MAX_MEMORY_CHUNK];
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i);
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_write_memory(0x300, pattern, sizeof(pattern)));
    /* Exactly one doorbell — not two. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);

    uint8_t readback[HAILO_CONTROL_MAX_MEMORY_CHUNK];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x300, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_UINT32(2, mock_control_doorbells);
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sizeof(pattern));
}

/*
 * Partial last-chunk: 1025 B should split into 1024 + 1, with the
 * second chunk carrying the trailing byte at the right address
 * offset. Guards against address arithmetic errors across chunks.
 */
static void test_control_write_memory_partial_last_chunk(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t pattern[HAILO_CONTROL_MAX_MEMORY_CHUNK + 1];
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i ^ 0x5A);
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_write_memory(0x500, pattern, sizeof(pattern)));
    TEST_ASSERT_EQUAL_UINT32(2, mock_control_doorbells);

    /* The LAST captured request is the trailing 1-byte chunk. Its
     * address must be base + HAILO_CONTROL_MAX_MEMORY_CHUNK, and
     * its data_length must be 1. */
    uint32_t addr_be, data_len_be;
    memcpy(&addr_be,     mock_last_control_request + 24, 4);
    memcpy(&data_len_be, mock_last_control_request + 28, 4);
    TEST_ASSERT_EQUAL_UINT32(0x500u + HAILO_CONTROL_MAX_MEMORY_CHUNK,
                             __builtin_bswap32(addr_be));
    TEST_ASSERT_EQUAL_UINT32(1u, __builtin_bswap32(data_len_be));
    TEST_ASSERT_EQUAL_UINT8(pattern[HAILO_CONTROL_MAX_MEMORY_CHUNK],
                            mock_last_control_request[32]);

    /* Round-trip confirmation. */
    uint8_t readback[HAILO_CONTROL_MAX_MEMORY_CHUNK + 1];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x500, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_MEMORY(pattern, readback, sizeof(pattern));
}

/*
 * Read response-format assertions the happy-path test didn't quite
 * cover: verify parameter_count, address_length, data_count_length,
 * and the BE encoding of data_count.
 */
static void test_control_read_memory_sends_correct_wire(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t buf[16];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x600, buf, sizeof(buf)));

    uint32_t pcount_be, addr_len_be, addr_be, count_len_be, count_be;
    memcpy(&pcount_be,    mock_last_control_request + 16, 4);
    memcpy(&addr_len_be,  mock_last_control_request + 20, 4);
    memcpy(&addr_be,      mock_last_control_request + 24, 4);
    memcpy(&count_len_be, mock_last_control_request + 28, 4);
    memcpy(&count_be,     mock_last_control_request + 32, 4);
    TEST_ASSERT_EQUAL_UINT32(2u,      __builtin_bswap32(pcount_be));
    TEST_ASSERT_EQUAL_UINT32(4u,      __builtin_bswap32(addr_len_be));
    TEST_ASSERT_EQUAL_UINT32(0x600u,  __builtin_bswap32(addr_be));
    TEST_ASSERT_EQUAL_UINT32(4u,      __builtin_bswap32(count_len_be));
    TEST_ASSERT_EQUAL_UINT32(16u,     __builtin_bswap32(count_be));
}

/* -------------------------------------------------------------------------- */
/* Phase 5.3: DMA tensor buffers                                               */
/* -------------------------------------------------------------------------- */

/* All tensor tests assume the mock platform is installed. Pull that
 * setup into a helper since tensor tests don't need the probed /
 * running state machine — just a live hailo_platform pointer. */
static void tensor_setup(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
}

static void test_tensor_size_from_shape_small(void)
{
    TEST_ASSERT_EQUAL_UINT32(224 * 224 * 3,
        hailo_tensor_size_from_shape(224, 224, 3, 1));
    TEST_ASSERT_EQUAL_UINT32(56 * 56 * 256 * 2,
        hailo_tensor_size_from_shape(56, 56, 256, 2));
}

static void test_tensor_size_from_shape_rejects_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, hailo_tensor_size_from_shape(0, 10, 10, 1));
    TEST_ASSERT_EQUAL_UINT32(0, hailo_tensor_size_from_shape(10, 0, 10, 1));
    TEST_ASSERT_EQUAL_UINT32(0, hailo_tensor_size_from_shape(10, 10, 0, 1));
    TEST_ASSERT_EQUAL_UINT32(0, hailo_tensor_size_from_shape(10, 10, 10, 0));
}

static void test_tensor_size_from_shape_rejects_overflow(void)
{
    /* 65536 * 65536 = 4 G > UINT32_MAX → reject at step 1. */
    TEST_ASSERT_EQUAL_UINT32(0,
        hailo_tensor_size_from_shape(65536, 65536, 1, 1));
    /* 1024 * 1024 * 4096 * 1 = 4 G → overflow at step 2. */
    TEST_ASSERT_EQUAL_UINT32(0,
        hailo_tensor_size_from_shape(1024, 1024, 4096, 1));
}

static void test_tensor_alloc_happy_path(void)
{
    tensor_setup();
    struct hailo_tensor t = {0};

    int rc = hailo_tensor_alloc(224 * 224 * 3, &t);
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
    TEST_ASSERT_NOT_NULL(t.cpu_addr);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)t.cpu_addr, t.iova);
    TEST_ASSERT_EQUAL_UINT32(224 * 224 * 3, t.tensor_bytes);
    /* alloc_size rounded up to 4 KB boundary. */
    TEST_ASSERT_EQUAL_UINT32(
        (224 * 224 * 3 + 4095) & ~4095u,
        t.alloc_size);
    TEST_ASSERT_EQUAL_UINT32(HAILO_TENSOR_DMA_ALIGN, t.align);
    /* Buffer is page-aligned. */
    TEST_ASSERT_EQUAL_UINT64(0,
        (uintptr_t)t.cpu_addr & (HAILO_TENSOR_DMA_ALIGN - 1));
    /* Buffer is zero-initialized. */
    for (uint32_t i = 0; i < t.tensor_bytes; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, ((uint8_t *)t.cpu_addr)[i]);
    }
    /* dma_alloc fired exactly once. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_dma_alloc_calls);

    hailo_tensor_free(&t);
    TEST_ASSERT_EQUAL_UINT32(1, mock_dma_free_calls);
    /* Handle is zeroed post-free. */
    TEST_ASSERT_NULL(t.cpu_addr);
    TEST_ASSERT_EQUAL_UINT32(0, t.tensor_bytes);
}

static void test_tensor_alloc_rejects_null_out(void)
{
    tensor_setup();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, hailo_tensor_alloc(1024, NULL));
}

static void test_tensor_alloc_rejects_zero_size(void)
{
    tensor_setup();
    struct hailo_tensor t;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, hailo_tensor_alloc(0, &t));
}

static void test_tensor_alloc_nodev_without_platform(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    hailo_platform = NULL;
    struct hailo_tensor t;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV, hailo_tensor_alloc(1024, &t));
    hailo_platform = saved;
}

static void test_tensor_alloc_propagates_nomem(void)
{
    tensor_setup();
    mock_dma_force_null = true;
    struct hailo_tensor t;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NOMEM, hailo_tensor_alloc(4096, &t));
    /* alloc_calls bumped but handle stays zeroed. */
    TEST_ASSERT_EQUAL_UINT32(1, mock_dma_alloc_calls);
    TEST_ASSERT_NULL(t.cpu_addr);
}

static void test_tensor_prepare_for_device_fires_cache_clean(void)
{
    tensor_setup();
    struct hailo_tensor t;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_tensor_alloc(8192, &t));

    hailo_tensor_prepare_for_device(&t);
    TEST_ASSERT_EQUAL_UINT32(1, mock_cache_clean_calls);
    /* Clean is scoped to the logical tensor bytes, not the rounded
     * alloc_size — device only reads what the caller wrote. */
    TEST_ASSERT_EQUAL_UINT64(8192, mock_last_cache_clean_size);

    hailo_tensor_free(&t);
}

static void test_tensor_prepare_for_host_fires_cache_invalidate(void)
{
    tensor_setup();
    struct hailo_tensor t;
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_tensor_alloc(8192, &t));

    hailo_tensor_prepare_for_host(&t);
    TEST_ASSERT_EQUAL_UINT32(1, mock_cache_invalidate_calls);
    TEST_ASSERT_EQUAL_UINT64(8192, mock_last_cache_invalidate_size);

    hailo_tensor_free(&t);
}

static void test_tensor_free_zero_handle_is_noop(void)
{
    tensor_setup();
    struct hailo_tensor t = {0};
    hailo_tensor_free(&t);   /* must not crash */
    TEST_ASSERT_EQUAL_UINT32(0, mock_dma_free_calls);
}

static void test_tensor_multi_alloc_distinct_buffers(void)
{
    tensor_setup();
    struct hailo_tensor a = {0}, b = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_tensor_alloc(4096, &a));
    TEST_ASSERT_EQUAL_INT(HAILO_OK, hailo_tensor_alloc(4096, &b));
    TEST_ASSERT_NOT_NULL(a.cpu_addr);
    TEST_ASSERT_NOT_NULL(b.cpu_addr);
    TEST_ASSERT_NOT_EQUAL(a.cpu_addr, b.cpu_addr);
    /* Bump-allocator grows monotonically. */
    TEST_ASSERT_TRUE((uintptr_t)b.cpu_addr > (uintptr_t)a.cpu_addr);

    hailo_tensor_free(&a);
    hailo_tensor_free(&b);
}

/* -------------------------------------------------------------------------- */
/* Phase 5.4: VDMA descriptor-list allocator                                   */
/* -------------------------------------------------------------------------- */

/* Descriptor-list allocation shares the tensor-path setup — just
 * needs the mock platform installed. */
static void vdma_setup(void)
{
    mock_reset();
    hailo_platform = &mock_ops;
}

static void test_vdma_alloc_size_rounds_up_to_64k(void)
{
    /* 64 descriptors × 16 B = 1024 B → round up to 64 KB. */
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_DESC_LIST_ALIGN,
        hailo_vdma_desc_list_alloc_size(64));
    /* 4096 descriptors × 16 B = 65536 B → exactly 64 KB (no bump). */
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_DESC_LIST_ALIGN,
        hailo_vdma_desc_list_alloc_size(4096));
    /* 8192 descriptors × 16 B = 128 KB. */
    TEST_ASSERT_EQUAL_UINT32(2u * HAILO_VDMA_DESC_LIST_ALIGN,
        hailo_vdma_desc_list_alloc_size(8192));
}

static void test_vdma_alloc_happy_path(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    int rc = hailo_vdma_desc_list_alloc(256, 4096, true, &list);
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
    TEST_ASSERT_NOT_NULL(list.descs);
    TEST_ASSERT_EQUAL_UINT32(256, list.desc_count);
    TEST_ASSERT_EQUAL_UINT32(255, list.desc_count_mask);
    TEST_ASSERT_EQUAL_UINT16(4096, list.desc_page_size);
    TEST_ASSERT_TRUE(list.is_circular);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(uintptr_t)list.descs, list.iova);
    /* 64 KB-aligned address — VDMA engine's HOST_DESC_BASE_ADDR
     * truncates low 16 bits; any misalignment is a silent firmware
     * bug. */
    TEST_ASSERT_EQUAL_UINT64(0,
        (uintptr_t)list.descs & (HAILO_VDMA_DESC_LIST_ALIGN - 1));
    /* Buffer zero-initialized — an all-zero descriptor is inert. */
    for (uint32_t i = 0; i < 256 * sizeof(struct hailo_vdma_descriptor); i++) {
        TEST_ASSERT_EQUAL_UINT8(0, ((uint8_t *)list.descs)[i]);
    }
    hailo_vdma_desc_list_free(&list);
    TEST_ASSERT_NULL(list.descs);
    TEST_ASSERT_EQUAL_UINT32(0, list.desc_count);
}

static void test_vdma_alloc_rejects_non_power_of_two(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(3, 512, false, &list));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(100, 512, false, &list));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(1023, 512, false, &list));
}

static void test_vdma_alloc_rejects_out_of_range(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    /* desc_count = 1 is below MIN_DESC_COUNT. */
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(1, 512, false, &list));
    /* desc_count = 131072 = 2^17 exceeds MAX (16-bit num_avail/num_proc). */
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(131072, 512, false, &list));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(0, 512, false, &list));
}

static void test_vdma_alloc_rejects_zero_page_size(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(64, 0, false, &list));
}

static void test_vdma_alloc_rejects_null_out(void)
{
    vdma_setup();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_desc_list_alloc(64, 512, false, NULL));
}

static void test_vdma_alloc_nodev_without_platform(void)
{
    const struct hailo_platform_ops *saved = hailo_platform;
    hailo_platform = NULL;
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));
    hailo_platform = saved;
}

static void test_vdma_alloc_propagates_nomem(void)
{
    vdma_setup();
    mock_dma_force_null = true;
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NOMEM,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));
    TEST_ASSERT_NULL(list.descs);
}

static void test_vdma_free_zero_handle_is_noop(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    hailo_vdma_desc_list_free(&list);   /* must not crash */
    TEST_ASSERT_EQUAL_UINT32(0, mock_dma_free_calls);
}

static void test_vdma_alloc_accepts_min_count(void)
{
    /* Smallest legal list: 2 descriptors. */
    vdma_setup();
    struct hailo_vdma_desc_list small;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(HAILO_VDMA_MIN_DESC_COUNT, 64, false, &small));
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_MIN_DESC_COUNT, small.desc_count);
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_MIN_DESC_COUNT - 1u,
                             small.desc_count_mask);
    hailo_vdma_desc_list_free(&small);
}

static void test_vdma_program_descriptor_encodes_fields(void)
{
    /* Unit-test the bit-layout without any allocation. Reference
     * layout (hailo-vdma-common.c:139-147):
     *   PageSize_DescControl     = (page_size << 8) | 0x02
     *   AddrL_rsvd_DataID        = (addr & 0xFFFFFFC0) | data_id
     *   AddrH                    = addr >> 32
     *   RemainingPageSize_Status = 0
     */
    struct hailo_vdma_descriptor d = {0};
    hailo_vdma_program_descriptor(&d,
        /*dma=*/0x0000000A12345680ULL,
        /*page=*/512,
        /*data_id=*/0x3C);

    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u, d.page_size_desc_control);
    /* 0x12345680 & 0xFFFFFFC0 = 0x12345680 (already 64-aligned) | 0x3C. */
    TEST_ASSERT_EQUAL_UINT32(0x12345680u | 0x3Cu, d.addr_l_rsvd_data_id);
    TEST_ASSERT_EQUAL_UINT32(0x0000000Au, d.addr_h);
    TEST_ASSERT_EQUAL_UINT32(0u, d.remaining_page_size_status);
}

static void test_vdma_program_descriptor_masks_low_addr_bits(void)
{
    /* Descriptor addresses must be 64-byte aligned; hardware
     * silently masks the low 6 bits. Verify our packer does the
     * same mask. */
    struct hailo_vdma_descriptor d = {0};
    hailo_vdma_program_descriptor(&d,
        /*dma=*/0x100000003Fu,   /* misaligned by 0x3F */
        /*page=*/256, /*data_id=*/0x00);
    /* Low 6 bits masked → 0x10_00000000 | 0x00 data_id = 0. */
    TEST_ASSERT_EQUAL_UINT32(0u, d.addr_l_rsvd_data_id);
    TEST_ASSERT_EQUAL_UINT32(0x10u, d.addr_h);
}

static void test_vdma_program_buffer_one_descriptor(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));

    /* Buffer smaller than page_size → one descriptor with the
     * residue as the last-descriptor page size. */
    int rc = hailo_vdma_program_buffer(&list, 0,
        /*iova=*/0x10000, /*size=*/200, /*data_id=*/0x05);
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT32((200u << 8) | 0x02u,
                             list.descs[0].page_size_desc_control);
    TEST_ASSERT_EQUAL_UINT32(0x10000u | 0x05u,
                             list.descs[0].addr_l_rsvd_data_id);
    /* Descriptors past the programmed one remain zeroed. */
    TEST_ASSERT_EQUAL_UINT32(0u, list.descs[1].page_size_desc_control);
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_program_buffer_exact_multiple(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));

    /* 1024 B / 512 page = exactly 2 descriptors, no residue.
     * Each descriptor's page_size field = full page size. */
    int rc = hailo_vdma_program_buffer(&list, 0,
        /*iova=*/0x40000, /*size=*/1024, /*data_id=*/0x01);
    TEST_ASSERT_EQUAL_INT(2, rc);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[0].page_size_desc_control);
    TEST_ASSERT_EQUAL_UINT32(0x40000u | 0x01u,
                             list.descs[0].addr_l_rsvd_data_id);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[1].page_size_desc_control);
    /* Second descriptor's address advances by page_size. */
    TEST_ASSERT_EQUAL_UINT32((0x40000u + 512u) | 0x01u,
                             list.descs[1].addr_l_rsvd_data_id);
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_program_buffer_with_residue(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));

    /* 1280 B / 512 page = 2 full + 256 residue = 3 descriptors.
     * Last descriptor's page size == residue size. */
    int rc = hailo_vdma_program_buffer(&list, 0,
        /*iova=*/0x80000, /*size=*/1280, /*data_id=*/0x02);
    TEST_ASSERT_EQUAL_INT(3, rc);
    /* descs[0] and descs[1] carry full page_size. */
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[0].page_size_desc_control);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[1].page_size_desc_control);
    /* descs[2] carries residue (256). */
    TEST_ASSERT_EQUAL_UINT32((256u << 8) | 0x02u,
                             list.descs[2].page_size_desc_control);
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_program_buffer_wraps_circular_list(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(4, 512, /*circular=*/true, &list));

    /* 4-descriptor circular list; 3 full-page descriptors starting
     * at index 2 → programs descs[2], descs[3], descs[0] (wrap). */
    int rc = hailo_vdma_program_buffer(&list, /*start=*/2,
        /*iova=*/0x2000, /*size=*/1536, /*data_id=*/0x04);
    TEST_ASSERT_EQUAL_INT(3, rc);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[2].page_size_desc_control);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[3].page_size_desc_control);
    TEST_ASSERT_EQUAL_UINT32((512u << 8) | 0x02u,
                             list.descs[0].page_size_desc_control);
    /* descs[1] untouched. */
    TEST_ASSERT_EQUAL_UINT32(0u, list.descs[1].page_size_desc_control);
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_program_buffer_rejects_overrun(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(4, 512, /*circular=*/false, &list));
    /* 4-desc non-circular list, start at 2 → only 2 descriptors
     * available. Ask for 3 → reject. */
    int rc = hailo_vdma_program_buffer(&list, /*start=*/2,
        /*iova=*/0x2000, /*size=*/1536, /*data_id=*/0);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_program_buffer_rejects_null_list(void)
{
    int rc = hailo_vdma_program_buffer(NULL, 0, 0x1000, 512, 0);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
}

static void test_vdma_program_buffer_rejects_zero_size(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(4, 512, false, &list));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_program_buffer(&list, 0, 0x1000, 0, 0));
    hailo_vdma_desc_list_free(&list);
}

/* -------------------------------------------------------------------------- */
/* VDMA channel start / stop / submit                                          */
/* -------------------------------------------------------------------------- */

static void test_vdma_channel_start_programs_regs(void)
{
    /* Start channel 3 with a 256-desc list at a deterministic iova.
     * desc_depth = ceil_log2(256) = 8. Reference `start_channel`
     * (hailo-vdma-common.c:859-894) writes, in order:
     *   1. ALIGNED_ADDR_L (offset 0x08 within channel block), RMW
     *      to put address_l = high 16 of iova-low-32 in bits [31:16].
     *   2. ADDR_H (offset 0x0C) = iova >> 32.
     *   3. BASE_DWORD (offset 0x00) = (depth<<11) | (data_id<<8) —
     *      also clears CONTROL bits [7:0].
     *   4. BASE_DWORD RMW to set CONTROL bits [7:0] = START (0x01),
     *      preserving DEPTH + DATA_ID.
     *
     * Pick iova = 0x0001_0000_0000 (low-32 = 0, high-32 = 1); so
     * address_l = 0 and address_h = 1. */
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(256, 512, false, &list));
    list.iova = 0x0000000100000000ULL;

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_channel_start(3, &list, 0x05));

    /* Channel 3 base = 3 * 32 = 0x60. */
    const uint32_t ch_base = 3u * 32u;

    /* BASE_DWORD: CONTROL=START in low byte, depth=8<<11 (=0x4000),
     *             data_id=5<<8 (=0x500). Combined low 16 bits:
     *               (0x4000 | 0x500 | 0x01) & 0xFFFF = 0x4501. */
    uint32_t base_dword;
    memcpy(&base_dword, &mock_bar2[ch_base], sizeof(base_dword));
    TEST_ASSERT_EQUAL_UINT32(0x01u, base_dword & 0xFFu);
    TEST_ASSERT_EQUAL_UINT32(0x5u << 8, base_dword & (0x7u << 8));
    TEST_ASSERT_EQUAL_UINT32(8u << 11, base_dword & (0xFu << 11));

    /* ALIGNED_ADDR_L at ch_base + 0x08: bits[31:16] = address_l (0). */
    uint32_t aligned;
    memcpy(&aligned, &mock_bar2[ch_base + 0x08], sizeof(aligned));
    TEST_ASSERT_EQUAL_UINT32(0u, aligned >> 16);

    /* ADDR_H at ch_base + 0x0C: full u32 = 1. */
    uint32_t addr_h_dword;
    memcpy(&addr_h_dword, &mock_bar2[ch_base + 0x0C], sizeof(addr_h_dword));
    TEST_ASSERT_EQUAL_UINT32(1u, addr_h_dword);

    hailo_vdma_desc_list_free(&list);
}

/* Second iova pattern: low-32 nonzero so address_l is nonzero. */
static void test_vdma_channel_start_encodes_address_l(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));
    /* iova = 0x0000000ABCD80000 — low 32 bits = 0xABCD0000, so
     * address_l = 0xABCD (high 16 of low 32) once we mask the
     * 64 KB-alignment (low 16 bits are zero). */
    list.iova = 0x0ABCD80000ULL & ~0xFFFFULL;  /* 64 KB-align */
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_channel_start(2, &list, 0x00));

    const uint32_t ch_base = 2u * 32u;
    uint32_t aligned;
    memcpy(&aligned, &mock_bar2[ch_base + 0x08], sizeof(aligned));
    uint16_t addr_l = (uint16_t)(aligned >> 16);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)((list.iova >> 16) & 0xFFFFu), addr_l);

    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_channel_start_rejects_misaligned_iova(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));
    /* Clobber iova to something NOT 64 KB-aligned. */
    list.iova = 0x1234;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_channel_start(0, &list, 0));
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_channel_start_rejects_bad_channel(void)
{
    vdma_setup();
    struct hailo_vdma_desc_list list = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(64, 512, false, &list));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_channel_start(HAILO_VDMA_MAX_CHANNELS, &list, 0));
    hailo_vdma_desc_list_free(&list);
}

static void test_vdma_channel_start_rejects_null(void)
{
    vdma_setup();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_channel_start(0, NULL, 0));
}

static void test_vdma_channel_stop_writes_abort_pause(void)
{
    /* Precondition: channel's CONTROL is 0 (idle after mock_reset).
     * hailo_vdma_channel_stop should NOT short-circuit — idle != abort-pause. */
    vdma_setup();
    hailo_vdma_channel_stop(2);

    /* Channel 2 base = 2 * 32 = 0x40. Bits [7:0] should now be
     * ABORT_PAUSE (0x02). */
    uint32_t dword;
    memcpy(&dword, &mock_bar2[0x40], sizeof(dword));
    TEST_ASSERT_EQUAL_UINT32(0x02u, dword & 0xFFu);
}

static void test_vdma_channel_stop_skips_if_already_abort_pause(void)
{
    vdma_setup();
    /* Pre-seed channel 5's control byte with ABORT_PAUSE. */
    uint32_t seed = 0x02u;
    memcpy(&mock_bar2[5 * 32], &seed, sizeof(seed));
    hailo_vdma_channel_stop(5);
    /* Should remain unchanged. */
    uint32_t dword;
    memcpy(&dword, &mock_bar2[5 * 32], sizeof(dword));
    TEST_ASSERT_EQUAL_UINT32(0x02u, dword);
}

static void test_vdma_submit_and_wait_completes_fast(void)
{
    /* Auto-advance on: any num_avail write immediately mirrors to
     * num_proc. hailo_vdma_submit_and_wait should find completion
     * on the first poll, well under the timeout. */
    vdma_setup();
    mock_vdma_auto_advance = true;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_submit_and_wait(4, /*num_avail=*/0x1234, /*timeout=*/1000));

    /* num_avail landed in the channel's base dword bits [31:16]. */
    uint32_t dword;
    memcpy(&dword, &mock_bar2[4 * 32], sizeof(dword));
    TEST_ASSERT_EQUAL_UINT32(0x1234u, dword >> 16);
    /* num_proc matches. */
    uint32_t proc;
    memcpy(&proc, &mock_bar2[4 * 32 + 4], sizeof(proc));
    TEST_ASSERT_EQUAL_UINT32(0x1234u, proc & 0xFFFFu);
}

static void test_vdma_submit_and_wait_times_out(void)
{
    /* Without auto-advance, num_proc never catches up → timeout. */
    vdma_setup();
    mock_vdma_auto_advance = false;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
        hailo_vdma_submit_and_wait(0, /*num_avail=*/1, /*timeout=*/200));
}

static void test_vdma_submit_rejects_bad_channel(void)
{
    vdma_setup();
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_vdma_submit_and_wait(HAILO_VDMA_MAX_CHANNELS, 1, 1000));
}

/* -------------------------------------------------------------------------- */
/* Phase 5.4: hailo_infer orchestration                                        */
/* -------------------------------------------------------------------------- */

/* Helper — bring hailo into RUNNING state so hailo_infer_run's
 * state-guard passes. Reuses control_setup_running from the control
 * tests above. */
static void infer_setup_running(void)
{
    control_setup_running();
    /* Ensure mock's auto-advance is on so the channel submits
     * inside hailo_infer_run complete immediately. */
    mock_vdma_auto_advance = true;
}

static void test_infer_rejects_null_args(void)
{
    infer_setup_running();
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    uint8_t buf[512] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(NULL, buf, buf, NULL));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, NULL, buf, NULL));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, buf, NULL, NULL));
}

static void test_infer_rejects_same_channels(void)
{
    infer_setup_running();
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 3, .output_channel = 3,   /* same = reject */
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    uint8_t buf[512] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, buf, buf, NULL));
}

static void test_infer_rejects_bad_channel(void)
{
    infer_setup_running();
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = HAILO_VDMA_MAX_CHANNELS,
        .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    uint8_t buf[512] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, buf, buf, NULL));
}

static void test_infer_rejects_zero_sizes(void)
{
    infer_setup_running();
    uint8_t buf[512] = {0};
    struct hailo_infer_config cfg = {
        .input_bytes = 0,   .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, buf, buf, NULL));
    cfg.input_bytes = 512;
    cfg.output_bytes = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_infer_run(&cfg, buf, buf, NULL));
}

static void test_infer_rejects_nodev_when_not_running(void)
{
    boot_setup_probed();   /* state=PROBED, not RUNNING */
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    uint8_t buf[512] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
        hailo_infer_run(&cfg, buf, buf, NULL));
}

static void test_infer_end_to_end_via_auto_advance(void)
{
    /* Full pipeline with the mock's auto-advance flag: input and
     * output submits complete immediately. Returns HAILO_OK with a
     * measured elapsed time (though very small under QEMU). */
    infer_setup_running();
    struct hailo_infer_config cfg = {
        .input_bytes = 1024, .output_bytes = 2048,
        .input_channel = 0, .output_channel = 1,
        .input_data_id = 0x01, .output_data_id = 0x02,
        .input_page_size = 512, .output_page_size = 1024,
        .timeout_us = 100000,
    };
    uint8_t in[1024];
    uint8_t out[2048];
    for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)i;
    memset(out, 0, sizeof(out));

    uint64_t elapsed = 0xDEADBEEF;
    int rc = hailo_infer_run(&cfg, in, out, &elapsed);
    TEST_ASSERT_EQUAL_INT(HAILO_OK, rc);
    /* elapsed is a non-negative duration; under QEMU/auto-advance
     * it can be 0 since both reads happen in the same tick. */
    TEST_ASSERT_NOT_EQUAL(0xDEADBEEF, elapsed);
}

static void test_infer_timeout_without_auto_advance(void)
{
    /* Without auto-advance the first submit-and-wait times out. */
    control_setup_running();
    mock_vdma_auto_advance = false;
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 500,   /* very short so the test finishes fast */
    };
    uint8_t in[512] = {0};
    uint8_t out[512] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
        hailo_infer_run(&cfg, in, out, NULL));
}

static void test_infer_propagates_tensor_alloc_failure(void)
{
    /* Force the platform allocator to fail — the very first
     * hailo_tensor_alloc inside hailo_infer_run should propagate
     * HAILO_ERR_NOMEM and no channels should get started. */
    infer_setup_running();
    mock_dma_force_null = true;
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 100000,
    };
    uint8_t buf[512] = {0};
    int rc = hailo_infer_run(&cfg, buf, buf, NULL);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NOMEM, rc);
    /* mock_dma_alloc counts TOTAL calls across alloc/free; at
     * least one fired (for the input tensor attempt). */
    TEST_ASSERT_TRUE(mock_dma_alloc_calls >= 1);
}

static void test_infer_rejects_oversized_output_desc_count(void)
{
    /* output_page_size=1 with 1 MB output forces 1 M descriptors
     * which rounds up to 2^20 > HAILO_VDMA_MAX_DESC_COUNT → the
     * pick_desc_count helper returns 0 → INVAL. */
    infer_setup_running();
    struct hailo_infer_config cfg = {
        .input_bytes = 512,
        .output_bytes = 1024u * 1024u,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 1,
        .timeout_us = 100000,
    };
    /* Use page-sized scratch bufs; hailo_infer_run validates
     * config before touching them. */
    static uint8_t in[512];
    static uint8_t out[1024 * 1024];
    int rc = hailo_infer_run(&cfg, in, out, NULL);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL, rc);
    TEST_ASSERT_EQUAL_UINT32(0, mock_dma_alloc_calls);
}

static void test_infer_cleanup_on_midflight_failure(void)
{
    /* Let the alloc succeed but force a channel-start failure by
     * clobbering list IOVA alignment post-alloc. Easiest: passing
     * matching but invalid page_size (0 rejected by validation) —
     * use a different lever: set both channels to the same index
     * through runtime state manipulation... actually
     * rejects_same_channels already covers pre-alloc rejection.
     *
     * For mid-flight: drive the pipeline far enough that an
     * allocation succeeds but the next step fails. Easiest path
     * with current mock hooks: tensor alloc first succeeds (force
     * null off), then the vdma desc-list alloc hits force-null
     * partway. Not supported by current mock — leave as a doc
     * note that the goto-out cleanup path is exercised by the
     * timeout test (which runs through *_alloc + *_start and
     * cleans up at *_submit_and_wait timeout). */
    infer_setup_running();
    mock_vdma_auto_advance = false;
    struct hailo_infer_config cfg = {
        .input_bytes = 512, .output_bytes = 512,
        .input_channel = 0, .output_channel = 1,
        .input_page_size = 512, .output_page_size = 512,
        .timeout_us = 500,
    };
    uint8_t in[512] = {0};
    uint8_t out[512] = {0};
    uint32_t allocs_before = mock_dma_alloc_calls;
    uint32_t frees_before  = mock_dma_free_calls;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
        hailo_infer_run(&cfg, in, out, NULL));
    /* Cleanup: every dma_alloc has a matching dma_free post-timeout. */
    uint32_t new_allocs = mock_dma_alloc_calls - allocs_before;
    uint32_t new_frees  = mock_dma_free_calls - frees_before;
    TEST_ASSERT_EQUAL_UINT32(new_allocs, new_frees);
}

static void test_vdma_alloc_accepts_max_count(void)
{
    /* Largest legal list: 65536 descriptors = 1 MB, which matches
     * the mock pool size exactly. Separate test so mock_reset
     * gives this alloc a fresh pool — the mock's bump allocator
     * doesn't reuse memory on free. */
    vdma_setup();
    struct hailo_vdma_desc_list large;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_vdma_desc_list_alloc(HAILO_VDMA_MAX_DESC_COUNT, 4096, true, &large));
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_MAX_DESC_COUNT, large.desc_count);
    TEST_ASSERT_EQUAL_UINT32(HAILO_VDMA_MAX_DESC_COUNT - 1u,
                             large.desc_count_mask);
    hailo_vdma_desc_list_free(&large);
}

/* -------------------------------------------------------------------------- */
/* Phase 5.3: CCW upload from hef_info                                          */
/* -------------------------------------------------------------------------- */

/* Build a minimal hef_info with N synthetic CCW actions sourced from
 * a contiguous pattern embedded in a caller-owned blob. The test
 * fills one deterministic pattern into the blob and records
 * (offset, size) triples that slice it into N pieces — matching
 * what the real parser produces from a preliminary_config proto. */
static void build_ccw_info(struct hef_info *info,
                           uint8_t *blob, uint32_t blob_size,
                           const uint32_t *sizes, uint32_t count)
{
    memset(info, 0, sizeof(*info));
    for (uint32_t i = 0; i < blob_size; i++) {
        blob[i] = (uint8_t)(i ^ 0x5A);
    }
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(off + sizes[i] <= blob_size);
        info->ccw_actions[i].data_offset_in_blob = off;
        info->ccw_actions[i].data_size           = sizes[i];
        info->ccw_actions[i].cfg_channel_index   = 0;
        info->ccw_actions[i].cfg_channel_index_known = true;
        off += sizes[i];
        info->ccw_total_bytes += sizes[i];
    }
    info->ccw_action_count = count;
    info->ccw_actions_truncated = false;
}

static void test_ccw_upload_rejects_null(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[4] = {0};
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(NULL, blob, sizeof(blob), NULL, 0, 0x10000, NULL));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, NULL, 0, NULL, 0, 0x10000, NULL));
}

static void test_ccw_upload_rejects_truncated(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[4] = {0};
    info.ccw_actions_truncated = true;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x10000, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_rejects_address_wrap(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[16];
    uint32_t sizes[] = { 8 };
    build_ccw_info(&info, blob, sizeof(blob), sizes, 1);
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0xFFFFFFFC, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_empty_info_is_noop(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[4] = {0};
    uint64_t uploaded = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x10000, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(0, uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_single_action(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t blob[256];
    struct hef_info info;
    uint32_t sizes[] = { 32 };
    build_ccw_info(&info, blob, sizeof(blob), sizes, 1);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x100, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(32, uploaded);
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);

    /* Round-trip via READ_MEMORY to confirm the bytes landed at
     * device_base_addr = 0x100. */
    uint8_t readback[32];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x100, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_MEMORY(&blob[info.ccw_actions[0].data_offset_in_blob],
                             readback, sizeof(readback));
}

static void test_ccw_upload_multiple_actions_contiguous(void)
{
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t blob[256];
    struct hef_info info;
    uint32_t sizes[] = { 16, 20, 12 };
    build_ccw_info(&info, blob, sizeof(blob), sizes, 3);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x200, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(16u + 20u + 12u, uploaded);
    TEST_ASSERT_EQUAL_UINT32(3, mock_control_doorbells);

    /* All three actions appended contiguously, so the 48 bytes at
     * [0x200, 0x200+48) should match the first 48 bytes of the
     * blob (because build_ccw_info filled the blob with a single
     * deterministic pattern and sliced it sequentially). */
    uint8_t readback[48];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x200, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_MEMORY(blob, readback, sizeof(readback));
}

static void test_ccw_upload_chunks_large_action(void)
{
    /* 2500 B single action → hailo_control_write_memory's internal
     * 1 KB chunking fires 3 WRITE_MEMORY doorbells. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    static uint8_t blob[4096];
    struct hef_info info;
    uint32_t sizes[] = { 2500 };
    build_ccw_info(&info, blob, sizeof(blob), sizes, 1);

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x400, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, mock_control_doorbells);
}

/* Helper: build a CCW action list where every entry is a CCW_PTR
 * (is_ccw_ptr=true), with offsets into a standalone ccws buffer. */
static void build_ccw_ptr_info(struct hef_info *info,
                               uint8_t *ccws, uint32_t ccws_size,
                               const uint32_t *sizes, uint32_t count)
{
    memset(info, 0, sizeof(*info));
    for (uint32_t i = 0; i < ccws_size; i++) {
        ccws[i] = (uint8_t)(i ^ 0xA5);
    }
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(off + sizes[i] <= ccws_size);
        info->ccw_actions[i].data_offset_in_blob = off;
        info->ccw_actions[i].data_size           = sizes[i];
        info->ccw_actions[i].cfg_channel_index   = 0;
        info->ccw_actions[i].cfg_channel_index_known = true;
        info->ccw_actions[i].is_ccw_ptr          = true;
        off += sizes[i];
        info->ccw_total_bytes += sizes[i];
    }
    info->ccw_action_count = count;
    info->ccw_actions_truncated = false;
}

static void test_ccw_upload_ptr_variant_resolves_from_ccws_base(void)
{
    /* is_ccw_ptr=true actions should source bytes from ccws_base +
     * data_offset_in_blob, NOT from blob_base. Confirm the correct
     * bytes land on the device. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t proto[64];    /* dummy — unused for ptr actions */
    uint8_t ccws[128];
    struct hef_info info;
    uint32_t sizes[] = { 16, 24 };
    build_ccw_ptr_info(&info, ccws, sizeof(ccws), sizes, 2);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, proto, sizeof(proto), ccws, sizeof(ccws), 0x200, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(40, uploaded);

    /* Read back and verify source bytes were ccws[] not proto[]. */
    uint8_t readback[40];
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_read_memory(0x200, readback, sizeof(readback)));
    TEST_ASSERT_EQUAL_MEMORY(ccws, readback, sizeof(readback));
}

static void test_ccw_upload_ptr_variant_rejects_null_ccws_base(void)
{
    /* A v2+ action with is_ccw_ptr=true and no ccws_base is
     * unresolvable; upload must return INVAL without touching the
     * device. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t proto[64];
    uint8_t ccws[128];
    struct hef_info info;
    uint32_t sizes[] = { 16 };
    build_ccw_ptr_info(&info, ccws, sizeof(ccws), sizes, 1);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, proto, sizeof(proto),
                                  /*ccws_base=*/NULL, /*ccws_size=*/0,
                                  0x300, &uploaded));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

/* Shared builder for single-action out-of-bounds tests. */
static void build_oob_action(struct hef_info *info, bool is_ptr,
                             uint32_t offset, uint32_t size)
{
    memset(info, 0, sizeof(*info));
    info->ccw_action_count = 1;
    info->ccw_actions[0].is_ccw_ptr            = is_ptr;
    info->ccw_actions[0].data_offset_in_blob   = offset;
    info->ccw_actions[0].data_size             = size;
    info->ccw_actions[0].cfg_channel_index_known = true;
    info->ccw_total_bytes = size;
}

static void test_ccw_upload_rejects_action_past_ccws_size(void)
{
    /* Malformed/adversarial HEF: action claims offset+size past the
     * CCWS block's actual size. Without the bounds check, upload_ccw
     * would read past the end of `ccws` and forward arbitrary kernel
     * memory to firmware. The check must reject cleanly before the
     * first doorbell fires. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t proto[64];
    uint8_t ccws[128];
    struct hef_info info;
    /* offset=100 + size=64 → end=164 > 128 */
    build_oob_action(&info, /*is_ptr=*/true, 100, 64);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, proto, sizeof(proto),
                                  ccws, sizeof(ccws),
                                  0x500, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(0, uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_rejects_action_past_blob_size(void)
{
    /* Mirror for the v0/v1 (inline-blob) path: action's offset+size
     * exceeds blob_size → INVAL. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t blob[32];
    struct hef_info info;
    /* offset=30 + size=16 → end=46 > 32 */
    build_oob_action(&info, /*is_ptr=*/false, 30, 16);

    uint64_t uploaded = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0,
                                  0x600, &uploaded));
    TEST_ASSERT_EQUAL_UINT64(0, uploaded);
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_skips_zero_size_action(void)
{
    /* A zero-size action is silently skipped — no doorbell, no
     * accounting. Defensive handling keeps callers from tripping
     * WRITE_MEMORY's zero-length INVAL guard. */
    control_setup_running();
    mock_fw_sim_smart_memory_enabled = true;

    uint8_t blob[64];
    struct hef_info info;
    uint32_t sizes[] = { 8, 0, 8 };
    build_ccw_info(&info, blob, sizeof(blob), sizes, 3);

    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, sizeof(blob), NULL, 0, 0x300, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, mock_control_doorbells);
}

/* -------------------------------------------------------------------------- */
/* CONFIG_STREAM (Phase 5.3, #281 tier-3)                                      */
/* -------------------------------------------------------------------------- */

/* Helper — minimal valid cfg with predictable field values. */
static void make_default_pcie_cfg(struct hailo_stream_pcie_config *cfg, bool is_input)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->stream_index             = 3;
    cfg->is_input                 = is_input;
    cfg->skip_nn_stream_config    = false;
    cfg->nn_stream_config.core_bytes_per_buffer    = 0x1234;
    cfg->nn_stream_config.core_buffers_per_frame   = 0x0020;
    cfg->nn_stream_config.periph_bytes_per_buffer  = 0x4000;
    cfg->nn_stream_config.periph_buffers_per_frame = 0x0002;
    cfg->nn_stream_config.feature_padding_payload  = 0x0000;
    cfg->nn_stream_config.buffer_padding_payload   = 0x0000;
    cfg->nn_stream_config.buffer_padding           = 0x0000;
    cfg->nn_stream_config.is_core_hw_padding_config_in_dfc = false;
    cfg->pcie_channel_index       = 5;
    if (is_input) {
        cfg->pcie_dataflow_type = (uint8_t)HAILO_PCIE_DATAFLOW_TYPE_CONTINUOUS;
    } else {
        cfg->desc_page_size = 512;
    }
}

static void test_control_config_stream_rejects_null(void)
{
    control_setup_running();
    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_config_stream_pcie(NULL, &dmid));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_config_stream_pcie(&cfg, NULL));
}

static void test_control_config_stream_rejects_bad_channel(void)
{
    control_setup_running();
    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    cfg.pcie_channel_index = 16;   /* out of range */
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_config_stream_pcie(&cfg, &dmid));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_control_config_stream_rejects_when_not_running(void)
{
    boot_setup_probed();   /* state=PROBED, not RUNNING */
    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_NODEV,
        hailo_control_config_stream_pcie(&cfg, &dmid));
}

static void test_control_config_stream_input_wire_format(void)
{
    control_setup_running();
    mock_fw_sim_config_stream_enabled    = true;
    mock_fw_sim_config_stream_manager_id = 0x42;

    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_config_stream_pcie(&cfg, &dmid));
    TEST_ASSERT_EQUAL_UINT8(0x42, dmid);

    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);

    /* Common header: opcode == CONFIG_STREAM (BE), parameter_count == 7. */
    struct hailo_control_common_header hdr;
    memcpy(&hdr, mock_last_control_request, sizeof(hdr));
    TEST_ASSERT_EQUAL_UINT32(__builtin_bswap32(HAILO_CONTROL_OPCODE_CONFIG_STREAM),
                             hdr.opcode);
    uint32_t pcount_be;
    memcpy(&pcount_be, mock_last_control_request + 16, 4);
    TEST_ASSERT_EQUAL_UINT32(7u, __builtin_bswap32(pcount_be));

    /* Body layout (all scalar lengths BE):
     *   [16 header]
     *   [20 pcount=7]
     *   [24 stream_index_length=1] [28 stream_index=3]
     *   [29 is_input_length=1]     [33 is_input=1]
     *   [34 comm_type_length=4]    [38 comm_type=PCIE=2]
     *   [42 skip_nn_len=1]         [46 skip_nn=0]
     *   [47 nn_len=?]              [51 nn_stream_config...]
     * The "nn_stream_config" struct is 19 B packed (see header).
     */
    uint8_t stream_index, is_input, skip_nn;
    uint32_t stream_index_len_be, is_input_len_be, comm_type_len_be, comm_type_be;
    uint32_t skip_nn_len_be;

    memcpy(&stream_index_len_be, mock_last_control_request + 20, 4);
    memcpy(&stream_index,        mock_last_control_request + 24, 1);
    memcpy(&is_input_len_be,     mock_last_control_request + 25, 4);
    memcpy(&is_input,            mock_last_control_request + 29, 1);
    memcpy(&comm_type_len_be,    mock_last_control_request + 30, 4);
    memcpy(&comm_type_be,        mock_last_control_request + 34, 4);
    memcpy(&skip_nn_len_be,      mock_last_control_request + 38, 4);
    memcpy(&skip_nn,             mock_last_control_request + 42, 1);

    TEST_ASSERT_EQUAL_UINT32(1u,  __builtin_bswap32(stream_index_len_be));
    TEST_ASSERT_EQUAL_UINT8(3,    stream_index);
    TEST_ASSERT_EQUAL_UINT32(1u,  __builtin_bswap32(is_input_len_be));
    TEST_ASSERT_EQUAL_UINT8(1,    is_input);
    TEST_ASSERT_EQUAL_UINT32(4u,  __builtin_bswap32(comm_type_len_be));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)HAILO_COMMUNICATION_TYPE_PCIE,
                             __builtin_bswap32(comm_type_be));
    TEST_ASSERT_EQUAL_UINT32(1u,  __builtin_bswap32(skip_nn_len_be));
    TEST_ASSERT_EQUAL_UINT8(0,    skip_nn);
}

static void test_control_config_stream_output_variant(void)
{
    control_setup_running();
    mock_fw_sim_config_stream_enabled    = true;
    mock_fw_sim_config_stream_manager_id = 0x77;

    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, false);
    cfg.desc_page_size = 0x0200;   /* 512 */
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_config_stream_pcie(&cfg, &dmid));
    TEST_ASSERT_EQUAL_UINT8(0x77, dmid);

    /* Output variant is pcie_channel_index (u8) + desc_page_size (u16).
     * desc_page_size is native LE per HailoRT's non-byteswapped
     * packer — so the low byte comes first on the wire. */
    uint8_t captured_is_input;
    memcpy(&captured_is_input, mock_last_control_request + 29, 1);
    TEST_ASSERT_EQUAL_UINT8(0, captured_is_input);

    /* Wire offsets for CONFIG_STREAM request body (packed):
     *   0  common_header (16)
     *   16 parameter_count (4)
     *   20 stream_index_length (4)  + 24 stream_index (1)
     *   25 is_input_length (4)      + 29 is_input (1)
     *   30 comm_type_length (4)     + 34 communication_type (4)
     *   38 skip_nn_length (4)       + 42 skip_nn (1)
     *   43 nn_stream_config_length (4) + 47 nn_stream_config (19)
     *   66 comm_params_length (4)
     *   70 variant bytes (3 for pcie_output: u8 + u16) */
    uint32_t comm_params_len_be;
    memcpy(&comm_params_len_be, mock_last_control_request + 66, 4);
    TEST_ASSERT_EQUAL_UINT32(3u, __builtin_bswap32(comm_params_len_be));

    uint8_t variant_channel;
    uint16_t variant_page_size;
    memcpy(&variant_channel,  mock_last_control_request + 70, 1);
    memcpy(&variant_page_size, mock_last_control_request + 71, 2);
    TEST_ASSERT_EQUAL_UINT8(5, variant_channel);
    TEST_ASSERT_EQUAL_UINT16(0x0200, variant_page_size);   /* native LE */
}

static void test_control_config_stream_timeout_no_response(void)
{
    control_setup_running();
    /* Neither sim enabled: doorbell fires, nothing responds. */
    mock_fw_sim_config_stream_enabled = false;

    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    uint8_t dmid = 0xFF;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_TIMEOUT,
        hailo_control_config_stream_pcie(&cfg, &dmid));
    TEST_ASSERT_EQUAL_UINT32(1, mock_control_doorbells);
}

static void test_control_config_stream_propagates_fw_error(void)
{
    control_setup_running();

    /* Canned response with non-zero major_status — not smart-mode. */
    struct {
        struct hailo_control_response_header header;
        uint32_t parameter_count;
        uint32_t dataflow_manager_id_length;
        uint8_t  dataflow_manager_id;
    } __attribute__((packed)) fake;
    memset(&fake, 0, sizeof(fake));
    fake.header.common.version = __builtin_bswap32(HAILO_CONTROL_PROTOCOL_VERSION);
    fake.header.common.opcode  = __builtin_bswap32(HAILO_CONTROL_OPCODE_CONFIG_STREAM);
    fake.header.status.major_status = __builtin_bswap32(0x40000058u);
    fake.header.status.minor_status = __builtin_bswap32(0x40000058u);
    fake.dataflow_manager_id_length = __builtin_bswap32(1u);
    memcpy(mock_fw_sim_control_resp, &fake, sizeof(fake));
    mock_fw_sim_control_resp_len = sizeof(fake);
    mock_fw_sim_control_enabled  = true;

    struct hailo_stream_pcie_config cfg;
    make_default_pcie_cfg(&cfg, true);
    uint8_t dmid = 0;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_IO,
        hailo_control_config_stream_pcie(&cfg, &dmid));
}

/* -------------------------------------------------------------------------- */
/* Phase 6.2a: inference_device Hailo backend                                  */
/* -------------------------------------------------------------------------- */

/* Little-endian varint emitter (proto wire format). Returns bytes written. */
static size_t emit_varint(uint8_t *buf, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        buf[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    buf[n++] = (uint8_t)v;
    return n;
}

static void emit_tag(uint8_t *buf, size_t *off, uint32_t field, uint32_t wire_type)
{
    *off += emit_varint(buf + *off, ((uint64_t)field << 3) | wire_type);
}

static void emit_varint_field(uint8_t *buf, size_t *off, uint32_t field, uint64_t v)
{
    emit_tag(buf, off, field, 0);            /* wire type 0 = varint */
    *off += emit_varint(buf + *off, v);
}

static void emit_lenprefix(uint8_t *buf, size_t *off, uint32_t field,
                           const uint8_t *payload, size_t payload_len)
{
    emit_tag(buf, off, field, 2);            /* wire type 2 = length-delimited */
    *off += emit_varint(buf + *off, payload_len);
    memcpy(buf + *off, payload, payload_len);
    *off += payload_len;
}

/* Build a minimal TensorShape sub-message: h/w/f + padded_h/w/f. */
static size_t emit_tensor_shape(uint8_t *buf, uint32_t h, uint32_t w, uint32_t f,
                                uint32_t ph, uint32_t pw, uint32_t pf)
{
    size_t off = 0;
    emit_varint_field(buf, &off, 1, h);
    emit_varint_field(buf, &off, 2, w);
    emit_varint_field(buf, &off, 3, f);
    emit_varint_field(buf, &off, 4, ph);
    emit_varint_field(buf, &off, 5, pw);
    emit_varint_field(buf, &off, 6, pf);
    return off;
}

/* Pad = {index (f1), name (f2), tensor_shape (f6)}. */
static size_t emit_pad_with_shape(uint8_t *buf, uint32_t index, const char *name,
                                  uint32_t h, uint32_t w, uint32_t f,
                                  uint32_t ph, uint32_t pw, uint32_t pf)
{
    size_t off = 0;
    emit_varint_field(buf, &off, 1, index);
    emit_lenprefix(buf, &off, 2, (const uint8_t *)name, strlen(name));
    uint8_t shape_buf[32];
    size_t  shape_len = emit_tensor_shape(shape_buf, h, w, f, ph, pw, pf);
    emit_lenprefix(buf, &off, 6, shape_buf, shape_len);
    return off;
}

/* Build a full ProtoHEFHef body containing one network group with one
 * op holding `num_in` input pads + `num_out` output pads. Each pad has
 * the same tensor shape; change `in_h`/`out_h` to differentiate. */
static size_t build_minimal_proto(uint8_t *out, size_t cap,
                                  uint32_t num_in, uint32_t num_out,
                                  uint32_t in_h, uint32_t in_w, uint32_t in_f,
                                  uint32_t out_h, uint32_t out_w, uint32_t out_f)
{
    (void)cap;
    /* Op: name + input_pads[] + output_pads[]. */
    uint8_t op_buf[2048];
    size_t  op_len = 0;
    emit_lenprefix(op_buf, &op_len, 1, (const uint8_t *)"op", 2);
    for (uint32_t i = 0; i < num_in; i++) {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad_with_shape(pad_buf, i, "in",
                                              in_h, in_w, in_f,
                                              in_h, in_w, in_f);
        emit_lenprefix(op_buf, &op_len, 2, pad_buf, pad_len);
    }
    for (uint32_t i = 0; i < num_out; i++) {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad_with_shape(pad_buf, 100 + i, "out",
                                              out_h, out_w, out_f,
                                              out_h, out_w, out_f);
        emit_lenprefix(op_buf, &op_len, 3, pad_buf, pad_len);
    }

    /* NG: name + ops[]. */
    uint8_t ng_buf[4096];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 10, (const uint8_t *)"ng", 2);
    emit_lenprefix(ng_buf, &ng_len, 8, op_buf, op_len);

    /* Root: one network_groups entry. */
    size_t olen = 0;
    emit_lenprefix(out, &olen, 2, ng_buf, ng_len);
    return olen;
}

/* Wrap a proto body in a valid v0 HEF header and return total blob size.
 * Assumes buf is at least 32 + proto_len bytes. */
static size_t wrap_hef_v0(uint8_t *buf, size_t cap,
                          const uint8_t *proto, size_t proto_len)
{
    if (cap < 32 + proto_len) return 0;
    /* HEF_MAGIC = 0x01484546, stored big-endian as 01 48 45 46. */
    buf[0] = 0x01; buf[1] = 0x48; buf[2] = 0x45; buf[3] = 0x46;
    /* Version V0 = 0. */
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;
    /* proto_size as big-endian u32. */
    buf[8]  = (uint8_t)(proto_len >> 24);
    buf[9]  = (uint8_t)(proto_len >> 16);
    buf[10] = (uint8_t)(proto_len >>  8);
    buf[11] = (uint8_t)(proto_len      );
    /* Reserved (4) + MD5 (16) zero. */
    memset(buf + 12, 0, 20);
    memcpy(buf + 32, proto, proto_len);
    return 32 + proto_len;
}

/* Helper: build a full valid minimal HEF (header + 1-in/1-out proto).
 * Returns total size; caller's buf must be >= 256 bytes. */
static size_t build_test_hef(uint8_t *buf, size_t cap)
{
    uint8_t proto[512];
    size_t  plen = build_minimal_proto(proto, sizeof(proto),
                                        /*in*/1, /*out*/1,
                                        /*in_h,w,f*/1, 1, 128,
                                        /*out_h,w,f*/1, 1, 32);
    return wrap_hef_v0(buf, cap, proto, plen);
}

/* Phase 6.2b helpers: synthesize an EdgeLayerBase + NumericInfo sub-
 * message so the new parser callbacks have something to extract. */
static size_t emit_edge_layer_base(uint8_t *buf,
                                   uint32_t sys_index,
                                   uint32_t core_bytes_per_buffer,
                                   uint32_t core_buffers_per_frame)
{
    size_t off = 0;
    /* Minimal fields: sys_index (f8), core_bytes_per_buffer (f9),
     * core_buffers_per_frame (f10). Other HWxC fields omitted — the
     * parser skips unknown varints, so leaving them absent is fine. */
    emit_varint_field(buf, &off, 8,  sys_index);
    emit_varint_field(buf, &off, 9,  core_bytes_per_buffer);
    emit_varint_field(buf, &off, 10, core_buffers_per_frame);
    return off;
}

/* Emit a NumericInfo message with qp_zp (f1) + qp_scale (f2) as
 * 32-bit fixed-width floats (wire type 5, little-endian). */
static size_t emit_numeric_info(uint8_t *buf,
                                uint32_t qp_zp_raw, uint32_t qp_scale_raw)
{
    size_t off = 0;
    /* f1 tag + 4-byte LE qp_zp */
    emit_tag(buf, &off, 1, 5);
    buf[off + 0] = (uint8_t)(qp_zp_raw      );
    buf[off + 1] = (uint8_t)(qp_zp_raw >>  8);
    buf[off + 2] = (uint8_t)(qp_zp_raw >> 16);
    buf[off + 3] = (uint8_t)(qp_zp_raw >> 24);
    off += 4;
    /* f2 tag + 4-byte LE qp_scale */
    emit_tag(buf, &off, 2, 5);
    buf[off + 0] = (uint8_t)(qp_scale_raw      );
    buf[off + 1] = (uint8_t)(qp_scale_raw >>  8);
    buf[off + 2] = (uint8_t)(qp_scale_raw >> 16);
    buf[off + 3] = (uint8_t)(qp_scale_raw >> 24);
    off += 4;
    return off;
}

/* ProtoHEFEdgeLayerInfo = {name(f1), edge_layer_base(f2), numeric_info(f3)}. */
static size_t emit_edge_layer_info(uint8_t *buf,
                                   uint32_t sys_index,
                                   uint32_t core_bytes_per_buffer,
                                   uint32_t core_buffers_per_frame,
                                   uint32_t qp_zp_raw, uint32_t qp_scale_raw)
{
    uint8_t base_buf[32];
    size_t  base_len = emit_edge_layer_base(base_buf, sys_index,
                                             core_bytes_per_buffer,
                                             core_buffers_per_frame);
    uint8_t num_buf[16];
    size_t  num_len = emit_numeric_info(num_buf, qp_zp_raw, qp_scale_raw);
    size_t  off = 0;
    emit_lenprefix(buf, &off, 1, (const uint8_t *)"ly", 2);
    emit_lenprefix(buf, &off, 2, base_buf, base_len);
    emit_lenprefix(buf, &off, 3, num_buf,  num_len);
    return off;
}

/* ProtoHEFEdgeLayer = {direction(f1), edge_layer_type(f2),
 *                      edge.layer_info(f3), pad_index(f7)}. */
static size_t emit_edge_layer(uint8_t *buf,
                              uint32_t direction, uint32_t pad_index,
                              uint32_t sys_index,
                              uint32_t core_bytes_per_buffer,
                              uint32_t core_buffers_per_frame,
                              uint32_t qp_zp_raw, uint32_t qp_scale_raw)
{
    uint8_t info_buf[128];
    size_t  info_len = emit_edge_layer_info(info_buf,
                                             sys_index,
                                             core_bytes_per_buffer,
                                             core_buffers_per_frame,
                                             qp_zp_raw, qp_scale_raw);
    size_t off = 0;
    emit_varint_field(buf, &off, 1, direction);
    emit_varint_field(buf, &off, 2, 0);    /* edge_layer_type = INFO */
    emit_lenprefix(buf, &off, 3, info_buf, info_len);
    emit_varint_field(buf, &off, 7, pad_index);
    return off;
}

/* Build a HEF with one network group containing one op (1-in + 1-out)
 * AND one context whose metadata carries edge_layers matching both
 * pads (pad_index 0 → input, pad_index 100 → output to match the
 * build_minimal_proto scheme). */
static size_t build_test_hef_with_edge_layers(uint8_t *buf, size_t cap,
                                              uint32_t in_sys_index,
                                              uint32_t in_core_bpb,
                                              uint32_t in_qp_zp_raw,
                                              uint32_t in_qp_scale_raw,
                                              uint32_t out_sys_index,
                                              uint32_t out_core_bpb,
                                              uint32_t out_qp_zp_raw,
                                              uint32_t out_qp_scale_raw)
{
    (void)cap;
    /* Op with 1-in, 1-out (same as build_minimal_proto defaults). */
    uint8_t op_buf[512];
    size_t  op_len = 0;
    emit_lenprefix(op_buf, &op_len, 1, (const uint8_t *)"op", 2);
    {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad_with_shape(pad_buf, 0, "in",
                                              1, 1, 108, 1, 1, 108);
        emit_lenprefix(op_buf, &op_len, 2, pad_buf, pad_len);
    }
    {
        uint8_t pad_buf[64];
        size_t  pad_len = emit_pad_with_shape(pad_buf, 100, "out",
                                              1, 1, 24, 1, 1, 24);
        emit_lenprefix(op_buf, &op_len, 3, pad_buf, pad_len);
    }

    /* Edge layer matching pad_index=0 (input, direction=0). */
    uint8_t elin_buf[256];
    size_t  elin_len = emit_edge_layer(elin_buf, 0, 0,
                                        in_sys_index, in_core_bpb, 1,
                                        in_qp_zp_raw, in_qp_scale_raw);
    /* Edge layer matching pad_index=100 (output, direction=1). */
    uint8_t elout_buf[256];
    size_t  elout_len = emit_edge_layer(elout_buf, 1, 100,
                                         out_sys_index, out_core_bpb, 1,
                                         out_qp_zp_raw, out_qp_scale_raw);

    /* ContextMetadata.edge_layers — two repeated entries. */
    uint8_t md_buf[1024];
    size_t  md_len = 0;
    emit_lenprefix(md_buf, &md_len, 1, (const uint8_t *)"ctx", 3);
    emit_lenprefix(md_buf, &md_len, 2, elin_buf, elin_len);
    emit_lenprefix(md_buf, &md_len, 2, elout_buf, elout_len);

    /* Context: context_index (f1) + metadata (f3). */
    uint8_t ctx_buf[1280];
    size_t  ctx_len = 0;
    emit_varint_field(ctx_buf, &ctx_len, 1, 0);   /* context_index */
    emit_lenprefix(ctx_buf, &ctx_len, 3, md_buf, md_len);

    /* NetworkGroup: name (f10), ops (f8 via op_buf), contexts (f3). */
    uint8_t ng_buf[2048];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 10, (const uint8_t *)"ng", 2);
    emit_lenprefix(ng_buf, &ng_len, 8,  op_buf, op_len);
    emit_lenprefix(ng_buf, &ng_len, 3,  ctx_buf, ctx_len);

    /* ProtoHEFHef: network_groups (f2). */
    uint8_t proto[4096];
    size_t  plen = 0;
    emit_lenprefix(proto, &plen, 2, ng_buf, ng_len);

    return wrap_hef_v0(buf, cap, proto, plen);
}

/* Lookup the Hailo backend, (re-)register it, reset slots, and make
 * sure the firmware is in the RUNNING state. Most tests need all three. */
static struct inference_device *hailo_backend_ready(void)
{
    control_setup_running();
    (void)inference_device_hailo_register();   /* idempotent for test purposes */
    hailo_backend_reset_slots_for_tests();
    return inference_device_find("hailo-8");
}

static void test_inf_hailo_register_succeeds(void)
{
    control_setup_running();
    int rc = inference_device_hailo_register();
    /* The first register returns OK; subsequent runs may return
     * INF_ERR_FULL or similar — both acceptable. Find is the real
     * check. */
    (void)rc;
    struct inference_device *dev = inference_device_find("hailo-8");
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_STRING("hailo-8", dev->ops->name);
    TEST_ASSERT_TRUE(dev->ops->caps & INF_CAP_INT8);
    TEST_ASSERT_TRUE(dev->ops->caps & INF_CAP_LOAD_MODEL);
}

static void test_inf_hailo_load_rejects_null(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_load_model(dev, NULL, 0, &h));
    uint8_t blob[16] = {0};
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_load_model(dev, blob, 0, &h));
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_load_model(dev, blob, sizeof(blob), NULL));
}

static void test_inf_hailo_load_rejects_when_not_running(void)
{
    /* Force device back to PROBED (firmware not booted). */
    boot_setup_probed();
    (void)inference_device_hailo_register();
    hailo_backend_reset_slots_for_tests();
    struct inference_device *dev = inference_device_find("hailo-8");
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[256];
    size_t  n = build_test_hef(blob, sizeof(blob));
    TEST_ASSERT_TRUE(n != 0);

    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_ERR_NODEV,
        inference_load_model(dev, blob, n, &h));
}

static void test_inf_hailo_load_rejects_bad_header(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);
    /* Random 64 bytes with no valid HEF magic. */
    uint8_t blob[64];
    memset(blob, 0xAB, sizeof(blob));
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_ERR_BAD_MODEL,
        inference_load_model(dev, blob, sizeof(blob), &h));
}

static void test_inf_hailo_load_rejects_zero_pads(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);
    /* Build a HEF whose proto body has no network_groups at all →
     * the backend can't find input/output pads → rejection. */
    uint8_t blob[128];
    size_t n = wrap_hef_v0(blob, sizeof(blob), (const uint8_t *)"", 0);
    TEST_ASSERT_TRUE(n != 0);
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_ERR_BAD_MODEL,
        inference_load_model(dev, blob, n, &h));
}

static void test_inf_hailo_load_happy_path(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    TEST_ASSERT_TRUE(n != 0);

    inference_model_handle_t h = INF_INVALID_HANDLE;
    TEST_ASSERT_EQUAL_INT(INF_OK,
        inference_load_model(dev, blob, n, &h));
    TEST_ASSERT_TRUE(h >= 1);
    TEST_ASSERT_EQUAL_UINT32(1, hailo_backend_in_use_slots());
}

static void test_inf_hailo_load_fills_slots_until_full(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    TEST_ASSERT_TRUE(n != 0);

    inference_model_handle_t handles[4];
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(INF_OK,
            inference_load_model(dev, blob, n, &handles[i]));
    }
    TEST_ASSERT_EQUAL_UINT32(4, hailo_backend_in_use_slots());
    /* Fifth load must fail — slot table saturated. */
    inference_model_handle_t overflow;
    TEST_ASSERT_EQUAL_INT(INF_ERR_FULL,
        inference_load_model(dev, blob, n, &overflow));
}

static void test_inf_hailo_run_rejects_bad_handle(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t in_buf[128], out_buf[32];
    inference_tensor_t in = {
        .data = in_buf, .n_elems = 128, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {128, 0, 0, 0},
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = 32, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {32, 0, 0, 0},
    };
    /* h=0 reserved for BUILTIN (not a Hailo handle). */
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_run(dev, INF_BUILTIN_HANDLE, &in, &out));
    /* h > HAILO_MAX_MODELS out of range. */
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_run(dev, 99, &in, &out));
}

static void test_inf_hailo_run_rejects_wrong_dtype(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h));

    uint8_t in_buf[128], out_buf[32];
    inference_tensor_t in = {
        .data = in_buf, .n_elems = 128, .dtype = INF_DTYPE_FP32, /* wrong */
        .rank = 1, .shape = {128, 0, 0, 0},
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = 32, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {32, 0, 0, 0},
    };
    TEST_ASSERT_EQUAL_INT(INF_ERR_BAD_TENSOR,
        inference_run(dev, h, &in, &out));
}

static void test_inf_hailo_run_rejects_size_mismatch(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h));

    uint8_t in_buf[64], out_buf[32];
    inference_tensor_t in = {
        .data = in_buf, .n_elems = 64, /* expected 128 */ .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {64, 0, 0, 0},
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = 32, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {32, 0, 0, 0},
    };
    TEST_ASSERT_EQUAL_INT(INF_ERR_BAD_TENSOR,
        inference_run(dev, h, &in, &out));
}

static void test_inf_hailo_run_happy_path_via_auto_advance(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h));

    /* hailo_infer_run polls VDMA num_proc; mock_vdma_auto_advance = true
     * makes every num_avail write echo into num_proc so the poll
     * converges on the first iteration. */
    mock_vdma_auto_advance = true;

    uint8_t in_buf[128], out_buf[32];
    memset(in_buf, 0x7F, sizeof(in_buf));
    memset(out_buf, 0x00, sizeof(out_buf));
    inference_tensor_t in = {
        .data = in_buf, .n_elems = 128, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {128, 0, 0, 0},
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = 32, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {32, 0, 0, 0},
    };
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_run(dev, h, &in, &out));
}

static void test_inf_hailo_free_releases_slot(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h));
    TEST_ASSERT_EQUAL_UINT32(1, hailo_backend_in_use_slots());

    TEST_ASSERT_EQUAL_INT(INF_OK, inference_free_model(dev, h));
    TEST_ASSERT_EQUAL_UINT32(0, hailo_backend_in_use_slots());

    /* Double-free is an error. */
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL, inference_free_model(dev, h));
    /* Reserved handle is always an error. */
    TEST_ASSERT_EQUAL_INT(INF_ERR_INVAL,
        inference_free_model(dev, INF_BUILTIN_HANDLE));
}

#ifdef CONFIG_AI_SCHEDULER
#include "../sched/ai/ai_policy_hailo.h"
#include "../sched/ai/ai_types.h"

static void test_ai_policy_hailo_set_and_get_handle(void)
{
    /* set_model_placeholder installs a handle; get_model_handle returns it. */
    ai_policy_hailo_set_model_placeholder((inference_model_handle_t)3,
                                           AI_STATE_DIM, AI_SCHED_N_ACTIONS);
    TEST_ASSERT_EQUAL_INT(3, (int)ai_policy_hailo_get_model_handle());
}

static void test_ai_policy_hailo_clear_detaches_model(void)
{
    /* Arm, then detach — get_model_handle must report INVALID. */
    ai_policy_hailo_set_model_placeholder((inference_model_handle_t)2,
                                           AI_STATE_DIM, AI_SCHED_N_ACTIONS);
    TEST_ASSERT_EQUAL_INT(2, (int)ai_policy_hailo_get_model_handle());

    ai_policy_hailo_set_model_placeholder(INF_INVALID_HANDLE, 0, 0);
    TEST_ASSERT_EQUAL_INT((int)INF_INVALID_HANDLE,
                          (int)ai_policy_hailo_get_model_handle());
}

static void test_ai_policy_hailo_set_from_raw_happy_path(void)
{
    /* Valid IEEE-754 float bit patterns: scale=0x3C000000 (1/128),
     * zp=0. Setter installs and get returns the handle. */
    int rc = ai_policy_hailo_set_model_from_raw((inference_model_handle_t)4,
                                                0x3C000000u, 0,
                                                0x3C000000u, 0,
                                                AI_STATE_DIM,
                                                AI_SCHED_N_ACTIONS);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(4, (int)ai_policy_hailo_get_model_handle());
    /* Clean up for independence between tests. */
    ai_policy_hailo_set_model_placeholder(INF_INVALID_HANDLE, 0, 0);
}

static void test_ai_policy_hailo_set_from_raw_rejects_zero_scale(void)
{
    /* scale_raw=0 → float 0.0 → setter returns -1 without mutating state. */
    ai_policy_hailo_set_model_placeholder(INF_INVALID_HANDLE, 0, 0);
    int rc = ai_policy_hailo_set_model_from_raw((inference_model_handle_t)7,
                                                0u, 0,              /* scale=0 */
                                                0x3C000000u, 0,
                                                AI_STATE_DIM,
                                                AI_SCHED_N_ACTIONS);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    /* No model installed despite the failure. */
    TEST_ASSERT_EQUAL_INT((int)INF_INVALID_HANDLE,
                          (int)ai_policy_hailo_get_model_handle());
}

static void test_ai_policy_hailo_set_from_raw_rejects_nan_scale(void)
{
    /* scale_raw=0x7FC00000 = quiet NaN → setter rejects. */
    ai_policy_hailo_set_model_placeholder(INF_INVALID_HANDLE, 0, 0);
    int rc = ai_policy_hailo_set_model_from_raw((inference_model_handle_t)8,
                                                0x7FC00000u, 0,
                                                0x3C000000u, 0,
                                                AI_STATE_DIM,
                                                AI_SCHED_N_ACTIONS);
    TEST_ASSERT_EQUAL_INT(-1, rc);
}
#endif

/* Phase 6.2b: HEF edge-layer quant + stream extraction. */

static void test_hef_parser_captures_edge_layer_quant(void)
{
    /* scale=0x3C000000 = IEEE-754 0.0078125 (1/128), zp=0x00000000 = 0.0.
     * Arbitrary but deterministic so the bit-pattern round-trip can be
     * asserted without reinterpreting to float inside the test. */
    uint8_t blob[2048];
    size_t  n = build_test_hef_with_edge_layers(blob, sizeof(blob),
                                                /*in*/ 5, 128,
                                                0x00000000u, 0x3C000000u,
                                                /*out*/ 9, 32,
                                                0x00000000u, 0x3C800000u);
    TEST_ASSERT_TRUE(n != 0);

    struct hef_info info;
    const uint8_t *proto = blob + 32;   /* 32-byte v0 header */
    size_t proto_len = n - 32;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK,
                          hef_parse_body(proto, proto_len, &info));
    TEST_ASSERT_EQUAL_UINT32(2, info.pad_count);

    /* Input pad (index 0) should have quant + stream from edge layer 1. */
    TEST_ASSERT_TRUE(info.pads[0].is_input);
    TEST_ASSERT_TRUE(info.pads[0].has_quant_info);
    TEST_ASSERT_EQUAL_UINT32(0x3C000000u, info.pads[0].qp_scale_raw);
    TEST_ASSERT_EQUAL_UINT32(0x00000000u, info.pads[0].qp_zp_raw);
    TEST_ASSERT_TRUE(info.pads[0].has_stream_info);
    TEST_ASSERT_EQUAL_UINT32(5,   info.pads[0].sys_index);
    TEST_ASSERT_EQUAL_UINT32(128, info.pads[0].core_bytes_per_buffer);

    /* Output pad (index 100) should have quant + stream from edge layer 2. */
    TEST_ASSERT_FALSE(info.pads[1].is_input);
    TEST_ASSERT_TRUE(info.pads[1].has_quant_info);
    TEST_ASSERT_EQUAL_UINT32(0x3C800000u, info.pads[1].qp_scale_raw);
    TEST_ASSERT_TRUE(info.pads[1].has_stream_info);
    TEST_ASSERT_EQUAL_UINT32(9,  info.pads[1].sys_index);
    TEST_ASSERT_EQUAL_UINT32(32, info.pads[1].core_bytes_per_buffer);
}

static void test_hef_parser_edge_layer_creates_pad_when_ops_empty(void)
{
    /* DFC 3.33.1 leaves ProtoHEFNetworkGroup.ops[] empty for simple
     * MLPs. Under that layout, edge_layers[] is the ONLY source of
     * pad info. Build a HEF with NO ops and just edge layers, and
     * verify the parser creates pad entries directly. */
    uint8_t proto[1024];
    size_t  plen = 0;

    /* Edge layer for an input pad: direction=0, pad_index=0,
     * shape 1x1x108, sys_index=3, core_bytes=128, scale=0x3C000000 (1/128). */
    uint8_t elin[256];
    size_t  elin_len = 0;
    emit_varint_field(elin, &elin_len, 1, 0);      /* direction = H2D */
    emit_varint_field(elin, &elin_len, 2, 0);      /* edge_layer_type = INFO */
    /* edge.layer_info with shape + stream + quant */
    uint8_t info_buf[128];
    size_t  info_len = 0;
    emit_lenprefix(info_buf, &info_len, 1, (const uint8_t *)"in", 2);
    {
        uint8_t base[32];
        size_t  base_len = 0;
        emit_varint_field(base, &base_len, 1, 1);   /* height */
        emit_varint_field(base, &base_len, 2, 1);   /* padded_height */
        emit_varint_field(base, &base_len, 3, 1);   /* width */
        emit_varint_field(base, &base_len, 4, 1);   /* padded_width */
        emit_varint_field(base, &base_len, 5, 108); /* features */
        emit_varint_field(base, &base_len, 6, 108); /* padded_features */
        emit_varint_field(base, &base_len, 8, 3);   /* sys_index */
        emit_varint_field(base, &base_len, 9, 128); /* core_bytes_per_buffer */
        emit_lenprefix(info_buf, &info_len, 2, base, base_len);
    }
    {
        uint8_t num[16];
        size_t  num_len = emit_numeric_info(num, 0u, 0x3C000000u);
        emit_lenprefix(info_buf, &info_len, 3, num, num_len);
    }
    emit_lenprefix(elin, &elin_len, 3, info_buf, info_len);
    emit_varint_field(elin, &elin_len, 7, 0);      /* pad_index */

    /* ContextMetadata with just this one edge layer (no ops). */
    uint8_t md_buf[512];
    size_t  md_len = 0;
    emit_lenprefix(md_buf, &md_len, 1, (const uint8_t *)"ctx", 3);
    emit_lenprefix(md_buf, &md_len, 2, elin, elin_len);

    uint8_t ctx_buf[768];
    size_t  ctx_len = 0;
    emit_varint_field(ctx_buf, &ctx_len, 1, 0);    /* context_index */
    emit_lenprefix(ctx_buf, &ctx_len, 3, md_buf, md_len);

    /* NetworkGroup with contexts but NO ops. */
    uint8_t ng_buf[1024];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 10, (const uint8_t *)"ng", 2);
    emit_lenprefix(ng_buf, &ng_len, 3,  ctx_buf, ctx_len);

    emit_lenprefix(proto, &plen, 2, ng_buf, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK,
                          hef_parse_body(proto, plen, &info));
    TEST_ASSERT_EQUAL_UINT32(0, info.op_count);   /* no ops */
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);  /* pad created from edge_layer */
    TEST_ASSERT_TRUE(info.pads[0].is_input);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(108, info.pads[0].features);
    TEST_ASSERT_TRUE(info.pads[0].has_quant_info);
    TEST_ASSERT_EQUAL_UINT32(0x3C000000u, info.pads[0].qp_scale_raw);
    TEST_ASSERT_TRUE(info.pads[0].has_stream_info);
    TEST_ASSERT_EQUAL_UINT32(3,   info.pads[0].sys_index);
    TEST_ASSERT_EQUAL_UINT32(128, info.pads[0].core_bytes_per_buffer);
}

static void test_hef_parser_edge_layer_backfills_shape_on_shapeless_pad(void)
{
    /* Op with a shape-less pad (no tensor_shape emitted), then an
     * edge_layer with direction + pad_index + shape. The edge-layer
     * callback should back-fill the existing pad's shape. Matches the
     * "found pad, p->has_tensor_shape == false" branch. */
    uint8_t proto[1024];
    size_t  plen = 0;

    /* Op with one input pad (index 0), NO tensor_shape set. We
     * build a pad message with just name + index, no shape_info. */
    uint8_t op_buf[256];
    size_t  op_len = 0;
    emit_lenprefix(op_buf, &op_len, 1, (const uint8_t *)"op", 2);
    {
        uint8_t pad_buf[64];
        size_t  pad_len = 0;
        emit_varint_field(pad_buf, &pad_len, 1, 0);  /* pad index = 0 */
        emit_lenprefix(pad_buf, &pad_len, 2,
                       (const uint8_t *)"in", 2);    /* pad name */
        /* shape_info intentionally omitted. */
        emit_lenprefix(op_buf, &op_len, 2, pad_buf, pad_len);
    }

    /* Edge layer for pad 0 with shape 1x1x42. */
    uint8_t elin[256];
    size_t  elin_len = 0;
    emit_varint_field(elin, &elin_len, 1, 0);     /* direction = H2D */
    emit_varint_field(elin, &elin_len, 2, 0);     /* edge_layer_type = INFO */
    uint8_t info_buf[128];
    size_t  info_len = 0;
    emit_lenprefix(info_buf, &info_len, 1, (const uint8_t *)"ly", 2);
    {
        uint8_t base[32];
        size_t  base_len = 0;
        emit_varint_field(base, &base_len, 1, 1);   /* height = 1 */
        emit_varint_field(base, &base_len, 3, 1);   /* width = 1 */
        emit_varint_field(base, &base_len, 5, 42);  /* features = 42 */
        emit_lenprefix(info_buf, &info_len, 2, base, base_len);
    }
    emit_lenprefix(elin, &elin_len, 3, info_buf, info_len);
    emit_varint_field(elin, &elin_len, 7, 0);     /* pad_index */

    uint8_t md_buf[512];
    size_t  md_len = 0;
    emit_lenprefix(md_buf, &md_len, 1, (const uint8_t *)"ctx", 3);
    emit_lenprefix(md_buf, &md_len, 2, elin, elin_len);

    uint8_t ctx_buf[768];
    size_t  ctx_len = 0;
    emit_varint_field(ctx_buf, &ctx_len, 1, 0);
    emit_lenprefix(ctx_buf, &ctx_len, 3, md_buf, md_len);

    uint8_t ng_buf[1024];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 10, (const uint8_t *)"ng", 2);
    emit_lenprefix(ng_buf, &ng_len, 8,  op_buf, op_len);
    emit_lenprefix(ng_buf, &ng_len, 3,  ctx_buf, ctx_len);

    emit_lenprefix(proto, &plen, 2, ng_buf, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK,
                          hef_parse_body(proto, plen, &info));
    /* ops path registered the pad, edge_layer path back-filled shape. */
    TEST_ASSERT_EQUAL_UINT32(1, info.op_count);
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(42, info.pads[0].features);
}

static void test_hef_parser_edge_layer_uses_sys_index_when_pad_index_absent(void)
{
    /* DFC 3.33.1 wire format: scheduler_mlp_pi5.hef edge_layers omit
     * pad_index entirely on their boundary layers. Our parser must
     * fall back to sys_index (from edge_layer_base) as the pad key,
     * and treat missing direction as input (proto3 default=0).
     *
     * Build a HEF with ONE edge_layer that has shape + sys_index but
     * NO pad_index and NO direction. Parser should create an input
     * pad indexed by sys_index. */
    uint8_t proto[1024];
    size_t  plen = 0;

    uint8_t elin[256];
    size_t  elin_len = 0;
    /* NO direction (field 1) — default to H2D=input */
    /* NO edge_layer_type (field 2) — default to INFO */
    uint8_t info_buf[128];
    size_t  info_len = 0;
    emit_lenprefix(info_buf, &info_len, 1, (const uint8_t *)"in", 2);
    {
        uint8_t base[32];
        size_t  base_len = 0;
        emit_varint_field(base, &base_len, 1, 1);    /* height */
        emit_varint_field(base, &base_len, 3, 1);    /* width */
        emit_varint_field(base, &base_len, 5, 108);  /* features */
        emit_varint_field(base, &base_len, 8, 7);    /* sys_index = 7 */
        emit_varint_field(base, &base_len, 9, 256);  /* core_bytes_per_buffer */
        emit_lenprefix(info_buf, &info_len, 2, base, base_len);
    }
    emit_lenprefix(elin, &elin_len, 3, info_buf, info_len);
    /* NO pad_index (field 7) */

    uint8_t md_buf[512];
    size_t  md_len = 0;
    emit_lenprefix(md_buf, &md_len, 2, elin, elin_len);

    uint8_t ctx_buf[768];
    size_t  ctx_len = 0;
    emit_lenprefix(ctx_buf, &ctx_len, 3, md_buf, md_len);

    uint8_t ng_buf[1024];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 3, ctx_buf, ctx_len);

    emit_lenprefix(proto, &plen, 2, ng_buf, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK,
                          hef_parse_body(proto, plen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_TRUE(info.pads[0].is_input);        /* default direction 0 */
    TEST_ASSERT_EQUAL_UINT32(7, info.pads[0].index); /* sys_index = 7 */
    TEST_ASSERT_TRUE(info.pads[0].has_tensor_shape);
    TEST_ASSERT_EQUAL_UINT32(108, info.pads[0].features);
    TEST_ASSERT_EQUAL_UINT32(7,   info.pads[0].sys_index);
    TEST_ASSERT_EQUAL_UINT32(256, info.pads[0].core_bytes_per_buffer);
}

static void test_hef_parser_edge_layer_direction_1_means_output(void)
{
    /* Mirror of the above but with explicit direction=1 (DEVICE_TO_HOST).
     * Verifies is_input flips to false. */
    uint8_t proto[1024];
    size_t  plen = 0;

    uint8_t el[256];
    size_t  el_len = 0;
    emit_varint_field(el, &el_len, 1, 1);            /* direction = D2H (output) */

    uint8_t info_buf[128];
    size_t  info_len = 0;
    emit_lenprefix(info_buf, &info_len, 1, (const uint8_t *)"out", 3);
    {
        uint8_t base[32];
        size_t  base_len = 0;
        emit_varint_field(base, &base_len, 5, 24);   /* features = 24 */
        emit_varint_field(base, &base_len, 8, 3);    /* sys_index = 3 */
        emit_lenprefix(info_buf, &info_len, 2, base, base_len);
    }
    emit_lenprefix(el, &el_len, 3, info_buf, info_len);

    uint8_t md_buf[512];
    size_t  md_len = 0;
    emit_lenprefix(md_buf, &md_len, 2, el, el_len);

    uint8_t ctx_buf[768];
    size_t  ctx_len = 0;
    emit_lenprefix(ctx_buf, &ctx_len, 3, md_buf, md_len);

    uint8_t ng_buf[1024];
    size_t  ng_len = 0;
    emit_lenprefix(ng_buf, &ng_len, 3, ctx_buf, ctx_len);

    emit_lenprefix(proto, &plen, 2, ng_buf, ng_len);

    struct hef_info info;
    TEST_ASSERT_EQUAL_INT(HEF_PARSER_OK,
                          hef_parse_body(proto, plen, &info));
    TEST_ASSERT_EQUAL_UINT32(1, info.pad_count);
    TEST_ASSERT_FALSE(info.pads[0].is_input);
    TEST_ASSERT_EQUAL_UINT32(3, info.pads[0].index);
    TEST_ASSERT_EQUAL_UINT32(24, info.pads[0].features);
}

static void test_inf_hailo_load_with_no_ccw_actions_succeeds(void)
{
    /* build_test_hef produces a HEF with valid pads but NO
     * write_data_ccw actions, so `info.ccw_action_count == 0` and
     * upload_ccw_best_effort short-circuits. load_model returns OK
     * and the slot is live.
     *
     * The survives-failure-path-when-CCW-actions-exist scenario is
     * exercised at the unit level by the test_ccw_upload_rejects_*
     * tests above, which hit hailo_control_upload_ccw directly and
     * confirm the bounds-check rejection without going through
     * load_model. */
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    mock_fw_sim_config_stream_enabled    = true;
    mock_fw_sim_config_stream_manager_id = 0x33;

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    TEST_ASSERT_TRUE(n != 0);

    inference_model_handle_t h = INF_INVALID_HANDLE;
    TEST_ASSERT_EQUAL_INT(INF_OK,
        inference_load_model(dev, blob, n, &h));
    TEST_ASSERT_TRUE(h >= 1);
    TEST_ASSERT_EQUAL_UINT32(1, hailo_backend_in_use_slots());
}

static void test_inf_hailo_load_threads_hef_stream_info(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    /* Build a HEF where the input pad has sys_index=7 and
     * core_bytes_per_buffer=256 via an edge layer. The backend must
     * propagate those values into its slot's hailo_infer_config so
     * when run() later feeds the device, the right stream params
     * are used. We can't read the slot's cfg directly; instead, we
     * assert the load succeeded + the input_bytes field (which is
     * pad-shape-derived) matches the pad shape.
     *
     * With the Phase 6.2+ chain in place, load_model ALSO fires two
     * CONFIG_STREAM RPCs (input + output) whenever pads carry real
     * has_stream_info. Enable the mock firmware's config_stream
     * simulator so the handshake completes rather than timing out. */
    mock_fw_sim_config_stream_enabled    = true;
    mock_fw_sim_config_stream_manager_id = 0x42;

    uint8_t blob[2048];
    size_t  n = build_test_hef_with_edge_layers(blob, sizeof(blob),
                                                7, 256, 0, 0x3C000000u,
                                                9,  32, 0, 0x3C800000u);
    TEST_ASSERT_TRUE(n != 0);

    inference_model_handle_t h;
    TEST_ASSERT_EQUAL_INT(INF_OK,
        inference_load_model(dev, blob, n, &h));

    /* Run a forward pass — confirms the slot's cfg is structurally
     * valid (nonzero page sizes, valid channels). Actual stream
     * propagation is a mock-internal detail that hailo_infer_run
     * covers via its own VDMA reg writes. */
    mock_vdma_auto_advance = true;
    uint8_t in_buf[108], out_buf[24];
    memset(in_buf, 0, sizeof(in_buf));
    memset(out_buf, 0, sizeof(out_buf));
    inference_tensor_t in = {
        .data = in_buf, .n_elems = 108, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {108, 0, 0, 0},
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = 24, .dtype = INF_DTYPE_INT8,
        .rank = 1, .shape = {24, 0, 0, 0},
    };
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_run(dev, h, &in, &out));
}

static void test_inf_hailo_shutdown_clears_slots(void)
{
    struct inference_device *dev = hailo_backend_ready();
    TEST_ASSERT_NOT_NULL(dev);

    uint8_t blob[512];
    size_t  n = build_test_hef(blob, sizeof(blob));
    inference_model_handle_t h1, h2;
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h1));
    TEST_ASSERT_EQUAL_INT(INF_OK, inference_load_model(dev, blob, n, &h2));
    TEST_ASSERT_EQUAL_UINT32(2, hailo_backend_in_use_slots());

    /* Shutdown is idempotent and clears all slots. */
    if (dev->ops->shutdown) dev->ops->shutdown(dev);
    TEST_ASSERT_EQUAL_UINT32(0, hailo_backend_in_use_slots());
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_hailo(void)
{
    UnityBegin("Hailo driver core");

    RUN_TEST(test_init_rejects_null_ops);
    RUN_TEST(test_init_rejects_incomplete_ops);
    RUN_TEST(test_init_rejects_missing_mb);
    RUN_TEST(test_init_accepts_complete_ops);
    RUN_TEST(test_probe_detects_no_device);
    RUN_TEST(test_probe_rejects_wrong_vendor);
    RUN_TEST(test_probe_reads_ids_then_boot_status);
    RUN_TEST(test_validate_firmware_rejects_short_blob);
    RUN_TEST(test_validate_firmware_rejects_bad_magic);
    RUN_TEST(test_validate_firmware_rejects_oversize_code);
    RUN_TEST(test_validate_firmware_rejects_truncated);
    RUN_TEST(test_validate_firmware_accepts_minimal_valid);
    RUN_TEST(test_validate_firmware_rejects_zero_code_size);
    RUN_TEST(test_validate_firmware_rejects_null_bytes);
    RUN_TEST(test_init_propagates_platform_init_failure);
    RUN_TEST(test_state_str_labels_known_values);
    RUN_TEST(test_boot_rejects_wrong_state);
    RUN_TEST(test_boot_rejects_bad_magic);
    RUN_TEST(test_boot_rejects_missing_cert);
    RUN_TEST(test_boot_rejects_cert_oversize);
    RUN_TEST(test_boot_rejects_unexpected_boot_status);
    RUN_TEST(test_boot_succeeds_and_uploads_sections);
    RUN_TEST(test_boot_rejects_missing_core_fw);
    RUN_TEST(test_boot_rejects_bad_core_magic);
    RUN_TEST(test_boot_rejects_core_code_size_zero);
    RUN_TEST(test_boot_rejects_core_code_size_oversize);
    RUN_TEST(test_boot_rejects_core_code_truncated);
    RUN_TEST(test_boot_fails_when_fw_never_signals_loaded);
    RUN_TEST(test_boot_chunks_large_code);
    RUN_TEST(test_boot_core_chunks_large_code);

    /* Direct decode-helper tests (PR #275) — bypass hailo_boot so the
     * pure format/bounds logic is exercised in isolation. */
    RUN_TEST(test_decode_cert_rejects_null_blob);
    RUN_TEST(test_decode_cert_rejects_null_cert_out);
    RUN_TEST(test_decode_cert_rejects_null_key_out);
    RUN_TEST(test_decode_cert_rejects_null_content_out);
    RUN_TEST(test_decode_cert_rejects_null_cert_end_out);
    RUN_TEST(test_decode_cert_rejects_header_past_end);
    RUN_TEST(test_decode_cert_rejects_zero_key);
    RUN_TEST(test_decode_cert_rejects_zero_content);
    RUN_TEST(test_decode_cert_rejects_oversize_key);
    RUN_TEST(test_decode_cert_rejects_oversize_content);
    RUN_TEST(test_decode_cert_rejects_misaligned_key);
    RUN_TEST(test_decode_cert_rejects_misaligned_content);
    RUN_TEST(test_decode_cert_rejects_truncated_payload);
    RUN_TEST(test_decode_cert_accepts_tight_fit);
    RUN_TEST(test_decode_cert_rejects_one_byte_short);
    RUN_TEST(test_decode_cert_success_populates_outputs);
    RUN_TEST(test_decode_core_fw_rejects_null_blob);
    RUN_TEST(test_decode_core_fw_rejects_null_core_hdr_out);
    RUN_TEST(test_decode_core_fw_rejects_null_core_code_out);
    RUN_TEST(test_decode_core_fw_rejects_header_past_end);
    RUN_TEST(test_decode_core_fw_rejects_bad_magic);
    RUN_TEST(test_decode_core_fw_rejects_unsupported_header_version);
    RUN_TEST(test_decode_core_fw_rejects_zero_code_size);
    RUN_TEST(test_decode_core_fw_rejects_oversize_code);
    RUN_TEST(test_decode_core_fw_rejects_truncated_code);
    RUN_TEST(test_decode_core_fw_accepts_tight_fit);
    RUN_TEST(test_decode_core_fw_rejects_one_byte_short);
    RUN_TEST(test_decode_core_fw_success_populates_outputs);

    /* Control-channel IDENTIFY (Phase 5.2 tier 1, #281) */
    RUN_TEST(test_control_identify_rejects_when_not_running);
    RUN_TEST(test_control_identify_rejects_null_out);
    RUN_TEST(test_control_identify_happy_path);
    RUN_TEST(test_control_identify_timeout_no_response);
    RUN_TEST(test_control_send_recv_cpu_core_rings_core_doorbell);
    RUN_TEST(test_set_network_group_header_rings_core_doorbell_and_wire);
    RUN_TEST(test_set_network_group_header_rejects_null);
    RUN_TEST(test_set_network_group_header_rejects_bad_config_count);
    RUN_TEST(test_set_context_info_chunk_single_wire_layout);
    RUN_TEST(test_set_context_info_chunks_oversize_payload);
    RUN_TEST(test_set_context_info_zero_length_is_single_chunk);
    RUN_TEST(test_set_context_info_chunk_rejects_oversize);
    RUN_TEST(test_set_context_info_chunk_rejects_null_with_nonzero_len);
    RUN_TEST(test_cs_builder_append_emits_header_then_body);
    RUN_TEST(test_cs_builder_appends_concatenate);
    RUN_TEST(test_cs_builder_returns_nomem_on_overflow);
    RUN_TEST(test_cs_builder_rejects_null_buffer);
    RUN_TEST(test_control_send_recv_default_rings_app_doorbell);
    RUN_TEST(test_control_identify_request_wire_format_is_be);
    RUN_TEST(test_control_identify_arms_imask_once);
    RUN_TEST(test_control_identify_ignores_non_fw_control_irq);

    /* WRITE_MEMORY / READ_MEMORY (Phase 5.3 tier-2, #281) */
    RUN_TEST(test_control_write_memory_rejects_null);
    RUN_TEST(test_control_read_memory_rejects_null);
    RUN_TEST(test_control_write_memory_rejects_oversize);
    RUN_TEST(test_control_read_memory_rejects_oversize);
    RUN_TEST(test_control_write_memory_rejects_address_wrap);
    RUN_TEST(test_control_read_memory_rejects_address_wrap);
    RUN_TEST(test_control_write_memory_rejects_when_not_running);
    RUN_TEST(test_control_read_memory_rejects_when_not_running);
    RUN_TEST(test_control_write_memory_sends_correct_wire);
    RUN_TEST(test_control_read_memory_sends_correct_wire);
    RUN_TEST(test_control_read_memory_returns_device_bytes);
    RUN_TEST(test_control_memory_round_trip);
    RUN_TEST(test_control_memory_chunks_large_transfer);
    RUN_TEST(test_control_write_memory_single_chunk_boundary);
    RUN_TEST(test_control_write_memory_partial_last_chunk);
    RUN_TEST(test_control_write_memory_propagates_fw_error);
    RUN_TEST(test_control_read_memory_propagates_fw_error);
    RUN_TEST(test_control_write_memory_rejects_wrong_opcode_echo);

    /* Phase 5.3 tensor-buffer tests */
    RUN_TEST(test_tensor_size_from_shape_small);
    RUN_TEST(test_tensor_size_from_shape_rejects_zero);
    RUN_TEST(test_tensor_size_from_shape_rejects_overflow);
    RUN_TEST(test_tensor_alloc_happy_path);
    RUN_TEST(test_tensor_alloc_rejects_null_out);
    RUN_TEST(test_tensor_alloc_rejects_zero_size);
    RUN_TEST(test_tensor_alloc_nodev_without_platform);
    RUN_TEST(test_tensor_alloc_propagates_nomem);
    RUN_TEST(test_tensor_prepare_for_device_fires_cache_clean);
    RUN_TEST(test_tensor_prepare_for_host_fires_cache_invalidate);
    RUN_TEST(test_tensor_free_zero_handle_is_noop);
    RUN_TEST(test_tensor_multi_alloc_distinct_buffers);

    /* Phase 5.3 CCW upload */
    RUN_TEST(test_ccw_upload_rejects_null);
    RUN_TEST(test_ccw_upload_rejects_truncated);
    RUN_TEST(test_ccw_upload_rejects_address_wrap);
    RUN_TEST(test_ccw_upload_empty_info_is_noop);
    RUN_TEST(test_ccw_upload_single_action);
    RUN_TEST(test_ccw_upload_multiple_actions_contiguous);
    RUN_TEST(test_ccw_upload_chunks_large_action);
    RUN_TEST(test_ccw_upload_skips_zero_size_action);
    RUN_TEST(test_ccw_upload_ptr_variant_resolves_from_ccws_base);
    RUN_TEST(test_ccw_upload_ptr_variant_rejects_null_ccws_base);
    RUN_TEST(test_ccw_upload_rejects_action_past_ccws_size);
    RUN_TEST(test_ccw_upload_rejects_action_past_blob_size);

    /* Phase 5.4 VDMA descriptor-list allocator */
    RUN_TEST(test_vdma_alloc_size_rounds_up_to_64k);
    RUN_TEST(test_vdma_alloc_happy_path);
    RUN_TEST(test_vdma_alloc_rejects_non_power_of_two);
    RUN_TEST(test_vdma_alloc_rejects_out_of_range);
    RUN_TEST(test_vdma_alloc_rejects_zero_page_size);
    RUN_TEST(test_vdma_alloc_rejects_null_out);
    RUN_TEST(test_vdma_alloc_nodev_without_platform);
    RUN_TEST(test_vdma_alloc_propagates_nomem);
    RUN_TEST(test_vdma_free_zero_handle_is_noop);
    RUN_TEST(test_vdma_alloc_accepts_min_count);
    RUN_TEST(test_vdma_program_descriptor_encodes_fields);
    RUN_TEST(test_vdma_program_descriptor_masks_low_addr_bits);
    RUN_TEST(test_vdma_program_buffer_one_descriptor);
    RUN_TEST(test_vdma_program_buffer_exact_multiple);
    RUN_TEST(test_vdma_program_buffer_with_residue);
    RUN_TEST(test_vdma_program_buffer_wraps_circular_list);
    RUN_TEST(test_vdma_program_buffer_rejects_overrun);
    RUN_TEST(test_vdma_program_buffer_rejects_null_list);
    RUN_TEST(test_vdma_program_buffer_rejects_zero_size);
    RUN_TEST(test_vdma_channel_start_programs_regs);
    RUN_TEST(test_vdma_channel_start_encodes_address_l);
    RUN_TEST(test_vdma_channel_start_rejects_misaligned_iova);
    RUN_TEST(test_vdma_channel_start_rejects_bad_channel);
    RUN_TEST(test_vdma_channel_start_rejects_null);
    RUN_TEST(test_vdma_channel_stop_writes_abort_pause);
    RUN_TEST(test_vdma_channel_stop_skips_if_already_abort_pause);
    RUN_TEST(test_vdma_submit_and_wait_completes_fast);
    RUN_TEST(test_vdma_submit_and_wait_times_out);
    RUN_TEST(test_vdma_submit_rejects_bad_channel);

    /* Phase 5.4 hailo_infer orchestration */
    RUN_TEST(test_infer_rejects_null_args);
    RUN_TEST(test_infer_rejects_same_channels);
    RUN_TEST(test_infer_rejects_bad_channel);
    RUN_TEST(test_infer_rejects_zero_sizes);
    RUN_TEST(test_infer_rejects_nodev_when_not_running);
    RUN_TEST(test_infer_end_to_end_via_auto_advance);
    RUN_TEST(test_infer_timeout_without_auto_advance);
    RUN_TEST(test_infer_propagates_tensor_alloc_failure);
    RUN_TEST(test_infer_rejects_oversized_output_desc_count);
    RUN_TEST(test_infer_cleanup_on_midflight_failure);

    RUN_TEST(test_vdma_alloc_accepts_max_count);

    /* CONFIG_STREAM (Phase 5.3, #281 tier-3) */
    RUN_TEST(test_control_config_stream_rejects_null);
    RUN_TEST(test_control_config_stream_rejects_bad_channel);
    RUN_TEST(test_control_config_stream_rejects_when_not_running);
    RUN_TEST(test_control_config_stream_input_wire_format);
    RUN_TEST(test_control_config_stream_output_variant);
    RUN_TEST(test_control_config_stream_timeout_no_response);
    RUN_TEST(test_control_config_stream_propagates_fw_error);

    /* Phase 6.2a: inference_device Hailo backend */
#ifdef CONFIG_AI_SCHEDULER
    RUN_TEST(test_ai_policy_hailo_set_and_get_handle);
    RUN_TEST(test_ai_policy_hailo_clear_detaches_model);
    RUN_TEST(test_ai_policy_hailo_set_from_raw_happy_path);
    RUN_TEST(test_ai_policy_hailo_set_from_raw_rejects_zero_scale);
    RUN_TEST(test_ai_policy_hailo_set_from_raw_rejects_nan_scale);
#endif
    RUN_TEST(test_inf_hailo_register_succeeds);
    RUN_TEST(test_inf_hailo_load_rejects_null);
    RUN_TEST(test_inf_hailo_load_rejects_when_not_running);
    RUN_TEST(test_inf_hailo_load_rejects_bad_header);
    RUN_TEST(test_inf_hailo_load_rejects_zero_pads);
    RUN_TEST(test_inf_hailo_load_happy_path);
    RUN_TEST(test_inf_hailo_load_fills_slots_until_full);
    RUN_TEST(test_inf_hailo_run_rejects_bad_handle);
    RUN_TEST(test_inf_hailo_run_rejects_wrong_dtype);
    RUN_TEST(test_inf_hailo_run_rejects_size_mismatch);
    RUN_TEST(test_inf_hailo_run_happy_path_via_auto_advance);
    RUN_TEST(test_inf_hailo_free_releases_slot);
    RUN_TEST(test_inf_hailo_shutdown_clears_slots);

    /* Phase 6.2b: edge-layer quant + stream extraction */
    RUN_TEST(test_hef_parser_captures_edge_layer_quant);
    RUN_TEST(test_hef_parser_edge_layer_creates_pad_when_ops_empty);
    RUN_TEST(test_hef_parser_edge_layer_backfills_shape_on_shapeless_pad);
    RUN_TEST(test_hef_parser_edge_layer_uses_sys_index_when_pad_index_absent);
    RUN_TEST(test_hef_parser_edge_layer_direction_1_means_output);
    RUN_TEST(test_inf_hailo_load_with_no_ccw_actions_succeeds);
    RUN_TEST(test_inf_hailo_load_threads_hef_stream_info);

    return UnityEnd();
}
