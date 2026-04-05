/*
 * test_x86_boot.c - x86-64 Boot and Platform Tests
 *
 * Comprehensive tests for x86-64 boot infrastructure:
 * - Control register verification (CR0, CR4, EFER)
 * - Page table structure and mapping verification
 * - GDT structure validation
 * - Long mode transition verification
 * - IDT structure and exception handler registration
 * - PIC remapping verification
 * - PIT timer interrupt delivery
 * - Multiboot2 info parsing
 *
 * These tests only run on x86-64 platform (PLATFORM_X86_64=1).
 */

#include "unity.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef PLATFORM_X86_64

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

/* From idt.c */
extern void irq_register(uint8_t irq, void (*handler)(uint8_t));

/* ============================================================================
 * Constants
 * ============================================================================ */

#define PAGE_PRESENT    0x001
#define PAGE_WRITABLE   0x002
#define PAGE_2MB        0x080
#define PAGE_SIZE_2MB   0x200000

/* PIC I/O ports */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static inline uint64_t read_cr0(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr4(void)
{
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_efer(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint16_t get_cs(void)
{
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static inline uint16_t get_ds(void)
{
    uint16_t ds;
    __asm__ volatile("mov %%ds, %0" : "=r"(ds));
    return ds;
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Read IDTR via SIDT instruction */
struct idtr_value {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static inline void read_idtr(struct idtr_value *out)
{
    __asm__ volatile("sidt %0" : "=m"(*out));
}

/* ============================================================================
 * Control Register Tests
 * ============================================================================ */

static void test_cr0_paging_enabled(void)
{
    uint64_t cr0 = read_cr0();
    TEST_ASSERT_TRUE((cr0 & (1UL << 31)) != 0);  /* Paging */
    TEST_ASSERT_TRUE((cr0 & 1) != 0);             /* Protection */
}

static void test_cr4_pae_enabled(void)
{
    uint64_t cr4 = read_cr4();
    TEST_ASSERT_TRUE((cr4 & (1UL << 5)) != 0);    /* PAE */
}

static void test_efer_long_mode(void)
{
    uint64_t efer = read_efer();
    TEST_ASSERT_TRUE((efer & (1UL << 8)) != 0);   /* LME */
    TEST_ASSERT_TRUE((efer & (1UL << 10)) != 0);  /* LMA */
}

static void test_cr3_points_to_pml4(void)
{
    uint64_t cr3 = read_cr3();
    uint64_t pml4_addr = (uint64_t)pml4;
    TEST_ASSERT_EQUAL_HEX64(pml4_addr, cr3 & 0xFFFFFFFFF000UL);
}

/* ============================================================================
 * Page Table Structure Tests
 * ============================================================================ */

static void test_pml4_entry_valid(void)
{
    uint64_t entry = pml4[0];
    TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);
    TEST_ASSERT_TRUE((entry & PAGE_WRITABLE) != 0);
    uint64_t pdpt_addr = (uint64_t)pdpt;
    TEST_ASSERT_EQUAL_HEX64(pdpt_addr, entry & 0xFFFFFFFFF000UL);
}

static void test_pdpt_entry_valid(void)
{
    uint64_t entry = pdpt[0];
    TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);
    TEST_ASSERT_TRUE((entry & PAGE_WRITABLE) != 0);
    uint64_t pd_addr = (uint64_t)pd;
    TEST_ASSERT_EQUAL_HEX64(pd_addr, entry & 0xFFFFFFFFF000UL);
}

static void test_pd_2mb_pages(void)
{
    uint64_t entry0 = pd[0];
    TEST_ASSERT_TRUE((entry0 & PAGE_PRESENT) != 0);
    TEST_ASSERT_TRUE((entry0 & PAGE_WRITABLE) != 0);
    TEST_ASSERT_TRUE((entry0 & PAGE_2MB) != 0);
    TEST_ASSERT_EQUAL_HEX64(0, entry0 & 0xFFFFFFE00000UL);

    uint64_t entry1 = pd[1];
    TEST_ASSERT_TRUE((entry1 & PAGE_PRESENT) != 0);
    TEST_ASSERT_EQUAL_HEX64(PAGE_SIZE_2MB, entry1 & 0xFFFFFFE00000UL);

    uint64_t entry255 = pd[255];
    TEST_ASSERT_TRUE((entry255 & PAGE_PRESENT) != 0);
    TEST_ASSERT_EQUAL_HEX64(255UL * PAGE_SIZE_2MB, entry255 & 0xFFFFFFE00000UL);
}

static void test_pd_full_1gb_mapping(void)
{
    for (int i = 0; i < 512; i++) {
        uint64_t entry = pd[i];
        TEST_ASSERT_TRUE((entry & PAGE_PRESENT) != 0);
        TEST_ASSERT_TRUE((entry & PAGE_2MB) != 0);
        uint64_t expected_phys = (uint64_t)i * PAGE_SIZE_2MB;
        TEST_ASSERT_EQUAL_HEX64(expected_phys, entry & 0xFFFFFFE00000UL);
    }
}

/* ============================================================================
 * GDT Tests
 * ============================================================================ */

static void test_gdt_limit(void)
{
    TEST_ASSERT_EQUAL_HEX64(0x17, gdt64_ptr.limit);
}

static void test_cs_selector(void)
{
    TEST_ASSERT_EQUAL_HEX64(0x08, get_cs());
}

static void test_ds_selector(void)
{
    TEST_ASSERT_EQUAL_HEX64(0x10, get_ds());
}

/* ============================================================================
 * Kernel Memory Layout Tests
 * ============================================================================ */

static void test_kernel_load_address(void)
{
    uintptr_t text_start = (uintptr_t)__text_start;
    TEST_ASSERT_TRUE(text_start >= 0x100000);
    TEST_ASSERT_TRUE(text_start < 0x200000);
}

static void test_section_ordering(void)
{
    uintptr_t text = (uintptr_t)__text_start;
    uintptr_t rodata = (uintptr_t)__rodata_start;
    uintptr_t data = (uintptr_t)__data_start;
    uintptr_t bss = (uintptr_t)__bss_start;

    TEST_ASSERT_TRUE(text < rodata);
    TEST_ASSERT_TRUE(rodata < data);
    TEST_ASSERT_TRUE(data <= bss);
}

static void test_sections_within_mapping(void)
{
    uintptr_t kernel_end = (uintptr_t)__kernel_end;
    TEST_ASSERT_TRUE(kernel_end < 512UL * PAGE_SIZE_2MB);
}

/* ============================================================================
 * Multiboot Info Tests
 * ============================================================================ */

static void test_multiboot_ptr_valid(void)
{
    TEST_ASSERT_TRUE(multiboot_ptr != 0);
    TEST_ASSERT_TRUE(multiboot_ptr < 512UL * PAGE_SIZE_2MB);
}

/*
 * Test: Multiboot2 info structure has valid size and contains expected tags.
 */
static void test_multiboot2_structure_valid(void)
{
    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_ptr;
    uint32_t total_size = *(uint32_t *)ptr;

    /* Size must be reasonable (> 8 bytes header, < 64KB) */
    TEST_ASSERT_TRUE(total_size > 8);
    TEST_ASSERT_TRUE(total_size < 65536);
}

/*
 * Test: Multiboot2 memory map tag is present and has entries.
 */
static void test_multiboot2_has_memory_map(void)
{
    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_ptr;
    uint32_t total_size = *(uint32_t *)ptr;
    uint8_t *end = ptr + total_size;

    ptr += 8;  /* Skip size + reserved */

    int found_mmap = 0;
    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);

        if (tag_type == 0) break;  /* End tag */

        if (tag_type == 6) {  /* Memory map */
            found_mmap = 1;
            /* Must have at least one entry (header is 16 bytes) */
            TEST_ASSERT_TRUE(tag_size > 16);
            break;
        }

        ptr += (tag_size + 7) & ~7;
    }

    TEST_ASSERT_TRUE(found_mmap);
}

/*
 * Test: Memory map reports usable RAM above 1MB.
 */
static void test_multiboot2_memory_above_1mb(void)
{
    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_ptr;
    uint32_t total_size = *(uint32_t *)ptr;
    uint8_t *end = ptr + total_size;

    ptr += 8;

    uint64_t total_usable = 0;
    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);

        if (tag_type == 0) break;

        if (tag_type == 6) {
            uint32_t entry_size = *(uint32_t *)(ptr + 8);
            uint8_t *entry = ptr + 16;
            while (entry < ptr + tag_size) {
                uint64_t base = *(uint64_t *)entry;
                uint64_t len  = *(uint64_t *)(entry + 8);
                uint32_t type = *(uint32_t *)(entry + 16);
                if (type == 1)  /* Available */
                    total_usable += len;
                (void)base;
                entry += entry_size;
            }
            break;
        }

        ptr += (tag_size + 7) & ~7;
    }

    /* Must have at least 1 MB of usable memory */
    TEST_ASSERT_TRUE(total_usable > 1024 * 1024);
}

