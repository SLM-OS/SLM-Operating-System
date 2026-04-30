/*
 * ramdisk.h - RAM Disk Block Device for SLM-OS
 *
 * Provides a RAM-backed block device for testing filesystems.
 */

#ifndef RAMDISK_H
#define RAMDISK_H

#include "blkdev.h"
#include "config.h"   /* RAMDISK_DEFAULT_MB per-platform sizing */

/* Default RAM disk configuration.
 *
 * Block size is fixed at 4 KB (matches PMM page size). Block count
 * is derived from the per-platform `RAMDISK_DEFAULT_MB` knob in
 * `<config.h>` — see the comment block there for sizing rationale.
 * Jetson is 1.5 GB to stage a Q4_K_M GGUF (Qwen2.5-1.5B is ~1.0 GB);
 * Pi 5 and QEMU stay at 32 MB for Hailo HEFs and the test budget.
 *
 * Block-count history:
 *   - 256 blocks (1 MB) — original Phase 5 ONNX-MNIST sizing.
 *   - 8192 blocks (32 MB) — bumped 2026-04-21 for Hailo HEFs.
 *   - per-platform (32 / 1536 MB) — bumped 2026-04-30 for SLM GGUFs. */
#define RAMDISK_DEFAULT_BLOCK_SIZE   4096u    /* 4 KB blocks */
#define RAMDISK_DEFAULT_BLOCK_COUNT  ((RAMDISK_DEFAULT_MB) * 256u)  /* 256 blocks per MB */

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
