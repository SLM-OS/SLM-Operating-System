/*
 * test_dtb.c - DTB parser regression tests
 *
 * Covers dtb_validate() bounds handling against crafted headers
 * (BOOT-H1 in docs/code-review-2026-04-12.md).
 */

#include "unity.h"
#include "../include/dtb.h"
#include <stdint.h>

/* Build a DTB header in-place using big-endian stores (DTB is always BE). */
static inline uint32_t cpu_to_be32(uint32_t v)
{
    return ((v & 0xFFU) << 24) |
           ((v & 0xFF00U) << 8) |
           ((v & 0xFF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static void fill_header(struct fdt_header *hdr,
                        uint32_t magic,
                        uint32_t version,
                        uint32_t off_dt_struct,
                        uint32_t size_dt_struct,
                        uint32_t totalsize)
{
    hdr->magic             = cpu_to_be32(magic);
    hdr->totalsize         = cpu_to_be32(totalsize);
    hdr->off_dt_struct     = cpu_to_be32(off_dt_struct);
    hdr->off_dt_strings    = cpu_to_be32(0);
    hdr->off_mem_rsvmap    = cpu_to_be32(0);
    hdr->version           = cpu_to_be32(version);
    hdr->last_comp_version = cpu_to_be32(version);
    hdr->boot_cpuid_phys   = cpu_to_be32(0);
    hdr->size_dt_strings   = cpu_to_be32(0);
    hdr->size_dt_struct    = cpu_to_be32(size_dt_struct);
}

static void test_dtb_validate_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADPTR, dtb_validate(NULL));
}

static void test_dtb_validate_rejects_bad_magic(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, 0xDEADBEEFU, FDT_VERSION, 64, 16, 1024);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADMAGIC, dtb_validate(&hdr));
}

static void test_dtb_validate_rejects_old_version(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION - 1, 64, 16, 1024);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADVERSION, dtb_validate(&hdr));
}

static void test_dtb_validate_accepts_well_formed(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION, 64, 128, 512);
    TEST_ASSERT_EQUAL_INT(FDT_OK, dtb_validate(&hdr));
}

/* BOOT-H1 regression: struct block larger than totalsize must be rejected. */
static void test_dtb_validate_rejects_struct_past_total(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION,
                /*off*/ 64,
                /*size*/ 1024,
                /*total*/ 512);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADSTRUCT, dtb_validate(&hdr));
}

/* BOOT-H1 regression: off + size overflow must be rejected. */
static void test_dtb_validate_rejects_overflow(void)
{
    struct fdt_header hdr;
    fill_header(&hdr, FDT_MAGIC, FDT_VERSION,
                /*off*/ 0xFFFFFFF0U,
                /*size*/ 0x00000100U,
                /*total*/ 0xFFFFFFFFU);
    TEST_ASSERT_EQUAL_INT(FDT_ERR_BADSTRUCT, dtb_validate(&hdr));
}

int test_suite_dtb(void)
{
    UnityBegin("DTB Parser Tests");

    RUN_TEST(test_dtb_validate_rejects_null);
    RUN_TEST(test_dtb_validate_rejects_bad_magic);
    RUN_TEST(test_dtb_validate_rejects_old_version);
    RUN_TEST(test_dtb_validate_accepts_well_formed);
    RUN_TEST(test_dtb_validate_rejects_struct_past_total);
    RUN_TEST(test_dtb_validate_rejects_overflow);

    return UnityEnd();
}
