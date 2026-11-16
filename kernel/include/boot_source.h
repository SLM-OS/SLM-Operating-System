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

/*
 * Second record of a UEFI-direct boot, for the boot test only (#1231).
 * efi_stub_entry() writes EFI_STUB_MARKER_RAN at the same point as it
 * writes slmos_boot_source; every other entry path leaves the
 * initialiser. test_boot_source_matches_stub_marker checks that the boot
 * source reads UEFI exactly when the marker says the stub ran, so a stray
 * store to either one fails the test on every boot path instead of being
 * mistaken for the path itself. Drivers keep using boot_source_is_uefi().
 *
 * Defined in kernel/arch/arm64/efi_stub.c (a different object from
 * slmos_boot_source in main.c) and kept in .data by its non-zero
 * initialiser, for the reason given above for slmos_boot_source. Exists
 * only where efi_stub.c is built (not Pi 5, not x86-64).
 */
#define EFI_STUB_MARKER_UNSET 0x534c4d4f53444952ull   /* "SLMOSDIR" */
#define EFI_STUB_MARKER_RAN   0x534c4d4f53554546ull   /* "SLMOSUEF" */
extern volatile uint64_t slmos_efi_stub_marker;

#endif /* BOOT_SOURCE_H */
