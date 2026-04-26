#include "blob_autoload.h"

#include "littlefs_slm.h"
#include "runtime_blob_file.h"
#include "runtime_model.h"
#include "slm_ffi.h"
#include "shell_internal.h"
#include "uart.h"
#include "vfs.h"
#include "string.h"

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
#define BLOB_AUTOLOAD_ENTRY_LINE_OVERHEAD 80u

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

static int g_blob_autoload_test_fail_next_write = 0;

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

static int blob_autoload_file_identity(const char *path,
                                       uint32_t *size_out,
                                       uint32_t *checksum_out)
{
    struct vfs_entry_info info;
    char buf[256];
    uint32_t checksum = 0x811C9DC5u;
    int offset = 0;

    if (!path || !size_out || !checksum_out) return -1;
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

static int blob_autoload_read_entries(struct blob_autoload_entry *entries, size_t count)
{
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    int bytes = vfs_read_path(BLOB_AUTOLOAD_CONF_PATH, buf, sizeof(buf) - 1, 0);
    if (bytes <= 0) {
        bytes = vfs_read_path(BLOB_AUTOLOAD_CONF_BAK_VFS_PATH, buf, sizeof(buf) - 1, 0);
    }
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
                            char *size_text = path + strlen(path);
                            char *checksum_text = NULL;

                            while (size_text > path &&
                                   size_text[-1] != ' ' &&
                                   size_text[-1] != '\t') {
                                size_text--;
                            }
                            if (size_text > path) {
                                char *size_sep = size_text - 1;
                                while (size_sep > path &&
                                       size_sep[-1] != ' ' &&
                                       size_sep[-1] != '\t') {
                                    size_sep--;
                                }
                                if (size_sep > path) {
                                    checksum_text = size_text;
                                    size_text = size_sep;
                                    checksum_text[-1] = '\0';
                                    size_text[-1] = '\0';
                                    if (blob_autoload_parse_u32(size_text, 10, &parsed_size) != 0 ||
                                        blob_autoload_parse_u32(checksum_text, 16, &parsed_checksum) != 0) {
                                        parsed_size = 0;
                                        parsed_checksum = 0;
                                    }
                                }
                            }
                            if (strcmp(entries[i].domain, domain) == 0 &&
                                strcmp(entries[i].kind, kind) == 0) {
                                strncpy(entries[i].path, path, sizeof(entries[i].path) - 1);
                                entries[i].path[sizeof(entries[i].path) - 1] = '\0';
                                entries[i].size_bytes = parsed_size;
                                entries[i].checksum = parsed_checksum;
                                entries[i].present = 1;
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

int blob_autoload_init(void)
{
    struct vfs_entry_info info;
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(BLOB_AUTOLOAD_STORE_DIR, &subpath);

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

int blob_autoload_set(const char *domain, const char *kind, const char *path)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    char resolved[VFS_MAX_PATH];
    char managed_path[VFS_MAX_PATH];
    struct blob_autoload_managed_txn txn;
    struct blob_autoload_entry *entry;
    uint32_t managed_size = 0;
    uint32_t managed_checksum = 0;
    int rc;

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
    if (blob_autoload_stage_managed_replacement(resolved, domain, kind,
                                                &txn,
                                                managed_path, sizeof(managed_path),
                                                &managed_size, &managed_checksum) != 0) {
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    entry = blob_find_entry(entries, sizeof(entries) / sizeof(entries[0]), domain, kind);
    if (!entry) {
        blob_autoload_rollback_managed_txn(&txn);
        return -1;
    }
    strncpy(entry->path, managed_path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->size_bytes = managed_size;
    entry->checksum = managed_checksum;
    entry->present = 1;
    rc = blob_autoload_write_entries(entries, sizeof(entries) / sizeof(entries[0]));
    if (rc != 0) {
        blob_autoload_rollback_managed_txn(&txn);
        return rc;
    }
    blob_autoload_commit_managed_txn(&txn);
    return 0;
}

int blob_autoload_clear(const char *domain, const char *kind)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    struct blob_autoload_managed_txn txn;
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            if (blob_autoload_stage_managed_clear(domain, kind, &txn) != 0) {
                return -1;
            }
            entries[i].path[0] = '\0';
            entries[i].size_bytes = 0;
            entries[i].checksum = 0;
            entries[i].present = 0;
            if (blob_autoload_write_entries(entries, sizeof(entries) / sizeof(entries[0])) != 0) {
                blob_autoload_rollback_managed_txn(&txn);
                return -1;
            }
            blob_autoload_commit_managed_txn(&txn);
            return 0;
        }
    }
    return -1;
}

void blob_autoload_test_fail_next_write(void)
{
    g_blob_autoload_test_fail_next_write++;
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
