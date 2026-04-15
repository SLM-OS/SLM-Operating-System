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
/* Cache-maintenance + barrier call capture. The bare-metal Jetson
 * backend issues `dc cvac` / `dc civac` per cacheline and `dsb sy`
 * for the barrier; our mock records the cumulative (addr, size)
 * plus call counts so tests can assert the RPC code actually
 * follows the publish discipline (clean BEFORE mb, both BEFORE the
 * next observable side-effect).
 *
 * `g_event_log` records the relative order of clean/inv/mb so the
 * ordering test can assert "publish_word issues clean then mb",
 * not just "issues both somehow".
 */
static const void *g_last_clean_addr;
static size_t      g_last_clean_size;
static uint32_t    g_clean_count;
static const void *g_last_inv_addr;
static size_t      g_last_inv_size;
static uint32_t    g_inv_count;
static uint32_t    g_mb_count;

#define EVENT_LOG_CAP 32
static char     g_event_log[EVENT_LOG_CAP];
static uint32_t g_event_count;
static void event_push(char e)
{
    if (g_event_count < EVENT_LOG_CAP) g_event_log[g_event_count++] = e;
}
static void mock_cache(const void *p, size_t s)
{
    g_last_clean_addr = p;
    g_last_clean_size = s;
    g_clean_count++;
    event_push('c');
}
static void mock_cache_inv(void *p, size_t s)
{
    g_last_inv_addr = p;
    g_last_inv_size = s;
    g_inv_count++;
    event_push('i');
}
static void mock_mb(void) { g_mb_count++; event_push('m'); }
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
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
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
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
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
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
    /* Without gsp_init_done, send must refuse — otherwise it'd silently
     * fill the ring with no consumer to drain it. Specific NOSYS code
     * lets the caller distinguish "GSP-RM not yet alive" from other
     * failure modes (full ring → NOSPC, bad arg → INVAL). */
    char payload[16] = {0};
    REQUIRE(gsp_rpc_send(&ch, NV_VGPU_MSG_EVENT_GSP_INIT_DONE,
                         payload, sizeof(payload)) == GSP_ERR_NOSYS);
    gsp_rpc_dtor(&ch);
}

static void test_rpc_init_null_arg_rejected(void)
{
    REQUIRE(gsp_rpc_init(NULL) == GSP_ERR_INVAL);
}

static void reset_event_log(void)
{
    g_clean_count = 0; g_inv_count = 0; g_mb_count = 0; g_event_count = 0;
    g_last_clean_addr = NULL; g_last_clean_size = 0;
    g_last_inv_addr = NULL; g_last_inv_size = 0;
    memset(g_event_log, 0, sizeof(g_event_log));
}

/* ---- publish_word / snapshot_word: barrier discipline tests ----
 *
 * The whole point of the helpers is to make "store + cache_clean +
 * dsb sy" indivisible. If anyone reorders or drops a step, ARM64
 * publishes that the GSP can't see (or worse, can see partially).
 * These tests pin down the contract.
 */
static void test_publish_word_ordering_clean_then_mb(void)
{
    gsp_platform = &mock_ops;
    reset_event_log();

    volatile uint32_t cell = 0;
    gsp_rpc_publish_word(&cell, 0xDEADBEEFu);

    /* Cell holds the new value. */
    REQUIRE(cell == 0xDEADBEEFu);
    /* Exactly one clean and one mb, in that order. */
    REQUIRE(g_clean_count == 1);
    REQUIRE(g_mb_count == 1);
    REQUIRE(g_event_count == 2);
    REQUIRE(g_event_log[0] == 'c');
    REQUIRE(g_event_log[1] == 'm');
    /* Clean covers exactly the cell's 4 bytes — flushing more would
     * be safe but flushing less means the GSP could see stale. */
    REQUIRE(g_last_clean_addr == (const void *)&cell);
    REQUIRE(g_last_clean_size == sizeof(cell));
}

