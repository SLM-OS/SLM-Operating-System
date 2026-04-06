/*
 * pci.h - PCI/PCIe interface for x86-64
 *
 * Provides PCI config space access, device enumeration, and lookup.
 * Only available on PLATFORM_X86_64.
 */

#ifndef PCI_H
#define PCI_H

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>

/* PCI device descriptor — populated during bus enumeration */
struct pci_device {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint8_t  irq_pin;
    uint32_t bar[6];
};

/* Config space access */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
uint8_t  pci_config_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);
void     pci_config_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);

/* Device lookup */
uint32_t pci_get_device_count(void);
const struct pci_device *pci_get_device(uint32_t index);
const struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id);
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);

/* Initialization (called from main.c) */
void pci_init(void);
void pci_register_shell_commands(void);

#endif /* PLATFORM_X86_64 */
#endif /* PCI_H */
