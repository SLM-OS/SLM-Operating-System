/*
 * test_sdhci.c — SDHCI block-driver integration tests.
 *
 * Stage 3 of dynamic-kernel-replace (#369). Exercises the
 * `sdhci_create_qemu_pci()` path against QEMU's `-device sdhci-pci`
 * with a 64 MB raw-file backing store (staged by the Makefile's
 * `sdhci-test-img` rule).
 *
 * Tests skip cleanly when QEMU is started without `-device
 * sdhci-pci` (`pcie_find_class` returns NULL → `sdhci_create_qemu_pci`
 * returns NULL); production-platform builds (Pi 5, Jetson) use
 * `sdhci_create()` against a fixed MMIO base instead. Hardware
 * verification of the full round-trip on `pi-5-1` is Stage 5 (#371).
 *
 * The tests reuse the FatFs harness from Stage 2 (#368) for the
 * end-to-end case: after the SDHCI driver brings up the card, we
 * format it, mount via `fatfs_disk_attach`, and run the same
 * stage→promote→rollback flow that ran against the ramdisk.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/blkdev.h"
#include "../include/sdhci.h"
#include "../include/fat32.h"
#include "../lib/fatfs/ff.h"

#include <stdint.h>
#include <string.h>

/* QEMU sdhci-pci registers a 64 MB SDHC card (per Makefile's
 * SDHCI_TEST_IMG_BYTES). Block size is 512, so block_count is
 * 131072. The driver derives this from CMD9 (CSD) at probe time. */
#define SDHCI_TEST_EXPECTED_MIN_BLOCKS    (32u * 1024u * 1024u / 512u)

#define FAT32_VOLUME_PATH                 "0:"
#define KERNEL_IMG_NAME                   FAT32_VOLUME_PATH "/kernel_2712.img"
#define TRYBOOT_IMG_NAME                  FAT32_VOLUME_PATH "/tryboot.img"

static const char kernel_payload[]  = "SLMOS-KERNEL-CANONICAL-PAYLOAD-v1";
static const char tryboot_payload[] = "TRYBOOT-CANDIDATE-IMAGE-v2";

/* mkfs work area for the FatFs end-to-end test. Same shape as
 * test_fat32.c's. */
static BYTE mkfs_work[4096];
static FATFS test_fs;

static struct blkdev *test_dev;

/* Helper: present-in-QEMU vs run-without-sdhci. */
static bool sdhci_available(void)
{
    /* Probe once per test; cheap and safe to repeat. */
    struct blkdev *d = sdhci_create_qemu_pci("probe_only");
    if (!d) {
        return false;
    }
    sdhci_destroy(d);
    return true;
}

static void test_probe_reports_capacity(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_test");
    TEST_ASSERT_NOT_NULL(test_dev);
    TEST_ASSERT_EQUAL_UINT(512, test_dev->block_size);
    TEST_ASSERT_GREATER_OR_EQUAL(SDHCI_TEST_EXPECTED_MIN_BLOCKS,
                                 test_dev->block_count);
    sdhci_destroy(test_dev);
    test_dev = NULL;
}

static void test_block0_round_trip(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_test");
    TEST_ASSERT_NOT_NULL(test_dev);

    /* Pattern: each byte = (i & 0xFF). Pick block 16 (avoids MBR /
     * any boot signature region the SDHCI controller may simulate). */
    static uint8_t pattern[512];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i & 0xFFu);
    }

    int rc = test_dev->ops->prog(test_dev, /*block=*/16, /*off=*/0,
                                 pattern, sizeof(pattern));
    TEST_ASSERT_EQUAL_INT(BLKDEV_OK, rc);

    static uint8_t verify[512];
    memset(verify, 0xAA, sizeof(verify));
    rc = test_dev->ops->read(test_dev, 16, 0, verify, sizeof(verify));
    TEST_ASSERT_EQUAL_INT(BLKDEV_OK, rc);
    TEST_ASSERT_EQUAL_MEMORY(pattern, verify, sizeof(pattern));

    sdhci_destroy(test_dev);
    test_dev = NULL;
}

static void test_multi_block_round_trip(void)
{
    /* CMD18/CMD25 multi-block path. 8 blocks = one cluster on a
     * default-formatted small FAT32. */
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_test");
    TEST_ASSERT_NOT_NULL(test_dev);

    static uint8_t multi[512 * 8];
    for (size_t i = 0; i < sizeof(multi); i++) {
        multi[i] = (uint8_t)((i * 7u + 13u) & 0xFFu);
    }

    int rc = test_dev->ops->prog(test_dev, /*block=*/100, 0,
                                 multi, sizeof(multi));
    TEST_ASSERT_EQUAL_INT(BLKDEV_OK, rc);

    static uint8_t verify[512 * 8];
    rc = test_dev->ops->read(test_dev, 100, 0, verify, sizeof(verify));
    TEST_ASSERT_EQUAL_INT(BLKDEV_OK, rc);
    TEST_ASSERT_EQUAL_MEMORY(multi, verify, sizeof(multi));

    sdhci_destroy(test_dev);
    test_dev = NULL;
}

