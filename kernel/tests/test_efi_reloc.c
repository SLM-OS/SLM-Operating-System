/*
 * test_efi_reloc.c - efi_check_relocation() decision tests (#26)
 *
 * UEFI-direct boot loads the PE image wherever the firmware likes; the
 * EFI stub then decides, from the UEFI memory map, whether boot.S may
 * copy the kernel to its link address after ExitBootServices. A wrong
 * "OK" overwrites firmware runtime memory, the DTB or the copy loop
 * itself, and the board dies with no console output, so the decision
 * is pinned here with synthetic maps.
 *
 * The maps below use the layout seen on jetson-nano-2 (UEFI 36.4.7):
 * link base 0x80000000 inside a ConventionalMemory block, image loaded
 * as LoaderCode near 0x25C870000, DTB in BootServicesData.
 *
 * Built for every ARM64 platform that compiles efi_stub.c (not Pi 5,
 * which has no PE header).
 */

#include "unity.h"
#include "../arch/arm64/efi.h"
#include "../include/boot_source.h"
#include <stdint.h>
#include <stddef.h>

#define LINK   0x80000000ULL
#define SIZE   0x01950000ULL           /* SizeOfImage of a Jetson build */
#define LOAD   0x25C870000ULL
#define FDT    0x2679DF000ULL
#define FDTSZ  0x000F39DCULL
#define PAGES(bytes) ((bytes) / EFI_PAGE_SIZE)

static efi_memory_desc_t desc(uint32_t type, uint64_t start, uint64_t bytes)
{
    efi_memory_desc_t d = {
        .type = type, .pad = 0, .phys_start = start,
        .virt_start = 0, .num_pages = PAGES(bytes), .attribute = 0,
    };
    return d;
}

static efi_reloc_verdict_t check(const efi_memory_desc_t *map, size_t n,
                                 uint64_t src, uint64_t dst,
                                 uint64_t fdt)
{
    return efi_check_relocation(map, n * sizeof(*map), sizeof(*map),
                                src, dst, SIZE, fdt, FDTSZ);
}

/* The jetson-nano-2 case: free destination, image and DTB far away. */
static void test_reloc_ok_conventional_dest(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x3E000000ULL),
        desc(EFI_RUNTIME_SERVICES_DATA, 0xFFFE0000ULL, 0x20000ULL),
        desc(EFI_LOADER_CODE, LOAD - 0xD000, SIZE + 0x10000),
        desc(EFI_BOOT_SERVICES_DATA, 0x266CAC000ULL, 0x3CA4000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OK, check(map, 4, LOAD, LINK, FDT));
}

/* Firmware honoured ImageBase: nothing to copy, map is irrelevant. */
static void test_reloc_not_needed_at_link(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_LOADER_CODE, LINK, SIZE),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_NOT_NEEDED,
                          check(map, 1, LINK, LINK, FDT));
}

/* Boot-services and loader memory become the OS's after EBS. */
static void test_reloc_ok_bootservices_and_loader_mix(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_BOOT_SERVICES_CODE, LINK, 0x400000ULL),
        desc(EFI_BOOT_SERVICES_DATA, LINK + 0x400000ULL, 0x400000ULL),
        desc(EFI_LOADER_DATA, LINK + 0x800000ULL, 0x100000ULL),
        desc(EFI_CONVENTIONAL_MEMORY, LINK + 0x900000ULL, 0x2000000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OK, check(map, 4, LOAD, LINK, FDT));
}

/* Runtime-services memory in the destination must survive EBS. */
static void test_reloc_rejects_runtime_in_dest(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x800000ULL),
        desc(EFI_RUNTIME_SERVICES_CODE, LINK + 0x800000ULL, 0x10000ULL),
        desc(EFI_CONVENTIONAL_MEMORY, LINK + 0x810000ULL, 0x2000000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_DEST_BUSY,
                          check(map, 3, LOAD, LINK, FDT));
}

/* Reserved / ACPI / MMIO are equally off-limits. */
static void test_reloc_rejects_reserved_acpi_mmio(void)
{
    static const uint32_t bad[] = {
        EFI_RESERVED_MEMORY_TYPE, EFI_RUNTIME_SERVICES_DATA,
        EFI_UNUSABLE_MEMORY, EFI_ACPI_RECLAIM_MEMORY, EFI_ACPI_MEMORY_NVS,
        EFI_MEMORY_MAPPED_IO, EFI_PERSISTENT_MEMORY,
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        efi_memory_desc_t map[] = {
            desc(bad[i], LINK + SIZE - 0x1000ULL, 0x1000ULL),
            desc(EFI_CONVENTIONAL_MEMORY, LINK, SIZE - 0x1000ULL),
        };
        TEST_ASSERT_EQUAL_INT(EFI_RELOC_DEST_BUSY,
                              check(map, 2, LOAD, LINK, FDT));
    }
}

/* A hole in the map (e.g. the OP-TEE carveout) is not RAM. */
static void test_reloc_rejects_hole_in_dest(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x1000000ULL),
        /* gap of 0x10000 */
        desc(EFI_CONVENTIONAL_MEMORY, LINK + 0x1010000ULL, 0x2000000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_DEST_NOT_RAM,
                          check(map, 2, LOAD, LINK, FDT));
}

