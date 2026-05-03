/*
 * test_kernel_cmd.c — `kernel` admin command surface tests.
 *
 * Stage 4 of dynamic-kernel-replace (#370). End-to-end exercise of
 * `kernel status / stage / activate / promote / rollback` against
 * the real driver stack:
 *
 *   /mnt/files (LittleFS-backed filesystem)  → source for `kernel stage`
 *   QEMU sdhci-pci + FAT32 partition 1    → destination
 *
 * Tests skip cleanly with `TEST_IGNORE` when QEMU is started without
 * `-device sdhci-pci` (production RASPI5 builds use the real card).
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/blkdev.h"
#include "../include/boot_media.h"
#include "../include/ramdisk.h"
#include "../include/sdhci.h"
#include "../include/fat32.h"
#include "../include/shell.h"
#include "../include/vfs.h"
#include "../include/sha256.h"
#include "../include/littlefs_slm.h"
#include "../include/littlefs_vfs.h"
#include "../lib/fatfs/ff.h"
#include "../lib/littlefs/lfs.h"

#include <stdint.h>
#include <string.h>

/* The test source file lives in /mnt/files and is staged to the
 * SD card. Content is a fixed-size deterministic pattern so the
 * hash is reproducible across runs. */
#define SOURCE_VFS_PATH       "/mnt/files/test_kernel.img"
#define SOURCE_LFS_PATH       "/test_kernel.img"   /* relative to LFS root */
#define SOURCE_PAYLOAD_SIZE   (16u * 1024u)        /* 16 KB; spans many FAT clusters */

#define VOL                   "0:"
#define KERNEL_IMG_PATH       VOL "/kernel_2712.img"
#define TRYBOOT_IMG_PATH      VOL "/tryboot.img"
#define TRYBOOT_SHA_PATH      VOL "/tryboot.sha"

/* The test kernel mounts /mnt/files at boot (main.c). We piggy-back
 * on that mount rather than building a second one — vfs_mount
 * would reject the duplicate path. The test owns only the source
 * file lifecycle: create on setup, delete on teardown. */
static struct lfs_mount *g_files_mnt;

/* Single shared payload buffer — content is deterministic (byte i =
 * i & 0xFF) so the SHA-256 below is fixed and can be pinned in
 * test_stage_writes_correct_sha256. */
static uint8_t source_payload[SOURCE_PAYLOAD_SIZE];

/* SHA-256 of `source_payload` as initialized by build_source_payload().
 * Computed once at suite setup and reused across tests that verify
 * the sidecar matches. */
static char expected_sha_hex[SHA256_HEX_LEN + 1];

static BYTE mkfs_work[4096];

/* ---- helpers ---- */

static void build_source_payload(void)
{
    for (size_t i = 0; i < SOURCE_PAYLOAD_SIZE; i++) {
        source_payload[i] = (uint8_t)(i & 0xFFu);
    }
    uint8_t digest[SHA256_DIGEST_LEN];
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, source_payload, SOURCE_PAYLOAD_SIZE);
    sha256_final(&ctx, digest);
    sha256_bytes_to_hex(digest, expected_sha_hex);
}

static bool sdhci_available(void)
{
    struct blkdev *d = sdhci_create_qemu_pci("kc_probe");
    if (!d) return false;
    sdhci_destroy(d);
    return true;
}

/*
 * Locate the boot-time /mnt/files LittleFS mount (set up by main.c)
 * and write the source payload into it. Tears down only the file,
 * not the mount.
 */
static void setup_source_vfs(void)
{
    const char *subpath = NULL;
    g_files_mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL_MESSAGE(g_files_mnt,
                                 "/mnt/files not mounted — main.c boot path missing?");

    /* Write the test payload. CREATE_ALWAYS pattern via TRUNC. */
    int fh = littlefs_file_open(g_files_mnt, SOURCE_LFS_PATH,
                                 LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fh >= 0);
    int written = littlefs_file_write(g_files_mnt, fh,
                                       source_payload, SOURCE_PAYLOAD_SIZE);
    TEST_ASSERT_EQUAL_INT((int)SOURCE_PAYLOAD_SIZE, written);
    littlefs_file_close(g_files_mnt, fh);
}

static void teardown_source_vfs(void)
{
    if (!g_files_mnt) return;
    /* Best-effort delete — the file may already be gone if a
     * previous test cleaned up. */
    int fh = littlefs_file_open(g_files_mnt, SOURCE_LFS_PATH, LFS_O_RDONLY);
    if (fh >= 0) {
        littlefs_file_close(g_files_mnt, fh);
        /* No littlefs_remove_path in tree; leave the file. The boot
         * /mnt/files is small but the next test will TRUNC it
         * anyway. */
    }
    g_files_mnt = NULL;
}

/*
 * (Re-)format the SDHCI-backed FAT32 partition 1. Required because
 * the QEMU sd-card image persists across tests in the same run, so
 * each test starts from a clean known state. Also drops any
 * production blkdev that an earlier test's `kernel ...` subcommand
 * pinned in boot_media's keep-alive cache (kernel_cmd.c routes
 * through boot_media_acquire/release, see #371 sub-task 5) so each
 * test gets a fresh acquire path rather than inheriting a cached
 * dev that aliases this fixture's `kc_format` blkdev.
 */
