#include "unity.h"

#include "../include/net_http.h"
#include "../include/shell.h"
#include "../include/net.h"
#include "../include/vfs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void test_parse_http_url_basic(void)
{
    struct net_http_url url;

    TEST_ASSERT_EQUAL_INT(0,
        net_http_parse_url("http://example.com/models/a.bin", &url));
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(80, url.port);
    TEST_ASSERT_EQUAL_STRING("/models/a.bin", url.uri);
}

static void test_parse_http_url_with_port_and_root_default(void)
{
    struct net_http_url url;

    TEST_ASSERT_EQUAL_INT(0, net_http_parse_url("http://10.0.2.2:8080", &url));
    TEST_ASSERT_EQUAL_STRING("10.0.2.2", url.host);
    TEST_ASSERT_EQUAL_UINT16(8080, url.port);
    TEST_ASSERT_EQUAL_STRING("/", url.uri);
}

static void test_parse_http_url_query_only_uses_root_path(void)
{
    struct net_http_url url;

    TEST_ASSERT_EQUAL_INT(0, net_http_parse_url("http://example.com?sig=abc", &url));
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(80, url.port);
    TEST_ASSERT_EQUAL_STRING("/?sig=abc", url.uri);
}

static void test_parse_http_url_rejects_invalid_inputs(void)
{
    struct net_http_url url;

    TEST_ASSERT_TRUE(net_http_parse_url("https://example.com/a", &url) < 0);
    TEST_ASSERT_TRUE(net_http_parse_url("http:///a", &url) < 0);
    TEST_ASSERT_TRUE(net_http_parse_url("http://:8080/a", &url) < 0);
    TEST_ASSERT_TRUE(net_http_parse_url("http://example.com:0/a", &url) < 0);
    TEST_ASSERT_TRUE(net_http_parse_url("http://example.com:abc/a", &url) < 0);
}

static void test_parse_http_url_accepts_long_signed_uri(void)
{
    struct net_http_url url;
    char query[700];
    char long_url[900];
    static const char prefix[] = "/releases/model.blob?X-Amz-Signature=";
    int n;

    memset(query, 'a', sizeof(query) - 1);
    query[sizeof(query) - 1] = '\0';

    n = snprintf(
        long_url,
        sizeof(long_url),
        "http://example.com/releases/model.blob?X-Amz-Signature=%s&X-Amz-Expires=3600",
        query
    );
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(long_url));

    TEST_ASSERT_EQUAL_INT(0, net_http_parse_url(long_url, &url));
    TEST_ASSERT_EQUAL_STRING("example.com", url.host);
    TEST_ASSERT_EQUAL_UINT16(80, url.port);
    TEST_ASSERT_TRUE(strncmp(url.uri, prefix, strlen(prefix)) == 0);
}

static void test_parse_sha256_hex_accepts_valid_and_rejects_invalid(void)
{
    uint8_t digest[SHA256_DIGEST_LEN];

    TEST_ASSERT_EQUAL_INT(0,
        net_http_parse_sha256_hex(
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            digest));
    TEST_ASSERT_EQUAL_HEX8(0x01, digest[0]);
    TEST_ASSERT_EQUAL_HEX8(0xef, digest[15]);
    TEST_ASSERT_EQUAL_HEX8(0x01, digest[16]);
    TEST_ASSERT_EQUAL_HEX8(0xef, digest[31]);

    TEST_ASSERT_TRUE(net_http_parse_sha256_hex("abc", digest) < 0);
    TEST_ASSERT_TRUE(net_http_parse_sha256_hex(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg",
        digest) < 0);
}

static void ensure_net_shell_commands_registered(void)
{
    static bool registered = false;

    if (!registered) {
        net_shell_init();
        registered = true;
    }
}

static void test_http_shell_usage_and_validation(void)
{
    ensure_net_shell_commands_registered();

    TEST_ASSERT_EQUAL_INT(-1, shell_execute("http"));
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("http get"));
    TEST_ASSERT_EQUAL_INT(-1,
        shell_execute("http get https://example.com/a.bin /mnt/files/a.bin"));
    TEST_ASSERT_EQUAL_INT(-1,
        shell_execute("http get http://example.com/a.bin /mnt/files/a.bin badsha"));
}

static void test_http_shell_requires_network_for_valid_request(void)
{
    ensure_net_shell_commands_registered();

    if (net_is_up()) {
        TEST_IGNORE_MESSAGE("network already initialized in this test target");
    }
    TEST_ASSERT_EQUAL_INT(-1,
        shell_execute("http get http://example.com/a.bin /mnt/files/a.bin"));
}

static void test_http_shell_accepts_max_length_destination_path(void)
{
    char path[VFS_MAX_PATH];
    char cmd[1400];
    size_t prefix_len = strlen("/mnt/files/");
    size_t suffix_len = strlen(".bin");
    size_t fill_len;
    int n;

    ensure_net_shell_commands_registered();

    TEST_ASSERT_TRUE(VFS_MAX_PATH > (prefix_len + suffix_len + 1));
    fill_len = (VFS_MAX_PATH - 1) - prefix_len - suffix_len;
    memcpy(path, "/mnt/files/", prefix_len);
    memset(path + prefix_len, 'x', fill_len);
    memcpy(path + prefix_len + fill_len, ".bin", suffix_len);
    path[VFS_MAX_PATH - 1] = '\0';

    n = snprintf(cmd, sizeof(cmd), "http get http://example.com/a.bin %s", path);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE((size_t)n < sizeof(cmd));

    if (net_is_up()) {
        TEST_IGNORE_MESSAGE("network already initialized in this test target");
    }
    TEST_ASSERT_EQUAL_INT(-1, shell_execute(cmd));
}

int test_suite_net_http(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_http_url_basic);
    RUN_TEST(test_parse_http_url_with_port_and_root_default);
    RUN_TEST(test_parse_http_url_query_only_uses_root_path);
    RUN_TEST(test_parse_http_url_rejects_invalid_inputs);
    RUN_TEST(test_parse_http_url_accepts_long_signed_uri);
    RUN_TEST(test_parse_sha256_hex_accepts_valid_and_rejects_invalid);
    RUN_TEST(test_http_shell_usage_and_validation);
    RUN_TEST(test_http_shell_requires_network_for_valid_request);
    RUN_TEST(test_http_shell_accepts_max_length_destination_path);

    return UNITY_END();
}
