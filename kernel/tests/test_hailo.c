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
#include "../ai_accel/hailo/hailo_internal.h"
#include "../ai_accel/hailo/hailo_tensor.h"
#include "../ai_accel/hailo/hef_parser.h"
#include "../include/md5.h"
#include "../include/uart.h"
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
#define MOCK_SRAM_BASE  0x00000000u
#define MOCK_SRAM_SIZE  0x00100000u   /* 1 MB — covers all Hailo-8 FW targets */

static uint8_t  mock_bar0[MOCK_BAR0_SIZE];
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
#define MOCK_DMA_POOL_SIZE (256u * 1024u)
static alignas(HAILO_TENSOR_DMA_ALIGN) uint8_t mock_dma_pool[MOCK_DMA_POOL_SIZE];
static size_t   mock_dma_next_off;
static bool     mock_dma_force_null;
static uint32_t mock_dma_alloc_calls;
static uint32_t mock_dma_free_calls;
static uint32_t mock_cache_clean_calls;
static uint32_t mock_cache_invalidate_calls;
static size_t   mock_last_cache_clean_size;
static size_t   mock_last_cache_invalidate_size;

static void mock_reset(void)
{
    memset(mock_bar0, 0, sizeof(mock_bar0));
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
    if (!mock_fw_sim_smart_memory_enabled) return false;
    uint32_t req_opcode_be;
    memcpy(&req_opcode_be, mock_last_control_request + 12, 4);

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
        if (v == HAILO_FW_ACCESS_APP_CPU_CONTROL_MASK) {
            mock_control_doorbells++;
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
        hailo_control_upload_ccw(NULL, blob, 0x10000, NULL));
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, NULL, 0x10000, NULL));
}

static void test_ccw_upload_rejects_truncated(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[4] = {0};
    info.ccw_actions_truncated = true;
    TEST_ASSERT_EQUAL_INT(HAILO_ERR_INVAL,
        hailo_control_upload_ccw(&info, blob, 0x10000, NULL));
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
        hailo_control_upload_ccw(&info, blob, 0xFFFFFFFC, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, mock_control_doorbells);
}

static void test_ccw_upload_empty_info_is_noop(void)
{
    control_setup_running();
    struct hef_info info = {0};
    uint8_t blob[4] = {0};
    uint64_t uploaded = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_INT(HAILO_OK,
        hailo_control_upload_ccw(&info, blob, 0x10000, &uploaded));
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
        hailo_control_upload_ccw(&info, blob, 0x100, &uploaded));
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
        hailo_control_upload_ccw(&info, blob, 0x200, &uploaded));
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
        hailo_control_upload_ccw(&info, blob, 0x400, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, mock_control_doorbells);
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
        hailo_control_upload_ccw(&info, blob, 0x300, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, mock_control_doorbells);
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

    return UnityEnd();
}
