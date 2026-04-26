#include "blob_autoload.h"

#include "littlefs_slm.h"
#include "runtime_blob_file.h"
#include "runtime_model.h"
#include "slm_ffi.h"
#include "shell_internal.h"
#include "fat32.h"
#include "boot_media.h"
#include "uart.h"
#include "vfs.h"
#include "string.h"
#include "../lib/fatfs/ff.h"

struct blob_autoload_entry {
    const char *domain;
    const char *kind;
    uint16_t kind_id;
    char path[VFS_MAX_PATH];
    uint32_t size_bytes;
    uint32_t checksum;
    int present;
};

#define BLOB_AUTOLOAD_CONF_TMP_PATH "/blob_autoload.conf.tmp"
#define BLOB_AUTOLOAD_CONF_BAK_LFS_PATH "/blob_autoload.conf.bak"
#define BLOB_AUTOLOAD_CONF_BAK_VFS_PATH "/mnt/files/blob_autoload.conf.bak"
#define BLOB_AUTOLOAD_STORE_LFS_DIR "/autoload"
#define BLOB_AUTOLOAD_FAT_AUTHORITY_MARKER_LFS_PATH BLOB_AUTOLOAD_STORE_LFS_DIR "/.fat-authoritative"
#define BLOB_AUTOLOAD_ENTRY_LINE_OVERHEAD 80u

#define BLOB_AUTOLOAD_FAT_VOL "0:"
#define BLOB_AUTOLOAD_FAT_ROOT "0:/slmstore"
#define BLOB_AUTOLOAD_FAT_AUTOLOAD_DIR BLOB_AUTOLOAD_FAT_ROOT "/autoload"
#define BLOB_AUTOLOAD_CONF_FAT_PATH BLOB_AUTOLOAD_FAT_ROOT "/blob_autoload.conf"
#define BLOB_AUTOLOAD_CONF_FAT_TMP_PATH BLOB_AUTOLOAD_FAT_ROOT "/blob_autoload.conf.tmp"
#define BLOB_AUTOLOAD_CONF_FAT_BAK_PATH BLOB_AUTOLOAD_FAT_ROOT "/blob_autoload.conf.bak"

static struct blob_autoload_entry blob_entries[] = {
    {"eviction", "xgboost", 1, {0}, 0, 0, 0},
    {"eviction", "mlp", 2, {0}, 0, 0, 0},
    {"eviction", "cacheus_config", 3, {0}, 0, 0, 0},
    {"sched", "mlp", SCHED_MODEL_KIND_MLP, {0}, 0, 0, 0},
    {"sched", "ppo", SCHED_MODEL_KIND_PPO, {0}, 0, 0, 0},
    {"sched", "config", SCHED_MODEL_KIND_CONFIG, {0}, 0, 0, 0},
    {"sched", "thresholds", SCHED_MODEL_KIND_THRESHOLDS, {0}, 0, 0, 0},
    {"sched", "rebalance", SCHED_MODEL_KIND_REBALANCE, {0}, 0, 0, 0},
};

enum {
    BLOB_AUTOLOAD_ENTRY_COUNT =
        (int)(sizeof(blob_entries) / sizeof(blob_entries[0])),
    BLOB_AUTOLOAD_CONF_BUF_SIZE =
        (int)(sizeof(
            "# Runtime blob autoload config\n"
            "# Format: <domain> <kind> <absolute-path> <size-bytes> <checksum-hex>\n"
            "# Entries listed here are staged and activated at boot.\n")) +
        (int)(sizeof(blob_entries) / sizeof(blob_entries[0])) *
            (int)(VFS_MAX_PATH + BLOB_AUTOLOAD_ENTRY_LINE_OVERHEAD)
};

struct blob_autoload_managed_txn {
    struct lfs_mount *mnt;
    char final_lfs[VFS_MAX_PATH];
    char temp_lfs[VFS_MAX_PATH];
    char bak_lfs[VFS_MAX_PATH];
    int had_existing;
    int active;
};

struct blob_autoload_fat_store {
    FATFS fs;
    struct blkdev *dev;
    int mounted;
};

struct blob_autoload_fat_txn {
    char final_path[VFS_MAX_PATH];
    char temp_path[VFS_MAX_PATH];
    char bak_path[VFS_MAX_PATH];
    int had_existing;
    int active;
};

static int g_blob_autoload_test_fail_next_write = 0;
static int g_blob_autoload_test_fail_next_fat_mount = 0;

static int blob_autoload_ensure_store_dir(struct lfs_mount *mnt);
static void blob_autoload_seed_entries(struct blob_autoload_entry *entries, size_t count);
static int blob_autoload_load_sources_with_fat_mounted(struct blob_autoload_entry *fat_entries,
                                                       struct blob_autoload_entry *lfs_entries,
                                                       size_t count);
static int blob_autoload_commit_fat_entries_mounted(const struct blob_autoload_entry *entries,
                                                    struct blob_autoload_fat_txn *txns,
                                                    size_t txn_count,
                                                    int mark_authoritative);
static int blob_autoload_reconcile_existing_fat_mounted(int fat_marker);
static int blob_autoload_bootstrap_fat_authority(void);
static int blob_autoload_mutate_entry(const char *domain,
                                      const char *kind,
                                      const char *resolved_path,
                                      int clear_entry);

static struct blkdev *blob_autoload_boot_partition_create(void)
{
    return boot_media_acquire();
}

static void blob_autoload_boot_partition_destroy(struct blkdev *dev)
{
    boot_media_release(dev);
}

static int blob_autoload_fat_mount(struct blob_autoload_fat_store *store)
{
    FRESULT res;

    if (!store) return -1;
    if (g_blob_autoload_test_fail_next_fat_mount > 0) {
        g_blob_autoload_test_fail_next_fat_mount--;
        return -1;
    }
    memset(store, 0, sizeof(*store));
    store->dev = blob_autoload_boot_partition_create();
    if (!store->dev) return -1;

    fatfs_disk_attach(store->dev);
    res = f_mount(&store->fs, BLOB_AUTOLOAD_FAT_VOL, 1);
    if (res != FR_OK) {
        fatfs_disk_detach();
        blob_autoload_boot_partition_destroy(store->dev);
        memset(store, 0, sizeof(*store));
        return -1;
    }
    store->mounted = 1;
    return 0;
}

static void blob_autoload_fat_unmount(struct blob_autoload_fat_store *store)
{
    if (!store || !store->mounted) return;
    (void)f_mount(NULL, BLOB_AUTOLOAD_FAT_VOL, 0);
    fatfs_disk_detach();
    blob_autoload_boot_partition_destroy(store->dev);
    memset(store, 0, sizeof(*store));
}

static int blob_autoload_fat_exists(const char *path)
{
    FILINFO info;
    return f_stat(path, &info) == FR_OK;
}

static int blob_autoload_fat_ensure_dirs(void)
{
    FRESULT res = f_mkdir(BLOB_AUTOLOAD_FAT_ROOT);
    if (res != FR_OK && res != FR_EXIST) return -1;
    res = f_mkdir(BLOB_AUTOLOAD_FAT_AUTOLOAD_DIR);
    if (res != FR_OK && res != FR_EXIST) return -1;
    return 0;
}

