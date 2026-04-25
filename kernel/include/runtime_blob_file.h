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

#endif /* RUNTIME_BLOB_FILE_H */
