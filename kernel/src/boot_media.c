#include "boot_media.h"

#include <stdbool.h>

#include "sdhci.h"
#include "spinlock.h"

static spinlock_t g_boot_media_lock = SPINLOCK_INIT;
static struct blkdev *g_boot_media_dev;
static unsigned g_boot_media_refs;
static struct blkdev *g_boot_media_test_dev;
static unsigned g_boot_media_test_refs;

static boot_media_create_hook_t g_test_create_hook;

static struct blkdev *boot_media_create(void)
{
    if (g_test_create_hook) {
        return g_test_create_hook();
    }
#if defined(PLATFORM_RASPI5)
    return sdhci_create_bcm2712();
#elif defined(PLATFORM_QEMU_VIRT)
    return sdhci_create_qemu_pci("boot_media");
#else
    return NULL;
#endif
}

/* #414 gate: only run boot_media_create() after the kernel has
 * cleared this. The SDHCI bring-up takes ~50 ms of busy-wait + a
 * full SD card init, and pulling that into the pre-scheduler boot
 * path makes secondary CPUs miss their `scheduler_is_initialized`
 * deadline. Cleared by the kernel once scheduler is up. */
static volatile bool g_boot_media_creates_allowed = false;

void boot_media_allow_creates(void) { g_boot_media_creates_allowed = true; }

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

    if (!g_boot_media_creates_allowed) {
        return NULL;
    }

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

    /* Pin the production device for the kernel's lifetime: take a
     * keep-alive ref alongside the caller's ref so refcount never
     * drops to 0 once we've done the first create. boot_media_create()
     * is expensive on Pi 5 (~50 ms settle delay + full SDHCI probe),
     * so callers that acquire-use-release in tight loops (#414 WIP:
     * runtime_blob_read_fat_path) should not pay that cost per call. */
    g_boot_media_dev = dev;
    g_boot_media_refs = 2;
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

void boot_media_test_set_creates_allowed(bool allowed)
{
    g_boot_media_creates_allowed = allowed;
}

void boot_media_test_set_create_hook(boot_media_create_hook_t hook)
{
    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);
    g_test_create_hook = hook;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
}

void boot_media_test_clear_production_cache(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_boot_media_lock);
    g_boot_media_dev = NULL;
    g_boot_media_refs = 0;
    spin_unlock_irqrestore(&g_boot_media_lock, flags);
}
