#ifndef NET_HTTP_H
#define NET_HTTP_H

#include <stdint.h>
#include "sha256.h"

#define NET_HTTP_MAX_HOST 96
#define NET_HTTP_MAX_URI  1024

struct net_http_url {
    char host[NET_HTTP_MAX_HOST];
    uint16_t port;
    char uri[NET_HTTP_MAX_URI];
};

struct net_http_get_result {
    uint32_t http_status;
    uint32_t bytes_received;
    uint32_t content_length;
    int httpc_result;
    int lwip_err;
    uint8_t hash_checked;
    uint8_t hash_verified;
    char sha256_hex[SHA256_HEX_LEN + 1u];
};

/*
 * Parse a minimal HTTP URL of the form:
 *   http://host/path
 *   http://host:port/path
 *   http://host
 *
 * Returns 0 on success, negative on invalid/unsupported input.
 */
int net_http_parse_url(const char *url, struct net_http_url *out);
int net_http_parse_sha256_hex(const char *hex,
                              uint8_t out[SHA256_DIGEST_LEN]);

/*
 * Download one HTTP resource into an already-resolved filesystem path.
 *
 * The destination path must be an absolute mounted-filesystem path.
 * Returns 0 on success, negative on parse/network/filesystem error.
 */
int net_http_get_file(const char *url, const char *dest_path,
                      const char *expected_sha256_hex,
                      struct net_http_get_result *out);

#endif /* NET_HTTP_H */
