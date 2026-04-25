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
    int present;
};

#define BLOB_AUTOLOAD_CONF_TMP_PATH "/blob_autoload.conf.tmp"
#define BLOB_AUTOLOAD_CONF_BAK_LFS_PATH "/blob_autoload.conf.bak"
#define BLOB_AUTOLOAD_CONF_BAK_VFS_PATH "/mnt/files/blob_autoload.conf.bak"
#define BLOB_AUTOLOAD_ENTRY_LINE_OVERHEAD 48u

static struct blob_autoload_entry blob_entries[] = {
    {"eviction", "xgboost", 1, {0}, 0},
    {"eviction", "mlp", 2, {0}, 0},
    {"eviction", "cacheus_config", 3, {0}, 0},
    {"sched", "mlp", SCHED_MODEL_KIND_MLP, {0}, 0},
    {"sched", "ppo", SCHED_MODEL_KIND_PPO, {0}, 0},
    {"sched", "config", SCHED_MODEL_KIND_CONFIG, {0}, 0},
};

enum {
    BLOB_AUTOLOAD_ENTRY_COUNT =
        (int)(sizeof(blob_entries) / sizeof(blob_entries[0])),
    BLOB_AUTOLOAD_CONF_BUF_SIZE =
        (int)(sizeof(
            "# Runtime blob autoload config\n"
            "# Format: <domain> <kind> <absolute-path>\n"
            "# Entries listed here are staged and activated at boot.\n")) +
        (int)(sizeof(blob_entries) / sizeof(blob_entries[0])) *
            (int)(VFS_MAX_PATH + BLOB_AUTOLOAD_ENTRY_LINE_OVERHEAD)
};

static void blob_entries_reset(struct blob_autoload_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        entries[i].path[0] = '\0';
        entries[i].present = 0;
    }
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
                            if (strcmp(entries[i].domain, domain) == 0 &&
                                strcmp(entries[i].kind, kind) == 0) {
                                strncpy(entries[i].path, path, sizeof(entries[i].path) - 1);
                                entries[i].path[sizeof(entries[i].path) - 1] = '\0';
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
        "# Format: <domain> <kind> <absolute-path>\n"
        "# Entries listed here are staged and activated at boot.\n";
    static char buf[BLOB_AUTOLOAD_CONF_BUF_SIZE];
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    struct vfs_entry_info info;
    int fd;
    int written;
    size_t pos = 0;

    if (!mnt) return -1;
    if (sizeof(header) - 1 >= sizeof(buf)) return -1;
    memcpy(buf, header, sizeof(header) - 1);
    pos = sizeof(header) - 1;

    for (size_t i = 0; i < count; i++) {
        int n;
        if (!entries[i].present) continue;
        n = uart_snprintf(buf + pos, sizeof(buf) - pos, "%s %s %s\n",
                          entries[i].domain, entries[i].kind, entries[i].path);
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
    if (vfs_stat_path(BLOB_AUTOLOAD_CONF_PATH, &info) == 0) {
        return 0;
    }
    if (vfs_stat_path(BLOB_AUTOLOAD_CONF_BAK_VFS_PATH, &info) == 0) {
        const char *subpath = NULL;
        struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
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

int blob_autoload_set(const char *domain, const char *kind, const char *path)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    char resolved[VFS_MAX_PATH];
    struct blob_autoload_entry *entry;
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
        if (kind_id == 0) return -1;
        rc = sched_blob_validate_file(kind_id, path, resolved, sizeof(resolved));
    } else {
        return -1;
    }
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    entry = NULL;
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            entry = &entries[i];
            break;
        }
    }
    if (!entry) return -1;
    strncpy(entry->path, resolved, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->present = 1;
    return blob_autoload_write_entries(entries, sizeof(entries) / sizeof(entries[0]));
}

int blob_autoload_clear(const char *domain, const char *kind)
{
    struct blob_autoload_entry entries[sizeof(blob_entries) / sizeof(blob_entries[0])];
    memcpy(entries, blob_entries, sizeof(entries));
    blob_autoload_read_entries(entries, sizeof(entries) / sizeof(entries[0]));
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (strcmp(entries[i].domain, domain) == 0 &&
            strcmp(entries[i].kind, kind) == 0) {
            entries[i].path[0] = '\0';
            entries[i].present = 0;
            return blob_autoload_write_entries(entries, sizeof(entries) / sizeof(entries[0]));
        }
    }
    return -1;
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
        if (!entries[i].present) continue;
        resolved[0] = '\0';
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
