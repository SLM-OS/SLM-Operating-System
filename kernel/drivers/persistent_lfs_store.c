#include "../include/persistent_lfs_store.h"

#include "../include/blkdev.h"
#include "../include/boot_media.h"
#include "../include/debug.h"
#include "../include/fat32.h"
#include "../include/kprintf.h"
#include "../include/pmm.h"
#include "../include/spinlock.h"
#include "../include/string.h"
#include "../lib/fatfs/ff.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PERSISTENT_LFS_STORE_TMP_PATH "0:/slmstore/files.lfs.tmp"
#define PERSISTENT_LFS_STORE_BAK_PATH "0:/slmstore/files.lfs.bak"
#define PERSISTENT_LFS_DELTA_PATH     "0:/slmstore/files.lfs.delta"
#define PERSISTENT_LFS_DELTA_TMP_PATH "0:/slmstore/files.lfs.delta.tmp"
#define PERSISTENT_LFS_DELTA_BAK_PATH "0:/slmstore/files.lfs.delta.bak"
#define PERSISTENT_LFS_STORE_DIR      "0:/slmstore"
#define PERSISTENT_LFS_BLOCK_SIZE     4096u
#define PERSISTENT_LFS_DEFAULT_BYTES  (8u * 1024u * 1024u)
#define PERSISTENT_LFS_FAT_VOL        "0:"
#define PERSISTENT_LFS_DELTA_MAGIC    0x3146444Cu /* "LDF1" */

struct persistent_lfs_delta_header {
    uint32_t magic;
    uint32_t image_bytes;
    uint32_t block_size;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t reserved[3];
};

struct persistent_lfs_store_priv {
    uint8_t *data;
    size_t image_bytes;
    size_t data_pages;
    size_t desc_pages;
    bool dirty;
    bool backing_valid;
    bool journal_valid;
    uint32_t dirty_first_block;
    uint32_t dirty_last_block;
    uint32_t journal_first_block;
    uint32_t journal_last_block;
    spinlock_t lock;
};

static bool persistent_lfs_valid_image_size(FSIZE_t size)
{
    return size >= PERSISTENT_LFS_BLOCK_SIZE &&
           (size % PERSISTENT_LFS_BLOCK_SIZE) == 0;
}

static bool persistent_lfs_load_delta_file(FIL *fp,
                                           FILINFO *fno,
                                           struct blkdev *store_dev,
                                           struct persistent_lfs_store_priv *priv)
{
    struct persistent_lfs_delta_header delta_hdr;
    FRESULT res;
    UINT got = 0;

    res = f_read(fp, &delta_hdr, sizeof(delta_hdr), &got);
    if (res != FR_OK ||
        got != sizeof(delta_hdr) ||
        delta_hdr.magic != PERSISTENT_LFS_DELTA_MAGIC ||
        delta_hdr.image_bytes != priv->image_bytes ||
        delta_hdr.block_size != store_dev->block_size ||
        delta_hdr.block_count == 0 ||
        delta_hdr.first_block >= store_dev->block_count ||
        delta_hdr.block_count > store_dev->block_count ||
        delta_hdr.first_block > store_dev->block_count - delta_hdr.block_count ||
        fno->fsize != sizeof(delta_hdr) +
                      (FSIZE_t)delta_hdr.block_count * store_dev->block_size) {
        return false;
    }

    size_t remaining = (size_t)delta_hdr.block_count * store_dev->block_size;
    uint8_t *dst = priv->data +
                   (size_t)delta_hdr.first_block * store_dev->block_size;
    while (remaining > 0) {
        UINT chunk = remaining > 32768u ? 32768u : (UINT)remaining;
        res = f_read(fp, dst, chunk, &got);
        if (res != FR_OK || got != chunk) {
            return false;
        }
        dst += chunk;
        remaining -= chunk;
    }

    priv->journal_valid = true;
    priv->journal_first_block = delta_hdr.first_block;
    priv->journal_last_block = delta_hdr.first_block + delta_hdr.block_count - 1u;
    return true;
}

