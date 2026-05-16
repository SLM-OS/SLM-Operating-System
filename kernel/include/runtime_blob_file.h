#ifndef RUNTIME_BLOB_FILE_H
#define RUNTIME_BLOB_FILE_H

#include <stddef.h>
#include <stdint.h>

enum runtime_blob_file_result {
    RUNTIME_BLOB_FILE_OK = 0,
    RUNTIME_BLOB_FILE_PATH_TOO_LONG = -1,
    RUNTIME_BLOB_FILE_NOT_FOUND = -2,
    RUNTIME_BLOB_FILE_NOT_A_FILE = -3,
    RUNTIME_BLOB_FILE_EMPTY = -4,
    RUNTIME_BLOB_FILE_NOMEM = -5,
    RUNTIME_BLOB_FILE_READ_FAILED = -6,
    RUNTIME_BLOB_FILE_STAGE_FAILED = -7,
    RUNTIME_BLOB_FILE_KIND_MISMATCH = -8,
};

int sched_blob_stage_file(uint16_t kind_id, const char *path,
                          char *resolved_out, size_t resolved_out_cap);
int eviction_blob_stage_file(uint16_t kind_id, const char *path,
                             char *resolved_out, size_t resolved_out_cap);
int sched_blob_validate_file(uint16_t kind_id, const char *path,
                             char *resolved_out, size_t resolved_out_cap);
int eviction_blob_validate_file(uint16_t kind_id, const char *path,
                                char *resolved_out, size_t resolved_out_cap);

/* True if `path` is a fatfs-rooted path (`0:/...`). Use this to route
 * reads through `runtime_blob_read_fat_buf` instead of the VFS, which
 * has no view into the boot FAT partition. */
int runtime_blob_is_fat_path(const char *path);

/* Read a file from the boot FAT partition into a PMM-page-aligned
 * buffer. Caller owns the buffer and must call `pmm_free_pages(*buf,
 * *pages)` to release it. `path` must satisfy
 * `runtime_blob_is_fat_path(path)`. Returns one of
 * `runtime_blob_file_result`. Extracted as a shared helper for
 * shell-side verbs (e.g. `bench xgb-equiv`, #904) that need corpus
 * files from FAT without depending on the LittleFS mount. */
int runtime_blob_read_fat_buf(const char *path,
                              uint8_t **buf_out,
                              size_t *buf_len_out,
                              size_t *pages_out);

#endif /* RUNTIME_BLOB_FILE_H */
