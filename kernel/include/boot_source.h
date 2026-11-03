/*
 * boot_source.h - How SLM-OS was entered (#26).
 *
 * Jetson can start SLM-OS two ways:
 *   - kexec from Linux: Linux has already powered, clocked and
 *     configured peripherals (XUSB, PCIe, GPU channels, camera RCE),
 *     and several drivers inherit that state.
 *   - UEFI-direct: the firmware launches slmos.bin as an EFI
 *     application. Nothing beyond what UEFI needed for itself has been
 *     initialised, and UEFI shuts its own devices down in
 *     ExitBootServices.
 *
 * Drivers that depend on Linux-inherited state must not touch their
 * hardware after a UEFI-direct boot unless they first bring it up
 * themselves (xhci_init does, via xusb_t234_cold.c, #1084): on Tegra234 an access to an
 * unpowered block is answered with an external abort that BL31 turns
 * into "Unhandled Exception in EL3", stopping the board. The power and
 * clock state can't be asked of BPMP instead — its MRQ_PG / MRQ_CLK
 * state queries report SLM-OS's own requests, not the physical state,
 * and read "off" even for XUSB that Linux left running (measured on
 * jetson-nano-2, 2026-11-03).
 *
 * The record lives in .data, not .bss: efi_stub_entry() writes it
 * before boot.S copies the image (code + .data) to the link address,
 * and primary_cpu clears .bss afterwards.
 */
#ifndef BOOT_SOURCE_H
#define BOOT_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

enum boot_source {
    /* Loaded directly at the link address: QEMU -kernel, Pi 5
     * firmware, kexec from Linux, multiboot2. The default. */
    BOOT_SOURCE_DIRECT = 1,
    /* Launched as a UEFI application (efi_stub_entry ran). */
    BOOT_SOURCE_UEFI   = 2,
};

/* Defined in kernel/src/main.c, initialised to BOOT_SOURCE_DIRECT. */
extern volatile uint32_t slmos_boot_source;

static inline bool boot_source_is_uefi(void)
{
    return slmos_boot_source == BOOT_SOURCE_UEFI;
}

#endif /* BOOT_SOURCE_H */
