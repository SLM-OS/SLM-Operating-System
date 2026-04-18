/*
 * pcie_core.c — Platform-agnostic PCIe enumeration and API layer.
 *
 * All hardware access goes through `struct pcie_host_ops` installed
 * by a platform backend (pcie_qemu_gpex.c / pcie_bcm2712.c / etc.).
 *
 * Responsibilities:
 *   - Scan configured bus range, probe each (bus, dev, func) for a
 *     vendor ID, and record type-0 header fields + BARs.
 *   - Decode BARs: 32/64-bit split, size probe (write 1s, read back),
 *     flag prefetchable vs non-prefetchable.
 *   - Walk the capability list.
 *   - Forward config accesses and MSI allocation through the backend.
 *
 * Deliberately excluded:
 *   - Bridge/type-1 header handling (the AI HAT+ is a single-function
 *     endpoint on bus 1; we don't program PCI-to-PCI bridges, the
 *     root complex firmware already did that).
 *   - ECAM extended config (offsets 0x100-0xFFF). Our current use cases
 *     (MSI, MSI-X, bus-master enable) all live in the legacy 256-byte
 *     region; the backend can still expose the extended window if a
 *     future consumer needs it.
 */

#include "pcie.h"
#include "platform.h"
#include "debug.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Config-space offsets                                                        */
/* -------------------------------------------------------------------------- */

#define CFG_VENDOR_ID          0x00
#define CFG_DEVICE_ID          0x02
#define CFG_COMMAND            0x04
#define CFG_STATUS             0x06
#define CFG_REVISION           0x08
#define CFG_PROG_IF            0x09
#define CFG_SUBCLASS           0x0A
#define CFG_CLASS              0x0B
#define CFG_HEADER_TYPE        0x0E
#define CFG_BAR0               0x10
#define CFG_CAP_POINTER        0x34

#define CMD_IO_SPACE           (1u << 0)
#define CMD_MEMORY_SPACE       (1u << 1)
#define CMD_BUS_MASTER         (1u << 2)

#define STATUS_CAP_LIST        (1u << 4)

#define HEADER_TYPE_MASK       0x7F   /* bit 7 = multi-function */
#define HEADER_TYPE_STANDARD   0x00
#define HEADER_TYPE_BRIDGE     0x01

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

static const struct pcie_host_ops *host_ops;
static struct pcie_device pcie_devices[PCIE_MAX_DEVICES];
static uint32_t pcie_device_count;

int pcie_core_register_host(const struct pcie_host_ops *ops)
{
    /* Validate the full vtable the core is guaranteed to call. A
     * partially-filled table would NULL-deref on first use — better
     * to reject at registration. `alloc_msi` and `bind_irq_handler`
     * are optional (QEMU GPEX stubs them out) so they aren't
     * required here, but every config / BAR / init op is. */
    if (!ops
     || !ops->init
     || !ops->link_up
     || !ops->config_read8
     || !ops->config_read16
     || !ops->config_read32
     || !ops->config_write32
     || !ops->map_bar) {
        return PCIE_ERR_INVAL;
    }
    if (host_ops) {
        WARN("pcie: host_ops already installed ('%s'), replacing with '%s'",
             host_ops->name ? host_ops->name : "(unnamed)",
             ops->name      ? ops->name      : "(unnamed)");
    }
    host_ops = ops;
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* Config-space accessors                                                      */
/* -------------------------------------------------------------------------- */

/* Internal helpers that bypass the device pointer — used during bus
 * scan before a device is in the table. */
static uint16_t cfg_r16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off)
{
    return host_ops->config_read16(bus, dev, func, off);
}

static uint32_t cfg_r32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off)
{
    return host_ops->config_read32(bus, dev, func, off);
}

static void cfg_w32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t off,
                    uint32_t val)
{
    host_ops->config_write32(bus, dev, func, off, val);
}

uint8_t pcie_config_read8(const struct pcie_device *d, uint16_t off)
{
    if (!d || !host_ops) return 0xFF;
    return host_ops->config_read8(d->bus, d->dev, d->func, off);
}

uint16_t pcie_config_read16(const struct pcie_device *d, uint16_t off)
{
    if (!d || !host_ops) return 0xFFFF;
    return host_ops->config_read16(d->bus, d->dev, d->func, off);
}

