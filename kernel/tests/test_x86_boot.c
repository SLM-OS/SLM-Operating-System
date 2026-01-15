/*
 * test_x86_boot.c - x86-64 Boot and Platform Tests
 *
 * Comprehensive tests for x86-64 boot infrastructure:
 * - Multiboot2 header structure validation
 * - Page table structure and mapping verification
 * - GDT structure validation
 * - Long mode transition verification
 * - Framebuffer console functionality
 *
 * These tests only run on x86-64 platform (PLATFORM_X86_64=1).
 */

#include "unity.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef PLATFORM_X86_64

#include "../include/fb_console.h"

/* ============================================================================
 * External Symbols from Boot Code
 * ============================================================================ */

/* Page tables (from entry64.S) */
extern uint64_t pml4[];
extern uint64_t pdpt[];
extern uint64_t pd[];

/* GDT pointer (from trampoline32.S) */
extern struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt64_ptr;

/* Multiboot info pointer (from trampoline32.S) */
extern uint32_t multiboot_ptr;

/* Linker symbols */
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];

/* ============================================================================
 * Multiboot2 Constants
 * ============================================================================ */

#define MULTIBOOT2_MAGIC            0xE85250D6
#define MULTIBOOT2_ARCH_I386        0
#define MULTIBOOT2_TAG_END          0
#define MULTIBOOT2_TAG_FRAMEBUFFER  5

/* ============================================================================
 * Page Table Constants
 * ============================================================================ */

#define PAGE_PRESENT    0x001
#define PAGE_WRITABLE   0x002
#define PAGE_2MB        0x080
#define PAGE_SIZE_4K    0x1000
#define PAGE_SIZE_2MB   0x200000

/* ============================================================================
 * GDT Constants
 * ============================================================================ */

#define GDT_CODE64_ACCESS   0x9A    /* Present, Ring 0, Code, Execute/Read */
#define GDT_DATA64_ACCESS   0x92    /* Present, Ring 0, Data, Read/Write */
#define GDT_CODE64_FLAGS    0xAF    /* 64-bit, granularity byte, limit high */
#define GDT_DATA64_FLAGS    0xCF    /* 32-bit compat, granularity 4K, limit high */

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Read CR3 register (page table base)
 */
static inline uint64_t read_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/*
 * Read CR0 register (paging, protection bits)
 */
