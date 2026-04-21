/*
 * test_bpmp.c — Unit tests for the Tegra234 BPMP IPC stack.
 *
 * The BPMP driver splits into four files under kernel/drivers/bpmp/:
 *   hsp.c    HSP doorbell (requires real MMIO to HSP_TOP_BASE)
 *   ivc.c    IVC channel protocol + handshake
 *   mrq.c    MRQ send/recv
 *   bpmp.c   public API glue (clk, reset, uphy, pg wrappers)
 *
 * All of these are gated on PLATFORM_JETSON_ORIN_NANO. On any other
 * platform, bpmp.c provides stubs that return neutral values so
 * callers (e.g. uart_tegra.c UART_INIT_MODE=1) link cleanly.
 *
 * These tests exercise the public API contract on QEMU/Pi5/x86-64
 * by checking the stub behaviour. Live Jetson hardware exercise
 * happens via the `hspdiag` / `bpmp` / `pcietrain` shell commands
 * and is logged in docs/jetson-pcie-investigation.md.
 */

#include "unity.h"
#include "platform.h"     /* TEGRA234_* ID constants on Jetson builds */
#include "bpmp.h"
#include "test_harness.h"
#include <stdbool.h>

#if !defined(PLATFORM_JETSON_ORIN_NANO)

/*
 * Stub-behaviour contract. The stubs exist so any code that links
 * against bpmp.h (e.g. kernel/drivers/uart_tegra.c) works on every
 * platform without conditional compilation. The contract:
 *
 *   bpmp_init()         returns 0 (success — nothing to do).
 *   bpmp_is_available() returns false (no BPMP here).
 *   All clk / reset / uphy / pg wrappers return 0 (success),
 *   regardless of arguments, so callers that only care about
 *   error propagation can treat non-Jetson as "operation succeeded".
 *
 * We use literal IDs here because the TEGRA234_* constants are
 * defined only on PLATFORM_JETSON_ORIN_NANO; for stub testing the
 * stubs ignore the argument anyway.
 */

static void test_bpmp_init_returns_zero_on_stub_platform(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_init());
}

static void test_bpmp_is_available_returns_false_on_stub(void)
{
    TEST_ASSERT_FALSE(bpmp_is_available());
}

static void test_bpmp_clk_enable_returns_zero_on_stub(void)
{
    /* Stub ignores argument; use literal UART-A id (155). */
    TEST_ASSERT_EQUAL_INT(0, bpmp_clk_enable(155));
    /* Zero is also accepted (some callers pass 0 for "no clock"). */
    TEST_ASSERT_EQUAL_INT(0, bpmp_clk_enable(0));
}

static void test_bpmp_clk_disable_returns_zero_on_stub(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_clk_disable(155));
}

static void test_bpmp_clk_is_enabled_returns_zero_on_stub(void)
{
    int state = 42;          /* poison — stub must overwrite to 0 */
    TEST_ASSERT_EQUAL_INT(0, bpmp_clk_is_enabled(155, &state));
    TEST_ASSERT_EQUAL_INT(0, state);
}

static void test_bpmp_reset_assert_returns_zero_on_stub(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_reset_assert(100));   /* UART-A reset */
}

static void test_bpmp_reset_deassert_returns_zero_on_stub(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_reset_deassert(100)); /* UART-A reset */
    TEST_ASSERT_EQUAL_INT(0, bpmp_reset_deassert(25));  /* PEX2_CORE_8 */
}

static void test_bpmp_uphy_pcie_controller_state_returns_zero_on_stub(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_uphy_pcie_controller_state(8, true));
    TEST_ASSERT_EQUAL_INT(0, bpmp_uphy_pcie_controller_state(8, false));
}

static void test_bpmp_pg_set_state_returns_zero_on_stub(void)
{
    TEST_ASSERT_EQUAL_INT(0, bpmp_pg_set_state(13, true));  /* PCIEX4CA */
    TEST_ASSERT_EQUAL_INT(0, bpmp_pg_set_state(13, false));
}

/*
 * Compile-time sanity: the public header must expose the opcode /
 * sub-command IDs so client code doesn't reach into private headers.
 * A mismatch here would break any caller that builds against bpmp.h
 * without pulling in mrq.h.
 */
static void test_bpmp_public_opcodes_are_stable(void)
{
    TEST_ASSERT_EQUAL_INT(0,  MRQ_PING);
    TEST_ASSERT_EQUAL_INT(22, MRQ_CLK);
    TEST_ASSERT_EQUAL_INT(20, MRQ_RESET);

    TEST_ASSERT_EQUAL_INT(7, CMD_CLK_ENABLE);
    TEST_ASSERT_EQUAL_INT(8, CMD_CLK_DISABLE);
    TEST_ASSERT_EQUAL_INT(6, CMD_CLK_IS_ENABLED);

    TEST_ASSERT_EQUAL_INT(1, CMD_RESET_ASSERT);
    TEST_ASSERT_EQUAL_INT(2, CMD_RESET_DEASSERT);
}

#endif /* !PLATFORM_JETSON_ORIN_NANO */

#if defined(PLATFORM_JETSON_ORIN_NANO)
/*
 * On Jetson, cross-check the TEGRA234_* ID constants platform.h
 * exports against their upstream dt-bindings values. If these drift,
 * BPMP MRQs will target the wrong hardware.
 */
static void test_bpmp_clock_and_reset_ids_match_tegra234_bindings(void)
{
    TEST_ASSERT_EQUAL_INT(172, TEGRA234_CLK_PEX2_C8_CORE);
    TEST_ASSERT_EQUAL_INT(155, TEGRA234_CLK_UARTA);

    TEST_ASSERT_EQUAL_INT(25, TEGRA234_RESET_PEX2_CORE_8);
    TEST_ASSERT_EQUAL_INT(26, TEGRA234_RESET_PEX2_CORE_8_APB);
    TEST_ASSERT_EQUAL_INT(100, TEGRA234_RESET_UARTA);

    TEST_ASSERT_EQUAL_INT(13, TEGRA234_POWER_DOMAIN_PCIEX4CA);
}
#endif

int test_suite_bpmp(void)
{
    UNITY_BEGIN();
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * On Jetson the BPMP IPC stack needs to talk to a live BPMP
     * firmware (HSP MMIO + SYSRAM doorbells) — no QEMU model. Real
     * hardware exercise is via the `bpmp`, `hspdiag`, and `pcietrain`
     * shell commands. See docs/jetson-pcie-investigation.md for the
     * recorded live outputs. The only QEMU-reachable check on Jetson
     * is that the TEGRA234_* id constants haven't drifted.
     */
    RUN_TEST(test_bpmp_clock_and_reset_ids_match_tegra234_bindings);
#else
    RUN_TEST(test_bpmp_init_returns_zero_on_stub_platform);
    RUN_TEST(test_bpmp_is_available_returns_false_on_stub);
    RUN_TEST(test_bpmp_clk_enable_returns_zero_on_stub);
    RUN_TEST(test_bpmp_clk_disable_returns_zero_on_stub);
    RUN_TEST(test_bpmp_clk_is_enabled_returns_zero_on_stub);
    RUN_TEST(test_bpmp_reset_assert_returns_zero_on_stub);
    RUN_TEST(test_bpmp_reset_deassert_returns_zero_on_stub);
    RUN_TEST(test_bpmp_uphy_pcie_controller_state_returns_zero_on_stub);
    RUN_TEST(test_bpmp_pg_set_state_returns_zero_on_stub);
    RUN_TEST(test_bpmp_public_opcodes_are_stable);
#endif
    return UNITY_END();
}
