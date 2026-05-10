/*
 * test_elf.c - ELF loader regression tests
 *
 * Covers elf_validate() bounds handling against truncated/crafted headers
 * (BOOT-H2 in docs/code-review-2026-04-12.md).
 */

#include "unity.h"
#include "../include/elf.h"
#include "../include/string.h"
#include "../include/vmm.h"
#include "../include/pmm.h"
#include "../include/task.h"
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

/* ===========================================================================
 * elf_load segment-bounds overflow regressions
 *
 * The PR-465 review fix replaces unguarded `p_offset + p_filesz` and
 * `p_vaddr + p_memsz` additions in elf_load with subtraction-form
 * bounds checks. A crafted phdr with values near UINT64_MAX could
 * previously wrap to a small sum and pass the naive bounds check,
 * letting elf_load proceed with out-of-bounds reads or a truncated
 * `load_size`. Tests below construct minimal in-memory ELF buffers
 * exercising both overflow scenarios.
 * =========================================================================== */

/* Build a minimal ELF that elf_load will accept (header + N phdrs). The
 * phdr table is placed immediately after the ehdr; trailing space for
 * segment data is left out — these tests should fail in segment
 * validation before any memory is allocated. */
static void build_phdr_elf(uint8_t *buf, size_t cap, size_t *out_size,
                           const Elf64_Phdr *phdrs, uint16_t n)
{
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buf;
    fill_ehdr(ehdr, sizeof(Elf64_Ehdr), n);
    size_t phdr_off = sizeof(Elf64_Ehdr);
    size_t total = phdr_off + (size_t)n * sizeof(Elf64_Phdr);
    TEST_ASSERT_TRUE(total <= cap);
    for (uint16_t i = 0; i < n; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(buf + phdr_off + i * sizeof(Elf64_Phdr));
        *p = phdrs[i];
    }
    *out_size = total;
}

/* p_offset near UINT64_MAX with non-zero p_filesz must be rejected
 * even though the unsigned addition would wrap to a small sum. */
static void test_elf_load_rejects_p_offset_overflow(void)
{
    uint8_t buf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    Elf64_Phdr phdr;
    for (size_t i = 0; i < sizeof(phdr); i++) ((uint8_t *)&phdr)[i] = 0;
    phdr.p_type = PT_LOAD;
    phdr.p_offset = (uint64_t)-32;   /* Near UINT64_MAX. */
    phdr.p_filesz = 64;              /* Wraps to ~32 if added naively. */
    phdr.p_vaddr = 0x40000000;
    phdr.p_memsz = 64;

    size_t size;
    build_phdr_elf(buf, sizeof(buf), &size, &phdr, 1);
    struct elf_info info;
    int rc = elf_load(buf, size, &info);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED, rc);
}

/* p_vaddr near UINT64_MAX with non-zero p_memsz must be rejected
 * even though the unsigned addition would wrap to a small `end`,
 * which would otherwise produce a small `load_size`. */
static void test_elf_load_rejects_p_vaddr_overflow(void)
{
    uint8_t buf[sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)];
    Elf64_Phdr phdr;
    for (size_t i = 0; i < sizeof(phdr); i++) ((uint8_t *)&phdr)[i] = 0;
    phdr.p_type = PT_LOAD;
    phdr.p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    phdr.p_filesz = 0;               /* Skip the file-bounds check. */
    phdr.p_vaddr = (uint64_t)-32;    /* Near UINT64_MAX. */
    phdr.p_memsz = 64;               /* Wraps to ~32 if added naively. */

    size_t size;
    build_phdr_elf(buf, sizeof(buf), &size, &phdr, 1);
    struct elf_info info;
    int rc = elf_load(buf, size, &info);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_TRUNCATED, rc);
}

#if !defined(PLATFORM_X86_64)
/* Embedded EL0 hello ELF (kernel/src/user_hello_embed.S). The build
 * system always supplies this on ARM64 platforms — used both as a
 * runtime artefact (the `userelf` shell command) and as a ready-made
 * fixture for the elf_load_user / task_create_user_elf tests. */
extern const uint8_t user_hello_elf_start[];
extern const uint8_t user_hello_elf_end[];

