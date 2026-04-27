/*
 * littlefs_slm.c - LittleFS Wrapper for SLM-OS
 *
 * Bridges LittleFS to the block device abstraction layer.
 * Uses static allocation (no malloc) for embedded use.
 */

/* LittleFS configuration - must be before lfs.h is included */
#define LFS_NO_MALLOC 1
#define LFS_NO_DEBUG 1
#define LFS_NO_WARN 1
#define LFS_NO_ERROR 1
#define LFS_NO_ASSERT 1

/* Include LittleFS wrapper header (which includes lfs.h) */
#include "../include/littlefs_slm.h"
#include "../include/pmm.h"
#include "../include/spinlock.h"
#include "../include/debug.h"

/* Maximum simultaneous mounts. Production needs 1 (the boot
 * /mnt/files mount); persistent_lfs_store_create temporarily holds
 * a second when migrating between primary + backup images. The
 * test harness routinely cycles many persistent_lfs stores within
 * one boot, and any TEST_ASSERT failure mid-test returns from the
 * test function without unmounting — those slots leak until the
 * next boot. Pre-#486 this was 2, so a single such leak cascaded
 * into "no slot available" for every subsequent littlefs_mount in
 * the same run, making unrelated test_lfs_* / test_persistent_lfs
 * tests fail downstream of the first failure. Bumped to 8 so a
 * handful of leaked slots don't break later tests; production
 * cost is `sizeof(struct lfs_mount) * 6` ≈ a few KB of BSS, which
 * is negligible compared to the LittleFS read/prog buffers each
 * mount carries. */
#define LFS_MAX_MOUNTS 8

/*
 * File handle state
 */
struct file_handle {
    bool in_use;
    uint32_t generation;
    lfs_file_t file;
    uint8_t cache[LFS_SLM_CACHE_SIZE];      /* Per-file cache buffer */
    struct lfs_file_config cfg;
};

/*
 * Directory handle state
 */
struct dir_handle {
    bool in_use;
    uint32_t generation;
    lfs_dir_t dir;
};

/*
 * Mount context - contains all LittleFS state for one mounted filesystem
 */
struct lfs_mount {
    bool in_use;
    struct blkdev *dev;                  /* Underlying block device */

    /* LittleFS core state */
    lfs_t lfs;
    struct lfs_config config;

    /* Static buffers (LFS_NO_MALLOC) */
    uint8_t read_buffer[LFS_SLM_CACHE_SIZE];
    uint8_t prog_buffer[LFS_SLM_CACHE_SIZE];
    uint8_t lookahead_buffer[LFS_SLM_LOOKAHEAD_SIZE];

    /* File/directory handles */
    struct file_handle files[LFS_SLM_MAX_FILES];
    struct dir_handle dirs[LFS_SLM_MAX_DIRS];

    /* Synchronization */
    spinlock_t lock;
};

/* Static mount pool */
static struct lfs_mount mounts[LFS_MAX_MOUNTS];
static spinlock_t mounts_lock;
static bool initialized = false;

/* ---- Memory helpers (no libc) ---- */

static void lfs_memset(void *dst, int val, size_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)val;
    }
}

static void lfs_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---- LittleFS <-> Block Device Bridge ---- */

/*
 * Read callback for LittleFS
 */
