/*
 * pcie_qemu_gpex.c — QEMU "virt" machine PCIe backend.
 *
 * QEMU's ARM64 virt machine exposes a GPEX (Generic PCIe Express)
 * host bridge. Per the machine's device tree
 * (`qemu-system-aarch64 -machine virt,dumpdtb=...`), the ECAM window
 * lives at **`0x40_10000000`** — in high physical memory, 40 bits up
 * from the low I/O addresses. The misleadingly-named `pcie@10000000`
 * DT node refers to the low MMIO base (`0x10000000`), not the ECAM
 * base. Low MMIO runs 0x10000000-0x3effffff (~750 MB); high MMIO
 * runs 0x80_00000000-0xff_ffffffff (not mapped by SLM-OS today).
 *
 * Older docs and a recent revision of the SLM-OS plan cited
 * `0x3f000000` for ECAM, but that address is the legacy PIO window
 * on QEMU virt and aborts on read.
 *
 * This backend is used by `make test` to exercise enumeration, BAR
 * size probing, and the shared pcie_core bus-walk code without
 * requiring real PCIe hardware. Adding a virtio device to the QEMU
 * command line (e.g. `-device virtio-rng-pci,bus=pcie.0`) is enough
 * to have pcie_init() discover one endpoint on bus 0.
 *
 * MSI / MSI-X allocation is not implemented here — QEMU virt with
 * GICv2 has no ITS, so MSI routing would require an entirely
 * different mechanism. The Phase 1 test doesn't need interrupts;
 * alloc_msi returns PCIE_ERR_UNSUPPORTED and the backend stays
 * enumeration-only.
 */

#include "platform.h"

#if defined(PLATFORM_QEMU_VIRT)

#include "pcie.h"
#include "debug.h"
#include <stdint.h>
#include <stdbool.h>

/* QEMU virt GPEX layout — confirmed by DT dump on qemu 8.x. */
#define QEMU_GPEX_ECAM_BASE   0x4010000000UL
#define QEMU_GPEX_ECAM_SIZE   0x0010000000UL   /* 256 MB = 256 buses × 1 MB */

/* ECAM config-space addressing: (bus << 20) | (dev << 15) | (func << 12) | off */
static inline volatile uint8_t *cfg_addr(uint8_t bus, uint8_t dev, uint8_t func,
                                         uint16_t offset)
{
    uintptr_t addr = QEMU_GPEX_ECAM_BASE
                   | ((uintptr_t)bus  << 20)
                   | ((uintptr_t)dev  << 15)
                   | ((uintptr_t)func << 12)
                   | (offset & 0xFFFu);
    return (volatile uint8_t *)addr;
}

static int gpex_init(void)
{
    /* ECAM and low MMIO are mapped statically by vmm_setup_platform()
     * for QEMU_VIRT (see kernel/mm/vmm.c). Nothing else to do. */
    return PCIE_OK;
}

static bool gpex_link_up(void)
{
    /* QEMU's emulated PCIe is always "up" — no training sequence.
     * Devices attached via `-device ...,bus=pcie.0` are present
     * from reset. */
    return true;
}

static uint32_t gpex_config_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint16_t offset)
{
    volatile uint32_t *p = (volatile uint32_t *)cfg_addr(bus, dev, func,
                                                         (uint16_t)(offset & ~3u));
    return *p;
}

static uint16_t gpex_config_read16(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint16_t offset)
{
    volatile uint16_t *p = (volatile uint16_t *)cfg_addr(bus, dev, func,
                                                         (uint16_t)(offset & ~1u));
    return *p;
}

static uint8_t gpex_config_read8(uint8_t bus, uint8_t dev, uint8_t func,
                                 uint16_t offset)
{
    return *cfg_addr(bus, dev, func, offset);
}

static void gpex_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                                uint16_t offset, uint32_t value)
{
    volatile uint32_t *p = (volatile uint32_t *)cfg_addr(bus, dev, func,
                                                         (uint16_t)(offset & ~3u));
    *p = value;
}

static void *gpex_map_bar(uint64_t pcie_addr, uint64_t size)
{
    (void)size;
    /* GPEX outbound window is identity-mapped into QEMU virt's
     * address space (PCIe addr == CPU phys addr for the low MMIO
     * range). vmm_setup_platform maps this range as Device, so
     * returning the raw PA as a kernel VA is safe. */
    return (void *)(uintptr_t)pcie_addr;
}

static int gpex_alloc_msi(const struct pcie_device *dev, uint8_t cap_ptr,
                          bool is_msix, int count, struct pcie_msi_handle *out)
{
    (void)dev; (void)cap_ptr; (void)is_msix; (void)count; (void)out;
    /* GICv2 has no ITS and QEMU virt's legacy INTx delivery isn't
     * wired up in SLM-OS. The Phase 1 test case doesn't use
     * interrupts — return unsupported so callers handle it cleanly. */
    return PCIE_ERR_UNSUPPORTED;
}

static int gpex_bind_irq_handler(const struct pcie_msi_handle *h, int vec,
                                 void (*handler)(void *), void *ctx)
{
    (void)h; (void)vec; (void)handler; (void)ctx;
    return PCIE_ERR_UNSUPPORTED;
}

static const struct pcie_host_ops gpex_ops = {
    .name             = "qemu-gpex",
    .init             = gpex_init,
    .link_up          = gpex_link_up,
    .config_read8     = gpex_config_read8,
    .config_read16    = gpex_config_read16,
    .config_read32    = gpex_config_read32,
    .config_write32   = gpex_config_write32,
    .map_bar          = gpex_map_bar,
    .alloc_msi        = gpex_alloc_msi,
    .bind_irq_handler = gpex_bind_irq_handler,
};

int pcie_backend_register(void)
{
    return pcie_core_register_host(&gpex_ops);
}

#endif /* PLATFORM_QEMU_VIRT */
