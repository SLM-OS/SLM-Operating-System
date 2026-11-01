/*
 * Tegra234 CPU NDIV helper tests (#1053).
 *
 * The register access itself only exists on Jetson; these pin the pure
 * helpers it relies on, using the values read from jetson-nano-2:
 *
 *   - MPIDR -> register id: CPUs 0-3 are MPIDR 0x000/0x100/0x200/0x300
 *     (cluster 0, ids 0-3); CPUs 4-5 are 0x10200/0x10300 (cluster 1,
 *     Aff1 2/3, ids 6-7). A wrong id would write another core's NDIV,
 *     and a wrong offset can hit an unbacked word (+4) that SErrors
 *     into EL3.
 *   - NDIV -> kHz: 12.8 MHz per step. Linux showed NDIV 57 = 729.6 MHz,
 *     87 = 1113.6, 105 = 1344; post-kexec CPUs 4-5 read NDIV 9 = 115.2.
 *   - Target selection: only an in-range boot-CPU NDIV is copied.
 */
#include "unity.h"
#include "../include/tegra_cpufreq.h"
#include <stdint.h>

static void test_reg_id_cluster0(void)
{
    TEST_ASSERT_EQUAL_INT(0, tegra234_cpufreq_reg_id(0x000));
    TEST_ASSERT_EQUAL_INT(1, tegra234_cpufreq_reg_id(0x100));
    TEST_ASSERT_EQUAL_INT(2, tegra234_cpufreq_reg_id(0x200));
    TEST_ASSERT_EQUAL_INT(3, tegra234_cpufreq_reg_id(0x300));
}

static void test_reg_id_cluster1_jetson_cpus(void)
{
    /* Jetson CPUs 4-5: cluster 1, Aff1 2 and 3 (not 0 and 1). */
    TEST_ASSERT_EQUAL_INT(6, tegra234_cpufreq_reg_id(0x10200));
    TEST_ASSERT_EQUAL_INT(7, tegra234_cpufreq_reg_id(0x10300));
    /* MT bit / Aff3 / U bit above Aff2 don't change the id. */
    TEST_ASSERT_EQUAL_INT(7, tegra234_cpufreq_reg_id(0x81010300ULL));
}

static void test_reg_id_rejects_out_of_layout(void)
{
    TEST_ASSERT_EQUAL_INT(8, tegra234_cpufreq_reg_id(0x20000));  /* cluster 2 exists */
    TEST_ASSERT_EQUAL_INT(-1, tegra234_cpufreq_reg_id(0x30000)); /* no cluster 3 */
    TEST_ASSERT_EQUAL_INT(-1, tegra234_cpufreq_reg_id(0x400));   /* Aff1 4 */
}

static void test_reg_offset_stride_8(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x2000, tegra234_cpufreq_reg_offset(0));
    TEST_ASSERT_EQUAL_UINT32(0x2030, tegra234_cpufreq_reg_offset(6));
    TEST_ASSERT_EQUAL_UINT32(0x2038, tegra234_cpufreq_reg_offset(7));
    /* Never lands on the unbacked +4 words. */
    for (uint32_t id = 0; id < 12; id++) {
        TEST_ASSERT_EQUAL_UINT32(0, tegra234_cpufreq_reg_offset(id) % 8);
    }
}

static void test_ndiv_to_khz_matches_linux(void)
{
    TEST_ASSERT_EQUAL_UINT32(115200, tegra234_ndiv_to_khz(9));
    TEST_ASSERT_EQUAL_UINT32(729600, tegra234_ndiv_to_khz(57));
    TEST_ASSERT_EQUAL_UINT32(1113600, tegra234_ndiv_to_khz(87));
    TEST_ASSERT_EQUAL_UINT32(1344000, tegra234_ndiv_to_khz(105));
    TEST_ASSERT_EQUAL_UINT32(1728000, tegra234_ndiv_to_khz(135));
    /* Bits above the 9-bit NDIV field are ignored. */
    TEST_ASSERT_EQUAL_UINT32(1344000, tegra234_ndiv_to_khz(0x1000 | 105));
}

static void test_sync_target_copies_in_range_only(void)
{
    TEST_ASSERT_EQUAL_UINT32(105, tegra234_cpufreq_sync_target(105));
    TEST_ASSERT_EQUAL_UINT32(9, tegra234_cpufreq_sync_target(9));
    TEST_ASSERT_EQUAL_UINT32(135, tegra234_cpufreq_sync_target(135));
    TEST_ASSERT_EQUAL_UINT32(105, tegra234_cpufreq_sync_target(0x1000 | 105));
    TEST_ASSERT_EQUAL_UINT32(0, tegra234_cpufreq_sync_target(0));
    TEST_ASSERT_EQUAL_UINT32(0, tegra234_cpufreq_sync_target(8));
    TEST_ASSERT_EQUAL_UINT32(0, tegra234_cpufreq_sync_target(136));
    TEST_ASSERT_EQUAL_UINT32(0, tegra234_cpufreq_sync_target(0x1FF));
}

int test_suite_tegra_cpufreq(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reg_id_cluster0);
    RUN_TEST(test_reg_id_cluster1_jetson_cpus);
    RUN_TEST(test_reg_id_rejects_out_of_layout);
    RUN_TEST(test_reg_offset_stride_8);
    RUN_TEST(test_ndiv_to_khz_matches_linux);
    RUN_TEST(test_sync_target_copies_in_range_only);
    return UNITY_END();
}
