/*
 * ramdisk.h - RAM Disk Block Device for SLM-OS
 *
 * Provides a RAM-backed block device for testing filesystems.
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include "blkdev.h"

/* Default RAM disk configuration */
#define RAMDISK_DEFAULT_BLOCK_SIZE   4096    /* 4 KB blocks */
#define RAMDISK_DEFAULT_BLOCK_COUNT  256     /* 1 MB total */

/*
 * Create a RAM disk with specified geometry.
 * Memory is allocated from PMM.
 *
 * @name:        Device name (e.g., "ramdisk0")
 * @block_size:  Erase block size in bytes (must be power of 2)
 * @block_count: Number of blocks
 *
 * Returns: Block device descriptor, or NULL on failure.
 */
struct blkdev *ramdisk_create(const char *name,
                              uint32_t block_size,
                              uint32_t block_count);

/*
 * Create a RAM disk with default configuration (1 MB).
 *
 * @name: Device name (e.g., "ramdisk0")
 *
 * Returns: Block device descriptor, or NULL on failure.
 */
struct blkdev *ramdisk_create_default(const char *name);

/*
 * Destroy a RAM disk and free memory.
 *
 * @dev: Block device to destroy
 */
void ramdisk_destroy(struct blkdev *dev);

#endif /* RAMDISK_H */