static int lfs_blkdev_read(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, void *buffer, lfs_size_t size)
{
    struct lfs_mount *mnt = c->context;
    struct blkdev *dev = mnt->dev;

    int err = dev->ops->read(dev, block, off, buffer, size);
    if (err != BLKDEV_OK) {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

/*
 * Program (write) callback for LittleFS
 */
static int lfs_blkdev_prog(const struct lfs_config *c, lfs_block_t block,
                           lfs_off_t off, const void *buffer, lfs_size_t size)
{
    struct lfs_mount *mnt = c->context;
    struct blkdev *dev = mnt->dev;

    int err = dev->ops->prog(dev, block, off, buffer, size);
    if (err != BLKDEV_OK) {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

/*
 * Erase callback for LittleFS
 */
static int lfs_blkdev_erase(const struct lfs_config *c, lfs_block_t block)
{
    struct lfs_mount *mnt = c->context;
    struct blkdev *dev = mnt->dev;

    int err = dev->ops->erase(dev, block);
    if (err != BLKDEV_OK) {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

/*
 * Sync callback for LittleFS
 */
static int lfs_blkdev_sync(const struct lfs_config *c)
{
    struct lfs_mount *mnt = c->context;
    struct blkdev *dev = mnt->dev;

    int err = dev->ops->sync(dev);
    if (err != BLKDEV_OK) {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

/* ---- Mount Context Management ---- */

static struct lfs_mount *alloc_mount(void)
{
    irq_flags_t flags = spin_lock_irqsave(&mounts_lock);

    for (int i = 0; i < LFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            lfs_memset(&mounts[i], 0, sizeof(struct lfs_mount));
            mounts[i].in_use = true;
            spin_init(&mounts[i].lock);
            spin_unlock_irqrestore(&mounts_lock, flags);
            return &mounts[i];
        }
    }

    spin_unlock_irqrestore(&mounts_lock, flags);
    return NULL;
}

static void free_mount(struct lfs_mount *mnt)
{
    if (!mnt) return;

    irq_flags_t flags = spin_lock_irqsave(&mounts_lock);
    mnt->in_use = false;
    spin_unlock_irqrestore(&mounts_lock, flags);
}

/* ---- Handle Management ---- */

/* Encode/decode a handle as (index | generation << 16). The caller
 * sees a plain int; the generation component guards against stale
 * references after a close + reopen cycle that reuses the same pool
 * slot. The 16-bit generation wraps after 65536 close/reopen cycles
 * on the same slot — acceptable for any realistic workload. */
static int encode_file_handle(int idx, uint32_t gen)
{
    return (int)((gen & 0xFFFFu) << 16 | (idx & 0xFFFF));
}

static int decode_file_index(int handle) { return handle & 0xFFFF; }
static uint32_t decode_file_gen(int handle) { return ((uint32_t)handle >> 16) & 0xFFFF; }

static int validate_file_handle(struct lfs_mount *mnt, int handle)
{
    int idx = decode_file_index(handle);
    if (idx < 0 || idx >= LFS_SLM_MAX_FILES) return -1;
    if (!mnt->files[idx].in_use) return -1;
    if ((mnt->files[idx].generation & 0xFFFF) != decode_file_gen(handle)) return -1;
    return idx;
}

static int encode_dir_handle(int idx, uint32_t gen)
{
    return (int)((gen & 0xFFFFu) << 16 | (idx & 0xFFFF));
}

static int decode_dir_index(int handle) { return handle & 0xFFFF; }
static uint32_t decode_dir_gen(int handle) { return ((uint32_t)handle >> 16) & 0xFFFF; }

static int validate_dir_handle(struct lfs_mount *mnt, int handle)
{
    int idx = decode_dir_index(handle);
    if (idx < 0 || idx >= LFS_SLM_MAX_DIRS) return -1;
    if (!mnt->dirs[idx].in_use) return -1;
    if ((mnt->dirs[idx].generation & 0xFFFF) != decode_dir_gen(handle)) return -1;
    return idx;
}

static int alloc_file_handle(struct lfs_mount *mnt)
{
    for (int i = 0; i < LFS_SLM_MAX_FILES; i++) {
        if (!mnt->files[i].in_use) {
            uint32_t gen = mnt->files[i].generation;
            lfs_memset(&mnt->files[i], 0, sizeof(struct file_handle));
            mnt->files[i].generation = gen;
            mnt->files[i].in_use = true;
            mnt->files[i].cfg.buffer = mnt->files[i].cache;
            return encode_file_handle(i, gen);
        }
    }
    return LFS_ERR_NOMEM;
}

static void free_file_handle(struct lfs_mount *mnt, int handle)
{
    int idx = decode_file_index(handle);
    if (idx >= 0 && idx < LFS_SLM_MAX_FILES) {
        mnt->files[idx].in_use = false;
        mnt->files[idx].generation++;
    }
}

static int alloc_dir_handle(struct lfs_mount *mnt)
{
    for (int i = 0; i < LFS_SLM_MAX_DIRS; i++) {
        if (!mnt->dirs[i].in_use) {
            uint32_t gen = mnt->dirs[i].generation;
            lfs_memset(&mnt->dirs[i], 0, sizeof(struct dir_handle));
            mnt->dirs[i].generation = gen;
            mnt->dirs[i].in_use = true;
            return encode_dir_handle(i, gen);
        }
    }
    return LFS_ERR_NOMEM;
}

static void free_dir_handle(struct lfs_mount *mnt, int handle)
{
    int idx = decode_dir_index(handle);
    if (idx >= 0 && idx < LFS_SLM_MAX_DIRS) {
        mnt->dirs[idx].in_use = false;
        mnt->dirs[idx].generation++;
    }
}

/* ---- Public API ---- */

void littlefs_init(void)
{
    if (initialized) return;

    spin_init(&mounts_lock);
    lfs_memset(mounts, 0, sizeof(mounts));
    initialized = true;

    INFO("LittleFS subsystem initialized");
}

struct lfs_mount *littlefs_mount(struct blkdev *dev, bool format)
{
    if (!dev) {
        ERROR("littlefs_mount: NULL device");
        return NULL;
    }

    struct lfs_mount *mnt = alloc_mount();
    if (!mnt) {
        ERROR("littlefs_mount: no free mount slots");
        return NULL;
    }

    mnt->dev = dev;

    /* Configure LittleFS */
    lfs_memset(&mnt->config, 0, sizeof(mnt->config));
    mnt->config.context = mnt;
    mnt->config.read = lfs_blkdev_read;
    mnt->config.prog = lfs_blkdev_prog;
    mnt->config.erase = lfs_blkdev_erase;
    mnt->config.sync = lfs_blkdev_sync;

    /* Device geometry */
    mnt->config.read_size = dev->read_size;
    mnt->config.prog_size = dev->prog_size;
    mnt->config.block_size = dev->block_size;
    mnt->config.block_count = dev->block_count;
    mnt->config.block_cycles = LFS_SLM_BLOCK_CYCLES;
    mnt->config.cache_size = LFS_SLM_CACHE_SIZE;
    mnt->config.lookahead_size = LFS_SLM_LOOKAHEAD_SIZE;

    /* Static buffers (no malloc) */
    mnt->config.read_buffer = mnt->read_buffer;
    mnt->config.prog_buffer = mnt->prog_buffer;
    mnt->config.lookahead_buffer = mnt->lookahead_buffer;

    /* Format if requested */
    if (format) {
        int err = lfs_format(&mnt->lfs, &mnt->config);
        if (err < 0) {
            ERROR("littlefs_mount: format failed (%d)", err);
            free_mount(mnt);
            return NULL;
        }
        INFO("Formatted LittleFS on '%s'", dev->name);
    }

    /* Mount the filesystem */
    int err = lfs_mount(&mnt->lfs, &mnt->config);
    if (err < 0) {
        ERROR("littlefs_mount: mount failed (%d)", err);
        free_mount(mnt);
        return NULL;
    }

    INFO("Mounted LittleFS on '%s' (%u blocks x %u bytes)",
         dev->name, dev->block_count, dev->block_size);

    return mnt;
}

int littlefs_unmount(struct lfs_mount *mnt)
{
    if (!mnt || !mnt->in_use) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);

    /* Close any open files */
    for (int i = 0; i < LFS_SLM_MAX_FILES; i++) {
        if (mnt->files[i].in_use) {
            lfs_file_close(&mnt->lfs, &mnt->files[i].file);
            mnt->files[i].in_use = false;
        }
    }

    /* Close any open directories */
    for (int i = 0; i < LFS_SLM_MAX_DIRS; i++) {
        if (mnt->dirs[i].in_use) {
            lfs_dir_close(&mnt->lfs, &mnt->dirs[i].dir);
            mnt->dirs[i].in_use = false;
        }
    }

    int err = lfs_unmount(&mnt->lfs);

    spin_unlock_irqrestore(&mnt->lock, flags);

    if (err == 0) {
        INFO("Unmounted LittleFS from '%s'", mnt->dev->name);
        free_mount(mnt);
    }

    return err;
}

int littlefs_format(struct blkdev *dev)
{
    if (!dev) {
        return LFS_ERR_INVAL;
    }

    /* Create temporary config for formatting */
    struct lfs_config config;
    lfs_t lfs;

    /* Static buffers for format operation */
    static uint8_t read_buf[LFS_SLM_CACHE_SIZE];
    static uint8_t prog_buf[LFS_SLM_CACHE_SIZE];
    static uint8_t lookahead_buf[LFS_SLM_LOOKAHEAD_SIZE];

    /* We need a temporary mount context for the callbacks */
    struct lfs_mount temp_mnt;
    lfs_memset(&temp_mnt, 0, sizeof(temp_mnt));
    temp_mnt.dev = dev;

    lfs_memset(&config, 0, sizeof(config));
    config.context = &temp_mnt;
    config.read = lfs_blkdev_read;
    config.prog = lfs_blkdev_prog;
    config.erase = lfs_blkdev_erase;
    config.sync = lfs_blkdev_sync;

    config.read_size = dev->read_size;
    config.prog_size = dev->prog_size;
    config.block_size = dev->block_size;
    config.block_count = dev->block_count;
    config.block_cycles = LFS_SLM_BLOCK_CYCLES;
    config.cache_size = LFS_SLM_CACHE_SIZE;
    config.lookahead_size = LFS_SLM_LOOKAHEAD_SIZE;

    config.read_buffer = read_buf;
    config.prog_buffer = prog_buf;
    config.lookahead_buffer = lookahead_buf;

    return lfs_format(&lfs, &config);
}

int littlefs_stat(struct lfs_mount *mnt,
                  uint32_t *total_blocks,
                  uint32_t *used_blocks)
{
    if (!mnt || !mnt->in_use) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);

    if (total_blocks) {
        *total_blocks = mnt->dev->block_count;
    }

    int used = 0;
    if (used_blocks) {
        used = lfs_fs_size(&mnt->lfs);
        if (used < 0) {
            spin_unlock_irqrestore(&mnt->lock, flags);
            return used;
        }
        *used_blocks = (uint32_t)used;
    }

    spin_unlock_irqrestore(&mnt->lock, flags);
    return LFS_ERR_OK;
}

/* ---- File Operations ---- */

int littlefs_file_open(struct lfs_mount *mnt, const char *path, int flags)
{
    if (!mnt || !mnt->in_use || !path) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t iflags = spin_lock_irqsave(&mnt->lock);

    int handle = alloc_file_handle(mnt);
    if (handle < 0) {
        spin_unlock_irqrestore(&mnt->lock, iflags);
        return handle;
    }
    int idx = decode_file_index(handle);

    /* Use opencfg with static buffer (required for LFS_NO_MALLOC) */
    int err = lfs_file_opencfg(&mnt->lfs, &mnt->files[idx].file,
                                path, flags, &mnt->files[idx].cfg);
    if (err < 0) {
        free_file_handle(mnt, handle);
        spin_unlock_irqrestore(&mnt->lock, iflags);
        return err;
    }

    spin_unlock_irqrestore(&mnt->lock, iflags);
    return handle;
}

int littlefs_file_close(struct lfs_mount *mnt, int handle)
{
    if (!mnt || !mnt->in_use) return LFS_ERR_INVAL;
    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int idx = validate_file_handle(mnt, handle);
    if (idx < 0) {
        spin_unlock_irqrestore(&mnt->lock, flags);
        return LFS_ERR_BADF;
    }
    int err = lfs_file_close(&mnt->lfs, &mnt->files[idx].file);
    free_file_handle(mnt, handle);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return err;
}

/* Macro that validates a file handle, acquires the spinlock, and sets
 * `idx` to the decoded pool index. Jumps to a local `badf:` label on
 * validation failure. Used by every littlefs_file_* accessor below. */
#define VALIDATE_FILE(mnt, handle, idx, flags_var)         \
    if (!mnt || !mnt->in_use) return LFS_ERR_INVAL;       \
    flags_var = spin_lock_irqsave(&mnt->lock);             \
    idx = validate_file_handle(mnt, handle);               \
    if (idx < 0) {                                         \
        spin_unlock_irqrestore(&mnt->lock, flags_var);     \
        return LFS_ERR_BADF;                               \
    }

int littlefs_file_read(struct lfs_mount *mnt, int handle,
                       void *buffer, size_t size)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_read(&mnt->lfs, &mnt->files[idx].file,
                                buffer, (lfs_size_t)size);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_file_write(struct lfs_mount *mnt, int handle,
                        const void *buffer, size_t size)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_write(&mnt->lfs, &mnt->files[idx].file,
                                 buffer, (lfs_size_t)size);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_file_seek(struct lfs_mount *mnt, int handle,
                       int32_t offset, int whence)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_seek(&mnt->lfs, &mnt->files[idx].file,
                                offset, whence);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_file_size(struct lfs_mount *mnt, int handle)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_size(&mnt->lfs, &mnt->files[idx].file);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_file_sync(struct lfs_mount *mnt, int handle)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_sync(&mnt->lfs, &mnt->files[idx].file);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_file_truncate(struct lfs_mount *mnt, int handle, uint32_t size)
{
    irq_flags_t flags; int idx;
    VALIDATE_FILE(mnt, handle, idx, flags);
    int result = lfs_file_truncate(&mnt->lfs, &mnt->files[idx].file, size);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

#undef VALIDATE_FILE

/* ---- Directory Operations ---- */

int littlefs_dir_open(struct lfs_mount *mnt, const char *path)
{
    if (!mnt || !mnt->in_use) {
        return LFS_ERR_INVAL;
    }

    /* Empty path or "/" means root */
    const char *dir_path = (path && path[0]) ? path : "/";

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);

    int handle = alloc_dir_handle(mnt);
    if (handle < 0) {
        spin_unlock_irqrestore(&mnt->lock, flags);
        return handle;
    }

    int didx = decode_dir_index(handle);
    int err = lfs_dir_open(&mnt->lfs, &mnt->dirs[didx].dir, dir_path);
    if (err < 0) {
        free_dir_handle(mnt, handle);
        spin_unlock_irqrestore(&mnt->lock, flags);
        return err;
    }

    spin_unlock_irqrestore(&mnt->lock, flags);
    return handle;
}

int littlefs_dir_close(struct lfs_mount *mnt, int handle)
{
    if (!mnt || !mnt->in_use) return LFS_ERR_INVAL;
    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int idx = validate_dir_handle(mnt, handle);
    if (idx < 0) {
        spin_unlock_irqrestore(&mnt->lock, flags);
        return LFS_ERR_BADF;
    }
    int err = lfs_dir_close(&mnt->lfs, &mnt->dirs[idx].dir);
    free_dir_handle(mnt, handle);
    spin_unlock_irqrestore(&mnt->lock, flags);
    return err;
}

int littlefs_dir_read(struct lfs_mount *mnt, int handle,
                      struct lfs_entry_info *info)
{
    if (!mnt || !mnt->in_use || !info) return LFS_ERR_INVAL;
    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int idx = validate_dir_handle(mnt, handle);
    if (idx < 0) {
        spin_unlock_irqrestore(&mnt->lock, flags);
        return LFS_ERR_BADF;
    }

    struct lfs_info lfs_info;
    int result = lfs_dir_read(&mnt->lfs, &mnt->dirs[idx].dir, &lfs_info);

    if (result > 0) {
        info->type = lfs_info.type;
        info->size = lfs_info.size;
        lfs_strcpy(info->name, lfs_info.name, sizeof(info->name));
    }

    spin_unlock_irqrestore(&mnt->lock, flags);
    return result;
}

int littlefs_mkdir(struct lfs_mount *mnt, const char *path)
{
    if (!mnt || !mnt->in_use || !path) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int err = lfs_mkdir(&mnt->lfs, path);
    spin_unlock_irqrestore(&mnt->lock, flags);

    return err;
}

int littlefs_remove(struct lfs_mount *mnt, const char *path)
{
    if (!mnt || !mnt->in_use || !path) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int err = lfs_remove(&mnt->lfs, path);
    spin_unlock_irqrestore(&mnt->lock, flags);

    return err;
}

int littlefs_stat_path(struct lfs_mount *mnt, const char *path,
                       struct lfs_entry_info *info)
{
    if (!mnt || !mnt->in_use || !path || !info) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);

    struct lfs_info lfs_info;
    int err = lfs_stat(&mnt->lfs, path, &lfs_info);

    if (err == 0) {
        info->type = lfs_info.type;
        info->size = lfs_info.size;
        lfs_strcpy(info->name, lfs_info.name, sizeof(info->name));
    }

    spin_unlock_irqrestore(&mnt->lock, flags);
    return err;
}

int littlefs_rename(struct lfs_mount *mnt,
                    const char *oldpath, const char *newpath)
{
    if (!mnt || !mnt->in_use || !oldpath || !newpath) {
        return LFS_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&mnt->lock);
    int err = lfs_rename(&mnt->lfs, oldpath, newpath);
    spin_unlock_irqrestore(&mnt->lock, flags);

    return err;
}

/* ---- Utility Functions ---- */

lfs_t *littlefs_get_lfs(struct lfs_mount *mnt)
{
    return mnt ? &mnt->lfs : NULL;
}

struct blkdev *littlefs_get_blkdev(struct lfs_mount *mnt)
{
    return mnt ? mnt->dev : NULL;
}
