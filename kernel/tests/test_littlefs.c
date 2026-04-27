/**
 * @file test_littlefs.c
 * @brief LittleFS Integration Tests
 *
 * Tests for the LittleFS filesystem integration, including:
 * - RAM disk creation and operations
 * - LittleFS mount/unmount
 * - File create/read/write
 * - Directory operations
 * - VFS mount point access
 */

#include "unity.h"
#include "test_harness.h"
#include "../include/blkdev.h"
#include "../include/boot_media.h"
#include "../include/fat32.h"
#include "../include/ramdisk.h"
#include "../include/littlefs_slm.h"
#include "../include/littlefs_vfs.h"
#include "../include/persistent_lfs_store.h"
#include "../include/vfs.h"
#include "../include/pmm.h"
#include "../include/uart.h"
#include "../lib/fatfs/ff.h"
#include <stdint.h>
#include <string.h>

/* Forward declarations for string functions */
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, size_t n);
extern size_t strlen(const char *s);
extern char *strcpy(char *dest, const char *src);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

#define TEST_FAT32_BLOCK_SIZE   512u
#define TEST_FAT32_BLOCK_COUNT  (32u * 1024u * 1024u / 512u)
#define TEST_FAT32_VOL          "0:"
#define TEST_PERSISTENT_LFS_BAK "0:/slmstore/files.lfs.bak"
#define TEST_PERSISTENT_LFS_DELTA     "0:/slmstore/files.lfs.delta"
#define TEST_PERSISTENT_LFS_DELTA_BAK "0:/slmstore/files.lfs.delta.bak"

static BYTE g_test_fat_mkfs_work[4096];

static void make_boot_media_fat_volume(struct blkdev **out_dev)
{
    struct blkdev *dev;
    LBA_t plist[] = { 100, 0, 0, 0 };
    MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 0,
    };
    FATFS fs;

    TEST_ASSERT_NOT_NULL(out_dev);
    dev = ramdisk_create("test_persistent_fat",
                         TEST_FAT32_BLOCK_SIZE,
                         TEST_FAT32_BLOCK_COUNT);
    TEST_ASSERT_NOT_NULL(dev);
    fatfs_disk_attach(dev);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_fdisk(0, plist, g_test_fat_mkfs_work));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mkfs(TEST_FAT32_VOL, &opt,
                                        g_test_fat_mkfs_work,
                                        sizeof(g_test_fat_mkfs_work)));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(&fs, TEST_FAT32_VOL, 1));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(NULL, TEST_FAT32_VOL, 0));
    fatfs_disk_detach();
    *out_dev = dev;
}

static void destroy_boot_media_fat_volume(struct blkdev *dev)
{
    boot_media_test_clear_device();
    fatfs_disk_detach();
    if (dev) {
        ramdisk_destroy(dev);
    }
}

/* ============================================================================
 * RAM Disk Tests
 * ============================================================================ */

/**
 * Test RAM disk creation and registration.
 */
static void test_ramdisk_create(void)
{
    struct blkdev *dev = ramdisk_create("test_rd1", 4096, 16);
    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL(4096, dev->block_size);
    TEST_ASSERT_EQUAL(16, dev->block_count);
    TEST_ASSERT_EQUAL_STRING("test_rd1", dev->name);

    /* Clean up */
    ramdisk_destroy(dev);
}

/**
 * Test RAM disk read/write operations.
 */
static void test_ramdisk_read_write(void)
{
    struct blkdev *dev = ramdisk_create("test_rd2", 512, 4);
    TEST_ASSERT_NOT_NULL(dev);

    /* Write pattern to block 0 */
    uint8_t write_buf[512];
    memset(write_buf, 0xAA, sizeof(write_buf));
    int result = dev->ops->erase(dev, 0);
    TEST_ASSERT_EQUAL(0, result);
    result = dev->ops->prog(dev, 0, 0, write_buf, sizeof(write_buf));
    TEST_ASSERT_EQUAL(0, result);

    /* Read back and verify */
    uint8_t read_buf[512];
    memset(read_buf, 0, sizeof(read_buf));
    result = dev->ops->read(dev, 0, 0, read_buf, sizeof(read_buf));
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_MEMORY(write_buf, read_buf, sizeof(read_buf));

    /* Clean up */
    ramdisk_destroy(dev);
}