static void persistent_lfs_mark_dirty(struct persistent_lfs_store_priv *priv,
                                      uint32_t first_block,
                                      uint32_t last_block)
{
    if (!priv->dirty) {
        priv->dirty = true;
        priv->dirty_first_block = first_block;
        priv->dirty_last_block = last_block;
        return;
    }
    if (first_block < priv->dirty_first_block) {
        priv->dirty_first_block = first_block;
    }
    if (last_block > priv->dirty_last_block) {
        priv->dirty_last_block = last_block;
    }
}

static int persistent_lfs_read(struct blkdev *dev, uint32_t block, uint32_t off,
                               void *buffer, uint32_t size)
{
    struct persistent_lfs_store_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size + off;

    if (offset + size > priv->image_bytes) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    memcpy(buffer, priv->data + offset, size);
    spin_unlock_irqrestore(&priv->lock, flags);
    return BLKDEV_OK;
}

static int persistent_lfs_prog(struct blkdev *dev, uint32_t block, uint32_t off,
                               const void *buffer, uint32_t size)
{
    struct persistent_lfs_store_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size + off;

    if (offset + size > priv->image_bytes) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    memcpy(priv->data + offset, buffer, size);
    persistent_lfs_mark_dirty(priv, block, block);
    spin_unlock_irqrestore(&priv->lock, flags);
    return BLKDEV_OK;
}

static int persistent_lfs_erase(struct blkdev *dev, uint32_t block)
{
    struct persistent_lfs_store_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size;

    if (block >= dev->block_count) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    memset(priv->data + offset, 0xFF, dev->block_size);
    persistent_lfs_mark_dirty(priv, block, block);
    spin_unlock_irqrestore(&priv->lock, flags);
    return BLKDEV_OK;
}

