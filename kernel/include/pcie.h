/*
 * pcie.h — Cross-platform PCIe host controller API for SLM-OS.
 *
 * This is a new, ARM64-first PCIe subsystem separate from the
 * x86-64 legacy-I/O driver in kernel/arch/x86_64/pci.c. It exposes
 * one uniform API (`struct pcie_device`, `pcie_*` calls) on top of
 * a small `struct pcie_host_ops` vtable that each backend installs
 * at boot:
 *
 *   pcie_core.c            — shared bus walk, capability walk, BAR
 *                            decode, device lookup table.
 *   pcie_qemu_gpex.c       — QEMU virt GPEX (standard ECAM at
 *                            0x3f000000) — used by `make test`.
 *   pcie_bcm2712.c         — Pi 5 pcie1 root complex (AI HAT+ link)
 *                            with EXT_CFG_INDEX/DATA + MIP1 MSI.
 *   pcie_stub.c            — Jetson / other ARM64: no-op, keeps
 *                            pcie_find_device() returning NULL.
 *
 * The x86-64 PCI subsystem is left alone for now. A future
 * refactor can fold `kernel/arch/x86_64/pci.c` behind this API.
 *
 * Call flow at boot:
 *
 *   pcie_init()            // selects backend per platform, calls
 *                          //   host_ops.init() then scans buses.
 *   dev = pcie_find_device(vendor_id, device_id);
 *   pcie_enable_bus_master(dev);
 *   void *bar0 = pcie_map_bar(dev, 0, &bar0_size);
 *   struct pcie_msi_handle msi;
 *   pcie_alloc_msi(dev, 1, &msi);
 *   pcie_bind_irq_handler(&msi, 0, my_isr, ctx);
 */

#ifndef PCIE_H
#define PCIE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Error codes                                                                */
/* -------------------------------------------------------------------------- */

#define PCIE_OK                 0
#define PCIE_ERR_INVAL         (-1)  /* bad argument */
#define PCIE_ERR_NODEV         (-2)  /* no device found */
#define PCIE_ERR_NOLINK        (-3)  /* root complex link down */
#define PCIE_ERR_UNSUPPORTED   (-4)  /* backend does not implement op */
#define PCIE_ERR_NOMEM         (-5)  /* mapping / allocation failed */
#define PCIE_ERR_NOCAP         (-6)  /* capability not present */
#define PCIE_ERR_NOVEC         (-7)  /* no free MSI/MSI-X vectors */
#define PCIE_ERR_IO            (-8)  /* hardware responded with garbage */
#define PCIE_ERR_TIMEOUT       (-9)  /* poll loop exceeded budget */

/* -------------------------------------------------------------------------- */
/* Device descriptor                                                          */
/* -------------------------------------------------------------------------- */

#define PCIE_NUM_BARS          6
#define PCIE_MAX_DEVICES       32   /* SLM-OS caps enumeration — bump if needed */

/* BAR flags stored in pcie_device.bar_flags[n]. Bits chosen to avoid
 * collision with the raw BAR register contents. */
#define PCIE_BAR_IO            (1u << 0)  /* I/O BAR (ignored on ARM64) */
#define PCIE_BAR_MEM_64        (1u << 1)  /* 64-bit memory BAR */
#define PCIE_BAR_PREFETCHABLE  (1u << 2)  /* prefetchable memory BAR */
#define PCIE_BAR_PRESENT       (1u << 3)  /* non-zero BAR; 0 if unused */

struct pcie_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint8_t  header_type;

    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;

    /* BAR contents after the standard size-probe
     * (write 0xFFFFFFFF, read back, restore). `bar[n]` holds the
     * endpoint-side PCIe physical address; `bar_size[n]` the region
     * size in bytes; `bar_flags[n]` the decoded type bits. */
    uint64_t bar[PCIE_NUM_BARS];
    uint64_t bar_size[PCIE_NUM_BARS];
    uint32_t bar_flags[PCIE_NUM_BARS];
};

/* -------------------------------------------------------------------------- */
/* MSI/MSI-X handle                                                            */
/* -------------------------------------------------------------------------- */

/* Opaque-ish to callers — they only pass it to pcie_bind_irq_handler.
 * The fields are public because test code inspects them. */