/**
 * Test RAM disk erase sets blocks to 0xFF.
 */
static void test_ramdisk_erase(void)
{
    struct blkdev *dev = ramdisk_create("test_rd3", 512, 2);
    TEST_ASSERT_NOT_NULL(dev);

    /* Write non-0xFF pattern */
    uint8_t write_buf[512];
    memset(write_buf, 0x55, sizeof(write_buf));
    dev->ops->erase(dev, 0);
    dev->ops->prog(dev, 0, 0, write_buf, sizeof(write_buf));

    /* Erase block */
    int result = dev->ops->erase(dev, 0);
    TEST_ASSERT_EQUAL(0, result);

    /* Read and verify 0xFF (flash erased state) */
    uint8_t read_buf[512];
    dev->ops->read(dev, 0, 0, read_buf, sizeof(read_buf));
    for (size_t i = 0; i < sizeof(read_buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xFF, read_buf[i]);
    }

    ramdisk_destroy(dev);
}

/* ============================================================================
 * LittleFS Mount/Unmount Tests
 * ============================================================================ */

/**
 * Test LittleFS format and mount.
 */
static void test_lfs_format_mount(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs1", 4096, 32);
    TEST_ASSERT_NOT_NULL(dev);

    /* Mount with format=true should format first */
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Unmount */
    int result = littlefs_unmount(mnt);
    TEST_ASSERT_EQUAL(0, result);

    ramdisk_destroy(dev);
}

/**
 * Test LittleFS remount after format.
 */
static void test_lfs_remount(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs2", 4096, 32);
    TEST_ASSERT_NOT_NULL(dev);

    /* Format and mount */
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);
    littlefs_unmount(mnt);

    /* Remount without format */
    mnt = littlefs_mount(dev, false);
    TEST_ASSERT_NOT_NULL(mnt);
    littlefs_unmount(mnt);

    ramdisk_destroy(dev);
}

/**
 * Test LittleFS filesystem statistics.
 */