uint32_t pcie_config_read32(const struct pcie_device *d, uint16_t off)
{
    if (!d || !host_ops) return 0xFFFFFFFFu;
    return host_ops->config_read32(d->bus, d->dev, d->func, off);
}

void pcie_config_write32(const struct pcie_device *d, uint16_t off,
                         uint32_t val)
{
    if (!d || !host_ops) return;
    host_ops->config_write32(d->bus, d->dev, d->func, off, val);
}

/* 8/16-bit writes via read-modify-write on the enclosing dword.
 * PCIe config space is dword-accessible on every controller we care
 * about; byte-granularity writes are a convenience. */
void pcie_config_write16(const struct pcie_device *d, uint16_t off, uint16_t val)
{
    if (!d || !host_ops) return;
    uint16_t aligned = (uint16_t)(off & ~1u);
    uint32_t dword = host_ops->config_read32(d->bus, d->dev, d->func,
                                             (uint16_t)(aligned & ~2u));
    unsigned shift = (aligned & 2u) * 8u;
    dword = (dword & ~(0xFFFFu << shift)) | (((uint32_t)val) << shift);
    host_ops->config_write32(d->bus, d->dev, d->func,
                             (uint16_t)(aligned & ~2u), dword);
}

void pcie_config_write8(const struct pcie_device *d, uint16_t off, uint8_t val)
{
    if (!d || !host_ops) return;
    uint16_t aligned = (uint16_t)(off & ~3u);
    uint32_t dword = host_ops->config_read32(d->bus, d->dev, d->func, aligned);
    unsigned shift = (off & 3u) * 8u;
    dword = (dword & ~(0xFFu << shift)) | (((uint32_t)val) << shift);
    host_ops->config_write32(d->bus, d->dev, d->func, aligned, dword);
}

/* -------------------------------------------------------------------------- */
/* BAR decode                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Probe one BAR slot. Returns the number of BAR slots consumed
 * (1 for 32-bit, 2 for 64-bit). Updates dev->bar[i], bar_size[i],
 * bar_flags[i]. For 64-bit BARs the `i+1` slot is zeroed out.
 *
 * The size probe writes 0xFFFFFFFF, reads back, restores. Command
 * register bits 0-1 (I/O + mem) are cleared during the probe so the
 * BAR isn't briefly mapping all-ones into the platform address map
 * (some controllers fault on that). Real init sequences restore
 * mem-space enable via pcie_enable_bus_master().
 */
