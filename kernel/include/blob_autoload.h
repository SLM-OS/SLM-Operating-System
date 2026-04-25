#ifndef BLOB_AUTOLOAD_H
#define BLOB_AUTOLOAD_H

#include <stddef.h>

#define BLOB_AUTOLOAD_CONF_PATH "/mnt/files/blob_autoload.conf"

int blob_autoload_init(void);
void blob_boot_autoload(void);
int blob_autoload_get(const char *domain, const char *kind,
                      char *path_out, size_t path_out_cap);
int blob_autoload_set(const char *domain, const char *kind, const char *path);
int blob_autoload_clear(const char *domain, const char *kind);

#endif /* BLOB_AUTOLOAD_H */
