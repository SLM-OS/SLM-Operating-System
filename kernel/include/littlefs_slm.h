/*
 * littlefs_slm.h - LittleFS Wrapper for SLM-OS
 *
 * Provides a simplified interface to LittleFS, bridging it to the
 * block device abstraction layer.
 */

#ifndef LITTLEFS_SLM_H
#define LITTLEFS_SLM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "blkdev.h"

/*
 * Include LittleFS header for type definitions and constants.
 * This provides:
 *   - lfs_t, lfs_file_t, lfs_dir_t types
 *   - LFS_ERR_* error codes
 *   - LFS_O_* open flags
 *   - LFS_TYPE_* file types
 *   - LFS_SEEK_* seek modes
 */
#include "lfs.h"

/* LittleFS configuration for SLM-OS */
#define LFS_SLM_CACHE_SIZE      256    /* Read/program cache size */
#define LFS_SLM_LOOKAHEAD_SIZE  16     /* Lookahead buffer size (in bytes, tracks 128 blocks) */
#define LFS_SLM_BLOCK_CYCLES    500    /* Wear leveling cycles before moving metadata */

/* Maximum open files/directories */
#define LFS_SLM_MAX_FILES       4
#define LFS_SLM_MAX_DIRS        4

/*
 * File/directory info structure (simplified from lfs_info).
 * Uses the same name field size as LFS_NAME_MAX.
 */
struct lfs_entry_info {
    uint8_t type;               /* LFS_TYPE_REG or LFS_TYPE_DIR */
    uint32_t size;              /* File size (only valid for files) */
    char name[LFS_NAME_MAX+1];  /* Null-terminated name */
};

/*
 * LittleFS mount context.
 * Contains all state needed for a mounted filesystem.
 */
struct lfs_mount;  /* Opaque type, defined in implementation */

/*
 * Initialize the LittleFS subsystem.
 * Must be called before any other littlefs_* functions.
 */
void littlefs_init(void);

/*
 * Mount a LittleFS filesystem on a block device.
 *
 * @dev:     Block device to mount
 * @format:  If true, format the device before mounting
 *
 * Returns: Mount context on success, NULL on failure
 */
struct lfs_mount *littlefs_mount(struct blkdev *dev, bool format);

/*
 * Unmount a LittleFS filesystem.
 *
 * @mnt: Mount context from littlefs_mount()
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_unmount(struct lfs_mount *mnt);

/*
 * Format a block device with LittleFS.
 * Device does not need to be mounted.
 *
 * @dev: Block device to format
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_format(struct blkdev *dev);

/*
 * Get filesystem statistics.
 *
 * @mnt:          Mount context
 * @total_blocks: Output: total blocks in filesystem
 * @used_blocks:  Output: blocks currently in use
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_stat(struct lfs_mount *mnt,
                  uint32_t *total_blocks,
                  uint32_t *used_blocks);

/* ---- File Operations ---- */

/*
 * Open a file.
 *
 * @mnt:   Mount context
 * @path:  Path to file (relative to mount root)
 * @flags: Open flags (LFS_O_* values)
 *
 * Returns: File handle (>= 0) on success, negative error code on failure
 */
int littlefs_file_open(struct lfs_mount *mnt, const char *path, int flags);

/*
 * Close a file.
 *
 * @mnt:    Mount context
 * @handle: File handle from littlefs_file_open()
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_file_close(struct lfs_mount *mnt, int handle);

/*
 * Read from a file.
 *
 * @mnt:    Mount context
 * @handle: File handle
 * @buffer: Destination buffer
 * @size:   Maximum bytes to read
 *
 * Returns: Number of bytes read (>= 0), or negative error code
 */
int littlefs_file_read(struct lfs_mount *mnt, int handle,
                       void *buffer, size_t size);

/*
 * Write to a file.
 *
 * @mnt:    Mount context
 * @handle: File handle
 * @buffer: Source buffer
 * @size:   Number of bytes to write
 *
 * Returns: Number of bytes written (>= 0), or negative error code
 */
int littlefs_file_write(struct lfs_mount *mnt, int handle,
                        const void *buffer, size_t size);

/*
 * Seek within a file.
 *
 * @mnt:    Mount context
 * @handle: File handle
 * @offset: Seek offset
 * @whence: LFS_SEEK_SET, LFS_SEEK_CUR, or LFS_SEEK_END
 *
 * Returns: New file position (>= 0), or negative error code
 */
int littlefs_file_seek(struct lfs_mount *mnt, int handle,
                       int32_t offset, int whence);

/*
 * Get file size.
 *
 * @mnt:    Mount context
 * @handle: File handle
 *
 * Returns: File size (>= 0), or negative error code
 */
int littlefs_file_size(struct lfs_mount *mnt, int handle);

/*
 * Sync file to storage.
 *
 * @mnt:    Mount context
 * @handle: File handle
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_file_sync(struct lfs_mount *mnt, int handle);

/*
 * Truncate file to specified size.
 *
 * @mnt:    Mount context
 * @handle: File handle
 * @size:   New file size
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_file_truncate(struct lfs_mount *mnt, int handle, uint32_t size);

/* ---- Directory Operations ---- */

/*
 * Open a directory for reading.
 *
 * @mnt:  Mount context
 * @path: Path to directory (relative to mount root, use "" for root)
 *
 * Returns: Directory handle (>= 0) on success, negative error code on failure
 */
int littlefs_dir_open(struct lfs_mount *mnt, const char *path);

/*
 * Close a directory.
 *
 * @mnt:    Mount context
 * @handle: Directory handle
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_dir_close(struct lfs_mount *mnt, int handle);

/*
 * Read the next entry from a directory.
 *
 * @mnt:    Mount context
 * @handle: Directory handle
 * @info:   Output: entry information
 *
 * Returns: 1 if entry read, 0 if end of directory, negative error code on failure
 */
int littlefs_dir_read(struct lfs_mount *mnt, int handle,
                      struct lfs_entry_info *info);

/*
 * Create a directory.
 *
 * @mnt:  Mount context
 * @path: Path for new directory
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_mkdir(struct lfs_mount *mnt, const char *path);

/*
 * Remove a file or empty directory.
 *
 * @mnt:  Mount context
 * @path: Path to remove
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_remove(struct lfs_mount *mnt, const char *path);

/*
 * Get information about a file or directory.
 *
 * @mnt:  Mount context
 * @path: Path to query
 * @info: Output: entry information
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_stat_path(struct lfs_mount *mnt, const char *path,
                       struct lfs_entry_info *info);

/*
 * Rename a file or directory.
 *
 * @mnt:     Mount context
 * @oldpath: Current path
 * @newpath: New path
 *
 * Returns: LFS_OK on success, negative error code on failure
 */
int littlefs_rename(struct lfs_mount *mnt,
                    const char *oldpath, const char *newpath);

/* ---- Utility Functions ---- */

/*
 * Get the underlying LittleFS handle (for advanced use).
 */
lfs_t *littlefs_get_lfs(struct lfs_mount *mnt);

/*
 * Get the block device for a mount.
 */
struct blkdev *littlefs_get_blkdev(struct lfs_mount *mnt);

#endif /* LITTLEFS_SLM_H */