static int probe_one_bar(struct pcie_device *dev, int bar_idx)
{
    uint16_t off = (uint16_t)(CFG_BAR0 + bar_idx * 4);

    uint32_t raw_lo = cfg_r32(dev->bus, dev->dev, dev->func, off);

    /*
     * Do the size probe first. A truly absent BAR keeps reading 0
     * after we write 0xFFFFFFFF — the hardware has no BAR register
     * there. A present-but-unprogrammed BAR (no UEFI, no firmware
     * assignment) shows raw_lo=0 but the write sticks and read-back
     * reveals the size. This distinction matters on Pi 5 where the
     * RC comes up with all endpoint BARs at zero.
     *
     * Clear COMMAND bits 0-1 before flailing the BAR so the bridge
     * doesn't briefly decode all-ones into the platform address
     * space (some controllers fault on that).
     */
    uint16_t cmd_before = cfg_r16(dev->bus, dev->dev, dev->func, CFG_COMMAND);
    cfg_w32(dev->bus, dev->dev, dev->func, CFG_COMMAND,
            cmd_before & ~(CMD_IO_SPACE | CMD_MEMORY_SPACE));
    cfg_w32(dev->bus, dev->dev, dev->func, off, 0xFFFFFFFFu);
    uint32_t probe_lo_initial = cfg_r32(dev->bus, dev->dev, dev->func, off);
    cfg_w32(dev->bus, dev->dev, dev->func, off, raw_lo);
    cfg_w32(dev->bus, dev->dev, dev->func, CFG_COMMAND, cmd_before);

    if (probe_lo_initial == 0) {
        return 1;   /* BAR truly doesn't exist — skip */
    }

    /* Type-bits come from the *probe* response (when unprogrammed),
     * or from the raw value (when already programmed). Prefer the
     * probe because raw_lo might be zero. */
    uint32_t type_bits = (raw_lo != 0) ? raw_lo : probe_lo_initial;
    bool is_io = (type_bits & 1u) != 0;
    bool is_64 = !is_io && (((type_bits >> 1) & 0x3) == 0x2);
    bool is_prefetch = !is_io && ((type_bits & (1u << 3)) != 0);

    /*
     * A spec-compliant device cannot advertise a 64-bit BAR in the
     * last slot (there's no adjacent high-half register). Refuse
     * the claim rather than read/clobber config offset 0x28
     * (Cardbus CIS Pointer) or overflow dev->bar[]/bar_size[]/
     * bar_flags[] when the code below writes bar_idx + 1.
     */
    if (is_64 && bar_idx >= PCIE_NUM_BARS - 1) {
        WARN("pcie: %02x:%02x.%x BAR%d claims 64-bit in last slot — "
             "treating as 32-bit", dev->bus, dev->dev, dev->func, bar_idx);
        is_64 = false;
    }

    /* probe_lo_initial was captured above while COMMAND had mem/io
     * bits cleared — reuse it. For 64-bit BARs, still need to probe
     * the high half. */
    uint32_t probe_lo = probe_lo_initial;
    uint32_t probe_hi = 0;
    uint32_t raw_hi = 0;
    if (is_64) {
        /* Same disable-and-probe pattern as the low half above. */
        cfg_w32(dev->bus, dev->dev, dev->func, CFG_COMMAND,
                cmd_before & ~(CMD_IO_SPACE | CMD_MEMORY_SPACE));
        raw_hi = cfg_r32(dev->bus, dev->dev, dev->func, (uint16_t)(off + 4));
        cfg_w32(dev->bus, dev->dev, dev->func, (uint16_t)(off + 4), 0xFFFFFFFFu);
        probe_hi = cfg_r32(dev->bus, dev->dev, dev->func, (uint16_t)(off + 4));
        cfg_w32(dev->bus, dev->dev, dev->func, (uint16_t)(off + 4), raw_hi);
        cfg_w32(dev->bus, dev->dev, dev->func, CFG_COMMAND, cmd_before);
    }

    /* Decode size. Mask off the low type bits, then invert + 1.
     * For 32-bit BARs, clamp the inversion to 32 bits so the size
     * doesn't accidentally include high-half 1 bits. */
    uint32_t mask_lo = is_io ? 0xFFFFFFFCu : 0xFFFFFFF0u;
    uint64_t size_mask = is_64
        ? (((uint64_t)probe_hi << 32) | (probe_lo & mask_lo))
        : (uint64_t)(probe_lo & mask_lo);
    uint64_t size_inv = is_64 ? ~size_mask : ((~size_mask) & 0xFFFFFFFFull);
    uint64_t size = size_mask ? size_inv + 1ull : 0;

    uint64_t addr_lo = raw_lo & mask_lo;
    uint64_t addr = is_64
        ? (((uint64_t)raw_hi << 32) | addr_lo)
        : addr_lo;

    dev->bar[bar_idx]       = addr;
    dev->bar_size[bar_idx]  = size;
    dev->bar_flags[bar_idx] = PCIE_BAR_PRESENT
                            | (is_io       ? PCIE_BAR_IO : 0)
                            | (is_64       ? PCIE_BAR_MEM_64 : 0)
                            | (is_prefetch ? PCIE_BAR_PREFETCHABLE : 0);

    if (is_64) {
        dev->bar[bar_idx + 1]       = 0;
        dev->bar_size[bar_idx + 1]  = 0;
        dev->bar_flags[bar_idx + 1] = 0;
        return 2;
    }
    return 1;
}

static void probe_bars(struct pcie_device *dev)
{
    int i = 0;
    while (i < PCIE_NUM_BARS) {
        i += probe_one_bar(dev, i);
    }
}

/* -------------------------------------------------------------------------- */
/* Bus scan                                                                    */
/* -------------------------------------------------------------------------- */

