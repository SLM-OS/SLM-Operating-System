/*
 * test_nvidia_gsp_platform.c — regression tests for the Jetson GA10B
 * platform shim (kernel/arch/arm64/nvidia_gsp_platform.c).
 *
 * The shim is ARM-only by nature (compiled under
 * PLATFORM_JETSON_ORIN_NANO), but several surfaces are pure C and
 * worth regression-testing without hardware:
 *
 *   1. firmware_get — enum dispatch. Returns {NULL, 0, NULL} for
 *      every kind when ENABLE_GSP_FIRMWARE is off (the default on
 *      Jetson since GA10B doesn't use the discrete-Ampere blobs).
 *
 *   2. vbios_get_fwsec — always returns success with NULL/0 on
 *      Jetson (GA10B has no VBIOS). Simple but a regression here
 *      would make bringup.c's FWSEC phase misbehave silently.
 *
 *   3. nvidia_vbios_platform_load — always returns -1 on Jetson.
 *
 *   4. dma_alloc alignment math — over-allocates when the requested
 *      alignment exceeds PAGE_SIZE; returns an aligned pointer within
 *      the allocation; size=0 returns NULL/0 without touching PMM.
 *      Verified via a mock PMM that tracks page requests.
 *
 *   5. bar1_read/write early-out — both functions bail with a warning
 *      if g_bar1_base is 0 (before set_bar1_base is called). Important
 *      because Phase-0 calls shouldn't fault if bringup mis-orders.
 *
 *   6. platform_install — sets the global `gsp_platform` pointer.
 *
 * NOT tested here: the MMIO read/write paths (they only return device
 * memory values — no host substrate), cache maintenance (ARM DC CVAC
 * / DC CIVAC — host has no equivalent), or the memory barrier
 * (compiler-only on host).
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* The shim's file guard wants PLATFORM_JETSON_ORIN_NANO. We define
 * GPU_BASE before including so we don't pull platform.h's full
 * header (which references other kernel internals). */
#define PLATFORM_JETSON_ORIN_NANO 1
#define SLM_HOST_HARNESS          1

/* Provide GPU_BASE at a host-safe address — we never dereference it
 * in the surfaces this test exercises (MMIO read/write paths are
 * excluded). */
#define GPU_BASE                  ((uintptr_t)0x17000000UL)

/* pmm.h / uart.h don't use ARM asm, so they include cleanly. But we
 * need to preempt platform.h — it's pulled in transitively by the
 * shim's `#include "platform.h"`. The shim expects platform.h to
 * define GPU_BASE for Jetson. Instead of including the real
 * platform.h (which has full kernel context), we stub the symbols
 * the shim directly uses. */

/* ---- kernel header surrogates (shim's external dependencies) ---- */

/* uart surrogates — route to stderr for debugging. */
void uart_puts(const char *s) { fputs(s, stderr); }
void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- Mock PMM ---- */

static struct {
    size_t   alloc_calls;
    size_t   free_calls;
    size_t   last_pages;
    void    *last_ptr;
    void    *next_return;   /* set before test to control the PMM's output */
    bool     fail_next;
} g_pmm;

/* Replacement for pmm_alloc_pages. Returns a page-aligned simulated
 * "physical" address, or NULL on configured failure. */
void *pmm_alloc_pages(size_t pages)
{
    g_pmm.alloc_calls++;
    g_pmm.last_pages = pages;
    if (g_pmm.fail_next) {
        g_pmm.fail_next = false;
        return NULL;
    }
    if (g_pmm.next_return) {
        void *r = g_pmm.next_return;
        g_pmm.next_return = NULL;
        return r;
    }
    /* Default: return a page-aligned malloc'd block. Real PMM always
     * returns page-aligned; the alignment math in the shim depends on
     * it. */
    void *raw = aligned_alloc(4096, pages * 4096);
    g_pmm.last_ptr = raw;
    return raw;
}

void pmm_free_pages(void *ptr, size_t pages)
{
    g_pmm.free_calls++;
    g_pmm.last_pages = pages;
    g_pmm.last_ptr   = ptr;
    /* Intentionally don't free — the shim's dma_free can be given an
     * aligned-up pointer (not the original malloc'd block), so free()
     * here would corrupt. Leak is fine for a bounded-duration test. */
}

