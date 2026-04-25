#include "unity.h"
#include "../include/blob_autoload.h"
#include "runtime_model.h"
#include "../include/shell.h"
#include "../include/slm_ffi.h"
#include "../include/vfs.h"
#include "../include/littlefs_slm.h"
#include "../include/string.h"

#include <stddef.h>
#include <stdint.h>

extern int32_t rust_eviction_blob_clear(uint16_t kind_id);
extern int32_t rust_eviction_blob_status(uint16_t kind_id, RustEvictionBlobStatus *out);

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

static uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static size_t build_outer_blob(uint16_t kind_id, const uint8_t *payload,
                               size_t payload_len, uint8_t *out, size_t out_cap)
{
    uint32_t checksum = fnv1a32(payload, payload_len);
    size_t total = 24 + payload_len;
    if (out_cap < total) return 0;

    out[0] = 'S'; out[1] = 'E'; out[2] = 'M'; out[3] = 'B';
    out[4] = 1; out[5] = 0;
    out[6] = (uint8_t)(kind_id & 0xFF);
    out[7] = (uint8_t)(kind_id >> 8);
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    out[12] = (uint8_t)(payload_len & 0xFF);
    out[13] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[14] = (uint8_t)((payload_len >> 16) & 0xFF);
    out[15] = (uint8_t)((payload_len >> 24) & 0xFF);
    out[16] = (uint8_t)(checksum & 0xFF);
    out[17] = (uint8_t)((checksum >> 8) & 0xFF);
    out[18] = (uint8_t)((checksum >> 16) & 0xFF);
    out[19] = (uint8_t)((checksum >> 24) & 0xFF);
    out[20] = 0; out[21] = 0; out[22] = 0; out[23] = 0;
    memcpy(out + 24, payload, payload_len);
    return total;
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

#ifdef CONFIG_AI_SCHEDULER
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
#endif

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

static void test_blob_autoload_init_creates_conf(void)
{
    char buf[256];
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_init());
    TEST_ASSERT_TRUE(read_text_file(BLOB_AUTOLOAD_CONF_PATH, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(find_substr(buf, "Runtime blob autoload config"));
}

static void test_blob_autoload_set_get_clear_round_trip(void)
{
    char path[VFS_MAX_PATH];
    uint8_t ev_payload[80];
    uint8_t ev_blob[128];
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/xgb.blob", ev_blob, ev_blob_len));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/xgb.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("eviction", "xgboost", path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/mnt/files/xgb.blob", path);

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
    TEST_ASSERT_EQUAL_STRING("/mnt/files/sched.cfg", path);
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
#endif
    uint8_t ev_blob[128];
#ifdef CONFIG_AI_SCHEDULER
    uint8_t sched_blob[128];
#endif
    size_t ev_payload_len = build_eviction_xgb_payload(ev_payload, sizeof(ev_payload));
    size_t ev_blob_len = build_outer_blob(1, ev_payload, ev_payload_len, ev_blob, sizeof(ev_blob));
    RustEvictionBlobStatus ev_status = {0};
#ifdef CONFIG_AI_SCHEDULER
    size_t sched_payload_len = build_sched_config_payload(sched_payload, sizeof(sched_payload));
    size_t sched_blob_len = build_outer_blob(SCHED_MODEL_KIND_CONFIG, sched_payload, sched_payload_len,
                                             sched_blob, sizeof(sched_blob));
    struct sched_model_status sched_status = {0};
#endif

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_TRUE(sched_payload_len > 0);
    TEST_ASSERT_TRUE(sched_blob_len > 0);
#endif
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_xgb.blob", ev_blob, ev_blob_len));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_cfg.blob", sched_blob, sched_blob_len));
#endif

    rust_eviction_blob_clear(1);
#ifdef CONFIG_AI_SCHEDULER
    sched_model_clear(SCHED_MODEL_KIND_CONFIG);
#endif
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/autoload_xgb.blob"));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
#else
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
#endif

    blob_boot_autoload();

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_CONFIG, &sched_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, sched_status.state);
#endif
}

static void test_blob_autoload_shell_commands(void)
{
    char path[VFS_MAX_PATH];
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
    TEST_ASSERT_EQUAL_STRING("/mnt/files/cmd_xgb.blob", path);

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
    TEST_ASSERT_EQUAL_STRING("/mnt/files/cmd_sched_cfg.blob", path);
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

int test_suite_blob_autoload(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blob_autoload_init_creates_conf);
    RUN_TEST(test_blob_autoload_set_get_clear_round_trip);
    RUN_TEST(test_blob_boot_autoload_activates_runtime_blobs);
    RUN_TEST(test_blob_autoload_shell_commands);
    RUN_TEST(test_blob_autoload_rejects_invalid_paths);
    return UNITY_END();
}
