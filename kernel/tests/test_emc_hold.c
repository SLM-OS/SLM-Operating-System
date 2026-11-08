/*
 * test_emc_hold.c - EMC hold check against a fake BPMP (#1161).
 *
 * The fake models what jetson-nano-2 showed: a request for 3199 MHz is
 * granted, and some later event puts the rate back to 204 MHz.
 */
#include "unity.h"
#include "emc_hold.h"
#include "test_harness.h"

#include <stdint.h>
#include <string.h>

#define MHZ(x) ((uint64_t)(x) * 1000000u)

static struct {
    uint64_t rate;          /* what get_rate reports */
    uint64_t grant;         /* what a request sets the rate to */
    int get_rc;
    unsigned get_fail_from;  /* if non-zero, gets from this one on fail */
    int req_rc;
    unsigned gets;
    unsigned requests;
    uint64_t last_request;
} fake;

static int fake_get(uint64_t *hz)
{
    fake.gets++;
    if (fake.get_rc != 0) return fake.get_rc;
    if (fake.get_fail_from != 0 && fake.gets >= fake.get_fail_from) return -4;
    *hz = fake.rate;
    return 0;
}

static int fake_request(uint64_t hz, uint64_t *got)
{
    fake.requests++;
    fake.last_request = hz;
    if (fake.req_rc != 0) return fake.req_rc;
    fake.rate = fake.grant;
    *got = hz;  /* BPMP's reply: the rate it chose, not what stays */
    return 0;
}

static const struct emc_hold_ops fake_ops = {
    .get_rate = fake_get,
    .request = fake_request,
};

static void fake_reset(uint64_t rate, uint64_t grant)
{
    memset(&fake, 0, sizeof(fake));
    fake.rate = rate;
    fake.grant = grant;
}

static void test_at_target_is_ok_and_not_rerequested(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(3199), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    uint64_t before = 0, after = 0;
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, emc_hold_check(&s, "step", &before, &after));
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), before);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), after);
    TEST_ASSERT_EQUAL_UINT32(1, s.checks);
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);
    TEST_ASSERT_EQUAL_UINT(0, fake.requests);
    TEST_ASSERT_EQUAL_STRING("", s.first_drop);
}

static void test_drop_is_counted_located_and_restored(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    uint64_t before = 0, after = 0;
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_RESTORED,
                          emc_hold_check(&s, "pg_on", &before, &after));
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), before);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), after);
    TEST_ASSERT_EQUAL_UINT(1, fake.requests);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), fake.last_request);
    TEST_ASSERT_EQUAL_UINT32(1, s.drops);
    TEST_ASSERT_EQUAL_UINT32(1, s.reasserts);
    TEST_ASSERT_EQUAL_STRING("pg_on", s.first_drop);
    TEST_ASSERT_EQUAL_STRING("pg_on", s.last_drop);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), s.last_hz);
}

static void test_readback_wins_over_request_reply(void)
{
    /* BPMP replies 3199 but the rate stays at 204: still low. */
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(204));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    uint64_t after = 0;
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_STILL_LOW, emc_hold_check(&s, "x", NULL, &after));
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), after);
    TEST_ASSERT_EQUAL_UINT32(1, s.drops);
    TEST_ASSERT_EQUAL_UINT32(0, s.reasserts);
}

static void test_failed_rerequest_is_still_low(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    fake.req_rc = -4;
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_STILL_LOW, emc_hold_check(&s, "x", NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, s.drops);
    TEST_ASSERT_EQUAL_UINT32(0, s.reasserts);
}

static void test_read_error_does_not_request(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    fake.get_rc = -4;
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_READ_ERROR, emc_hold_check(&s, "x", NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, s.read_errors);
    TEST_ASSERT_EQUAL_UINT32(0, s.checks);
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);
    TEST_ASSERT_EQUAL_UINT(0, fake.requests);
}

static void test_failed_readback_after_rerequest_is_unknown(void)
{
    /* The re-request is granted, but the readback after it fails: the
     * reply alone must not count as restored (#1161 review). */
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    fake.get_fail_from = 2;
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    uint64_t before = 0, after = 0;
    /* Unity's macros evaluate their arguments twice: check once. */
    enum emc_hold_result r = emc_hold_check(&s, "pg_on", &before, &after);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_READ_ERROR, r);
    TEST_ASSERT_EQUAL_UINT(1, fake.requests);
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), before);
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), after);
    TEST_ASSERT_EQUAL_UINT32(1, s.drops);
    TEST_ASSERT_EQUAL_UINT32(0, s.reasserts);
    TEST_ASSERT_EQUAL_UINT32(1, s.read_errors);
    TEST_ASSERT_EQUAL_UINT32(1, s.checks);
    TEST_ASSERT_EQUAL_STRING("pg_on", s.first_drop);
}

/* -95: what a refused floor comes back as (-BPMP_ENOTSUP). */
#define FAKE_ENOTSUP (-95)

static void test_set_holds_the_granted_rate_not_the_request(void)
{
    /* `clocks emc 3200`: BPMP grants its top rate, 3199. Holding 3200
     * would count a drop and re-request on every check (#1161 review). */
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    emc_hold_init(&s, &fake_ops, 0);
    struct emc_hold_grant g;
    int rc = emc_hold_set(&s, MHZ(3200), &g);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(0, g.read_rc);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), g.read_hz);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), s.target_hz);
    for (int i = 0; i < 3; i++) {
        enum emc_hold_result r = emc_hold_check(&s, "runtime", NULL, NULL);
        TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, r);
    }
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);
    TEST_ASSERT_EQUAL_UINT(1, fake.requests);
}