static int blob_autoload_fat_managed_path(const char *domain, const char *kind,
                                          char *out, size_t cap)
{
    int n = uart_snprintf(out, cap, BLOB_AUTOLOAD_FAT_AUTOLOAD_DIR "/%s-%s.blob",
                          domain, kind);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int blob_autoload_is_fat_path(const char *path)
{
    return path && path[0] == '0' && path[1] == ':' && path[2] == '/';
}

static int blob_autoload_fat_prepare_txn(const char *domain, const char *kind,
                                         struct blob_autoload_fat_txn *txn)
{
    if (!txn) return -1;
    memset(txn, 0, sizeof(*txn));
    if (blob_autoload_fat_managed_path(domain, kind,
                                       txn->final_path, sizeof(txn->final_path)) != 0) {
        return -1;
    }
    if (uart_snprintf(txn->temp_path, sizeof(txn->temp_path), "%s.tmp",
                      txn->final_path) >= (int)sizeof(txn->temp_path)) {
        return -1;
    }
    if (uart_snprintf(txn->bak_path, sizeof(txn->bak_path), "%s.bak",
                      txn->final_path) >= (int)sizeof(txn->bak_path)) {
        return -1;
    }
    return 0;
}

static void blob_entries_reset(struct blob_autoload_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        entries[i].path[0] = '\0';
        entries[i].size_bytes = 0;
        entries[i].checksum = 0;
        entries[i].present = 0;
    }
}

static struct blob_autoload_entry *blob_find_entry(struct blob_autoload_entry *entries,
                                                   size_t count,
                                                   const char *domain,
                                                   const char *kind)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static int blob_autoload_any_present(const struct blob_autoload_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (entries[i].present) {
            return 1;
        }
    }
    return 0;
}

static int blob_autoload_has_fat_authority_marker(void)
{
    struct vfs_entry_info info;
    return vfs_stat_path(BLOB_AUTOLOAD_STORE_DIR "/.fat-authoritative", &info) == 0;
}

static void blob_autoload_mark_fat_authoritative(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx(BLOB_AUTOLOAD_STORE_DIR, &subpath);
    int fd;

    if (!mnt) {
        return;
    }
    (void)blob_autoload_ensure_store_dir(mnt);
    fd = littlefs_file_open(mnt, BLOB_AUTOLOAD_FAT_AUTHORITY_MARKER_LFS_PATH,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd >= 0) {
        (void)littlefs_file_close(mnt, fd);
    }
}

static int blob_autoload_managed_path(const char *domain, const char *kind,
                                      char *path_out, size_t path_out_cap)
{
    int n;
    if (!path_out || path_out_cap == 0) return -1;
    n = uart_snprintf(path_out, path_out_cap, BLOB_AUTOLOAD_STORE_DIR "/%s-%s.blob",
                      domain, kind);
    if (n <= 0 || (size_t)n >= path_out_cap) {
        return -1;
    }
    return 0;
}

static int blob_autoload_managed_subpath(const char *domain, const char *kind,
                                         const char *suffix,
                                         char *path_out, size_t path_out_cap)
{
    int n;
    if (!path_out || path_out_cap == 0) return -1;
    n = uart_snprintf(path_out, path_out_cap, BLOB_AUTOLOAD_STORE_LFS_DIR "/%s-%s.blob%s",
                      domain, kind, suffix ? suffix : "");
    if (n <= 0 || (size_t)n >= path_out_cap) {
        return -1;
    }
    return 0;
}

static int blob_autoload_ensure_store_dir(struct lfs_mount *mnt)
{
    int rc;
    rc = littlefs_mkdir(mnt, BLOB_AUTOLOAD_STORE_LFS_DIR);
    if (rc == 0 || rc == LFS_ERR_EXIST) {
        return 0;
    }
    return -1;
}

