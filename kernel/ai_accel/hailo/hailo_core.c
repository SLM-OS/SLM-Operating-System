/*
 * hailo_core.c — platform-independent Hailo-8/8L bringup.
 *
 * Everything platform-specific (BAR mapping, DMA allocation, IRQ
 * routing, cache ops) is reached through `hailo_platform_ops`.
 * This file only manipulates offsets and state — it never
 * dereferences an MMIO pointer directly.
 *
 * Current scope:
 *   - hailo_init() validates the ops table; resets state to UNINIT.
 *   - hailo_probe() reads vendor ID from BAR0 and, as a liveness
 *     check, reads the boot_status register through the ATR[0]
 *     window into device SRAM.
 *   - hailo_validate_firmware() parses the outer firmware header.
 *   - hailo_boot() uploads the app + core firmware sections via
 *     ATR[0]+BAR4, writes the trigger doorbell, and polls ATR[1]
 *     for the FW-loaded handshake.
 *   - Control-channel RPC / VDMA / inference are Phase 5 stubs
 *     returning HAILO_ERR_UNSUPPORTED.
 *
 * State machine:
 *   UNINIT --init--> UNINIT (ops installed)
 *   UNINIT --probe--> PROBED
 *   PROBED --boot--> FIRMWARE_ARMED --> BOOTING --> RUNNING
 *   * --failure--> FAILED
 */

#include "hailo.h"
#include "hailo_internal.h"
#include "debug.h"
#include "spinlock.h"
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

/*
 * Driver state machine. Transitions are single-threaded by design:
 * hailo_init and hailo_probe run from the main-kernel boot path on
 * CPU 0, and hailo_boot runs from the shell (also CPU 0).
 * Readers on other CPUs (e.g. `hailo` shell command from a future
 * per-CPU shell) get a best-effort snapshot — the value is a small
 * enum so the read is atomic on ARM64. If Phase 5 introduces a
 * writer off the boot path, extend `atr0_lock` to cover the state
 * transitions at lines ~194 and ~251 rather than adding a second
 * lock (keeps lock ordering trivial).
 */
static enum hailo_state state = HAILO_STATE_UNINIT;

/*
 * ATR[0] is shared with device firmware post-boot
 * (docs/reference/hailo-driver-notes.md §9.7). Every dev_read /
 * dev_write through the ATR[0] window must save → retarget →
 * access → restore atomically, or concurrent accesses (including
 * firmware traffic after hailo_boot) corrupt the control channel
 * silently. Guard the whole save/access/restore sequence under a
 * single IRQ-disable spinlock.
 */
static spinlock_t atr0_lock = SPINLOCK_INIT;

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
                            HAILO_ATR_PARAM(0));
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
    hailo_platform->mb();
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
    hailo_platform->mb();
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

    uint32_t page = dev_addr & ~(HAILO_ATR_TABLE_SIZE - 1u);
    uint32_t off  = dev_addr &  (HAILO_ATR_TABLE_SIZE - 1u);
    if (off + n > HAILO_ATR_TABLE_SIZE) return HAILO_ERR_INVAL;

    /*
     * IRQ-disable spinlock across the whole save/retarget/read/
     * restore sequence. Required because firmware (post-boot) also
     * uses ATR[0] for its own traffic — a preempt between our
     * save() and restore() could land firmware writes into the
     * wrong device-side page, or drop ours on the floor. Even
     * single-CPU, an IRQ-driven dev_read would corrupt.
     */
    irq_flags_t flags = spin_lock_irqsave(&atr0_lock);

    uint32_t saved_lo, saved_hi;
    atr0_save(&saved_lo, &saved_hi);
    atr0_set_target(page);
    hailo_platform->bar4_read(off, dst, n);
    atr0_restore(saved_lo, saved_hi);

    spin_unlock_irqrestore(&atr0_lock, flags);
    return HAILO_OK;
}

/* Read a single 32-bit device register via the ATR[0] window. */
static int dev_read32(uint32_t dev_addr, uint32_t *out)
{
    return dev_read(dev_addr, out, sizeof(uint32_t));
}

/*
 * Write `n` bytes to a device-side address through ATR[0]. Same
 * save/retarget/write/restore dance as dev_read — see that function
 * for the lock rationale. `n` must be a multiple of 4 (BAR4 supports
 * only dword-aligned writes; see pi5_bar4_write's alignment check)
 * and fit within a 4 KB ATR window.
 */
