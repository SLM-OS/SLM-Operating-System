/*
 * littlefs_vfs.h - LittleFS VFS Adapter for SLM-OS
 *
 * Provides VFS integration for LittleFS filesystems.
 */

#ifndef LITTLEFS_VFS_H
#define LITTLEFS_VFS_H

#include "vfs.h"
#include "blkdev.h"
#include <stdbool.h>

/* Forward declaration */
struct lfs_mount;

/*
 * VFS filesystem operations for LittleFS.
 * Use this with vfs_mount() to mount a LittleFS filesystem.
 *
 * Example:
 *   struct lfs_mount *lfs = littlefs_mount(dev, true);
 *   vfs_mount("/mnt/files", &littlefs_vfs_ops, lfs);
 */
extern const struct vfs_fs_ops littlefs_vfs_ops;

/*
 * Mount LittleFS at a VFS path (convenience function).
 *
 * Combines littlefs_mount() and vfs_mount() into a single call.
 *
 * @path:   VFS path (e.g., "/mnt/files")
 * @dev:    Block device to mount
 * @format: If true, format the device before mounting
 *
 * Returns: LittleFS mount context on success, NULL on failure.
 *
 * On success, the filesystem is accessible at the specified VFS path.
 * To unmount, call vfs_unmount() on the VFS node, then littlefs_unmount()
 * on the returned mount context.
 */
struct lfs_mount *littlefs_mount_at(const char *path,
                                     struct blkdev *dev,
                                     bool format);

#endif /* LITTLEFS_VFS_H */