static void scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint16_t vendor = cfg_r16(bus, dev, func, CFG_VENDOR_ID);
    if (vendor == 0xFFFF || vendor == 0) {
        return;
    }

    if (pcie_device_count >= PCIE_MAX_DEVICES) {
        WARN("pcie: device table full (%u), dropping %02x:%02x.%x",
             PCIE_MAX_DEVICES, bus, dev, func);
        return;
    }

    struct pcie_device *d = &pcie_devices[pcie_device_count];
    memset(d, 0, sizeof(*d));

    d->bus         = bus;
    d->dev         = dev;
    d->func        = func;
    d->vendor_id   = vendor;
    d->device_id   = cfg_r16(bus, dev, func, CFG_DEVICE_ID);
    d->revision    = host_ops->config_read8(bus, dev, func, CFG_REVISION);
    d->prog_if     = host_ops->config_read8(bus, dev, func, CFG_PROG_IF);
    d->subclass    = host_ops->config_read8(bus, dev, func, CFG_SUBCLASS);
    d->class_code  = host_ops->config_read8(bus, dev, func, CFG_CLASS);
    d->header_type = host_ops->config_read8(bus, dev, func, CFG_HEADER_TYPE)
                   & HEADER_TYPE_MASK;

    /*
     * Normally only type-0 headers get BAR probing (bridges have a
     * different layout). On Pi 5 the endpoint's config[0x0E] can
     * read as 0xFF during enumeration (see plan §3 — CRS-adjacent
     * behavior past config[0x07]), which would look like an
     * unknown header type. Probe BARs anyway unless we're sure
     * it's a bridge. probe_one_bar does its own size probe and
     * discards BARs that don't respond, so this is safe.
     */
    if (d->header_type != HEADER_TYPE_BRIDGE) {
        probe_bars(d);
    }

    pcie_device_count++;
}

static void scan_device(uint8_t bus, uint8_t dev)
{
    /* Probe function 0 first. Header-type bit 7 says multi-function. */
    uint16_t vendor = cfg_r16(bus, dev, 0, CFG_VENDOR_ID);
    if (vendor == 0xFFFF || vendor == 0) {
        return;
    }

    uint8_t hdr = host_ops->config_read8(bus, dev, 0, CFG_HEADER_TYPE);
    scan_function(bus, dev, 0);

    if (hdr & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            scan_function(bus, dev, func);
        }
    }
}

static void scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        scan_device(bus, dev);
    }
}

/* -------------------------------------------------------------------------- */
/* Init                                                                        */
/* -------------------------------------------------------------------------- */

/* Each platform's backend provides one of these. pcie_init() calls
 * the right one based on the compile-time PLATFORM. */
extern int pcie_backend_register(void);

