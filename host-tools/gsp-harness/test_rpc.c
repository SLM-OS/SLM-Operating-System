/*
 * test_rpc.c — regression tests for the GSP-RM RPC ring helpers.
 *
 * Pure-logic tests for:
 *   - gsp_rpc_pages_for_payload: page-count rounding for a given
 *     payload size.
 *   - gsp_rpc_advance_ptr: ring-pointer wraparound modulo page count.
 *   - gsp_rpc_free_pages: producer free-space calc, including the
 *     "subtract one to disambiguate full vs empty" rule.
 *   - gsp_rpc_init / dtor: shm allocation + ring view setup against
 *     a mock platform vtable.
 *
 * Hardware integration (post-GSP-RM-alive RPC handshake) is exercised
 * via the gsp-harness CLI once GSP boots end-to-end.
 *
 * Wired into `make test-rpc`.
 */

/* posix_memalign needs _XOPEN_SOURCE >= 600. */
#define _XOPEN_SOURCE 600
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../kernel/gpu/nvidia/rpc.h"

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* ---- Mock platform ops for gsp_rpc_init testing.
 *
 * dma_alloc returns a malloc'd page-aligned buffer with the IOVA set
 * to the VA — adequate for verifying the channel layout. The other
 * fields are set so the GSP shared core compiles.
 */

static void *mock_dma_alloc(size_t size, size_t align, uint64_t *out_iova)
{
    (void)align;
    void *p = NULL;
    if (posix_memalign(&p, 4096, size) != 0) return NULL;
    if (out_iova) *out_iova = (uint64_t)(uintptr_t)p;
    return p;
}

static void mock_dma_free(void *p, size_t size) { (void)size; free(p); }

static uint32_t mock_read32(uint32_t off) { (void)off; return 0; }
static void     mock_write32(uint32_t off, uint32_t v) { (void)off; (void)v; }
static void     mock_bar1_read(uint32_t off, void *d, size_t n)
                              { (void)off; memset(d, 0, n); }
static void     mock_bar1_write(uint32_t off, const void *s, size_t n)
                              { (void)off; (void)s; (void)n; }
static void     mock_cache(const void *p, size_t s) { (void)p; (void)s; }
static void     mock_cache_inv(void *p, size_t s) { (void)p; (void)s; }
static void     mock_mb(void) {}
static void     mock_fwget(enum gsp_firmware_kind k, struct gsp_firmware_blob *o)
                          { (void)k; o->data = NULL; o->size = 0; o->version = NULL; }
static int      mock_vbios_get_fwsec(const void **d, size_t *s)
                          { *d = NULL; *s = 0; return -1; }

static const struct gsp_platform_ops mock_ops = {
    .read32          = mock_read32,
    .write32         = mock_write32,
    .bar1_read       = mock_bar1_read,
    .bar1_write      = mock_bar1_write,
    .dma_alloc       = mock_dma_alloc,
    .dma_free        = mock_dma_free,
    .cache_clean     = mock_cache,
    .cache_invalidate= mock_cache_inv,
    .mb              = mock_mb,
    .firmware_get    = mock_fwget,
    .vbios_get_fwsec = mock_vbios_get_fwsec,
};

extern const struct gsp_platform_ops *gsp_platform;

/* ---- pages_for_payload tests ---- */

static void test_pages_payload_empty(void)
{
    /* Zero payload still needs 1 page for the framing. */
    REQUIRE(gsp_rpc_pages_for_payload(0) == 1);
}

static void test_pages_payload_small_fits_one(void)
{
    /* Header (48 bytes) + 100 byte payload fits in 1 page. */
    REQUIRE(gsp_rpc_pages_for_payload(100) == 1);
}

static void test_pages_payload_exact_page(void)
{
    /* 4096 - sizeof(gsp_msg_hdr) bytes fits in 1 page. One byte
     * more spills to 2 pages. */
    uint32_t hdr = (uint32_t)sizeof(struct gsp_msg_hdr);
    REQUIRE(gsp_rpc_pages_for_payload(GSP_PAGE_SIZE - hdr) == 1);
    REQUIRE(gsp_rpc_pages_for_payload(GSP_PAGE_SIZE - hdr + 1) == 2);
}

static void test_pages_payload_large(void)
{
    /* 5 pages of payload + framing = 6 or 7 pages depending on hdr. */
    uint32_t pages = gsp_rpc_pages_for_payload(5u * GSP_PAGE_SIZE);
    REQUIRE(pages >= 5 && pages <= 7);
}

/* ---- advance_ptr tests ---- */

static void test_advance_ptr_no_wrap(void)
{
    REQUIRE(gsp_rpc_advance_ptr(/*start=*/0,  /*adv=*/3,  /*cnt=*/64) == 3);
    REQUIRE(gsp_rpc_advance_ptr(/*start=*/10, /*adv=*/5,  /*cnt=*/64) == 15);
}

static void test_advance_ptr_wraps_at_boundary(void)
{
    /* 60 + 5 = 65, mod 64 = 1. */
    REQUIRE(gsp_rpc_advance_ptr(60, 5, 64) == 1);
    /* Exact wrap to 0. */
    REQUIRE(gsp_rpc_advance_ptr(63, 1, 64) == 0);
}

