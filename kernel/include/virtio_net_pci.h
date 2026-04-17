/**
 * VirtIO-Net PCI driver public interface (x86-64).
 *
 * Symmetric counterpart to kernel/include/virtio_net.h for the MMIO
 * driver. The PCI driver is gated on PLATFORM_X86_64 in its .c file;
 * this header is harmless on other platforms but only useful there.
 *
 * Kept small: consumers are `main.c` (registration), `test_net.c`
 * (observability accessors), and the MSI-X IRQ path itself. The
 * driver state remains file-static — no struct exposed here.
 */

#ifndef VIRTIO_NET_PCI_H
#define VIRTIO_NET_PCI_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Register the VirtIO-Net PCI driver with net_driver abstraction.
 * Called from platform init in main.c before net_init().
 */
void virtio_net_pci_register(void);

/**
 * MSI-X interrupt handler.
 *
 * Installed via irq_register() during driver init. Argument is the
 * IDT index (vector - 32) from kernel/arch/x86_64/idt.c's dispatch;
 * unused because we only allocate one vector. Non-static so tests
 * can invoke the handler directly to exercise the drain path
 * without relying on end-to-end MSI-X delivery.
 */
void virtio_net_pci_irq_handler(uint8_t irq);

/**
 * Count of MSI-X interrupts the handler has observed. Bumped on
 * every invocation after the !initialized early-exit. Tests use
 * this to verify the dispatch path is live during traffic.
 */
uint32_t virtio_net_pci_get_irq_count(void);

/**
 * IDT vector the driver allocated for MSI-X, or 0 if MSI-X was not
 * enabled (capability absent, or binding rejected). Tests use this
 * to guard against a silent drift of the vector constant.
 */
uint32_t virtio_net_pci_get_msix_vector(void);

/**
 * Whether MSI-X is currently delivering completions. False means the
 * driver fell back to polled completion via net_poll() — still
 * functional, but higher latency under load.
 */
bool virtio_net_pci_msix_enabled(void);

/**
 * Count of times the stuck-descriptor watchdog's WARN has fired.
 * Once per stall episode (latch resets on the next successful reap).
 * Tests assert this stays at zero on healthy traffic and advances
 * exactly once when a stall is simulated.
 */
uint32_t virtio_net_pci_get_tx_stall_count(void);

/**
 * TEST-ONLY: fire the stuck-descriptor watchdog check with synthetic
 * inputs (no-progress + in-flight pool + elapsed > threshold). Same
 * semantics as virtio_net_test_trigger_watchdog on the MMIO side.
 * Not for driver code.
 */
void virtio_net_pci_test_trigger_watchdog(void);

#endif /* VIRTIO_NET_PCI_H */
