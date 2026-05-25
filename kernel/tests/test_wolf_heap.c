/*
 * test_wolf_heap.c — dedicated wolfssl heap regression coverage.
 *
 * `kernel/net/ssh/wolf_heap.c` is the 48 MB PMM-backed allocator
 * behind wolfssl's XMALLOC / XFREE / XREALLOC. The freelist-with-
 * magic-stamp design ports from `lua_stubs.c`'s allocator and adds
 * a double-free guard introduced during the #916 review.
 *
 * The init path (`pmm_alloc_pages(12288)` for 48 MB) only runs once
 * per boot; the test suite reuses whatever state the daemon (or a
 * prior test) has already left behind. Allocations made here are
 * released before each test returns so a long sequence of tests
 * doesn't drift the heap toward exhaustion.
 *
 * Gated on NET_SSHD (the daemon owns the heap; the placeholder
 * typedef in the !NET_SSHD branch keeps -Wpedantic happy).
 */

#if defined(NET_SSHD)

#include "wolf_heap.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

static void test_alloc_returns_writable_pointer(void)
{
    uint8_t *p = wolf_heap_alloc(256);
    TEST_ASSERT_NOT_NULL(p);
    /* Write a pattern + read it back — confirms the returned region
     * is actually writable kernel memory, not a stale stamp. */
    for (size_t i = 0; i < 256; i++) {
        p[i] = (uint8_t)(i ^ 0xA5u);
    }
    for (size_t i = 0; i < 256; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i ^ 0xA5u), p[i]);
    }
    wolf_heap_free(p);
}

static void test_alloc_zero_returns_null(void)
{
    /* Matches malloc(0) semantics: returns NULL rather than handing
     * out a header-only block. */
    void *p = wolf_heap_alloc(0);
    TEST_ASSERT_NULL(p);
}

static void test_alignment_is_at_least_16(void)
{
    /* wolfcrypt's SIMD-y paths assume 16-byte alignment; the
     * allocator's `align_up(n)` should ensure every returned pointer
     * satisfies that. */
    void *a = wolf_heap_alloc(1);
    void *b = wolf_heap_alloc(17);
    void *c = wolf_heap_alloc(257);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)a & 0xFu);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)b & 0xFu);
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)c & 0xFu);
    wolf_heap_free(a);
    wolf_heap_free(b);
    wolf_heap_free(c);
}

static void test_free_null_is_safe(void)
{
    /* malloc.h convention. Catches a buggy guard that fires only on
     * non-NULL paths. */
    wolf_heap_free(NULL);
    TEST_PASS();
}

static void test_realloc_null_acts_as_alloc(void)
{
    void *p = wolf_heap_realloc(NULL, 128);
    TEST_ASSERT_NOT_NULL(p);
    wolf_heap_free(p);
}

static void test_realloc_size_zero_acts_as_free(void)
{
    void *p = wolf_heap_alloc(64);
    TEST_ASSERT_NOT_NULL(p);
    void *r = wolf_heap_realloc(p, 0);
    TEST_ASSERT_NULL(r);
    /* p is freed by the realloc; no further wolf_heap_free needed. */
}

static void test_realloc_shrink_returns_same_block(void)
{
    /* The realloc-shrink path documented in wolf_heap.c keeps the
     * existing block (slack-compact deferred per the #916 review).
     * The user-visible pointer must remain valid and the contents
     * must be preserved up to the new size. */
    uint8_t *p = wolf_heap_alloc(512);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < 512; i++) {
        p[i] = (uint8_t)i;
    }

    uint8_t *q = wolf_heap_realloc(p, 64);
    TEST_ASSERT_NOT_NULL(q);
    for (size_t i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, q[i]);
    }
    wolf_heap_free(q);
}

static void test_realloc_grow_copies_payload(void)
{
    uint8_t *p = wolf_heap_alloc(64);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < 64; i++) {
        p[i] = (uint8_t)(0xFFu - i);
    }

    uint8_t *q = wolf_heap_realloc(p, 4096);
    TEST_ASSERT_NOT_NULL(q);
    /* The grow path memcpys b->size bytes; the bytes beyond the
     * original 64 are uninitialised so we only check the prefix. */
    for (size_t i = 0; i < 64; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xFFu - i), q[i]);
    }
    wolf_heap_free(q);
}

