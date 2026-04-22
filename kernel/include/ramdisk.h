/*
 * ramdisk.h - RAM Disk Block Device for SLM-OS
 *
 * Provides a RAM-backed block device for testing filesystems.
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include "blkdev.h"

/* Default RAM disk configuration.
 * Bumped 2026-04-21 from 256 blocks (1 MB) to 8192 blocks (32 MB) so
 * a single Hailo HEF file (18-20 MB for ResNet-18 Hailo-8L) plus the
 * embedded demo scripts (~30 KB) and preload.conf can fit without
 * silently truncating in littlefs_file_write. Pi 5 has 8 GB RAM so
 * the 31 MB cost is negligible; QEMU defaults to 1 GB which still
 * leaves plenty. Revisit if HEFs ever grow past ~30 MB. */
#define RAMDISK_DEFAULT_BLOCK_SIZE   4096     /* 4 KB blocks */
#define RAMDISK_DEFAULT_BLOCK_COUNT  8192     /* 32 MB total */

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