static uint32_t blob_autoload_fnv1a32_update(uint32_t hash,
                                             const uint8_t *data,
                                             size_t len)
{
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static int blob_autoload_parse_u32(const char *text, int base, uint32_t *out)
{
    uint32_t value = 0;
    const char *p = text;
    if (!text || !*text || !out) return -1;
    while (*p) {
        uint32_t digit;
        char c = *p++;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10u + (uint32_t)(c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10u + (uint32_t)(c - 'A');
        else return -1;
        if (digit >= (uint32_t)base) return -1;
        value = value * (uint32_t)base + digit;
    }
    *out = value;
    return 0;
}

static int blob_autoload_fat_file_identity_mounted(const char *path,
                                                   uint32_t *size_out,
                                                   uint32_t *checksum_out)
{
    FIL fp;
    FILINFO info;
    FRESULT res;
    UINT bytes_read = 0;
    uint8_t buf[256];
    uint32_t checksum = 0x811C9DC5u;
    uint32_t total = 0;

    if (!path || !size_out || !checksum_out) return -1;
    res = f_stat(path, &info);
    if (res != FR_OK || (info.fattrib & AM_DIR) || info.fsize == 0) {
        return -1;
    }
    res = f_open(&fp, path, FA_READ);
    if (res != FR_OK) {
        return -1;
    }
    for (;;) {
        res = f_read(&fp, buf, sizeof(buf), &bytes_read);
        if (res != FR_OK) {
            (void)f_close(&fp);
            return -1;
        }
        if (bytes_read == 0) break;
        checksum = blob_autoload_fnv1a32_update(checksum, buf, (size_t)bytes_read);
        total += bytes_read;
    }
    (void)f_close(&fp);
    if (total != info.fsize) {
        return -1;
    }
    *size_out = total;
    *checksum_out = checksum;
    return 0;
}

static int blob_autoload_fat_file_identity(const char *path,
                                           uint32_t *size_out,
                                           uint32_t *checksum_out)
{
    struct blob_autoload_fat_store store;

    if (!path || !size_out || !checksum_out) return -1;
    if (blob_autoload_fat_mount(&store) != 0) {
        return -1;
    }
    if (blob_autoload_fat_file_identity_mounted(path, size_out, checksum_out) != 0) {
        blob_autoload_fat_unmount(&store);
        return -1;
    }
    blob_autoload_fat_unmount(&store);
    return 0;
}

static int blob_autoload_file_identity(const char *path,
                                       uint32_t *size_out,
                                       uint32_t *checksum_out)
{
    struct vfs_entry_info info;
    char buf[256];
    uint32_t checksum = 0x811C9DC5u;
    int offset = 0;

    if (!path || !size_out || !checksum_out) return -1;
    if (path[0] == '0' && path[1] == ':' && path[2] == '/') {
        return blob_autoload_fat_file_identity(path, size_out, checksum_out);
    }
    if (vfs_stat_path(path, &info) != 0 || info.type != 0 || info.size == 0) {
        return -1;
    }
    while (offset < (int)info.size) {
        size_t chunk = info.size - (uint32_t)offset;
        int bytes_read;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        bytes_read = vfs_read_path(path, buf, chunk, (size_t)offset);
        if (bytes_read <= 0 || (size_t)bytes_read != chunk) {
            return -1;
        }
        checksum = blob_autoload_fnv1a32_update(checksum,
                                                (const uint8_t *)buf,
                                                (size_t)bytes_read);
        offset += bytes_read;
    }
    *size_out = info.size;
    *checksum_out = checksum;
    return 0;
}

static int blob_autoload_prepare_managed_txn(const char *domain,
                                             const char *kind,
                                             struct blob_autoload_managed_txn *txn)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx(BLOB_AUTOLOAD_STORE_DIR, &subpath);
    if (!txn || !mnt) return -1;
    memset(txn, 0, sizeof(*txn));
    txn->mnt = mnt;
    if (blob_autoload_managed_subpath(domain, kind, "", txn->final_lfs, sizeof(txn->final_lfs)) != 0) {
        return -1;
    }
    if (blob_autoload_managed_subpath(domain, kind, ".tmp", txn->temp_lfs, sizeof(txn->temp_lfs)) != 0) {
        return -1;
    }
    if (blob_autoload_managed_subpath(domain, kind, ".bak", txn->bak_lfs, sizeof(txn->bak_lfs)) != 0) {
        return -1;
    }
    return 0;
}

static void blob_autoload_cleanup_managed_txn(struct blob_autoload_managed_txn *txn)
{
    if (!txn || !txn->mnt) return;
    (void)littlefs_remove(txn->mnt, txn->temp_lfs);
    if (!txn->active) {
        (void)littlefs_remove(txn->mnt, txn->bak_lfs);
    }
}

static int blob_autoload_stage_managed_replacement(const char *source_path,
                                                   const char *domain,
                                                   const char *kind,
                                                   struct blob_autoload_managed_txn *txn,
                                                   char *managed_path_out,
                                                   size_t managed_path_out_cap,
                                                   uint32_t *size_out,
                                                   uint32_t *checksum_out)
{
    struct vfs_entry_info source_info;
    struct vfs_entry_info existing_info;
    char final_vfs[VFS_MAX_PATH];
    char buf[512];
    uint32_t checksum = 0x811C9DC5u;
    int src_offset = 0;
    int fd;

    if (!txn || !size_out || !checksum_out) return -1;
    if (blob_autoload_prepare_managed_txn(domain, kind, txn) != 0) return -1;
    if (blob_autoload_ensure_store_dir(txn->mnt) != 0) return -1;
    if (blob_autoload_managed_path(domain, kind, final_vfs, sizeof(final_vfs)) != 0) return -1;
    if (vfs_stat_path(source_path, &source_info) != 0 || source_info.type != 0 || source_info.size == 0) {
        return -1;
    }

    fd = littlefs_file_open(txn->mnt, txn->temp_lfs, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;

    while (src_offset < (int)source_info.size) {
        size_t chunk = source_info.size - (uint32_t)src_offset;
        int bytes_read;
        int bytes_written;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        bytes_read = vfs_read_path(source_path, buf, chunk, (size_t)src_offset);
        if (bytes_read <= 0 || (size_t)bytes_read != chunk) {
            littlefs_file_close(txn->mnt, fd);
            blob_autoload_cleanup_managed_txn(txn);
            return -1;
        }
        bytes_written = littlefs_file_write(txn->mnt, fd, buf, (size_t)bytes_read);
        if (bytes_written != bytes_read) {
            littlefs_file_close(txn->mnt, fd);
            blob_autoload_cleanup_managed_txn(txn);
            return -1;
        }
        checksum = blob_autoload_fnv1a32_update(checksum, (const uint8_t *)buf, (size_t)bytes_read);
        src_offset += bytes_read;
    }
    littlefs_file_close(txn->mnt, fd);

    txn->had_existing = (vfs_stat_path(final_vfs, &existing_info) == 0);
    (void)littlefs_remove(txn->mnt, txn->bak_lfs);
    if (txn->had_existing) {
        if (littlefs_rename(txn->mnt, txn->final_lfs, txn->bak_lfs) != 0) {
            blob_autoload_cleanup_managed_txn(txn);
            return -1;
        }
    }
    if (littlefs_rename(txn->mnt, txn->temp_lfs, txn->final_lfs) != 0) {
        if (txn->had_existing) {
            (void)littlefs_rename(txn->mnt, txn->bak_lfs, txn->final_lfs);
        }
        blob_autoload_cleanup_managed_txn(txn);
        return -1;
    }
    txn->active = 1;
    if (managed_path_out && managed_path_out_cap > 0) {
        strncpy(managed_path_out, final_vfs, managed_path_out_cap - 1);
        managed_path_out[managed_path_out_cap - 1] = '\0';
    }
    *size_out = source_info.size;
    *checksum_out = checksum;
    return 0;
}

static int blob_autoload_stage_managed_clear(const char *domain,
                                             const char *kind,
                                             struct blob_autoload_managed_txn *txn)
{
    char final_vfs[VFS_MAX_PATH];
    struct vfs_entry_info existing_info;
    if (!txn) return -1;
    if (blob_autoload_prepare_managed_txn(domain, kind, txn) != 0) return -1;
    if (blob_autoload_managed_path(domain, kind, final_vfs, sizeof(final_vfs)) != 0) return -1;
    txn->had_existing = (vfs_stat_path(final_vfs, &existing_info) == 0);
    (void)littlefs_remove(txn->mnt, txn->temp_lfs);
    (void)littlefs_remove(txn->mnt, txn->bak_lfs);
    if (!txn->had_existing) {
        txn->active = 1;
        return 0;
    }
    if (littlefs_rename(txn->mnt, txn->final_lfs, txn->bak_lfs) != 0) {
        return -1;
    }
    txn->active = 1;
    return 0;
}

static void blob_autoload_commit_managed_txn(struct blob_autoload_managed_txn *txn)
{
    if (!txn || !txn->mnt || !txn->active) return;
    (void)littlefs_remove(txn->mnt, txn->temp_lfs);
    (void)littlefs_remove(txn->mnt, txn->bak_lfs);
    txn->active = 0;
}

static void blob_autoload_rollback_managed_txn(struct blob_autoload_managed_txn *txn)
{
    if (!txn || !txn->mnt || !txn->active) return;
    (void)littlefs_remove(txn->mnt, txn->temp_lfs);
    if (txn->had_existing) {
        (void)littlefs_remove(txn->mnt, txn->final_lfs);
        (void)littlefs_rename(txn->mnt, txn->bak_lfs, txn->final_lfs);
    } else {
        (void)littlefs_remove(txn->mnt, txn->final_lfs);
    }
    (void)littlefs_remove(txn->mnt, txn->bak_lfs);
    txn->active = 0;
}

static int blob_autoload_stage_managed_replacement_fat(const char *source_path,
                                                       const char *domain,
                                                       const char *kind,
                                                       struct blob_autoload_fat_txn *txn,
                                                       char *managed_path_out,
                                                       size_t managed_path_out_cap,
                                                       uint32_t *size_out,
                                                       uint32_t *checksum_out)
{
    struct vfs_entry_info source_info;
    FIL fp;
    FRESULT res;
    char buf[512];
    uint32_t checksum = 0x811C9DC5u;
    int src_offset = 0;

    if (!txn || !size_out || !checksum_out) return -1;
    if (blob_autoload_fat_prepare_txn(domain, kind, txn) != 0) return -1;
    if (blob_autoload_fat_ensure_dirs() != 0) return -1;
    if (vfs_stat_path(source_path, &source_info) != 0 || source_info.type != 0 || source_info.size == 0) {
        return -1;
    }

    txn->had_existing = blob_autoload_fat_exists(txn->final_path);
    (void)f_unlink(txn->temp_path);
    (void)f_unlink(txn->bak_path);

    res = f_open(&fp, txn->temp_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) return -1;
    while (src_offset < (int)source_info.size) {
        size_t chunk = source_info.size - (uint32_t)src_offset;
        int bytes_read;
        UINT bytes_written = 0;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        bytes_read = vfs_read_path(source_path, buf, chunk, (size_t)src_offset);
        if (bytes_read <= 0 || (size_t)bytes_read != chunk) {
            (void)f_close(&fp);
            (void)f_unlink(txn->temp_path);
            return -1;
        }
        res = f_write(&fp, buf, (UINT)bytes_read, &bytes_written);
        if (res != FR_OK || bytes_written != (UINT)bytes_read) {
            (void)f_close(&fp);
            (void)f_unlink(txn->temp_path);
            return -1;
        }
        checksum = blob_autoload_fnv1a32_update(checksum, (const uint8_t *)buf, (size_t)bytes_read);
        src_offset += bytes_read;
    }
    (void)f_close(&fp);

    if (txn->had_existing) {
        if (f_rename(txn->final_path, txn->bak_path) != FR_OK) {
            (void)f_unlink(txn->temp_path);
            return -1;
        }
    }
    if (f_rename(txn->temp_path, txn->final_path) != FR_OK) {
        if (txn->had_existing) {
            (void)f_rename(txn->bak_path, txn->final_path);
        }
        (void)f_unlink(txn->temp_path);
        return -1;
    }
    txn->active = 1;
    if (managed_path_out && managed_path_out_cap > 0) {
        strncpy(managed_path_out, txn->final_path, managed_path_out_cap - 1);
        managed_path_out[managed_path_out_cap - 1] = '\0';
    }
    *size_out = source_info.size;
    *checksum_out = checksum;
    return 0;
}

static int blob_autoload_stage_managed_clear_fat(const char *domain,
                                                 const char *kind,
                                                 struct blob_autoload_fat_txn *txn)
{
    if (!txn) return -1;
    if (blob_autoload_fat_prepare_txn(domain, kind, txn) != 0) return -1;
    txn->had_existing = blob_autoload_fat_exists(txn->final_path);
    (void)f_unlink(txn->temp_path);
    (void)f_unlink(txn->bak_path);
    if (!txn->had_existing) {
        txn->active = 1;
        return 0;
    }
    if (f_rename(txn->final_path, txn->bak_path) != FR_OK) {
        return -1;
    }
    txn->active = 1;
    return 0;
}

static void blob_autoload_commit_managed_fat_txn(struct blob_autoload_fat_txn *txn)
{
    if (!txn || !txn->active) return;
    (void)f_unlink(txn->temp_path);
    (void)f_unlink(txn->bak_path);
    txn->active = 0;
}

static void blob_autoload_rollback_managed_fat_txn(struct blob_autoload_fat_txn *txn)
{
    if (!txn || !txn->active) return;
    (void)f_unlink(txn->temp_path);
    if (txn->had_existing) {
        (void)f_unlink(txn->final_path);
        (void)f_rename(txn->bak_path, txn->final_path);
    } else {
        (void)f_unlink(txn->final_path);
    }
    (void)f_unlink(txn->bak_path);
    txn->active = 0;
}

static void blob_autoload_commit_fat_txn_array(struct blob_autoload_fat_txn *txns, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        blob_autoload_commit_managed_fat_txn(&txns[i]);
    }
}

static void blob_autoload_rollback_fat_txn_array(struct blob_autoload_fat_txn *txns, size_t count)
{
    while (count > 0) {
        count--;
        blob_autoload_rollback_managed_fat_txn(&txns[count]);
    }
}

static int blob_autoload_entry_is_usable(const struct blob_autoload_entry *entry)
{
    uint32_t actual_size = 0;
    uint32_t actual_checksum = 0;

    if (!entry || !entry->present || entry->path[0] == '\0') {
        return 0;
    }
    if (blob_autoload_file_identity(entry->path, &actual_size, &actual_checksum) != 0) {
        return 0;
    }
    if (entry->size_bytes != 0 || entry->checksum != 0) {
        if (actual_size != entry->size_bytes || actual_checksum != entry->checksum) {
            return 0;
        }
    }
    return 1;
}

static int blob_autoload_entry_is_usable_with_fat_mounted(const struct blob_autoload_entry *entry)
{
    uint32_t actual_size = 0;
    uint32_t actual_checksum = 0;

    if (!entry || !entry->present || entry->path[0] == '\0') {
        return 0;
    }
    if (blob_autoload_is_fat_path(entry->path)) {
        if (blob_autoload_fat_file_identity_mounted(entry->path, &actual_size, &actual_checksum) != 0) {
            return 0;
        }
    } else if (blob_autoload_file_identity(entry->path, &actual_size, &actual_checksum) != 0) {
        return 0;
    }
    if (entry->size_bytes != 0 || entry->checksum != 0) {
        if (actual_size != entry->size_bytes || actual_checksum != entry->checksum) {
            return 0;
        }
    }
    return 1;
}

static void blob_autoload_reconcile_entries(struct blob_autoload_entry *dst,
                                            const struct blob_autoload_entry *src,
                                            size_t count,
                                            int fat_mounted)
{
    for (size_t i = 0; i < count; i++) {
        int dst_usable;

        if (!src[i].present ||
            !(fat_mounted ? blob_autoload_entry_is_usable_with_fat_mounted(&src[i])
                          : blob_autoload_entry_is_usable(&src[i]))) {
            continue;
        }
        dst_usable = fat_mounted ? blob_autoload_entry_is_usable_with_fat_mounted(&dst[i])
                                 : blob_autoload_entry_is_usable(&dst[i]);
        if (dst[i].present && dst_usable) {
            continue;
        }
        strncpy(dst[i].path, src[i].path, sizeof(dst[i].path) - 1);
        dst[i].path[sizeof(dst[i].path) - 1] = '\0';
        dst[i].size_bytes = src[i].size_bytes;
        dst[i].checksum = src[i].checksum;
        dst[i].present = 1;
    }
}

static int blob_autoload_migrate_entries_to_fat(struct blob_autoload_entry *entries,
                                                size_t count,
                                                const char *skip_domain,
                                                const char *skip_kind,
                                                struct blob_autoload_fat_txn *txns,
                                                size_t *txn_count_out,
                                                int drop_stale,
                                                int fat_mounted)
{
    size_t txn_count = 0;

    if (!entries || !txns || !txn_count_out) return -1;
    *txn_count_out = 0;

    for (size_t i = 0; i < count; i++) {
        char managed_path[VFS_MAX_PATH];
        uint32_t managed_size = 0;
        uint32_t managed_checksum = 0;

        if (!entries[i].present) continue;
        if (skip_domain && skip_kind &&
            strcmp(entries[i].domain, skip_domain) == 0 &&
            strcmp(entries[i].kind, skip_kind) == 0) {
            continue;
        }
        if (blob_autoload_is_fat_path(entries[i].path)) {
            if (drop_stale &&
                !(fat_mounted ? blob_autoload_entry_is_usable_with_fat_mounted(entries + i)
                              : blob_autoload_entry_is_usable(entries + i))) {
                entries[i].path[0] = '\0';
                entries[i].size_bytes = 0;
                entries[i].checksum = 0;
                entries[i].present = 0;
            }
            continue;
        }

        if (blob_autoload_stage_managed_replacement_fat(entries[i].path,
                                                        entries[i].domain,
                                                        entries[i].kind,
                                                        &txns[txn_count],
                                                        managed_path,
                                                        sizeof(managed_path),
                                                        &managed_size,
                                                        &managed_checksum) != 0) {
            if (drop_stale) {
                entries[i].path[0] = '\0';
                entries[i].size_bytes = 0;
                entries[i].checksum = 0;
                entries[i].present = 0;
                continue;
            }
            blob_autoload_rollback_fat_txn_array(txns, txn_count);
            return -1;
        }
        strncpy(entries[i].path, managed_path, sizeof(entries[i].path) - 1);
        entries[i].path[sizeof(entries[i].path) - 1] = '\0';
        entries[i].size_bytes = managed_size;
        entries[i].checksum = managed_checksum;
        txn_count++;
    }

    *txn_count_out = txn_count;
    return 0;
}

static char *trim_ascii(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

static int blob_autoload_parse_entries(struct blob_autoload_entry *entries,
                                       size_t count,
                                       char *buf,
                                       int bytes)
{
    if (bytes <= 0) {
        blob_entries_reset(entries, count);
        return 0;
    }

    blob_entries_reset(entries, count);
    buf[bytes] = '\0';

    char *line = buf;
    while (*line) {
        char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        char saved = *eol;
        *eol = '\0';
        line = trim_ascii(line);

        if (*line != '\0' && *line != '#') {
            char *domain = line;
            char *kind = line;
            char *path = line;

            while (*kind && *kind != ' ' && *kind != '\t') kind++;
            if (*kind) {
                *kind++ = '\0';
                while (*kind == ' ' || *kind == '\t') kind++;
                path = kind;
                while (*path && *path != ' ' && *path != '\t') path++;
                if (*path) {
                    *path++ = '\0';
                    while (*path == ' ' || *path == '\t') path++;
                    path = trim_ascii(path);
                    if (*path != '\0') {
                        for (size_t i = 0; i < count; i++) {
                            uint32_t parsed_size = 0;
                            uint32_t parsed_checksum = 0;
                            int malformed_metadata = 0;
                            char *path_end = path + strlen(path);
                            char *size_text = NULL;
                            char *checksum_text = NULL;

                            while (path_end > path &&
                                   path_end[-1] != ' ' &&
                                   path_end[-1] != '\t') {
                                path_end--;
                            }
                            if (path_end > path) {
                                char *size_sep = path_end - 1;
                                while (size_sep > path &&
                                       size_sep[-1] != ' ' &&
                                       size_sep[-1] != '\t') {
                                    size_sep--;
                                }
                                if (size_sep <= path) {
                                    malformed_metadata = 1;
                                } else {
                                    checksum_text = path_end;
                                    size_text = size_sep;
                                    checksum_text[-1] = '\0';
                                    size_text[-1] = '\0';
                                    if (*size_text == '\0' ||
                                        *checksum_text == '\0' ||
                                        blob_autoload_parse_u32(size_text, 10, &parsed_size) != 0 ||
                                        blob_autoload_parse_u32(checksum_text, 16, &parsed_checksum) != 0) {
                                        malformed_metadata = 1;
                                    }
                                }
                            }
                            if (strcmp(entries[i].domain, domain) == 0 &&
                                strcmp(entries[i].kind, kind) == 0) {
                                if (path_end == path) {
                                    strncpy(entries[i].path, path, sizeof(entries[i].path) - 1);
                                    entries[i].path[sizeof(entries[i].path) - 1] = '\0';
                                    entries[i].size_bytes = 0;
                                    entries[i].checksum = 0;
                                    entries[i].present = 1;
                                } else if (!malformed_metadata) {
                                    strncpy(entries[i].path, path, sizeof(entries[i].path) - 1);
                                    entries[i].path[sizeof(entries[i].path) - 1] = '\0';
                                    entries[i].size_bytes = parsed_size;
                                    entries[i].checksum = parsed_checksum;
                                    entries[i].present = 1;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        *eol = saved;
        line = eol;
        while (*line == '\n' || *line == '\r') line++;
    }

    return 0;
}

static int blob_autoload_read_entries_lfs_only(struct blob_autoload_entry *entries, size_t count)
{
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    int bytes = vfs_read_path(BLOB_AUTOLOAD_CONF_PATH, buf, sizeof(buf) - 1, 0);
    if (bytes <= 0) {
        bytes = vfs_read_path(BLOB_AUTOLOAD_CONF_BAK_VFS_PATH, buf, sizeof(buf) - 1, 0);
    }
    return blob_autoload_parse_entries(entries, count, buf, bytes);
}

static int blob_autoload_read_entries_fat_mounted(struct blob_autoload_entry *entries, size_t count)
{
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    FIL fp;
    FRESULT res;
    int bytes = -1;

    res = f_open(&fp, BLOB_AUTOLOAD_CONF_FAT_PATH, FA_READ);
    if (res == FR_OK) {
        UINT bytes_read = 0;
        res = f_read(&fp, buf, sizeof(buf) - 1, &bytes_read);
        (void)f_close(&fp);
        if (res == FR_OK && bytes_read > 0) {
            bytes = (int)bytes_read;
        }
    }
    if (bytes <= 0) {
        res = f_open(&fp, BLOB_AUTOLOAD_CONF_FAT_BAK_PATH, FA_READ);
        if (res == FR_OK) {
            UINT bytes_read = 0;
            res = f_read(&fp, buf, sizeof(buf) - 1, &bytes_read);
            (void)f_close(&fp);
            if (res == FR_OK && bytes_read > 0) {
                bytes = (int)bytes_read;
            }
        }
    }
    return blob_autoload_parse_entries(entries, count, buf, bytes);
}

static int blob_autoload_read_entries(struct blob_autoload_entry *entries, size_t count)
{
    struct blob_autoload_fat_store store;
    int fat_authoritative = blob_autoload_has_fat_authority_marker();
    struct blob_autoload_entry fat_entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    int fat_rc = -1;

    if (blob_autoload_fat_mount(&store) == 0) {
        blob_autoload_seed_entries(fat_entries, count);
        fat_rc = blob_autoload_read_entries_fat_mounted(fat_entries, count);
        blob_autoload_fat_unmount(&store);
        if (fat_authoritative) {
            memcpy(entries, fat_entries, count * sizeof(entries[0]));
            return fat_rc;
        }
    }
    if (fat_authoritative) {
        blob_entries_reset(entries, count);
        return 0;
    }
    blob_autoload_seed_entries(entries, count);
    if (blob_autoload_read_entries_lfs_only(entries, count) == 0 &&
        blob_autoload_any_present(entries, count)) {
        return 0;
    }
    if (fat_rc == 0 && blob_autoload_any_present(fat_entries, count)) {
        memcpy(entries, fat_entries, count * sizeof(entries[0]));
        return 0;
    }
    blob_entries_reset(entries, count);
    return 0;
}

static int blob_autoload_write_entries(const struct blob_autoload_entry *entries, size_t count)
{
    static const char header[] =
        "# Runtime blob autoload config\n"
        "# Format: <domain> <kind> <absolute-path> <size-bytes> <checksum-hex>\n"
        "# Entries listed here are staged and activated at boot.\n";
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    struct vfs_entry_info info;
    int fd;
    int written;
    size_t pos = 0;

    if (!mnt) return -1;
    if (g_blob_autoload_test_fail_next_write > 0) {
        g_blob_autoload_test_fail_next_write--;
        return -1;
    }
    if (sizeof(header) - 1 >= sizeof(buf)) return -1;
    memcpy(buf, header, sizeof(header) - 1);
    pos = sizeof(header) - 1;

    for (size_t i = 0; i < count; i++) {
        int n;
        if (!entries[i].present) continue;
        n = uart_snprintf(buf + pos, sizeof(buf) - pos, "%s %s %s %u %08x\n",
                          entries[i].domain, entries[i].kind, entries[i].path,
                          entries[i].size_bytes, entries[i].checksum);
        if (n <= 0 || (size_t)n >= sizeof(buf) - pos) {
            return -1;
        }
        pos += (size_t)n;
    }

    fd = littlefs_file_open(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH,
                            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;
    written = littlefs_file_write(mnt, fd, buf, pos);
    littlefs_file_close(mnt, fd);
    if (written != (int)pos) {
        littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH);
        return -1;
    }
    if (littlefs_rename(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH, "/blob_autoload.conf") != 0) {
        int had_existing = (vfs_stat_path(BLOB_AUTOLOAD_CONF_PATH, &info) == 0);
        if (!had_existing) {
            littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH);
            return -1;
        }
        (void)littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_BAK_LFS_PATH);
        if (littlefs_rename(mnt, "/blob_autoload.conf", BLOB_AUTOLOAD_CONF_BAK_LFS_PATH) != 0) {
            littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH);
            return -1;
        }
        if (littlefs_rename(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH, "/blob_autoload.conf") != 0) {
            (void)littlefs_rename(mnt, BLOB_AUTOLOAD_CONF_BAK_LFS_PATH, "/blob_autoload.conf");
            littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_TMP_PATH);
            return -1;
        }
        (void)littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_BAK_LFS_PATH);
    }
    return 0;
}

static int blob_autoload_write_entries_fat_mounted(const struct blob_autoload_entry *entries, size_t count)
{
    static const char header[] =
        "# Runtime blob autoload config\n"
        "# Format: <domain> <kind> <absolute-path> <size-bytes> <checksum-hex>\n"
        "# Entries listed here are staged and activated at boot.\n";
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    size_t pos = 0;

    if (g_blob_autoload_test_fail_next_write > 0) {
        g_blob_autoload_test_fail_next_write--;
        return -1;
    }
    if (sizeof(header) - 1 >= sizeof(buf)) return -1;
    memcpy(buf, header, sizeof(header) - 1);
    pos = sizeof(header) - 1;

    for (size_t i = 0; i < count; i++) {
        int n;
        if (!entries[i].present) continue;
        n = uart_snprintf(buf + pos, sizeof(buf) - pos, "%s %s %s %u %08x\n",
                          entries[i].domain, entries[i].kind, entries[i].path,
                          entries[i].size_bytes, entries[i].checksum);
        if (n <= 0 || (size_t)n >= sizeof(buf) - pos) {
            return -1;
        }
        pos += (size_t)n;
    }

    {
        FIL fp;
        UINT written_u = 0;
        FRESULT res;

        if (blob_autoload_fat_ensure_dirs() != 0) {
            return -1;
        }
        res = f_open(&fp, BLOB_AUTOLOAD_CONF_FAT_TMP_PATH, FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            return -1;
        }
        res = f_write(&fp, buf, (UINT)pos, &written_u);
        (void)f_close(&fp);
        if (res != FR_OK || written_u != (UINT)pos) {
            (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH);
            return -1;
        }
        if (f_rename(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH, BLOB_AUTOLOAD_CONF_FAT_PATH) != FR_OK) {
            int had_existing = blob_autoload_fat_exists(BLOB_AUTOLOAD_CONF_FAT_PATH);
            if (!had_existing) {
                (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH);
                return -1;
            }
            (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_BAK_PATH);
            if (f_rename(BLOB_AUTOLOAD_CONF_FAT_PATH, BLOB_AUTOLOAD_CONF_FAT_BAK_PATH) != FR_OK) {
                (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH);
                return -1;
            }
            if (f_rename(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH, BLOB_AUTOLOAD_CONF_FAT_PATH) != FR_OK) {
                (void)f_rename(BLOB_AUTOLOAD_CONF_FAT_BAK_PATH, BLOB_AUTOLOAD_CONF_FAT_PATH);
                (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_TMP_PATH);
                return -1;
            }
            (void)f_unlink(BLOB_AUTOLOAD_CONF_FAT_BAK_PATH);
        }
        return 0;
    }
}

static void blob_autoload_invalidate_lfs_fallback(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt =
        (struct lfs_mount *)vfs_get_mount_ctx(BLOB_AUTOLOAD_STORE_DIR, &subpath);
    if (!mnt) {
        return;
    }

    (void)littlefs_remove(mnt, "/blob_autoload.conf");
    (void)littlefs_remove(mnt, BLOB_AUTOLOAD_CONF_BAK_LFS_PATH);
}

static void blob_autoload_seed_entries(struct blob_autoload_entry *entries, size_t count)
{
    if (!entries) {
        return;
    }
    memcpy(entries, blob_entries, count * sizeof(entries[0]));
}

static int blob_autoload_load_sources_with_fat_mounted(struct blob_autoload_entry *fat_entries,
                                                       struct blob_autoload_entry *lfs_entries,
                                                       size_t count)
{
    blob_autoload_seed_entries(fat_entries, count);
    blob_autoload_seed_entries(lfs_entries, count);
    (void)blob_autoload_read_entries_fat_mounted(fat_entries, count);
    (void)blob_autoload_read_entries_lfs_only(lfs_entries, count);
    return 0;
}

static int blob_autoload_commit_fat_entries_mounted(const struct blob_autoload_entry *entries,
                                                    struct blob_autoload_fat_txn *txns,
                                                    size_t txn_count,
                                                    int mark_authoritative)
{
    if (blob_autoload_write_entries_fat_mounted(entries,
                                                sizeof(blob_entries) / sizeof(blob_entries[0])) != 0) {
        blob_autoload_rollback_fat_txn_array(txns, txn_count);
        return -1;
    }
    blob_autoload_commit_fat_txn_array(txns, txn_count);
    if (mark_authoritative) {
        blob_autoload_mark_fat_authoritative();
        blob_autoload_invalidate_lfs_fallback();
    }
    return 0;
}

static int blob_autoload_reconcile_existing_fat_mounted(int fat_marker)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    struct blob_autoload_entry fat_entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    struct blob_autoload_entry lfs_entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    struct blob_autoload_fat_txn fat_txns[BLOB_AUTOLOAD_ENTRY_COUNT];
    size_t fat_txn_count = 0;

    if (fat_marker) {
        return 0;
    }

    blob_autoload_load_sources_with_fat_mounted(fat_entries, lfs_entries,
                                                sizeof(entries) / sizeof(entries[0]));
    memcpy(entries, fat_entries, sizeof(entries));
    blob_autoload_reconcile_entries(entries, lfs_entries,
                                    sizeof(entries) / sizeof(entries[0]), 1);
    if (blob_autoload_migrate_entries_to_fat(entries,
                                             sizeof(entries) / sizeof(entries[0]),
                                             NULL, NULL,
                                             fat_txns, &fat_txn_count,
                                             1, 1) != 0) {
        blob_autoload_rollback_fat_txn_array(fat_txns, fat_txn_count);
        return -1;
    }
    return blob_autoload_commit_fat_entries_mounted(entries, fat_txns, fat_txn_count, 1);
}

static int blob_autoload_bootstrap_fat_authority(void)
{
    struct blob_autoload_fat_store store;
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    struct blob_autoload_fat_txn fat_txns[BLOB_AUTOLOAD_ENTRY_COUNT];
    size_t fat_txn_count = 0;

    blob_autoload_seed_entries(entries, sizeof(entries) / sizeof(entries[0]));
    (void)blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));

    if (blob_autoload_fat_mount(&store) != 0) {
        return -1;
    }
    (void)blob_autoload_fat_ensure_dirs();
    if (blob_autoload_any_present(entries, sizeof(entries) / sizeof(entries[0]))) {
        if (blob_autoload_migrate_entries_to_fat(entries,
                                                 sizeof(entries) / sizeof(entries[0]),
                                                 NULL, NULL,
                                                 fat_txns, &fat_txn_count,
                                                 1, 1) != 0) {
            blob_autoload_rollback_fat_txn_array(fat_txns, fat_txn_count);
            blob_autoload_fat_unmount(&store);
            return -1;
        }
        if (blob_autoload_commit_fat_entries_mounted(entries, fat_txns, fat_txn_count, 1) != 0) {
            blob_autoload_fat_unmount(&store);
            return -1;
        }
        blob_autoload_fat_unmount(&store);
        return 0;
    }
    if (blob_autoload_write_entries_fat_mounted(entries,
                                                sizeof(entries) / sizeof(entries[0])) == 0) {
        blob_autoload_fat_unmount(&store);
        blob_autoload_mark_fat_authoritative();
        return 0;
    }
    blob_autoload_fat_unmount(&store);
    return -1;
}

int blob_autoload_init(void)
{
    struct vfs_entry_info info;
    struct blob_autoload_fat_store store;
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(BLOB_AUTOLOAD_STORE_DIR, &subpath);
    int fat_marker = blob_autoload_has_fat_authority_marker();

    if (blob_autoload_fat_mount(&store) == 0) {
        (void)blob_autoload_fat_ensure_dirs();
        if (blob_autoload_fat_exists(BLOB_AUTOLOAD_CONF_FAT_PATH)) {
            (void)blob_autoload_reconcile_existing_fat_mounted(fat_marker);
            blob_autoload_fat_unmount(&store);
            if (blob_autoload_has_fat_authority_marker()) {
                blob_autoload_invalidate_lfs_fallback();
            }
            return 0;
        }
        if (blob_autoload_fat_exists(BLOB_AUTOLOAD_CONF_FAT_BAK_PATH)) {
            if (f_rename(BLOB_AUTOLOAD_CONF_FAT_BAK_PATH, BLOB_AUTOLOAD_CONF_FAT_PATH) == FR_OK) {
                (void)blob_autoload_reconcile_existing_fat_mounted(fat_marker);
                blob_autoload_fat_unmount(&store);
                if (blob_autoload_has_fat_authority_marker()) {
                    blob_autoload_invalidate_lfs_fallback();
                }
                return 0;
            }
            blob_autoload_fat_unmount(&store);
            return 0;
        }
        blob_autoload_fat_unmount(&store);
        (void)blob_autoload_bootstrap_fat_authority();
    }

    if (mnt) {
        (void)blob_autoload_ensure_store_dir(mnt);
    }
    if (vfs_stat_path(BLOB_AUTOLOAD_CONF_PATH, &info) == 0) {
        return 0;
    }
    if (vfs_stat_path(BLOB_AUTOLOAD_CONF_BAK_VFS_PATH, &info) == 0) {
        if (mnt && littlefs_rename(mnt, BLOB_AUTOLOAD_CONF_BAK_LFS_PATH, "/blob_autoload.conf") == 0) {
            return 0;
        }
        return 0;
    }
    blob_entries_reset(blob_entries, sizeof(blob_entries) / sizeof(blob_entries[0]));
    return blob_autoload_write_entries(blob_entries, sizeof(blob_entries) / sizeof(blob_entries[0]));
}

int blob_autoload_get(const char *domain, const char *kind,
                      char *path_out, size_t path_out_cap)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            if (!entries[i].present) return 1;
            if (path_out && path_out_cap > 0) {
                strncpy(path_out, entries[i].path, path_out_cap - 1);
                path_out[path_out_cap - 1] = '\0';
            }
            return 0;
        }
    }
    return -1;
}

