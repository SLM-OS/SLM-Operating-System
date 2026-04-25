/*
 * kernel_cmd.h — `kernel` admin shell command surface.
 *
 * Stage 4 of the dynamic-kernel-replace plan (#370). Wires the
 * Stage 1 / 2 / 3 building blocks (BCM mailbox tag helpers, FatFs,
 * SDHCI driver) into a top-level `kernel` shell command with
 * subcommands:
 *
 *   kernel status
 *     Show the running kernel's identity (SLMOS_VERSION /
 *     SLMOS_BUILD_STAMP / SLMOS_BUILD_SHA from build_info.h) and
 *     whether a tryboot candidate is staged on the SD card.
 *
 *   kernel stage <vfs-path>
 *     Copy the VFS file at <vfs-path> to the SD card's FAT32
 *     partition 1 as `tryboot.img`, plus a `tryboot.sha` sidecar
 *     containing the SHA-256 of the staged bytes.
 *
 *   kernel activate
 *     Verify a tryboot candidate is staged, arm the firmware
 *     tryboot flag via the BCM mailbox (Stage 1 helpers), then
 *     trigger a system reset. Does not return on real hardware.
 *
 *   kernel promote
 *     Rename `tryboot.img` over `kernel_2712.img` so the candidate
 *     becomes the default kernel. Removes the .sha sidecar.
 *
 *   kernel rollback
 *     Delete `tryboot.img` and `tryboot.sha`. Clears the firmware
 *     tryboot flag in case it was armed.
 *
 * State machine (inferred from FAT file existence):
 *   empty          : no `tryboot.img`
 *   staged         : `tryboot.img` + `tryboot.sha` exist
 *   armed          : same on-disk state as `staged`; the firmware
 *                    tryboot flag is set but not visible to us
 *                    (one-shot, consumed by next boot)
 *   promoted       : kernel_2712.img has been replaced by the
 *                    former tryboot.img; tryboot.img is gone
 *   rolled_back    : same as `empty`
 *
 * Concurrency: the command is registered with `mutates = true` so
 * the shell dispatcher serializes concurrent admin sessions.
 *
 * Block backend: probes via `sdhci_create_qemu_pci()` on QEMU virt
 * (test path) and `sdhci_create_bcm2712()` on Pi 5 (production
 * path). Mounts on each command, unmounts before returning — keeps
 * the volume in a consistent state if the kernel is power-cycled
 * mid-operation.
 */

#ifndef KERNEL_CMD_H
#define KERNEL_CMD_H

/*
 * Register the `kernel` shell command. Must be called once during
 * shell init, before the shell task starts dispatching.
 */
void kernel_cmd_register_shell(void);

#endif /* KERNEL_CMD_H */