static void test_snapshot_word_ordering_inv_then_mb(void)
{
    gsp_platform = &mock_ops;
    reset_event_log();

    volatile uint32_t cell = 0xCAFEu;
    uint32_t v = gsp_rpc_snapshot_word(&cell);

    REQUIRE(v == 0xCAFEu);
    REQUIRE(g_inv_count == 1);
    REQUIRE(g_mb_count == 1);
    REQUIRE(g_event_count == 2);
    REQUIRE(g_event_log[0] == 'i');
    REQUIRE(g_event_log[1] == 'm');
    REQUIRE(g_last_inv_addr == (void *)&cell);
    REQUIRE(g_last_inv_size == sizeof(cell));
}

static void test_publish_snapshot_null_safe(void)
{
    /* Defensive: NULL cell should silently no-op rather than crash. */
    gsp_platform = &mock_ops;
    gsp_rpc_publish_word(NULL, 0);
    REQUIRE(gsp_rpc_snapshot_word(NULL) == 0);
}

static void test_rpc_init_publishes_all_four_pointers(void)
{
    /* Init must use the publish helper for cmdq + msgq w/r — that's
     * 4 publishes, each with one clean + one mb. The full-region
     * cache_clean + mb adds another pair at the end. So we expect
     * exactly 5 cleans + 5 mbs, alternating c, m, c, m, ... — any
     * 'm' before its preceding 'c' would mean an unbarriered window
     * where the GSP could observe stale. */
    gsp_platform = &mock_ops;
    reset_event_log();

    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
    REQUIRE(g_clean_count == 5);
    REQUIRE(g_mb_count == 5);
    REQUIRE(g_event_count == 10);
    for (uint32_t i = 0; i < g_event_count; i += 2) {
        REQUIRE(g_event_log[i]     == 'c');
        REQUIRE(g_event_log[i + 1] == 'm');
    }
    gsp_rpc_dtor(&ch);
}

static void test_rpc_init_flushes_shm_for_arm64(void)
{
    /* Cross-domain coherency check: gsp_rpc_init must call
     * cache_clean on the entire shm region after zeroing it, so
     * the Jetson backend's `dc cvac` actually pushes the zeros to
     * PoC before the GSP reads them. Without this, an integrated
     * GPU on Jetson would see stale DRAM contents in the rings. */
    gsp_platform = &mock_ops;
    reset_event_log();

    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
    REQUIRE(g_clean_count >= 1);
    /* The full-region flush is the LAST clean call (after the four
     * publish_word calls). Its (addr, size) must cover the whole
     * shm region — anything less leaves a window where the GSP
     * could observe stale ring entries. */
    REQUIRE(g_last_clean_addr == ch.shm_va);
    REQUIRE(g_last_clean_size == ch.shm_size);
    gsp_rpc_dtor(&ch);
}

static void test_rpc_send_oversize_rejected(void)
{
    gsp_platform = &mock_ops;
    struct gsp_rpc_channel ch;
    REQUIRE(gsp_rpc_init(&ch) == GSP_OK);
    /* Anything > 16 GSP pages is rejected at the boundary check —
     * INVAL, not NOSYS, since the failure is caller-fault not
     * "channel not ready". */
    static char big[17u * GSP_PAGE_SIZE];
    REQUIRE(gsp_rpc_send(&ch, NV_VGPU_MSG_EVENT_GSP_INIT_DONE,
                         big, sizeof(big)) == GSP_ERR_INVAL);
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
    test_rpc_init_null_arg_rejected();
    test_publish_word_ordering_clean_then_mb();
    test_snapshot_word_ordering_inv_then_mb();
    test_publish_snapshot_null_safe();
    test_rpc_init_publishes_all_four_pointers();
    test_rpc_init_flushes_shm_for_arm64();
    test_rpc_send_oversize_rejected();

    if (failures == 0) {
        printf("test_rpc: all tests PASS\n");
        return 0;
    }
    printf("test_rpc: %d FAIL\n", failures);
    return 1;
}