static int persistent_lfs_flush_image(struct blkdev *dev)
{
    struct persistent_lfs_store_priv *priv = dev->priv;
    struct blkdev *boot_dev = NULL;
    FATFS fs;
    FIL fp;
    FILINFO fno;
    FRESULT res;
    UINT written = 0;
    bool mounted = false;
    bool temp_open = false;
    bool full_rewrite = false;
    bool journal_valid = false;
    uint32_t first_block = 0;
    uint32_t last_block = 0;
    size_t first_offset = 0;
    size_t flush_bytes = 0;
    struct persistent_lfs_delta_header delta_hdr;

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    if (!priv->dirty) {
        spin_unlock_irqrestore(&priv->lock, flags);
        return BLKDEV_OK;
    }
    first_block = priv->dirty_first_block;
    last_block = priv->dirty_last_block;
    full_rewrite = !priv->backing_valid;
    journal_valid = priv->journal_valid;
    if (!full_rewrite && journal_valid) {
        if (priv->journal_first_block < first_block) {
            first_block = priv->journal_first_block;
        }
        if (priv->journal_last_block > last_block) {
            last_block = priv->journal_last_block;
        }
    }
    spin_unlock_irqrestore(&priv->lock, flags);

    first_offset = (size_t)first_block * dev->block_size;
    flush_bytes = ((size_t)(last_block - first_block) + 1u) * dev->block_size;

    boot_dev = boot_media_acquire();
    if (!boot_dev) {
        return BLKDEV_ERR_IO;
    }

    fatfs_disk_attach(boot_dev);
    res = f_mount(&fs, PERSISTENT_LFS_FAT_VOL, 1);
    if (res != FR_OK) {
        fatfs_disk_detach();
        boot_media_release(boot_dev);
        return BLKDEV_ERR_IO;
    }
    mounted = true;

    res = f_mkdir(PERSISTENT_LFS_STORE_DIR);
    if (res != FR_OK && res != FR_EXIST) {
        goto fail;
    }

    if (!full_rewrite) {
        memset(&delta_hdr, 0, sizeof(delta_hdr));
        delta_hdr.magic = PERSISTENT_LFS_DELTA_MAGIC;
        delta_hdr.image_bytes = (uint32_t)priv->image_bytes;
        delta_hdr.block_size = dev->block_size;
        delta_hdr.first_block = first_block;
        delta_hdr.block_count = last_block - first_block + 1u;

        (void)f_unlink(PERSISTENT_LFS_DELTA_TMP_PATH);
        res = f_open(&fp, PERSISTENT_LFS_DELTA_TMP_PATH,
                     FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            goto fail;
        }
        temp_open = true;
        res = f_write(&fp, &delta_hdr, sizeof(delta_hdr), &written);
        if (res != FR_OK || written != sizeof(delta_hdr)) {
            goto fail;
        }

        irq_flags_t write_flags = spin_lock_irqsave(&priv->lock);
        const uint8_t *data = priv->data + first_offset;
        size_t remaining = flush_bytes;
        while (remaining > 0) {
            UINT chunk = remaining > 32768u ? 32768u : (UINT)remaining;
            res = f_write(&fp, data, chunk, &written);
            if (res != FR_OK || written != chunk) {
                spin_unlock_irqrestore(&priv->lock, write_flags);
                goto fail;
            }
            data += chunk;
            remaining -= chunk;
        }
        spin_unlock_irqrestore(&priv->lock, write_flags);
        res = f_close(&fp);
        temp_open = false;
        if (res != FR_OK) {
            goto fail;
        }

        (void)f_unlink(PERSISTENT_LFS_DELTA_BAK_PATH);
        if (f_stat(PERSISTENT_LFS_DELTA_PATH, &fno) == FR_OK) {
            res = f_rename(PERSISTENT_LFS_DELTA_PATH,
                           PERSISTENT_LFS_DELTA_BAK_PATH);
            if (res != FR_OK) {
                goto fail;
            }
        }
        res = f_rename(PERSISTENT_LFS_DELTA_TMP_PATH, PERSISTENT_LFS_DELTA_PATH);
        if (res != FR_OK) {
            if (f_stat(PERSISTENT_LFS_DELTA_BAK_PATH, &fno) == FR_OK) {
                (void)f_rename(PERSISTENT_LFS_DELTA_BAK_PATH,
                               PERSISTENT_LFS_DELTA_PATH);
            }
            goto fail;
        }
        (void)f_unlink(PERSISTENT_LFS_DELTA_BAK_PATH);
    } else {
        (void)f_unlink(PERSISTENT_LFS_STORE_TMP_PATH);
        res = f_open(&fp, PERSISTENT_LFS_STORE_TMP_PATH,
                     FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            goto fail;
        }
        temp_open = true;

        irq_flags_t write_flags = spin_lock_irqsave(&priv->lock);
        const uint8_t *data = priv->data;
        size_t remaining = priv->image_bytes;
        while (remaining > 0) {
            UINT chunk = remaining > 32768u ? 32768u : (UINT)remaining;
            res = f_write(&fp, data, chunk, &written);
            if (res != FR_OK || written != chunk) {
                spin_unlock_irqrestore(&priv->lock, write_flags);
                goto fail;
            }
            data += chunk;
            remaining -= chunk;
        }
        spin_unlock_irqrestore(&priv->lock, write_flags);

        res = f_close(&fp);
        temp_open = false;
        if (res != FR_OK) {
            goto fail;
        }

        (void)f_unlink(PERSISTENT_LFS_STORE_BAK_PATH);
        if (f_stat(PERSISTENT_LFS_STORE_PATH, &fno) == FR_OK) {
            res = f_rename(PERSISTENT_LFS_STORE_PATH, PERSISTENT_LFS_STORE_BAK_PATH);
            if (res != FR_OK) {
                goto fail;
            }
        }

        res = f_rename(PERSISTENT_LFS_STORE_TMP_PATH, PERSISTENT_LFS_STORE_PATH);
        if (res != FR_OK) {
            if (f_stat(PERSISTENT_LFS_STORE_BAK_PATH, &fno) == FR_OK) {
                (void)f_rename(PERSISTENT_LFS_STORE_BAK_PATH, PERSISTENT_LFS_STORE_PATH);
            }
            goto fail;
        }
        (void)f_unlink(PERSISTENT_LFS_STORE_BAK_PATH);
        (void)f_unlink(PERSISTENT_LFS_DELTA_PATH);
        (void)f_unlink(PERSISTENT_LFS_DELTA_BAK_PATH);
    }

    (void)f_mount(NULL, PERSISTENT_LFS_FAT_VOL, 0);
    fatfs_disk_detach();
    boot_media_release(boot_dev);

    flags = spin_lock_irqsave(&priv->lock);
    priv->dirty = false;
    priv->backing_valid = true;
    priv->journal_valid = !full_rewrite;
    if (priv->journal_valid) {
        priv->journal_first_block = first_block;
        priv->journal_last_block = last_block;
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    return BLKDEV_OK;

fail:
    if (temp_open) {
        (void)f_close(&fp);
    }
    (void)f_unlink(PERSISTENT_LFS_STORE_TMP_PATH);
    (void)f_unlink(PERSISTENT_LFS_DELTA_TMP_PATH);
    if (mounted) {
        (void)f_mount(NULL, PERSISTENT_LFS_FAT_VOL, 0);
    }
    fatfs_disk_detach();
    boot_media_release(boot_dev);
    return BLKDEV_ERR_IO;
}

static int persistent_lfs_sync(struct blkdev *dev)
{
    struct persistent_lfs_store_priv *priv = dev->priv;
    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    bool dirty = priv->dirty;
    spin_unlock_irqrestore(&priv->lock, flags);

    if (!dirty) {
        return BLKDEV_OK;
    }
    return persistent_lfs_flush_image(dev);
}

static const struct blkdev_ops persistent_lfs_store_ops = {
    .read = persistent_lfs_read,
    .prog = persistent_lfs_prog,
    .erase = persistent_lfs_erase,
    .sync = persistent_lfs_sync,
};

static struct blkdev *persistent_lfs_store_alloc(const char *name, size_t image_bytes)
{
    size_t desc_size = sizeof(struct blkdev) + sizeof(struct persistent_lfs_store_priv);
    size_t desc_pages = (desc_size + PAGE_SIZE - 1) / PAGE_SIZE;
    struct blkdev *dev = pmm_alloc_pages(desc_pages);
    if (!dev) {
        return NULL;
    }
    memset(dev, 0, desc_size);

    struct persistent_lfs_store_priv *priv =
        (struct persistent_lfs_store_priv *)(dev + 1);
    size_t data_pages = (image_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    priv->data = pmm_alloc_pages(data_pages);
    if (!priv->data) {
        pmm_free_pages(dev, desc_pages);
        return NULL;
    }

    memset(priv->data, 0xFF, image_bytes);
    priv->image_bytes = image_bytes;
    priv->data_pages = data_pages;
    priv->desc_pages = desc_pages;
    priv->dirty = false;
    priv->backing_valid = false;
    priv->journal_valid = false;
    priv->dirty_first_block = 0;
    priv->dirty_last_block = 0;
    priv->journal_first_block = 0;
    priv->journal_last_block = 0;
    spin_init(&priv->lock);

    if (uart_snprintf(dev->name, sizeof(dev->name), "%s", name) < 0) {
        pmm_free_pages(priv->data, data_pages);
        pmm_free_pages(dev, desc_pages);
        return NULL;
    }
    dev->read_size = 1;
    dev->prog_size = 1;
    dev->block_size = PERSISTENT_LFS_BLOCK_SIZE;
    dev->block_count = (uint32_t)(image_bytes / PERSISTENT_LFS_BLOCK_SIZE);
    dev->ops = &persistent_lfs_store_ops;
    dev->priv = priv;
    dev->registered = false;
    return dev;
}

struct blkdev *persistent_lfs_store_create(const char *name,
                                           bool *needs_format_out)
{
    struct blkdev *boot_dev = NULL;
    struct blkdev *store_dev = NULL;
    FATFS fs;
    FIL fp;
    FILINFO fno;
    FRESULT res;
    UINT got = 0;
    bool mounted = false;
    bool needs_format = false;
    bool have_primary = false;
    bool have_backup = false;
    bool primary_invalid = false;
    const char *image_path = PERSISTENT_LFS_STORE_PATH;
    size_t image_bytes = PERSISTENT_LFS_DEFAULT_BYTES;

    if (needs_format_out) {
        *needs_format_out = false;
    }

    boot_dev = boot_media_acquire();
    if (!boot_dev) {
        return NULL;
    }

    fatfs_disk_attach(boot_dev);
    res = f_mount(&fs, PERSISTENT_LFS_FAT_VOL, 1);
    if (res != FR_OK) {
        fatfs_disk_detach();
        boot_media_release(boot_dev);
        return NULL;
    }
    mounted = true;

    if (f_stat(PERSISTENT_LFS_STORE_PATH, &fno) == FR_OK) {
        have_primary = true;
        if (persistent_lfs_valid_image_size(fno.fsize)) {
            image_bytes = fno.fsize;
        } else {
            WARN("persistent_lfs_store: ignoring invalid image size %lu",
                 (unsigned long)fno.fsize);
            primary_invalid = true;
            have_primary = false;
            needs_format = true;
        }
    }

    if (!have_primary || needs_format) {
        if (f_stat(PERSISTENT_LFS_STORE_BAK_PATH, &fno) == FR_OK) {
            have_backup = true;
            if (persistent_lfs_valid_image_size(fno.fsize)) {
                image_bytes = fno.fsize;
                image_path = PERSISTENT_LFS_STORE_BAK_PATH;
                if (primary_invalid) {
                    (void)f_unlink(PERSISTENT_LFS_STORE_PATH);
                }
                if (f_rename(PERSISTENT_LFS_STORE_BAK_PATH,
                             PERSISTENT_LFS_STORE_PATH) == FR_OK) {
                    image_path = PERSISTENT_LFS_STORE_PATH;
                } else {
                    WARN("persistent_lfs_store: using backup image in place");
                }
                have_primary = true;
                needs_format = false;
            } else {
                WARN("persistent_lfs_store: ignoring invalid backup image size %lu",
                     (unsigned long)fno.fsize);
            }
        }
    }

    if (!have_primary && !have_backup) {
        needs_format = true;
    }

    store_dev = persistent_lfs_store_alloc(name, image_bytes);
    if (!store_dev) {
        goto fail;
    }

    if (!needs_format) {
        struct persistent_lfs_store_priv *priv = store_dev->priv;
        bool loaded_delta = false;

        res = f_open(&fp, image_path, FA_READ);
        if (res != FR_OK) {
            needs_format = true;
        } else {
            size_t remaining = priv->image_bytes;
            uint8_t *dst = priv->data;
            while (remaining > 0) {
                UINT chunk = remaining > 32768u ? 32768u : (UINT)remaining;
                res = f_read(&fp, dst, chunk, &got);
                if (res != FR_OK || got != chunk) {
                    needs_format = true;
                    break;
                }
                dst += chunk;
                remaining -= chunk;
            }
            (void)f_close(&fp);
            if (!needs_format) {
                priv->backing_valid = true;
            }
        }

        if (!needs_format && have_primary) {
            if (f_stat(PERSISTENT_LFS_DELTA_PATH, &fno) == FR_OK) {
                res = f_open(&fp, PERSISTENT_LFS_DELTA_PATH, FA_READ);
                if (res == FR_OK) {
                    loaded_delta = persistent_lfs_load_delta_file(&fp, &fno,
                                                                  store_dev, priv);
                    (void)f_close(&fp);
                }
            }

            if (!loaded_delta && f_stat(PERSISTENT_LFS_DELTA_BAK_PATH, &fno) == FR_OK) {
                res = f_open(&fp, PERSISTENT_LFS_DELTA_BAK_PATH, FA_READ);
                if (res == FR_OK) {
                    loaded_delta = persistent_lfs_load_delta_file(&fp, &fno,
                                                                  store_dev, priv);
                    (void)f_close(&fp);
                }
            }

            if (!loaded_delta) {
                priv->journal_valid = false;
                priv->journal_first_block = 0;
                priv->journal_last_block = 0;
            }
        }
    }

    (void)f_mount(NULL, PERSISTENT_LFS_FAT_VOL, 0);
    fatfs_disk_detach();
    boot_media_release(boot_dev);

    if (needs_format_out) {
        *needs_format_out = needs_format;
    }
    return store_dev;

fail:
    if (store_dev) {
        persistent_lfs_store_destroy(store_dev);
    }
    if (mounted) {
        (void)f_mount(NULL, PERSISTENT_LFS_FAT_VOL, 0);
    }
    fatfs_disk_detach();
    boot_media_release(boot_dev);
    return NULL;
}

void persistent_lfs_store_destroy(struct blkdev *dev)
{
    if (!dev) {
        return;
    }
    struct persistent_lfs_store_priv *priv = dev->priv;
    if (priv) {
        if (priv->data) {
            pmm_free_pages(priv->data, priv->data_pages);
        }
        pmm_free_pages(dev, priv->desc_pages);
    }
}
