/*
 * Boot-media subsystem tests (#414).
 *
 * The Pi 5 SDHCI bring-up takes ~50 ms of busy-wait plus a full SD
 * card init, which overruns the secondary-CPU
 * `scheduler_is_initialized` budget if it runs from the pre-scheduler
 * VFS-init path. The kernel now defers boot-media creation behind a
 * gate (`boot_media_allow_creates`) and pins the device for the
 * kernel's lifetime via a keep-alive ref so callers in tight loops
 * don't pay the 50 ms cost per acquire/release pair.
 *
 * These tests verify both behaviors against the production code path
 * (not the boot_media_test_set_device override) by injecting a
 * caller-owned ramdisk via boot_media_test_set_create_hook.
 */
#include "unity.h"
#include "../include/blkdev.h"
#include "../include/boot_media.h"
#include "../include/ramdisk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_MEDIA_TEST_BLOCK_SIZE   512u
#define BOOT_MEDIA_TEST_BLOCK_COUNT  256u

static struct blkdev *g_hook_dev;
static unsigned g_hook_call_count;

static struct blkdev *test_create_hook(void)
{
    g_hook_call_count++;
    return g_hook_dev;
}

static void boot_media_test_setup(void)
{
    g_hook_dev = NULL;
    g_hook_call_count = 0;
    boot_media_test_clear_device();
    boot_media_test_clear_production_cache();
    boot_media_test_set_create_hook(NULL);
    boot_media_test_set_creates_allowed(false);
}

static void boot_media_test_teardown(void)
{
    boot_media_test_clear_device();
    boot_media_test_set_create_hook(NULL);
    boot_media_test_clear_production_cache();
    /* Restore production state: kernel_main has already opened the
     * gate before tests run; subsequent tests may rely on it. */
    boot_media_test_set_creates_allowed(true);
    if (g_hook_dev) {
        ramdisk_destroy(g_hook_dev);
        g_hook_dev = NULL;
    }
}

/*
 * The gate is the headline #414 fix: pre-scheduler callers must see
 * NULL so the SDHCI bring-up doesn't run on the boot path. Without
 * this gate, calling boot_media_acquire from VFS init busy-waits
 * 50 ms for EMMC2 to settle and then runs the full SD init — long
 * enough that secondaries time out waiting for `scheduler_is_initialized`.
 */
