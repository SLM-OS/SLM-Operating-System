#include "unity.h"
#include "../include/blob_autoload.h"
#include "../include/blkdev.h"
#include "../include/boot_media.h"
#include "../include/fat32.h"
#include "../include/ramdisk.h"
#include "runtime_model.h"
#include "../include/shell.h"
#include "../include/slm_ffi.h"
#include "../include/uart.h"
#include "../include/vfs.h"
#include "../include/littlefs_slm.h"
#include "../include/string.h"
#include "test_blob_helpers.h"
#include "../lib/fatfs/ff.h"

#include <stddef.h>
#include <stdint.h>

extern int32_t rust_eviction_blob_clear(uint16_t kind_id);
extern int32_t rust_eviction_blob_status(uint16_t kind_id, RustEvictionBlobStatus *out);

#define TEST_FAT32_BLOCK_SIZE   512u
#define TEST_FAT32_BLOCK_COUNT  (48u * 1024u * 1024u / 512u)
#define TEST_FAT32_VOL          "0:"

static BYTE g_test_fat_mkfs_work[4096];

static const char *find_substr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);

    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return NULL;

    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static size_t build_eviction_xgb_payload(uint8_t *out, size_t out_cap)
{
    uint8_t tmp[66];
    size_t cursor = 0;
    if (out_cap < sizeof(tmp)) return 0;

    tmp[cursor++] = 'X'; tmp[cursor++] = 'G'; tmp[cursor++] = 'B'; tmp[cursor++] = '1';
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 3; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0; tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;

    /* root node */
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 2; tmp[cursor++] = 0;
    { uint32_t bits = 0x3f000000u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    /* left leaf */
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0x3dcccccd; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    /* right leaf */
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 1; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    tmp[cursor++] = 0; tmp[cursor++] = 0;
    { uint32_t bits = 0u; memcpy(tmp + cursor, &bits, 4); cursor += 4; }
    { uint32_t bits = 0x3f4ccccd; memcpy(tmp + cursor, &bits, 4); cursor += 4; }

    memcpy(out, tmp, cursor);
    return cursor;
}

static size_t build_eviction_cacheus_config_payload(uint8_t *out, size_t out_cap)
{
    size_t cursor = 0;
    if (out_cap < 24u) return 0;
    memset(out, 0, 24u);
    out[0] = 'C'; out[1] = 'C'; out[2] = 'F'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 0; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_CACHEUS_U32_LE(v) do {                  \
    uint32_t value_ = (v);                            \
    out[cursor + 0] = (uint8_t)(value_ & 0xFF);       \
    out[cursor + 1] = (uint8_t)((value_ >> 8) & 0xFF);\
    out[cursor + 2] = (uint8_t)((value_ >> 16) & 0xFF);\
    out[cursor + 3] = (uint8_t)((value_ >> 24) & 0xFF);\
    cursor += 4;                                      \
} while (0)
    WRITE_CACHEUS_U32_LE(0x3e4ccccdu);
    WRITE_CACHEUS_U32_LE(200u);
    WRITE_CACHEUS_U32_LE(0x3dcccccdu);
#undef WRITE_CACHEUS_U32_LE
    return cursor;
}

#ifdef CONFIG_AI_SCHEDULER
static size_t build_sched_mlp_payload(uint32_t out_weight_bits,
                                      uint8_t *out,
                                      size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 12,
        W0 = AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN,
        B0 = AI_MLP_LAYER0_OUT,
        W1 = AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN,
        B1 = AI_MLP_LAYER1_OUT,
        W2 = AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN,
        B2 = AI_MLP_LAYER2_OUT,
        W3 = AI_SCHED_N_ACTIONS * AI_MLP_LAYER3_IN,
        B3 = AI_SCHED_N_ACTIONS,
        FLOATS = W0 + B0 + W1 + B1 + W2 + B2 + W3 + B3,
        TOTAL = PAYLOAD_HEADER_LEN + FLOATS * 4
    };
    size_t cursor = 0;
    size_t idx = 0;

    if (out_cap < TOTAL) return 0;
    memset(out, 0, TOTAL);
    out[0] = 'S'; out[1] = 'M'; out[2] = 'L'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = (uint8_t)(AI_SCHED_N_ACTIONS & 0xFF);
    out[11] = (uint8_t)(AI_SCHED_N_ACTIONS >> 8);

    cursor = PAYLOAD_HEADER_LEN;
#define WRITE_U32_LE(bits)                                                   \
    do {                                                                     \
        uint32_t bits_ = (bits);                                             \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                           \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                    \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                   \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                   \
        cursor += 4;                                                         \
    } while (0)

    for (idx = 0; idx < FLOATS; idx++) {
        uint32_t bits = 0u;
        if (idx == 0) bits = 0x3F800000u;
        if (idx == W0 + B0) bits = 0x3F800000u;
        if (idx == W0 + B0 + W1 + B1) bits = 0x3F800000u;
        if (idx == W0 + B0 + W1 + B1 + W2 + B2) bits = out_weight_bits;
        WRITE_U32_LE(bits);
    }
#undef WRITE_U32_LE

    return cursor;
}

static size_t build_sched_config_payload(uint8_t *out, size_t out_cap)
{
    size_t cursor = 0;
    if (out_cap < 32u) return 0;
    memset(out, 0, 32u);
    out[0] = 'S'; out[1] = 'C'; out[2] = 'F'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_U32_LE(v) do {                         \
    uint32_t bits_ = (v);                            \
    out[cursor + 0] = (uint8_t)(bits_ & 0xFF);       \
    out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);\
    out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);\
    out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);\
    cursor += 4;                                     \
} while (0)
    WRITE_U32_LE(0u);
    WRITE_U32_LE(1u);
    WRITE_U32_LE(1u);
    WRITE_U32_LE(1u);
    WRITE_U32_LE(4u);
