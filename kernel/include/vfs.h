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
} vfs_node_type_t;

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
    vfs_node_type_t type;           /* File or directory */

    /* For files */
    vfs_read_fn read;               /* Read callback */
    void *read_ctx;                 /* Context passed to read callback */

    /* For directories */
    struct vfs_node *children[VFS_MAX_CHILDREN];
    int num_children;

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

#endif /* VFS_H */