static void format_boot_partition(void)
{
    boot_media_test_clear_production_cache();
    struct blkdev *dev = sdhci_create_qemu_pci("kc_format");
    TEST_ASSERT_NOT_NULL(dev);
    /* Detach any prior FatFs binding before claiming the disk. */
    f_mount(NULL, VOL, 0);
    fatfs_disk_detach();
    fatfs_disk_attach(dev);

    LBA_t plist[] = { 100, 0, 0, 0 };
    FRESULT res = f_fdisk(0, plist, mkfs_work);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    MKFS_PARM opt = { .fmt = FM_FAT32, .n_fat = 1, };
    res = f_mkfs(VOL, &opt, mkfs_work, sizeof(mkfs_work));
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    /* Pre-populate kernel_2712.img so `kernel status` has a non-trivial
     * "active kernel" line and `kernel promote` has a real overwrite
     * target. Content is unrelated to the staged payload. */
    static FATFS fs;
    res = f_mount(&fs, VOL, 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    FIL fp;
    res = f_open(&fp, KERNEL_IMG_PATH, FA_WRITE | FA_CREATE_ALWAYS);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    static const char active_marker[] =
        "ACTIVE-KERNEL-PRE-EXISTING-CONTENT-DO-NOT-MISTAKE-FOR-STAGED";
    UINT n;
    f_write(&fp, active_marker, sizeof(active_marker), &n);
    f_close(&fp);

    f_mount(NULL, VOL, 0);
    fatfs_disk_detach();
    sdhci_destroy(dev);
}

/*
 * Inspect the FAT partition after `kernel ...` ran. Caller passes
 * a function that takes a mounted state. Caller-side fn does the
 * checks; this wrapper handles the mount/unmount boilerplate.
 */
typedef void (*fat_inspect_fn)(FATFS *fs);

static void inspect_boot_partition(fat_inspect_fn fn)
{
    struct blkdev *dev = sdhci_create_qemu_pci("kc_inspect");
    TEST_ASSERT_NOT_NULL(dev);
    f_mount(NULL, VOL, 0);
    fatfs_disk_detach();
    fatfs_disk_attach(dev);
    static FATFS fs;
    FRESULT res = f_mount(&fs, VOL, 1);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);

    fn(&fs);

    f_mount(NULL, VOL, 0);
    fatfs_disk_detach();
    sdhci_destroy(dev);
}

/* ---- Inspectors used by tests ---- */

static void check_staged(FATFS *fs)
{
    (void)fs;
    FILINFO fno;
    /* tryboot.img exists and matches payload size. */
    TEST_ASSERT_EQUAL_INT(FR_OK, f_stat(TRYBOOT_IMG_PATH, &fno));
    TEST_ASSERT_EQUAL_UINT32(SOURCE_PAYLOAD_SIZE, fno.fsize);
    /* tryboot.sha exists and matches SHA-256 of payload. */
    FIL sha_fp;
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&sha_fp, TRYBOOT_SHA_PATH, FA_READ));
    char hex_buf[SHA256_HEX_LEN + 4];
    UINT got = 0;
    f_read(&sha_fp, hex_buf, sizeof(hex_buf) - 1, &got);
    f_close(&sha_fp);
    /* Strip trailing newline for compare. */
    if (got > 0 && hex_buf[got - 1] == '\n') got--;
    hex_buf[got] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected_sha_hex, hex_buf);

    /* Read back tryboot.img content and compare to payload. */
    FIL img_fp;
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&img_fp, TRYBOOT_IMG_PATH, FA_READ));
    static uint8_t verify[SOURCE_PAYLOAD_SIZE];
    UINT igot = 0;
    f_read(&img_fp, verify, SOURCE_PAYLOAD_SIZE, &igot);
    f_close(&img_fp);
    TEST_ASSERT_EQUAL_UINT32(SOURCE_PAYLOAD_SIZE, igot);
    TEST_ASSERT_EQUAL_MEMORY(source_payload, verify, SOURCE_PAYLOAD_SIZE);
}

static void check_empty_after_rollback(FATFS *fs)
{
    (void)fs;
    FILINFO fno;
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, f_stat(TRYBOOT_IMG_PATH, &fno));
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, f_stat(TRYBOOT_SHA_PATH, &fno));
}

static void check_promoted(FATFS *fs)
{
    (void)fs;
    FILINFO fno;
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, f_stat(TRYBOOT_IMG_PATH, &fno));
    TEST_ASSERT_EQUAL_INT(FR_NO_FILE, f_stat(TRYBOOT_SHA_PATH, &fno));
    /* kernel_2712.img is now the staged content. */
    TEST_ASSERT_EQUAL_INT(FR_OK, f_stat(KERNEL_IMG_PATH, &fno));
    TEST_ASSERT_EQUAL_UINT32(SOURCE_PAYLOAD_SIZE, fno.fsize);

    FIL fp;
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&fp, KERNEL_IMG_PATH, FA_READ));
    static uint8_t verify[SOURCE_PAYLOAD_SIZE];
    UINT got = 0;
    f_read(&fp, verify, SOURCE_PAYLOAD_SIZE, &got);
    f_close(&fp);
    TEST_ASSERT_EQUAL_UINT32(SOURCE_PAYLOAD_SIZE, got);
    TEST_ASSERT_EQUAL_MEMORY(source_payload, verify, SOURCE_PAYLOAD_SIZE);
}