#undef WRITE_U32_LE
    return cursor;
}
static size_t build_sched_thresholds_payload(uint64_t critical_ns,
                                             uint64_t high_ns,
                                             uint64_t boost_ns,
                                             uint8_t *out,
                                             size_t out_cap)
{
    size_t cursor = 0;
    if (out_cap < 36u) return 0;
    memset(out, 0, 36u);
    out[0] = 'S'; out[1] = 'T'; out[2] = 'H'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = (uint8_t)(AI_SCHED_N_ACTIONS & 0xFF);
    out[11] = (uint8_t)(AI_SCHED_N_ACTIONS >> 8);
    cursor = 12;
#define WRITE_U64_LE(v) do {                               \
    uint64_t value_ = (v);                                 \
    out[cursor + 0] = (uint8_t)(value_ & 0xFF);            \
    out[cursor + 1] = (uint8_t)((value_ >> 8) & 0xFF);     \
    out[cursor + 2] = (uint8_t)((value_ >> 16) & 0xFF);    \
    out[cursor + 3] = (uint8_t)((value_ >> 24) & 0xFF);    \
    out[cursor + 4] = (uint8_t)((value_ >> 32) & 0xFF);    \
    out[cursor + 5] = (uint8_t)((value_ >> 40) & 0xFF);    \
    out[cursor + 6] = (uint8_t)((value_ >> 48) & 0xFF);    \
    out[cursor + 7] = (uint8_t)((value_ >> 56) & 0xFF);    \
    cursor += 8;                                           \
} while (0)
    WRITE_U64_LE(critical_ns);
    WRITE_U64_LE(high_ns);
    WRITE_U64_LE(boost_ns);
#undef WRITE_U64_LE
    return cursor;
}
static size_t build_sched_rebalance_payload(uint32_t enabled,
                                            uint32_t interval_ticks,
                                            uint32_t imbalance_min,
                                            uint8_t *out,
                                            size_t out_cap)
{
    size_t cursor = 0;
    if (out_cap < 24u) return 0;
    memset(out, 0, 24u);
    out[0] = 'S'; out[1] = 'R'; out[2] = 'B'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_REBAL_U32_LE(v) do {                      \
    uint32_t value_ = (v);                              \
    out[cursor + 0] = (uint8_t)(value_ & 0xFF);         \
    out[cursor + 1] = (uint8_t)((value_ >> 8) & 0xFF);  \
    out[cursor + 2] = (uint8_t)((value_ >> 16) & 0xFF); \
    out[cursor + 3] = (uint8_t)((value_ >> 24) & 0xFF); \
    cursor += 4;                                        \
} while (0)
    WRITE_REBAL_U32_LE(enabled);
    WRITE_REBAL_U32_LE(interval_ticks);
    WRITE_REBAL_U32_LE(imbalance_min);
#undef WRITE_REBAL_U32_LE
    return cursor;
}
#endif /* CONFIG_AI_SCHEDULER for sched helpers */

static int write_binary_file(const char *path, const uint8_t *data, size_t len)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(path, &subpath);
    int fd;
    if (!mnt || !subpath) return -1;
    fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;
    if (littlefs_file_write(mnt, fd, data, len) != (int)len) {
        littlefs_file_close(mnt, fd);
        return -1;
    }
    littlefs_file_close(mnt, fd);
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t cap)
{
    int n = vfs_read_path(path, buf, cap - 1, 0);
    if (n < 0) return n;
    buf[n] = '\0';
    return n;
}

static void build_managed_path(char *out, size_t cap,
                               const char *domain, const char *kind)
{
    size_t pos = 0;
    size_t dir_len = strlen(BLOB_AUTOLOAD_STORE_DIR);
    size_t domain_len = strlen(domain);
    size_t kind_len = strlen(kind);

    TEST_ASSERT_TRUE(dir_len + 1 + domain_len + 1 + kind_len + 5 < cap);
    memcpy(out + pos, BLOB_AUTOLOAD_STORE_DIR, dir_len);
    pos += dir_len;
    out[pos++] = '/';
    memcpy(out + pos, domain, domain_len);
    pos += domain_len;
    out[pos++] = '-';
    memcpy(out + pos, kind, kind_len);
    pos += kind_len;
    memcpy(out + pos, ".blob", 5);
    pos += 5;
    out[pos] = '\0';
}

static int remove_file(const char *path)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(path, &subpath);
    if (!mnt || !subpath) return -1;
    return littlefs_remove(mnt, subpath);
}

static void make_boot_media_fat_volume(struct blkdev **out_dev)
{
    struct blkdev *dev;
    LBA_t plist[] = { 100, 0, 0, 0 };
    MKFS_PARM opt = {
        .fmt     = FM_FAT32,
        .n_fat   = 1,
        .align   = 0,
        .n_root  = 0,
        .au_size = 0,
    };
    FATFS fs;

    TEST_ASSERT_NOT_NULL(out_dev);
    dev = ramdisk_create("autoload_fat",
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

static void write_boot_media_fat_file(const char *path, const void *data, UINT len)
{
    struct blkdev *dev = boot_media_acquire();
    FATFS fs;
    FIL fp;
    FRESULT mkdir_res;
    UINT written = 0;

    TEST_ASSERT_NOT_NULL(dev);
    fatfs_disk_attach(dev);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(&fs, TEST_FAT32_VOL, 1));
    /* Ensure the slmstore/autoload tree exists before f_open with
     * FA_CREATE_ALWAYS — f_open does not create missing parent
     * directories and would otherwise return FR_NO_PATH (#486). */
    mkdir_res = f_mkdir("0:/slmstore");
    TEST_ASSERT_TRUE(mkdir_res == FR_OK || mkdir_res == FR_EXIST);
    mkdir_res = f_mkdir("0:/slmstore/autoload");
    TEST_ASSERT_TRUE(mkdir_res == FR_OK || mkdir_res == FR_EXIST);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_write(&fp, data, len, &written));
    TEST_ASSERT_EQUAL_UINT(len, written);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_close(&fp));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(NULL, TEST_FAT32_VOL, 0));
    fatfs_disk_detach();
    boot_media_release(dev);
}

static void write_boot_media_fat_autoload_conf(const char *text)
{
    struct blkdev *dev = boot_media_acquire();
    FATFS fs;
    FIL fp;
    UINT written = 0;
    UINT len = (UINT)strlen(text);

    TEST_ASSERT_NOT_NULL(dev);
    fatfs_disk_attach(dev);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(&fs, TEST_FAT32_VOL, 1));
    TEST_ASSERT_TRUE(f_mkdir("0:/slmstore") == FR_OK || f_mkdir("0:/slmstore") == FR_EXIST);
    TEST_ASSERT_TRUE(f_mkdir("0:/slmstore/autoload") == FR_OK || f_mkdir("0:/slmstore/autoload") == FR_EXIST);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&fp, "0:/slmstore/blob_autoload.conf", FA_WRITE | FA_CREATE_ALWAYS));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_write(&fp, text, len, &written));
    TEST_ASSERT_EQUAL_UINT(len, written);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_close(&fp));
    TEST_ASSERT_EQUAL_INT(FR_OK, f_mount(NULL, TEST_FAT32_VOL, 0));
    fatfs_disk_detach();
    boot_media_release(dev);
}

