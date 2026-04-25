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

/* Only used by the CONFIG_AI_SCHEDULER tests below; gate to silence
 * `-Werror=unused-function` on default builds (issue #399). */
#ifdef CONFIG_AI_SCHEDULER
static size_t build_eviction_mlp_payload(uint32_t out_weight_bits,
                                         uint8_t *out,
                                         size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 8,
        L1_IN = 27,
        L1_OUT = 64,
        L2_OUT = 32,
        L3_OUT = 16,
        OUT_DIM = 1,
        W_L1_LEN = L1_OUT * L1_IN,
        B_L1_LEN = L1_OUT,
        W_L2_LEN = L2_OUT * L1_OUT,
        B_L2_LEN = L2_OUT,
        W_L3_LEN = L3_OUT * L2_OUT,
        B_L3_LEN = L3_OUT,
        W_OUT_LEN = OUT_DIM * L3_OUT,
        B_OUT_LEN = OUT_DIM,
        FLOAT_COUNT = W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN
                    + W_L3_LEN + B_L3_LEN + W_OUT_LEN + B_OUT_LEN,
        TOTAL = PAYLOAD_HEADER_LEN + FLOAT_COUNT * 4
    };
    size_t cursor = 0;
    size_t idx = 0;

    if (out_cap < TOTAL) return 0;
    memset(out, 0, TOTAL);
    out[0] = 'M'; out[1] = 'L'; out[2] = 'P'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 0; out[7] = 0;
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

    for (idx = 0; idx < FLOAT_COUNT; idx++) {
        uint32_t bits = 0u;
        if (idx == 0) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN
                + W_L3_LEN + B_L3_LEN) {
            bits = out_weight_bits;
        }
        WRITE_U32_LE(bits);
    }
#undef WRITE_U32_LE

    return cursor;
}
#endif /* CONFIG_AI_SCHEDULER for build_eviction_mlp_payload */

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

static void test_blob_autoload_set_get_clear_round_trip(void)
{
    char path[VFS_MAX_PATH];
    char managed_path[VFS_MAX_PATH];
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
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/sched.cfg", sched_blob, sched_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", "/mnt/files/sched.cfg"));
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
        TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/sched.thresholds",
                                                   thresholds_blob, thresholds_blob_len));
    }
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds", "/mnt/files/sched.thresholds"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_get("sched", "thresholds", path, sizeof(path)));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "thresholds");
    TEST_ASSERT_EQUAL_STRING(managed_path, path);
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
#endif
    uint8_t ev_blob[128];
#ifdef CONFIG_AI_SCHEDULER
    uint8_t sched_blob[128];
    uint8_t thresholds_blob[128];
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
    struct sched_model_status sched_status = {0};
    struct sched_model_status thresholds_status = {0};
#endif

    TEST_ASSERT_TRUE(ev_payload_len > 0);
    TEST_ASSERT_TRUE(ev_blob_len > 0);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_TRUE(sched_payload_len > 0);
    TEST_ASSERT_TRUE(sched_blob_len > 0);
    TEST_ASSERT_TRUE(thresholds_payload_len > 0);
    TEST_ASSERT_TRUE(thresholds_blob_len > 0);
#endif
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_xgb.blob", ev_blob, ev_blob_len));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_cfg.blob", sched_blob, sched_blob_len));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file("/mnt/files/autoload_sched_thresholds.blob",
                                               thresholds_blob, thresholds_blob_len));
#endif

    rust_eviction_blob_clear(1);
#ifdef CONFIG_AI_SCHEDULER
    sched_model_clear(SCHED_MODEL_KIND_CONFIG);
    sched_model_clear(SCHED_MODEL_KIND_THRESHOLDS);
#endif
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", "/mnt/files/autoload_xgb.blob"));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds",
                                               "/mnt/files/autoload_sched_thresholds.blob"));
#else
    TEST_ASSERT_NOT_EQUAL(0, blob_autoload_set("sched", "config", "/mnt/files/autoload_sched_cfg.blob"));
#endif

    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_xgb.blob"));
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_sched_cfg.blob"));
    TEST_ASSERT_EQUAL_INT(0, remove_file("/mnt/files/autoload_sched_thresholds.blob"));
#endif

    blob_boot_autoload();

    TEST_ASSERT_EQUAL_INT(0, rust_eviction_blob_status(1, &ev_status));
    TEST_ASSERT_EQUAL_UINT32(1, ev_status.has_active);
#ifdef CONFIG_AI_SCHEDULER
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_CONFIG, &sched_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, sched_status.state);
    TEST_ASSERT_EQUAL_INT(0, sched_model_status(SCHED_MODEL_KIND_THRESHOLDS, &thresholds_status));
    TEST_ASSERT_EQUAL_UINT16(SCHED_MODEL_ACTIVE, thresholds_status.state);
#endif
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
    char conf_buf[1400];
    uint8_t ev_xgb_payload[80];
    uint8_t ev_mlp_payload[2048];
    uint8_t ev_cacheus_payload[24];
    uint8_t sched_dense_payload[4096];
    uint8_t sched_cfg_payload[40];
    uint8_t sched_thresholds_payload[48];
    uint8_t blob[8192];
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

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "xgboost", path_xgb));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "mlp", path_mlp));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("eviction", "cacheus_config", path_cacheus));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "mlp", path_sched_mlp));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "ppo", path_sched_ppo));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", path_sched_cfg));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "thresholds", path_sched_thresholds));

    conf_len = read_text_file(BLOB_AUTOLOAD_CONF_PATH, conf_buf, sizeof(conf_buf));
    TEST_ASSERT_TRUE(conf_len > 0);
    TEST_ASSERT_NULL(find_substr(conf_buf, path_sched_cfg));
    build_managed_path(managed_path, sizeof(managed_path), "sched", "thresholds");
    TEST_ASSERT_NOT_NULL(find_substr(conf_buf, managed_path));

    TEST_ASSERT_EQUAL_INT(0, blob_autoload_clear("sched", "config"));
    TEST_ASSERT_EQUAL_INT(0, blob_autoload_set("sched", "config", path_sched_cfg));
}
#endif

int test_suite_blob_autoload(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blob_autoload_init_creates_conf);
    RUN_TEST(test_blob_autoload_set_get_clear_round_trip);
    RUN_TEST(test_blob_boot_autoload_activates_runtime_blobs);
    RUN_TEST(test_blob_autoload_shell_commands);
    RUN_TEST(test_blob_autoload_rejects_invalid_paths);
    RUN_TEST(test_blob_autoload_overwrites_existing_conf);
    RUN_TEST(test_blob_autoload_recovers_from_backup_conf);
#ifdef CONFIG_AI_SCHEDULER
    RUN_TEST(test_blob_autoload_accepts_max_length_paths_across_all_slots);
#endif
    return UNITY_END();
}