static void test_partial_block_rejected(void)
{
    /* The diskio shim only ever issues full-block I/O; the driver
     * rejects partial-block calls rather than buffer-then-copy. Pin
     * the contract. */
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_test");
    TEST_ASSERT_NOT_NULL(test_dev);

    uint8_t buf[16];
    int rc = test_dev->ops->read(test_dev, 0, /*off=*/4, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BLKDEV_ERR_INVAL, rc);

    rc = test_dev->ops->read(test_dev, 0, 0, buf, /*size=*/13);
    TEST_ASSERT_EQUAL_INT(BLKDEV_ERR_INVAL, rc);

    sdhci_destroy(test_dev);
    test_dev = NULL;
}

static void test_out_of_range_rejected(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_test");
    TEST_ASSERT_NOT_NULL(test_dev);

    uint8_t buf[512];
    /* block_count is the first invalid block. */
    uint32_t bad_block = test_dev->block_count;
    int rc = test_dev->ops->read(test_dev, bad_block, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(BLKDEV_ERR_INVAL, rc);

    sdhci_destroy(test_dev);
    test_dev = NULL;
}

static void test_fatfs_round_trip_on_sdhci(void)
{
    /* End-to-end: format the SDHCI-backed card, attach to FatFs,
     * write `kernel_2712.img` (LFN) and `tryboot.img` (8.3),
     * promote, read back. Mirrors the Stage 2 ramdisk flow. */
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    test_dev = sdhci_create_qemu_pci("sdhci_fatfs");
    TEST_ASSERT_NOT_NULL(test_dev);

    /* Detach any prior FatFs binding (e.g., a stranded ramdisk from
     * test_fat32.c if a teardown was skipped). */
    f_mount(NULL, FAT32_VOLUME_PATH, 0);
    fatfs_disk_detach();

    fatfs_disk_attach(test_dev);

    /* Format: MBR + FAT32 partition 1, just like test_fat32.c. */
    LBA_t plist[] = { 100, 0, 0, 0 };
    FRESULT res = f_fdisk(0, plist, mkfs_work);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    MKFS_PARM opt = { .fmt = FM_FAT32, .n_fat = 1, };
    res = f_mkfs(FAT32_VOLUME_PATH, &opt, mkfs_work, sizeof(mkfs_work));
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    res = f_mount(&test_fs, FAT32_VOLUME_PATH, 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    /* Write LFN file (kernel_2712.img exceeds 8.3). */
    FIL fp;
    UINT n;
    res = f_open(&fp, KERNEL_IMG_NAME, FA_WRITE | FA_CREATE_ALWAYS);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    res = f_write(&fp, kernel_payload, sizeof(kernel_payload), &n);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(sizeof(kernel_payload), n);
    f_close(&fp);

    /* Write 8.3 staged image. */
    res = f_open(&fp, TRYBOOT_IMG_NAME, FA_WRITE | FA_CREATE_ALWAYS);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    res = f_write(&fp, tryboot_payload, sizeof(tryboot_payload), &n);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    f_close(&fp);

    /* `kernel promote`: unlink old kernel + rename. */
    res = f_unlink(KERNEL_IMG_NAME);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    res = f_rename(TRYBOOT_IMG_NAME, KERNEL_IMG_NAME);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    /* Verify post-promote: kernel_2712.img holds tryboot bytes. */
    res = f_open(&fp, KERNEL_IMG_NAME, FA_READ);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    static uint8_t verify[256];
    res = f_read(&fp, verify, sizeof(verify), &n);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(sizeof(tryboot_payload), n);
    TEST_ASSERT_EQUAL_MEMORY(tryboot_payload, verify, sizeof(tryboot_payload));
    f_close(&fp);

    /* Cleanup. */
    f_mount(NULL, FAT32_VOLUME_PATH, 0);
    fatfs_disk_detach();
    sdhci_destroy(test_dev);
    test_dev = NULL;
}

int test_suite_sdhci(void)
{
    UnityBegin("SDHCI / SD Host Controller integration tests");

    RUN_TEST(test_probe_reports_capacity);
    RUN_TEST(test_block0_round_trip);
    RUN_TEST(test_multi_block_round_trip);
    RUN_TEST(test_partial_block_rejected);
    RUN_TEST(test_out_of_range_rejected);
    RUN_TEST(test_fatfs_round_trip_on_sdhci);

    return UnityEnd();
}
