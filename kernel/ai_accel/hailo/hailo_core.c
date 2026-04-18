/*
 * hailo_core.c — platform-independent Hailo-8/8L bringup.
 *
 * Everything platform-specific (BAR mapping, DMA allocation, IRQ
 * routing, cache ops) is reached through `hailo_platform_ops`.
 * This file only manipulates offsets and state — it never
 * dereferences an MMIO pointer directly.
 *
 * Phase 3 scope:
 *   - hailo_init() validates the ops table.
 *   - hailo_probe() reads vendor ID from BAR0 and, as a liveness
 *     check, reads the boot_status register through the ATR[0]
 *     window into device SRAM.
 *   - hailo_validate_firmware() parses the outer firmware header.
 *   - Boot / control-channel / VDMA are stubbed out as
 *     HAILO_ERR_UNSUPPORTED (Phase 4 wires them up).
 *
 * State machine:
 *   UNINIT --init--> UNINIT (ops installed)
 *   UNINIT --probe--> PROBED
 *   PROBED --boot(Phase 4)--> FIRMWARE_ARMED --> BOOTING --> RUNNING
 *   * --failure--> FAILED
 */

#include "hailo.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

const struct hailo_platform_ops *hailo_platform;

const struct hailo_fw_addrs hailo_fw_addrs_hailo8 = {
    .boot_fw_header       = 0x000E0030u,
    .boot_key_cert        = 0x000E0048u,
    .boot_cont_cert       = 0x000E0390u,
    .app_fw_code_ram_base = 0x00060000u,
    .core_code_ram_base   = 0x000C0000u,
    .core_fw_header       = 0x000A0000u,
    .raise_ready_offset   = 0x00001684u,
    .boot_status          = 0x000E0000u,
    .trigger_address      = 0x000E0980u,
};

static enum hailo_state state = HAILO_STATE_UNINIT;

enum hailo_state hailo_get_state(void) { return state; }

const char *hailo_state_str(enum hailo_state s)
{
    switch (s) {
    case HAILO_STATE_UNINIT:         return "uninit";
    case HAILO_STATE_PROBED:         return "probed";
    case HAILO_STATE_FIRMWARE_ARMED: return "firmware-armed";
    case HAILO_STATE_BOOTING:        return "booting";
    case HAILO_STATE_RUNNING:        return "running";
    case HAILO_STATE_FAILED:         return "failed";
    default:                         return "unknown";
    }
}

/* -------------------------------------------------------------------------- */
/* ATR[0] window helper — reads / writes device SRAM via BAR4                 */
/* -------------------------------------------------------------------------- */

/*
 * Reprogram ATR[0] to translate BAR4 accesses into `dev_addr` in
 * device-side address space. Writes go to ATR[0]'s trsl_addr_{lo,hi}
 * registers in BAR0. Caller is responsible for saving and restoring
 * the prior ATR[0] value around any read/write through the window —
 * post-boot, firmware itself uses ATR[0] for its own traffic
 * (docs/reference/hailo-driver-notes.md §9.7).
 */
static void atr0_set_target(uint64_t dev_addr)
{
    uint32_t atr0 = HAILO_ATR_BASE;
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_PARAM,
                            HAILO_ATR_PARAM_VALUE | (0u << 12));
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_SRC, 0u);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_LO,
                            (uint32_t)(dev_addr & 0xFFFFFFFFu));
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_HI,
                            (uint32_t)(dev_addr >> 32));
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_PARAM,
                            HAILO_ATR_TRSL_AXI);
    if (hailo_platform->mb) hailo_platform->mb();
}

/* Read the ATR[0] entry's trsl_addr_{lo,hi} so the caller can save
 * it and restore after a one-off poke. */
static void atr0_save(uint32_t *lo, uint32_t *hi)
{
    uint32_t atr0 = HAILO_ATR_BASE;
    *lo = hailo_platform->read32(HAILO_BAR_CONFIG,
                                 atr0 + HAILO_ATR_OFF_TRSL_ADDR_LO);
    *hi = hailo_platform->read32(HAILO_BAR_CONFIG,
                                 atr0 + HAILO_ATR_OFF_TRSL_ADDR_HI);
}