struct pcie_msi_handle {
    const struct pcie_device *dev;
    uint32_t first_irq;      /* GIC/IDT vector of handle[0] */
    uint16_t count;          /* number of vectors allocated */
    uint16_t is_msix;        /* 1 = MSI-X, 0 = plain MSI */
    uint8_t  cap_ptr;        /* config-space offset of the (MSI|MSI-X) cap */
};

/* -------------------------------------------------------------------------- */
/* Backend vtable (pcie_host_ops)                                              */
/* -------------------------------------------------------------------------- */

/*
 * Each backend fills this in and passes it to pcie_core_register_host()
 * from its own init routine. pcie_core then uses these ops for every
 * config-space access and MSI allocation — the shared enumeration
 * code never touches hardware directly.
 *
 * All offsets are in the target endpoint's config space (0..4095).
 * Widths: read8/16/32 return the value; write32 is the only write
 * primitive exposed (8/16-bit writes happen via RMW in pcie_core).
 */
struct pcie_host_ops {
    const char *name;

    /* Called first; returns PCIE_OK or a negative error. After
     * success, pcie_core will call config_read8/16/32 freely. */
    int (*init)(void);

    /* True if the root-complex link is trained and endpoints are
     * reachable. pcie_core calls this before every bus scan. */
    bool (*link_up)(void);

