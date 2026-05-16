/*
 * test_host_key.c — Ed25519 host-key persistence coverage.
 *
 * Verifies the read/generate/persist contract documented in
 * `kernel/net/ssh/host_key.h`:
 *
 *   - First call generates + persists; subsequent calls return the
 *     same bytes (read path).
 *   - Fingerprint is stable for the same public key and deterministic
 *     across calls.
 *   - Different public keys produce different fingerprints.
 *
 * The "persist across reboots" claim is verifiable on hardware (#199e)
 * but inside a QEMU test run we can only check that two same-boot
 * calls return identical bytes — the littlefs mount is reused between
 * them, which is the same code path as a fresh load post-reboot.
 *
 * Gated on NET_SSHD so the test only compiles when the daemon is
 * enabled (the host_key module is only built then). #199b / #892.
 */

#if defined(NET_SSHD)

#include "host_key.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

static void test_load_or_generate_produces_keypair(void)
{
    uint8_t buf[HOST_KEY_RAW_BUF_LEN];
    memset(buf, 0, sizeof(buf));

    int rc = host_key_load_or_generate(buf);
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, rc);

    /* Private + public should both be non-zero with overwhelming
     * probability for a real Ed25519 keypair. */
    int priv_nz = 0, pub_nz = 0;
    for (size_t i = 0; i < HOST_KEY_PRIV_LEN; i++)
        if (buf[i] != 0u) { priv_nz = 1; break; }
    for (size_t i = 0; i < HOST_KEY_PUB_LEN; i++)
        if (buf[HOST_KEY_PRIV_LEN + i] != 0u) { pub_nz = 1; break; }
    TEST_ASSERT_EQUAL_INT(1, priv_nz);
    TEST_ASSERT_EQUAL_INT(1, pub_nz);
}

static void test_load_after_generate_matches(void)
{
    uint8_t a[HOST_KEY_RAW_BUF_LEN];
    uint8_t b[HOST_KEY_RAW_BUF_LEN];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    /* First call generates + persists (or loads if a prior test
     * already did). */
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, host_key_load_or_generate(a));
    /* Second call exercises the load path — bytes must match. */
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, host_key_load_or_generate(b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, HOST_KEY_RAW_BUF_LEN);
}

static void test_fingerprint_is_stable(void)
{
    uint8_t buf[HOST_KEY_RAW_BUF_LEN];
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, host_key_load_or_generate(buf));

    char fp_a[HOST_KEY_FINGERPRINT_MAX];
    char fp_b[HOST_KEY_FINGERPRINT_MAX];
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK,
        host_key_fingerprint(buf + HOST_KEY_PRIV_LEN, fp_a, sizeof(fp_a)));
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK,
        host_key_fingerprint(buf + HOST_KEY_PRIV_LEN, fp_b, sizeof(fp_b)));
    TEST_ASSERT_EQUAL_STRING(fp_a, fp_b);

    /* Must start with the OpenSSH "SHA256:" prefix. */
    TEST_ASSERT_EQUAL_INT(0, strncmp(fp_a, "SHA256:", 7));
    /* "SHA256:" + 43 base64 chars (no padding) = 50 chars. */
    TEST_ASSERT_EQUAL_INT(50, (int)strlen(fp_a));
}

static void test_fingerprint_differs_for_different_keys(void)
{
    /* Build two synthetic public keys with all-A vs all-B bytes —
     * SHA-256 of their RFC 4253 blobs must differ. */
    uint8_t pub_a[HOST_KEY_PUB_LEN];
    uint8_t pub_b[HOST_KEY_PUB_LEN];
    memset(pub_a, 0xAAu, sizeof(pub_a));
    memset(pub_b, 0xBBu, sizeof(pub_b));

    char fp_a[HOST_KEY_FINGERPRINT_MAX];
    char fp_b[HOST_KEY_FINGERPRINT_MAX];
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK,
        host_key_fingerprint(pub_a, fp_a, sizeof(fp_a)));
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK,
        host_key_fingerprint(pub_b, fp_b, sizeof(fp_b)));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(fp_a, fp_b));
}

static void test_regenerate_produces_new_key(void)
{
    uint8_t before[HOST_KEY_RAW_BUF_LEN];
    uint8_t after[HOST_KEY_RAW_BUF_LEN];

    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, host_key_load_or_generate(before));
    TEST_ASSERT_EQUAL_INT(HOST_KEY_OK, host_key_regenerate(after));

    /* Equal bytes after regenerate would imply the regenerate code
     * path read the file rather than removing + recreating. */
    int differ = (memcmp(before, after, HOST_KEY_RAW_BUF_LEN) != 0);
    TEST_ASSERT_EQUAL_INT(1, differ);
}

static void test_fingerprint_buf_too_small(void)
{
    uint8_t pub[HOST_KEY_PUB_LEN];
    memset(pub, 0x55u, sizeof(pub));
    char tiny[8];
    int rc = host_key_fingerprint(pub, tiny, sizeof(tiny));
    TEST_ASSERT_NOT_EQUAL(0, rc);
}

int test_suite_host_key(void)
{
    UnityBegin("SSH host key (Ed25519, VFS-persisted)");

    RUN_TEST(test_load_or_generate_produces_keypair);
    RUN_TEST(test_load_after_generate_matches);
    RUN_TEST(test_fingerprint_is_stable);
    RUN_TEST(test_fingerprint_differs_for_different_keys);
    RUN_TEST(test_regenerate_produces_new_key);
    RUN_TEST(test_fingerprint_buf_too_small);

    return UnityEnd();
}

#else  /* !NET_SSHD */

/* Keep the TU non-empty so -Wpedantic stays happy when NET_SSHD is
 * off. The test_suite_host_key() symbol is only referenced from
 * test_harness.c inside an #if defined(NET_SSHD) guard, so this
 * stub never runs in the OFF build. */
typedef int test_host_key_placeholder_t;

#endif /* NET_SSHD */