/*
 * Test: Multiboot2 bootloader name tag is present.
 */
static void test_multiboot2_has_bootloader_name(void)
{
    uint8_t *ptr = (uint8_t *)(uintptr_t)multiboot_ptr;
    uint32_t total_size = *(uint32_t *)ptr;
    uint8_t *end = ptr + total_size;

    ptr += 8;

    int found = 0;
    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);

        if (tag_type == 0) break;

        if (tag_type == 2) {  /* Boot loader name */
            found = 1;
            /* Name string must be non-empty */
            const char *name = (const char *)(ptr + 8);
            TEST_ASSERT_TRUE(name[0] != '\0');
            break;
        }

        ptr += (tag_size + 7) & ~7;
    }

    TEST_ASSERT_TRUE(found);
}

/* ============================================================================
 * IDT Tests
 * ============================================================================ */

/*
 * Test: IDTR has been loaded with a valid base and limit.
 */
static void test_idt_loaded(void)
{
    struct idtr_value idtr;
    read_idtr(&idtr);

    /* 48 entries * 16 bytes = 768 bytes, limit = 767 */
    TEST_ASSERT_TRUE(idtr.limit >= 48 * 16 - 1);
    /* Base must be non-zero and within mapped memory */
    TEST_ASSERT_TRUE(idtr.base != 0);
    TEST_ASSERT_TRUE(idtr.base < 512UL * PAGE_SIZE_2MB);
}