/* Only used by the CONFIG_AI_SCHEDULER tests below; gate to silence
 * `-Werror=unused-function` on default builds (issue #399). */
#ifdef CONFIG_AI_SCHEDULER
static void build_long_path(char *out, size_t cap,
                            const char *stem, char fill,
                            const char *suffix)
{
    static const char prefix[] = "/mnt/files/";
    size_t prefix_len = sizeof(prefix) - 1;
    size_t stem_len = strlen(stem);
    size_t suffix_len = strlen(suffix);
    size_t target_len = VFS_MAX_PATH - 1;
    size_t fill_len;

    TEST_ASSERT_TRUE(cap >= VFS_MAX_PATH);
    TEST_ASSERT_TRUE(target_len > prefix_len + stem_len + suffix_len);
    fill_len = target_len - prefix_len - stem_len - suffix_len;

    memcpy(out, prefix, prefix_len);
    memcpy(out + prefix_len, stem, stem_len);
    memset(out + prefix_len + stem_len, fill, fill_len);
    memcpy(out + prefix_len + stem_len + fill_len, suffix, suffix_len);
    out[target_len] = '\0';
}
#endif /* CONFIG_AI_SCHEDULER for build_long_path */
static void test_blob_autoload_init_creates_conf(void)
{
    char buf[256];
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_TRUE(read_text_file(BLOB_AUTOLOAD_CONF_PATH, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(find_substr(buf, "Runtime blob autoload config"));
}

static void test_blob_autoload_init_marks_existing_fat_config_authoritative(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    char fat_conf[256];
    struct blob_autoload_info info;
    uint8_t ev_payload[80];
    uint8_t ev_payload_fat[80];
    uint8_t ev_blob[128];
    uint8_t ev_blob_fat[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_payload_fat_len = build_eviction_xgb_payload(ev_payload_fat, sizeof(ev_payload_fat));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    size_t ev_blob_fat_len;
    uint32_t fat_checksum;

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_payload_fat_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    ev_payload_fat[ev_payload_fat_len - 1] ^= 0x01u;
    ev_blob_fat_len = build_outer_blob(1, ev_payload_fat, ev_payload_fat_len,
                                       ev_blob_fat, sizeof(ev_blob_fat));
    TEST_ASSERT_TRUE(ev_blob_fat_len > 0);
    fat_checksum = fnv1a32(ev_blob_fat, ev_blob_fat_len);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/init-fat-old.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/init-fat-old.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-xgboost.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    write_boot_media_fat_file("0:/slmstore/autoload/eviction-xgboost.blob",
                              ev_blob_fat, (UINT)ev_blob_fat_len);
    uart_snprintf(fat_conf, sizeof(fat_conf),
                  "# Runtime blob autoload config\n"
                  "eviction xgboost 0:/slmstore/autoload/eviction-xgboost.blob %u %08x\n",
                  (unsigned)ev_blob_fat_len, (unsigned)fat_checksum);
    write_boot_media_fat_autoload_conf(fat_conf);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info));
    TEST_ASSERT_EQUAL_UINT32(ev_blob_fat_len, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(fat_checksum, info.checksum);

    blob_autoload_test_fail_next_fat_mount();
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(0, remove_file(BLOB_AUTOLOAD_STORE_DIR "/.fat-authoritative"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_init_recovers_missing_fat_blob_from_lfs(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    struct blob_autoload_info info;
    char fat_conf[256];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    uint32_t checksum = fnv1a32(ev_blob, ev_blob_len);

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/init-fat-recover.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/init-fat-recover.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-xgboost.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    uart_snprintf(fat_conf, sizeof(fat_conf),
                  "# Runtime blob autoload config\n"
                  "eviction xgboost 0:/slmstore/autoload/eviction-xgboost.blob %u %08x\n",
                  (unsigned)ev_blob_len, (unsigned)checksum);
    write_boot_media_fat_autoload_conf(fat_conf);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info));
    TEST_ASSERT_EQUAL_UINT32(ev_blob_len, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(checksum, info.checksum);

    TEST_ASSERT_EQUAL_INT(0, remove_file(BLOB_AUTOLOAD_STORE_DIR "/.fat-authoritative"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_init_uses_fat_backup_when_primary_empty(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    struct blob_autoload_info info;
    char fat_conf[256];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    uint32_t checksum = fnv1a32(ev_blob, ev_blob_len);

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    write_boot_media_fat_file("0:/slmstore/autoload/eviction-xgboost.blob",
                              ev_blob, (UINT)ev_blob_len);
    uart_snprintf(fat_conf, sizeof(fat_conf),
                  "# Runtime blob autoload config\n"
                  "eviction xgboost 0:/slmstore/autoload/eviction-xgboost.blob %u %08x\n",
                  (unsigned)ev_blob_len, (unsigned)checksum);
    write_boot_media_fat_file("0:/slmstore/blob_autoload.conf", "", 0);
    write_boot_media_fat_file("0:/slmstore/blob_autoload.conf.bak",
                              fat_conf, (UINT)strlen(fat_conf));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info));
    TEST_ASSERT_EQUAL_UINT32(ev_blob_len, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(checksum, info.checksum);

    TEST_ASSERT_EQUAL_INT(0, remove_file(BLOB_AUTOLOAD_STORE_DIR "/.fat-authoritative"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_init_drops_unusable_fat_entry_without_lfs_fallback(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    char fat_conf[256];

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    uart_snprintf(fat_conf, sizeof(fat_conf),
                  "# Runtime blob autoload config\n"
                  "eviction xgboost 0:/slmstore/autoload/eviction-xgboost.blob 123 89abcdef\n");
    write_boot_media_fat_autoload_conf(fat_conf);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(0, remove_file(BLOB_AUTOLOAD_STORE_DIR "/.fat-authoritative"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_set_get_clear_round_trip(void)
{
    char path[VFS_MAX_PATH];
    char managed_path[VFS_MAX_PATH];
    struct blob_autoload_info info;
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info));
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
    TEST_ASSERT_EQUAL_STRING(managed_path, info.path);
    TEST_ASSERT_EQUAL_UINT32(ev_blob_len, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(ev_blob, ev_blob_len), info.checksum);

#ifdef CONFIG_AI_SCHEDULER
    {
        uint8_t sched_payload[40];
        uint8_t sched_blob[128];
        size_t sched_payload_len = build_sched_config_payload(sched_payload, sizeof(sched_payload));
        size_t sched_blob_len = build_outer_blob(SCHED_MODEL_KIND_CONFIG, sched_payload, sched_payload_len,
                                                 sched_blob, sizeof(sched_blob));

        TEST_ASSERT_TRUE(sched_payload_len > 0);
        TEST_ASSERT_TRUE(sched_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/sched.cfg", sched_blob, sched_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", "/mnt/files/sched.cfg"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "config", path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("sched", "config", &info));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "config");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
    TEST_ASSERT_EQUAL_STRING(managed_path, info.path);
    TEST_ASSERT_TRUE(info.size_bytes > 0);
    TEST_ASSERT_TRUE(info.checksum != 0);

    {
        uint8_t thresholds_payload[48];
        uint8_t thresholds_blob[128];
        size_t thresholds_payload_len =
            build_sched_thresholds_payload(10u * 1000000u,
                                          50u * 1000000u,
                                          100u * 1000000u,
                                          thresholds_payload,
                                          sizeof(thresholds_payload));
        size_t thresholds_blob_len =
            build_outer_blob(SCHED_MODEL_KIND_THRESHOLDS,
                             thresholds_payload,
                             thresholds_payload_len,
                             thresholds_blob,
                             sizeof(thresholds_blob));

        TEST_ASSERT_TRUE(thresholds_payload_len > 0);
        TEST_ASSERT_TRUE(thresholds_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/sched.thresholds",
                                                   thresholds_blob, thresholds_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds", "/mnt/files/sched.thresholds"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "thresholds", path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("sched", "thresholds", &info));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "thresholds");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
    TEST_ASSERT_EQUAL_STRING(managed_path, info.path);
    TEST_ASSERT_TRUE(info.size_bytes > 0);
    TEST_ASSERT_TRUE(info.checksum != 0);

    {
        uint8_t rebalance_payload[32];
        uint8_t rebalance_blob[128];
        size_t rebalance_payload_len =
            build_sched_rebalance_payload(1u, 7u, 2u,
                                          rebalance_payload,
                                          sizeof(rebalance_payload));
        size_t rebalance_blob_len =
            build_outer_blob(SCHED_MODEL_KIND_REBALANCE,
                             rebalance_payload,
                             rebalance_payload_len,
                             rebalance_blob,
                             sizeof(rebalance_blob));

        TEST_ASSERT_TRUE(rebalance_payload_len > 0);
        TEST_ASSERT_TRUE(rebalance_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/sched.rebalance",
                                                   rebalance_blob, rebalance_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "rebalance", "/mnt/files/sched.rebalance"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "rebalance", path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("sched", "rebalance", &info));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "rebalance");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
    TEST_ASSERT_EQUAL_STRING(managed_path, info.path);
    TEST_ASSERT_TRUE(info.size_bytes > 0);
    TEST_ASSERT_TRUE(info.checksum != 0);
#else
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("sched", "config", "/mnt/files/sched.cfg"));
#endif

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
}

static void test_blob_boot_autoload_activates_runtime_blobs(void)
{
    uint8_t ev_payload[80];
#ifdef CONFIG_AI_SCHEDULER
    uint8_t sched_payload[40];
    uint8_t thresholds_payload[48];
    uint8_t rebalance_payload[32];
#endif
    uint8_t ev_blob[128];
#ifdef CONFIG_AI_SCHEDULER
    uint8_t sched_blob[128];
    uint8_t thresholds_blob[128];
    uint8_t rebalance_blob[128];
#endif
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    RustEvictionBlobStatus ev_status = {0};
#ifdef CONFIG_AI_SCHEDULER
    size_t sched_payload_len = build_sched_config_payload(sched_payload, sizeof(sched_payload));
    size_t sched_blob_len = build_outer_blob(SCHED_MODEL_KIND_CONFIG, sched_payload, sched_payload_len,
                                             sched_blob, sizeof(sched_blob));
    size_t thresholds_payload_len =
        build_sched_thresholds_payload(10u * 1000000u,
                                      50u * 1000000u,
                                      100u * 1000000u,
                                      thresholds_payload,
                                      sizeof(thresholds_payload));
    size_t thresholds_blob_len =
        build_outer_blob(SCHED_MODEL_KIND_THRESHOLDS, thresholds_payload, thresholds_payload_len,
                         thresholds_blob, sizeof(thresholds_blob));
    size_t rebalance_payload_len =
        build_sched_rebalance_payload(1u, 7u, 2u,
                                      rebalance_payload,
                                      sizeof(rebalance_payload));
    size_t rebalance_blob_len =
        build_outer_blob(SCHED_MODEL_KIND_REBALANCE, rebalance_payload, rebalance_payload_len,
                         rebalance_blob, sizeof(rebalance_blob));
    struct sched_model_status sched_status = {0};
    struct sched_model_status thresholds_status = {0};
    struct sched_model_status rebalance_status = {0};
#endif

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_TRUE(sched_payload_len > 0);
    TEST_ASSERT_TRUE(sched_blob_len > 0);
    TEST_ASSERT_TRUE(thresholds_payload_len > 0);
    TEST_ASSERT_TRUE(thresholds_blob_len > 0);
    TEST_ASSERT_TRUE(rebalance_payload_len > 0);
    TEST_ASSERT_TRUE(rebalance_blob_len > 0);
#endif
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_xgb.blob", ev_blob, ev_blob_len));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_cfg.blob", sched_blob, sched_blob_len));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_thresholds.blob",
                                               thresholds_blob, thresholds_blob_len));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_rebalance.blob",
                                               rebalance_blob, rebalance_blob_len));
#endif

    rust_eviction_blob_clear(1);
#ifdef CONFIG_AI_SCHEDULER
    sched_model_clear(SCHED_MODEL_KIND_CONFIG);
    sched_model_clear(SCHED_MODEL_KIND_THRESHOLDS);
    sched_model_clear(SCHED_MODEL_KIND_REBALANCE);
#endif
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/autoload_xgb.blob"));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds",
                                               "/mnt/files/autoload_sched_thresholds.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "rebalance",
                                               "/mnt/files/autoload_sched_rebalance.blob"));
#else
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
#endif

    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_xgb.blob"));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_sched_cfg.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_sched_thresholds.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_sched_rebalance.blob"));
#endif

    blob_boot_autoload();

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_CONFIG, &sched_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, sched_status.state);
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_THRESHOLDS, &thresholds_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, thresholds_status.state);
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_REBALANCE, &rebalance_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, rebalance_status.state);
#endif
}

