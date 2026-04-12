/*
 * test_elf.c - ELF loader regression tests
 *
 * Covers elf_validate() bounds handling against truncated/crafted headers
 * (BOOT-H2 in docs/code-review-2026-04-12.md).
 */

#include "unity.h"
#include "../include/elf.h"
#include "../include/string.h"
#include <stdint.h>

/* Build a minimally valid ELF64 header for the current target machine.
 * The header claims program-header metadata so the phdr-bounds branch is
 * exercised; the file buffer itself is provided separately by each test. */
static void fill_ehdr(Elf64_Ehdr *ehdr,
                      uint64_t e_phoff,
                      uint16_t e_phnum)
{
    /* zero then populate */
    for (size_t i = 0; i < sizeof(*ehdr); i++) {
        ((uint8_t *)ehdr)[i] = 0;
    }
    *(uint32_t *)ehdr->e_ident = ELF_MAGIC;
    ehdr->e_ident[4] = ELFCLASS64;
    ehdr->e_ident[5] = ELFDATA2LSB;
    ehdr->e_type = ET_EXEC;
#if defined(PLATFORM_X86_64)
    ehdr->e_machine = 0x3E;  /* EM_X86_64 */
#else
    ehdr->e_machine = EM_AARCH64;
#endif
    ehdr->e_version = 1;
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phoff = e_phoff;
    ehdr->e_phnum = e_phnum;
}

static void test_elf_validate_rejects_tiny_buffer(void)
{
    Elf64_Ehdr ehdr;
    fill_ehdr(&ehdr, sizeof(Elf64_Ehdr), 0);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED,
                          elf_validate(&ehdr, sizeof(Elf64_Ehdr) - 1));
}

static void test_elf_validate_accepts_no_phdrs(void)
{
    /* Zero program headers, phoff == sizeof(Ehdr) → always in bounds. */
    Elf64_Ehdr ehdr;
    fill_ehdr(&ehdr, sizeof(Elf64_Ehdr), 0);
    TEST_ASSERT_EQUAL_INT(ELF_OK, elf_validate(&ehdr, sizeof(Elf64_Ehdr)));
}

/* BOOT-H2 regression: e_phoff beyond buffer must be rejected. */
static void test_elf_validate_rejects_phoff_past_size(void)
{
    Elf64_Ehdr ehdr;
    fill_ehdr(&ehdr, /*e_phoff*/ 4096, /*e_phnum*/ 1);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED, elf_validate(&ehdr, 1024));
}

/* BOOT-H2 regression: e_phoff + e_phnum*sizeof(Phdr) past size rejected. */
static void test_elf_validate_rejects_phdr_array_truncated(void)
{
    Elf64_Ehdr ehdr;
    /* Header + one full phdr = sizeof(Ehdr) + 56. Claim 10 phdrs. */
    fill_ehdr(&ehdr, sizeof(Elf64_Ehdr), 10);
    /* Buffer only covers header + 1 phdr's worth of trailing space. */
    size_t size = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED, elf_validate(&ehdr, size));
}

/* BOOT-H2 regression: e_phoff near UINT64_MAX with small phnum must be
 * caught even if the 32-bit-mode addition would wrap to a small sum. */
static void test_elf_validate_rejects_overflow_phoff(void)
{
    Elf64_Ehdr ehdr;
    fill_ehdr(&ehdr, /*e_phoff*/ (uint64_t)-64, /*e_phnum*/ 2);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED, elf_validate(&ehdr, 4096));
}

int test_suite_elf(void)
{
    UnityBegin("ELF Loader Tests");

    RUN_TEST(test_elf_validate_rejects_tiny_buffer);
    RUN_TEST(test_elf_validate_accepts_no_phdrs);
    RUN_TEST(test_elf_validate_rejects_phoff_past_size);
    RUN_TEST(test_elf_validate_rejects_phdr_array_truncated);
    RUN_TEST(test_elf_validate_rejects_overflow_phoff);

    return UnityEnd();
}