int blob_autoload_info_get(const char *domain, const char *kind,
                           struct blob_autoload_info *out)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            if (!entries[i].present) return 1;
            strncpy(out->path, entries[i].path, sizeof(out->path) - 1);
            out->path[sizeof(out->path) - 1] = '\0';
            out->size_bytes = entries[i].size_bytes;
            out->checksum = entries[i].checksum;
            out->present = 1;
            return 0;
        }
    }
    return -1;
}

static int blob_autoload_mutate_entry(const char *domain,
                                      const char *kind,
                                      const char *resolved_path,
                                      int clear_entry)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    char managed_path[VFS_MAX_PATH];
    struct blob_autoload_managed_txn txn;
    struct blob_autoload_fat_store fat_store;
    struct blob_autoload_fat_txn fat_txn;
    struct blob_autoload_fat_txn migrated_txns[BLOB_AUTOLOAD_ENTRY_COUNT];
    size_t migrated_txn_count = 0;
    struct blob_autoload_entry *entry;
    uint32_t managed_size = 0;
    uint32_t managed_checksum = 0;
    int rc;
    int use_fat = 0;
    int fat_authoritative = blob_autoload_has_fat_authority_marker();

    blob_autoload_seed_entries(entries, sizeof(entries) / sizeof(entries[0]));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    entry = blob_find_entry(entries, sizeof(entries) / sizeof(entries[0]), domain, kind);
    if (!entry) {
        return -1;
    }

    if (blob_autoload_fat_mount(&fat_store) == 0) {
        if (blob_autoload_migrate_entries_to_fat(entries,
                                                 sizeof(entries) / sizeof(entries[0]),
                                                 domain, kind,
                                                 migrated_txns, &migrated_txn_count,
                                                 1, 1) != 0) {
            blob_autoload_fat_unmount(&fat_store);
            return -1;
        }
        rc = clear_entry
            ? blob_autoload_stage_managed_clear_fat(domain, kind, &fat_txn)
            : blob_autoload_stage_managed_replacement_fat(resolved_path, domain, kind,
                                                          &fat_txn,
                                                          managed_path, sizeof(managed_path),
                                                          &managed_size, &managed_checksum);
        if (rc != 0) {
            blob_autoload_rollback_fat_txn_array(migrated_txns, migrated_txn_count);
            blob_autoload_fat_unmount(&fat_store);
            return -1;
        }
        use_fat = 1;
    } else {
        if (fat_authoritative) {
            return -1;
        }
        rc = clear_entry
            ? blob_autoload_stage_managed_clear(domain, kind, &txn)
            : blob_autoload_stage_managed_replacement(resolved_path, domain, kind,
                                                      &txn,
                                                      managed_path, sizeof(managed_path),
                                                      &managed_size, &managed_checksum);
        if (rc != 0) {
            return -1;
        }
    }

    if (clear_entry) {
        entry->path[0] = '\0';
        entry->size_bytes = 0;
        entry->checksum = 0;
        entry->present = 0;
    } else {
        strncpy(entry->path, managed_path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
        entry->size_bytes = managed_size;
        entry->checksum = managed_checksum;
        entry->present = 1;
    }

    rc = use_fat
        ? blob_autoload_write_entries_fat_mounted(entries, sizeof(entries) / sizeof(entries[0]))
        : blob_autoload_write_entries(entries, sizeof(entries) / sizeof(entries[0]));
    if (rc != 0) {
        if (use_fat) {
            blob_autoload_rollback_managed_fat_txn(&fat_txn);
            blob_autoload_rollback_fat_txn_array(migrated_txns, migrated_txn_count);
            blob_autoload_fat_unmount(&fat_store);
        } else {
            blob_autoload_rollback_managed_txn(&txn);
        }
        return rc;
    }
    if (use_fat) {
        blob_autoload_commit_managed_fat_txn(&fat_txn);
        blob_autoload_commit_fat_txn_array(migrated_txns, migrated_txn_count);
        blob_autoload_fat_unmount(&fat_store);
        blob_autoload_mark_fat_authoritative();
        blob_autoload_invalidate_lfs_fallback();
    } else {
        blob_autoload_commit_managed_txn(&txn);
    }
    return 0;
}

