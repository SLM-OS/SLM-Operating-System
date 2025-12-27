/*
 * blkdev.h - Block Device Abstraction for SLM-OS
 *
 * Provides a hardware-independent interface for block storage devices.
 * Designed to support LittleFS and future filesystem implementations.
 */

#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Block device error codes */
#define BLKDEV_OK           0
#define BLKDEV_ERR_IO      -1
#define BLKDEV_ERR_CORRUPT -2
#define BLKDEV_ERR_NOENT   -3
#define BLKDEV_ERR_EXIST   -4
#define BLKDEV_ERR_NOTDIR  -5
#define BLKDEV_ERR_ISDIR   -6
#define BLKDEV_ERR_NOTEMPTY -7
#define BLKDEV_ERR_BADF    -8
#define BLKDEV_ERR_FBIG    -9
#define BLKDEV_ERR_INVAL   -10
#define BLKDEV_ERR_NOSPC   -11
#define BLKDEV_ERR_NOMEM   -12

/* Maximum number of registered block devices */
#define BLKDEV_MAX_DEVICES  8

/* Maximum device name length */
#define BLKDEV_MAX_NAME     32

/* Forward declaration */
struct blkdev;

/*
 * Block device operations structure.
 * Each driver implements these callbacks.
 */
struct blkdev_ops {
    /*
     * Read a region from the block device.
     *
     * @dev:    Block device
     * @block:  Block number
     * @off:    Offset within block
     * @buffer: Destination buffer
     * @size:   Number of bytes to read
     *
     * Returns: BLKDEV_OK on success, negative error code on failure
     */
    int (*read)(struct blkdev *dev, uint32_t block, uint32_t off,
                void *buffer, uint32_t size);

    /*
     * Program (write) a region. Block must be erased first for flash devices.
     *
     * @dev:    Block device
     * @block:  Block number
     * @off:    Offset within block
     * @buffer: Source buffer
     * @size:   Number of bytes to write
     *
     * Returns: BLKDEV_OK on success, negative error code on failure
     */
    int (*prog)(struct blkdev *dev, uint32_t block, uint32_t off,
                const void *buffer, uint32_t size);

    /*
     * Erase a block. Required before programming for flash devices.
     *
     * @dev:   Block device
     * @block: Block number to erase
     *
     * Returns: BLKDEV_OK on success, negative error code on failure
     */
    int (*erase)(struct blkdev *dev, uint32_t block);

    /*
     * Sync any cached writes to storage.
     *
     * @dev: Block device
     *
     * Returns: BLKDEV_OK on success, negative error code on failure
     */
    int (*sync)(struct blkdev *dev);
};

/*
 * Block device descriptor.
 */
struct blkdev {
    char name[BLKDEV_MAX_NAME];  /* Device name (e.g., "ramdisk0") */

    /* Geometry */
    uint32_t read_size;          /* Minimum read size (bytes) */
    uint32_t prog_size;          /* Minimum program size (bytes) */
    uint32_t block_size;         /* Erase block size (bytes) */
    uint32_t block_count;        /* Number of blocks */

    /* Operations */
    const struct blkdev_ops *ops;

    /* Driver-specific data */
    void *priv;

    /* Registration state */
    bool registered;
};

/*
 * Initialize the block device subsystem.
 */
void blkdev_init(void);

/*
 * Register a block device.
 *
 * @dev: Block device descriptor (must remain valid while registered)
 *
 * Returns: BLKDEV_OK on success, negative error code on failure
 */
int blkdev_register(struct blkdev *dev);

/*
 * Unregister a block device.
 *
 * @dev: Block device to unregister
 *
 * Returns: BLKDEV_OK on success, negative error code on failure
 */
int blkdev_unregister(struct blkdev *dev);

/*
 * Find a block device by name.
 *
 * @name: Device name to look up
 *
 * Returns: Block device descriptor, or NULL if not found
 */
struct blkdev *blkdev_find(const char *name);

/*
 * Get the number of registered block devices.
 */
int blkdev_count(void);

/*
 * Get total capacity in bytes.
 */
static inline size_t blkdev_capacity(const struct blkdev *dev)
{
    return (size_t)dev->block_size * dev->block_count;
}

/*
 * Dump block device information (for debugging).
 */
void blkdev_dump_info(const struct blkdev *dev);

#endif /* BLKDEV_H */
