/*
 * test_fat32.c — FatFs integration tests with the ramdisk stub block
 * device. Stage 2 of dynamic-kernel-replace (#368).
 *
 * Validates the FAT32 round-trip operations needed for staged kernel
 * replacement:
 *   - Format an in-memory disk with MBR + FAT32 partition 1.
 *   - Mount via FF_MULTI_PARTITION (volume 0 → drive 0, partition 1).
 *   - Read/write LFN-named files (kernel_2712.img exceeds 8.3).
 *   - Read/write 8.3-named files (tryboot.img).
 *   - Rename across the LFN boundary (`kernel promote` does this).
 *   - Unlink (`kernel rollback` does this).
 *
 * The MMIO block driver lands in Stage 3 (#369). Until then the
 * ramdisk-backed blkdev stands in for the real EMMC2 controller.
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/blkdev.h"
#include "../include/ramdisk.h"
#include "../include/fat32.h"
#include "../lib/fatfs/ff.h"

#include <stdint.h>
#include <string.h>

/* ---- Test parameters. ----
 *
 * 48 MB. Smaller volumes (≤32 MB) trip the FAT32 minimum: with the
 * MBR's first-track gap (~63 sectors) eating into partition 1, the
 * data area on a 32 MB disk yields ~65000 clusters at 512-byte
 * granularity — just below `MAX_FAT16` (65525) — and FatFs's f_mkfs
 * aborts with `FR_MKFS_ABORTED`. 48 MB lifts the cluster count to
 * ~97000 with comfortable margin.
 *
 * The LittleFS test ramdisk is 32 MB but lives in a different
 * suite's lifecycle — created/destroyed around its own tests, no
 * overlap with this one. */
#define FAT32_BLOCK_SIZE        512u
#define FAT32_BLOCK_COUNT       (48u * 1024u * 1024u / 512u)   /* 48 MB */
#define FAT32_VOLUME_PATH       "0:"

#define KERNEL_IMG_NAME         FAT32_VOLUME_PATH "/kernel_2712.img"
#define TRYBOOT_IMG_NAME        FAT32_VOLUME_PATH "/tryboot.img"
#define CONFIG_TXT_NAME         FAT32_VOLUME_PATH "/config.txt"

/* Test payloads — small but distinguishable. */
static const char kernel_payload[]  = "SLMOS-KERNEL-CANONICAL-PAYLOAD-v1";
static const char tryboot_payload[] = "TRYBOOT-CANDIDATE-IMAGE-v2";
static const char config_payload[]  = "[all]\nkernel=kernel_2712.img\n";

/* FatFs needs a work buffer of >= sector size for f_mkfs(); 4 KB is
 * a common, comfortable size. */
static BYTE mkfs_work[4096];

/* The mounted volume's FATFS struct must outlive every f_* call —
 * FatFs stores per-volume state in this struct, not internally. */
static FATFS test_fs;

static struct blkdev *test_dev;

/* ---- Setup / teardown helpers ---- */

