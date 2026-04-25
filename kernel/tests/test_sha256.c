/*
 * test_sha256.c — SHA-256 vector tests.
 *
 * Verifies `kernel/lib/sha256.c` against the canonical NIST FIPS
 * 180-4 test vectors plus a streaming-equivalence check.
 *
 * Belt-and-suspenders coverage: PR #395 (#370 / Stage 4) exercises
 * the implementation indirectly via
 * `test_kernel_cmd.c::test_stage_writes_image_and_sha_sidecar`
 * (pinned-hex sidecar verification). This suite verifies the
 * algorithm directly so a future maintainer who breaks the impl
 * (e.g. while editing for warnings) gets a focused failure
 * instead of an opaque kernel_cmd-test crash.
 *
 * The lib's API uses init/update/final (lower-case) plus a
 * `sha256_bytes_to_hex` helper — see `kernel/include/sha256.h`.
 */

#include "unity.h"
#include "../include/sha256.h"

#include <stdint.h>
#include <string.h>

/*
 * Compute SHA-256 in one shot via the streaming API. Saves a
 * three-line ritual at every call site.
 */
static void sha256_oneshot(const void *data, size_t len,
                           uint8_t out[SHA256_DIGEST_LEN])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* ---- Vectors ---- */

/*
 * NIST FIPS 180-4 §B.1 — empty input.
 *   SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 */
static void test_sha256_empty(void)
{
    uint8_t out[SHA256_DIGEST_LEN];
    sha256_oneshot("", 0, out);
    char hex[SHA256_HEX_LEN + 1];
    sha256_bytes_to_hex(out, hex);
    TEST_ASSERT_EQUAL_STRING(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        hex);
}

/*
 * NIST FIPS 180-2 §B.1 — "abc".
 *   SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 * The most widely-cited canonical vector for SHA-256.
 */
static void test_sha256_abc(void)
{
    uint8_t out[SHA256_DIGEST_LEN];
    sha256_oneshot("abc", 3, out);
    char hex[SHA256_HEX_LEN + 1];
    sha256_bytes_to_hex(out, hex);
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        hex);
}

/*
 * NIST FIPS 180-2 §B.2 — 56-byte input straddling a single
 * 64-byte block (forces the padding to span into a second block).
 *   SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
 *     = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
 * Catches off-by-one errors in the length-byte padding.
 */
static void test_sha256_two_block_padding(void)
{
    static const char *msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t out[SHA256_DIGEST_LEN];
    sha256_oneshot(msg, strlen(msg), out);
    char hex[SHA256_HEX_LEN + 1];
    sha256_bytes_to_hex(out, hex);
    TEST_ASSERT_EQUAL_STRING(
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        hex);
}

/*
 * Streaming-API equivalence: feeding the same bytes through three
 * `sha256_update` calls must yield the same digest as a single
 * call. Catches state-corruption bugs across update boundaries
 * (e.g. a wrong residual count after a sub-block update).
 */
static void test_sha256_streaming_matches_oneshot(void)
{
    static const uint8_t payload[1024];   /* 1 KB of zeros */
    uint8_t one[SHA256_DIGEST_LEN];
    uint8_t streamed[SHA256_DIGEST_LEN];

    sha256_oneshot(payload, sizeof(payload), one);

    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, payload,        100);
    sha256_update(&ctx, payload + 100,  337);
    sha256_update(&ctx, payload + 437,  sizeof(payload) - 437);
    sha256_final(&ctx, streamed);

    TEST_ASSERT_EQUAL_MEMORY(one, streamed, SHA256_DIGEST_LEN);
}

/*
 * Idempotency: hashing the same buffer twice with separate
 * contexts produces the same digest. Catches static-state
 * pollution between calls.
 */
static void test_sha256_repeatable(void)
{
    static const uint8_t payload[256];
    uint8_t a[SHA256_DIGEST_LEN];
    uint8_t b[SHA256_DIGEST_LEN];
    sha256_oneshot(payload, sizeof(payload), a);
    sha256_oneshot(payload, sizeof(payload), b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, SHA256_DIGEST_LEN);
}

int test_suite_sha256(void)
{
    UnityBegin("SHA-256 vector tests");

    RUN_TEST(test_sha256_empty);
    RUN_TEST(test_sha256_abc);
    RUN_TEST(test_sha256_two_block_padding);
    RUN_TEST(test_sha256_streaming_matches_oneshot);
    RUN_TEST(test_sha256_repeatable);

    return UnityEnd();
}
