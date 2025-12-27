# Filesystem Subsystem

This document describes the SLM-OS filesystem stack, including the block device abstraction, LittleFS integration, and VFS mount point support.

**Status:** Implemented (Phase 4)

---

## Overview

SLM-OS provides a layered filesystem architecture designed for:
- **Flash-friendly storage** via LittleFS (wear leveling, power-loss resilience)
- **RAM disk testing** during development (no hardware dependencies)
- **VFS integration** for unified namespace access
- **Easy extension** to real storage (eMMC, SD cards) via block device abstraction

```
┌─────────────────────────────────────────────────────────────────┐
│  Shell Commands (ls, cat)                                       │
├─────────────────────────────────────────────────────────────────┤
│  VFS Layer - Unified namespace                                  │
│    ├── Virtual nodes: /sys/*, /proc/* (dynamic content)        │
│    └── Mount points: /mnt/* → LittleFS (persistent storage)    │
├─────────────────────────────────────────────────────────────────┤
│  LittleFS Wrapper (littlefs_slm.c)                              │
│    Static allocation, handle-based API                          │
├─────────────────────────────────────────────────────────────────┤
│  Block Device Abstraction (blkdev.h)                            │
│    read / prog / erase / sync operations                        │
├─────────────────────────────────────────────────────────────────┤
│  RAM Disk (ramdisk.c)  →  Future: eMMC, SD card                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Block Device Layer

The block device abstraction provides a hardware-independent interface for storage devices.

### API

```c
#include "blkdev.h"

/* Block device operations */
struct blkdev_ops {
    int (*read)(struct blkdev *dev, uint32_t block,
                uint32_t off, void *buf, uint32_t size);
    int (*prog)(struct blkdev *dev, uint32_t block,
                uint32_t off, const void *buf, uint32_t size);
    int (*erase)(struct blkdev *dev, uint32_t block);
    int (*sync)(struct blkdev *dev);
};

/* Block device descriptor */
struct blkdev {
    char name[BLKDEV_MAX_NAME];      /* Device name (e.g., "ramdisk0") */
    uint32_t read_size;              /* Minimum read size */
    uint32_t prog_size;              /* Minimum program size */
    uint32_t block_size;             /* Erase block size */
    uint32_t block_count;            /* Number of blocks */
    const struct blkdev_ops *ops;    /* Device operations */
    void *priv;                      /* Driver-private data */
    bool registered;                 /* In registry? */
};
```

### Registry Functions

| Function | Description |
|----------|-------------|
| `blkdev_init()` | Initialize the block device subsystem |
| `blkdev_register(dev)` | Register a device in the registry |
| `blkdev_unregister(dev)` | Remove a device from the registry |
| `blkdev_find(name)` | Look up a device by name |
| `blkdev_count()` | Return number of registered devices |
| `blkdev_capacity(dev)` | Get device capacity in bytes |

### Flash Semantics

Block devices follow flash storage semantics:
- **Read**: Any offset, any size within block
- **Program**: Write data (only valid after erase)
- **Erase**: Reset block to 0xFF (required before program)
- **Sync**: Ensure all writes are committed

---

## RAM Disk Driver

The RAM disk driver provides a memory-backed block device for testing and development.

### API

```c
#include "ramdisk.h"

/* Create a RAM disk with specified geometry */
struct blkdev *ramdisk_create(const char *name,
                               size_t block_size,
                               size_t block_count);

/* Create a 1MB RAM disk with default settings */
struct blkdev *ramdisk_create_default(const char *name);