/* Destination running off the end of the last descriptor. */
static void test_reloc_rejects_dest_past_end_of_ram(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, SIZE - 0x10000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_DEST_NOT_RAM,
                          check(map, 1, LOAD, LINK, FDT));
}

/* Overlapping load/link ranges in either direction are refused: the
 * copy loop executes from the source and could overwrite itself. */
static void test_reloc_rejects_overlap(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x4000000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OVERLAP,
                          check(map, 1, LINK + 0x100000ULL, LINK, FDT));
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OVERLAP,
                          check(map, 1, LINK - 0x100000ULL, LINK, FDT));
    /* Exactly adjacent is fine. */
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OK,
                          check(map, 1, LINK + SIZE, LINK, FDT));
}

/* The copy must not overwrite the DTB before kernel_main parses it. */
static void test_reloc_rejects_fdt_in_dest(void)
{
    efi_memory_desc_t map[] = {
        desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x4000000ULL),
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_FDT_IN_DEST,
                          check(map, 1, LOAD, LINK, LINK + 0x200000ULL));
    /* DTB straddling the end of the destination still counts. */
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_FDT_IN_DEST,
                          check(map, 1, LOAD, LINK, LINK + SIZE - 0x1000ULL));
    /* No DTB (ACPI firmware) is not an error. */
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_OK, check(map, 1, LOAD, LINK, 0));
}

/* Firmware descriptors are often larger than the spec struct; the
 * walk must use desc_size as its stride, and refuse a smaller one. */
static void test_reloc_honours_desc_size_stride(void)
{
    struct wide_desc {
        efi_memory_desc_t d;
        uint64_t vendor_extra;
    } map[2] = {
        { desc(EFI_CONVENTIONAL_MEMORY, LINK, 0x1000000ULL), 0xFFFFFFFFULL },
        { desc(EFI_RUNTIME_SERVICES_DATA, LINK + 0x1000000ULL, 0x10000ULL),
          0 },
    };
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_DEST_BUSY,
        efi_check_relocation(&map[0].d, sizeof(map), sizeof(map[0]),
                             LOAD, LINK, SIZE, FDT, FDTSZ));
    TEST_ASSERT_EQUAL_INT(EFI_RELOC_BAD_MAP,
        efi_check_relocation(&map[0].d, sizeof(map), 16,
                             LOAD, LINK, SIZE, FDT, FDTSZ));
}

static void test_mem_type_free_after_ebs(void)
{
    TEST_ASSERT_TRUE(efi_mem_type_free_after_ebs(EFI_LOADER_CODE));
    TEST_ASSERT_TRUE(efi_mem_type_free_after_ebs(EFI_LOADER_DATA));
    TEST_ASSERT_TRUE(efi_mem_type_free_after_ebs(EFI_BOOT_SERVICES_CODE));
    TEST_ASSERT_TRUE(efi_mem_type_free_after_ebs(EFI_BOOT_SERVICES_DATA));
    TEST_ASSERT_TRUE(efi_mem_type_free_after_ebs(EFI_CONVENTIONAL_MEMORY));
    TEST_ASSERT_FALSE(efi_mem_type_free_after_ebs(EFI_RESERVED_MEMORY_TYPE));
    TEST_ASSERT_FALSE(efi_mem_type_free_after_ebs(EFI_RUNTIME_SERVICES_CODE));
    TEST_ASSERT_FALSE(efi_mem_type_free_after_ebs(EFI_RUNTIME_SERVICES_DATA));
    TEST_ASSERT_FALSE(efi_mem_type_free_after_ebs(EFI_ACPI_RECLAIM_MEMORY));
    TEST_ASSERT_FALSE(efi_mem_type_free_after_ebs(EFI_MEMORY_MAPPED_IO));
}

/* The test kernel is loaded with QEMU -kernel, not through the EFI
 * stub, so the boot source must still read DIRECT. A UEFI value here
 * would mean the default was lost (e.g. the variable moved to .bss and
 * a stray store set it) and xhci_init would wrongly skip USB on kexec
 * boots. The UEFI side is checked by `make test-qemu-uefi`, which
 * looks for "Boot source: UEFI-direct" on the console. */
static void test_boot_source_default_is_direct(void)
{
    TEST_ASSERT_EQUAL_UINT32(BOOT_SOURCE_DIRECT, slmos_boot_source);
    TEST_ASSERT_FALSE(boot_source_is_uefi());
}

int test_suite_efi_reloc(void)
{
    UnityBegin("test_efi_reloc.c");
    RUN_TEST(test_reloc_ok_conventional_dest);
    RUN_TEST(test_reloc_not_needed_at_link);
    RUN_TEST(test_reloc_ok_bootservices_and_loader_mix);
    RUN_TEST(test_reloc_rejects_runtime_in_dest);
    RUN_TEST(test_reloc_rejects_reserved_acpi_mmio);
    RUN_TEST(test_reloc_rejects_hole_in_dest);
    RUN_TEST(test_reloc_rejects_dest_past_end_of_ram);
    RUN_TEST(test_reloc_rejects_overlap);
    RUN_TEST(test_reloc_rejects_fdt_in_dest);
    RUN_TEST(test_reloc_honours_desc_size_stride);
    RUN_TEST(test_mem_type_free_after_ebs);
    RUN_TEST(test_boot_source_default_is_direct);
    return UnityEnd();
}
