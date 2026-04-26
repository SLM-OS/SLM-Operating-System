#include "boot_media.h"

#include <stdbool.h>

#include "sdhci.h"
#include "spinlock.h"

static spinlock_t g_boot_media_lock = SPINLOCK_INIT;
static struct blkdev *g_boot_media_dev;
static unsigned g_boot_media_refs;
static struct blkdev *g_boot_media_test_dev;
static unsigned g_boot_media_test_refs;

static struct blkdev *boot_media_create(void)
{
#if defined(PLATFORM_RASPI5)
    return sdhci_create_bcm2712();
#elif defined(PLATFORM_QEMU_VIRT)
    return sdhci_create_qemu_pci("boot_media");
#else
    return NULL;
#endif
}

struct blkdev *boot_media_acquire(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);

    if (g_boot_media_test_dev) {
        g_boot_media_test_refs++;
        struct blkdev *dev = g_boot_media_test_dev;
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        return dev;
    }

    if (g_boot_media_dev) {
        g_boot_media_refs++;
        struct blkdev *dev = g_boot_media_dev;
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        return dev;
    }

    spin_unlock_irqrestore(&g_boot_media_lock, flags);

    struct blkdev *dev = boot_media_create();
    if (!dev) {
        return NULL;
    }

    flags = spin_lock_irqsave(&g_boot_media_lock);
    if (g_boot_media_dev) {
        g_boot_media_refs++;
        struct blkdev *shared = g_boot_media_dev;
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        sdhci_destroy(dev);
        return shared;
    }

    g_boot_media_dev = dev;
    g_boot_media_refs = 1;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
    return dev;
}

void boot_media_release(struct blkdev *dev)
{
    if (!dev) {
        return;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);
    if (dev == g_boot_media_test_dev) {
        if (g_boot_media_test_refs != 0) {
            g_boot_media_test_refs--;
        }
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        return;
    }

    if (dev != g_boot_media_dev || g_boot_media_refs == 0) {
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        return;
    }

    g_boot_media_refs--;
    if (g_boot_media_refs != 0) {
        spin_unlock_irqrestore(&g_boot_media_lock, flags);
        return;
    }

    g_boot_media_dev = NULL;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
    sdhci_destroy(dev);
}

void boot_media_test_set_device(struct blkdev *dev)
{
    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);
    g_boot_media_test_dev = dev;
    g_boot_media_test_refs = 0;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
}

void boot_media_test_clear_device(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);
    g_boot_media_test_dev = NULL;
    g_boot_media_test_refs = 0;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
}
