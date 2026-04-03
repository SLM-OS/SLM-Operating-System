/*
 * shell_fs.c - Filesystem commands for SLM-OS shell
 *
 * Commands: ls, cd, pwd, cat, write, mkdir, rm, mv, df, truncate,
 *           append, cp, touch, stat, tree, wc, hexdump, grep, find
 */

#include "shell_internal.h"
#include "uart.h"
#include "vfs.h"
#include "littlefs_slm.h"
#include "string.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * VFS Commands (pwd, cd, ls, cat)
 * ============================================================================ */

/*
 * pwd - Print working directory
 */
int cmd_pwd(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    uart_printf("%s\r\n", shell_cwd);
    return 0;
}

/*
 * cd <path> - Change directory
 */
int cmd_cd(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *path = "/";  /* Default to root */

    if (argc >= 2) {
        path = argv[1];
    }

    /* Resolve the path */
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        uart_puts("cd: path too long\r\n");
        return -1;
    }

    /* Check if path exists and is a directory */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        uart_printf("cd: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* Check if it's a directory or mount point */
    if (node->type != VFS_NODE_DIR && node->type != VFS_NODE_MOUNT) {
        uart_printf("cd: %s: Not a directory\r\n", resolved);
        return -1;
    }

    /* For mount points, we also need to check if subpath is a directory */
    if (node->type == VFS_NODE_MOUNT && subpath && subpath[0] != '\0' &&
        !(subpath[0] == '/' && subpath[1] == '\0')) {
        /* There's a subpath within the mount - verify it's a directory */
        struct vfs_entry_info info;
        if (vfs_stat_path(resolved, &info) < 0) {
            uart_printf("cd: %s: No such file or directory\r\n", resolved);
            return -1;
        }
        if (info.type != 1) {  /* 1 = directory */
            uart_printf("cd: %s: Not a directory\r\n", resolved);
            return -1;
        }
    }

    /* Update cwd */
    strcpy(shell_cwd, resolved);
    return 0;
}

/*
 * Callback for listing directory entries (VFS nodes).
 */
static void ls_print_entry(struct vfs_node *node, void *ctx)
{
    (void)ctx;
    if (node->type == VFS_NODE_DIR || node->type == VFS_NODE_MOUNT) {
        uart_printf("  %s/\r\n", node->name);
    } else {
        uart_printf("  %s\r\n", node->name);
    }
}

/*
 * Callback for listing directory entries (mount point entries).
 */
static void ls_print_mount_entry(const struct vfs_entry_info *info, void *ctx)
{
    (void)ctx;
    if (info->type == 1) {  /* Directory */
        uart_printf("  %s/\r\n", info->name);
    } else {
        uart_printf("  %s  (%lu bytes)\r\n", info->name, (unsigned long)info->size);
    }
}

/*
 * ls [path] - List directory contents
 * Defaults to current working directory if no path given.
 */
int cmd_ls(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";  /* Default to cwd */

    if (argc >= 2) {
        input_path = argv[1];
    }

    /* Resolve path (handles relative paths) */
    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        uart_puts("ls: path too long\r\n");
        return -1;
    }

    /* Try the path-based lookup which handles mount points */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        uart_printf("ls: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* If it's a mount point, use the path-based listing */
    if (node->type == VFS_NODE_MOUNT) {
        uart_printf("%s:\r\n", resolved);
        int err = vfs_list_path(resolved, ls_print_mount_entry, NULL);
        if (err < 0) {
            uart_printf("ls: %s: Failed to read directory\r\n", resolved);
            return -1;
        }
        return 0;
    }

    if (node->type == VFS_NODE_FILE) {
        /* It's a file, just show its name */
        uart_printf("%s\r\n", node->name);
        return 0;
    }

    /* Regular directory */
    uart_printf("%s:\r\n", resolved);
    vfs_list(node, ls_print_entry, NULL);

    return 0;
}

/*
 * cat <path> [offset] [length] - Show file contents
 *
 * With offset and length, reads a portion of the file (useful for large files).
 * Supports relative paths.
 */
