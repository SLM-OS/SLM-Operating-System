/*
 * test_rng.c — crypto-quality RNG regression coverage.
 *
 * Targets:
 *
 *   - rng_init runs without crashing on every platform (TRNG present
 *     or absent).
 *   - rng_get_bytes returns 0 and the buffer is no longer all-zero
 *     after a populated call.
 *   - Source dispatch: forcing RNG_SOURCE_JITTER produces output
 *     independent of any architectural TRNG state.
 *   - Test-injection path: rng_test_inject_bytes drains in order
 *     and falls through to the live source for the tail.
 *   - rng_selftest's chi-squared check fires on a stuck source
 *     (injected all-zero stream long enough to exhaust the buffer).
 *
 * Out of scope here: NIST SP800-90B battery (lands with #199e / #895
 * once the test infrastructure to run `ent`-equivalent off-target
 * exists). The chi-squared check inside the kernel is a sanity test
 * only; the audit happens off-target with much more data.
 */

#include "rng.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Unity uses module-global setUp/tearDown shared across the whole
 * test_harness build. Per-test reset of force-source / injection
 * happens inline at the top of any test that touches them, and the
 * tests that don't touch them tolerate prior state. */
static void rng_test_reset_overrides(void)
{
    rng_test_force_source(RNG_SOURCE_NONE);
    rng_test_inject_bytes(NULL, 0);
}

static void test_init_returns_and_source_is_set(void)
{
    /* rng_init has already been called from kernel_main, so the
     * source must be one of the three live values. NONE would
     * indicate the init path silently exited without bootstrapping
     * the jitter pool — a regression. */
    enum rng_source s = rng_active_source();
    TEST_ASSERT_TRUE(s == RNG_SOURCE_RNDR ||
                     s == RNG_SOURCE_RDRAND ||
                     s == RNG_SOURCE_JITTER);
}

static void test_get_bytes_produces_nonzero_output(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    int rc = rng_get_bytes(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, rc);

    /* Probability that 64 bytes from a sane source are all zero
     * is ~2^-512. Failure here is a real signal. */
    int nonzero = 0;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0u) { nonzero = 1; break; }
    }
    TEST_ASSERT_EQUAL_INT(1, nonzero);
}

static void test_get_bytes_rejects_bad_args(void)
{
    uint8_t buf[8];
    TEST_ASSERT_NOT_EQUAL(0, rng_get_bytes(NULL, sizeof(buf)));
    TEST_ASSERT_NOT_EQUAL(0, rng_get_bytes(buf, 0u));
}

static void test_force_jitter_source(void)
{
    rng_test_reset_overrides();
    /* Force the jitter path; even on a Jetson with RNDR live, the
     * output must come from the SHA-256 pool. We don't assert anything
     * about the byte distribution here — that's covered by the
     * selftest path — but a non-zero return + non-all-zero buffer is
     * a clean dispatch check. */
    enum rng_source prev = rng_test_force_source(RNG_SOURCE_JITTER);

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, rng_get_bytes(buf, sizeof(buf)));

    int all_zero = 1;
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0u) { all_zero = 0; break; }
    }
    TEST_ASSERT_EQUAL_INT(0, all_zero);

    /* Restore. */
    (void)rng_test_force_source(prev);
}

static void test_inject_drains_then_falls_through(void)
{
    rng_test_reset_overrides();
    static const uint8_t pattern[] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
    };
    size_t accepted = rng_test_inject_bytes(pattern, sizeof(pattern));
    TEST_ASSERT_EQUAL_UINT(sizeof(pattern), accepted);

    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, rng_get_bytes(buf, sizeof(buf)));

    /* First 8 bytes match the injected pattern. */
    TEST_ASSERT_EQUAL_MEMORY(pattern, buf, sizeof(pattern));

    /* Remaining 8 bytes came from the live source. Not all zero,
     * with overwhelming probability. */
    int tail_nonzero = 0;
    for (size_t i = sizeof(pattern); i < sizeof(buf); i++) {
        if (buf[i] != 0u) { tail_nonzero = 1; break; }
    }
    TEST_ASSERT_EQUAL_INT(1, tail_nonzero);
}

static void test_selftest_passes_on_live_source(void)
{
    rng_test_reset_overrides();
    char reason[80] = {0};
    int rc = rng_selftest(reason, sizeof(reason));
    if (rc != 0) {
        /* Surface the reason in the failure message — Unity captures
         * the assertion site, the reason gives the operator a hint
         * about which check fired. */
        TEST_FAIL_MESSAGE(reason[0] != '\0' ? reason : "rng_selftest failed with no reason");
    }
}

static void test_selftest_fails_on_stuck_zero_source(void)
{
    rng_test_reset_overrides();
    /* Inject 4 KB of zeros. The selftest pulls exactly 4 KB; the
     * histogram is then 100 % in bin 0, sum_sq_dev = 4080^2 +
     * 255 * 16^2 ≈ 16.7 M, way above the FAIL threshold of
     * 600 * 16 = 9600. */
    static uint8_t zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    size_t accepted = rng_test_inject_bytes(zeros, sizeof(zeros));
    TEST_ASSERT_EQUAL_UINT(sizeof(zeros), accepted);

    char reason[80] = {0};
    int rc = rng_selftest(reason, sizeof(reason));
    TEST_ASSERT_NOT_EQUAL(0, rc);
    /* Reason should be non-empty so the operator can tell which arm
     * fired. */
    TEST_ASSERT_NOT_EQUAL(0, reason[0]);
}

static void test_get_u32_nonzero_with_high_probability(void)
{
    /* A single u32 is 0 with probability ~2^-32; do four pulls and
     * assert at least one is non-zero to keep the test deterministic
     * even on a (statistically improbable) zero output. */
    int saw_nonzero = 0;
    for (int i = 0; i < 4; i++) {
        if (rng_get_u32() != 0u) { saw_nonzero = 1; break; }
    }
    TEST_ASSERT_EQUAL_INT(1, saw_nonzero);
}

static void test_stats_monotonic(void)
{
    struct rng_stats a, b;
    rng_get_stats(&a);

    uint8_t buf[64];
    TEST_ASSERT_EQUAL_INT(0, rng_get_bytes(buf, sizeof(buf)));

    rng_get_stats(&b);
    TEST_ASSERT_TRUE(b.bytes_served >= a.bytes_served + sizeof(buf));
}

int test_suite_rng(void)
{
    UnityBegin("RNG (crypto-quality)");

    RUN_TEST(test_init_returns_and_source_is_set);
    RUN_TEST(test_get_bytes_produces_nonzero_output);
    RUN_TEST(test_get_bytes_rejects_bad_args);
    RUN_TEST(test_force_jitter_source);
    RUN_TEST(test_inject_drains_then_falls_through);
    RUN_TEST(test_selftest_passes_on_live_source);
    RUN_TEST(test_selftest_fails_on_stuck_zero_source);
    RUN_TEST(test_get_u32_nonzero_with_high_probability);
    RUN_TEST(test_stats_monotonic);

    return UnityEnd();
}
