#ifndef BOOT_MEDIA_H
#define BOOT_MEDIA_H

#include "blkdev.h"

/*
 * Acquire the shared boot media block device, if one exists on this platform.
 *
 * The returned device remains owned by the boot-media subsystem. Callers must
 * pair every successful acquire with boot_media_release().
 */
struct blkdev *boot_media_acquire(void);

/*
 * Release a device previously returned by boot_media_acquire().
 */
void boot_media_release(struct blkdev *dev);

/*
 * Test-only hook: override boot-media acquisition with a caller-owned blkdev.
 * The boot-media subsystem will not destroy the injected device.
 */
void boot_media_test_set_device(struct blkdev *dev);
void boot_media_test_clear_device(void);

/*
 * Permit lazy creation of the shared boot-media device. Must be called
 * before the first boot_media_acquire that should actually back a
 * device. Until called, boot_media_acquire returns NULL — needed on
 * Pi 5 because the SDHCI bring-up cost (#414 settle delay + full SD
 * init) overruns the pre-scheduler init budget if invoked from the
 * VFS-init path. The kernel calls this once after the scheduler is
 * running.
 */
void boot_media_allow_creates(void);

/*
 * Test-only hook: toggle the creates-allowed gate without going
 * through boot_media_allow_creates(). Used by test_boot_media.c to
 * verify gate-blocked behavior in a build where the kernel main
 * has already opened the gate.
 */
void boot_media_test_set_creates_allowed(bool allowed);

/*
 * Test-only hook: replace the platform-specific create function
 * (sdhci_create_bcm2712 on Pi 5, sdhci_create_qemu_pci on QEMU)
 * with a caller-supplied factory. Tests use this to inject a
 * ramdisk and exercise the keep-alive ref logic without needing
 * actual SDHCI hardware. Pass NULL to restore the platform default.
 */
typedef struct blkdev *(*boot_media_create_hook_t)(void);
void boot_media_test_set_create_hook(boot_media_create_hook_t hook);

/*
 * Test-only hook: drop the pinned production device + refcount
 * without calling sdhci_destroy. Caller is responsible for
 * destroying any injected hook device. Used to isolate tests that
 * exercise the production-create path from later tests.
 */
void boot_media_test_clear_production_cache(void);

#endif /* BOOT_MEDIA_H */
