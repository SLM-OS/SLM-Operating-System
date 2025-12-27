/*
 * ramdisk.c - RAM Disk Block Device for SLM-OS
 *
 * Provides a RAM-backed block device for testing filesystems.
 * Uses PMM for memory allocation.
 */

#include "../include/ramdisk.h"
#include "../include/pmm.h"
#include "../include/spinlock.h"
#include "../include/uart.h"
#include "../include/debug.h"

/* RAM disk private data */
struct ramdisk_priv {
    uint8_t *data;           /* Backing memory */
    size_t size;             /* Total size in bytes */
    size_t data_pages;       /* Pages allocated for data */
    size_t desc_pages;       /* Pages allocated for descriptor */
    spinlock_t lock;         /* Protects concurrent access */
};

/* Memory helpers (no libc) */
static void ramdisk_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
}

static void ramdisk_memset(void *dst, int val, size_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)val;
    }
}

static void ramdisk_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Block device operations */

static int ramdisk_read(struct blkdev *dev, uint32_t block, uint32_t off,
                        void *buffer, uint32_t size)
{
    struct ramdisk_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size + off;

    if (offset + size > priv->size) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    ramdisk_memcpy(buffer, priv->data + offset, size);
    spin_unlock_irqrestore(&priv->lock, flags);

    return BLKDEV_OK;
}

static int ramdisk_prog(struct blkdev *dev, uint32_t block, uint32_t off,
                        const void *buffer, uint32_t size)
{
    struct ramdisk_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size + off;

    if (offset + size > priv->size) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    ramdisk_memcpy(priv->data + offset, buffer, size);
    spin_unlock_irqrestore(&priv->lock, flags);

    return BLKDEV_OK;
}

static int ramdisk_erase(struct blkdev *dev, uint32_t block)
{
    struct ramdisk_priv *priv = dev->priv;
    size_t offset = (size_t)block * dev->block_size;

    if (block >= dev->block_count) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&priv->lock);
    /* Flash typically erases to 0xFF */
    ramdisk_memset(priv->data + offset, 0xFF, dev->block_size);
    spin_unlock_irqrestore(&priv->lock, flags);

    return BLKDEV_OK;
}

static int ramdisk_sync(struct blkdev *dev)
{
    (void)dev;
    /* RAM disk has no cache to sync */
    return BLKDEV_OK;
}

static const struct blkdev_ops ramdisk_ops = {
    .read  = ramdisk_read,
    .prog  = ramdisk_prog,
    .erase = ramdisk_erase,
    .sync  = ramdisk_sync,
};

struct blkdev *ramdisk_create(const char *name,
                              uint32_t block_size,
                              uint32_t block_count)
{
    if (!name || block_size == 0 || block_count == 0) {
        ERROR("ramdisk: invalid parameters");
        return NULL;
    }

    /* Allocate device descriptor and private data together */
    size_t desc_size = sizeof(struct blkdev) + sizeof(struct ramdisk_priv);
    size_t desc_pages = (desc_size + PAGE_SIZE - 1) / PAGE_SIZE;

    struct blkdev *dev = pmm_alloc_pages(desc_pages);
    if (!dev) {
        ERROR("ramdisk: failed to allocate descriptor (%zu pages)", desc_pages);
        return NULL;
    }

    /* Zero the descriptor */
    ramdisk_memset(dev, 0, desc_size);

    struct ramdisk_priv *priv = (struct ramdisk_priv *)(dev + 1);

    /* Allocate backing memory */
    size_t data_size = (size_t)block_size * block_count;
    size_t data_pages = (data_size + PAGE_SIZE - 1) / PAGE_SIZE;

    priv->data = pmm_alloc_pages(data_pages);
    if (!priv->data) {
        ERROR("ramdisk: failed to allocate %zu pages for data", data_pages);
        pmm_free_pages(dev, desc_pages);
        return NULL;
    }

    priv->size = data_size;
    priv->data_pages = data_pages;
    priv->desc_pages = desc_pages;
    spin_init(&priv->lock);

    /* Initialize to erased state (0xFF like flash) */
    ramdisk_memset(priv->data, 0xFF, data_size);

    /* Configure device */
    ramdisk_strcpy(dev->name, name, BLKDEV_MAX_NAME);
    dev->read_size = 1;          /* Can read any size */
    dev->prog_size = 1;          /* Can program any size */
    dev->block_size = block_size;
    dev->block_count = block_count;
    dev->ops = &ramdisk_ops;
    dev->priv = priv;
    dev->registered = false;

    INFO("Created RAM disk '%s': %u KB (%u blocks x %u bytes)",
         name, (unsigned)(data_size / 1024), block_count, block_size);

    return dev;
}

struct blkdev *ramdisk_create_default(const char *name)
{
    return ramdisk_create(name,
                          RAMDISK_DEFAULT_BLOCK_SIZE,
                          RAMDISK_DEFAULT_BLOCK_COUNT);
}

void ramdisk_destroy(struct blkdev *dev)
{
    if (!dev) {
        return;
    }

    struct ramdisk_priv *priv = dev->priv;
    if (!priv) {
        return;
    }

    INFO("Destroying RAM disk '%s'", dev->name);

    /* Free backing memory */
    if (priv->data) {
        pmm_free_pages(priv->data, priv->data_pages);
    }

    /* Free descriptor */
    pmm_free_pages(dev, priv->desc_pages);
}
