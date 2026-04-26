#ifndef BLOB_AUTOLOAD_H
#define BLOB_AUTOLOAD_H

#include <stddef.h>
#include <stdint.h>
#include "vfs.h"

#define BLOB_AUTOLOAD_CONF_PATH "/mnt/files/blob_autoload.conf"
#define BLOB_AUTOLOAD_STORE_DIR "/mnt/files/autoload"

struct blob_autoload_info {
    char path[VFS_MAX_PATH];
    uint32_t size_bytes;
    uint32_t checksum;
    int present;
};

int blob_autoload_init(void);
void blob_boot_autoload(void);
int blob_autoload_get(const char *domain, const char *kind,
                      char *path_out, size_t path_out_cap);
int blob_autoload_info_get(const char *domain, const char *kind,
                           struct blob_autoload_info *out);
int blob_autoload_set(const char *domain, const char *kind, const char *path);
int blob_autoload_clear(const char *domain, const char *kind);

#endif /* BLOB_AUTOLOAD_H */