/* ---- Mock cache helpers ---- */
static struct {
    size_t clean_calls;
    size_t invalidate_calls;
    size_t last_clean_size;
    size_t last_inv_size;
} g_cache;

void cache_clean_range(const volatile void *addr, size_t size)
{
    (void)addr;
    g_cache.clean_calls++;
    g_cache.last_clean_size = size;
}

void cache_invalidate_range(const volatile void *addr, size_t size)
{
    (void)addr;
    g_cache.invalidate_calls++;
    g_cache.last_inv_size = size;
}

/* ---- gsp_platform global — the shim's install() writes here ---- */
#include "../../kernel/gpu/nvidia/gsp.h"
const struct gsp_platform_ops *gsp_platform;

/* Installer declared in the shim. */
extern void jetson_gsp_platform_install(void);
extern void jetson_gsp_set_bar1_base(uintptr_t base);

/* Test framework ------------------------------------------------------- */

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define REQUIRE_EQ(a, b) do { \
    unsigned long _a = (unsigned long)(a); \
    unsigned long _b = (unsigned long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d: %s (=%lu) != %s (=%lu)\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        failures++; \
    } \
} while (0)

static void mock_reset(void)
{
    memset(&g_pmm,   0, sizeof(g_pmm));
    memset(&g_cache, 0, sizeof(g_cache));
    jetson_gsp_platform_install();
    jetson_gsp_set_bar1_base(0);        /* reset BAR1 every test */
}

/* ---------------------------------------------------------------------
 * Test 1: vtable installation
 * --------------------------------------------------------------------- */

static void test_install_populates_vtable(void)
{
    printf("== test_install_populates_vtable ==\n");
    gsp_platform = NULL;
    jetson_gsp_platform_install();
    REQUIRE(gsp_platform != NULL);
    /* Every vtable slot must be filled — a NULL slot would crash the
     * shared bringup code at the first dereference. */
    REQUIRE(gsp_platform->read32 != NULL);
    REQUIRE(gsp_platform->write32 != NULL);
    REQUIRE(gsp_platform->bar1_read != NULL);
    REQUIRE(gsp_platform->bar1_write != NULL);
    REQUIRE(gsp_platform->dma_alloc != NULL);
    REQUIRE(gsp_platform->dma_free != NULL);
    REQUIRE(gsp_platform->cache_clean != NULL);
    REQUIRE(gsp_platform->cache_invalidate != NULL);
    REQUIRE(gsp_platform->mb != NULL);
    REQUIRE(gsp_platform->firmware_get != NULL);
    REQUIRE(gsp_platform->vbios_get_fwsec != NULL);
}

/* ---------------------------------------------------------------------
 * Test 2: firmware_get enum dispatch
 * --------------------------------------------------------------------- */

static void test_firmware_get_returns_zeros_when_disabled(void)
{
    printf("== test_firmware_get_returns_zeros_when_disabled ==\n");
    mock_reset();

    /* ENABLE_GSP_FIRMWARE is not defined when the test compiles the
     * shim (we don't pass -DENABLE_GSP_FIRMWARE). Every firmware
     * kind — including out-of-range ones — must return zeros. */
    for (int k = 0; k < (int)GSP_FW_KIND_COUNT + 2; k++) {
        struct gsp_firmware_blob blob = { (const uint8_t *)0xdead,
                                          0x1234, (const char *)0xdead };
        gsp_platform->firmware_get((enum gsp_firmware_kind)k, &blob);
        REQUIRE_EQ((uintptr_t)blob.data, 0);
        REQUIRE_EQ(blob.size, 0);
        REQUIRE_EQ((uintptr_t)blob.version, 0);
    }
}

/* ---------------------------------------------------------------------
 * Test 3: VBIOS accessors
 * --------------------------------------------------------------------- */

static void test_vbios_get_fwsec_returns_null_success(void)
{
    printf("== test_vbios_get_fwsec_returns_null_success ==\n");
    mock_reset();
    /* On Jetson (integrated GPU) there's no VBIOS. The contract is to
     * return 0 (success) with both out pointers set to NULL/0 — so
     * callers can distinguish "no VBIOS" from "VBIOS broken". */
    const void *data = (const void *)0xdead;
    size_t       size = 0x1234;
    int rc = gsp_platform->vbios_get_fwsec(&data, &size);
    REQUIRE_EQ(rc, 0);
    REQUIRE_EQ((uintptr_t)data, 0);
    REQUIRE_EQ(size, 0);
}