static void test_blob_autoload_persists_authoritative_copy_on_boot_fat(void)
{
    struct blkdev *fat_dev = NULL;
    RustEvictionBlobStatus ev_status = {0};
    struct blob_autoload_info info;
    char path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/persistent_xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost",
                                               "/mnt/files/persistent_xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", info.path);
    TEST_ASSERT_EQUAL_UINT32(ev_blob_len, info.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(fnv1a32(ev_blob, ev_blob_len), info.checksum);

    rust_eviction_blob_clear(1);
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/persistent_xgb.blob"));
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_rejects_fat_source_paths(void)
{
    struct blkdev *fat_dev = NULL;
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    write_boot_media_fat_file("0:/fat-src.blob", ev_blob, (UINT)ev_blob_len);

    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("eviction", "xgboost", "0:/fat-src.blob"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_fat_update_invalidates_lfs_fallback(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/fallback-old.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/fallback-old.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-xgboost.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/fallback-new.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/fallback-new.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);

    boot_media_test_clear_device();
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_fat_authority_mount_failure_preserves_config(void)
{
    struct blkdev *fat_dev = NULL;
    RustEvictionBlobStatus ev_status = {0};
    char path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/fat-authority-old.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/fat-authority-old.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/fat-authority-new.blob", ev_blob, ev_blob_len));
    blob_autoload_test_fail_next_fat_mounts(2u);
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/fat-authority-new.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);

    rust_eviction_blob_clear(1);
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/fat-authority-old.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/fat-authority-new.blob"));
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_set_ignores_unrelated_stale_lfs_entry(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    uint8_t xgb_payload[80];
    uint8_t xgb_blob[128];
    uint8_t cacheus_payload[24];
    uint8_t cacheus_blob[128];
    size_t xgb_payload_len = build_eviction_xgb_payload(xgb_payload, sizeof(xgb_payload));
    size_t xgb_blob_len = build_outer_blob(1, xgb_payload, xgb_payload_len, xgb_blob, sizeof(xgb_blob));
    size_t cacheus_payload_len =
        build_eviction_cacheus_config_payload(cacheus_payload, sizeof(cacheus_payload));
    size_t cacheus_blob_len =
        build_outer_blob(3, cacheus_payload, cacheus_payload_len,
                         cacheus_blob, sizeof(cacheus_blob));

    TEST_ASSERT_TRUE(xgb_payload_len > 0);
    TEST_ASSERT_TRUE(xgb_blob_len > 0);
    TEST_ASSERT_TRUE(cacheus_payload_len > 0);
    TEST_ASSERT_TRUE(cacheus_blob_len > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/stale-cacheus.blob",
                                               cacheus_blob, cacheus_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "cacheus_config",
                                               "/mnt/files/stale-cacheus.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/stale-cacheus.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "cacheus_config", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-cacheus_config.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/update-xgb.blob", xgb_blob, xgb_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/update-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "cacheus_config", path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_set_succeeds_with_existing_fat_backed_entry(void)
{
    struct blkdev *fat_dev = NULL;
    char path[VFS_MAX_PATH];
    uint8_t xgb_payload[80];
    uint8_t xgb_blob[128];
    uint8_t cacheus_payload[24];
    uint8_t cacheus_blob[128];
    size_t xgb_payload_len = build_eviction_xgb_payload(xgb_payload, sizeof(xgb_payload));
    size_t xgb_blob_len = build_outer_blob(1, xgb_payload, xgb_payload_len, xgb_blob, sizeof(xgb_blob));
    size_t cacheus_payload_len =
        build_eviction_cacheus_config_payload(cacheus_payload, sizeof(cacheus_payload));
    size_t cacheus_blob_len =
        build_outer_blob(3, cacheus_payload, cacheus_payload_len,
                         cacheus_blob, sizeof(cacheus_blob));

    TEST_ASSERT_TRUE(xgb_payload_len > 0);
    TEST_ASSERT_TRUE(xgb_blob_len > 0);
    TEST_ASSERT_TRUE(cacheus_payload_len > 0);
    TEST_ASSERT_TRUE(cacheus_blob_len > 0);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/existing-fat-xgb.blob",
                                               xgb_blob, xgb_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost",
                                               "/mnt/files/existing-fat-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/new-fat-cacheus.blob",
                                               cacheus_blob, cacheus_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "cacheus_config",
                                               "/mnt/files/new-fat-cacheus.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "cacheus_config", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-cacheus_config.blob", path);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "cacheus_config"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_init_migrates_lfs_entries_to_fat(void)
{
    struct blkdev *fat_dev = NULL;
    RustEvictionBlobStatus ev_status = {0};
    char path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/init-migrate-xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/init-migrate-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-xgboost.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);

    rust_eviction_blob_clear(1);
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/init-migrate-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload/eviction-xgboost.blob"));
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_set_migrates_all_lfs_entries_to_fat(void)
{
    struct blkdev *fat_dev = NULL;
    RustEvictionBlobStatus xgb_status = {0};
    RustEvictionBlobStatus cacheus_status = {0};
    char path[VFS_MAX_PATH];
    uint8_t xgb_payload_old[128];
    uint8_t xgb_payload_new[128];
    uint8_t xgb_blob_old[128];
    uint8_t xgb_blob_new[128];
    uint8_t cacheus_payload[24];
    uint8_t cacheus_blob[128];
    size_t xgb_payload_len = build_eviction_xgb_payload(xgb_payload_old, sizeof(xgb_payload_old));
    size_t xgb_blob_old_len = build_outer_blob(1, xgb_payload_old, xgb_payload_len,
                                               xgb_blob_old, sizeof(xgb_blob_old));
    memcpy(xgb_payload_new, xgb_payload_old, xgb_payload_len);
    xgb_payload_new[xgb_payload_len - 1] ^= 0x01u;
    size_t xgb_blob_new_len = build_outer_blob(1, xgb_payload_new, xgb_payload_len,
                                               xgb_blob_new, sizeof(xgb_blob_new));
    size_t cacheus_payload_len =
        build_eviction_cacheus_config_payload(cacheus_payload, sizeof(cacheus_payload));
    size_t cacheus_blob_len =
        build_outer_blob(3, cacheus_payload, cacheus_payload_len,
                         cacheus_blob, sizeof(cacheus_blob));

    TEST_ASSERT_TRUE(xgb_payload_len > 0);
    TEST_ASSERT_TRUE(xgb_blob_old_len > 0);
    TEST_ASSERT_TRUE(xgb_blob_new_len > 0);
    TEST_ASSERT_TRUE(cacheus_payload_len > 0);
    TEST_ASSERT_TRUE(cacheus_blob_len > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/migrate-old-xgb.blob",
                                               xgb_blob_old, xgb_blob_old_len));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/migrate-cacheus.blob",
                                               cacheus_blob, cacheus_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/migrate-old-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "cacheus_config",
                                               "/mnt/files/migrate-cacheus.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "cacheus_config", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/autoload/eviction-cacheus_config.blob", path);

    make_boot_media_fat_volume(&fat_dev);
    boot_media_test_set_device(fat_dev);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/migrate-new-xgb.blob",
                                               xgb_blob_new, xgb_blob_new_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/migrate-new-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-xgboost.blob", path);
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "cacheus_config", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("0:/slmstore/autoload/eviction-cacheus_config.blob", path);

    rust_eviction_blob_clear(1);
    rust_eviction_blob_clear(3);
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/migrate-old-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/migrate-new-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/migrate-cacheus.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload/eviction-xgboost.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload/eviction-cacheus_config.blob"));
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &xgb_status));
    TEST_ASSERT_EQUAL_UINT32(1, xgb_status.has_active);
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(3, &cacheus_status));
    TEST_ASSERT_EQUAL_UINT32(1, cacheus_status.has_active);

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "cacheus_config"));
    destroy_boot_media_fat_volume(fat_dev);
}