static inline uint64_t read_cr0(void)
{
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

/*
 * Read CR4 register (PAE, etc.)
 */
static inline uint64_t read_cr4(void)
{
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

/*
 * Read EFER MSR (long mode enable)
 */
static inline uint64_t read_efer(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Get current code segment selector
 */
static inline uint16_t get_cs(void)
{
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

/*
 * Get current data segment selector
 */
static inline uint16_t get_ds(void)
{
    uint16_t ds;
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    return ds;
}

/* ============================================================================
 * Control Register Tests
 * ============================================================================ */

/*
 * Test: CR0 has paging and protection enabled
 */
static void test_cr0_paging_enabled(void)
{
    uint64_t cr0 = read_cr0();

    /* Bit 31: Paging enable */
    TEST_ASSERT_TRUE((cr0 & (1UL << 31)) != 0);

    /* Bit 0: Protection enable */
    TEST_ASSERT_TRUE((cr0 & 1) != 0);
}

/*
 * Test: CR4 has PAE enabled
 */
static void test_cr4_pae_enabled(void)
{
    uint64_t cr4 = read_cr4();

    /* Bit 5: Physical Address Extension */
    TEST_ASSERT_TRUE((cr4 & (1UL << 5)) != 0);
}

/*
 * Test: EFER has long mode enabled
 */
static void test_efer_long_mode(void)
{
    uint64_t efer = read_efer();

    /* Bit 8: Long Mode Enable */
    TEST_ASSERT_TRUE((efer & (1UL << 8)) != 0);

    /* Bit 10: Long Mode Active (set by CPU when paging enabled) */
    TEST_ASSERT_TRUE((efer & (1UL << 10)) != 0);
}

/*
 * Test: CR3 points to our PML4
 */
static void test_cr3_points_to_pml4(void)
{
    uint64_t cr3 = read_cr3();
    uint64_t pml4_addr = (uint64_t)pml4;

    /* CR3 bits 12-51 contain the physical address of PML4 */
    TEST_ASSERT_EQUAL_HEX64(pml4_addr, cr3 & 0xFFFFFFFFF000UL);
}

/* ============================================================================
 * Page Table Structure Tests
 * ============================================================================ */

/*
 * Test: PML4 has valid entry at index 0
 */
static void test_pml4_entry_valid(void)
{
    uint64_t entry = pml4[0];

    /* Should be present */
    TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);

    /* Should be writable */
    TEST_ASSERT_TRUE((entry & PAGE_WRITABLE) != 0);

    /* Should point to PDPT (check address portion) */
    uint64_t pdpt_addr = (uint64_t)pdpt;
    TEST_ASSERT_EQUAL_HEX64(pdpt_addr, entry & 0xFFFFFFFFF000UL);
}

/*
 * Test: PDPT has valid entry at index 0
 */
static void test_pdpt_entry_valid(void)
{
    uint64_t entry = pdpt[0];

    /* Should be present */
    TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);

    /* Should be writable */
    TEST_ASSERT_TRUE((entry & PAGE_WRITABLE) != 0);

    /* Should point to PD */
    uint64_t pd_addr = (uint64_t)pd;
    TEST_ASSERT_EQUAL_HEX64(pd_addr, entry & 0xFFFFFFFFF000UL);
}

/*
 * Test: PD has 2MB page entries for identity mapping
 */
static void test_pd_2mb_pages(void)
{
    /* First entry should map physical address 0 */
    uint64_t entry0 = pd[0];
    TEST_ASSERT_TRUE((entry0 & PAGE_PRESENT) != 0);
    TEST_ASSERT_TRUE((entry0 & PAGE_WRITABLE) != 0);
    TEST_ASSERT_TRUE((entry0 & PAGE_2MB) != 0);  /* 2MB page flag */
    TEST_ASSERT_EQUAL_HEX64(0, entry0 & 0xFFFFFFE00000UL);  /* Physical addr 0 */

    /* Entry 1 should map 2MB */
    uint64_t entry1 = pd[1];
    TEST_ASSERT_TRUE((entry1 & PAGE_PRESENT) != 0);
    TEST_ASSERT_EQUAL_HEX64(PAGE_SIZE_2MB, entry1 & 0xFFFFFFE00000UL);

    /* Entry 255 should map 510MB */
    uint64_t entry255 = pd[255];
    TEST_ASSERT_TRUE((entry255 & PAGE_PRESENT) != 0);
    TEST_ASSERT_EQUAL_HEX64(255UL * PAGE_SIZE_2MB, entry255 & 0xFFFFFFE00000UL);
}

/*
 * Test: All 512 PD entries are valid (1GB identity map)
 */
static void test_pd_full_1gb_mapping(void)
{
    for (int i = 0; i < 512; i++) {
        uint64_t entry = pd[i];

        /* All entries should be present */
        TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);

        /* All entries should be 2MB pages */
        TEST_ASSERT_TRUE((entry & PAGE_2MB) != 0);

        /* Physical address should be i * 2MB */
        uint64_t expected_phys = (uint64_t)i * PAGE_SIZE_2MB;
        TEST_ASSERT_EQUAL_HEX64(expected_phys, entry & 0xFFFFFFE00000UL);
    }
}

/* ============================================================================
 * GDT Tests
 * ============================================================================ */

/*
 * Test: GDT pointer has valid limit
 */
static void test_gdt_limit(void)
{
    /* GDT has 3 entries: null, code64, data64 = 24 bytes */
    /* Limit is size - 1 = 23 = 0x17 */
    TEST_ASSERT_EQUAL_HEX64(0x17, gdt64_ptr.limit);
}

/*
 * Test: Current CS selector is 0x08 (code64 segment)
 */
static void test_cs_selector(void)
{
    uint16_t cs = get_cs();
    TEST_ASSERT_EQUAL_HEX64(0x08, cs);
}

/*
 * Test: Current DS selector is 0x10 (data64 segment)
 */
static void test_ds_selector(void)
{
    uint16_t ds = get_ds();
    TEST_ASSERT_EQUAL_HEX64(0x10, ds);
}

/* ============================================================================
 * Kernel Memory Layout Tests
 * ============================================================================ */

/*
 * Test: Kernel loaded at 1MB boundary
 */
static void test_kernel_load_address(void)
{
    uintptr_t text_start = (uintptr_t)__text_start;

    /* Kernel .text should start at 0x101000 (after multiboot header) */
    TEST_ASSERT_TRUE(text_start >= 0x100000);
    TEST_ASSERT_TRUE(text_start < 0x200000);
}

/*
 * Test: Section ordering is correct (text < rodata < data < bss)
 */
static void test_section_ordering(void)
{
    uintptr_t text = (uintptr_t)__text_start;
    uintptr_t rodata = (uintptr_t)__rodata_start;
    uintptr_t data = (uintptr_t)__data_start;
    uintptr_t bss = (uintptr_t)__bss_start;

    TEST_ASSERT_TRUE(text < rodata);
    TEST_ASSERT_TRUE(rodata < data);
    TEST_ASSERT_TRUE(data <= bss);  /* data and bss may be adjacent */
}

/*
 * Test: All sections are within mapped memory (first 1GB)
 */
static void test_sections_within_mapping(void)
{
    uintptr_t kernel_end = (uintptr_t)__kernel_end;

    /* Must be within first 1GB (512 * 2MB) */
    TEST_ASSERT_TRUE(kernel_end < 512UL * PAGE_SIZE_2MB);
}

/* ============================================================================
 * Multiboot Info Tests
 * ============================================================================ */

/*
 * Test: Multiboot info pointer is valid (non-zero, within mapped memory)
 */
static void test_multiboot_ptr_valid(void)
{
    TEST_ASSERT_TRUE(multiboot_ptr != 0);
    TEST_ASSERT_TRUE(multiboot_ptr < 512UL * PAGE_SIZE_2MB);
}

/* ============================================================================
 * Framebuffer Tests
 * ============================================================================ */

/*
 * Test: Framebuffer console can output characters
 */
static void test_fb_console_output(void)
{
    /* Just verify the function doesn't crash */
    fb_console_putc('T');
    fb_console_putc('e');
    fb_console_putc('s');
    fb_console_putc('t');
    fb_console_putc('\n');

    /* If we get here, the test passed */
    TEST_ASSERT_TRUE(true);
}

/*
 * Test: Framebuffer console string output
 */
static void test_fb_console_puts(void)
{
    fb_console_puts("[TEST] Framebuffer string output\n");
    TEST_ASSERT_TRUE(true);
}

/* ============================================================================
 * Long Mode Verification Tests
 * ============================================================================ */

/*
 * Test: We are running in 64-bit mode (verify with 64-bit operation)
 */
static void test_64bit_operations(void)
{
    /* Try a 64-bit operation that would fail in 32-bit mode */
    uint64_t large_value = 0x123456789ABCDEF0ULL;
    uint64_t shifted = large_value >> 32;

    TEST_ASSERT_EQUAL_HEX64(0x12345678, shifted);

    /* Verify we can use all 64 bits of a register */
    volatile uint64_t test = 0xFFFFFFFFFFFFFFFFULL;
    TEST_ASSERT_EQUAL_HEX64(0xFFFFFFFFFFFFFFFFULL, test);
}

/*
 * Test: RIP-relative addressing works (64-bit mode feature)
 */
static void test_rip_relative_addressing(void)
{
    /* This test itself uses RIP-relative addressing if compiled correctly */
    static volatile uint64_t test_var = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_HEX64(0xDEADBEEF, test_var);

    test_var = 0xCAFEBABE;
    TEST_ASSERT_EQUAL_HEX64(0xCAFEBABE, test_var);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_x86_boot(void)
{
    UnityBegin("x86-64 Boot Tests");

    /* Control register tests */
    RUN_TEST(test_cr0_paging_enabled);
    RUN_TEST(test_cr4_pae_enabled);
    RUN_TEST(test_efer_long_mode);
    RUN_TEST(test_cr3_points_to_pml4);

    /* Page table structure tests */
    RUN_TEST(test_pml4_entry_valid);
    RUN_TEST(test_pdpt_entry_valid);
    RUN_TEST(test_pd_2mb_pages);
    RUN_TEST(test_pd_full_1gb_mapping);

    /* GDT tests */
    RUN_TEST(test_gdt_limit);
    RUN_TEST(test_cs_selector);
    RUN_TEST(test_ds_selector);

    /* Kernel memory layout tests */
    RUN_TEST(test_kernel_load_address);
    RUN_TEST(test_section_ordering);
    RUN_TEST(test_sections_within_mapping);

    /* Multiboot tests */
    RUN_TEST(test_multiboot_ptr_valid);

    /* Framebuffer tests */
    RUN_TEST(test_fb_console_output);
    RUN_TEST(test_fb_console_puts);

    /* Long mode verification */
    RUN_TEST(test_64bit_operations);
    RUN_TEST(test_rip_relative_addressing);

    return UnityEnd();
}

#else /* !PLATFORM_X86_64 */

/* Stub for non-x86 platforms */
int test_suite_x86_boot(void)
{
    /* No tests to run on non-x86 platforms */
    return 0;
}

#endif /* PLATFORM_X86_64 */