extern int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size);

static void test_nvidia_vbios_platform_load_returns_error(void)
{
    printf("== test_nvidia_vbios_platform_load_returns_error ==\n");
    /* The shared bringup layer calls nvidia_vbios_platform_load before
     * FWSEC phases. On Jetson this always fails — the shared code
     * then takes its Jetson-specific path. */
    const uint8_t *data = (const uint8_t *)0xdead;
    size_t         size = 0x1234;
    int rc = nvidia_vbios_platform_load(&data, &size);
    REQUIRE_EQ(rc, -1);
}

/* ---------------------------------------------------------------------
 * Test 4: dma_alloc alignment math
 * --------------------------------------------------------------------- */

static void test_dma_alloc_size_zero_returns_null(void)
{
    printf("== test_dma_alloc_size_zero_returns_null ==\n");
    mock_reset();

    uint64_t dma_addr = 0xdeadbeef;
    void *p = gsp_platform->dma_alloc(0, 4096, &dma_addr);
    REQUIRE_EQ((uintptr_t)p, 0);
    REQUIRE_EQ(dma_addr, 0);
    /* size=0 must short-circuit before PMM. */
    REQUIRE_EQ(g_pmm.alloc_calls, 0);
}

static void test_dma_alloc_page_alignment(void)
{
    printf("== test_dma_alloc_page_alignment ==\n");
    mock_reset();

    uint64_t dma_addr = 0;
    void *p = gsp_platform->dma_alloc(8192, 4096, &dma_addr);
    REQUIRE(p != NULL);
    /* For page-sized alignment, 8KB request = 2 pages. No over-alloc. */
    REQUIRE_EQ(g_pmm.last_pages, 2);
    /* Aligned address equals the raw page address. */
    REQUIRE_EQ((uintptr_t)p % 4096, 0);
    REQUIRE_EQ(dma_addr, (uintptr_t)p);
}

static void test_dma_alloc_larger_than_page_alignment(void)
{
    printf("== test_dma_alloc_larger_than_page_alignment ==\n");
    mock_reset();

    /* 64 KB alignment on an 8 KB request. The shim should
     * over-allocate by (align - page_size) = 60 KB, totalling
     * 8 KB + 60 KB = 68 KB → 17 pages. */
    uint64_t dma_addr = 0;
    void *p = gsp_platform->dma_alloc(8192, 64 * 1024, &dma_addr);
    REQUIRE(p != NULL);
    REQUIRE_EQ(g_pmm.last_pages, 17);
    /* Returned pointer must respect the 64 KB alignment. */
    REQUIRE_EQ((uintptr_t)p % (64 * 1024), 0);
    REQUIRE_EQ(dma_addr, (uintptr_t)p);
}

static void test_dma_alloc_pmm_failure_propagates(void)
{
    printf("== test_dma_alloc_pmm_failure_propagates ==\n");
    mock_reset();
    g_pmm.fail_next = true;

    uint64_t dma_addr = 0xdeadbeef;
    void *p = gsp_platform->dma_alloc(4096, 4096, &dma_addr);
    REQUIRE_EQ((uintptr_t)p, 0);
    REQUIRE_EQ(dma_addr, 0);
    REQUIRE_EQ(g_pmm.alloc_calls, 1);
}

static void test_dma_alloc_alignment_below_page_is_promoted(void)
{
    printf("== test_dma_alloc_alignment_below_page_is_promoted ==\n");
    mock_reset();

    /* Sub-page alignment (128) must be promoted to PAGE_SIZE so the
     * PMM's natural page alignment satisfies the request without
     * over-allocation. 4 KB request = 1 page. */
    uint64_t dma_addr = 0;
    void *p = gsp_platform->dma_alloc(4096, 128, &dma_addr);
    REQUIRE(p != NULL);
    REQUIRE_EQ(g_pmm.last_pages, 1);
    REQUIRE_EQ((uintptr_t)p % 4096, 0);
}

