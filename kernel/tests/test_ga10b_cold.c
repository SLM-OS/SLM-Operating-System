/*
 * test_ga10b_cold.c - Pure helpers of the GA10B cold bring-up (#1088).
 *
 * The step functions in ga10b_cold.c drive hardware and run only on
 * Jetson; the decisions they make are factored into the inline helpers
 * of ga10b_cold.h, checked here against the values seen on
 * jetson-nano-2 and in nvgpu's power-on trace.
 */
#include "unity.h"
#include "../gpu/nvidia/ga10b_cold.h"

#include <stdint.h>

static void test_pri_start_and_poison(void)
{
    /* START_RESULTS connectivity bit (trace line 97 reads 0x1). */
    TEST_ASSERT_TRUE(ga10b_cold_pri_start_passed(0x1u));
    TEST_ASSERT_TRUE(ga10b_cold_pri_start_passed(0xFFFFFFF1u));
    TEST_ASSERT_FALSE(ga10b_cold_pri_start_passed(0x0u));
    TEST_ASSERT_FALSE(ga10b_cold_pri_start_passed(0x2u));

    /* FECS_CURRENT_CTX before the PRI ring is up (UEFI boot log). */
    TEST_ASSERT_TRUE(ga10b_cold_is_pri_poison(0xbadf5546u));
    TEST_ASSERT_TRUE(ga10b_cold_is_pri_poison(0xbadf0000u));
    TEST_ASSERT_FALSE(ga10b_cold_is_pri_poison(0x00000010u));   /* halted FECS CPUCTL */
    TEST_ASSERT_FALSE(ga10b_cold_is_pri_poison(0xb7b000a1u));   /* BOOT_0 */
}

static void test_gpc_floorsweep(void)
{
    /* Orin Nano: NV_FUSE_STATUS_OPT_GPC = 0x2, GPC1 swept. */
    TEST_ASSERT_TRUE(ga10b_cold_gpc_present(0x2u, 0));
    TEST_ASSERT_FALSE(ga10b_cold_gpc_present(0x2u, 1));
    TEST_ASSERT_TRUE(ga10b_cold_gpc_present(0x0u, 1));
    TEST_ASSERT_FALSE(ga10b_cold_gpc_present(0x3u, 0));
}

static void test_reset_id_mask(void)
{
    /* GR reset_id 12 -> PMC_DEVICE_ENABLE 0x1000; reset_id 1 -> 0x2
     * (trace: 0x600 goes 0x2 then 0x1002). */
    TEST_ASSERT_EQUAL_HEX32(0x1000u, ga10b_cold_reset_id_mask(12));
    TEST_ASSERT_EQUAL_HEX32(0x2u, ga10b_cold_reset_id_mask(1));
    TEST_ASSERT_EQUAL_HEX32(0x1u, ga10b_cold_reset_id_mask(32));   /* register 1, bit 0 */
}

static void test_mmu_debug_val(void)
{
    /* Trace: debug_wr page 0x1be366000 -> 0x01be3663. */
    TEST_ASSERT_EQUAL_HEX32(0x01be3663u, ga10b_cold_mmu_debug_val(0x1be366000ull));
    TEST_ASSERT_EQUAL_HEX32(0x01b39453u, ga10b_cold_mmu_debug_val(0x1b3945000ull));
    TEST_ASSERT_EQUAL_HEX32(0x3u, ga10b_cold_mmu_debug_val(0x0ull));
}

static void test_acr_image_fits(void)
{
    /* acr-gsp prod: 28672 B code, 9472 B data, 2048 B manifest;
     * GSP falcon 64 KB IMEM / 64 KB DMEM (HWCFG 0x80420100). */
    TEST_ASSERT_TRUE(ga10b_cold_acr_image_fits(28672u, 9472u, 2048u, 65536u, 65536u));
    TEST_ASSERT_FALSE(ga10b_cold_acr_image_fits(65540u, 9472u, 2048u, 65536u, 65536u));
    TEST_ASSERT_FALSE(ga10b_cold_acr_image_fits(28672u, 64000u, 2048u, 65536u, 65536u));
    TEST_ASSERT_FALSE(ga10b_cold_acr_image_fits(28670u, 9472u, 2048u, 65536u, 65536u));
    /* data + manifest must not wrap in 32 bits. */
    TEST_ASSERT_FALSE(ga10b_cold_acr_image_fits(4u, 0xFFFFF000u, 0x2000u, 65536u, 65536u));
}

#if defined(PLATFORM_JETSON_ORIN_NANO)
/* ga10b_cold.c (and with it the step-name table) is Jetson-only. */
static void test_step_names(void)
{
    TEST_ASSERT_EQUAL_STRING("power", ga10b_cold_step_name(GA10B_COLD_POWER));
    TEST_ASSERT_EQUAL_STRING("mnist", ga10b_cold_step_name(GA10B_COLD_MNIST));
    TEST_ASSERT_EQUAL_STRING("policies", ga10b_cold_step_name(GA10B_COLD_POLICIES));
    TEST_ASSERT_EQUAL_STRING("?", ga10b_cold_step_name(GA10B_COLD_STEP_COUNT));
}
#endif

/* #1171: recovery skips the channel-only level when an error is latched,
 * as nvgpu always resets the engine after an MMU fault. A plain hang (a
 * pending semaphore acquire) latches nothing. */
static void test_recover_needs_engine_reset(void)
{
    TEST_ASSERT_FALSE(ga10b_cold_recover_needs_engine_reset(0x00000ac2u, 0, 0, 0, 0));
    TEST_ASSERT_TRUE(ga10b_cold_recover_needs_engine_reset(0x00000020u, 0, 0, 0, 0));
    TEST_ASSERT_TRUE(ga10b_cold_recover_needs_engine_reset(0, 0x80000000u, 0, 0, 0));
    TEST_ASSERT_TRUE(ga10b_cold_recover_needs_engine_reset(0, 0, 0x80000000u, 0, 0));
    TEST_ASSERT_FALSE(ga10b_cold_recover_needs_engine_reset(0, 0x00000100u, 0x40000000u, 0, 0));
    TEST_ASSERT_TRUE(ga10b_cold_recover_needs_engine_reset(0, 0, 0, 0x00200000u, 0));
    TEST_ASSERT_TRUE(ga10b_cold_recover_needs_engine_reset(0, 0, 0, 0, 0x00000001u));
}

int test_suite_ga10b_cold(void)
{
    UnityBegin("test_ga10b_cold.c");
    RUN_TEST(test_pri_start_and_poison);
    RUN_TEST(test_gpc_floorsweep);
    RUN_TEST(test_reset_id_mask);
    RUN_TEST(test_mmu_debug_val);
    RUN_TEST(test_acr_image_fits);
    RUN_TEST(test_recover_needs_engine_reset);
#if defined(PLATFORM_JETSON_ORIN_NANO)
    RUN_TEST(test_step_names);
#endif
    return UnityEnd();
}
