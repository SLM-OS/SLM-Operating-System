/*
 * vfs.h - Virtual Filesystem Interface for SLM-OS
 *
 * Provides a hierarchical namespace for system resources.
 * All files are virtual - no persistent storage backing.
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/* Maximum path length */
#define VFS_MAX_PATH        128

/* Maximum name length for a node */
#define VFS_MAX_NAME        32

/* Maximum children per directory */
#define VFS_MAX_CHILDREN    16

/* Maximum open files */
#define VFS_MAX_OPEN_FILES  8

/* Node types */
typedef enum {
    VFS_NODE_FILE,      /* Virtual file (read generates content) */
    VFS_NODE_DIR,       /* Directory containing other nodes */
    VFS_NODE_MOUNT,     /* Mount point for a real filesystem */
} vfs_node_type_t;

/* Forward declaration for filesystem operations */
struct vfs_fs_ops;

/*
 * Entry info for mounted filesystem traversal.
 * Generic structure that filesystems populate.
 */
struct vfs_entry_info {
    char name[VFS_MAX_NAME];    /* Entry name */
    uint8_t type;               /* 0=file, 1=directory */
    uint32_t size;              /* File size (for files) */
};

/*
 * Filesystem operations structure.
 * Mounted filesystems implement these callbacks.
 */
struct vfs_fs_ops {
    /*
     * Read file content.
     *
     * @ctx:    Filesystem context (e.g., lfs_mount)
     * @path:   Path within filesystem (relative)
     * @buf:    Buffer to write content to
     * @size:   Maximum bytes to read
     * @offset: Offset within file
     *
     * Returns: Number of bytes read, or -1 on error.
     */
    int (*read)(void *ctx, const char *path, char *buf, size_t size, size_t offset);

    /*
     * Read directory entries.
     *
     * @ctx:      Filesystem context
     * @path:     Path within filesystem (relative)
     * @callback: Called for each entry
     * @cb_ctx:   User context passed to callback
     *
     * Returns: 0 on success, -1 on error.
     */
    int (*readdir)(void *ctx, const char *path,
                   void (*callback)(const struct vfs_entry_info *info, void *cb_ctx),
                   void *cb_ctx);

    /*
     * Get file/directory info.
     *
     * @ctx:  Filesystem context
     * @path: Path within filesystem (relative)
     * @info: Output entry info
     *
     * Returns: 0 on success, -1 on error.
     */
    int (*stat)(void *ctx, const char *path, struct vfs_entry_info *info);
};

/*
 * Read callback for virtual files.
 *
 * @buf:    Buffer to write content to
 * @size:   Maximum bytes to write
 * @ctx:    User-provided context (optional)
 *
 * Returns: Number of bytes written, or -1 on error.
 */
typedef int (*vfs_read_fn)(char *buf, size_t size, void *ctx);

/*
 * VFS node structure.
 *
 * Represents a file or directory in the virtual filesystem.
 */
struct vfs_node {
    char name[VFS_MAX_NAME];        /* Node name (e.g., "memory") */
    vfs_node_type_t type;           /* File, directory, or mount point */

    /* For files */
    vfs_read_fn read;               /* Read callback */
    void *read_ctx;                 /* Context passed to read callback */

    /* For directories */
    struct vfs_node *children[VFS_MAX_CHILDREN];
    int num_children;

    /* For mount points (VFS_NODE_MOUNT) */
    const struct vfs_fs_ops *fs_ops;  /* Filesystem operations */
    void *fs_ctx;                     /* Filesystem context (e.g., lfs_mount*) */

    /* Tree structure */
    struct vfs_node *parent;
};

/*
 * Initialize the VFS subsystem.
 *
 * Creates root directory and standard mount points:
 *   /         - root directory
 *   /sys/     - system information
 *   /proc/    - process/task information
 *   /components/ - component system mount point
 */
void vfs_init(void);

/*
 * Lookup a node by path.
 *
 * @path: Absolute path (must start with '/')
 *
 * Returns: Node pointer, or NULL if not found.
 */
struct vfs_node *vfs_lookup(const char *path);

/*
 * Create a file node.
 *
 * @parent: Parent directory
 * @name:   File name
 * @read:   Read callback function
 * @ctx:    Context passed to read callback
 *
 * Returns: New node, or NULL on error.
 */