static int dev_write(uint32_t dev_addr, const void *src, size_t n)
{
    if (!src || (n & 3u) != 0) return HAILO_ERR_INVAL;
    if (n > HAILO_ATR_TABLE_SIZE) return HAILO_ERR_INVAL;

    uint32_t page = dev_addr & ~(HAILO_ATR_TABLE_SIZE - 1u);
    uint32_t off  = dev_addr &  (HAILO_ATR_TABLE_SIZE - 1u);
    if (off + n > HAILO_ATR_TABLE_SIZE) return HAILO_ERR_INVAL;

    irq_flags_t flags = spin_lock_irqsave(&atr0_lock);
    uint32_t saved_lo, saved_hi;
    atr0_save(&saved_lo, &saved_hi);
    atr0_set_target(page);
    hailo_platform->bar4_write(off, src, n);
    atr0_restore(saved_lo, saved_hi);
    spin_unlock_irqrestore(&atr0_lock, flags);
    return HAILO_OK;
}

/* Write a single 32-bit device register via the ATR[0] window. */
static int dev_write32(uint32_t dev_addr, uint32_t val)
{
    return dev_write(dev_addr, &val, sizeof(val));
}

/*
 * Write `n` bytes to `dev_addr`, chunking into ATR-window-sized
 * pieces. `n` can exceed HAILO_ATR_TABLE_SIZE; dev_write handles one
 * page at a time. First chunk handles head-misalignment so that
 * subsequent chunks are page-aligned. All writes must be dword-sized;
 * caller's payload length must be a multiple of 4.
 *
 * Three entry conditions:
 *   1. dev_addr page-aligned (head_off == 0): head branch is a
 *      no-op; while loop writes full pages then a final short tail.
 *   2. Misaligned dev_addr with `n` spanning a page boundary
 *      (head_off != 0 && remain >= head_room): head branch writes
 *      the first partial page so the while-loop cursor is aligned.
 *   3. Misaligned dev_addr with `n` fitting inside a single page
 *      (head_off != 0 && remain < head_room): head branch skipped,
 *      while loop's single iteration handles the sub-page write
 *      (dev_write's internal bounds check accepts it because
 *      head_off + remain < head_room <= ATR_TABLE_SIZE).
 */
static int dev_write_chunked(uint32_t dev_addr, const void *src, size_t n)
{
    if ((n & 3u) != 0) return HAILO_ERR_INVAL;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t cursor  = dev_addr;
    size_t   remain  = n;

    /* First (possibly partial) page. */
    uint32_t head_off = cursor & (HAILO_ATR_TABLE_SIZE - 1u);
    size_t head_room  = HAILO_ATR_TABLE_SIZE - head_off;
    if (head_off != 0 && remain >= head_room) {
        int rc = dev_write(cursor, p, head_room);
        if (rc != HAILO_OK) return rc;
        cursor += head_room;
        p      += head_room;
        remain -= head_room;
    }

    while (remain > 0) {
        size_t chunk = remain > HAILO_ATR_TABLE_SIZE ? HAILO_ATR_TABLE_SIZE : remain;
        int rc = dev_write(cursor, p, chunk);
        if (rc != HAILO_OK) return rc;
        cursor += chunk;
        p      += chunk;
        remain -= chunk;
    }
    return HAILO_OK;
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
    if (!ops->mb) return false;
    if (!ops->udelay) return false;
    return true;
}