static void test_refused_set_keeps_the_target(void)
{
    /* Held at 3199; a refused request leaves that target alone. */
    struct emc_hold_state s;
    fake_reset(MHZ(3199), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    fake.req_rc = FAKE_ENOTSUP;
    struct emc_hold_grant g;
    int rc = emc_hold_set(&s, MHZ(4000), &g);
    TEST_ASSERT_EQUAL_INT(FAKE_ENOTSUP, rc);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), s.target_hz);
    enum emc_hold_result r = emc_hold_check(&s, "runtime", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, r);
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);

    /* From observe only (a refused boot request): still nothing to hold,
     * so a 204 MHz reading is no drop and asks for nothing. */
    fake_reset(MHZ(204), MHZ(204));
    fake.req_rc = FAKE_ENOTSUP;
    emc_hold_init(&s, &fake_ops, 0);
    rc = emc_hold_set(&s, MHZ(3199), &g);
    TEST_ASSERT_EQUAL_INT(FAKE_ENOTSUP, rc);
    TEST_ASSERT_EQUAL_UINT64(0, s.target_hz);
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), s.last_hz);
    r = emc_hold_check(&s, "runtime", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, r);
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);
    TEST_ASSERT_EQUAL_UINT(1, fake.requests);
}

static void test_set_below_another_clients_floor(void)
{
    /* `clocks emc 204` while another floor keeps EMC at 3199: hold 204,
     * which 3199 satisfies. */
    struct emc_hold_state s;
    fake_reset(MHZ(3199), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    struct emc_hold_grant g;
    int rc = emc_hold_set(&s, MHZ(204), &g);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), s.target_hz);
    enum emc_hold_result r = emc_hold_check(&s, "runtime", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, r);
}

static void test_set_without_readback_uses_the_reply(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    fake.get_rc = -4;
    emc_hold_init(&s, &fake_ops, 0);
    struct emc_hold_grant g;
    int rc = emc_hold_set(&s, MHZ(3199), &g);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(-4, g.read_rc);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), g.reply_hz);
    TEST_ASSERT_EQUAL_UINT64(MHZ(3199), s.target_hz);
}

static void test_failed_rerequest_reports_its_rc(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    fake.req_rc = FAKE_ENOTSUP;
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    enum emc_hold_result r = emc_hold_check(&s, "runtime", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_STILL_LOW, r);
    TEST_ASSERT_EQUAL_INT(FAKE_ENOTSUP, s.last_request_rc);
    fake.req_rc = 0;
    r = emc_hold_check(&s, "runtime", NULL, NULL);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_RESTORED, r);
    TEST_ASSERT_EQUAL_INT(0, s.last_request_rc);
}

static void test_observe_only_never_requests(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    emc_hold_init(&s, &fake_ops, 0);
    TEST_ASSERT_EQUAL_INT(EMC_HOLD_OK, emc_hold_check(&s, "x", NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s.drops);
    TEST_ASSERT_EQUAL_UINT(0, fake.requests);
    TEST_ASSERT_EQUAL_UINT64(MHZ(204), s.last_hz);
}

static void test_boot_sequence_catches_every_reset(void)
{
    /* Request at bpmp_init, then two later events reset the rate (as a
     * second partition power-on would). A check after every step finds
     * both, keeps the first and the last, and ends at the target. */
    struct emc_hold_state s;
    fake_reset(MHZ(3199), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    static const char *steps[] = { "fan", "pg_on gpu", "pri", "pg_on xusba", "xhci" };
    for (unsigned i = 0; i < 5; i++) {
        if (i == 1 || i == 3) fake.rate = MHZ(204);   /* the reset */
        (void)emc_hold_check(&s, steps[i], NULL, NULL);
        TEST_ASSERT_EQUAL_UINT64(MHZ(3199), fake.rate);
    }
    TEST_ASSERT_EQUAL_UINT32(5, s.checks);
    TEST_ASSERT_EQUAL_UINT32(2, s.drops);
    TEST_ASSERT_EQUAL_UINT32(2, s.reasserts);
    TEST_ASSERT_EQUAL_STRING("pg_on gpu", s.first_drop);
    TEST_ASSERT_EQUAL_STRING("pg_on xusba", s.last_drop);
}

static void test_long_where_is_truncated(void)
{
    struct emc_hold_state s;
    fake_reset(MHZ(204), MHZ(3199));
    emc_hold_init(&s, &fake_ops, MHZ(3199));
    char longname[EMC_HOLD_WHERE_LEN * 2];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    (void)emc_hold_check(&s, longname, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT(EMC_HOLD_WHERE_LEN - 1, (unsigned)strlen(s.first_drop));
    TEST_ASSERT_EQUAL_UINT(EMC_HOLD_WHERE_LEN - 1, (unsigned)strlen(s.last_drop));
}

int test_suite_emc_hold(void)
{
    UnityBegin("test_emc_hold.c");
    RUN_TEST(test_at_target_is_ok_and_not_rerequested);
    RUN_TEST(test_drop_is_counted_located_and_restored);
    RUN_TEST(test_readback_wins_over_request_reply);
    RUN_TEST(test_failed_rerequest_is_still_low);
    RUN_TEST(test_read_error_does_not_request);
    RUN_TEST(test_failed_readback_after_rerequest_is_unknown);
    RUN_TEST(test_set_holds_the_granted_rate_not_the_request);
    RUN_TEST(test_refused_set_keeps_the_target);
    RUN_TEST(test_set_below_another_clients_floor);
    RUN_TEST(test_set_without_readback_uses_the_reply);
    RUN_TEST(test_failed_rerequest_reports_its_rc);
    RUN_TEST(test_observe_only_never_requests);
    RUN_TEST(test_boot_sequence_catches_every_reset);
    RUN_TEST(test_long_where_is_truncated);
    return UnityEnd();
}