struct vfs_node *vfs_create_file(struct vfs_node *parent, const char *name,
                                  vfs_read_fn read, void *ctx);

/*
 * Create a directory node.
 *
 * @parent: Parent directory
 * @name:   Directory name
 *
 * Returns: New node, or NULL on error.
 */
struct vfs_node *vfs_create_dir(struct vfs_node *parent, const char *name);

/*
 * Read from a virtual file.
 *
 * @node:   File node
 * @buf:    Buffer to write content to
 * @size:   Maximum bytes to write
 *
 * Returns: Number of bytes read, or -1 on error.
 */
int vfs_read(struct vfs_node *node, char *buf, size_t size);

/*
 * List directory contents.
 *
 * @node:      Directory node
 * @callback:  Called for each child node
 * @ctx:       User context passed to callback
 */
typedef void (*vfs_list_fn)(struct vfs_node *child, void *ctx);
void vfs_list(struct vfs_node *node, vfs_list_fn callback, void *ctx);

/*
 * Get the root directory.
 */
struct vfs_node *vfs_root(void);

/*
 * Get a standard mount point.
 */
struct vfs_node *vfs_get_sys(void);      /* /sys/ */
struct vfs_node *vfs_get_proc(void);     /* /proc/ */
struct vfs_node *vfs_get_components(void); /* /components/ */

/*
 * Build full path for a node.
 *
 * @node: Node to get path for
 * @buf:  Buffer to write path to
 * @size: Buffer size
 *
 * Returns: buf on success, NULL on error.
 */
char *vfs_get_path(struct vfs_node *node, char *buf, size_t size);

/*
 * Mount a filesystem at a path.
 *
 * Creates a mount point node that redirects operations to the
 * provided filesystem.
 *
 * @path:   Absolute path for mount point (e.g., "/mnt/files")
 * @ops:    Filesystem operations
 * @ctx:    Filesystem context (passed to ops callbacks)
 *
 * Returns: Mount point node, or NULL on error.
 */
struct vfs_node *vfs_mount(const char *path,
                            const struct vfs_fs_ops *ops,
                            void *ctx);

/*
 * Unmount a filesystem.
 *
 * @mnt: Mount point node from vfs_mount()
 *
 * Returns: 0 on success, -1 on error.
 */
int vfs_unmount(struct vfs_node *mnt);

/*
 * Check if a node is a mount point.
 */
static inline int vfs_is_mount(struct vfs_node *node)
{
    return node && node->type == VFS_NODE_MOUNT;
}

/*
 * Lookup extended: returns both node and remaining subpath.
 *
 * When a mount point is encountered, returns the mount point node
 * and the remaining path within that filesystem.
 *
 * @path:        Absolute path to look up
 * @subpath_out: Output: remaining path within mount (NULL if not mount)
 *
 * Returns: Node pointer (may be mount point), or NULL if not found.
 */
struct vfs_node *vfs_lookup_mount(const char *path, const char **subpath_out);

/*
 * Read from a file, including mount point files.
 *
 * If path resolves to a mount point, uses the mounted filesystem.
 *
 * @path:   Absolute path
 * @buf:    Buffer to write content to
 * @size:   Maximum bytes to read
 * @offset: Offset within file
 *
 * Returns: Number of bytes read, or -1 on error.
 */
int vfs_read_path(const char *path, char *buf, size_t size, size_t offset);

/*
 * List directory at path, including mount point directories.
 *
 * @path:     Absolute path
 * @callback: Called for each entry
 * @ctx:      User context passed to callback
 *
 * Returns: 0 on success, -1 on error.
 */
int vfs_list_path(const char *path,
                  void (*callback)(const struct vfs_entry_info *info, void *ctx),
                  void *ctx);

/*
 * Get info for a path, including mount point paths.
 *
 * @path: Absolute path
 * @info: Output entry info
 *
 * Returns: 0 on success, -1 on error.
 */
int vfs_stat_path(const char *path, struct vfs_entry_info *info);

/*
 * Get the filesystem context for a mount point path.
 *
 * This is used by shell commands that need direct access to the
 * underlying filesystem (e.g., for write operations).
 *
 * @path:        Absolute path to look up
 * @subpath_out: Output: remaining path within mount
 *
 * Returns: Filesystem context (e.g., lfs_mount*), or NULL if not a mount.
 */
void *vfs_get_mount_ctx(const char *path, const char **subpath_out);

#endif /* VFS_H */