int hailo_init(void)
{
    if (!ops_valid(hailo_platform)) {
        return HAILO_ERR_INVAL;
    }
    /* Fresh start: clear any lingering state from a previous failed
     * session (e.g. a boot that timed out). Post-init the driver is
     * conceptually a blank slate waiting for hailo_probe.
     *
     * Why explicit: hailo_probe refuses when state == FAILED, and
     * hailo_boot requires state == PROBED. Without this reset, a
     * caller that re-runs init after a transient failure would find
     * probe still refusing — "init the device fresh" is the
     * expected semantic, so the state machine is reset here. */
    state = HAILO_STATE_UNINIT;
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
    if (hdr.header_version != HAILO_FW_HEADER_VERSION_V0) {
        INFO("hailo: unsupported firmware header_version %u "
             "(driver only understands v0)", hdr.header_version);
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
/* Boot                                                                        */
/* -------------------------------------------------------------------------- */

/*
 * Read ATR[1]'s trsl_addr_lo. Post-boot, firmware writes
 * HAILO_ATR1_FW_LOADED_MAGIC (0x00200000) here as the "FW loaded"
 * handshake. The register lives in BAR0 at the start of ATR[1],
 * hence HAILO_ATR_BASE + HAILO_ATR_STRIDE.
 */
static uint32_t atr1_read_trsl_lo(void)
{
    uint32_t atr1 = HAILO_ATR_BASE + HAILO_ATR_STRIDE;
    return hailo_platform->read32(HAILO_BAR_CONFIG,
                                  atr1 + HAILO_ATR_OFF_TRSL_ADDR_LO);
}

/*
 * Poll a predicate via a shared udelay/retry loop. Returns HAILO_OK
 * if the predicate turned true within `total_us`, HAILO_ERR_TIMEOUT
 * otherwise. The platform's udelay is used (CNTPCT-backed on Pi 5),
 * so this is safe before the scheduler is running.
 */
static int hailo_poll(bool (*pred)(void), uint32_t interval_us, uint32_t total_us)
{
    uint32_t elapsed = 0;
    while (elapsed < total_us) {
        if (pred()) return HAILO_OK;
        hailo_platform->udelay(interval_us);
        elapsed += interval_us;
    }
    return pred() ? HAILO_OK : HAILO_ERR_TIMEOUT;
}

/* Poll predicate — captured state lives in module-globals during
 * the short window of a single boot call (which is single-threaded
 * by construction; see the state-variable comment at the top). */
static bool atr1_shows_fw_loaded(void)
{
    return atr1_read_trsl_lo() == HAILO_ATR1_FW_LOADED_MAGIC;
}

/*
 * hailo_decode_cert — see hailo_internal.h for the full contract.
 *
 * Extracted from hailo_boot so negative-path unit tests can drive
 * the decode without stubbing the whole boot state machine.
 */
int hailo_decode_cert(const uint8_t *blob, size_t fw_size,
                      size_t cert_off,
                      struct hailo_fw_cert_header *out_cert,
                      const uint8_t **out_key,
                      const uint8_t **out_content,
                      size_t *out_cert_end)
{
    if (!blob || !out_cert || !out_key || !out_content || !out_cert_end) {
        return HAILO_ERR_INVAL;
    }
    if (cert_off + sizeof(*out_cert) > fw_size) {
        INFO("hailo: firmware missing secure-boot certificate");
        return HAILO_ERR_BAD_FIRMWARE;
    }
    memcpy(out_cert, blob + cert_off, sizeof(*out_cert));

    if (out_cert->key_size == 0
     || out_cert->key_size > HAILO_FW_MAX_CERT_KEY
     || out_cert->content_size == 0
     || out_cert->content_size > HAILO_FW_MAX_CERT_CONTENT
     || (out_cert->key_size & 3u) != 0
     || (out_cert->content_size & 3u) != 0) {
        INFO("hailo: bad cert sizes key=0x%x content=0x%x",
             out_cert->key_size, out_cert->content_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }

    size_t cert_end = cert_off + sizeof(*out_cert)
                    + out_cert->key_size + out_cert->content_size;
    if (cert_end > fw_size) {
        INFO("hailo: firmware truncated (cert needs 0x%lx, have 0x%lx)",
             (unsigned long)cert_end, (unsigned long)fw_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }

    *out_key      = blob + cert_off + sizeof(*out_cert);
    *out_content  = *out_key + out_cert->key_size;
    *out_cert_end = cert_end;
    return HAILO_OK;
}

/*
 * hailo_decode_core_fw — see hailo_internal.h for the full contract.
 *
 * Hailo-8 production firmware always carries a core section after
 * the cert; refusing a blob without one is intentional — the boot
 * ROM would otherwise sit at boot_status=1 forever waiting for
 * core code.
 */
int hailo_decode_core_fw(const uint8_t *blob, size_t fw_size,
                         size_t core_hdr_off,
                         struct hailo_firmware_header *out_core_hdr,
                         const uint8_t **out_core_code)
{
    if (!blob || !out_core_hdr || !out_core_code) {
        return HAILO_ERR_INVAL;
    }
    if (core_hdr_off + sizeof(*out_core_hdr) > fw_size) {
        INFO("hailo: firmware missing core-firmware section");
        return HAILO_ERR_BAD_FIRMWARE;
    }
    memcpy(out_core_hdr, blob + core_hdr_off, sizeof(*out_core_hdr));

    if (out_core_hdr->magic != HAILO_FW_MAGIC_HAILO8) {
        INFO("hailo: core-firmware bad magic 0x%x", out_core_hdr->magic);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    if (out_core_hdr->header_version != HAILO_FW_HEADER_VERSION_V0) {
        INFO("hailo: core-firmware unsupported header_version %u",
             out_core_hdr->header_version);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    if (out_core_hdr->code_size == 0
     || out_core_hdr->code_size > HAILO_FW_MAX_CORE_CODE_SIZE) {
        INFO("hailo: core-firmware code_size 0x%x out of range",
             out_core_hdr->code_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }
    size_t core_end = core_hdr_off + sizeof(*out_core_hdr)
                    + out_core_hdr->code_size;
    if (core_end > fw_size) {
        INFO("hailo: core-firmware truncated (needs 0x%lx, have 0x%lx)",
             (unsigned long)core_end, (unsigned long)fw_size);
        return HAILO_ERR_BAD_FIRMWARE;
    }

    *out_core_code = blob + core_hdr_off + sizeof(*out_core_hdr);
    return HAILO_OK;
}

/*
 * Bring the Hailo device to RUNNING state by uploading firmware and
 * triggering the boot ROM. Protocol distilled from
 * docs/reference/hailo-pcie-common.c hailo_pcie_write_firmware_batch
 * + hailo_trigger_firmware_boot + hailo_pcie_wait_for_firmware:
 *
 *   1. Validate the flat firmware blob (header magic, code_size).
 *   2. Require boot_status == UNINIT (device in ROM, ready to accept FW).
 *   3. Upload the app firmware header to boot_fw_header (0xE0030).
 *   4. Upload the app firmware code to app_fw_code_ram_base (0x60000),
 *      chunked by the 4 KB ATR window.
 *   5. If a secure-boot cert trails the code, upload key+content to
 *      boot_key_cert / boot_cont_cert. Hailo-8 ships a cert in every
 *      production firmware image; refuse to boot without one.
 *   6. Write HAILO_FW_TRIGGER_VALUE to trigger_address (0xE0980).
 *   7. Poll boot_status for the UNINIT→non-UNINIT transition
 *      (10 ms budget, 1 ms interval).
 *   8. Poll ATR[1].trsl_addr_lo for HAILO_ATR1_FW_LOADED_MAGIC
 *      (5 s budget, 50 ms interval — firmware decompress + init
 *      takes up to ~3 s in practice).
 *   9. State → RUNNING.
 *
 * On any failure the state flips to FAILED so subsequent probe/boot
 * calls short-circuit cleanly.
 *
 * The blob layout (after validate):
 *     [firmware_header] [code ...] [cert_header] [key ...] [content ...]
 *
 * Concurrency: each dev_write* call in this function takes and
 * releases atr0_lock independently — there's no single atomic lock
 * held across the whole upload. This is safe because during the
 * PROBED→RUNNING window nothing else touches ATR[0]: boot ROM owns
 * the device side, the control channel isn't open yet, and no MSI
 * handlers are wired. Post-boot firmware begins using ATR[0] for
 * its own traffic — at that point the per-call lock (plus save/
 * retarget/access/restore inside dev_read and dev_write) correctly
 * serializes with firmware use. If a future parallel boot path is
 * introduced (e.g. one driver instance per device on a multi-HAT
 * platform), the scope of atr0_lock would need to widen or become
 * per-device.
 */
int hailo_boot(const void *fw_bytes, size_t fw_size)
{
    if (!hailo_platform || state == HAILO_STATE_FAILED) return HAILO_ERR_NODEV;
    if (state != HAILO_STATE_PROBED) {
        INFO("hailo: boot rejected — state must be PROBED (got %s)",
             hailo_state_str(state));
        return HAILO_ERR_INVAL;
    }

    int rc = hailo_validate_firmware(fw_bytes, fw_size);
    if (rc != HAILO_OK) {
        state = HAILO_STATE_FAILED;
        return rc;
    }

    /* Sanity: device must be sitting in its boot ROM, waiting. */
    uint32_t boot_status = 0;
    rc = dev_read32(hailo_fw_addrs_hailo8.boot_status, &boot_status);
    if (rc != HAILO_OK) {
        state = HAILO_STATE_FAILED;
        return rc;
    }
    if (boot_status != HAILO_BOOT_STATUS_UNINIT) {
        INFO("hailo: boot_status=0x%x (expected UNINIT=0x1) — device not ready",
             boot_status);
        state = HAILO_STATE_FAILED;
        return HAILO_ERR_IO;
    }

    /* Decode the blob. hailo_validate_firmware already bounds-checked
     * [header+code] ⊆ fw_size; the helpers below check cert + core. */
    const uint8_t *blob = (const uint8_t *)fw_bytes;
    struct hailo_firmware_header hdr;
    memcpy(&hdr, blob, sizeof(hdr));
    const uint8_t *code = blob + sizeof(hdr);

    struct hailo_fw_cert_header cert_hdr;
    const uint8_t *key_data, *content_data;
    size_t cert_end;
    rc = hailo_decode_cert(blob, fw_size, sizeof(hdr) + hdr.code_size,
                           &cert_hdr, &key_data, &content_data, &cert_end);
    if (rc != HAILO_OK) {
        state = HAILO_STATE_FAILED;
        return rc;
    }

    struct hailo_firmware_header core_hdr;
    const uint8_t *core_code;
    rc = hailo_decode_core_fw(blob, fw_size, cert_end, &core_hdr, &core_code);
    if (rc != HAILO_OK) {
        state = HAILO_STATE_FAILED;
        return rc;
    }

    state = HAILO_STATE_FIRMWARE_ARMED;

    /* Upload. Order matches Linux's hailo_write_app_firmware
     * followed by hailo_write_core_firmware. */
    rc = dev_write(hailo_fw_addrs_hailo8.boot_fw_header, &hdr, sizeof(hdr));
    if (rc != HAILO_OK) goto fail;

    rc = dev_write_chunked(hailo_fw_addrs_hailo8.app_fw_code_ram_base,
                           code, hdr.code_size);
    if (rc != HAILO_OK) goto fail;

    rc = dev_write_chunked(hailo_fw_addrs_hailo8.boot_key_cert,
                           key_data, cert_hdr.key_size);
    if (rc != HAILO_OK) goto fail;

    rc = dev_write_chunked(hailo_fw_addrs_hailo8.boot_cont_cert,
                           content_data, cert_hdr.content_size);
    if (rc != HAILO_OK) goto fail;

    /* Core firmware: upload code FIRST, then header. Linux does it
     * in this order (hailo_write_core_firmware at pcie-common.c:654)
     * presumably because the boot ROM polls the core header and
     * starts fetching code from core_code_ram_base the moment it
     * sees a valid header — writing header last avoids a race where
     * the ROM reads partially-written code. */
    rc = dev_write_chunked(hailo_fw_addrs_hailo8.core_code_ram_base,
                           core_code, core_hdr.code_size);
    if (rc != HAILO_OK) goto fail;

    rc = dev_write(hailo_fw_addrs_hailo8.core_fw_header,
                   &core_hdr, sizeof(core_hdr));
    if (rc != HAILO_OK) goto fail;

    state = HAILO_STATE_BOOTING;

    /* Trigger: write 1 to trigger_address (doorbell). */
    rc = dev_write32(hailo_fw_addrs_hailo8.trigger_address,
                     HAILO_FW_TRIGGER_VALUE);
    if (rc != HAILO_OK) goto fail;

    /*
     * Wait for firmware-loaded handshake: the on-device bootloader
     * finishes loading our FW and writes HAILO_ATR1_FW_LOADED_MAGIC
     * into ATR[1].trsl_addr_lo. This is the only post-trigger poll
     * Linux runs (see hailo_pcie_wait_for_firmware in
     * pcie-common.c:832). boot_status may stay at 1 during the load
     * because "UNINITIALIZED" in the Hailo enum actually means
     * "boot ROM in ready state", not "device dead" — polling it to
     * transition off 1 was the wrong invariant and caused a
     * spurious timeout on real hardware. 5 s budget, 50 ms interval.
     */
    rc = hailo_poll(atr1_shows_fw_loaded, 50000u, 5000000u);
    if (rc != HAILO_OK) {
        INFO("hailo: ATR[1] never reached FW_LOADED magic (fw image bad?)");
        goto fail;
    }

    state = HAILO_STATE_RUNNING;
    /* firmware_revision is a build/tag ID (real FW 4.23.0 ships with
     * revision=0x20000000), not a semver digit — print it in hex so
     * the value reads as intentional. major/minor are conventional
     * decimals. */
    INFO("hailo: firmware %u.%u rev=0x%08x booted",
         hdr.firmware_major, hdr.firmware_minor, hdr.firmware_revision);
    return HAILO_OK;

fail:
    state = HAILO_STATE_FAILED;
    return rc;
}

int hailo_get_firmware_version(uint32_t *out_major, uint32_t *out_minor,
                               uint32_t *out_revision)
{
    (void)out_major; (void)out_minor; (void)out_revision;
    if (state != HAILO_STATE_RUNNING) return HAILO_ERR_NODEV;
    /* Phase 5: read back via a control-channel message after the
     * firmware has booted. */
    return HAILO_ERR_UNSUPPORTED;
}
