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
#define PERSISTENT_LFS_STORE_DIR      "0:/slmstore"
#define PERSISTENT_LFS_BLOCK_SIZE     4096u
#define PERSISTENT_LFS_DEFAULT_BYTES  (8u * 1024u * 1024u)
#define PERSISTENT_LFS_FAT_VOL        "0:"

struct persistent_lfs_store_priv {
    uint8_t *data;
    size_t image_bytes;
    size_t data_pages;
    size_t desc_pages;
    bool dirty;
    spinlock_t lock;
};

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
    priv->dirty = true;
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
    priv->dirty = true;
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

    (void)f_unlink(PERSISTENT_LFS_STORE_TMP_PATH);
    res = f_open(&fp, PERSISTENT_LFS_STORE_TMP_PATH,
                 FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        goto fail;
    }
    temp_open = true;

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    const uint8_t *data = priv->data;
    size_t remaining = priv->image_bytes;
    while (remaining > 0) {
        UINT chunk = remaining > 32768u ? 32768u : (UINT)remaining;
        res = f_write(&fp, data, chunk, &written);
        if (res != FR_OK || written != chunk) {
            spin_unlock_irqrestore(&priv->lock, flags);
            goto fail;
        }
        data += chunk;
        remaining -= chunk;
    }
    spin_unlock_irqrestore(&priv->lock, flags);

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

    (void)f_mount(NULL, PERSISTENT_LFS_FAT_VOL, 0);
    fatfs_disk_detach();
    boot_media_release(boot_dev);

    flags = spin_lock_irqsave(&priv->lock);
    priv->dirty = false;
    spin_unlock_irqrestore(&priv->lock, flags);
    return BLKDEV_OK;

fail:
    if (temp_open) {
        (void)f_close(&fp);
    }
    (void)f_unlink(PERSISTENT_LFS_STORE_TMP_PATH);
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
        if (fno.fsize >= PERSISTENT_LFS_BLOCK_SIZE &&
            (fno.fsize % PERSISTENT_LFS_BLOCK_SIZE) == 0) {
            image_bytes = fno.fsize;
        } else {
            WARN("persistent_lfs_store: ignoring invalid image size %lu",
                 (unsigned long)fno.fsize);
            needs_format = true;
        }
    } else {
        needs_format = true;
    }

    store_dev = persistent_lfs_store_alloc(name, image_bytes);
    if (!store_dev) {
        goto fail;
    }

    if (!needs_format) {
        struct persistent_lfs_store_priv *priv = store_dev->priv;
        res = f_open(&fp, PERSISTENT_LFS_STORE_PATH, FA_READ);
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
