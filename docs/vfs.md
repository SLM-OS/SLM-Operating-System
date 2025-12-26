# Virtual Filesystem (VFS)

This document describes the SLM-OS virtual filesystem, which provides a hierarchical namespace for system resources.

## Overview

The VFS is purely virtual — there is no persistent storage backing. All files generate their content dynamically via read callbacks. This design is ideal for exposing system state and configuration in a filesystem-like interface.

**Status:** Implemented (Phase 4)

## Directory Structure

```
/                       # Root directory
├── sys/                # System information
│   ├── memory          # PMM statistics
│   ├── cpus            # CPU topology info
│   ├── ipc             # IPC statistics
│   └── model           # Model memory pool stats
├── proc/               # Process/task information
│   ├── tasks           # List of all tasks
│   └── self            # Current task info
└── components/         # Component system mount point
```

## Virtual Files

### /sys/memory

Shows physical memory statistics from the PMM.

```
SLM-OS> cat /sys/memory
total_kb: 1048576
free_kb: 1047040
allocated_pages: 384
```

### /sys/cpus

Shows CPU topology information.

```
SLM-OS> cat /sys/cpus
cores: 4
online: 4
```

### /sys/ipc

Shows IPC subsystem statistics.

```
SLM-OS> cat /sys/ipc
queues: 2
buffers: 1
total_sent: 156
total_recv: 142
```

### /sys/model

Shows model memory pool statistics.

```
SLM-OS> cat /sys/model
weight_pool:
  total_blocks: 128
  free_blocks: 128
  allocated: 0
  shared: 0
workspace_pool:
  total_blocks: 64
  free_blocks: 64
  allocated: 0
  shared: 0
```

### /proc/tasks

Lists all tasks in the system.

```
SLM-OS> cat /proc/tasks
ID   State   CPU  Pri  Name
0    running 0    4    shell
1    blocked -    4    idle-0
2    blocked -    4    idle-1
```

### /proc/self

Shows information about the current (calling) task.

```
SLM-OS> cat /proc/self
id: 0
name: shell
state: running
priority: 4
cpu: 0
```

## API Reference

### Types

```c
/* Node types */
typedef enum {
    VFS_NODE_FILE,      /* Virtual file (read generates content) */
    VFS_NODE_DIR,       /* Directory containing other nodes */
} vfs_node_type_t;

/* Read callback for virtual files */
typedef int (*vfs_read_fn)(char *buf, size_t size, void *ctx);

/* List callback for directory enumeration */
typedef void (*vfs_list_fn)(struct vfs_node *child, void *ctx);
```

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `VFS_MAX_PATH` | 128 | Maximum path length |
| `VFS_MAX_NAME` | 32 | Maximum node name length |
| `VFS_MAX_CHILDREN` | 16 | Maximum children per directory |
| `VFS_MAX_OPEN_FILES` | 8 | Maximum open file descriptors |

### Functions

#### Initialization

```c
void vfs_init(void);
```

Initialize the VFS subsystem. Creates root directory and standard mount points (`/sys/`, `/proc/`, `/components/`). Called during kernel boot.

#### Node Lookup

```c
struct vfs_node *vfs_lookup(const char *path);
```

Look up a node by absolute path (must start with '/'). Returns the node pointer, or NULL if not found.

#### Creating Nodes

```c
struct vfs_node *vfs_create_file(struct vfs_node *parent, const char *name,
                                  vfs_read_fn read, void *ctx);
struct vfs_node *vfs_create_dir(struct vfs_node *parent, const char *name);
```

Create a new file or directory node. Files require a read callback that generates content dynamically.

#### Reading Files

```c
int vfs_read(struct vfs_node *node, char *buf, size_t size);
```

Read content from a virtual file. Calls the file's read callback. Returns bytes written, or -1 on error.

#### Listing Directories

```c
void vfs_list(struct vfs_node *node, vfs_list_fn callback, void *ctx);
```

Enumerate directory contents. Calls `callback` for each child node.

#### Mount Point Access

```c
struct vfs_node *vfs_root(void);           /* Get / */
struct vfs_node *vfs_get_sys(void);        /* Get /sys/ */
struct vfs_node *vfs_get_proc(void);       /* Get /proc/ */
struct vfs_node *vfs_get_components(void); /* Get /components/ */
```

#### Path Building

```c
char *vfs_get_path(struct vfs_node *node, char *buf, size_t size);
```

Build the full path for a node. Returns `buf` on success, NULL on error.

## Shell Commands

The VFS is accessible via shell commands:

```
SLM-OS> ls /
sys/
proc/
components/

SLM-OS> ls /sys
memory
cpus
ipc
model

SLM-OS> cat /sys/memory
total_kb: 1048576
free_kb: 1047040
allocated_pages: 384
```

## Implementation Notes

### Read Callbacks

Read callbacks must:
1. Write content to the provided buffer
2. Respect the `size` limit to prevent buffer overflow
3. Return the number of bytes written (or -1 on error)

Example:

```c
static int read_memory(char *buf, size_t size, void *ctx)
{
    (void)ctx;
    size_t total_kb = pmm_get_total_pages() * 4;
    size_t free_kb = pmm_get_free_pages() * 4;
    size_t alloc = pmm_get_total_pages() - pmm_get_free_pages();

    int pos = 0;
    size_t remain = size;
    int wrote;

    wrote = uart_snprintf(buf + pos, remain, "total_kb: %lu\n", total_kb);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "free_kb: %lu\n", free_kb);
    if (wrote > 0) { pos += wrote; remain = (size_t)pos < size ? size - pos : 0; }

    wrote = uart_snprintf(buf + pos, remain, "allocated_pages: %lu\n", alloc);
    if (wrote > 0) { pos += wrote; }

    return pos;
}
```

### Buffer Overflow Prevention

All VFS read callbacks use a `remain` variable pattern to prevent buffer overflow when `size - pos` would underflow:

```c
size_t remain = size;
// After each write:
remain = (size_t)pos < size ? size - pos : 0;
```

### Static Allocation

The VFS uses static allocation for nodes (no heap). The node pool is fixed at compile time, suitable for the embedded environment.

## Testing

VFS tests are in `kernel/tests/test_vfs.c`:

| Test Category | Tests |
|---------------|-------|
| Structure | vfs_init creates hierarchy, standard directories exist |
| Lookup | root lookup, sys directory, nested paths, invalid paths |
| Read | sys/memory content, sys/cpus content, proc/tasks, proc/self |
| Directory listing | root contains expected entries |
| Edge cases | zero-size read, non-existent paths |

Run tests:
```bash
make BUILD_DIR=/c/temp/slmos-build test
```

---

*Created: December 2025*
