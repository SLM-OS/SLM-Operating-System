/*
 * test_hailo_trace.c — coverage for the boundary-trace framework.
 *
 * Focuses on the deterministic surface: cmdline parser, mask
 * manipulators, phase tracker, and the guard inline. Emit helpers
 * write to UART and aren't validated here — they're best exercised
 * by visual inspection of captures (see PR C).
 */

#include "unity.h"
#include "test_harness.h"
#include "hailo_trace.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Helper to clear state between tests. Both masks → 0, current phase
 * → NONE. The reset function only clears the masks; phase is reset
 * here so test order doesn't matter. */
static void reset_trace(void)
{
    hailo_trace_reset();
    hailo_trace_set_phase(HAILO_TRACE_PHASE_NONE);
}

/* -------------------------------------------------------------------------- */
/* Cmdline parser                                                              */
/* -------------------------------------------------------------------------- */

static void test_cmdline_null_keeps_defaults(void)
{
    reset_trace();
    hailo_trace_cmdline_parse(NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

static void test_cmdline_empty_keeps_defaults(void)
{
    reset_trace();
    hailo_trace_cmdline_parse("");
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

static void test_cmdline_no_matching_tokens_keeps_defaults(void)
{
    reset_trace();
    hailo_trace_cmdline_parse("console=uart earlyprintk=1");
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

static void test_cmdline_phase_single(void)
{
    reset_trace();
    hailo_trace_cmdline_parse("hailo_trace.phase=link");
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_PHASE_LINKUP_BIT,
                             hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

static void test_cmdline_phase_multi(void)
{
    reset_trace();
    hailo_trace_cmdline_parse("hailo_trace.phase=link,fw_boot,postboot");
    const uint32_t expect = HAILO_TRACE_PHASE_LINKUP_BIT
                          | HAILO_TRACE_PHASE_FW_BOOT_BIT
                          | HAILO_TRACE_PHASE_POSTBOOT_BIT;
    TEST_ASSERT_EQUAL_UINT32(expect, hailo_trace_phase_mask);
}

static void test_cmdline_phase_all(void)
{
    reset_trace();
    hailo_trace_cmdline_parse("hailo_trace.phase=all");
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_PHASE_ALL, hailo_trace_phase_mask);
}

static void test_cmdline_mech_pair(void)
{
    reset_trace();
    hailo_trace_cmdline_parse(
        "hailo_trace.phase=inference hailo_trace.mech=mmio,pci");
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_PHASE_INFERENCE_BIT,
                             hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_MECH_MMIO | HAILO_TRACE_MECH_PCI_CFG,
                             hailo_trace_mech_mask);
}

static void test_cmdline_mech_synonyms(void)
{
    /* `pci`, `pci_cfg`, `cfg` all map to the same bit; OR is
     * idempotent so the mask should still just be PCI_CFG. */
    reset_trace();
    hailo_trace_cmdline_parse("hailo_trace.mech=pci,pci_cfg,cfg");
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_MECH_PCI_CFG, hailo_trace_mech_mask);
}

static void test_cmdline_unknown_token_skipped_others_applied(void)
{
    reset_trace();
    hailo_trace_cmdline_parse(
        "hailo_trace.phase=link,bogus,fw_boot");
    /* link and fw_boot armed, bogus silently skipped (warning logged). */
    const uint32_t expect = HAILO_TRACE_PHASE_LINKUP_BIT
                          | HAILO_TRACE_PHASE_FW_BOOT_BIT;
    TEST_ASSERT_EQUAL_UINT32(expect, hailo_trace_phase_mask);
}

static void test_cmdline_or_merges_with_build_default(void)
{
    /* Simulate a build-time non-zero default by arming a bit manually,
     * then parsing a cmdline. The parser OR-merges, so both should
     * end up armed. */
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_INFERENCE_BIT;
    hailo_trace_cmdline_parse("hailo_trace.phase=link");
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_PHASE_LINKUP_BIT
                             | HAILO_TRACE_PHASE_INFERENCE_BIT,
                             hailo_trace_phase_mask);
}

static void test_cmdline_key_prefix_collision_does_not_match(void)
{
    /* `hailo_trace.phasex=...` must NOT match `hailo_trace.phase`.
     * find_kv requires `tok[klen] == '='` so a prefix that doesn't
     * end at `=` is correctly rejected. Regression guard for anyone
     * who "simplifies" find_kv to a strncmp. */
    reset_trace();
    hailo_trace_cmdline_parse("hailo_trace.phasex=link "
                              "hailo_trace.mechs=mmio");
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

/* -------------------------------------------------------------------------- */
/* Shell-side manipulators                                                     */
/* -------------------------------------------------------------------------- */

static void test_set_phase_mask_overwrites(void)
{
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_ALL;
    int rc = hailo_trace_set_phase_mask("link");
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL_UINT32(HAILO_TRACE_PHASE_LINKUP_BIT,
                             hailo_trace_phase_mask);
}

static void test_set_mech_mask_off(void)
{
    reset_trace();
    hailo_trace_mech_mask = HAILO_TRACE_MECH_ALL;
    int rc = hailo_trace_set_mech_mask("off");
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

static void test_set_mask_unknown_returns_error(void)
{
    reset_trace();
    int rc = hailo_trace_set_phase_mask("not_a_phase");
    TEST_ASSERT_EQUAL(-1, rc);
    /* Mask still cleared (the unknown token contributed 0, no other
     * tokens were given). */
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
}

static void test_reset_clears_both(void)
{
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_ALL;
    hailo_trace_mech_mask  = HAILO_TRACE_MECH_ALL;
    hailo_trace_reset();
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_phase_mask);
    TEST_ASSERT_EQUAL_UINT32(0u, hailo_trace_mech_mask);
}

/* -------------------------------------------------------------------------- */
/* Phase tracker                                                               */
/* -------------------------------------------------------------------------- */

static void test_set_phase_updates_current(void)
{
    reset_trace();
    hailo_trace_set_phase(HAILO_TRACE_PHASE_FW_BOOT);
    TEST_ASSERT_EQUAL((int)HAILO_TRACE_PHASE_FW_BOOT,
                      (int)hailo_trace_current_phase);
}

static void test_set_phase_same_is_noop(void)
{
    reset_trace();
    hailo_trace_set_phase(HAILO_TRACE_PHASE_FW_BOOT);
    hailo_trace_set_phase(HAILO_TRACE_PHASE_FW_BOOT);
    /* No state change; just confirm the second call doesn't trash
     * anything. */
    TEST_ASSERT_EQUAL((int)HAILO_TRACE_PHASE_FW_BOOT,
                      (int)hailo_trace_current_phase);
}

/* -------------------------------------------------------------------------- */
/* hailo_trace_active() guard                                                  */
/* -------------------------------------------------------------------------- */

static void test_active_zero_mask_short_circuits(void)
{
    reset_trace();
    hailo_trace_set_phase(HAILO_TRACE_PHASE_INFERENCE);
    TEST_ASSERT_FALSE(hailo_trace_active(HAILO_TRACE_MECH_MMIO));
}

static void test_active_phase_mismatch_returns_false(void)
{
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_INFERENCE_BIT;
    hailo_trace_mech_mask  = HAILO_TRACE_MECH_MMIO;
    hailo_trace_set_phase(HAILO_TRACE_PHASE_FW_BOOT);  /* not in mask */
    TEST_ASSERT_FALSE(hailo_trace_active(HAILO_TRACE_MECH_MMIO));
}

static void test_active_mech_mismatch_returns_false(void)
{
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_INFERENCE_BIT;
    hailo_trace_mech_mask  = HAILO_TRACE_MECH_MMIO;
    hailo_trace_set_phase(HAILO_TRACE_PHASE_INFERENCE);
    TEST_ASSERT_FALSE(hailo_trace_active(HAILO_TRACE_MECH_PCI_CFG));
}

static void test_active_phase_none_returns_false(void)
{
    /* current_phase == NONE means we're before the first phase
     * transition. Tracing should be silent even with masks armed —
     * otherwise pre-init MMIO from cold-boot would spam without
     * locatable phase context. */
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_ALL;
    hailo_trace_mech_mask  = HAILO_TRACE_MECH_ALL;
    /* current_phase still NONE after reset_trace */
    TEST_ASSERT_FALSE(hailo_trace_active(HAILO_TRACE_MECH_MMIO));
}

static void test_active_intersection_returns_true(void)
{
    reset_trace();
    hailo_trace_phase_mask = HAILO_TRACE_PHASE_INFERENCE_BIT;
    hailo_trace_mech_mask  = HAILO_TRACE_MECH_MMIO | HAILO_TRACE_MECH_DMA;
    hailo_trace_set_phase(HAILO_TRACE_PHASE_INFERENCE);
    TEST_ASSERT_TRUE(hailo_trace_active(HAILO_TRACE_MECH_MMIO));
    TEST_ASSERT_TRUE(hailo_trace_active(HAILO_TRACE_MECH_DMA));
    TEST_ASSERT_FALSE(hailo_trace_active(HAILO_TRACE_MECH_RPC));
}

/* -------------------------------------------------------------------------- */
/* Suite entry                                                                 */
/* -------------------------------------------------------------------------- */

int test_suite_hailo_trace(void)
{
    UnityBegin("hailo_trace");

    RUN_TEST(test_cmdline_null_keeps_defaults);
    RUN_TEST(test_cmdline_empty_keeps_defaults);
    RUN_TEST(test_cmdline_no_matching_tokens_keeps_defaults);
    RUN_TEST(test_cmdline_phase_single);
    RUN_TEST(test_cmdline_phase_multi);
    RUN_TEST(test_cmdline_phase_all);
    RUN_TEST(test_cmdline_mech_pair);
    RUN_TEST(test_cmdline_mech_synonyms);
    RUN_TEST(test_cmdline_unknown_token_skipped_others_applied);
    RUN_TEST(test_cmdline_or_merges_with_build_default);
    RUN_TEST(test_cmdline_key_prefix_collision_does_not_match);

    RUN_TEST(test_set_phase_mask_overwrites);
    RUN_TEST(test_set_mech_mask_off);
    RUN_TEST(test_set_mask_unknown_returns_error);
    RUN_TEST(test_reset_clears_both);

    RUN_TEST(test_set_phase_updates_current);
    RUN_TEST(test_set_phase_same_is_noop);

    RUN_TEST(test_active_zero_mask_short_circuits);
    RUN_TEST(test_active_phase_mismatch_returns_false);
    RUN_TEST(test_active_mech_mismatch_returns_false);
    RUN_TEST(test_active_phase_none_returns_false);
    RUN_TEST(test_active_intersection_returns_true);

    reset_trace();  /* leave clean for subsequent suites */
    return UnityEnd();
}
