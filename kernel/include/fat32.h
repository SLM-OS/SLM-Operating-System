/*
 * fat32.h — FatFs binding for SLM-OS.
 *
 * Stage 2 of the dynamic-kernel-replace plan (#368): wires the
 * vendored FatFs (kernel/lib/fatfs/) onto a `struct blkdev` so the
 * kernel can read/write the FAT32 boot partition on the SD card.
 *
 * The kernel-facing surface is intentionally tiny — `attach`,
 * `detach`, and `mount` cover everything the admin commands and
 * tests need. Callers that want more (open/read/write/rename/unlink)
 * include FatFs's `ff.h` directly via `kernel/lib/fatfs/ff.h` and
 * use the `f_*` API once a volume is mounted.
 *
 * Single-volume for now. The `FF_MULTI_PARTITION` config in
 * ffconf.h declares volume 0 → physical drive 0, partition 1 — i.e.
 * partition 1 of the attached blkdev's MBR. See
 * docs/pi5-dual-boot-setup.md:29-35 for the lab card layout.
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

struct blkdev;

/*
 * Attach a block device as the backing store for FatFs volume 0.
 * Required before any `f_*` call. Replaces any previously attached
 * device. Pass NULL via `fat32_detach()` to release the binding.
 *
 * The blkdev MUST use 512-byte blocks (the production SD card and
 * the test ramdisk both do; FatFs is configured with
 * FF_MIN_SS = FF_MAX_SS = 512 in ffconf.h).
 *
 * Not thread-safe: FatFs is single-thread (FF_FS_REENTRANT = 0).
 */
void fatfs_disk_attach(struct blkdev *dev);
void fatfs_disk_detach(void);

/*
 * Returns the currently attached blkdev, or NULL if none.
 * Used by the test harness for sanity-check assertions.
 */
struct blkdev *fatfs_disk_current(void);

#endif /* FAT32_H */