int cmd_cat(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: cat <path> [offset] [length]\r\n");
        uart_puts("  Show contents of a file (virtual or from mount).\r\n");
        uart_puts("  Optional offset and length for large files.\r\n");
        uart_puts("  Example: cat /sys/memory\r\n");
        uart_puts("  Example: cat hello.txt  (relative to cwd)\r\n");
        uart_puts("  Example: cat /mnt/files/large.bin 0 1024\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("cat: path too long\r\n");
        return -1;
    }

    size_t offset = 0;
    size_t max_len = 1024;  /* Default max read */

    /* Parse optional offset */
    if (argc >= 3) {
        uint32_t off_val;
        if (shell_parse_uint(argv[2], &off_val) == 0) {
            offset = off_val;
        }
    }

    /* Parse optional length */
    if (argc >= 4) {
        uint32_t len_val;
        if (shell_parse_uint(argv[3], &len_val) == 0) {
            max_len = len_val;
            if (max_len > 4096) max_len = 4096;  /* Cap at 4KB for safety */
        }
    }

    /* Try the path-based lookup which handles mount points */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (!node) {
        uart_printf("cat: %s: No such file or directory\r\n", resolved);
        return -1;
    }

    /* If it's a mount point with subpath, use the path-based read */
    if (node->type == VFS_NODE_MOUNT) {
        /* Allocate buffer based on requested length (up to 4KB) */
        char buf[4096];
        size_t read_size = max_len < sizeof(buf) - 1 ? max_len : sizeof(buf) - 1;

        int len = vfs_read_path(resolved, buf, read_size, offset);
        if (len < 0) {
            uart_printf("cat: %s: Read error or is a directory\r\n", resolved);
            return -1;
        }

        buf[len] = '\0';

        /* Show offset info if using streaming */
        if (offset > 0 || argc >= 4) {
            uart_printf("[offset=%lu, read=%d bytes]\r\n",
                        (unsigned long)offset, len);
        }

        /* Print contents, converting \n to \r\n */
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                uart_putc('\r');
            }
            uart_putc(buf[i]);
        }

        /* Ensure newline at end */
        if (len > 0 && buf[len - 1] != '\n') {
            uart_puts("\r\n");
        }

        return 0;
    }

    if (node->type == VFS_NODE_DIR) {
        uart_printf("cat: %s: Is a directory\r\n", resolved);
        return -1;
    }

    /* Read virtual file contents */
    char buf[1024];
    int len = vfs_read(node, buf, sizeof(buf) - 1);
    if (len < 0) {
        uart_printf("cat: %s: Read error\r\n", resolved);
        return -1;
    }

    buf[len] = '\0';

    /* Print contents, converting \n to \r\n */
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(buf[i]);
    }

    /* Ensure newline at end */
    if (len > 0 && buf[len - 1] != '\n') {
        uart_puts("\r\n");
    }

    return 0;
}

/* ============================================================================
 * Filesystem Write Commands (write, mkdir, rm, mv, df)
 * ============================================================================ */

/*
 * write <path> <content> - Write content to a file
 *
 * Creates or overwrites a file in a mounted filesystem.
 * Supports relative paths.
 */