static void test_blob_autoload_shell_commands(void)
{
    char path[VFS_MAX_PATH];
    char managed_path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/cmd_xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("eviction model autoload set xgboost /mnt/files/cmd_xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);

#ifdef CONFIG_AI_SCHEDULER
    {
        uint8_t sched_payload[40];
        uint8_t sched_blob[128];
        size_t sched_payload_len = build_sched_config_payload(sched_payload, sizeof(sched_payload));
        size_t sched_blob_len = build_outer_blob(SCHED_MODEL_KIND_CONFIG, sched_payload, sched_payload_len,
                                                 sched_blob, sizeof(sched_blob));

        TEST_ASSERT_TRUE(sched_payload_len > 0);
        TEST_ASSERT_TRUE(sched_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/cmd_sched_cfg.blob",
                                                   sched_blob, sched_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("sched model autoload set config /mnt/files/cmd_sched_cfg.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "config", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "config");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);

    {
        uint8_t thresholds_payload[48];
        uint8_t thresholds_blob[128];
        size_t thresholds_payload_len =
            build_sched_thresholds_payload(10u * 1000000u,
                                          50u * 1000000u,
                                          100u * 1000000u,
                                          thresholds_payload,
                                          sizeof(thresholds_payload));
        size_t thresholds_blob_len =
            build_outer_blob(SCHED_MODEL_KIND_THRESHOLDS,
                             thresholds_payload,
                             thresholds_payload_len,
                             thresholds_blob,
                             sizeof(thresholds_blob));

        TEST_ASSERT_TRUE(thresholds_payload_len > 0);
        TEST_ASSERT_TRUE(thresholds_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/cmd_sched_thresholds.blob",
                                                   thresholds_blob, thresholds_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("sched model autoload set thresholds /mnt/files/cmd_sched_thresholds.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "thresholds", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "thresholds");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);

    {
        uint8_t rebalance_payload[32];
        uint8_t rebalance_blob[128];
        size_t rebalance_payload_len =
            build_sched_rebalance_payload(1u, 7u, 2u,
                                          rebalance_payload,
                                          sizeof(rebalance_payload));
        size_t rebalance_blob_len =
            build_outer_blob(SCHED_MODEL_KIND_REBALANCE,
                             rebalance_payload,
                             rebalance_payload_len,
                             rebalance_blob,
                             sizeof(rebalance_blob));

        TEST_ASSERT_TRUE(rebalance_payload_len > 0);
        TEST_ASSERT_TRUE(rebalance_blob_len > 0);
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/cmd_sched_rebalance.blob",
                                                   rebalance_blob, rebalance_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0,
        shell_execute("sched model autoload set rebalance /mnt/files/cmd_sched_rebalance.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "rebalance", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "rebalance");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
#else
    TEST_ASSERT_EQUAL_INT(1,
        shell_execute("sched model autoload set config /mnt/files/cmd_sched_cfg.blob"));
#endif

    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model autoload clear xgboost"));
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
}

static void test_blob_autoload_rejects_invalid_paths(void)
{
    char path[VFS_MAX_PATH];
    uint8_t bad_blob[32];

    memset(bad_blob, 0xA5, sizeof(bad_blob));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("eviction", "xgboost"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("sched", "config"));

    TEST_ASSERT_EQUAL_INT(1, shell_execute(
        "eviction model autoload set xgboost /mnt/files/missing-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/invalid-sched.blob",
                                               bad_blob, sizeof(bad_blob)));
    TEST_ASSERT_EQUAL_INT(1, shell_execute(
        "sched model autoload set config /mnt/files/invalid-sched.blob"));
    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("sched", "config", path, sizeof(path)));
}

static void test_blob_autoload_overwrites_existing_conf(void)
{
    char path[VFS_MAX_PATH];
    char conf_buf[512];
    char managed_path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/first-xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/second-xgb.blob", ev_blob, ev_blob_len));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/first-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/second-xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);

    TEST_ASSERT_TRUE(read_text_file(BLOB_AUTOLOAD_CONF_PATH, conf_buf, sizeof(conf_buf)) > 0);
    TEST_ASSERT_NULL(find_substr(conf_buf, "/mnt/files/first-xgb.blob"));
    TEST_ASSERT_NOT_NULL(find_substr(conf_buf, managed_path));
    TEST_ASSERT_NOT_NULL(find_substr(conf_buf, " 90 "));
}

static void test_blob_autoload_set_preserves_existing_entry_on_write_failure(void)
{
    struct blob_autoload_info info_before;
    struct blob_autoload_info info_after;
    RustEvictionBlobStatus ev_status = {0};
    uint8_t payload_a[80];
    uint8_t payload_b[80];
    uint8_t blob_a[128];
    uint8_t blob_b[128];
    size_t payload_len_a = build_eviction_xgb_payload(payload_a, sizeof(payload_a));
    size_t payload_len_b = build_eviction_xgb_payload(payload_b, sizeof(payload_b));
    size_t blob_len_a;
    size_t blob_len_b;
    uint32_t alt_leaf_bits = 0x3f19999au;

    TEST_ASSERT_TRUE(payload_len_a > 0);
    TEST_ASSERT_TRUE(payload_len_b > 0);
    memcpy(payload_b + payload_len_b - 4, &alt_leaf_bits, sizeof(alt_leaf_bits));
    blob_len_a = build_outer_blob(1, payload_a, payload_len_a, blob_a, sizeof(blob_a));
    blob_len_b = build_outer_blob(1, payload_b, payload_len_b, blob_b, sizeof(blob_b));
    TEST_ASSERT_TRUE(blob_len_a > 0);
    TEST_ASSERT_TRUE(blob_len_b > 0);

    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/preserve-a.blob", blob_a, blob_len_a));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/preserve-b.blob", blob_b, blob_len_b));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/preserve-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info_before));

    blob_autoload_test_fail_next_write();
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/preserve-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info_after));
    TEST_ASSERT_EQUAL_STRING(info_before.path, info_after.path);
    TEST_ASSERT_EQUAL_UINT32(info_before.size_bytes, info_after.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(info_before.checksum, info_after.checksum);

    rust_eviction_blob_clear(1);
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
}

static void test_blob_autoload_clear_preserves_existing_entry_on_write_failure(void)
{
    struct blob_autoload_info info_before;
    struct blob_autoload_info info_after;
    RustEvictionBlobStatus ev_status = {0};
    uint8_t payload[80];
    uint8_t blob[128];
    size_t payload_len = build_eviction_xgb_payload(payload, sizeof(payload));
    size_t blob_len = build_outer_blob(1, payload, payload_len, blob, sizeof(blob));

    TEST_ASSERT_TRUE(payload_len > 0);
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/clear-preserve.blob", blob, blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/clear-preserve.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info_before));

    blob_autoload_test_fail_next_write();
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_clear("eviction", "xgboost"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_info_get("eviction", "xgboost", &info_after));
    TEST_ASSERT_EQUAL_STRING(info_before.path, info_after.path);
    TEST_ASSERT_EQUAL_UINT32(info_before.size_bytes, info_after.size_bytes);
    TEST_ASSERT_EQUAL_UINT32(info_before.checksum, info_after.checksum);

    rust_eviction_blob_clear(1);
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
}

static void test_blob_autoload_recovers_from_backup_conf(void)
{
    char path[VFS_MAX_PATH];
    char managed_path[VFS_MAX_PATH];
    const char *subpath = NULL;
    struct lfs_mount *mnt;
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/recover-xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/recover-xgb.blob"));

    mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);
    (void)littlefs_remove(mnt, "/blob_autoload.conf.bak");
    TEST_ASSERT_EQUAL_INT(0, littlefs_rename(mnt, "/blob_autoload.conf", "/blob_autoload.conf.bak"));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
}

static void test_blob_boot_autoload_skips_tampered_managed_blob(void)
{
    char managed_path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    uint8_t bad_blob[32];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    RustEvictionBlobStatus ev_status = {0};

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/tamper-xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/tamper-xgb.blob"));

    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    memset(bad_blob, 0xA5, sizeof(bad_blob));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(managed_path, bad_blob, sizeof(bad_blob)));

    rust_eviction_blob_clear(1);
    blob_boot_autoload();

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(0, ev_status.has_active);
}

static void test_blob_autoload_accepts_legacy_entry_without_metadata(void)
{
    char managed_path[VFS_MAX_PATH];
    char path[VFS_MAX_PATH];
    char conf_buf[512];
    RustEvictionBlobStatus ev_status = {0};
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    int conf_len;

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(managed_path, ev_blob, ev_blob_len));

    conf_len = snprintf(conf_buf, sizeof(conf_buf),
                        "# Runtime blob autoload config\n"
                        "# Format: <domain> <kind> <absolute-path> <size-bytes> <checksum-hex>\n"
                        "# Entries listed here are staged and activated at boot.\n"
                        "eviction xgboost %s\n",
                        managed_path);
    TEST_ASSERT_TRUE(conf_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(BLOB_AUTOLOAD_CONF_PATH,
                                               (const uint8_t *)conf_buf,
                                               (size_t)conf_len));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
    rust_eviction_blob_clear(1);
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
}