int blob_autoload_set(const char *domain, const char *kind, const char *path)
{
    char resolved[VFS_MAX_PATH];
    int rc;

    if (blob_autoload_is_fat_path(path)) {
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }

    if (strcmp(domain, "eviction") == 0) {
        int kind_id = 0;
        if (strcmp(kind, "xgboost") == 0) kind_id = 1;
        else if (strcmp(kind, "mlp") == 0) kind_id = 2;
        else if (strcmp(kind, "cacheus_config") == 0) kind_id = 3;
        if (kind_id == 0) return -1;
        rc = eviction_blob_validate_file((uint16_t)kind_id, path, resolved, sizeof(resolved));
    } else if (strcmp(domain, "sched") == 0) {
        uint16_t kind_id = 0;
        if (strcmp(kind, "mlp") == 0) kind_id = SCHED_MODEL_KIND_MLP;
        else if (strcmp(kind, "ppo") == 0) kind_id = SCHED_MODEL_KIND_PPO;
        else if (strcmp(kind, "config") == 0) kind_id = SCHED_MODEL_KIND_CONFIG;
        else if (strcmp(kind, "thresholds") == 0) kind_id = SCHED_MODEL_KIND_THRESHOLDS;
        else if (strcmp(kind, "rebalance") == 0) kind_id = SCHED_MODEL_KIND_REBALANCE;
        if (kind_id == 0) return -1;
        rc = sched_blob_validate_file(kind_id, path, resolved, sizeof(resolved));
    } else {
        return -1;
    }
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }
    return blob_autoload_mutate_entry(domain, kind, resolved, 0);
}

