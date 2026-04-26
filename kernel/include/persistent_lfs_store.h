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
 * Destroy a device previously returned by persistent_lfs_store_create().
 */
void persistent_lfs_store_destroy(struct blkdev *dev);

#endif /* PERSISTENT_LFS_STORE_H */
