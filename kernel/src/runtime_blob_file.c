#include "runtime_blob_file.h"

#include "shell_internal.h"
#include "slm_ffi.h"
#ifdef CONFIG_AI_SCHEDULER
#include "runtime_model.h"
#endif
#include "vfs.h"
#include "pmm.h"
#include "string.h"

static int runtime_blob_read_file(const char *path,
                                  char *resolved_out,
                                  size_t resolved_out_cap,
                                  uint8_t **buf_out,
                                  size_t *buf_len_out,
                                  size_t *pages_out)
{
    struct vfs_entry_info info;
    char resolved[VFS_MAX_PATH];
    uint8_t *buf;
    size_t pages_needed;
    int bytes_read;

    if (!path || !buf_out || !buf_len_out || !pages_out) {
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }

    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return RUNTIME_BLOB_FILE_PATH_TOO_LONG;
    }

    if (vfs_stat_path(resolved, &info) != 0) {
        return RUNTIME_BLOB_FILE_NOT_FOUND;
    }
    if (info.type != 0) {
        return RUNTIME_BLOB_FILE_NOT_A_FILE;
    }
    if (info.size == 0) {
        return RUNTIME_BLOB_FILE_EMPTY;
    }

    pages_needed = (info.size + 4095u) / 4096u;
    buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        return RUNTIME_BLOB_FILE_NOMEM;
    }

    bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        return RUNTIME_BLOB_FILE_READ_FAILED;
    }

    if (resolved_out && resolved_out_cap > 0) {
        strncpy(resolved_out, resolved, resolved_out_cap - 1);
        resolved_out[resolved_out_cap - 1] = '\0';
    }
    *buf_out = buf;
    *buf_len_out = (size_t)bytes_read;
    *pages_out = pages_needed;
    return RUNTIME_BLOB_FILE_OK;
}

int sched_blob_stage_file(uint16_t kind_id, const char *path,
                          char *resolved_out, size_t resolved_out_cap)
{
#ifndef CONFIG_AI_SCHEDULER
    (void)kind_id;
    (void)path;
    (void)resolved_out;
    (void)resolved_out_cap;
    return RUNTIME_BLOB_FILE_STAGE_FAILED;
#else
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    size_t pages = 0;
    int rc = runtime_blob_read_file(path, resolved_out, resolved_out_cap,
                                    &buf, &buf_len, &pages);
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }

    if (sched_model_stage_blob(kind_id, buf, buf_len) != 0) {
        pmm_free_pages(buf, pages);
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }

    pmm_free_pages(buf, pages);
    return RUNTIME_BLOB_FILE_OK;
#endif
}

int sched_blob_validate_file(uint16_t kind_id, const char *path,
                             char *resolved_out, size_t resolved_out_cap)
{
#ifndef CONFIG_AI_SCHEDULER
    (void)kind_id;
    (void)path;
    (void)resolved_out;
    (void)resolved_out_cap;
    return RUNTIME_BLOB_FILE_STAGE_FAILED;
#else
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    size_t pages = 0;
    int rc = runtime_blob_read_file(path, resolved_out, resolved_out_cap,
                                    &buf, &buf_len, &pages);
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }

    rc = sched_model_validate_blob(kind_id, buf, buf_len);
    pmm_free_pages(buf, pages);
    return rc == 0 ? RUNTIME_BLOB_FILE_OK : RUNTIME_BLOB_FILE_STAGE_FAILED;
#endif
}

int eviction_blob_stage_file(uint16_t kind_id, const char *path,
                             char *resolved_out, size_t resolved_out_cap)
{
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    size_t pages = 0;
    int rc = runtime_blob_read_file(path, resolved_out, resolved_out_cap,
                                    &buf, &buf_len, &pages);
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }

    rc = rust_eviction_blob_stage(kind_id, buf, buf_len);
    pmm_free_pages(buf, pages);

    if (rc == -4) {
        return RUNTIME_BLOB_FILE_KIND_MISMATCH;
    }
    if (rc != 0) {
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }
    return RUNTIME_BLOB_FILE_OK;
}

int eviction_blob_validate_file(uint16_t kind_id, const char *path,
                                char *resolved_out, size_t resolved_out_cap)
{
    uint8_t *buf = NULL;
    size_t buf_len = 0;
    size_t pages = 0;
    int rc = runtime_blob_read_file(path, resolved_out, resolved_out_cap,
                                    &buf, &buf_len, &pages);
    if (rc != RUNTIME_BLOB_FILE_OK) {
        return rc;
    }

    rc = rust_eviction_blob_validate(kind_id, buf, buf_len);
    pmm_free_pages(buf, pages);

    if (rc == -4) {
        return RUNTIME_BLOB_FILE_KIND_MISMATCH;
    }
    if (rc != 0) {
        return RUNTIME_BLOB_FILE_STAGE_FAILED;
    }
    return RUNTIME_BLOB_FILE_OK;
}
