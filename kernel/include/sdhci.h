/*
 * sdhci.h — Generic SDHCI / SD-Host-Controller block driver.
 *
 * Stage 3 of the dynamic-kernel-replace plan (#369). Targets the
 * SDHCI v3 protocol layer common to:
 *
 *   - QEMU's `sdhci-pci` device (test target — `-device sdhci-pci
 *     -drive if=sd,format=raw,file=...`).
 *   - BCM2712 EMMC2 on Pi 5 (production target, MMIO at
 *     0x10_00FF_F000 — see linux-bcm2712.dtsi:1188).
 *
 * Scope (per `docs/dynamic-kernel-replace-plan.md` §1):
 *   - Legacy 25 MHz SDR only. No HS200, HS400, CQE, TRIM, tuning.
 *   - PIO data path (no ADMA2). FAT32 cluster sizes at admin-write
 *     speed don't justify DMA setup for the first cut.
 *   - CMD0, CMD2, CMD3, CMD7, CMD8, CMD9, CMD16, CMD17, CMD18,
 *     CMD24, CMD25, CMD55, ACMD41 only.
 *   - Single-thread, no IRQ delivery — every wait is polled.
 *
 * The driver presents itself as a `struct blkdev` (kernel/include/blkdev.h)
 * so the FatFs diskio shim from Stage 2 (#368) plugs in unchanged.
 *
 * Hardware-side quirks deferred to Stage 5 (#371):
 *   - BCM2712 cfginit (sdhci-brcmstb.c:sdhci_brcmstb_cfginit_2712)
 *   - CPRMAN clock-gate enable
 *   - Real-card timing tuning
 * The protocol layer in this header / sdhci.c is hardware-agnostic;
 * the deferred items are Pi-5-specific glue.
 */

#ifndef SDHCI_H
#define SDHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct blkdev;

/*
 * Probe an SDHCI controller exposed at `mmio_base` and bring up the
 * attached card. Returns a `struct blkdev *` on success that the
 * caller can register with `blkdev_register()` or attach directly to
 * FatFs via `fatfs_disk_attach()`.
 *
 * `name` is copied into the blkdev descriptor (≤ BLKDEV_MAX_NAME).
 * `mmio_base` must be the byte address of the SDHCI register window
 * (offset 0x00 = SDMA_SYS_ADDR, 0xFE = HOST_CONTROLLER_VERSION),
 * 4-byte aligned.
 *
 * The returned descriptor is NULL on:
 *   - Soft-reset timeout (controller dead or wrong base address).
 *   - No card present after the bus power-up cycle.
 *   - CMD8 / ACMD41 / CMD2 / CMD3 / CMD9 / CMD7 timeout (card
 *     refused to enter the transfer state).
 *   - PMM exhaustion when allocating the descriptor.
 *
 * Failures are logged via ERROR(); callers can fall back to the
 * ramdisk stub for QEMU-virt-without-SDHCI scenarios.
 */
struct blkdev *sdhci_create(const char *name, uintptr_t mmio_base);

/*
 * Tear down a controller created by `sdhci_create()`. Does NOT
 * power-cycle the card — the next `sdhci_create()` will re-init
 * cleanly via the standard CMD0 path.
 */
void sdhci_destroy(struct blkdev *dev);

/*
 * Convenience: probe the QEMU `sdhci-pci` device through the existing
 * PCIe scaffolding (pcie_core.c) and call `sdhci_create()` against
 * its BAR0. Returns NULL if the device isn't present (e.g., the
 * test was started without `-device sdhci-pci`).
 *
 * Available on every platform that builds the PCIe core (i.e., not
 * x86-64-only). On RASPI5 / JETSON the pcie_core enumerates the
 * board's host controller, which doesn't expose an SDHCI function;
 * this helper returns NULL there. Use `sdhci_create()` directly with
 * the BCM2712 EMMC2 base on Pi 5.
 */
struct blkdev *sdhci_create_qemu_pci(const char *name);

#endif /* SDHCI_H */