/* Destroy a RAM disk and free memory */
void ramdisk_destroy(struct blkdev *dev);
```

### Default Configuration

| Parameter | Value | Description |
|-----------|-------|-------------|
| Block size | 4096 bytes | Standard flash erase block |
| Block count | 256 | 1MB total capacity |
| Read size | 1 byte | Any alignment supported |
| Program size | 1 byte | Any alignment supported |

### Memory Allocation

The RAM disk allocates backing memory from the PMM:
- Pages allocated via `pmm_alloc_pages()`
- Memory initialized to 0xFF (flash erased state)
- Pages freed on `ramdisk_destroy()`

---

## LittleFS Integration

[LittleFS](https://github.com/littlefs-project/littlefs) is a fail-safe filesystem designed for embedded systems.

### Features

- **Power-loss resilience**: Copy-on-write design survives unexpected power loss
- **Wear leveling**: Distributes writes across flash blocks
- **Bounded RAM**: Fixed memory usage, no heap allocation
- **Small footprint**: ~10KB code size

### Configuration

LittleFS is configured for freestanding use (no libc):

| Define | Value | Purpose |
|--------|-------|---------|
| `LFS_NO_MALLOC` | defined | Use static buffers only |
| `LFS_NO_ASSERT` | defined | No assert() calls |
| `LFS_NO_DEBUG` | defined | No debug output |
| `LFS_NO_WARN` | defined | No warning output |
| `LFS_NO_ERROR` | defined | No error output |

SLM-specific constants in `littlefs_slm.h`:

| Constant | Value | Purpose |
|----------|-------|---------|
| `LFS_SLM_CACHE_SIZE` | 256 | Read/program buffer size |
| `LFS_SLM_LOOKAHEAD_SIZE` | 16 | Lookahead buffer (tracks 128 blocks) |
| `LFS_SLM_BLOCK_CYCLES` | 500 | Wear leveling threshold |
| `LFS_SLM_MAX_FILES` | 4 | Maximum open files |
| `LFS_SLM_MAX_DIRS` | 4 | Maximum open directories |

### API

```c
#include "littlefs_slm.h"

/* Initialization */
void littlefs_init(void);

/* Mount/unmount */
struct lfs_mount *littlefs_mount(struct blkdev *dev, bool format);
int littlefs_unmount(struct lfs_mount *mnt);
int littlefs_format(struct blkdev *dev);

/* File operations */
int littlefs_file_open(struct lfs_mount *mnt, const char *path, int flags);
int littlefs_file_close(struct lfs_mount *mnt, int handle);
int littlefs_file_read(struct lfs_mount *mnt, int handle, void *buf, size_t size);
int littlefs_file_write(struct lfs_mount *mnt, int handle, const void *buf, size_t size);
int littlefs_file_seek(struct lfs_mount *mnt, int handle, int32_t offset, int whence);
int littlefs_file_size(struct lfs_mount *mnt, int handle);
int littlefs_file_sync(struct lfs_mount *mnt, int handle);

/* Directory operations */
int littlefs_dir_open(struct lfs_mount *mnt, const char *path);
int littlefs_dir_close(struct lfs_mount *mnt, int handle);
int littlefs_dir_read(struct lfs_mount *mnt, int handle, struct lfs_entry_info *info);
int littlefs_mkdir(struct lfs_mount *mnt, const char *path);
int littlefs_remove(struct lfs_mount *mnt, const char *path);
int littlefs_stat_path(struct lfs_mount *mnt, const char *path, struct lfs_entry_info *info);
int littlefs_rename(struct lfs_mount *mnt, const char *oldpath, const char *newpath);

/* Statistics */
int littlefs_stat(struct lfs_mount *mnt, uint32_t *total_blocks, uint32_t *used_blocks);
```

### Open Flags

Use standard LittleFS flags (from `lfs.h`):

| Flag | Description |
|------|-------------|
| `LFS_O_RDONLY` | Open for reading only |
| `LFS_O_WRONLY` | Open for writing only |
| `LFS_O_RDWR` | Open for reading and writing |
| `LFS_O_CREAT` | Create file if it doesn't exist |
| `LFS_O_TRUNC` | Truncate file to zero length |
| `LFS_O_APPEND` | Append to end of file |

### Error Codes

LittleFS returns negative error codes:

| Code | Meaning |
|------|---------|
| `LFS_ERR_OK` (0) | Success |
| `LFS_ERR_IO` (-5) | I/O error |
| `LFS_ERR_CORRUPT` (-84) | Filesystem corruption |
| `LFS_ERR_NOENT` (-2) | No such file or directory |
| `LFS_ERR_EXIST` (-17) | File already exists |
| `LFS_ERR_NOTDIR` (-20) | Not a directory |
| `LFS_ERR_ISDIR` (-21) | Is a directory |
| `LFS_ERR_NOTEMPTY` (-39) | Directory not empty |
| `LFS_ERR_NOSPC` (-28) | No space left |
| `LFS_ERR_NOMEM` (-12) | Out of memory |

---

## VFS Mount Points

The VFS layer provides a unified namespace where LittleFS filesystems can be mounted alongside virtual directories.

### Mount API

```c
#include "vfs.h"
#include "littlefs_vfs.h"