static void test_advance_ptr_zero_count_safe(void)
{
    /* Defensive: zero page count → return 0 rather than divide-by-zero. */
    REQUIRE(gsp_rpc_advance_ptr(0, 5, 0) == 0);
}

/* ---- free_pages tests ---- */

static void test_free_pages_empty_ring(void)
{
    /* wptr==rptr==0 with 64 pages → 63 free (one slot reserved). */
    REQUIRE(gsp_rpc_free_pages(0, 0, 64) == 63);
}

static void test_free_pages_one_used(void)
{
    /* Producer wrote 1 page → 62 free. */
    REQUIRE(gsp_rpc_free_pages(1, 0, 64) == 62);
}

static void test_free_pages_full_ring(void)
{
    /* wptr is one slot before rptr (mod 64) → ring full → 0 free. */
    REQUIRE(gsp_rpc_free_pages(63, 0, 64) == 0);
    /* Also: wptr=10, rptr=11 → wrap-around full. */
    REQUIRE(gsp_rpc_free_pages(10, 11, 64) == 0);
}

static void test_free_pages_wrap(void)
{
    /* wptr=5, rptr=10 → consumer ahead in linear sense (after wrap).
     * Free = 10 + 64 - 5 - 1 = 68; 68 - 64 = 4. */
    REQUIRE(gsp_rpc_free_pages(5, 10, 64) == 4);
}

static void test_free_pages_zero_count_safe(void)
{
    REQUIRE(gsp_rpc_free_pages(0, 0, 0) == 0);
}

/* ---- gsp_rpc_init / dtor against mock vtable ---- */

static void test_rpc_init_lays_out_rings(void)
{
    gsp_platform = &mock_ops;
    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == 0);
    REQUIRE(ch.initialized);
    REQUIRE(ch.shm_va != NULL);
    REQUIRE(ch.shm_size == 4096u + 0x40000u + 0x40000u);

    /* cmdq + msgq pointers must point inside shm region. */
    uint8_t *base = (uint8_t *)ch.shm_va;
    uint8_t *end  = base + ch.shm_size;
    REQUIRE((uint8_t *)ch.cmdq.wptr >= base
         && (uint8_t *)ch.cmdq.wptr <  end);
    REQUIRE((uint8_t *)ch.msgq.rptr >= base
         && (uint8_t *)ch.msgq.rptr <  end);
    REQUIRE(ch.cmdq.page_count == 64);
    REQUIRE(ch.msgq.page_count == 64);

    /* Ring data starts at page 1. cmdq base = shm + 4 KB. */
    REQUIRE(ch.cmdq.base == base + 4096u);
    REQUIRE(ch.msgq.base == base + 4096u + 0x40000u);

    /* Sequence starts at 1 (matches nouveau). */
    REQUIRE(ch.cmdq.seq == 1);

    gsp_rpc_dtor(&ch);
    REQUIRE(ch.initialized == false);
    REQUIRE(ch.shm_va == NULL);
}

static void test_rpc_init_pointers_at_canonical_offsets(void)
{
    gsp_platform = &mock_ops;
    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == 0);
    /* cmdq w/r at page 0 byte 0/4; msgq w/r at page 0 byte 0x100/0x104. */
    uint8_t *base = (uint8_t *)ch.shm_va;
    REQUIRE((uint8_t *)ch.cmdq.wptr - base == 0x000);
    REQUIRE((uint8_t *)ch.cmdq.rptr - base == 0x004);
    REQUIRE((uint8_t *)ch.msgq.wptr - base == 0x100);
    REQUIRE((uint8_t *)ch.msgq.rptr - base == 0x104);
    gsp_rpc_dtor(&ch);
}

static void test_rpc_send_rejects_when_gsp_not_alive(void)
{
    gsp_platform = &mock_ops;
    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == 0);
    /* Without gsp_init_done, send must refuse — otherwise it'd silently
     * fill the ring with no consumer to drain it. */
    char payload[16] = {0};
    REQUIRE(gsp_rpc_send(&ch, NV_VGPU_MSG_EVENT_GSP_INIT_DONE,
                         payload, sizeof(payload)) == -1);
    gsp_rpc_dtor(&ch);
}

int main(void)
{
    test_pages_payload_empty();
    test_pages_payload_small_fits_one();
    test_pages_payload_exact_page();
    test_pages_payload_large();

    test_advance_ptr_no_wrap();
    test_advance_ptr_wraps_at_boundary();
    test_advance_ptr_zero_count_safe();

    test_free_pages_empty_ring();
    test_free_pages_one_used();
    test_free_pages_full_ring();
    test_free_pages_wrap();
    test_free_pages_zero_count_safe();

    test_rpc_init_lays_out_rings();
    test_rpc_init_pointers_at_canonical_offsets();
    test_rpc_send_rejects_when_gsp_not_alive();

    if (failures == 0) {
        printf("test_rpc: all tests PASS\n");
        return 0;
    }
    printf("test_rpc: %d FAIL\n", failures);
    return 1;
}