static void test_boot_media_acquire_returns_null_when_creates_blocked(void)
{
    boot_media_test_setup();

    g_hook_dev = ramdisk_create("bm_gate",
                                BOOT_MEDIA_TEST_BLOCK_SIZE,
                                BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(g_hook_dev);
    boot_media_test_set_create_hook(test_create_hook);

    /* Gate closed: acquire must return NULL without invoking create. */
    struct blkdev *dev = boot_media_acquire();
    TEST_ASSERT_NULL(dev);
    TEST_ASSERT_EQUAL_UINT(0, g_hook_call_count);

    boot_media_test_teardown();
}

/*
 * Once the kernel calls boot_media_allow_creates() (or the test
 * equivalent), the gate opens and acquire reaches boot_media_create().
 * On a real Pi 5 this is where sdhci_create_bcm2712() runs.
 */
static void test_boot_media_acquire_invokes_create_when_allowed(void)
{
    boot_media_test_setup();

    g_hook_dev = ramdisk_create("bm_open",
                                BOOT_MEDIA_TEST_BLOCK_SIZE,
                                BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(g_hook_dev);
    boot_media_test_set_create_hook(test_create_hook);
    boot_media_test_set_creates_allowed(true);

    struct blkdev *dev = boot_media_acquire();
    TEST_ASSERT_EQUAL_PTR(g_hook_dev, dev);
    TEST_ASSERT_EQUAL_UINT(1, g_hook_call_count);

    boot_media_release(dev);

    boot_media_test_teardown();
}

/*
 * Keep-alive ref check. The fix in #414 raises the initial refcount
 * from 1 to 2 so the FIRST release leaves the device pinned. Without
 * the keep-alive ref, every acquire/release pair around a one-shot
 * caller (e.g. blob_autoload reading a single FAT entry) re-runs
 * boot_media_create — i.e., 50 ms settle delay + full SDHCI probe
 * per call.
 *
 * This test exercises the production code path (no test_set_device
 * override) and asserts:
 *   1. The first acquire calls the create hook exactly once.
 *   2. After release, a SECOND acquire returns the same device WITHOUT
 *      calling the hook again — proof the device is still cached.
 */
static void test_boot_media_keep_alive_ref_pins_device_after_release(void)
{
    boot_media_test_setup();

    g_hook_dev = ramdisk_create("bm_keepalive",
                                BOOT_MEDIA_TEST_BLOCK_SIZE,
                                BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(g_hook_dev);
    boot_media_test_set_create_hook(test_create_hook);
    boot_media_test_set_creates_allowed(true);

    struct blkdev *first = boot_media_acquire();
    TEST_ASSERT_EQUAL_PTR(g_hook_dev, first);
    TEST_ASSERT_EQUAL_UINT(1, g_hook_call_count);

    /* Caller releases. Without the keep-alive ref, refs would drop
     * to 0 and the device would be destroyed; the next acquire would
     * call create again. With the keep-alive ref, refs is still 1
     * and the cached device stays usable. */
    boot_media_release(first);

    struct blkdev *second = boot_media_acquire();
    TEST_ASSERT_EQUAL_PTR(g_hook_dev, second);
    TEST_ASSERT_EQUAL_UINT(1, g_hook_call_count);
    boot_media_release(second);

    boot_media_test_teardown();
}

/*
 * Multiple acquire-release pairs without re-create. Mirrors the
 * blob_autoload boot path that walks several FAT entries back to
 * back; each one goes through acquire/release. The keep-alive ref
 * means create is invoked once total, regardless of pair count.
 */
static void test_boot_media_repeated_acquire_release_does_not_recreate(void)
{
    boot_media_test_setup();

    g_hook_dev = ramdisk_create("bm_repeat",
                                BOOT_MEDIA_TEST_BLOCK_SIZE,
                                BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(g_hook_dev);
    boot_media_test_set_create_hook(test_create_hook);
    boot_media_test_set_creates_allowed(true);

    for (int i = 0; i < 5; i++) {
        struct blkdev *dev = boot_media_acquire();
        TEST_ASSERT_EQUAL_PTR(g_hook_dev, dev);
        boot_media_release(dev);
    }
    TEST_ASSERT_EQUAL_UINT(1, g_hook_call_count);

    boot_media_test_teardown();
}

/*
 * The test_set_device override is checked first in
 * boot_media_acquire and must bypass the creates-allowed gate so
 * existing kernel/tests/test_blob_autoload.c (which sets a ramdisk
 * before kernel_main has run boot_media_allow_creates() in some test
 * orderings) keeps working.
 */
static void test_boot_media_test_device_bypasses_creates_gate(void)
{
    boot_media_test_setup();

    struct blkdev *override = ramdisk_create("bm_override",
                                             BOOT_MEDIA_TEST_BLOCK_SIZE,
                                             BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(override);
    boot_media_test_set_device(override);
    /* Gate left CLOSED. */

    struct blkdev *dev = boot_media_acquire();
    TEST_ASSERT_EQUAL_PTR(override, dev);
    boot_media_release(dev);

    boot_media_test_clear_device();
    ramdisk_destroy(override);
    boot_media_test_teardown();
}

/*
 * boot_media_allow_creates() must be safe to call more than once.
 * The kernel only calls it once today (right after scheduler_init in
 * kernel_main), but a future cleanup could move the call into a
 * staged init helper; the gate is one-way so a duplicate must be a
 * no-op rather than a refcount underflow / log spam vector.
 */
static void test_boot_media_allow_creates_is_idempotent(void)
{
    boot_media_test_setup();

    boot_media_allow_creates();
    boot_media_allow_creates();
    boot_media_allow_creates();

    g_hook_dev = ramdisk_create("bm_idem",
                                BOOT_MEDIA_TEST_BLOCK_SIZE,
                                BOOT_MEDIA_TEST_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(g_hook_dev);
    boot_media_test_set_create_hook(test_create_hook);

    struct blkdev *dev = boot_media_acquire();
    TEST_ASSERT_EQUAL_PTR(g_hook_dev, dev);
    TEST_ASSERT_EQUAL_UINT(1, g_hook_call_count);
    boot_media_release(dev);

    boot_media_test_teardown();
}

int test_suite_boot_media(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_media_acquire_returns_null_when_creates_blocked);
    RUN_TEST(test_boot_media_acquire_invokes_create_when_allowed);
    RUN_TEST(test_boot_media_keep_alive_ref_pins_device_after_release);
    RUN_TEST(test_boot_media_repeated_acquire_release_does_not_recreate);
    RUN_TEST(test_boot_media_test_device_bypasses_creates_gate);
    RUN_TEST(test_boot_media_allow_creates_is_idempotent);
    return UNITY_END();
}