/* ---- Tests ---- */

static void test_status_runs_without_card_or_stage(void)
{
    /* `kernel status` must succeed even if no SDHCI controller is
     * available (unrelated platforms shouldn't error out). The
     * QEMU virt build with sdhci-pci has a controller, but it may
     * be unformatted at this point — status reports that gracefully.
     *
     * Returns 0 in BOTH cases: when the boot volume mounts and the
     * staged-image / active-kernel info gets reported, AND when
     * the volume is unavailable (which is the steady state on
     * Jetson and any platform without an SD-backed boot path).
     * The status query's job is to report state — reporting
     * "unavailable" is success, not failure. Pre-fix this returned
     * -1 in the unavailable case, which the shell rendered as
     * "Command returned error: -1" right after a successful
     * informational print, confusing operators. */
    int rc = shell_execute("kernel status");
    TEST_ASSERT_EQUAL_INT(0, rc);
}

static void test_stage_writes_image_and_sha_sidecar(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    setup_source_vfs();
    format_boot_partition();

    int rc = shell_execute("kernel stage " SOURCE_VFS_PATH);
    TEST_ASSERT_EQUAL_INT(0, rc);

    inspect_boot_partition(check_staged);
    teardown_source_vfs();
}

static void test_stage_rejects_missing_source(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    format_boot_partition();
    int rc = shell_execute("kernel stage /mnt/files/does-not-exist.img");
    TEST_ASSERT_TRUE(rc != 0);
}

static void test_promote_renames_tryboot_to_kernel(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    setup_source_vfs();
    format_boot_partition();

    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel stage " SOURCE_VFS_PATH));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel promote"));

    inspect_boot_partition(check_promoted);
    teardown_source_vfs();
}

static void test_promote_without_stage_fails(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    format_boot_partition();
    /* No stage. */
    int rc = shell_execute("kernel promote");
    TEST_ASSERT_TRUE(rc != 0);
}

static void test_rollback_clears_staged_state(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    setup_source_vfs();
    format_boot_partition();

    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel stage " SOURCE_VFS_PATH));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel rollback"));

    inspect_boot_partition(check_empty_after_rollback);
    teardown_source_vfs();
}

static void test_rollback_when_empty_is_idempotent(void)
{
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    format_boot_partition();
    int rc = shell_execute("kernel rollback");
    TEST_ASSERT_EQUAL_INT(0, rc);   /* succeeds even with nothing to clear */

    inspect_boot_partition(check_empty_after_rollback);
}

static void test_full_lifecycle_stage_promote_stage_rollback(void)
{
    /* End-to-end: stage→promote (becomes the new active kernel) →
     * stage again (different file? same payload here, but the
     * pipeline is the same) → rollback (clears the second stage,
     * leaving the promoted kernel in place). */
    if (!sdhci_available()) {
        TEST_IGNORE_MESSAGE("sdhci-pci not present — skipping");
    }
    setup_source_vfs();
    format_boot_partition();

    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel stage " SOURCE_VFS_PATH));
    inspect_boot_partition(check_staged);

    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel promote"));
    inspect_boot_partition(check_promoted);

    /* Re-stage on top of a promoted state. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel stage " SOURCE_VFS_PATH));
    inspect_boot_partition(check_staged);

    /* Rollback the second stage; promoted kernel still wins. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("kernel rollback"));

    inspect_boot_partition(check_promoted);

    teardown_source_vfs();
}

static void test_unknown_subcommand_rejected(void)
{
    int rc = shell_execute("kernel garbage");
    TEST_ASSERT_TRUE(rc != 0);
}

static void test_no_subcommand_rejected(void)
{
    int rc = shell_execute("kernel");
    TEST_ASSERT_TRUE(rc != 0);
}

int test_suite_kernel_cmd(void)
{
    UnityBegin("kernel admin command surface tests");

    /* One-time payload + expected-hash setup. */
    build_source_payload();

    RUN_TEST(test_status_runs_without_card_or_stage);
    RUN_TEST(test_stage_writes_image_and_sha_sidecar);
    RUN_TEST(test_stage_rejects_missing_source);
    RUN_TEST(test_promote_renames_tryboot_to_kernel);
    RUN_TEST(test_promote_without_stage_fails);
    RUN_TEST(test_rollback_clears_staged_state);
    RUN_TEST(test_rollback_when_empty_is_idempotent);
    RUN_TEST(test_full_lifecycle_stage_promote_stage_rollback);
    RUN_TEST(test_unknown_subcommand_rejected);
    RUN_TEST(test_no_subcommand_rejected);

    return UnityEnd();
}