static void test_dma_free_rounds_size_to_pages(void)
{
    printf("== test_dma_free_rounds_size_to_pages ==\n");
    mock_reset();

    uint64_t dma_addr = 0;
    void *p = gsp_platform->dma_alloc(4096, 4096, &dma_addr);
    REQUIRE(p != NULL);
    size_t allocs_before = g_pmm.alloc_calls;

    /* Free with a non-page-multiple size — must round up. */
    gsp_platform->dma_free(p, 4097);
    REQUIRE_EQ(g_pmm.free_calls, 1);
    REQUIRE_EQ(g_pmm.last_pages, 2);
    (void)allocs_before;
}

static void test_dma_free_null_is_noop(void)
{
    printf("== test_dma_free_null_is_noop ==\n");
    mock_reset();
    gsp_platform->dma_free(NULL, 4096);
    REQUIRE_EQ(g_pmm.free_calls, 0);
}

/* ---------------------------------------------------------------------
 * Test 5: BAR1 early-out when base unset
 * --------------------------------------------------------------------- */

static void test_bar1_read_returns_when_base_unset(void)
{
    printf("== test_bar1_read_returns_when_base_unset ==\n");
    mock_reset();
    /* set_bar1_base(0) in mock_reset; bar1_read must not touch dst
     * and must not call cache_invalidate. */
    uint8_t dst[16];
    memset(dst, 0xab, sizeof(dst));
    gsp_platform->bar1_read(0, dst, sizeof(dst));
    REQUIRE_EQ(g_cache.invalidate_calls, 0);
    for (size_t i = 0; i < sizeof(dst); i++)
        REQUIRE_EQ(dst[i], 0xab);   /* untouched */
}

static void test_bar1_write_returns_when_base_unset(void)
{
    printf("== test_bar1_write_returns_when_base_unset ==\n");
    mock_reset();
    uint8_t src[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    gsp_platform->bar1_write(0, src, sizeof(src));
    REQUIRE_EQ(g_cache.clean_calls, 0);
}

/* ---------------------------------------------------------------------
 * Test 6: memory barrier (smoke test)
 * --------------------------------------------------------------------- */

static void test_mb_is_callable(void)
{
    printf("== test_mb_is_callable ==\n");
    mock_reset();
    /* Just verify it doesn't crash or return unexpected state. */
    gsp_platform->mb();
    gsp_platform->mb();
    gsp_platform->mb();
    REQUIRE(1);
}

/* ---------------------------------------------------------------------
 * Test 7: cache_clean / cache_invalidate forward through vtable
 * --------------------------------------------------------------------- */

static void test_cache_ops_forward_to_helpers(void)
{
    printf("== test_cache_ops_forward_to_helpers ==\n");
    mock_reset();

    char buf[128];
    gsp_platform->cache_clean(buf, sizeof(buf));
    REQUIRE_EQ(g_cache.clean_calls, 1);
    REQUIRE_EQ(g_cache.last_clean_size, sizeof(buf));

    gsp_platform->cache_invalidate(buf, sizeof(buf));
    REQUIRE_EQ(g_cache.invalidate_calls, 1);
    REQUIRE_EQ(g_cache.last_inv_size, sizeof(buf));
}

/* ---------------------------------------------------------------------
 * Entry
 * --------------------------------------------------------------------- */

int main(void)
{
    printf("[test_nvidia_gsp_platform] starting\n");

    test_install_populates_vtable();
    test_firmware_get_returns_zeros_when_disabled();
    test_vbios_get_fwsec_returns_null_success();
    test_nvidia_vbios_platform_load_returns_error();
    test_dma_alloc_size_zero_returns_null();
    test_dma_alloc_page_alignment();
    test_dma_alloc_larger_than_page_alignment();
    test_dma_alloc_pmm_failure_propagates();
    test_dma_alloc_alignment_below_page_is_promoted();
    test_dma_free_rounds_size_to_pages();
    test_dma_free_null_is_noop();
    test_bar1_read_returns_when_base_unset();
    test_bar1_write_returns_when_base_unset();
    test_mb_is_callable();
    test_cache_ops_forward_to_helpers();

    if (failures) {
        fprintf(stderr, "[test_nvidia_gsp_platform] %d FAILURES\n", failures);
        return 1;
    }
    printf("[test_nvidia_gsp_platform] all OK\n");
    return 0;
}
