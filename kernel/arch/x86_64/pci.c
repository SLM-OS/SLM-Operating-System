/*
 * pci.c - PCI/PCIe enumeration for x86-64
 *
 * Discovers PCI devices using legacy I/O config space access (ports
 * 0xCF8/0xCFC) with optional ECAM (memory-mapped) if ACPI MCFG table
 * is available. Enumerates all buses, devices, and functions, storing
 * results for later use by device drivers.
 *
 * Also provides the "pci" shell command to list discovered devices.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "uart.h"
#include "shell.h"
#include "pci.h"

/* ---- I/O Port Access ---- */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- PCI Configuration Space Access ---- */

/* Legacy I/O ports */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

/* Config space register offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_SUBSYS_VENDOR   0x2C
#define PCI_SUBSYS_ID       0x2E
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

/* ECAM base address (from ACPI MCFG, 0 = not available) */
static volatile uint8_t *ecam_base;
static uint8_t ecam_start_bus;
static uint8_t ecam_end_bus;

/*
 * Build a PCI config address for legacy I/O access.
 * Format: [31] enable | [23:16] bus | [15:11] device | [10:8] func | [7:2] reg
 */
static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    return (1U << 31) |
           ((uint32_t)bus << 16) |
           ((uint32_t)(dev & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) |
           ((uint32_t)(reg & 0xFC));
}

/*
 * Read 32 bits from PCI config space.
 * Uses ECAM if available, falls back to legacy I/O.
 */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    if (ecam_base && bus >= ecam_start_bus && bus <= ecam_end_bus) {
        /* ECAM: each function gets 4KB, laid out as bus×256 + dev×8 + func */
        uintptr_t offset = ((uint32_t)(bus - ecam_start_bus) << 20) |
                           ((uint32_t)dev << 15) |
                           ((uint32_t)func << 12) |
                           reg;
        return *(volatile uint32_t *)(ecam_base + offset);
    }

    /* Legacy I/O */
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, reg));
    return inl(PCI_CONFIG_DATA);
}

/*
 * Write 32 bits to PCI config space.
 */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg,
                        uint32_t val)
{
    if (ecam_base && bus >= ecam_start_bus && bus <= ecam_end_bus) {
        uintptr_t offset = ((uint32_t)(bus - ecam_start_bus) << 20) |
                           ((uint32_t)dev << 15) |
                           ((uint32_t)func << 12) |
                           reg;
        *(volatile uint32_t *)(ecam_base + offset) = val;
        return;
    }

    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, reg));
    outl(PCI_CONFIG_DATA, val);
}

/*
 * Read 16 bits from PCI config space.
 */
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    uint32_t val = pci_config_read32(bus, dev, func, reg & 0xFC);
    return (val >> ((reg & 2) * 8)) & 0xFFFF;
}

/*
 * Read 8 bits from PCI config space.
 */
uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg)
{
    uint32_t val = pci_config_read32(bus, dev, func, reg & 0xFC);
    return (val >> ((reg & 3) * 8)) & 0xFF;
}

/* ---- ACPI MCFG Parsing ---- */

/* ---- ACPI MCFG Parsing ---- */

/*
 * MCFG entry structure (PCI Express memory-mapped config space).
 */
struct mcfg_entry {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed));

/* acpi_find_table() provided by acpi.c — must call acpi_init() first */
extern const void *acpi_find_table(const char *sig);

static void pci_init_ecam(void)
{
    const uint8_t *mcfg = (const uint8_t *)acpi_find_table("MCFG");
    if (!mcfg) {
        uart_printf("[PCI] No ACPI MCFG table — using legacy I/O\n");
        return;
    }

    /* MCFG: standard 36-byte SDT header + 8 bytes reserved = 44 bytes before entries */
    uint32_t mcfg_length = *(const uint32_t *)(mcfg + 4);  /* SDT length field */
    const struct mcfg_entry *entries =
        (const struct mcfg_entry *)(mcfg + 44);
    uint32_t count = (mcfg_length - 44) / sizeof(struct mcfg_entry);

    if (count == 0) return;

    /* Use first segment group (typically segment 0) */
    ecam_base = (volatile uint8_t *)(uintptr_t)entries[0].base_address;
    ecam_start_bus = entries[0].start_bus;
    ecam_end_bus = entries[0].end_bus;

    uart_printf("[PCI] ECAM at 0x%lx (bus %u-%u)\n",
                entries[0].base_address, ecam_start_bus, ecam_end_bus);
}

/* ---- PCI Device List ---- */

/* struct pci_device defined in pci.h */
#define PCI_MAX_DEVICES 64