static void atr0_restore(uint32_t lo, uint32_t hi)
{
    uint32_t atr0 = HAILO_ATR_BASE;
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_LO, lo);
    hailo_platform->write32(HAILO_BAR_CONFIG,
                            atr0 + HAILO_ATR_OFF_TRSL_ADDR_HI, hi);
    if (hailo_platform->mb) hailo_platform->mb();
}

/*
 * Read `n` bytes from a device-side address by pointing ATR[0] at
 * the containing 4 KB page and reading from BAR4 at the page
 * offset. Restores the prior ATR[0] value before returning. `n`
 * must be a multiple of 4 and fit within a 4 KB ATR window.
 */
static int dev_read(uint32_t dev_addr, void *dst, size_t n)
{
    if (!dst || (n & 3u) != 0) return HAILO_ERR_INVAL;
    if (n > HAILO_ATR_TABLE_SIZE) return HAILO_ERR_INVAL;

    uint32_t saved_lo, saved_hi;
    atr0_save(&saved_lo, &saved_hi);

    uint32_t page  = dev_addr & ~(HAILO_ATR_TABLE_SIZE - 1u);
    uint32_t off   = dev_addr &  (HAILO_ATR_TABLE_SIZE - 1u);
    if (off + n > HAILO_ATR_TABLE_SIZE) {
        atr0_restore(saved_lo, saved_hi);
        return HAILO_ERR_INVAL;
    }

    atr0_set_target(page);
    hailo_platform->bar4_read(off, dst, n);

    atr0_restore(saved_lo, saved_hi);
    return HAILO_OK;
}

/* Read a single 32-bit device register via the ATR[0] window. */
static int dev_read32(uint32_t dev_addr, uint32_t *out)
{
    return dev_read(dev_addr, out, sizeof(uint32_t));
}

/* -------------------------------------------------------------------------- */
/* Init + probe                                                                */
/* -------------------------------------------------------------------------- */

static bool ops_valid(const struct hailo_platform_ops *ops)
{
    if (!ops) return false;
    if (!ops->read32 || !ops->write32) return false;
    if (!ops->bar4_write || !ops->bar4_read) return false;
    if (!ops->dma_alloc || !ops->dma_free) return false;
    if (!ops->cache_clean || !ops->cache_invalidate) return false;
    if (!ops->udelay) return false;
    return true;
}

int hailo_init(void)
{
    if (!ops_valid(hailo_platform)) {
        return HAILO_ERR_INVAL;
    }
    if (hailo_platform->init) {
        int rc = hailo_platform->init();
        if (rc != HAILO_OK) {
            state = HAILO_STATE_FAILED;
            return rc;
        }
    }
    /* Deliberately stay in UNINIT until probe is called. The
     * platform may have succeeded in mapping BARs without any
     * device actually being present. */
    INFO("hailo: platform '%s' initialized",
         hailo_platform->name ? hailo_platform->name : "(unnamed)");
    return HAILO_OK;
}