static void make_ramdisk(void)
{
    test_dev = ramdisk_create("fat32_test",
                              FAT32_BLOCK_SIZE,
                              FAT32_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL_MESSAGE(test_dev,
                                 "ramdisk_create() returned NULL — PMM exhaustion?");
    fatfs_disk_attach(test_dev);
    TEST_ASSERT_EQUAL_PTR(test_dev, fatfs_disk_current());
}

static void destroy_ramdisk(void)
{
    /* f_mount with NULL FATFS releases the volume; safe to call even
     * if a prior test failed before mounting. */
    f_mount(NULL, FAT32_VOLUME_PATH, 0);
    fatfs_disk_detach();
    if (test_dev) {
        ramdisk_destroy(test_dev);
        test_dev = NULL;
    }
}

static void format_with_mbr_partition(void)
{
    /* Step 1: create the MBR partition table. plist[] is in entries
     * per primary partition; values 1..100 mean "% of disk", 0
     * terminates. {100, 0, 0, 0} → one primary covering the whole
     * disk, which the SLMOS partition does on the production card. */
    LBA_t plist[] = { 100, 0, 0, 0 };
    FRESULT res = f_fdisk(0, plist, mkfs_work);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);   /* f_fdisk(MBR) */

    /* Step 2: format partition 1 as FAT32. FF_MULTI_PARTITION = 1
     * means "0:" maps via VolToPart[] to (pdrv 0, partition 1). */
    MKFS_PARM opt = {
        .fmt     = FM_FAT32,    /* explicitly FAT32, not auto */
        .n_fat   = 1,           /* 1 FAT copy is enough for tests */
        .align   = 0,           /* default alignment */
        .n_root  = 0,           /* unused on FAT32 */
        .au_size = 0,           /* default cluster size */
    };
    res = f_mkfs(FAT32_VOLUME_PATH, &opt, mkfs_work, sizeof(mkfs_work));
    TEST_ASSERT_EQUAL_INT(FR_OK, res);   /* f_mkfs(FAT32) */
}

static void mount_volume(void)
{
    /* opt = 1 forces an immediate mount (fail-fast on bad volume).
     * Without it, FatFs defers mounting to the first f_open call,
     * which would mask geometry / format errors. */
    FRESULT res = f_mount(&test_fs, FAT32_VOLUME_PATH, 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);   /* f_mount("0:") */
}

static void write_file(const char *path, const void *data, UINT len)
{
    FIL fp;
    FRESULT res = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    UINT written = 0;
    res = f_write(&fp, data, len, &written);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(len, written);

    res = f_close(&fp);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
}

static void read_and_compare(const char *path,
                             const void *expected, UINT len)
{
    FIL fp;
    FRESULT res = f_open(&fp, path, FA_READ);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    /* Slightly oversized buffer so a too-long file would also fail.
     * 256 B comfortably exceeds every payload defined above. */
    BYTE buf[256];
    UINT got = 0;
    res = f_read(&fp, buf, sizeof(buf), &got);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(len, got);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, len);

    f_close(&fp);
}

/* ---- Tests ---- */

static void test_format_and_mount(void)
{
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    /* Sanity: a freshly-formatted volume has a free space close to
     * the partition size. Don't pin an exact number (cluster overhead
     * varies) — just assert > 16 MB free. */
    DWORD free_clust;
    FATFS *fs;
    FRESULT res = f_getfree(FAT32_VOLUME_PATH, &free_clust, &fs);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    /* Convert clusters → bytes via the volume's cluster size. */
    DWORD cluster_size = fs->csize * FAT32_BLOCK_SIZE;
    DWORD free_bytes = free_clust * cluster_size;
    TEST_ASSERT_GREATER_THAN(16u * 1024u * 1024u, free_bytes);

    destroy_ramdisk();
}

static void test_lfn_file_round_trip(void)
{
    /* `kernel_2712.img` is the production filename. Basename is 11
     * chars — exceeds 8.3, so this exercises the FF_USE_LFN path. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    write_file(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));
    read_and_compare(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    destroy_ramdisk();
}

static void test_8_3_file_round_trip(void)
{
    /* `tryboot.img` and `config.txt` both fit 8.3 — covers the
     * fast path that doesn't need the LFN engine. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    write_file(TRYBOOT_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));
    write_file(CONFIG_TXT_NAME,  config_payload,  sizeof(config_payload));

    read_and_compare(TRYBOOT_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));
    read_and_compare(CONFIG_TXT_NAME,  config_payload,  sizeof(config_payload));

    destroy_ramdisk();
}

static void test_promote_rename_overwrites_lfn(void)
{
    /* Mirrors the `kernel promote` flow exactly:
     *   1. `kernel_2712.img` is the live production kernel (LFN).
     *   2. `tryboot.img` is the staged candidate (8.3).
     *   3. Promote = rename tryboot.img over kernel_2712.img.
     *
     * FatFs's f_rename only renames; if the destination exists it
     * fails with FR_EXIST. So `kernel promote` must f_unlink the old
     * kernel first, then f_rename. Test that flow. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    write_file(KERNEL_IMG_NAME,  kernel_payload,  sizeof(kernel_payload));
    write_file(TRYBOOT_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));

    FRESULT res = f_unlink(KERNEL_IMG_NAME);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    res = f_rename(TRYBOOT_IMG_NAME, KERNEL_IMG_NAME);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    /* Post-state: tryboot.img gone, kernel_2712.img holds the
     * tryboot payload (the old kernel content was unlinked). */
    FILINFO fno;
    res = f_stat(TRYBOOT_IMG_NAME, &fno);
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, res);

    read_and_compare(KERNEL_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));

    destroy_ramdisk();
}