/* Mount LittleFS at a VFS path */
int littlefs_mount_at(const char *vfs_path, struct blkdev *dev, bool format);

/* Low-level mount (for custom filesystems) */
int vfs_mount(const char *path, const struct vfs_fs_ops *ops, void *ctx);
int vfs_unmount(const char *path);

/* Access files through mount points */
int vfs_read_path(const char *path, char *buf, size_t size, size_t offset);
int vfs_list_path(const char *path,
                  void (*callback)(const struct vfs_entry_info *info, void *ctx),
                  void *ctx);
int vfs_stat_path(const char *path, struct vfs_entry_info *info);
```

### Filesystem Operations Interface

Custom filesystems implement the `vfs_fs_ops` interface:

```c
struct vfs_fs_ops {
    int (*read)(void *ctx, const char *path, char *buf, size_t size, size_t offset);
    int (*readdir)(void *ctx, const char *path,
                   void (*callback)(const struct vfs_entry_info *info, void *ctx),
                   void *ctx);
    int (*stat)(void *ctx, const char *path, struct vfs_entry_info *info);
};
```

### Directory Structure with Mounts

```
/                       # Root directory
├── sys/                # System information (virtual)
│   ├── memory
│   ├── cpus
│   └── ...
├── proc/               # Process information (virtual)
│   ├── tasks
│   └── self
├── components/         # Component system (virtual)
└── mnt/                # Mount points
    └── files/          # LittleFS on RAM disk (persistent)
        ├── hello.txt
        └── readme.txt
```

---

## Shell Access

The shell provides full read/write access to mounted filesystems:

### Read Commands

```
SLM-OS> ls /mnt/files
hello.txt    [f]    20
readme.txt   [f]    74

SLM-OS> cat /mnt/files/hello.txt
Hello from LittleFS!

SLM-OS> cat /mnt/files/readme.txt
This file is stored on a RAM disk using LittleFS.
It demonstrates persistent file storage.

SLM-OS> df
Filesystem      Blocks     Used     Free   Use%
/mnt/files         256        4      252     1%
                 1024K     16K    1008K
```

### Write Commands

```
SLM-OS> write /mnt/files/new.txt This is new content
Wrote 19 bytes to /mnt/files/new.txt

SLM-OS> mkdir /mnt/files/logs
Created directory /mnt/files/logs

SLM-OS> append /mnt/files/logs/app.log Started at boot
Appended 16 bytes to /mnt/files/logs/app.log

SLM-OS> mv /mnt/files/new.txt /mnt/files/renamed.txt
Moved /mnt/files/new.txt -> /mnt/files/renamed.txt

SLM-OS> rm /mnt/files/renamed.txt
Removed /mnt/files/renamed.txt

SLM-OS> truncate /mnt/files/logs/app.log 0
Truncated /mnt/files/logs/app.log to 0 bytes
```

### Streaming Reads

For large files, `cat` supports offset and length:

```
SLM-OS> cat /mnt/files/model.bin 0 1024
[offset=0, read=1024 bytes]
(binary data)

SLM-OS> cat /mnt/files/model.bin 1024 1024
[offset=1024, read=1024 bytes]
(more binary data)
```

### Path Resolution

When accessing paths under a mount point:
1. VFS looks up the mount point node (e.g., `/mnt/files`)
2. Extracts the subpath (e.g., `/hello.txt`)
3. Calls the filesystem's operations with the subpath
4. Returns the result to the caller

---

## Initialization

LittleFS is initialized during kernel boot in `main.c`:

```c
/* Initialize block device and filesystem subsystems */
blkdev_init();
littlefs_init();

/* Create RAM disk */
struct blkdev *ramdisk = ramdisk_create_default("ramdisk0");
blkdev_register(ramdisk);

/* Mount LittleFS at /mnt/files (format on first mount) */
littlefs_mount_at("/mnt/files", ramdisk, true);

