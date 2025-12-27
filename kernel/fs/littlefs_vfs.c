/*
 * littlefs_vfs.c - LittleFS VFS Adapter for SLM-OS
 *
 * Implements the VFS filesystem operations interface for LittleFS,
 * allowing LittleFS filesystems to be mounted in the VFS namespace.
 */

#include "../include/littlefs_slm.h"
#include "../include/vfs.h"
#include "../include/debug.h"

/* ---- VFS Filesystem Operations ---- */

/*
 * Read file content from LittleFS.
 */
static int lfs_vfs_read(void *ctx, const char *path, char *buf,
                         size_t size, size_t offset)
{
    struct lfs_mount *mnt = ctx;
    if (!mnt || !path || !buf) {
        return -1;
    }

    /* Handle empty path as root (which is a directory, not readable) */
    if (!path[0] || (path[0] == '/' && !path[1])) {
        return -1;  /* Can't read a directory */
    }

    /* Open the file for reading */
    int handle = littlefs_file_open(mnt, path, LFS_O_RDONLY);
    if (handle < 0) {
        return -1;
    }

    /* Seek to offset if needed */
    if (offset > 0) {
        int pos = littlefs_file_seek(mnt, handle, (int32_t)offset, LFS_SEEK_SET);
        if (pos < 0) {
            littlefs_file_close(mnt, handle);
            return -1;
        }
    }

    /* Read the content */
    int bytes_read = littlefs_file_read(mnt, handle, buf, size);

    /* Close the file */
    littlefs_file_close(mnt, handle);

    return bytes_read;
}

/*
 * Read directory entries from LittleFS.
 */
static int lfs_vfs_readdir(void *ctx, const char *path,
                            void (*callback)(const struct vfs_entry_info *info, void *cb_ctx),
                            void *cb_ctx)
{
    struct lfs_mount *mnt = ctx;
    if (!mnt || !callback) {
        return -1;
    }

    /* Handle empty path as root */
    const char *dir_path = (path && path[0]) ? path : "/";

    /* Open the directory */
    int handle = littlefs_dir_open(mnt, dir_path);
    if (handle < 0) {
        return -1;
    }

    /* Read entries */
    struct lfs_entry_info lfs_info;
    struct vfs_entry_info vfs_info;

    while (1) {
        int result = littlefs_dir_read(mnt, handle, &lfs_info);
        if (result < 0) {
            /* Error */
            littlefs_dir_close(mnt, handle);
            return -1;
        }
        if (result == 0) {
            /* End of directory */
            break;
        }

        /* Skip "." and ".." */
        if (lfs_info.name[0] == '.') {
            if (lfs_info.name[1] == '\0') continue;
            if (lfs_info.name[1] == '.' && lfs_info.name[2] == '\0') continue;
        }

        /* Convert to VFS format */
        /* Copy name manually (no strncpy) */
        int i;
        for (i = 0; i < VFS_MAX_NAME - 1 && lfs_info.name[i]; i++) {
            vfs_info.name[i] = lfs_info.name[i];
        }
        vfs_info.name[i] = '\0';

        vfs_info.type = (lfs_info.type == LFS_TYPE_DIR) ? 1 : 0;
        vfs_info.size = lfs_info.size;

        /* Call the callback */
        callback(&vfs_info, cb_ctx);
    }

    /* Close the directory */
    littlefs_dir_close(mnt, handle);

    return 0;
}

/*
 * Get file/directory info from LittleFS.
 */
static int lfs_vfs_stat(void *ctx, const char *path,
                         struct vfs_entry_info *info)
{
    struct lfs_mount *mnt = ctx;
    if (!mnt || !path || !info) {
        return -1;
    }

    /* Handle root specially */
    if (!path[0] || (path[0] == '/' && !path[1])) {
        info->name[0] = '/';
        info->name[1] = '\0';
        info->type = 1;  /* Directory */
        info->size = 0;
        return 0;
    }

    struct lfs_entry_info lfs_info;
    int err = littlefs_stat_path(mnt, path, &lfs_info);
    if (err < 0) {
        return -1;
    }

    /* Copy to VFS format */
    int i;
    for (i = 0; i < VFS_MAX_NAME - 1 && lfs_info.name[i]; i++) {
        info->name[i] = lfs_info.name[i];
    }
    info->name[i] = '\0';

    info->type = (lfs_info.type == LFS_TYPE_DIR) ? 1 : 0;
    info->size = lfs_info.size;

    return 0;
}

/*
 * LittleFS VFS operations structure.
 * Use this when mounting a LittleFS filesystem.
 */
const struct vfs_fs_ops littlefs_vfs_ops = {
    .read = lfs_vfs_read,
    .readdir = lfs_vfs_readdir,
    .stat = lfs_vfs_stat,
};

/*
 * Mount LittleFS at a VFS path.
 *
 * Convenience function that mounts a LittleFS filesystem on a block device
 * at a specified VFS path.
 *
 * @path:   VFS path (e.g., "/mnt/files")
 * @dev:    Block device
 * @format: If true, format the device first
 *
 * Returns: LittleFS mount context, or NULL on failure.
 */
struct lfs_mount *littlefs_mount_at(const char *path,
                                     struct blkdev *dev,
                                     bool format)
{
    /* Mount LittleFS on the block device */
    struct lfs_mount *mnt = littlefs_mount(dev, format);
    if (!mnt) {
        ERROR("littlefs_mount_at: failed to mount LittleFS on %s", dev->name);
        return NULL;
    }

    /* Mount in VFS */
    struct vfs_node *vfs_mnt = vfs_mount(path, &littlefs_vfs_ops, mnt);
    if (!vfs_mnt) {
        ERROR("littlefs_mount_at: failed to mount at VFS path %s", path);
        littlefs_unmount(mnt);
        return NULL;
    }

    INFO("Mounted LittleFS at %s (device: %s)", path, dev->name);
    return mnt;
}