int hailo_probe(uint16_t *out_vendor, uint16_t *out_device)
{
    if (!hailo_platform || state == HAILO_STATE_FAILED) {
        return HAILO_ERR_NODEV;
    }

    /* Config-space vendor ID read — 16-bit at offset 0x0098. The
     * Hailo PLDA bridge mirrors the endpoint's PCIe vendor/device
     * id into BAR0 at fixed offsets, so we can identify the chip
     * before initialising the control channel.
     *
     * The platform's read32 gives us a 32-bit dword; vendor is the
     * low 16 bits, device the high 16 bits. */
    uint32_t id = hailo_platform->read32(HAILO_BAR_CONFIG, HAILO_REG_VENDOR);
    uint16_t vendor = (uint16_t)(id & 0xFFFFu);
    uint16_t device = (uint16_t)(id >> 16);

    /* Always expose what we read, even on error paths, so a caller
     * (shell, test harness) can see what's on the bus. */
    if (out_vendor) *out_vendor = vendor;
    if (out_device) *out_device = device;

    if (vendor == 0xFFFFu || vendor == 0x0000u) {
        /* BAR read returned all-ones or all-zeroes — no device. */
        INFO("hailo: probe found no device (vendor=0x%x)", vendor);
        return HAILO_ERR_NODEV;
    }
    if (vendor != HAILO_PCI_VENDOR_ID) {
        INFO("hailo: unexpected vendor 0x%x on BAR0", vendor);
        return HAILO_ERR_IO;
    }

    /* Liveness: peek at boot_status through ATR[0]. The register
     * reports a monotonic boot phase; any read that succeeds (no
     * abort) proves BAR4 + ATR programming is alive. */
    uint32_t boot_status = 0;
    int rc = dev_read32(hailo_fw_addrs_hailo8.boot_status, &boot_status);
    if (rc != HAILO_OK) {
        WARN("hailo: boot_status read failed (%d)", rc);
        return rc;
    }

    INFO("hailo: vendor=0x%04x device=0x%04x boot_status=0x%08x",
         vendor, device, boot_status);

    state = HAILO_STATE_PROBED;
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Firmware validation                                                         */
/* -------------------------------------------------------------------------- */

int hailo_validate_firmware(const void *fw_bytes, size_t fw_size)
{
    if (!fw_bytes) return HAILO_ERR_INVAL;
    if (fw_size < sizeof(struct hailo_firmware_header)) {
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* The header is little-endian raw u32s (hailo-fw-validation.c's
     * validate_fw_header copies directly into the struct). No
     * endian swap needed on ARM64/x86-64. */
    struct hailo_firmware_header hdr;
    memcpy(&hdr, fw_bytes, sizeof(hdr));

    if (hdr.magic != HAILO_FW_MAGIC_HAILO8) {
        INFO("hailo: bad firmware magic 0x%x (expected 0x%x)",
             hdr.magic, HAILO_FW_MAGIC_HAILO8);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    if (hdr.code_size == 0 || hdr.code_size > HAILO_FW_MAX_CODE_SIZE) {
        INFO("hailo: firmware code_size 0x%x out of range", hdr.code_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* Header + code must fit within the blob. */
    size_t need = sizeof(hdr) + hdr.code_size;
    if (need > fw_size) {
        INFO("hailo: firmware truncated (need 0x%lx, have 0x%lx)",
             (unsigned long)need, (unsigned long)fw_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }

    /* There may be a secure-boot certificate trailing the code, but
     * we don't require it at this level — hailo_boot() will check
     * its presence (the driver always ships one for Hailo-8, but a
     * malformed blob that drops it still parses up to this point). */
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Phase 4 stubs                                                               */
/* -------------------------------------------------------------------------- */

int hailo_boot(const void *fw_bytes, size_t fw_size)
{
    (void)fw_bytes; (void)fw_size;
    /* Implementation lands in Phase 4 once the firmware upload
     * path (ATR[0] window) + boot-status poll + ATR[1] "loaded"
     * flag poll are wired up. State machine:
     *
     *   validate header
     *     -> atr0_set_target(app_fw_code_ram_base)
     *        BAR4 write payload
     *        restore ATR[0]
     *     -> for each firmware section (header, key cert, cont cert)
     *     -> trigger: dev_write32(trigger_address, HAILO_FW_TRIGGER_VALUE)
     *     -> poll boot_status until not UNINITIALIZED (10 ms budget)
     *     -> poll ATR[1].trsl_addr_lo == HAILO_ATR1_FW_LOADED_MAGIC
     *        (5 s budget, 50 ms sleep)
     *     -> state = HAILO_STATE_RUNNING
     */
    return HAILO_ERR_UNSUPPORTED;
}

int hailo_get_firmware_version(uint32_t *out_major, uint32_t *out_minor,
                               uint32_t *out_revision)
{
    (void)out_major; (void)out_minor; (void)out_revision;
    if (state != HAILO_STATE_RUNNING) return HAILO_ERR_NODEV;
    /* Phase 4: read back via a control-channel message after the
     * firmware has booted. */
    return HAILO_ERR_UNSUPPORTED;
}