int cmd_write(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: write <path> <content>\r\n");
        uart_puts("  Write content to a file (creates or overwrites).\r\n");
        uart_puts("  Path must be in a mounted filesystem.\r\n");
        uart_puts("  Example: write test.txt Hello  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("write: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("write: %s: Not a mounted filesystem\r\n", resolved);
        uart_puts("  (Only mounted filesystems support writing)\r\n");
        return -1;
    }

    /* Build content from remaining arguments */
    char content[512];
    int pos = 0;
    for (int i = 2; i < argc && pos < (int)sizeof(content) - 1; i++) {
        /* Add space between arguments */
        if (i > 2 && pos < (int)sizeof(content) - 1) {
            content[pos++] = ' ';
        }
        /* Copy argument */
        const char *p = argv[i];
        while (*p && pos < (int)sizeof(content) - 1) {
            content[pos++] = *p++;
        }
    }
    content[pos] = '\0';

    /* Open file for writing (create + truncate) */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) {
        uart_printf("write: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Write content */
    int written = littlefs_file_write(mnt, fd, content, pos);
    littlefs_file_close(mnt, fd);

    if (written < 0) {
        uart_printf("write: %s: Write failed\r\n", resolved);
        return -1;
    }

    uart_printf("Wrote %d bytes to %s\r\n", written, resolved);
    return 0;
}

/*
 * mkdir <path> - Create a directory
 * Supports relative paths.
 */
int cmd_mkdir(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: mkdir <path>\r\n");
        uart_puts("  Create a directory in a mounted filesystem.\r\n");
        uart_puts("  Example: mkdir subdir  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("mkdir: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("mkdir: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int err = littlefs_mkdir(mnt, subpath);
    if (err < 0) {
        if (err == LFS_ERR_EXIST) {
            uart_printf("mkdir: %s: Already exists\r\n", resolved);
        } else {
            uart_printf("mkdir: %s: Failed (error %d)\r\n", resolved, err);
        }
        return -1;
    }

    uart_printf("Created directory %s\r\n", resolved);
    return 0;
}

/*
 * rm <path> - Remove a file or empty directory
 * Supports relative paths.
 */
int cmd_rm(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: rm <path>\r\n");
        uart_puts("  Remove a file or empty directory.\r\n");
        uart_puts("  Example: rm test.txt  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("rm: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("rm: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int err = littlefs_remove(mnt, subpath);
    if (err < 0) {
        if (err == LFS_ERR_NOENT) {
            uart_printf("rm: %s: No such file or directory\r\n", resolved);
        } else if (err == LFS_ERR_NOTEMPTY) {
            uart_printf("rm: %s: Directory not empty\r\n", resolved);
        } else {
            uart_printf("rm: %s: Failed (error %d)\r\n", resolved, err);
        }
        return -1;
    }

    uart_printf("Removed %s\r\n", resolved);
    return 0;
}

/*
 * mv <src> <dst> - Move/rename a file or directory
 * Supports relative paths.
 */
int cmd_mv(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: mv <source> <dest>\r\n");
        uart_puts("  Move or rename a file/directory.\r\n");
        uart_puts("  Both paths must be in the same filesystem.\r\n");
        uart_puts("  Example: mv old.txt new.txt  (relative to cwd)\r\n");
        return -1;
    }

    char src_resolved[VFS_MAX_PATH];
    char dst_resolved[VFS_MAX_PATH];

    if (shell_resolve_path(argv[1], src_resolved, sizeof(src_resolved)) < 0) {
        uart_puts("mv: source path too long\r\n");
        return -1;
    }
    if (shell_resolve_path(argv[2], dst_resolved, sizeof(dst_resolved)) < 0) {
        uart_puts("mv: destination path too long\r\n");
        return -1;
    }

    /* Get mount contexts for both paths */
    const char *src_subpath = NULL;
    const char *dst_subpath = NULL;
    struct lfs_mount *src_mnt = vfs_get_mount_ctx(src_resolved, &src_subpath);
    struct lfs_mount *dst_mnt = vfs_get_mount_ctx(dst_resolved, &dst_subpath);

    if (!src_mnt) {
        uart_printf("mv: %s: Not a mounted filesystem\r\n", src_resolved);
        return -1;
    }

    if (!dst_mnt) {
        uart_printf("mv: %s: Not a mounted filesystem\r\n", dst_resolved);
        return -1;
    }

    if (src_mnt != dst_mnt) {
        uart_puts("mv: Source and destination must be in the same filesystem\r\n");
        return -1;
    }

    int err = littlefs_rename(src_mnt, src_subpath, dst_subpath);
    if (err < 0) {
        if (err == LFS_ERR_NOENT) {
            uart_printf("mv: %s: No such file or directory\r\n", src_resolved);
        } else {
            uart_printf("mv: Failed (error %d)\r\n", err);
        }
        return -1;
    }

    uart_printf("Moved %s -> %s\r\n", src_resolved, dst_resolved);
    return 0;
}

/*
 * df [path] - Show filesystem statistics
 * Defaults to cwd or /mnt/files if cwd not in a mount.
 * Supports relative paths.
 */
int cmd_df(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";  /* Default to cwd */

    if (argc >= 2) {
        input_path = argv[1];
    }

    /* Resolve path */
    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        uart_puts("df: path too long\r\n");
        return -1;
    }

    /* Get the mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        /* If cwd isn't in a mount, try /mnt/files as fallback */
        if (argc < 2) {
            mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
            if (mnt) {
                strcpy(resolved, "/mnt/files");
            }
        }
        if (!mnt) {
            uart_printf("df: %s: Not a mounted filesystem\r\n", resolved);
            return -1;
        }
    }

    uint32_t total_blocks, used_blocks;
    int err = littlefs_stat(mnt, &total_blocks, &used_blocks);
    if (err < 0) {
        uart_printf("df: Failed to get stats (error %d)\r\n", err);
        return -1;
    }

    /* Get block device info */
    struct blkdev *dev = littlefs_get_blkdev(mnt);
    uint32_t block_size = dev ? dev->block_size : 4096;
    uint32_t free_blocks = total_blocks - used_blocks;

    uint32_t total_kb = (total_blocks * block_size) / 1024;
    uint32_t used_kb = (used_blocks * block_size) / 1024;
    uint32_t free_kb = (free_blocks * block_size) / 1024;
    uint32_t pct_used = total_blocks > 0 ? (used_blocks * 100) / total_blocks : 0;

    uart_puts("Filesystem      Blocks     Used     Free   Use%\r\n");
    uart_printf("%-14s  %6lu   %6lu   %6lu   %3lu%%\r\n",
                resolved, (unsigned long)total_blocks,
                (unsigned long)used_blocks, (unsigned long)free_blocks,
                (unsigned long)pct_used);
    uart_printf("                %5luK   %5luK   %5luK\r\n",
                (unsigned long)total_kb, (unsigned long)used_kb,
                (unsigned long)free_kb);

    return 0;
}

/*
 * truncate <path> <size> - Truncate file to specified size
 * Supports relative paths.
 */
int cmd_truncate(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: truncate <path> <size>\r\n");
        uart_puts("  Truncate or extend file to specified size (in bytes).\r\n");
        uart_puts("  Example: truncate log.txt 0  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("truncate: path too long\r\n");
        return -1;
    }

    /* Parse size */
    uint32_t size;
    if (shell_parse_uint(argv[2], &size) != 0) {
        uart_printf("truncate: Invalid size: %s\r\n", argv[2]);
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("truncate: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Open file for writing */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDWR);
    if (fd < 0) {
        uart_printf("truncate: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Truncate to specified size */
    int err = littlefs_file_truncate(mnt, fd, size);
    littlefs_file_close(mnt, fd);

    if (err < 0) {
        uart_printf("truncate: %s: Failed (error %d)\r\n", resolved, err);
        return -1;
    }

    uart_printf("Truncated %s to %lu bytes\r\n", resolved, (unsigned long)size);
    return 0;
}

/*
 * append <path> <content> - Append content to a file
 *
 * Creates the file if it doesn't exist.
 * Useful for logging. Supports relative paths.
 */
int cmd_append(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: append <path> <content>\r\n");
        uart_puts("  Append content to file (creates if needed).\r\n");
        uart_puts("  Example: append log.txt Entry 1  (relative to cwd)\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("append: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("append: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Build content from remaining arguments */
    char content[512];
    int pos = 0;
    for (int i = 2; i < argc && pos < (int)sizeof(content) - 2; i++) {
        /* Add space between arguments */
        if (i > 2 && pos < (int)sizeof(content) - 2) {
            content[pos++] = ' ';
        }
        /* Copy argument */
        const char *p = argv[i];
        while (*p && pos < (int)sizeof(content) - 2) {
            content[pos++] = *p++;
        }
    }
    /* Add newline for log entries */
    content[pos++] = '\n';
    content[pos] = '\0';

    /* Open file for appending (create if needed) */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND);
    if (fd < 0) {
        uart_printf("append: %s: Failed to open file\r\n", resolved);
        return -1;
    }

    /* Write content */
    int written = littlefs_file_write(mnt, fd, content, pos);
    littlefs_file_close(mnt, fd);

    if (written < 0) {
        uart_printf("append: %s: Write failed\r\n", resolved);
        return -1;
    }

    uart_printf("Appended %d bytes to %s\r\n", written, resolved);
    return 0;
}

/*
 * cp <src> <dst> - Copy a file
 * Supports relative paths. Cross-mount copy is supported.
 */
int cmd_cp(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: cp <source> <dest>\r\n");
        uart_puts("  Copy a file. Cross-mount copy is supported.\r\n");
        uart_puts("  Example: cp hello.txt backup.txt\r\n");
        return -1;
    }

    char src_resolved[VFS_MAX_PATH];
    char dst_resolved[VFS_MAX_PATH];

    if (shell_resolve_path(argv[1], src_resolved, sizeof(src_resolved)) < 0) {
        uart_puts("cp: source path too long\r\n");
        return -1;
    }
    if (shell_resolve_path(argv[2], dst_resolved, sizeof(dst_resolved)) < 0) {
        uart_puts("cp: destination path too long\r\n");
        return -1;
    }

    /* Get mount contexts for both paths */
    const char *src_subpath = NULL;
    const char *dst_subpath = NULL;
    struct lfs_mount *src_mnt = vfs_get_mount_ctx(src_resolved, &src_subpath);
    struct lfs_mount *dst_mnt = vfs_get_mount_ctx(dst_resolved, &dst_subpath);

    if (!src_mnt) {
        uart_printf("cp: %s: Not a mounted filesystem\r\n", src_resolved);
        return -1;
    }

    if (!dst_mnt) {
        uart_printf("cp: %s: Not a mounted filesystem\r\n", dst_resolved);
        return -1;
    }

    /* Open source for reading */
    int src_fd = littlefs_file_open(src_mnt, src_subpath, LFS_O_RDONLY);
    if (src_fd < 0) {
        uart_printf("cp: %s: Cannot open source file\r\n", src_resolved);
        return -1;
    }

    /* Get source file size */
    int src_size = littlefs_file_size(src_mnt, src_fd);
    if (src_size < 0) {
        littlefs_file_close(src_mnt, src_fd);
        uart_printf("cp: %s: Cannot get file size\r\n", src_resolved);
        return -1;
    }

    /* Open destination for writing */
    int dst_fd = littlefs_file_open(dst_mnt, dst_subpath,
                                     LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (dst_fd < 0) {
        littlefs_file_close(src_mnt, src_fd);
        uart_printf("cp: %s: Cannot create destination file\r\n", dst_resolved);
        return -1;
    }

    /* Copy in chunks */
    char buf[512];
    int total_copied = 0;
    int bytes_read;

    while ((bytes_read = littlefs_file_read(src_mnt, src_fd, buf, sizeof(buf))) > 0) {
        int written = littlefs_file_write(dst_mnt, dst_fd, buf, bytes_read);
        if (written != bytes_read) {
            littlefs_file_close(src_mnt, src_fd);
            littlefs_file_close(dst_mnt, dst_fd);
            uart_printf("cp: Write error after %d bytes\r\n", total_copied);
            return -1;
        }
        total_copied += written;
    }

    littlefs_file_close(src_mnt, src_fd);
    littlefs_file_close(dst_mnt, dst_fd);

    uart_printf("Copied %d bytes: %s -> %s\r\n", total_copied, src_resolved, dst_resolved);
    return 0;
}

/*
 * touch <path> - Create an empty file or update timestamp
 * Creates the file if it doesn't exist.
 * Supports relative paths.
 */
int cmd_touch(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: touch <path>\r\n");
        uart_puts("  Create an empty file if it doesn't exist.\r\n");
        uart_puts("  Example: touch newfile.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("touch: path too long\r\n");
        return -1;
    }

    /* Get the mount context and subpath */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("touch: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    /* Try to open existing file, or create new one */
    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDWR | LFS_O_CREAT);
    if (fd < 0) {
        uart_printf("touch: %s: Failed to create file\r\n", resolved);
        return -1;
    }

    littlefs_file_close(mnt, fd);
    uart_printf("Touched %s\r\n", resolved);
    return 0;
}

/*
 * stat <path> - Show file/directory information
 * Supports relative paths.
 */
int cmd_stat(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: stat <path>\r\n");
        uart_puts("  Show file or directory information.\r\n");
        uart_puts("  Example: stat hello.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("stat: path too long\r\n");
        return -1;
    }

    /* Try VFS stat first */
    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) == 0) {
        uart_printf("  File: %s\r\n", resolved);
        uart_printf("  Type: %s\r\n", info.type == 1 ? "directory" : "regular file");
        uart_printf("  Size: %lu bytes\r\n", (unsigned long)info.size);
        return 0;
    }

    /* Try VFS node lookup for virtual files */
    const char *subpath = NULL;
    struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
    if (node) {
        /* If it's a mount point with subpath, we already tried vfs_stat_path above */
        if (node->type == VFS_NODE_MOUNT && subpath && subpath[0] != '\0' &&
            !(subpath[0] == '/' && subpath[1] == '\0')) {
            /* Path within mount but file doesn't exist */
            uart_printf("stat: %s: No such file or directory\r\n", resolved);
            return -1;
        }

        uart_printf("  File: %s\r\n", resolved);
        const char *type_str;
        switch (node->type) {
            case VFS_NODE_DIR:   type_str = "directory"; break;
            case VFS_NODE_FILE:  type_str = "virtual file"; break;
            case VFS_NODE_MOUNT: type_str = "mount point"; break;
            default:             type_str = "unknown"; break;
        }
        uart_printf("  Type: %s\r\n", type_str);
        if (node->type == VFS_NODE_FILE) {
            /* Try to get size by reading */
            char buf[1024];
            int len = vfs_read(node, buf, sizeof(buf));
            if (len >= 0) {
                uart_printf("  Size: %d bytes\r\n", len);
            }
        }
        return 0;
    }

    uart_printf("stat: %s: No such file or directory\r\n", resolved);
    return -1;
}

/*
 * Recursive tree helper
 */
static void tree_recurse(struct lfs_mount *mnt, const char *path, int depth, int max_depth)
{
    if (depth > max_depth) return;

    /* Build indent string */
    char indent[64];
    int i;
    for (i = 0; i < depth * 2 && i < 62; i++) {
        indent[i] = ' ';
    }
    indent[i] = '\0';

    /* Open directory */
    int dh = littlefs_dir_open(mnt, path);
    if (dh < 0) return;

    struct lfs_entry_info entry;
    while (littlefs_dir_read(mnt, dh, &entry) > 0) {
        /* Skip . and .. */
        if (entry.name[0] == '.' &&
            (entry.name[1] == '\0' ||
             (entry.name[1] == '.' && entry.name[2] == '\0'))) {
            continue;
        }

        uart_printf("%s", indent);
        if (entry.type == 1) {
            uart_printf("%s/\r\n", entry.name);

            /* Recurse into subdirectory */
            char subpath[VFS_MAX_PATH];
            size_t path_len = strlen(path);
            size_t name_len = strlen(entry.name);

            if (path_len + name_len + 2 < sizeof(subpath)) {
                strcpy(subpath, path);
                if (path_len > 1) {
                    subpath[path_len] = '/';
                    strcpy(subpath + path_len + 1, entry.name);
                } else {
                    strcpy(subpath + 1, entry.name);
                }
                tree_recurse(mnt, subpath, depth + 1, max_depth);
            }
        } else {
            uart_printf("%s  (%lu bytes)\r\n", entry.name, (unsigned long)entry.size);
        }
    }

    littlefs_dir_close(mnt, dh);
}

/*
 * tree [path] [depth] - Recursive directory listing
 * Defaults to cwd. Supports relative paths.
 */
int cmd_tree(int argc, char *argv[])
{
    char resolved[VFS_MAX_PATH];
    const char *input_path = ".";
    int max_depth = 5;  /* Default max depth */

    if (argc >= 2) {
        input_path = argv[1];
    }
    if (argc >= 3) {
        uint32_t d;
        if (shell_parse_uint(argv[2], &d) == 0 && d > 0) {
            max_depth = (int)d;
        }
    }

    if (shell_resolve_path(input_path, resolved, sizeof(resolved)) < 0) {
        uart_puts("tree: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        /* Try VFS listing for virtual directories */
        struct vfs_node *node = vfs_lookup_mount(resolved, &subpath);
        if (!node) {
            uart_printf("tree: %s: No such directory\r\n", resolved);
            return -1;
        }

        uart_printf("%s\r\n", resolved);
        if (node->type == VFS_NODE_DIR) {
            /* Simple VFS listing (non-recursive for virtual dirs) */
            vfs_list(node, ls_print_entry, NULL);
        }
        return 0;
    }

    uart_printf("%s\r\n", resolved);
    tree_recurse(mnt, subpath, 1, max_depth);

    return 0;
}

/*
 * wc <path> - Count lines, words, and bytes in a file
 * Supports relative paths.
 */
int cmd_wc(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: wc <path>\r\n");
        uart_puts("  Count lines, words, and bytes in a file.\r\n");
        uart_puts("  Example: wc readme.txt\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("wc: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("wc: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        uart_printf("wc: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    char buf[256];
    int bytes_read;
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    while ((bytes_read = littlefs_file_read(mnt, fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            bytes++;
            char c = buf[i];

            if (c == '\n') {
                lines++;
            }

            /* Word counting: whitespace-separated */
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = 0;
            } else {
                if (!in_word) {
                    words++;
                    in_word = 1;
                }
            }
        }
    }

    littlefs_file_close(mnt, fd);

    uart_printf("  %7lu  %7lu  %7lu  %s\r\n", lines, words, bytes, resolved);
    return 0;
}

/*
 * hexdump <path> [offset] [length] - Hex dump of file contents
 * Supports relative paths.
 */
int cmd_hexdump(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: hexdump <path> [offset] [length]\r\n");
        uart_puts("  Display file contents in hexadecimal.\r\n");
        uart_puts("  Default: first 256 bytes.\r\n");
        uart_puts("  Example: hexdump model.bin 0 64\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("hexdump: path too long\r\n");
        return -1;
    }

    size_t offset = 0;
    size_t length = 256;  /* Default length */

    if (argc >= 3) {
        uint32_t val;
        if (shell_parse_uint(argv[2], &val) == 0) {
            offset = val;
        }
    }
    if (argc >= 4) {
        uint32_t val;
        if (shell_parse_uint(argv[3], &val) == 0) {
            length = val;
            if (length > 4096) length = 4096;  /* Cap at 4KB */
        }
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("hexdump: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        uart_printf("hexdump: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    /* Seek to offset */
    if (offset > 0) {
        littlefs_file_seek(mnt, fd, (int32_t)offset, 0);  /* SEEK_SET */
    }

    /* Read and display */
    unsigned char buf[16];
    size_t total_read = 0;

    while (total_read < length) {
        size_t to_read = 16;
        if (total_read + to_read > length) {
            to_read = length - total_read;
        }

        int bytes_read = littlefs_file_read(mnt, fd, buf, to_read);
        if (bytes_read <= 0) break;

        /* Print offset */
        uart_printf("%08lx  ", (unsigned long)(offset + total_read));

        /* Print hex bytes */
        for (int i = 0; i < 16; i++) {
            if (i < bytes_read) {
                uart_printf("%02x ", buf[i]);
            } else {
                uart_puts("   ");
            }
            if (i == 7) uart_putc(' ');
        }

        uart_puts(" |");

        /* Print ASCII */
        for (int i = 0; i < bytes_read; i++) {
            char c = buf[i];
            if (c >= 0x20 && c < 0x7F) {
                uart_putc(c);
            } else {
                uart_putc('.');
            }
        }

        uart_puts("|\r\n");
        total_read += bytes_read;
    }

    littlefs_file_close(mnt, fd);

    uart_printf("%08lx\r\n", (unsigned long)(offset + total_read));
    return 0;
}

/*
 * Simple pattern matching helper (supports * and ? wildcards)
 */
static int pattern_match(const char *pattern, const char *str)
{
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return 1;  /* Trailing * matches all */
            /* Try matching rest of pattern at each position */
            while (*str) {
                if (pattern_match(pattern, str)) return 1;
                str++;
            }
            return pattern_match(pattern, str);
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }

    /* Handle trailing wildcards */
    while (*pattern == '*') pattern++;

    return (*pattern == '\0' && *str == '\0');
}

/*
 * grep <pattern> <path> - Search for pattern in file
 * Simple substring search. Supports relative paths.
 */
int cmd_grep(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: grep <pattern> <path>\r\n");
        uart_puts("  Search for pattern in file (case-sensitive substring).\r\n");
        uart_puts("  Example: grep error log.txt\r\n");
        return -1;
    }

    const char *pattern = argv[1];
    size_t pattern_len = strlen(pattern);

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
        uart_puts("grep: path too long\r\n");
        return -1;
    }

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("grep: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int fd = littlefs_file_open(mnt, subpath, LFS_O_RDONLY);
    if (fd < 0) {
        uart_printf("grep: %s: Cannot open file\r\n", resolved);
        return -1;
    }

    /* Read line by line */
    char line[512];
    int lpos = 0;
    int line_num = 1;
    int matches = 0;
    char buf[256];
    int bytes_read;

    while ((bytes_read = littlefs_file_read(mnt, fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < bytes_read; i++) {
            char c = buf[i];

            if (c == '\n' || lpos >= (int)sizeof(line) - 1) {
                line[lpos] = '\0';

                /* Search for pattern in line */
                int found = 0;
                for (int j = 0; j <= lpos - (int)pattern_len; j++) {
                    int match = 1;
                    for (size_t k = 0; k < pattern_len; k++) {
                        if (line[j + k] != pattern[k]) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        found = 1;
                        break;
                    }
                }

                if (found) {
                    uart_printf("%d: %s\r\n", line_num, line);
                    matches++;
                }

                lpos = 0;
                line_num++;
            } else {
                line[lpos++] = c;
            }
        }
    }

    /* Check last line if no newline at end */
    if (lpos > 0) {
        line[lpos] = '\0';
        int found = 0;
        for (int j = 0; j <= lpos - (int)pattern_len; j++) {
            int match = 1;
            for (size_t k = 0; k < pattern_len; k++) {
                if (line[j + k] != pattern[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                found = 1;
                break;
            }
        }
        if (found) {
            uart_printf("%d: %s\r\n", line_num, line);
            matches++;
        }
    }

    littlefs_file_close(mnt, fd);

    if (matches == 0) {
        uart_puts("(no matches)\r\n");
    } else {
        uart_printf("(%d matches)\r\n", matches);
    }

    return 0;
}

/*
 * Recursive find helper
 */
static void find_recurse(struct lfs_mount *mnt, const char *base_path,
                         const char *path, const char *pattern, int *count)
{
    int dh = littlefs_dir_open(mnt, path);
    if (dh < 0) return;

    struct lfs_entry_info entry;
    while (littlefs_dir_read(mnt, dh, &entry) > 0) {
        /* Skip . and .. */
        if (entry.name[0] == '.' &&
            (entry.name[1] == '\0' ||
             (entry.name[1] == '.' && entry.name[2] == '\0'))) {
            continue;
        }

        /* Build full path for display */
        char full_path[VFS_MAX_PATH];
        size_t path_len = strlen(path);
        size_t name_len = strlen(entry.name);

        if (path_len + name_len + 2 < sizeof(full_path)) {
            strcpy(full_path, path);
            if (path_len > 1 || (path_len == 1 && path[0] != '/')) {
                full_path[path_len] = '/';
                strcpy(full_path + path_len + 1, entry.name);
            } else if (path_len == 1 && path[0] == '/') {
                strcpy(full_path + 1, entry.name);
            } else {
                strcpy(full_path, entry.name);
            }

            /* Check if name matches pattern */
            if (pattern_match(pattern, entry.name)) {
                uart_printf("%s%s%s\r\n", base_path, full_path,
                            entry.type == 1 ? "/" : "");
                (*count)++;
            }

            /* Recurse into directories */
            if (entry.type == 1) {
                find_recurse(mnt, base_path, full_path, pattern, count);
            }
        }
    }

    littlefs_dir_close(mnt, dh);
}

/*
 * find <path> <pattern> - Find files by name pattern
 * Supports wildcards: * (any chars), ? (single char)
 * Supports relative paths.
 */
int cmd_find(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("Usage: find <path> <pattern>\r\n");
        uart_puts("  Find files matching pattern (recursive).\r\n");
        uart_puts("  Wildcards: * (any chars), ? (single char)\r\n");
        uart_puts("  Example: find /mnt/files *.txt\r\n");
        uart_puts("  Example: find . log*\r\n");
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[1], resolved, sizeof(resolved)) < 0) {
        uart_puts("find: path too long\r\n");
        return -1;
    }

    const char *pattern = argv[2];

    /* Get mount context */
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) {
        uart_printf("find: %s: Not a mounted filesystem\r\n", resolved);
        return -1;
    }

    int count = 0;

    /* Calculate the base path prefix (mount point) */
    size_t resolved_len = strlen(resolved);
    size_t subpath_len = subpath ? strlen(subpath) : 0;
    char base_path[VFS_MAX_PATH];

    if (subpath_len > 0 && resolved_len >= subpath_len) {
        size_t base_len = resolved_len - subpath_len;
        for (size_t i = 0; i < base_len && i < sizeof(base_path) - 1; i++) {
            base_path[i] = resolved[i];
        }
        base_path[base_len] = '\0';
    } else {
        strcpy(base_path, resolved);
    }

    find_recurse(mnt, base_path, subpath, pattern, &count);

    if (count == 0) {
        uart_puts("(no files found)\r\n");
    } else {
        uart_printf("(%d files found)\r\n", count);
    }

    return 0;
}