static void test_elf_load_user_loads_embedded_blob(void)
{
    size_t blob_len = (size_t)(user_hello_elf_end - user_hello_elf_start);
    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));
    TEST_ASSERT_NOT_EQUAL(0, l1_pa);

    uint64_t entry = 0;
    int rc = elf_load_user(user_hello_elf_start, blob_len, l1_pa, &entry);
    TEST_ASSERT_EQUAL_INT(ELF_OK, rc);
    /* Entry must be inside the loaded ELF window. */
    TEST_ASSERT_TRUE(entry >= USER_TEXT_VA);
    TEST_ASSERT_TRUE(entry < USER_ELF_STACK_PAGE_VA);

    vmm_destroy_user_l1(l1_pa);
}

static void test_elf_load_user_pmm_neutral(void)
{
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    size_t blob_len = (size_t)(user_hello_elf_end - user_hello_elf_start);
    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    uint64_t entry = 0;
    int rc = elf_load_user(user_hello_elf_start, blob_len, l1_pa, &entry);
    TEST_ASSERT_EQUAL_INT(ELF_OK, rc);

    /* vmm_destroy_user_l1 walks user-region L3 leaves and frees every
     * PMM_OWNED page back to PMM. After the round trip, free pages
     * must be at least where they started — net-neutral or better. */
    vmm_destroy_user_l1(l1_pa);

    pmm_get_stats(&after);
    TEST_ASSERT_MESSAGE(after.free_pages >= before.free_pages,
                        "elf_load_user + destroy leaked pages");
}

static void test_elf_load_user_rejects_garbage(void)
{
    uint64_t l1_pa = 0;
    TEST_ASSERT_EQUAL_INT(0, vmm_create_user_l1(&l1_pa));

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    uint64_t entry = 0;
    int rc = elf_load_user(buf, sizeof(buf), l1_pa, &entry);
    TEST_ASSERT_NOT_EQUAL(ELF_OK, rc);

    vmm_destroy_user_l1(l1_pa);
}

static void test_elf_load_user_rejects_null_args(void)
{
    uint64_t entry = 0;
    /* NULL blob */
    TEST_ASSERT_EQUAL_INT(ELF_ERR_INVALID,
                          elf_load_user(NULL, 1, 0x1000, &entry));
    /* NULL entry_out */
    size_t blob_len = (size_t)(user_hello_elf_end - user_hello_elf_start);
    TEST_ASSERT_EQUAL_INT(ELF_ERR_INVALID,
                          elf_load_user(user_hello_elf_start, blob_len,
                                        0x1000, NULL));
    /* Zero L1 PA */
    TEST_ASSERT_EQUAL_INT(ELF_ERR_INVALID,
                          elf_load_user(user_hello_elf_start, blob_len,
                                        0, &entry));
}

static void test_task_create_user_elf_populates_task(void)
{
    size_t blob_len = (size_t)(user_hello_elf_end - user_hello_elf_start);
    struct task *t = task_create_user_elf("elf_t",
                                          user_hello_elf_start, blob_len, 4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(1, t->is_user);
    TEST_ASSERT_NOT_EQUAL(0, t->user_l1_pa);
    TEST_ASSERT_EQUAL_UINT64(USER_ELF_STACK_TOP, t->user_stack_top);
    TEST_ASSERT_EQUAL_UINT64(USER_MMAP_VA_START, t->user_va_next);
    /* Entry must lie in the user code range we loaded into. */
    uint64_t entry = (uint64_t)(uintptr_t)t->user_entry;
    TEST_ASSERT_TRUE(entry >= USER_TEXT_VA);
    TEST_ASSERT_TRUE(entry < USER_ELF_STACK_PAGE_VA);

    /* Tear down without ever scheduling. task_destroy frees the L1
     * (which reclaims every PMM_OWNED leaf the loader installed) and
     * the stack page. */
    t->state = TASK_TERMINATED;
    task_destroy(t);
}
#endif /* !PLATFORM_X86_64 */

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

    /* PR-465 segment-bounds overflow regressions. */
    RUN_TEST(test_elf_load_rejects_p_offset_overflow);
    RUN_TEST(test_elf_load_rejects_p_vaddr_overflow);

#if !defined(PLATFORM_X86_64)
    /* EL0 ELF loader (this PR). */
    RUN_TEST(test_elf_load_user_loads_embedded_blob);
    RUN_TEST(test_elf_load_user_pmm_neutral);
    RUN_TEST(test_elf_load_user_rejects_garbage);
    RUN_TEST(test_elf_load_user_rejects_null_args);
    RUN_TEST(test_task_create_user_elf_populates_task);
#endif

    return UnityEnd();
}