    /* Config-space accessors. `offset` is < 4096 (extended config
     * supported on backends that map the full 4 KB per function). */
    uint8_t  (*config_read8) (uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
    uint16_t (*config_read16)(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
    uint32_t (*config_read32)(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
    void     (*config_write32)(uint8_t bus, uint8_t dev, uint8_t func,
                               uint16_t offset, uint32_t value);

    /*
     * Translate a PCIe-side endpoint address (BAR contents) into a
     * CPU-virtual address mapped as Device-nGnRnE. `size` is the
     * region size from the standard BAR size-probe, always 4 KB aligned.
     *
     * On QEMU GPEX this is an identity-ish mapping (ECAM is already in
     * the kernel VA); on BCM2712 `pcie1` the outbound window shifts
     * PCIe `0x00_80000000+` → CPU phys `0x1b_80000000+`. Backends are
     * responsible for calling the VMM to install a Device mapping
     * (they'll typically share a 2 MB block for all of a device's BARs).
     *
     * Returns NULL on mapping failure.
     */
    void *(*map_bar)(uint64_t pcie_addr, uint64_t size);

    /*
     * Return the PCIe-side non-prefetchable MMIO window this backend
     * manages. `*base_out` is the low end (PCIe address), `*size_out`
     * is the window size. pcie_core uses this to assign BAR
     * addresses to endpoints whose BARs came up unprogrammed
     * (the "no UEFI resource allocator" case on bare-metal Pi 5).
     *
     * Optional — backends that inherit programmed BARs from firmware
     * (x86 with SeaBIOS) or that run enumeration-only tests (QEMU
     * GPEX without a device) can leave this NULL, in which case
     * pcie_core skips BAR assignment.
     */
    int (*get_mmio_window)(uint64_t *base_out, uint64_t *size_out);

    /*
     * Allocate `count` consecutive MSI (or MSI-X) vectors from the
     * backend's IRQ pool and program the endpoint's message
     * address/data fields to route writes into the backend's MSI
     * peripheral.
     *
     * `is_msix`: 1 if the caller already located an MSI-X capability
     * (table/PBA lives in a BAR); 0 for plain MSI (config-space-only).
     * Caller provides `cap_ptr` so the backend can patch the capability
     * register block without re-walking.
     *
     * On success, `out->first_irq` is the lowest GIC/IDT vector in the
     * allocated range, and vectors at `first_irq+i` belong to
     * handle[i]. On failure returns PCIE_ERR_NOVEC or similar.
     */
    int (*alloc_msi)(const struct pcie_device *dev, uint8_t cap_ptr,
                     bool is_msix, int count, struct pcie_msi_handle *out);

    /*
     * Bind a C handler to the IRQ at `handle->first_irq + vec_idx`.
     * Implementation plumbs through the platform IRQ layer
     * (gic_register_handler on ARM64, irq_register on x86). Once
     * bound, the backend must unmask the vector so writes to the
     * endpoint's MSI table fire the handler.
     */
    int (*bind_irq_handler)(const struct pcie_msi_handle *handle,
                            int vec_idx, void (*handler)(void *), void *ctx);
};

/*
 * Install the active backend. Called once from pcie_init() via the
 * platform's pcie_backend_register(). NOT a public API for device
 * drivers — published here only so backend files
 * (kernel/drivers/pcie/pcie_*.c) and unit tests can link to it
 * without an ad-hoc extern in every caller. Device drivers using
 * this subsystem should call the `pcie_*` functions below instead.
 */
int pcie_core_register_host(const struct pcie_host_ops *ops);

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Initialise the PCIe subsystem: select the platform backend, call
 * its init, and scan all reachable buses. Safe to call once from
 * platform init. Returns PCIE_OK even when no devices are found —
 * only a backend init failure or a link-down RC returns non-zero.
 */
int pcie_init(void);

/* Lookup and enumeration — all return const because the device
 * table is owned by pcie_core. */

const struct pcie_device *pcie_find_device(uint16_t vendor_id,
                                           uint16_t device_id);
const struct pcie_device *pcie_find_class(uint8_t class_code,
                                          uint8_t subclass);
uint32_t                  pcie_get_device_count(void);
const struct pcie_device *pcie_get_device(uint32_t index);

/* Config-space accessors — thin wrappers over the backend. These
 * exist so drivers don't need the host_ops pointer; the active
 * backend is looked up from pcie_core on each call. */

uint8_t  pcie_config_read8 (const struct pcie_device *dev, uint16_t offset);
uint16_t pcie_config_read16(const struct pcie_device *dev, uint16_t offset);
uint32_t pcie_config_read32(const struct pcie_device *dev, uint16_t offset);
void     pcie_config_write8 (const struct pcie_device *dev, uint16_t offset,
                             uint8_t  value);
void     pcie_config_write16(const struct pcie_device *dev, uint16_t offset,
                             uint16_t value);
void     pcie_config_write32(const struct pcie_device *dev, uint16_t offset,
                             uint32_t value);

/*
 * Set the Memory Space Enable + Bus Master Enable bits in COMMAND.
 * Required before the endpoint can DMA or respond to memory reads.
 * Returns PCIE_OK or PCIE_ERR_INVAL.
 */
int pcie_enable_bus_master(const struct pcie_device *dev);

/*
 * Map a BAR region as Device-nGnRnE memory and return a CPU VA.
 * `out_size` receives the mapped region's size (must be non-NULL).
 * Returns NULL if the BAR is absent, an I/O BAR, or the mapping
 * failed.
 */
void *pcie_map_bar(const struct pcie_device *dev, uint8_t bar_num,
                   uint64_t *out_size);

/*
 * Walk the capability list and return the config-space offset of
 * the first capability with `cap_id` (e.g. 0x05 = MSI, 0x11 = MSI-X).
 * Returns 0 if not present — 0 is never a valid capability pointer
 * (cap pointers start at 0x40).
 */
uint8_t pcie_find_capability(const struct pcie_device *dev, uint8_t cap_id);

/*
 * Allocate `count` plain-MSI vectors. The endpoint must expose an
 * MSI capability (cap id 0x05). `count` must be 1, 2, 4, 8, 16, or
 * 32 — MSI multi-vector is encoded as a power of two in the MSI
 * control register.
 */
int pcie_alloc_msi(const struct pcie_device *dev, int count,
                   struct pcie_msi_handle *out);

/*
 * Allocate `count` MSI-X vectors. The endpoint must expose an MSI-X
 * capability (cap id 0x11); `count` <= the table size reported by
 * the capability's message_control field.
 */
int pcie_alloc_msix(const struct pcie_device *dev, int count,
                    struct pcie_msi_handle *out);

/*
 * Bind `handler` to vector `vec_idx` of an existing MSI/MSI-X handle.
 * `ctx` is passed back to the handler on every invocation. Returns
 * PCIE_OK or a negative error.
 */
int pcie_bind_irq_handler(const struct pcie_msi_handle *handle,
                          int vec_idx,
                          void (*handler)(void *), void *ctx);

/*
 * Diagnostics — prints a one-line summary per device. Intended for
 * the `pcie` shell command; also useful in tests.
 */
void pcie_dump_devices(void);

#endif /* PCIE_H */
