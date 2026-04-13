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

/* CORE-C1: copy several argv strings into a buffer with just enough room. */
static void test_elf_argv_copy_fits(void)
{
    char buf[64];
    char *arg_locations[ELF_MAX_ARGV];
    char *argv[] = { (char *)"hello", (char *)"world", (char *)"!" };
    int argc = 3;

    int rc = elf_copy_argv_strings(buf, sizeof(buf), argc, argv, arg_locations);
    TEST_ASSERT_EQUAL_INT(ELF_OK, rc);

    /* arg_locations should point into buf in order, each followed by the
     * written string + NUL terminator. */
    TEST_ASSERT_EQUAL_PTR(buf, arg_locations[0]);
    TEST_ASSERT_EQUAL_STRING("hello", arg_locations[0]);
    TEST_ASSERT_EQUAL_STRING("world", arg_locations[1]);
    TEST_ASSERT_EQUAL_STRING("!", arg_locations[2]);
}

/* CORE-C1: buffer must be rejected when an argv string would overflow it. */
static void test_elf_argv_copy_rejects_overflow(void)
{
    char buf[8];  /* room for "hello\0" but not "hello\0world\0" */
    char *arg_locations[ELF_MAX_ARGV];
    char *argv[] = { (char *)"hello", (char *)"world" };

    int rc = elf_copy_argv_strings(buf, sizeof(buf), 2, argv, arg_locations);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_ARGV_TOO_LARGE, rc);
}

/* CORE-C1: single oversized arg must be rejected cleanly. */
static void test_elf_argv_copy_rejects_single_oversize(void)
{
    char buf[4];
    char *arg_locations[ELF_MAX_ARGV];
    char *argv[] = { (char *)"too-long-to-fit" };

    int rc = elf_copy_argv_strings(buf, sizeof(buf), 1, argv, arg_locations);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_ARGV_TOO_LARGE, rc);
}

/* CORE-C1: argc beyond ELF_MAX_ARGV must be rejected to protect the caller's
 * arg_locations[] array. */
static void test_elf_argv_copy_rejects_argc_over_limit(void)
{
    char buf[256];
    char *arg_locations[ELF_MAX_ARGV];
    char *argv[ELF_MAX_ARGV + 1];
    for (int i = 0; i <= ELF_MAX_ARGV; i++) {
        argv[i] = (char *)"a";
    }

    int rc = elf_copy_argv_strings(buf, sizeof(buf), ELF_MAX_ARGV + 1,
                                   argv, arg_locations);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_INVALID, rc);
}

/* CORE-C1: argc == 0 is a valid no-op. */
static void test_elf_argv_copy_zero_argc(void)
{
    char buf[8];
    char *arg_locations[ELF_MAX_ARGV];
    char *argv[] = { NULL };

    int rc = elf_copy_argv_strings(buf, sizeof(buf), 0, argv, arg_locations);
    TEST_ASSERT_EQUAL_INT(ELF_OK, rc);
}

int test_suite_elf(void)
{
    UnityBegin("ELF Loader Tests");

    RUN_TEST(test_elf_validate_rejects_tiny_buffer);
    RUN_TEST(test_elf_validate_accepts_no_phdrs);
    RUN_TEST(test_elf_validate_rejects_phoff_past_size);
    RUN_TEST(test_elf_validate_rejects_phdr_array_truncated);
    RUN_TEST(test_elf_validate_rejects_overflow_phoff);
    RUN_TEST(test_elf_argv_copy_fits);
    RUN_TEST(test_elf_argv_copy_rejects_overflow);
    RUN_TEST(test_elf_argv_copy_rejects_single_oversize);
    RUN_TEST(test_elf_argv_copy_rejects_argc_over_limit);
    RUN_TEST(test_elf_argv_copy_zero_argc);

    return UnityEnd();
}