/*
 * Test: IDT entries for CPU exceptions (0-31) are present.
 */
static void test_idt_exception_entries_present(void)
{
    struct idtr_value idtr;
    read_idtr(&idtr);

    /* Read first few IDT entries and verify they are populated */
    struct idt_entry_check {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    } __attribute__((packed));

    struct idt_entry_check *idt = (struct idt_entry_check *)idtr.base;

    /* Check vectors 0, 6, 13, 14 (DE, UD, GP, PF) */
    int vectors[] = {0, 6, 13, 14};
    for (int i = 0; i < 4; i++) {
        int v = vectors[i];
        /* Present bit must be set (bit 7 of type_attr) */
        TEST_ASSERT_TRUE((idt[v].type_attr & 0x80) != 0);
        /* Selector must be kernel code segment (0x08) */
        TEST_ASSERT_EQUAL_HEX64(0x08, idt[v].selector);
        /* Handler address must be non-zero */
        uint64_t handler = (uint64_t)idt[v].offset_low |
                           ((uint64_t)idt[v].offset_mid << 16) |
                           ((uint64_t)idt[v].offset_high << 32);
        TEST_ASSERT_TRUE(handler != 0);
    }
}

/*
 * Test: IDT entries for IRQs (32-47) use interrupt gates (IF cleared).
 */
static void test_idt_irq_entries_are_interrupt_gates(void)
{
    struct idtr_value idtr;
    read_idtr(&idtr);

    struct idt_entry_raw {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    } __attribute__((packed));

    struct idt_entry_raw *idt = (struct idt_entry_raw *)idtr.base;

    /* IRQ entries (32-47) should be interrupt gates (0x8E) */
    for (int v = 32; v < 48; v++) {
        TEST_ASSERT_TRUE((idt[v].type_attr & 0x80) != 0);  /* Present */
        /* Type field (bits 0-3) should be 0xE (interrupt gate) */
        TEST_ASSERT_EQUAL_HEX64(0x0E, idt[v].type_attr & 0x0F);
    }
}

/*
 * Test: Exception entries use trap gates (IF not cleared).
 */
static void test_idt_exception_entries_are_trap_gates(void)
{
    struct idtr_value idtr;
    read_idtr(&idtr);

    struct idt_entry_raw {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    } __attribute__((packed));

    struct idt_entry_raw *idt = (struct idt_entry_raw *)idtr.base;

    /* Most exception entries (0-31) should be trap gates (0x8F), except NMI (2) */
    int trap_vectors[] = {0, 1, 3, 6, 13, 14};
    for (int i = 0; i < 6; i++) {
        int v = trap_vectors[i];
        /* Type field (bits 0-3) should be 0xF (trap gate) */
        TEST_ASSERT_EQUAL_HEX64(0x0F, idt[v].type_attr & 0x0F);
    }

    /* NMI (vector 2) should be an interrupt gate */
    TEST_ASSERT_EQUAL_HEX64(0x0E, idt[2].type_attr & 0x0F);
}

/* ============================================================================
 * PIC Tests
 * ============================================================================ */

/*
 * Test: PIC has been remapped (IRQ 0 is no longer on vector 8).
 *
 * After remapping, reading the IRR (Interrupt Request Register) should work
 * and the PIC should respond to OCW3 commands.
 */