int blob_autoload_clear(const char *domain, const char *kind)
{
    return blob_autoload_mutate_entry(domain, kind, NULL, 1);
}

void blob_autoload_test_fail_next_write(void)
{
    g_blob_autoload_test_fail_next_write++;
}

void blob_autoload_test_fail_next_fat_mount(void)
{
    g_blob_autoload_test_fail_next_fat_mount++;
}

void blob_autoload_test_fail_next_fat_mounts(unsigned count)
{
    g_blob_autoload_test_fail_next_fat_mount += (int)count;
}

void blob_boot_autoload(void)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    char resolved[VFS_MAX_PATH];

    memcpy(entries, blob_entries, sizeof(entries));
    if (blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0])) != 0) {
        return;
    }

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        int rc;
        uint32_t actual_size = 0;
        uint32_t actual_checksum = 0;
        if (!entries[i].present) continue;
        resolved[0] = '\0';
        if (entries[i].size_bytes != 0 || entries[i].checksum != 0) {
            if (blob_autoload_file_identity(entries[i].path, &actual_size, &actual_checksum) != 0) {
                uart_printf("[WARN] blob_autoload: identity check failed %s %s path=%s\r\n",
                            entries[i].domain, entries[i].kind, entries[i].path);
                continue;
            }
            if (actual_size != entries[i].size_bytes ||
                actual_checksum != entries[i].checksum) {
                uart_printf("[WARN] blob_autoload: identity mismatch %s %s path=%s expected=%u/%08x actual=%u/%08x\r\n",
                            entries[i].domain, entries[i].kind, entries[i].path,
                            entries[i].size_bytes, entries[i].checksum,
                            actual_size, actual_checksum);
                continue;
            }
        }
        if (strcmp(entries[i].domain, "eviction") == 0) {
            rc = eviction_blob_stage_file(entries[i].kind_id, entries[i].path,
                                          resolved, sizeof(resolved));
            if (rc == 0) {
                rc = rust_eviction_blob_activate(entries[i].kind_id);
            }
        } else {
#ifdef CONFIG_AI_SCHEDULER
            rc = sched_blob_stage_file(entries[i].kind_id, entries[i].path,
                                       resolved, sizeof(resolved));
            if (rc == 0) {
                rc = sched_model_activate(entries[i].kind_id);
            }
#else
            rc = RUNTIME_BLOB_FILE_STAGE_FAILED;
#endif
        }
        if (rc == 0) {
            uart_printf("[INFO] blob_autoload: activated %s %s from %s\r\n",
                        entries[i].domain, entries[i].kind,
                        resolved[0] ? resolved : entries[i].path);
        } else {
            uart_printf("[WARN] blob_autoload: failed %s %s rc=%d path=%s\r\n",
                        entries[i].domain, entries[i].kind, rc, entries[i].path);
        }
    }
}