/* Create test files */
struct lfs_mount *mnt = ...;  /* Retrieved from mount */
int fd = littlefs_file_open(mnt, "/hello.txt", LFS_O_CREAT | LFS_O_WRONLY);
littlefs_file_write(mnt, fd, "Hello from LittleFS!", 20);
littlefs_file_close(mnt, fd);
```

---

## Testing

Tests are in `kernel/tests/test_littlefs.c`:

| Category | Tests |
|----------|-------|
| RAM Disk | create, read/write, erase to 0xFF |
| LittleFS Mount | format, mount, remount, stat |
| File Operations | create, write, read, seek, size |
| Directory Operations | mkdir, list, remove |
| VFS Integration | mount lookup, read through mount, list through mount |

Run tests:
```bash
make BUILD_DIR=/c/temp/slmos-build test
```

Expected output:
```
[TEST] LittleFS Integration Tests
  [PASS] test_ramdisk_create
  [PASS] test_ramdisk_read_write
  [PASS] test_ramdisk_erase
  [PASS] test_lfs_format_mount
  [PASS] test_lfs_remount
  [PASS] test_lfs_stat
  [PASS] test_lfs_file_create_write
  [PASS] test_lfs_file_read
  [PASS] test_lfs_file_seek_size
  [PASS] test_lfs_mkdir
  [PASS] test_lfs_dir_list
  [PASS] test_lfs_remove
  [PASS] test_vfs_mount_lookup
  [PASS] test_vfs_mount_read_file
  [PASS] test_vfs_mount_list_dir

Tests: 15  Passed: 15  Failed: 0
```

---

## File Locations

| File | Purpose |
|------|---------|
| `kernel/include/blkdev.h` | Block device interface |
| `kernel/drivers/blkdev.c` | Block device registry |
| `kernel/include/ramdisk.h` | RAM disk API |
| `kernel/drivers/ramdisk.c` | RAM disk driver |
| `kernel/include/littlefs_slm.h` | LittleFS wrapper API |
| `kernel/fs/littlefs_slm.c` | LittleFS wrapper implementation |
| `kernel/include/littlefs_vfs.h` | VFS adapter header |
| `kernel/fs/littlefs_vfs.c` | VFS adapter for LittleFS |
| `kernel/lib/littlefs/` | LittleFS source (upstream) |
| `kernel/src/string.c` | String functions for freestanding |
| `kernel/tests/test_littlefs.c` | Integration tests |

---

## Future Extensions

### eMMC/SD Card Support

To add a real storage driver:

1. Implement `struct blkdev_ops` for the hardware
2. Create the device structure with correct geometry
3. Register with `blkdev_register()`
4. Mount LittleFS on the new device

Example for Jetson eMMC:
```c
struct blkdev *emmc = emmc_init();  /* Initialize hardware */
blkdev_register(emmc);
littlefs_mount_at("/mnt/storage", emmc, false);  /* Don't format! */
```

### Additional Commands

The following shell commands are implemented for filesystem operations:

**Write commands:**
- `write <path> <content>` - Write/overwrite file
- `mkdir <path>` - Create directory
- `rm <path>` - Remove file or empty directory
- `mv <src> <dst>` - Move/rename
- `cp <src> <dst>` - Copy file (cross-mount supported)
- `touch <path>` - Create empty file
- `truncate <path> <size>` - Resize file
- `append <path> <content>` - Append to file (for logging)

**Read/info commands:**
- `df [path]` - Filesystem statistics
- `stat <path>` - Show file/directory info (type, size)
- `tree [path] [depth]` - Recursive directory listing
- `wc <path>` - Count lines, words, bytes
- `hexdump <path> [off] [len]` - Hex dump file contents

**Search commands:**
- `grep <pattern> <path>` - Search for pattern in file
- `find <path> <pattern>` - Find files by name (wildcards: `*`, `?`)

### Multiple Partitions

The block device layer supports multiple devices:
```
/mnt/system    -> LittleFS on eMMC partition 1
/mnt/data      -> LittleFS on eMMC partition 2
/mnt/sdcard    -> LittleFS on SD card
```

---

*Created: December 2025*
