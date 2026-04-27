#ifndef PERSISTENT_LFS_STORE_H
#define PERSISTENT_LFS_STORE_H

#include <stdbool.h>

struct blkdev;

#define PERSISTENT_LFS_STORE_PATH "0:/slmstore/files.lfs"

/*
 * Create a RAM-backed blkdev whose contents are loaded from the boot FAT
 * image file at PERSISTENT_LFS_STORE_PATH and flushed back on sync.
 *
 * If no persistent store is available, returns NULL so callers can fall back
 * to the existing RAM-only /mnt/files path.
 *
 * @name: Device name for diagnostics.
 * @needs_format_out: Set true when the backing image is missing/invalid and
 *                    the caller should format LittleFS on first mount.
 */
struct blkdev *persistent_lfs_store_create(const char *name,
                                           bool *needs_format_out);

/*
 * Reset the in-memory image to an erased state and mark it for a full
 * rewrite on the next sync. Used when a persisted image is readable from
 * FAT but its LittleFS metadata is not mountable.
 */
int persistent_lfs_store_reset(struct blkdev *dev);

/*
 * Temporarily suppress FAT-backed syncs while higher-level code is already
 * holding the boot FAT volume open. Dirty state is preserved until syncs are
 * resumed and the caller explicitly flushes.
 */
void persistent_lfs_store_suspend_sync(struct blkdev *dev);
void persistent_lfs_store_resume_sync(struct blkdev *dev);

/*
 * Destroy a device previously returned by persistent_lfs_store_create().
 */
void persistent_lfs_store_destroy(struct blkdev *dev);

#endif /* PERSISTENT_LFS_STORE_H */