static void test_blob_autoload_rejects_malformed_metadata_suffix(void)
{
    char managed_path[VFS_MAX_PATH];
    char path[VFS_MAX_PATH];
    char conf_buf[512];
    RustEvictionBlobStatus ev_status = {0};
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    int conf_len;

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    build_managed_path(managed_path, sizeof(managed_path), "eviction", "xgboost");
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(managed_path, ev_blob, ev_blob_len));

    conf_len = snprintf(conf_buf, sizeof(conf_buf),
                        "# Runtime blob autoload config\n"
                        "# Format: <domain> <kind> <absolute-path> <size-bytes> <checksum-hex>\n"
                        "# Entries listed here are staged and activated at boot.\n"
                        "eviction xgboost %s 90 nothex\n",
                        managed_path);
    TEST_ASSERT_TRUE(conf_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(BLOB_AUTOLOAD_CONF_PATH,
                                               (const uint8_t *)conf_buf,
                                               (size_t)conf_len));

    TEST_ASSERT_EQUAL_INT(1, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    rust_eviction_blob_clear(1);
    blob_boot_autoload();
    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(0, ev_status.has_active);
}

#ifdef CONFIG_AI_SCHEDULER
static void test_blob_autoload_accepts_max_length_paths_across_all_slots(void)
{
    char path_xgb[VFS_MAX_PATH];
    char path_mlp[VFS_MAX_PATH];
    char path_cacheus[VFS_MAX_PATH];
    char path_sched_mlp[VFS_MAX_PATH];
    char path_sched_ppo[VFS_MAX_PATH];
    char path_sched_cfg[VFS_MAX_PATH];
    char path_sched_thresholds[VFS_MAX_PATH];
    char path_sched_rebalance[VFS_MAX_PATH];
    char conf_buf[1400];
    uint8_t ev_xgb_payload[80];
    /* `static` so the ~17 KB buffer lives in BSS — kernel stacks are
     * 16 KB fixed and would not survive an automatic-storage version. */
    static uint8_t ev_mlp_payload[EVICTION_MLP_PAYLOAD_BYTES];
    uint8_t ev_cacheus_payload[24];
    uint8_t sched_dense_payload[4096];
    uint8_t sched_cfg_payload[40];
    uint8_t sched_thresholds_payload[48];
    uint8_t sched_rebalance_payload[32];
    /* Sized for the MLP (worst-case payload) plus the outer header. */
    static uint8_t blob[EVICTION_MLP_PAYLOAD_BYTES + BLOB_OUTER_HEADER_BYTES];
    char managed_path[VFS_MAX_PATH];
    size_t payload_len;
    size_t blob_len;
    int conf_len;

    build_long_path(path_xgb, sizeof(path_xgb), "xgb-", 'a', ".blob");
    build_long_path(path_mlp, sizeof(path_mlp), "mlp-", 'b', ".blob");
    build_long_path(path_cacheus, sizeof(path_cacheus), "cacheus-", 'c', ".blob");
    build_long_path(path_sched_mlp, sizeof(path_sched_mlp), "sched-mlp-", 'd', ".blob");
    build_long_path(path_sched_ppo, sizeof(path_sched_ppo), "sched-ppo-", 'e', ".blob");
    build_long_path(path_sched_cfg, sizeof(path_sched_cfg), "sched-cfg-", 'f', ".blob");
    build_long_path(path_sched_thresholds, sizeof(path_sched_thresholds), "sched-thr-", 'g', ".blob");
    build_long_path(path_sched_rebalance, sizeof(path_sched_rebalance), "sched-rebal-", 'h', ".blob");

    payload_len = build_eviction_xgb_payload(ev_xgb_payload, sizeof(ev_xgb_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(1, ev_xgb_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_xgb, blob, blob_len));

    payload_len = build_eviction_mlp_payload(0x3F800000u, ev_mlp_payload, sizeof(ev_mlp_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(2, ev_mlp_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_mlp, blob, blob_len));

    memset(ev_cacheus_payload, 0, sizeof(ev_cacheus_payload));
    ev_cacheus_payload[0] = 'C'; ev_cacheus_payload[1] = 'C';
    ev_cacheus_payload[2] = 'F'; ev_cacheus_payload[3] = 'G';
    ev_cacheus_payload[4] = 1; ev_cacheus_payload[5] = 0;
    ev_cacheus_payload[8] = 1; /* all5 */
    { uint32_t lr_bits = 0x3e4ccccdu; memcpy(ev_cacheus_payload + 12, &lr_bits, 4); }
    { uint32_t window = 200u; memcpy(ev_cacheus_payload + 16, &window, 4); }
    { uint32_t min_weight_bits = 0x3dcccccdu; memcpy(ev_cacheus_payload + 20, &min_weight_bits, 4); }
    blob_len = build_outer_blob(3, ev_cacheus_payload, sizeof(ev_cacheus_payload), blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_cacheus, blob, blob_len));

    payload_len = build_sched_mlp_payload(0x3F800000u, sched_dense_payload, sizeof(sched_dense_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(SCHED_MODEL_KIND_MLP, sched_dense_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_sched_mlp, blob, blob_len));

    blob_len = build_outer_blob(SCHED_MODEL_KIND_PPO, sched_dense_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_sched_ppo, blob, blob_len));

    payload_len = build_sched_config_payload(sched_cfg_payload, sizeof(sched_cfg_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(SCHED_MODEL_KIND_CONFIG, sched_cfg_payload, payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_sched_cfg, blob, blob_len));

    payload_len = build_sched_thresholds_payload(10u * 1000000u,
                                                 50u * 1000000u,
                                                 100u * 1000000u,
                                                 sched_thresholds_payload,
                                                 sizeof(sched_thresholds_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(SCHED_MODEL_KIND_THRESHOLDS, sched_thresholds_payload,
                                payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_sched_thresholds, blob, blob_len));

    payload_len = build_sched_rebalance_payload(1u, 7u, 2u,
                                                sched_rebalance_payload,
                                                sizeof(sched_rebalance_payload));
    TEST_ASSERT_TRUE(payload_len > 0);
    blob_len = build_outer_blob(SCHED_MODEL_KIND_REBALANCE, sched_rebalance_payload,
                                payload_len, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_sched_rebalance, blob, blob_len));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", path_xgb));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "mlp", path_mlp));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "cacheus_config", path_cacheus));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "mlp", path_sched_mlp));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "ppo", path_sched_ppo));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", path_sched_cfg));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds", path_sched_thresholds));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "rebalance", path_sched_rebalance));

    conf_len = read_text_file(BLOB_AUTOLOAD_CONF_PATH, conf_buf, sizeof(conf_buf));
    TEST_ASSERT_TRUE(conf_len > 0);
    TEST_ASSERT_NULL(find_substr(conf_buf, path_sched_cfg));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "rebalance");
    TEST_ASSERT_NOT_NULL(find_substr(conf_buf, managed_path));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("sched", "config"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", path_sched_cfg));
}
#endif

