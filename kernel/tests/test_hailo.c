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

static void mock_reset(void)
{
    memset(mock_bar0, 0, sizeof(mock_bar0));
    memset(mock_sram, 0, sizeof(mock_sram));
    mock_atr0_target = 0;
    mock_init_calls  = 0;
    mock_fw_sim_enabled = true;
    mock_fw_sim_set_atr1_magic = true;
    mock_trigger_writes = 0;
}

static int mock_init(void)
{
    mock_init_calls++;
    return HAILO_OK;
}

static uint32_t mock_read32(uint8_t bar, uint32_t offset)
{
    if (bar == HAILO_BAR_CONFIG && offset + 4 <= MOCK_BAR0_SIZE) {
        uint32_t v;
        memcpy(&v, &mock_bar0[offset], sizeof(v));
        return v;
    }
    return 0xFFFFFFFFu;
}

static void mock_write32(uint8_t bar, uint32_t offset, uint32_t value)
{
    if (bar == HAILO_BAR_CONFIG && offset + 4 <= MOCK_BAR0_SIZE) {
        memcpy(&mock_bar0[offset], &value, sizeof(value));
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
    uint64_t sram_off = dev_addr - MOCK_SRAM_BASE;
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

static void mock_bar4_write(uint32_t offset, const void *src, size_t n)
{
    uint64_t dev_addr = mock_atr0_target + offset;
    uint64_t sram_off = dev_addr - MOCK_SRAM_BASE;
    if (sram_off + n > MOCK_SRAM_SIZE) return;
    memcpy(&mock_sram[sram_off], src, n);

    /* Watch for the trigger-address doorbell. */
    if (dev_addr == hailo_fw_addrs_hailo8.trigger_address
     && n >= sizeof(uint32_t)) {
        uint32_t v;
        memcpy(&v, src, sizeof(v));
        if (v == HAILO_FW_TRIGGER_VALUE) {
            mock_trigger_writes++;
            mock_simulate_fw_after_trigger();
        }
    }
}

static void *mock_dma_alloc(size_t size, size_t align, uint64_t *iova_out)
{
    (void)size; (void)align;
    if (iova_out) *iova_out = 0;
    return NULL;        /* not needed for these tests */
}
static void  mock_dma_free(void *ptr, size_t size, size_t align)
{ (void)ptr; (void)size; (void)align; }
static void  mock_cache_clean(const void *a, size_t n) { (void)a; (void)n; }
static void  mock_cache_invalidate(void *a, size_t n)  { (void)a; (void)n; }
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
static size_t build_fw_blob(uint8_t *out, size_t out_cap,
                            uint32_t code_size,
                            uint32_t key_size,
                            uint32_t content_size,
                            uint32_t fw_major, uint32_t fw_minor, uint32_t fw_rev)
{
    /* Hailo-8 production firmware carries BOTH app and core sections.
     * The core has its own header + code appended after the cert
     * content. Tests use a small core (4 bytes) to keep blobs short. */
    const uint32_t core_code_size = 4u;
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

    return UnityEnd();
}