static void test_lfs_stat(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs3", 4096, 64);
    TEST_ASSERT_NOT_NULL(dev);

    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    uint32_t total, used;
    int result = littlefs_stat(mnt, &total, &used);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(64, total);
    TEST_ASSERT_TRUE(used < total);  /* Should have some metadata blocks used */

    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

static void test_persistent_lfs_store_round_trip(void)
{
    struct blkdev *fat_dev = NULL;
    struct blkdev *store = NULL;
    struct lfs_mount *mnt = NULL;
    bool needs_format = false;
    char buf[64];
    const char *data = "persistent littlefs payload";

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);

    store = persistent_lfs_store_create("persist_lfs0", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    int fd = littlefs_file_open(mnt, "/persist.txt",
                                LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data),
                          littlefs_file_write(mnt, fd, data, strlen(data)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    store = persistent_lfs_store_create("persist_lfs1", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    fd = littlefs_file_open(mnt, "/persist.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(data),
                          littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(data, buf);
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));

    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_persistent_lfs_store_incremental_update_round_trip(void)
{
    struct blkdev *fat_dev = NULL;
    struct blkdev *store = NULL;
    struct lfs_mount *mnt = NULL;
    bool needs_format = false;
    char buf[64];
    const char *data_a = "first persistent payload";
    const char *data_b = "second payload after remount";

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);

    store = persistent_lfs_store_create("persist_lfs2", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    int fd = littlefs_file_open(mnt, "/alpha.txt",
                                LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data_a),
                          littlefs_file_write(mnt, fd, data_a, strlen(data_a)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    store = persistent_lfs_store_create("persist_lfs3", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    fd = littlefs_file_open(mnt, "/beta.txt",
                            LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data_b),
                          littlefs_file_write(mnt, fd, data_b, strlen(data_b)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    store = persistent_lfs_store_create("persist_lfs4", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    fd = littlefs_file_open(mnt, "/alpha.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(data_a),
                          littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(data_a, buf);
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));

    fd = littlefs_file_open(mnt, "/beta.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(data_b),
                          littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(data_b, buf);
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));

    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_persistent_lfs_store_recovers_backup_image(void)
{
    struct blkdev *fat_dev = NULL;
    struct blkdev *store = NULL;
    struct lfs_mount *mnt = NULL;
    FATFS fs;
    FILINFO fno;
    FRESULT res;
    bool needs_format = false;
    char buf[64];
    const char *data = "backup recovery payload";

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);

    store = persistent_lfs_store_create("persist_lfs5", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    int fd = littlefs_file_open(mnt, "/recover.txt",
                                LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data),
                          littlefs_file_write(mnt, fd, data, strlen(data)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    fatfs_disk_attach(fat_dev);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(&fs, TEST_FAT32_VOL, 1));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_stat(PERSISTENT_LFS_STORE_PATH, &fno));
    TEST_ASSERT_EQUAL_INT(FR_OK,
                          f_rename(PERSISTENT_LFS_STORE_PATH,
                                   TEST_PERSISTENT_LFS_BAK));
    {
        FIL fp;
        UINT wrote = 0;
        static const uint8_t corrupt_primary[3] = {0x01, 0x02, 0x03};
        TEST_ASSERT_EQUAL_INT(FR_OK,
                              f_open(&fp, PERSISTENT_LFS_STORE_PATH,
                                     FA_WRITE | FA_CREATE_ALWAYS));
        TEST_ASSERT_EQUAL_INT(FR_OK,
                              f_write(&fp, corrupt_primary, sizeof(corrupt_primary),
                                      &wrote));
        TEST_ASSERT_EQUAL_UINT(sizeof(corrupt_primary), wrote);
        TEST_ASSERT_EQUAL_INT(FR_OK, f_close(&fp));
    }
    res = f_stat(PERSISTENT_LFS_STORE_PATH, &fno);
    TEST_ASSERT_EQUAL_INT(FR_OK, res);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(NULL, TEST_FAT32_VOL, 0));
    fatfs_disk_detach();

    store = persistent_lfs_store_create("persist_lfs6", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    fd = littlefs_file_open(mnt, "/recover.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(data),
                          littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(data, buf);
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));

    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_persistent_lfs_store_recovers_delta_backup(void)
{
    struct blkdev *fat_dev = NULL;
    struct blkdev *store = NULL;
    struct lfs_mount *mnt = NULL;
    FATFS fs;
    FIL fp;
    FILINFO fno;
    UINT wrote = 0;
    bool needs_format = false;
    char buf[64];
    const char *data_a = "base payload";
    const char *data_b = "delta backup payload";
    const char *data_c = "newer payload before corruption";

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);

    store = persistent_lfs_store_create("persist_lfs7", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_TRUE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    int fd = littlefs_file_open(mnt, "/journal.txt",
                                LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data_a),
                          littlefs_file_write(mnt, fd, data_a, strlen(data_a)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    store = persistent_lfs_store_create("persist_lfs8", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);
    fd = littlefs_file_open(mnt, "/journal.txt",
                            LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data_b),
                          littlefs_file_write(mnt, fd, data_b, strlen(data_b)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    store = persistent_lfs_store_create("persist_lfs9", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);
    fd = littlefs_file_open(mnt, "/journal.txt",
                            LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(data_c),
                          littlefs_file_write(mnt, fd, data_c, strlen(data_c)));
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));
    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);

    fatfs_disk_attach(fat_dev);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(&fs, TEST_FAT32_VOL, 1));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_stat(TEST_PERSISTENT_LFS_DELTA_BAK, &fno));
    TEST_ASSERT_EQUAL_INT(FR_OK,
                          f_open(&fp, TEST_PERSISTENT_LFS_DELTA,
                                 FA_WRITE | FA_CREATE_ALWAYS));
    {
        static const uint8_t corrupt_delta[5] = {0xde, 0xad, 0xbe, 0xef, 0x00};
        TEST_ASSERT_EQUAL_INT(FR_OK,
                              f_write(&fp, corrupt_delta, sizeof(corrupt_delta), &wrote));
        TEST_ASSERT_EQUAL_UINT(sizeof(corrupt_delta), wrote);
    }
    TEST_ASSERT_EQUAL_INT(FR_OK, f_close(&fp));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(NULL, TEST_FAT32_VOL, 0));
    fatfs_disk_detach();

    store = persistent_lfs_store_create("persist_lfs10", &needs_format);
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_FALSE(needs_format);
    mnt = littlefs_mount(store, needs_format);
    TEST_ASSERT_NOT_NULL(mnt);

    fd = littlefs_file_open(mnt, "/journal.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)strlen(data_b),
                          littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(data_b, buf);
    TEST_ASSERT_EQUAL_INT(0, littlefs_file_close(mnt, fd));

    TEST_ASSERT_EQUAL_INT(0, littlefs_unmount(mnt));
    persistent_lfs_store_destroy(store);
    destroy_boot_media_fat_volume(fat_dev);
}

/* ============================================================================
 * File Operation Tests
 * ============================================================================ */

/**
 * Test file create and write.
 */
static void test_lfs_file_create_write(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs4", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create and write file */
    int fd = littlefs_file_open(mnt, "/test.txt", LFS_O_CREAT | LFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    const char *data = "Hello, LittleFS!";
    int written = littlefs_file_write(mnt, fd, data, strlen(data));
    TEST_ASSERT_EQUAL((int)strlen(data), written);

    int result = littlefs_file_close(mnt, fd);
    TEST_ASSERT_EQUAL(0, result);

    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test file read.
 */
static void test_lfs_file_read(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs5", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Write file */
    const char *data = "Test data 12345";
    int fd = littlefs_file_open(mnt, "/read_test.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, data, strlen(data));
    littlefs_file_close(mnt, fd);

    /* Read file */
    fd = littlefs_file_open(mnt, "/read_test.txt", LFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int read_bytes = littlefs_file_read(mnt, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_EQUAL((int)strlen(data), read_bytes);
    TEST_ASSERT_EQUAL_STRING(data, buf);

    littlefs_file_close(mnt, fd);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test file seek and size.
 */
static void test_lfs_file_seek_size(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs6", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file with known content */
    int fd = littlefs_file_open(mnt, "/seek_test.txt", LFS_O_CREAT | LFS_O_RDWR);
    const char *data = "ABCDEFGHIJ";  /* 10 bytes */
    littlefs_file_write(mnt, fd, data, strlen(data));

    /* Test size */
    int size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(10, size);

    /* Test seek to beginning and read */
    int pos = littlefs_file_seek(mnt, fd, 0, LFS_SEEK_SET);
    TEST_ASSERT_EQUAL(0, pos);

    char buf[3];
    littlefs_file_read(mnt, fd, buf, 3);
    TEST_ASSERT_EQUAL_MEMORY("ABC", buf, 3);

    /* Test seek from current position */
    pos = littlefs_file_seek(mnt, fd, 2, LFS_SEEK_CUR);
    TEST_ASSERT_EQUAL(5, pos);
    littlefs_file_read(mnt, fd, buf, 3);
    TEST_ASSERT_EQUAL_MEMORY("FGH", buf, 3);

    littlefs_file_close(mnt, fd);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/* ============================================================================
 * Directory Operation Tests
 * ============================================================================ */

/**
 * Test directory creation.
 */
static void test_lfs_mkdir(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs7", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    int result = littlefs_mkdir(mnt, "/testdir");
    TEST_ASSERT_EQUAL(0, result);

    /* Verify directory exists via stat */
    struct lfs_entry_info info;
    result = littlefs_stat_path(mnt, "/testdir", &info);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(LFS_TYPE_DIR, info.type);

    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test directory listing.
 */
static void test_lfs_dir_list(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs8", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create some files and directories */
    littlefs_mkdir(mnt, "/dir1");
    int fd = littlefs_file_open(mnt, "/file1.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, "test", 4);
    littlefs_file_close(mnt, fd);

    /* List root directory */
    int dh = littlefs_dir_open(mnt, "/");
    TEST_ASSERT_TRUE(dh >= 0);

    struct lfs_entry_info info;
    int count = 0;
    int found_dir1 = 0;
    int found_file1 = 0;

    while (littlefs_dir_read(mnt, dh, &info) > 0) {
        count++;
        if (strcmp(info.name, "dir1") == 0 && info.type == LFS_TYPE_DIR) {
            found_dir1 = 1;
        }
        if (strcmp(info.name, "file1.txt") == 0 && info.type == LFS_TYPE_REG) {
            found_file1 = 1;
        }
    }

    TEST_ASSERT_TRUE(count >= 2);  /* At least dir1 and file1.txt */
    TEST_ASSERT_TRUE(found_dir1);
    TEST_ASSERT_TRUE(found_file1);

    littlefs_dir_close(mnt, dh);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test file/directory removal.
 */
static void test_lfs_remove(void)
{
    struct blkdev *dev = ramdisk_create("test_lfs9", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create and remove file */
    int fd = littlefs_file_open(mnt, "/to_remove.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_close(mnt, fd);

    struct lfs_entry_info info;
    int result = littlefs_stat_path(mnt, "/to_remove.txt", &info);
    TEST_ASSERT_EQUAL(0, result);

    result = littlefs_remove(mnt, "/to_remove.txt");
    TEST_ASSERT_EQUAL(0, result);

    result = littlefs_stat_path(mnt, "/to_remove.txt", &info);
    TEST_ASSERT_NOT_EQUAL(0, result);  /* Should not exist */

    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/* ============================================================================
 * File Truncate and Rename Tests
 * ============================================================================ */

/**
 * Test file truncation.
 */
static void test_lfs_file_truncate(void)
{
    struct blkdev *dev = ramdisk_create("test_trunc", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file with content */
    int fd = littlefs_file_open(mnt, "/truncate.txt", LFS_O_CREAT | LFS_O_RDWR);
    const char *data = "1234567890ABCDEFGHIJ";  /* 20 bytes */
    littlefs_file_write(mnt, fd, data, strlen(data));

    /* Verify initial size */
    int size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(20, size);

    /* Truncate to 10 bytes */
    int result = littlefs_file_truncate(mnt, fd, 10);
    TEST_ASSERT_EQUAL(0, result);

    size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(10, size);

    /* Verify content is truncated */
    littlefs_file_seek(mnt, fd, 0, LFS_SEEK_SET);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int read_bytes = littlefs_file_read(mnt, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(10, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("1234567890", buf, 10);

    /* Truncate to 0 (clear file) */
    result = littlefs_file_truncate(mnt, fd, 0);
    TEST_ASSERT_EQUAL(0, result);
    size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(0, size);

    littlefs_file_close(mnt, fd);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test file rename/move.
 */
static void test_lfs_rename(void)
{
    struct blkdev *dev = ramdisk_create("test_rename", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file */
    int fd = littlefs_file_open(mnt, "/original.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, "content", 7);
    littlefs_file_close(mnt, fd);

    /* Rename file */
    int result = littlefs_rename(mnt, "/original.txt", "/renamed.txt");
    TEST_ASSERT_EQUAL(0, result);

    /* Original should not exist */
    struct lfs_entry_info info;
    result = littlefs_stat_path(mnt, "/original.txt", &info);
    TEST_ASSERT_NOT_EQUAL(0, result);

    /* Renamed should exist with same content */
    result = littlefs_stat_path(mnt, "/renamed.txt", &info);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(LFS_TYPE_REG, info.type);
    TEST_ASSERT_EQUAL(7, info.size);

    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test file append mode.
 */
static void test_lfs_append_mode(void)
{
    struct blkdev *dev = ramdisk_create("test_append", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file with initial content */
    int fd = littlefs_file_open(mnt, "/log.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, "Line1\n", 6);
    littlefs_file_close(mnt, fd);

    /* Reopen in append mode and add more */
    fd = littlefs_file_open(mnt, "/log.txt", LFS_O_WRONLY | LFS_O_APPEND);
    TEST_ASSERT_TRUE(fd >= 0);
    littlefs_file_write(mnt, fd, "Line2\n", 6);
    littlefs_file_close(mnt, fd);

    /* Append again */
    fd = littlefs_file_open(mnt, "/log.txt", LFS_O_WRONLY | LFS_O_APPEND);
    littlefs_file_write(mnt, fd, "Line3\n", 6);
    littlefs_file_close(mnt, fd);

    /* Read and verify all content */
    fd = littlefs_file_open(mnt, "/log.txt", LFS_O_RDONLY);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int read_bytes = littlefs_file_read(mnt, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(18, read_bytes);
    TEST_ASSERT_EQUAL_STRING("Line1\nLine2\nLine3\n", buf);

    littlefs_file_close(mnt, fd);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/**
 * Test reading file with offset (streaming reads).
 */
static void test_lfs_offset_read(void)
{
    struct blkdev *dev = ramdisk_create("test_offset", 4096, 32);
    struct lfs_mount *mnt = littlefs_mount(dev, true);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file with known content */
    int fd = littlefs_file_open(mnt, "/stream.bin", LFS_O_CREAT | LFS_O_WRONLY);
    const char *data = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghij";  /* 46 bytes */
    littlefs_file_write(mnt, fd, data, strlen(data));
    littlefs_file_close(mnt, fd);

    /* Read first 10 bytes at offset 0 */
    fd = littlefs_file_open(mnt, "/stream.bin", LFS_O_RDONLY);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    int read_bytes = littlefs_file_read(mnt, fd, buf, 10);
    TEST_ASSERT_EQUAL(10, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("0123456789", buf, 10);

    /* Seek to offset 10 and read next 10 bytes */
    littlefs_file_seek(mnt, fd, 10, LFS_SEEK_SET);
    memset(buf, 0, sizeof(buf));
    read_bytes = littlefs_file_read(mnt, fd, buf, 10);
    TEST_ASSERT_EQUAL(10, read_bytes);
    TEST_ASSERT_EQUAL_MEMORY("ABCDEFGHIJ", buf, 10);

    /* Read from offset 36 to end */
    littlefs_file_seek(mnt, fd, 36, LFS_SEEK_SET);
    memset(buf, 0, sizeof(buf));
    read_bytes = littlefs_file_read(mnt, fd, buf, 20);  /* Request more than available */
    TEST_ASSERT_EQUAL(10, read_bytes);  /* Only 10 bytes left */
    TEST_ASSERT_EQUAL_MEMORY("abcdefghij", buf, 10);

    littlefs_file_close(mnt, fd);
    littlefs_unmount(mnt);
    ramdisk_destroy(dev);
}

/* ============================================================================
 * VFS Mount Context Tests
 * ============================================================================ */

/**
 * Test vfs_get_mount_ctx retrieves correct context.
 */
static void test_vfs_get_mount_ctx(void)
{
    /* Test with known mount point from main.c */
    const char *subpath = NULL;
    void *ctx = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(subpath);

    /* Test with path inside mount point */
    ctx = vfs_get_mount_ctx("/mnt/files/hello.txt", &subpath);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(subpath);
    TEST_ASSERT_TRUE(strcmp(subpath, "/hello.txt") == 0 ||
                     strcmp(subpath, "hello.txt") == 0);

    /* Test with non-mount path (should return NULL) */
    ctx = vfs_get_mount_ctx("/sys/memory", &subpath);
    TEST_ASSERT_NULL(ctx);
}

/**
 * Test VFS read with offset (streaming).
 */
static void test_vfs_read_with_offset(void)
{
    /* Read hello.txt at different offsets */
    /* File content: "Hello from SLM-OS LittleFS!\n" (28 bytes) */
    char buf[32];

    /* Read first 5 bytes */
    memset(buf, 0, sizeof(buf));
    int result = vfs_read_path("/mnt/files/hello.txt", buf, 5, 0);
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_MEMORY("Hello", buf, 5);

    /* Read starting at offset 6 */
    memset(buf, 0, sizeof(buf));
    result = vfs_read_path("/mnt/files/hello.txt", buf, 10, 6);
    TEST_ASSERT_TRUE(result > 0);
    /* "Hello from SLM-OS LittleFS!\n" - offset 6 is "from SLM-O" */
    TEST_ASSERT_EQUAL_MEMORY("from SLM-O", buf, 10);
}

/* CORE-M2: littlefs_file_seek takes int32_t; offsets beyond INT32_MAX must
 * be rejected rather than silently truncated by the cast. */
static void test_vfs_read_offset_overflow(void)
{
    char buf[16];
    /* Cast constant to size_t so the comparison inside the VFS read matches
     * the guard we added. (size_t)INT32_MAX + 1 triggers the guard. */
    size_t huge_offset = (size_t)INT32_MAX + 1;
    int result = vfs_read_path("/mnt/files/hello.txt", buf, sizeof(buf),
                               huge_offset);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/* ============================================================================
 * Write Through Mount Point Tests
 * ============================================================================ */

/**
 * Test writing new file through mount context.
 */
static void test_write_through_mount(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create new file */
    int fd = littlefs_file_open(mnt, "/test_write.txt", LFS_O_CREAT | LFS_O_WRONLY | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(fd >= 0);

    const char *content = "Written via mount context";
    int written = littlefs_file_write(mnt, fd, content, strlen(content));
    TEST_ASSERT_EQUAL((int)strlen(content), written);
    littlefs_file_close(mnt, fd);

    /* Read back through VFS */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int read_bytes = vfs_read_path("/mnt/files/test_write.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL((int)strlen(content), read_bytes);
    TEST_ASSERT_EQUAL_STRING(content, buf);

    /* Clean up */
    littlefs_remove(mnt, "/test_write.txt");
}

/**
 * Test mkdir through mount context.
 */
static void test_mkdir_through_mount(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create directory */
    int result = littlefs_mkdir(mnt, "/test_dir");
    TEST_ASSERT_EQUAL(0, result);

    /* Verify through stat */
    struct lfs_entry_info info;
    result = littlefs_stat_path(mnt, "/test_dir", &info);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(LFS_TYPE_DIR, info.type);

    /* Clean up */
    littlefs_remove(mnt, "/test_dir");
}

/**
 * Test rename through mount context.
 */
static void test_rename_through_mount(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file */
    int fd = littlefs_file_open(mnt, "/rename_src.txt", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, "data", 4);
    littlefs_file_close(mnt, fd);

    /* Rename */
    int result = littlefs_rename(mnt, "/rename_src.txt", "/rename_dst.txt");
    TEST_ASSERT_EQUAL(0, result);

    /* Verify old gone, new exists */
    struct lfs_entry_info info;
    result = littlefs_stat_path(mnt, "/rename_src.txt", &info);
    TEST_ASSERT_NOT_EQUAL(0, result);  /* Should not exist */

    result = littlefs_stat_path(mnt, "/rename_dst.txt", &info);
    TEST_ASSERT_EQUAL(0, result);

    /* Clean up */
    littlefs_remove(mnt, "/rename_dst.txt");
}

/**
 * Test truncate through mount context.
 */
static void test_truncate_through_mount(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file with content */
    int fd = littlefs_file_open(mnt, "/trunc_test.txt", LFS_O_CREAT | LFS_O_RDWR);
    littlefs_file_write(mnt, fd, "123456789012345", 15);

    int size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(15, size);

    /* Truncate to 5 */
    int result = littlefs_file_truncate(mnt, fd, 5);
    TEST_ASSERT_EQUAL(0, result);

    size = littlefs_file_size(mnt, fd);
    TEST_ASSERT_EQUAL(5, size);

    littlefs_file_close(mnt, fd);

    /* Clean up */
    littlefs_remove(mnt, "/trunc_test.txt");
}

/**
 * Test append mode through mount context.
 */
static void test_append_through_mount(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    /* Create file */
    int fd = littlefs_file_open(mnt, "/append_test.log", LFS_O_CREAT | LFS_O_WRONLY);
    littlefs_file_write(mnt, fd, "Entry1\n", 7);
    littlefs_file_close(mnt, fd);

    /* Append to it */
    fd = littlefs_file_open(mnt, "/append_test.log", LFS_O_WRONLY | LFS_O_APPEND);
    littlefs_file_write(mnt, fd, "Entry2\n", 7);
    littlefs_file_close(mnt, fd);

    /* Read and verify */
    char buf[32];
    memset(buf, 0, sizeof(buf));
    int read_bytes = vfs_read_path("/mnt/files/append_test.log", buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL(14, read_bytes);
    TEST_ASSERT_EQUAL_STRING("Entry1\nEntry2\n", buf);

    /* Clean up */
    littlefs_remove(mnt, "/append_test.log");
}

/*
 * Regression for #79: a stale file handle from a closed file must not
 * silently alias to a new file that reuses the same pool slot. The
 * generation counter in the encoded handle detects the mismatch.
 */
static void test_stale_handle_rejected(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);

    int fd1 = littlefs_file_open(mnt, "/stale_a.txt",
                                 LFS_O_CREAT | LFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd1 >= 0);
    littlefs_file_write(mnt, fd1, "AAAA", 4);
    littlefs_file_close(mnt, fd1);

    int fd2 = littlefs_file_open(mnt, "/stale_b.txt",
                                 LFS_O_CREAT | LFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd2 >= 0);
    littlefs_file_write(mnt, fd2, "BBBB", 4);

    char buf[8] = {0};
    int read_result = littlefs_file_read(mnt, fd1, buf, 4);
    TEST_ASSERT_EQUAL_INT(-9 /* LFS_ERR_BADF */, read_result);

    littlefs_file_close(mnt, fd2);
    littlefs_remove(mnt, "/stale_a.txt");
    littlefs_remove(mnt, "/stale_b.txt");
}

/* ============================================================================
 * VFS Mount Point Tests
 * ============================================================================ */

/**
 * Test VFS mount point lookup.
 */
static void test_vfs_mount_lookup(void)
{
    /* The /mnt/files mount is created in main.c during init */
    struct vfs_node *mnt_node = vfs_lookup("/mnt");
    TEST_ASSERT_NOT_NULL(mnt_node);
    TEST_ASSERT_EQUAL(VFS_NODE_DIR, mnt_node->type);

    struct vfs_node *files_node = vfs_lookup("/mnt/files");
    TEST_ASSERT_NOT_NULL(files_node);
    TEST_ASSERT_EQUAL(VFS_NODE_MOUNT, files_node->type);
}

/**
 * Test reading file through VFS mount point.
 */
static void test_vfs_mount_read_file(void)
{
    /* main.c creates /mnt/files with hello.txt containing "Hello from LittleFS!" */
    char buf[64];
    memset(buf, 0, sizeof(buf));

    int result = vfs_read_path("/mnt/files/hello.txt", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_TRUE(strncmp(buf, "Hello", 5) == 0);
}

/**
 * Test listing directory through VFS mount point.
 */
static int list_count = 0;

static void count_entries(const struct vfs_entry_info *info, void *ctx)
{
    (void)info;
    (void)ctx;
    list_count++;
}

static void test_vfs_mount_list_dir(void)
{
    list_count = 0;

    int result = vfs_list_path("/mnt/files", count_entries, NULL);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_TRUE(list_count >= 2);  /* hello.txt and readme.txt from main.c */
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

int test_suite_littlefs(void)
{
    uart_puts("\n[TEST] LittleFS Integration Tests\n");
    UNITY_BEGIN();

    /* RAM disk tests */
    RUN_TEST(test_ramdisk_create);
    RUN_TEST(test_ramdisk_read_write);
    RUN_TEST(test_ramdisk_erase);

    /* LittleFS mount tests */
    RUN_TEST(test_lfs_format_mount);
    RUN_TEST(test_lfs_remount);
    RUN_TEST(test_lfs_stat);
    RUN_TEST(test_persistent_lfs_store_round_trip);
    RUN_TEST(test_persistent_lfs_store_incremental_update_round_trip);
    RUN_TEST(test_persistent_lfs_store_recovers_backup_image);
    RUN_TEST(test_persistent_lfs_store_recovers_delta_backup);

    /* File operation tests */
    RUN_TEST(test_lfs_file_create_write);
    RUN_TEST(test_lfs_file_read);
    RUN_TEST(test_lfs_file_seek_size);
    RUN_TEST(test_lfs_file_truncate);
    RUN_TEST(test_lfs_rename);
    RUN_TEST(test_lfs_append_mode);
    RUN_TEST(test_lfs_offset_read);

    /* Directory tests */
    RUN_TEST(test_lfs_mkdir);
    RUN_TEST(test_lfs_dir_list);
    RUN_TEST(test_lfs_remove);

    /* VFS mount point tests */
    RUN_TEST(test_vfs_mount_lookup);
    RUN_TEST(test_vfs_mount_read_file);
    RUN_TEST(test_vfs_mount_list_dir);

    /* VFS mount context tests */
    RUN_TEST(test_vfs_get_mount_ctx);
    RUN_TEST(test_vfs_read_with_offset);
    RUN_TEST(test_vfs_read_offset_overflow);

    /* Write through mount point tests */
    RUN_TEST(test_write_through_mount);
    RUN_TEST(test_mkdir_through_mount);
    RUN_TEST(test_rename_through_mount);
    RUN_TEST(test_truncate_through_mount);
    RUN_TEST(test_append_through_mount);
    RUN_TEST(test_stale_handle_rejected);

    return UNITY_END();
}
