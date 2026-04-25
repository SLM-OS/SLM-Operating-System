#include "net_http.h"

#include "net.h"
#include "sha256.h"
#include "vfs.h"
#include "littlefs_slm.h"
#include "arch/sys_arch.h"

#include <limits.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif

#include "lwip/apps/http_client.h"
#include "lwip/altcp.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NET_HTTP_TMP_SUFFIX ".part"
#define NET_HTTP_OVERALL_TIMEOUT_MS 40000U
#define NET_HTTP_CONTENT_LEN_UNKNOWN 0xFFFFFFFFU

struct net_http_download {
    struct lfs_mount *mnt;
    int fd;
    char temp_subpath[VFS_MAX_PATH];
    char final_subpath[VFS_MAX_PATH];
    bool done;
    bool success;
    uint32_t content_length;
    uint8_t expected_sha256[SHA256_DIGEST_LEN];
    bool has_expected_sha256;
    struct sha256_ctx sha256;
    struct net_http_get_result result;
};

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_u16(const char *s, size_t len, uint16_t *out)
{
    uint32_t value = 0;

    if (!s || len == 0 || !out) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = (value * 10U) + (uint32_t)(s[i] - '0');
        if (value > 65535U) {
            return false;
        }
    }
    if (value == 0U) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

int net_http_parse_url(const char *url, struct net_http_url *out)
{
    static const char prefix[] = "http://";
    const char *host_start;
    const char *path_start;
    const char *host_end;
    const char *colon;
    size_t host_len;
    size_t path_len;

    if (!url || !out) {
        return NET_E_INVAL;
    }
    if (strncmp(url, prefix, sizeof(prefix) - 1) != 0) {
        return NET_E_INVAL;
    }

    host_start = url + (sizeof(prefix) - 1);
    if (*host_start == '\0') {
        return NET_E_INVAL;
    }

    path_start = strchr(host_start, '/');
    host_end = path_start ? path_start : (host_start + strlen(host_start));
    colon = NULL;
    for (const char *p = host_start; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    memset(out, 0, sizeof(*out));
    out->port = HTTP_DEFAULT_PORT;

    if (colon) {
        host_len = (size_t)(colon - host_start);
        if (host_len == 0 || host_len >= sizeof(out->host)) {
            return NET_E_INVAL;
        }
        memcpy(out->host, host_start, host_len);
        out->host[host_len] = '\0';
        if (!parse_u16(colon + 1, (size_t)(host_end - (colon + 1)), &out->port)) {
            return NET_E_INVAL;
        }
    } else {
        host_len = (size_t)(host_end - host_start);
        if (host_len == 0 || host_len >= sizeof(out->host)) {
            return NET_E_INVAL;
        }
        memcpy(out->host, host_start, host_len);
        out->host[host_len] = '\0';
    }

    if (path_start) {
        path_len = strlen(path_start);
        if (path_len == 0 || path_len >= sizeof(out->uri)) {
            return NET_E_INVAL;
        }
        memcpy(out->uri, path_start, path_len + 1U);
    } else {
        memcpy(out->uri, "/", 2U);
    }

    return NET_OK;
}

int net_http_parse_sha256_hex(const char *hex,
                              uint8_t out[SHA256_DIGEST_LEN])
{
    if (!hex || !out || strlen(hex) != SHA256_HEX_LEN) {
        return NET_E_INVAL;
    }
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        int hi = parse_hex_nibble(hex[i * 2u]);
        int lo = parse_hex_nibble(hex[(i * 2u) + 1u]);
        if (hi < 0 || lo < 0) {
            return NET_E_INVAL;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return NET_OK;
}

static int build_temp_path(const char *dest_path, char *temp_path, size_t temp_path_size)
{
    size_t dest_len;
    size_t suffix_len = sizeof(NET_HTTP_TMP_SUFFIX) - 1U;

    if (!dest_path || !temp_path || temp_path_size == 0U) {
        return NET_E_INVAL;
    }
    dest_len = strlen(dest_path);
    if ((dest_len + suffix_len + 1U) > temp_path_size) {
        return NET_E_INVAL;
    }
    memcpy(temp_path, dest_path, dest_len);
    memcpy(temp_path + dest_len, NET_HTTP_TMP_SUFFIX, suffix_len + 1U);
    return NET_OK;
}

static void http_result_cb(void *arg, httpc_result_t httpc_result,
                           u32_t rx_content_len, u32_t srv_res, err_t err)
{
    struct net_http_download *dl = arg;

    if (!dl) {
        return;
    }
    dl->done = true;
    dl->result.http_status = srv_res;
    dl->result.bytes_received = rx_content_len;
    dl->result.httpc_result = (int)httpc_result;
    dl->result.lwip_err = (int)err;
    dl->success = (httpc_result == HTTPC_RESULT_OK) &&
                  (srv_res >= 200U) && (srv_res < 300U);
}

static err_t http_headers_done_cb(httpc_state_t *connection, void *arg,
                                  struct pbuf *hdr, u16_t hdr_len, u32_t content_len)
{
    struct net_http_download *dl = arg;
    (void)connection;
    (void)hdr;
    (void)hdr_len;

    if (!dl) {
        return ERR_ARG;
    }
    dl->content_length = content_len;
    return ERR_OK;
}

static err_t http_recv_to_file(void *arg, struct altcp_pcb *pcb,
                               struct pbuf *p, err_t err)
{
    struct net_http_download *dl = arg;
    struct pbuf *q;

    (void)err;
    if (!dl || !pcb || !p || dl->fd < 0) {
        if (p) {
            pbuf_free(p);
        }
        return ERR_ARG;
    }

    for (q = p; q != NULL; q = q->next) {
        int written = littlefs_file_write(dl->mnt, dl->fd, q->payload, q->len);
        if (written != (int)q->len) {
            pbuf_free(p);
            return ERR_VAL;
        }
        sha256_update(&dl->sha256, q->payload, q->len);
    }

    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static int close_and_cleanup_temp(struct net_http_download *dl, bool keep_temp)
{
    int rc = NET_OK;

    if (dl->fd >= 0) {
        if (littlefs_file_sync(dl->mnt, dl->fd) != 0) {
            rc = NET_E_GENERIC;
        }
        if (littlefs_file_close(dl->mnt, dl->fd) != 0) {
            rc = NET_E_GENERIC;
        }
        dl->fd = -1;
    }
    if (!keep_temp) {
        (void)littlefs_remove(dl->mnt, dl->temp_subpath);
    }
    return rc;
}

int net_http_get_file(const char *url, const char *dest_path,
                      const char *expected_sha256_hex,
                      struct net_http_get_result *out)
{
    struct net_http_url parsed;
    struct net_http_download dl;
    httpc_connection_t conn;
    char temp_path[VFS_MAX_PATH];
    const char *subpath = NULL;
    const char *temp_subpath = NULL;
    httpc_state_t *httpc_conn = NULL;
    uint32_t start_ms;
    err_t err;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->content_length = NET_HTTP_CONTENT_LEN_UNKNOWN;
    }
    if (!url || !dest_path || dest_path[0] != '/') {
        return NET_E_INVAL;
    }
    if (!net_is_up()) {
        return NET_E_NOT_INIT;
    }
    if (net_http_parse_url(url, &parsed) != 0) {
        return NET_E_INVAL;
    }
    if (build_temp_path(dest_path, temp_path, sizeof(temp_path)) != 0) {
        return NET_E_INVAL;
    }

    memset(&dl, 0, sizeof(dl));
    dl.fd = -1;
    dl.content_length = NET_HTTP_CONTENT_LEN_UNKNOWN;
    dl.result.content_length = NET_HTTP_CONTENT_LEN_UNKNOWN;
    dl.has_expected_sha256 = (expected_sha256_hex != NULL);
    if (dl.has_expected_sha256 &&
        net_http_parse_sha256_hex(expected_sha256_hex, dl.expected_sha256) != 0) {
        return NET_E_INVAL;
    }
    sha256_init(&dl.sha256);

    dl.mnt = vfs_get_mount_ctx(dest_path, &subpath);
    if (!dl.mnt || !subpath) {
        return NET_E_INVAL;
    }
    if (strlen(subpath) >= sizeof(dl.final_subpath)) {
        return NET_E_INVAL;
    }
    memcpy(dl.final_subpath, subpath, strlen(subpath) + 1U);

    if (!vfs_get_mount_ctx(temp_path, &temp_subpath) || !temp_subpath) {
        return NET_E_INVAL;
    }
    if (strlen(temp_subpath) >= sizeof(dl.temp_subpath)) {
        return NET_E_INVAL;
    }
    memcpy(dl.temp_subpath, temp_subpath, strlen(temp_subpath) + 1U);

    (void)littlefs_remove(dl.mnt, dl.temp_subpath);
    dl.fd = littlefs_file_open(dl.mnt, dl.temp_subpath,
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (dl.fd < 0) {
        return NET_E_GENERIC;
    }

    memset(&conn, 0, sizeof(conn));
    conn.result_fn = http_result_cb;
    conn.headers_done_fn = http_headers_done_cb;

    {
        ip_addr_t server_addr;
        if (ipaddr_aton(parsed.host, &server_addr)) {
            err = httpc_get_file(&server_addr, parsed.port, parsed.uri,
                                 &conn, http_recv_to_file, &dl, &httpc_conn);
        } else {
            err = httpc_get_file_dns(parsed.host, parsed.port, parsed.uri,
                                     &conn, http_recv_to_file, &dl, &httpc_conn);
        }
    }
    (void)httpc_conn;
    if (err != ERR_OK) {
        dl.result.lwip_err = (int)err;
        (void)close_and_cleanup_temp(&dl, false);
        if (out) {
            *out = dl.result;
        }
        return (err == ERR_MEM) ? NET_E_NO_MEM : NET_E_GENERIC;
    }

    start_ms = sys_now();
    while (!dl.done) {
        net_poll();
        if ((sys_now() - start_ms) > NET_HTTP_OVERALL_TIMEOUT_MS) {
            (void)close_and_cleanup_temp(&dl, false);
            return NET_E_TIMEOUT;
        }
    }

    dl.result.content_length = dl.content_length;
    if (!dl.success) {
        (void)close_and_cleanup_temp(&dl, false);
        if (out) {
            *out = dl.result;
        }
        return (dl.result.httpc_result == HTTPC_RESULT_ERR_TIMEOUT)
                   ? NET_E_TIMEOUT
                   : NET_E_GENERIC;
    }

    if (close_and_cleanup_temp(&dl, true) != 0) {
        (void)littlefs_remove(dl.mnt, dl.temp_subpath);
        return NET_E_GENERIC;
    }
    {
        uint8_t digest[SHA256_DIGEST_LEN];
        sha256_final(&dl.sha256, digest);
        sha256_bytes_to_hex(digest, dl.result.sha256_hex);
        dl.result.hash_checked = dl.has_expected_sha256 ? 1u : 0u;
        dl.result.hash_verified = (!dl.has_expected_sha256 ||
                                   memcmp(digest, dl.expected_sha256, SHA256_DIGEST_LEN) == 0)
                                      ? 1u : 0u;
        if (dl.has_expected_sha256 && !dl.result.hash_verified) {
            (void)littlefs_remove(dl.mnt, dl.temp_subpath);
            if (out) {
                *out = dl.result;
            }
            return NET_E_GENERIC;
        }
    }
    if (littlefs_rename(dl.mnt, dl.temp_subpath, dl.final_subpath) != 0) {
        if (littlefs_remove(dl.mnt, dl.final_subpath) != 0 ||
            littlefs_rename(dl.mnt, dl.temp_subpath, dl.final_subpath) != 0) {
            (void)littlefs_remove(dl.mnt, dl.temp_subpath);
            return NET_E_GENERIC;
        }
    }

    if (out) {
        *out = dl.result;
    }
    return NET_OK;
}