static struct pci_device pci_devices[PCI_MAX_DEVICES];
static uint32_t pci_device_count;

/*
 * Get the class code description string.
 */
static const char *pci_class_name(uint8_t class, uint8_t subclass)
{
    switch (class) {
    case 0x00:
        return subclass == 0x01 ? "VGA Compatible" : "Unclassified";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI Bus";
        case 0x01: return "IDE";
        case 0x05: return "ATA";
        case 0x06: return "SATA";
        case 0x08: return "NVMe";
        default:   return "Mass Storage";
        }
    case 0x02:
        return subclass == 0x00 ? "Ethernet" : "Network";
    case 0x03:
        return subclass == 0x00 ? "VGA" : "Display";
    case 0x04:
        return "Multimedia";
    case 0x05:
        return "Memory";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host Bridge";
        case 0x01: return "ISA Bridge";
        case 0x04: return "PCI-PCI Bridge";
        case 0x80: return "Other Bridge";
        default:   return "Bridge";
        }
    case 0x07:
        return "Serial";
    case 0x08:
        return "System Peripheral";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB";
        case 0x05: return "SMBus";
        default:   return "Serial Bus";
        }
    case 0x0D:
        return "Wireless";
    default:
        return "Unknown";
    }
}

/*
 * Scan a single function for a valid PCI device.
 */
static void pci_scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint16_t vendor = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;  /* No device */

    if (pci_device_count >= PCI_MAX_DEVICES) return;

    struct pci_device *d = &pci_devices[pci_device_count];
    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor_id = vendor;
    d->device_id = pci_config_read16(bus, dev, func, PCI_DEVICE_ID);
    d->class_code = pci_config_read8(bus, dev, func, PCI_CLASS);
    d->subclass = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
    d->prog_if = pci_config_read8(bus, dev, func, PCI_PROG_IF);
    d->header_type = pci_config_read8(bus, dev, func, PCI_HEADER_TYPE);
    d->irq_line = pci_config_read8(bus, dev, func, PCI_INTERRUPT_LINE);
    d->irq_pin = pci_config_read8(bus, dev, func, PCI_INTERRUPT_PIN);

    /* Read BARs (only for Type 0 headers — endpoints, not bridges) */
    uint8_t hdr_type = d->header_type & 0x7F;
    int bar_count = (hdr_type == 0x00) ? 6 : (hdr_type == 0x01) ? 2 : 0;
    for (int i = 0; i < bar_count; i++)
        d->bar[i] = pci_config_read32(bus, dev, func, PCI_BAR0 + i * 4);
    for (int i = bar_count; i < 6; i++)
        d->bar[i] = 0;

    pci_device_count++;
}

/*
 * Scan a single device (all functions if multi-function).
 */
static void pci_scan_device(uint8_t bus, uint8_t dev)
{
    uint16_t vendor = pci_config_read16(bus, dev, 0, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;

    pci_scan_function(bus, dev, 0);

    /* Check if multi-function device (bit 7 of header type) */
    uint8_t hdr = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
    if (hdr & 0x80) {
        for (uint8_t func = 1; func < 8; func++)
            pci_scan_function(bus, dev, func);
    }
}

/*
 * Scan an entire PCI bus.
 */
static void pci_scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; dev++)
        pci_scan_device(bus, dev);
}

/* ---- Public Interface ---- */

/*
 * Initialize PCI subsystem: find ECAM, enumerate all devices.
 */