int test_suite_blob_autoload(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blob_autoload_init_creates_conf);
    RUN_TEST(test_blob_autoload_init_marks_existing_fat_config_authoritative);
    RUN_TEST(test_blob_autoload_init_recovers_missing_fat_blob_from_lfs);
    RUN_TEST(test_blob_autoload_init_uses_fat_backup_when_primary_empty);
    RUN_TEST(test_blob_autoload_init_drops_unusable_fat_entry_without_lfs_fallback);
    RUN_TEST(test_blob_autoload_set_get_clear_round_trip);
    RUN_TEST(test_blob_boot_autoload_activates_runtime_blobs);
    RUN_TEST(test_blob_autoload_persists_authoritative_copy_on_boot_fat);
    RUN_TEST(test_blob_autoload_rejects_fat_source_paths);
    RUN_TEST(test_blob_autoload_fat_update_invalidates_lfs_fallback);
    RUN_TEST(test_blob_autoload_fat_authority_mount_failure_preserves_config);
    RUN_TEST(test_blob_autoload_set_ignores_unrelated_stale_lfs_entry);
    RUN_TEST(test_blob_autoload_set_succeeds_with_existing_fat_backed_entry);
    RUN_TEST(test_blob_autoload_init_migrates_lfs_entries_to_fat);
    RUN_TEST(test_blob_autoload_set_migrates_all_lfs_entries_to_fat);
    RUN_TEST(test_blob_autoload_shell_commands);
    RUN_TEST(test_blob_autoload_rejects_invalid_paths);
    RUN_TEST(test_blob_autoload_overwrites_existing_conf);
    RUN_TEST(test_blob_autoload_set_preserves_existing_entry_on_write_failure);
    RUN_TEST(test_blob_autoload_clear_preserves_existing_entry_on_write_failure);
    RUN_TEST(test_blob_autoload_recovers_from_backup_conf);
    RUN_TEST(test_blob_boot_autoload_skips_tampered_managed_blob);
    RUN_TEST(test_blob_autoload_accepts_legacy_entry_without_metadata);
    RUN_TEST(test_blob_autoload_rejects_malformed_metadata_suffix);
#ifdef CONFIG_AI_SCHEDULER
    RUN_TEST(test_blob_autoload_accepts_max_length_paths_across_all_slots);
#endif
    return UNITY_END();
}