int pcie_init(void)
{
    pcie_device_count = 0;

    int rc = pcie_backend_register();
    if (rc != PCIE_OK) {
        INFO("pcie: no backend available on this platform (%d)", rc);
        return rc;
    }

    if (!host_ops) {
        /* backend_register returned OK but didn't install ops */
        return PCIE_ERR_INVAL;
    }

    rc = host_ops->init();
    if (rc != PCIE_OK) {
        INFO("pcie: %s init failed (%d)", host_ops->name, rc);
        return rc;
    }

    if (!host_ops->link_up()) {
        INFO("pcie: %s link down — skipping enumeration", host_ops->name);
        return PCIE_ERR_NOLINK;
    }

    /* Only bus 0 + bus 1 are populated on single-RC, single-device
     * topologies. Scanning bus 0..1 covers: QEMU GPEX (all devices on
     * bus 0), and Pi 5 pcie1 (RC at bus 0 func 0, endpoint at bus 1).
     * A deeper hierarchy would need recursive scanning via type-1
     * bridge secondary/subordinate bus numbers — out of scope. */
    scan_bus(0);
    scan_bus(1);

    /*
     * Resource allocation pass. On systems where firmware pre-assigns
     * BARs (x86 SeaBIOS, some ARM BIOSes), the BARs already have
     * non-zero addresses and this loop is a no-op. On bare-metal Pi 5
     * the RC comes up with every BAR at 0 — we have to program them
     * ourselves from the backend's available outbound window.
     *
     * Simple bump allocator: align each BAR to its own size and
     * assign sequentially from the start of the window. Programming
     * the bridge's MEM_BASE/MEM_LIMIT is left to the backend if it
     * cares; for a single endpoint behind the RC, the defaults from
     * firmware are usually fine because the bridge is transparent.
     */
    if (host_ops->get_mmio_window) {
        uint64_t win_base, win_size;
        int rc2 = host_ops->get_mmio_window(&win_base, &win_size);
        if (rc2 == PCIE_OK && win_size > 0) {
            uint64_t next = win_base;
            uint64_t end  = win_base + win_size;
            for (uint32_t i = 0; i < pcie_device_count; i++) {
                struct pcie_device *d = &pcie_devices[i];
                if (d->bus == 0) continue;  /* don't re-assign the RC */
                for (int b = 0; b < PCIE_NUM_BARS; b++) {
                    if (!(d->bar_flags[b] & PCIE_BAR_PRESENT)) continue;
                    if (d->bar_flags[b] & PCIE_BAR_IO) continue;
                    if (d->bar[b] != 0) continue;  /* already programmed */
                    uint64_t size = d->bar_size[b];
                    if (size == 0 || size > win_size) continue;

                    /* Align to the BAR's natural size. */
                    uint64_t addr = (next + size - 1) & ~(size - 1);
                    if (addr + size > end) {
                        WARN("pcie: outbound window exhausted — BAR%d of "
                             "%02x:%02x.%x (size 0x%lx) unassigned",
                             b, d->bus, d->dev, d->func, (unsigned long)size);
                        continue;
                    }

                    /* Write the BAR address back to config space.
                     * 64-bit BARs need a second write at offset+4. */
                    uint16_t off = (uint16_t)(CFG_BAR0 + b * 4);
                    uint32_t lo_bits = host_ops->config_read32(d->bus,
                                                               d->dev, d->func,
                                                               off) & 0xFu;
                    host_ops->config_write32(d->bus, d->dev, d->func, off,
                                             (uint32_t)(addr & 0xFFFFFFF0u)
                                             | lo_bits);
                    if (d->bar_flags[b] & PCIE_BAR_MEM_64) {
                        host_ops->config_write32(d->bus, d->dev, d->func,
                                                 (uint16_t)(off + 4),
                                                 (uint32_t)(addr >> 32));
                    }
                    d->bar[b] = addr;
                    INFO("pcie: %02x:%02x.%x BAR%d assigned 0x%lx (size 0x%lx)",
                         d->bus, d->dev, d->func, b,
                         (unsigned long)addr, (unsigned long)size);
                    next = addr + size;
                }
            }
        }
    }

    INFO("pcie: %s backend, %u device(s) enumerated",
         host_ops->name, pcie_device_count);
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* Lookup                                                                      */
/* -------------------------------------------------------------------------- */

const struct pcie_device *pcie_find_device(uint16_t vendor, uint16_t dev_id)
{
    for (uint32_t i = 0; i < pcie_device_count; i++) {
        if (pcie_devices[i].vendor_id == vendor
         && pcie_devices[i].device_id == dev_id) {
            return &pcie_devices[i];
        }
    }
    return NULL;
}

const struct pcie_device *pcie_find_class(uint8_t class_code, uint8_t subclass)
{
    for (uint32_t i = 0; i < pcie_device_count; i++) {
        if (pcie_devices[i].class_code == class_code
         && pcie_devices[i].subclass == subclass) {
            return &pcie_devices[i];
        }
    }
    return NULL;
}

uint32_t pcie_get_device_count(void) { return pcie_device_count; }

const struct pcie_device *pcie_get_device(uint32_t index)
{
    return (index < pcie_device_count) ? &pcie_devices[index] : NULL;
}

/* -------------------------------------------------------------------------- */
/* Bus master enable                                                           */
/* -------------------------------------------------------------------------- */

int pcie_enable_bus_master(const struct pcie_device *d)
{
    if (!d || !host_ops) return PCIE_ERR_INVAL;
    uint16_t cmd = host_ops->config_read16(d->bus, d->dev, d->func, CFG_COMMAND);
    uint16_t want = cmd | CMD_MEMORY_SPACE | CMD_BUS_MASTER;
    if (cmd == want) return PCIE_OK;

    uint32_t status_cmd = host_ops->config_read32(d->bus, d->dev, d->func,
                                                  CFG_COMMAND);
    status_cmd = (status_cmd & 0xFFFF0000u) | want;
    host_ops->config_write32(d->bus, d->dev, d->func, CFG_COMMAND, status_cmd);
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* BAR mapping                                                                 */
/* -------------------------------------------------------------------------- */

void *pcie_map_bar(const struct pcie_device *d, uint8_t bar_num,
                   uint64_t *out_size)
{
    if (!d || bar_num >= PCIE_NUM_BARS || !host_ops || !host_ops->map_bar) {
        return NULL;
    }
    if (!(d->bar_flags[bar_num] & PCIE_BAR_PRESENT)) {
        return NULL;
    }
    if (d->bar_flags[bar_num] & PCIE_BAR_IO) {
        /* I/O BARs aren't meaningfully mappable on ARM64 PCIe. */
        return NULL;
    }
    if (out_size) *out_size = d->bar_size[bar_num];
    return host_ops->map_bar(d->bar[bar_num], d->bar_size[bar_num]);
}

/* -------------------------------------------------------------------------- */
/* Capability walk                                                             */
/* -------------------------------------------------------------------------- */

uint8_t pcie_find_capability(const struct pcie_device *d, uint8_t cap_id)
{
    if (!d || !host_ops) return 0;

    uint16_t status = host_ops->config_read16(d->bus, d->dev, d->func,
                                              CFG_STATUS);
    if (!(status & STATUS_CAP_LIST)) return 0;

    uint8_t ptr = host_ops->config_read8(d->bus, d->dev, d->func,
                                         CFG_CAP_POINTER) & 0xFCu;
    /* Bounded walk — malformed devices can produce a cycle. */
    for (int iter = 0; iter < 48 && ptr != 0; iter++) {
        uint8_t id   = host_ops->config_read8(d->bus, d->dev, d->func, ptr);
        uint8_t next = host_ops->config_read8(d->bus, d->dev, d->func,
                                              (uint16_t)(ptr + 1));
        if (id == cap_id) return ptr;
        ptr = next & 0xFCu;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* MSI / MSI-X allocation                                                      */
/* -------------------------------------------------------------------------- */

int pcie_alloc_msi(const struct pcie_device *d, int count,
                   struct pcie_msi_handle *out)
{
    if (!d || !out || !host_ops || !host_ops->alloc_msi) {
        return PCIE_ERR_INVAL;
    }
    uint8_t cap = pcie_find_capability(d, 0x05);
    if (!cap) return PCIE_ERR_NOCAP;
    return host_ops->alloc_msi(d, cap, /*is_msix=*/false, count, out);
}

int pcie_alloc_msix(const struct pcie_device *d, int count,
                    struct pcie_msi_handle *out)
{
    if (!d || !out || !host_ops || !host_ops->alloc_msi) {
        return PCIE_ERR_INVAL;
    }
    uint8_t cap = pcie_find_capability(d, 0x11);
    if (!cap) return PCIE_ERR_NOCAP;
    return host_ops->alloc_msi(d, cap, /*is_msix=*/true, count, out);
}

int pcie_bind_irq_handler(const struct pcie_msi_handle *h, int vec_idx,
                          void (*handler)(void *), void *ctx)
{
    if (!h || !handler || !host_ops || !host_ops->bind_irq_handler) {
        return PCIE_ERR_INVAL;
    }
    if (vec_idx < 0 || vec_idx >= h->count) {
        return PCIE_ERR_INVAL;
    }
    return host_ops->bind_irq_handler(h, vec_idx, handler, ctx);
}

/* -------------------------------------------------------------------------- */
/* Diagnostics                                                                 */
/* -------------------------------------------------------------------------- */

void pcie_dump_devices(void)
{
    if (!host_ops) {
        INFO("pcie: no backend installed");
        return;
    }
    INFO("pcie: %u device(s) on %s:", pcie_device_count, host_ops->name);
    for (uint32_t i = 0; i < pcie_device_count; i++) {
        const struct pcie_device *d = &pcie_devices[i];
        INFO("  %02x:%02x.%x  vendor=0x%04x device=0x%04x "
             "class=%02x:%02x hdr=%02x",
             d->bus, d->dev, d->func,
             d->vendor_id, d->device_id,
             d->class_code, d->subclass, d->header_type);
        for (int b = 0; b < PCIE_NUM_BARS; b++) {
            if (!(d->bar_flags[b] & PCIE_BAR_PRESENT)) continue;
            INFO("    BAR%d: addr=0x%lx size=0x%lx flags=0x%x", b,
                 (unsigned long)d->bar[b],
                 (unsigned long)d->bar_size[b],
                 d->bar_flags[b]);
        }
    }
}