void pci_init(void)
{
    pci_device_count = 0;

    /* Try to find ECAM base from ACPI MCFG */
    pci_init_ecam();

    /* Check if host bridge exists at 00:00.0 */
    uint16_t vendor = pci_config_read16(0, 0, 0, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) {
        uart_printf("[PCI] No host bridge found — PCI not available\n");
        return;
    }

    /* Check if multi-root (bit 7 of header type at 00:00.0) */
    uint8_t hdr = pci_config_read8(0, 0, 0, PCI_HEADER_TYPE);
    if (hdr & 0x80) {
        /* Multi-root: scan buses starting from each function */
        for (uint8_t func = 0; func < 8; func++) {
            vendor = pci_config_read16(0, 0, func, PCI_VENDOR_ID);
            if (vendor != 0xFFFF)
                pci_scan_bus(func);
        }
    } else {
        /* Single root: scan bus 0, then follow bridges */
        pci_scan_bus(0);
    }

    /* Scan for PCI-PCI bridges and enumerate subordinate buses */
    for (uint32_t i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->class_code == 0x06 && d->subclass == 0x04) {
            /* PCI-PCI bridge — read secondary bus number */
            uint8_t secondary = pci_config_read8(d->bus, d->dev, d->func, 0x19);
            if (secondary > 0 && secondary < 255)
                pci_scan_bus(secondary);
        }
    }

    uart_printf("[PCI] %u devices found\n", pci_device_count);

    /* Print NVIDIA GPUs if found */
    for (uint32_t i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->vendor_id == 0x10DE && d->class_code == 0x03) {
            uart_printf("[PCI] NVIDIA GPU at %02x:%02x.%x (device 0x%04x)\n",
                        d->bus, d->dev, d->func, d->device_id);
            for (int b = 0; b < 6; b++) {
                if (d->bar[b] != 0) {
                    bool is_mmio = !(d->bar[b] & 1);
                    bool is_64bit = is_mmio && ((d->bar[b] >> 1) & 3) == 2;
                    uint64_t addr = d->bar[b] & (is_mmio ? ~0xFUL : ~0x3UL);
                    if (is_64bit && b < 5) {
                        addr |= (uint64_t)d->bar[b + 1] << 32;
                        uart_printf("[PCI]   BAR%d: 0x%lx (64-bit MMIO)\n", b, addr);
                        b++;  /* Skip high 32 bits */
                    } else if (is_mmio) {
                        uart_printf("[PCI]   BAR%d: 0x%lx (32-bit MMIO)\n", b, addr);
                    } else {
                        uart_printf("[PCI]   BAR%d: 0x%lx (I/O)\n", b, addr);
                    }
                }
            }
        }
    }
}

/*
 * Get discovered device count.
 */
uint32_t pci_get_device_count(void)
{
    return pci_device_count;
}

/*
 * Find a device by vendor and device ID.
 * Returns pointer to pci_device or NULL.
 */
const struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (uint32_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id)
            return &pci_devices[i];
    }
    return NULL;
}

/*
 * Find a device by class and subclass.
 * Returns pointer to pci_device or NULL.
 */
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (uint32_t i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass)
            return &pci_devices[i];
    }
    return NULL;
}

/*
 * Get device at index (for enumeration).
 */
const struct pci_device *pci_get_device(uint32_t index)
{
    if (index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

/* ---- Shell Command ---- */

static int cmd_pci(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (pci_device_count == 0) {
        uart_printf("No PCI devices found.\n");
        return 0;
    }

    uart_printf("PCI Devices (%u found):\n", pci_device_count);
    uart_printf("  BDF       Vendor:Device  Class     Description\n");
    uart_printf("  --------  -------------  --------  -----------\n");

    for (uint32_t i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        const char *name = pci_class_name(d->class_code, d->subclass);

        uart_printf("  %02x:%02x.%x   %04x:%04x      %02x:%02x     %s",
                    d->bus, d->dev, d->func,
                    d->vendor_id, d->device_id,
                    d->class_code, d->subclass,
                    name);

        /* Highlight NVIDIA */
        if (d->vendor_id == 0x10DE)
            uart_printf(" [NVIDIA]");

        uart_printf("\n");
    }

    /* Show BARs for VGA/display devices */
    for (uint32_t i = 0; i < pci_device_count; i++) {
        struct pci_device *d = &pci_devices[i];
        if (d->class_code != 0x03) continue;  /* Not a display device */

        uart_printf("\n  %02x:%02x.%x BARs:\n", d->bus, d->dev, d->func);
        for (int b = 0; b < 6; b++) {
            if (d->bar[b] == 0) continue;
            bool is_mmio = !(d->bar[b] & 1);
            bool is_64bit = is_mmio && ((d->bar[b] >> 1) & 3) == 2;
            uint64_t addr = d->bar[b] & (is_mmio ? ~0xFUL : ~0x3UL);
            if (is_64bit && b < 5) {
                addr |= (uint64_t)d->bar[b + 1] << 32;
                uart_printf("    BAR%d: 0x%016lx (64-bit MMIO)\n", b, addr);
                b++;
            } else if (is_mmio) {
                uart_printf("    BAR%d: 0x%08lx (32-bit MMIO)\n", b, (unsigned long)addr);
            } else {
                uart_printf("    BAR%d: 0x%04lx (I/O)\n", b, (unsigned long)addr);
            }
        }
    }

    return 0;
}

static const shell_cmd_t pci_cmd = {
    .name = "pci",
    .handler = cmd_pci,
    .help = "List PCI/PCIe devices",
    .mutates = false,
    .category = SHELL_CAT_HARDWARE,
};

void pci_register_shell_commands(void)
{
    shell_register_command(&pci_cmd);
}

#endif /* PLATFORM_X86_64 */