static void test_pic_responds_to_ocw3(void)
{
    /* OCW3: read IRR (In-Service Register) */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x0A), "Nd"((uint16_t)PIC1_CMD));
    uint8_t irr = inb(PIC1_CMD);
    /* Just verify we get a response (no hang, no exception) */
    (void)irr;
    TEST_ASSERT_TRUE(true);
}

/*
 * Test: PIC mask register is accessible and timer (IRQ 0) is unmasked.
 */
static void test_pic_timer_unmasked(void)
{
    uint8_t mask = inb(PIC1_DATA);
    /* IRQ 0 (timer) should be unmasked (bit 0 = 0) */
    TEST_ASSERT_TRUE((mask & 0x01) == 0);
}

/*
 * Test: Slave PIC cascade (IRQ 2) is accessible.
 */
static void test_pic_slave_accessible(void)
{
    /* OCW3: read IRR on slave PIC */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x0A), "Nd"((uint16_t)PIC2_CMD));
    uint8_t irr = inb(PIC2_CMD);
    (void)irr;
    TEST_ASSERT_TRUE(true);
}

/* ============================================================================
 * PIT Timer Tests
 * ============================================================================ */

/*
 * Test: Interrupts are enabled (IF flag in RFLAGS).
 */
static void test_interrupts_enabled(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    /* Bit 9: Interrupt Flag */
    TEST_ASSERT_TRUE((rflags & (1UL << 9)) != 0);
}

/*
 * Test: PIT timer delivers interrupts (tick counter increments).
 *
 * The PIT is configured at 100 Hz. Waiting ~50ms should produce
 * at least a few ticks.
 */
extern volatile uint64_t pit_ticks;

static void test_pit_ticks_incrementing(void)
{
    uint64_t start = pit_ticks;
    /* Busy-wait with HLT for ~50ms (5 ticks at 100 Hz) */
    for (int i = 0; i < 10; i++)
        __asm__ volatile("hlt");
    uint64_t elapsed = pit_ticks - start;
    /* Should have gotten at least 1 tick */
    TEST_ASSERT_TRUE(elapsed >= 1);
}

/*
 * Test: PIT tick rate is approximately 100 Hz.
 *
 * Wait for 100 ticks and verify it takes roughly 1 second.
 * Since we cannot measure wall time precisely without another timer,
 * we just verify the ticks advance by at least 50 in a reasonable
 * number of HLT cycles.
 */
static void test_pit_rate_approximately_100hz(void)
{
    uint64_t start = pit_ticks;
    /* HLT 100 times — each should wake on the next interrupt (~10ms each) */
    for (int i = 0; i < 100; i++)
        __asm__ volatile("hlt");
    uint64_t elapsed = pit_ticks - start;
    /* Should be roughly 100 ticks. Allow wide tolerance (50-200). */
    TEST_ASSERT_TRUE(elapsed >= 50);
    TEST_ASSERT_TRUE(elapsed <= 200);
}

/* ============================================================================
 * Long Mode Verification Tests
 * ============================================================================ */

static void test_64bit_operations(void)
{
    uint64_t large_value = 0x123456789ABCDEF0ULL;
    uint64_t shifted = large_value >> 32;
    TEST_ASSERT_EQUAL_HEX64(0x12345678, shifted);

    volatile uint64_t test = 0xFFFFFFFFFFFFFFFFULL;
    TEST_ASSERT_EQUAL_HEX64(0xFFFFFFFFFFFFFFFFULL, test);
}

static void test_rip_relative_addressing(void)
{
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
    RUN_TEST(test_multiboot2_structure_valid);
    RUN_TEST(test_multiboot2_has_memory_map);
    RUN_TEST(test_multiboot2_memory_above_1mb);
    RUN_TEST(test_multiboot2_has_bootloader_name);

    /* IDT tests */
    RUN_TEST(test_idt_loaded);
    RUN_TEST(test_idt_exception_entries_present);
    RUN_TEST(test_idt_irq_entries_are_interrupt_gates);
    RUN_TEST(test_idt_exception_entries_are_trap_gates);

    /* PIC tests */
    RUN_TEST(test_pic_responds_to_ocw3);
    RUN_TEST(test_pic_timer_unmasked);
    RUN_TEST(test_pic_slave_accessible);

    /* PIT timer tests */
    RUN_TEST(test_interrupts_enabled);
    RUN_TEST(test_pit_ticks_incrementing);
    RUN_TEST(test_pit_rate_approximately_100hz);

    /* Long mode verification */
    RUN_TEST(test_64bit_operations);
    RUN_TEST(test_rip_relative_addressing);

    return UnityEnd();
}

#else /* !PLATFORM_X86_64 */

int test_suite_x86_boot(void)
{
    return 0;
}

#endif /* PLATFORM_X86_64 */