static void test_rollback_unlink(void)
{
    /* `kernel rollback` removes a staged tryboot.img without
     * touching the live kernel. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    write_file(KERNEL_IMG_NAME,  kernel_payload,  sizeof(kernel_payload));
    write_file(TRYBOOT_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));

    FRESULT res = f_unlink(TRYBOOT_IMG_NAME);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    FILINFO fno;
    res = f_stat(TRYBOOT_IMG_NAME, &fno);
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, res);

    /* Live kernel untouched. */
    read_and_compare(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    destroy_ramdisk();
}

static void test_overwrite_preserves_volume_state(void)
{
    /* Overwriting an existing LFN file with FA_CREATE_ALWAYS
     * truncates and replaces the content — this is what
     * `kernel stage` does on a re-stage. The volume should still
     * mount cleanly afterwards. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    write_file(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    /* Stage a tryboot image; re-stage with different content. */
    write_file(TRYBOOT_IMG_NAME, tryboot_payload, sizeof(tryboot_payload));
    static const char tryboot_v2[] = "TRYBOOT-CANDIDATE-IMAGE-v2-RESTAGED";
    write_file(TRYBOOT_IMG_NAME, tryboot_v2, sizeof(tryboot_v2));

    read_and_compare(TRYBOOT_IMG_NAME, tryboot_v2, sizeof(tryboot_v2));
    /* And the canonical kernel still reads correctly. */
    read_and_compare(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    destroy_ramdisk();
}

static void test_attach_detach_returns_current(void)
{
    /* Independent micro-test: the attach/detach contract itself.
     * No FatFs operations — purely the pointer plumbing. Useful
     * because Stage 4 will rely on `fatfs_disk_current()` to decide
     * whether the kernel commands are usable. */
    /* If this fires, an earlier test forgot to detach. */
    TEST_ASSERT_NULL(fatfs_disk_current());

    struct blkdev *dev = ramdisk_create("attach_test", 512, 256);
    TEST_ASSERT_NOT_NULL(dev);

    fatfs_disk_attach(dev);
    TEST_ASSERT_EQUAL_PTR(dev, fatfs_disk_current());

    fatfs_disk_detach();
    TEST_ASSERT_NULL(fatfs_disk_current());

    ramdisk_destroy(dev);
}

static void test_mount_with_no_disk_returns_not_ready(void)
{
    /* `kernel status` may be invoked before any blkdev is wired in
     * (e.g., on platforms where the SDHCI driver isn't built). The
     * shim's STA_NOINIT propagates through f_mount as FR_NOT_READY,
     * which the command must surface as "not available" rather than
     * crash. Pin the contract here. */
    TEST_ASSERT_NULL(fatfs_disk_current());

    FRESULT res = f_mount(&test_fs, FAT32_VOLUME_PATH, 1);
    TEST_ASSERT_EQUAL_INT(FR_NOT_READY, res);

    /* And clean up the no-op mount registration so subsequent
     * tests start from a clean slate. */
    f_mount(NULL, FAT32_VOLUME_PATH, 0);
}

static void test_open_missing_file_returns_no_file(void)
{
    /* `kernel status` reads file existence via f_stat / f_open. The
     * "no staged image" path returns FR_NO_FILE — distinct from
     * FR_DISK_ERR or FR_NO_FILESYSTEM, which would indicate a real
     * problem. Lock that error code in. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    FILINFO fno;
    FRESULT res = f_stat(TRYBOOT_IMG_NAME, &fno);
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, res);

    FIL fp;
    res = f_open(&fp, TRYBOOT_IMG_NAME, FA_READ);
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, res);

    destroy_ramdisk();
}

static void test_unmount_remount_cycle_preserves_data(void)
{
    /* Detach + re-attach the same backing store and confirm a file
     * written before the cycle reads back identical bytes. This is
     * the path `kernel rollback` uses if it dismounts to flush
     * before re-arming the volume — a regression here would silently
     * corrupt the LFN directory entry on real hardware. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();
    write_file(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    /* First half: full unmount + detach, leaving the ramdisk
     * untouched. The data should survive in the ramdisk's bytes. */
    FRESULT res = f_mount(NULL, FAT32_VOLUME_PATH, 0);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    fatfs_disk_detach();
    TEST_ASSERT_NULL(fatfs_disk_current());

    /* Second half: re-attach + re-mount the same backing store. */
    fatfs_disk_attach(test_dev);
    res = f_mount(&test_fs, FAT32_VOLUME_PATH, 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    read_and_compare(KERNEL_IMG_NAME, kernel_payload, sizeof(kernel_payload));

    destroy_ramdisk();
}

static void test_large_file_spans_clusters(void)
{
    /* Production kernel images are several MB and span many FAT32
     * clusters. The default cluster size FatFs picks for a 48 MB
     * volume is small (a few KB), so a 64 KB write is guaranteed to
     * cross at least one cluster boundary. Confirms the diskio shim
     * passes multi-sector reads/writes through correctly. */
    make_ramdisk();
    format_with_mbr_partition();
    mount_volume();

    /* Build a deterministic 64 KB pattern: each byte is its index
     * mod 256. Easy to verify on read-back without storing two
     * 64 KB buffers in BSS. */
    static uint8_t large_buf[64 * 1024];
    for (size_t i = 0; i < sizeof(large_buf); i++) {
        large_buf[i] = (uint8_t)(i & 0xFFu);
    }
    write_file(KERNEL_IMG_NAME, large_buf, sizeof(large_buf));

    FIL fp;
    FRESULT res = f_open(&fp, KERNEL_IMG_NAME, FA_READ);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    /* Read in two halves to exercise mid-file seek-less continuation,
     * which is what the staging-state-machine readers will do. */
    static uint8_t verify_buf[64 * 1024];
    UINT got = 0;
    res = f_read(&fp, verify_buf, sizeof(verify_buf) / 2, &got);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(sizeof(verify_buf) / 2, got);
    res = f_read(&fp, verify_buf + sizeof(verify_buf) / 2,
                 sizeof(verify_buf) / 2, &got);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_UINT(sizeof(verify_buf) / 2, got);
    f_close(&fp);

    TEST_ASSERT_EQUAL_MEMORY(large_buf, verify_buf, sizeof(large_buf));

    destroy_ramdisk();
}

int test_suite_fat32(void)
{
    UnityBegin("FAT32 / FatFs integration tests");

    RUN_TEST(test_attach_detach_returns_current);
    RUN_TEST(test_mount_with_no_disk_returns_not_ready);
    RUN_TEST(test_format_and_mount);
    RUN_TEST(test_open_missing_file_returns_no_file);
    RUN_TEST(test_8_3_file_round_trip);
    RUN_TEST(test_lfn_file_round_trip);
    RUN_TEST(test_promote_rename_overwrites_lfn);
    RUN_TEST(test_rollback_unlink);
    RUN_TEST(test_overwrite_preserves_volume_state);
    RUN_TEST(test_unmount_remount_cycle_preserves_data);
    RUN_TEST(test_large_file_spans_clusters);

    return UnityEnd();
}