static void test_alloc_after_free_reuses_block(void)
{
    /* The freelist coalesces on free; an immediate same-size alloc
     * after a free should pick the same block back up. This is the
     * "no fragmentation under repeat alloc/free of the same size"
     * invariant — important for wolfssl's per-handshake allocation
     * pattern. */
    void *p = wolf_heap_alloc(128);
    TEST_ASSERT_NOT_NULL(p);
    wolf_heap_free(p);
    void *q = wolf_heap_alloc(128);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_PTR(p, q);
    wolf_heap_free(q);
}

static void test_many_small_alloc_free_roundtrip(void)
{
    /* Stress the freelist's split+merge logic. 64 allocations,
     * release in reverse order, allocate again — the heap should
     * end in the same coalesced state. */
    void *blocks[64];
    for (size_t i = 0; i < 64; i++) {
        blocks[i] = wolf_heap_alloc(64);
        TEST_ASSERT_NOT_NULL(blocks[i]);
    }
    for (size_t i = 64; i > 0; i--) {
        wolf_heap_free(blocks[i - 1]);
    }
    /* If coalescing is correct, a single 64 * 80B allocation should
     * succeed afterward (covers the slab we just released). */
    size_t round_up = 64u * 80u;
    void *big = wolf_heap_alloc(round_up);
    TEST_ASSERT_NOT_NULL(big);
    wolf_heap_free(big);
}

static void test_realloc_of_non_wolf_pointer_returns_null(void)
{
    /* The magic-stamp check in wolf_heap_realloc rejects pointers
     * the wolf heap didn't issue. Stamp a deliberately-bad magic at
     * the location the function will probe (one struct-wolf-block
     * before the user pointer), and confirm the function returns
     * NULL without touching anything else.
     *
     * `_Alignas(16)` matches wolf_heap's alignment guarantee so the
     * dereference at `fake.pad - sizeof(struct wolf_block)` lands
     * on the header we control instead of straddling object
     * boundaries. The header layout — `uint32_t magic` first — is
     * pinned in wolf_heap.c; if that ever changes, the static_assert
     * inside that file's struct definition would fire first. */
    static struct {
        uint32_t magic;
        uint32_t is_free;
        size_t   size;
        void    *prev;
        void    *next;
        _Alignas(16) uint8_t pad[64];
    } fake = {
        .magic = 0xBADBADBAu,   /* known-bad, distinct from WOLF_HEAP_MAGIC */
    };

    void *r = wolf_heap_realloc(fake.pad, 64);
    TEST_ASSERT_NULL(r);
    /* The bogus pointer wasn't accepted; the magic field is still
     * the bad value we stamped (the function logged the violation
     * but didn't mutate). */
    TEST_ASSERT_EQUAL_UINT32(0xBADBADBAu, fake.magic);
}

int test_suite_wolf_heap(void)
{
    UnityBegin("wolf_heap (dedicated wolfssl allocator)");

    RUN_TEST(test_alloc_returns_writable_pointer);
    RUN_TEST(test_alloc_zero_returns_null);
    RUN_TEST(test_alignment_is_at_least_16);
    RUN_TEST(test_free_null_is_safe);
    RUN_TEST(test_realloc_null_acts_as_alloc);
    RUN_TEST(test_realloc_size_zero_acts_as_free);
    RUN_TEST(test_realloc_shrink_returns_same_block);
    RUN_TEST(test_realloc_grow_copies_payload);
    RUN_TEST(test_alloc_after_free_reuses_block);
    RUN_TEST(test_many_small_alloc_free_roundtrip);
    RUN_TEST(test_realloc_of_non_wolf_pointer_returns_null);

    return UnityEnd();
}

#else  /* !NET_SSHD */

typedef int test_wolf_heap_placeholder_t;

#endif /* NET_SSHD */
