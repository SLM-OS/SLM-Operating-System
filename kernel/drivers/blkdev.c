/*
 * blkdev.c - Block Device Registry for SLM-OS
 *
 * Manages registration and lookup of block storage devices.
 */

#include "../include/blkdev.h"
#include "../include/spinlock.h"
#include "../include/uart.h"
#include "../include/debug.h"

/* Registry of block devices */
static struct blkdev *devices[BLKDEV_MAX_DEVICES];
static int device_count = 0;
static spinlock_t blkdev_lock = SPINLOCK_INIT;
static bool initialized = false;

/* String comparison helper (no libc) */
static int blkdev_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

void blkdev_init(void)
{
    irq_flags_t flags = spin_lock_irqsave(&blkdev_lock);

    device_count = 0;
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        devices[i] = NULL;
    }
    initialized = true;

    spin_unlock_irqrestore(&blkdev_lock, flags);

    INFO("Block device subsystem initialized (max %d devices)",
         BLKDEV_MAX_DEVICES);
}

int blkdev_register(struct blkdev *dev)
{
    if (!dev || !dev->name[0] || !dev->ops) {
        return BLKDEV_ERR_INVAL;
    }

    if (!initialized) {
        blkdev_init();
    }

    irq_flags_t flags = spin_lock_irqsave(&blkdev_lock);

    /* Check for space */
    if (device_count >= BLKDEV_MAX_DEVICES) {
        spin_unlock_irqrestore(&blkdev_lock, flags);
        ERROR("blkdev: registry full");
        return BLKDEV_ERR_NOSPC;
    }

    /* Check for duplicate name */
    for (int i = 0; i < device_count; i++) {
        if (devices[i] && blkdev_strcmp(devices[i]->name, dev->name) == 0) {
            spin_unlock_irqrestore(&blkdev_lock, flags);
            ERROR("blkdev: device '%s' already registered", dev->name);
            return BLKDEV_ERR_EXIST;
        }
    }

    /* Add to registry */
    devices[device_count++] = dev;
    dev->registered = true;

    spin_unlock_irqrestore(&blkdev_lock, flags);

    INFO("Registered block device: %s (%u blocks x %u bytes = %u KB)",
         dev->name, dev->block_count, dev->block_size,
         (dev->block_count * dev->block_size) / 1024);

    return BLKDEV_OK;
}

int blkdev_unregister(struct blkdev *dev)
{
    if (!dev) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&blkdev_lock);

    /* Find and remove from registry */
    for (int i = 0; i < device_count; i++) {
        if (devices[i] == dev) {
            /* Shift remaining devices down */
            for (int j = i; j < device_count - 1; j++) {
                devices[j] = devices[j + 1];
            }
            devices[--device_count] = NULL;
            dev->registered = false;

            spin_unlock_irqrestore(&blkdev_lock, flags);

            INFO("Unregistered block device: %s", dev->name);
            return BLKDEV_OK;
        }
    }

    spin_unlock_irqrestore(&blkdev_lock, flags);

    ERROR("blkdev: device '%s' not found in registry", dev->name);
    return BLKDEV_ERR_NOENT;
}

struct blkdev *blkdev_find(const char *name)
{
    if (!name) {
        return NULL;
    }

    irq_flags_t flags = spin_lock_irqsave(&blkdev_lock);

    for (int i = 0; i < device_count; i++) {
        if (devices[i] && blkdev_strcmp(devices[i]->name, name) == 0) {
            struct blkdev *dev = devices[i];
            spin_unlock_irqrestore(&blkdev_lock, flags);
            return dev;
        }
    }

    spin_unlock_irqrestore(&blkdev_lock, flags);
    return NULL;
}

int blkdev_count(void)
{
    irq_flags_t flags = spin_lock_irqsave(&blkdev_lock);
    int count = device_count;
    spin_unlock_irqrestore(&blkdev_lock, flags);
    return count;
}

void blkdev_dump_info(const struct blkdev *dev)
{
    if (!dev) {
        uart_puts("blkdev: NULL device\r\n");
        return;
    }

    uart_printf("Block device: %s\r\n", dev->name);
    uart_printf("  Read size:   %u bytes\r\n", dev->read_size);
    uart_printf("  Prog size:   %u bytes\r\n", dev->prog_size);
    uart_printf("  Block size:  %u bytes\r\n", dev->block_size);
    uart_printf("  Block count: %u\r\n", dev->block_count);
    uart_printf("  Capacity:    %u KB\r\n",
                (unsigned)(blkdev_capacity(dev) / 1024));
    uart_printf("  Registered:  %s\r\n", dev->registered ? "yes" : "no");
}
