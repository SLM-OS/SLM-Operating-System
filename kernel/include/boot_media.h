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

#endif /* BOOT_MEDIA_H */
