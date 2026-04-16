/*
 * rpc.c — GSP-RM RPC channel implementation (E4 skeleton).
 *
 * Allocates the shared sysmem region used by both rings, initializes
 * the head/tail pointer pages, and provides the host-side ring
 * arithmetic.
 *
 * The send/wait entry points are gated until GSP-RM is actually
 * running on the RISC-V core — without that, posting to cmdq has
 * no consumer and writePtr would just grow until the ring is full.
 * The pure-logic ring helpers are exercised by test_rpc.c against
 * a mock channel.
 */

#include "rpc.h"
#include "gsp.h"

#include <string.h>

extern const struct gsp_platform_ops *gsp_platform;

/* Shared region layout (matches r535_gsp_shared_init):
 *
 *   page 0          rptr/wptr cells for both rings (8 u32 max — only
 *                   need 4 actually, but we own the whole page)
 *   page 1..N       cmdq data pages
 *   page N+1..M     msgq data pages
 *
 * Header pointers live at fixed offsets in page 0 so the GSP side
 * can find them via the shared-mem base address alone. */
#define SHM_PTR_PAGE_BYTES        GSP_PAGE_SIZE

#define CMDQ_WPTR_OFFSET          0x000u
#define CMDQ_RPTR_OFFSET          0x004u
#define MSGQ_WPTR_OFFSET          0x100u
#define MSGQ_RPTR_OFFSET          0x104u

void gsp_rpc_publish_word(volatile uint32_t *cell, uint32_t value)
{
    if (!cell || !gsp_platform) return;
    *cell = value;
    /* Flush the cell's cache line to PoC, then `dsb sy` so any
     * subsequent observable event (another publish, an MMIO doorbell
     * write) is ordered after the GSP can see this update. The order
     * matters: cache_clean(cell) BEFORE the barrier, otherwise the
     * GSP can observe the value via the coherent path while the
     * barrier-protected dirty cache line still hasn't drained. */
    if (gsp_platform->cache_clean)
        gsp_platform->cache_clean((const void *)cell, sizeof(*cell));
    if (gsp_platform->mb)
        gsp_platform->mb();
}

uint32_t gsp_rpc_snapshot_word(volatile uint32_t *cell)
{
    if (!cell || !gsp_platform) return 0;
    /* Mirror image of publish: invalidate the cache line so any
     * stale CPU-side copy is dropped, barrier so the read below
     * doesn't speculate ahead of the invalidate. */
    if (gsp_platform->cache_invalidate)
        gsp_platform->cache_invalidate((void *)cell, sizeof(*cell));
    if (gsp_platform->mb)
        gsp_platform->mb();
    return *cell;
}

uint32_t gsp_rpc_pages_for_payload(uint32_t rpc_payload_len)
{
    /* Element header is part of the framing in the first page; a
     * payload of 0 still needs 1 page. Each subsequent page holds
     * GSP_PAGE_SIZE - element_header_size bytes — for sizing
     * purposes we round generously: every page after the first holds
     * up to GSP_PAGE_SIZE bytes (the header overhead is bounded). */
    if (rpc_payload_len == 0) return 1;
    /* Defensive overflow guard (#169): reject payloads so large that
     * `sizeof(hdr) + rpc_payload_len + GSP_PAGE_SIZE - 1` would wrap
     * u32. A wrap would silently produce an undersized page count
     * and the caller would write past the ring. The legitimate
     * send path already caps at 16 pages via a separate guard; this
     * is belt-and-suspenders at the arithmetic boundary. */
    const uint32_t hdr = (uint32_t)sizeof(struct gsp_msg_hdr);
    if (rpc_payload_len > UINT32_MAX - hdr - (GSP_PAGE_SIZE - 1))
        return UINT32_MAX;
    uint32_t total = hdr + rpc_payload_len;
    return (total + GSP_PAGE_SIZE - 1) / GSP_PAGE_SIZE;
}

uint32_t gsp_rpc_advance_ptr(uint32_t start_page, uint32_t advance,
                             uint32_t page_count)
{
    if (page_count == 0) return 0;
    /* Defensive reject (#169): an `advance` larger than the ring
     * is a caller-side bug (corrupt size from the GSP, wrong element
     * header, IOMMU remap that scrambled the shared page). Return
     * `page_count` as a sentinel rather than silently wrapping into
     * the consumer's window. Legitimate callers never advance by
     * more than the ring size. */
    if (advance > page_count) return page_count;
    /* Modulo-arithmetic that matches r535_gsp_msgq_recv_one_elem:
     *   rptr = (rptr + DIV_ROUND_UP(size, GSP_PAGE_SIZE)) % cnt */
    uint32_t r = (start_page % page_count) + (advance % page_count);
    if (r >= page_count) r -= page_count;
    return r;
}

uint32_t gsp_rpc_free_pages(uint32_t wptr, uint32_t rptr, uint32_t page_count)
{
    /* Mirror nouveau r535_gsp_cmdq_push:
     *   free = rptr + cnt - wptr - 1
     *   if (free >= cnt) free -= cnt;
     * Subtract one to disambiguate full vs empty. */
    if (page_count == 0) return 0;
    /* Clamp the GSP-written pointer cells to `< page_count` on entry
     * (#169). A corrupted `wptr` or `rptr` would otherwise produce a
     * nonsense `free` value that routes a real `gsp_rpc_send` into
     * the wrong ring offset. This is a structural invariant — the
     * GSP side always reduces modulo page_count before writing. */
    wptr %= page_count;
    rptr %= page_count;
    uint32_t free = rptr + page_count - wptr - 1u;
    if (free >= page_count) free -= page_count;
    return free;
}

int gsp_rpc_init(struct gsp_rpc_channel *ch)
{
    if (!ch) return GSP_ERR_INVAL;
    if (!gsp_platform || !gsp_platform->dma_alloc || !gsp_platform->dma_free
        || !gsp_platform->cache_clean)
        return GSP_ERR_INVAL;

    memset(ch, 0, sizeof(*ch));

    /* Total size: header page + cmdq + msgq. */
    ch->shm_size = SHM_PTR_PAGE_BYTES + GSP_CMDQ_SIZE + GSP_MSGQ_SIZE;

    /* Allocate page-aligned. The GSP needs the full block contiguous
     * in IOVA space. */
    ch->shm_va = gsp_dma_alloc_checked(ch->shm_size, GSP_PAGE_SIZE,
                                        &ch->shm_iova);
    if (!ch->shm_va) return GSP_ERR_NOMEM;

    memset(ch->shm_va, 0, ch->shm_size);

    /* Wire up ring views. */
    uint8_t *p = (uint8_t *)ch->shm_va;
    ch->cmdq.wptr = (volatile uint32_t *)(p + CMDQ_WPTR_OFFSET);
    ch->cmdq.rptr = (volatile uint32_t *)(p + CMDQ_RPTR_OFFSET);
    ch->cmdq.base = p + SHM_PTR_PAGE_BYTES;
    ch->cmdq.page_count = GSP_RING_PAGE_COUNT;

    ch->msgq.wptr = (volatile uint32_t *)(p + MSGQ_WPTR_OFFSET);
    ch->msgq.rptr = (volatile uint32_t *)(p + MSGQ_RPTR_OFFSET);
    ch->msgq.base = p + SHM_PTR_PAGE_BYTES + GSP_CMDQ_SIZE;
    ch->msgq.page_count = GSP_RING_PAGE_COUNT;

    /* Initialize ring pointers via the publish helper so the
     * canonical "store + cache_clean + dsb sy" sequence is exercised
     * exactly once per cell. Anything weaker would let the GSP read
     * a non-zero stale rptr on first poll. */
    gsp_rpc_publish_word(ch->cmdq.wptr, 0);
    gsp_rpc_publish_word(ch->cmdq.rptr, 0);
    gsp_rpc_publish_word(ch->msgq.wptr, 0);
    gsp_rpc_publish_word(ch->msgq.rptr, 0);
    ch->cmdq.seq = 1;

    /* Cross-domain coherency: on ARM64 (Jetson), DMA-allocated sysmem
     * may be cached on the CPU side. The memset above only updated
     * cache lines — the actual DRAM pages the GSP DMAs from could
     * still hold stale data. Flush the entire region to PoC so the
     * GSP, when it eventually reads any payload page, sees the
     * zeroed contents we just wrote. The mb() pairs the flush with
     * a `dsb sy` barrier so any subsequent observable event (another
     * cache op, an MMIO write that wakes the GSP) is ordered after
     * the dirty lines have drained.
     *
     * On x86-64 this whole block degenerates to one mfence (cache_*
     * are no-ops on coherent PCIe). The send/wait paths follow the
     * same discipline via gsp_rpc_publish_word / _snapshot_word. */
    gsp_platform->cache_clean(ch->shm_va, ch->shm_size);
    gsp_platform->mb();

    ch->initialized = true;
    return GSP_OK;
}

void gsp_rpc_dtor(struct gsp_rpc_channel *ch)
{
    if (!ch) return;
    if (ch->shm_va && gsp_platform && gsp_platform->dma_free) {
        gsp_platform->dma_free(ch->shm_va, ch->shm_size);
    }
    memset(ch, 0, sizeof(*ch));
}

int gsp_rpc_send(struct gsp_rpc_channel *ch, uint32_t function,
                 const void *payload, size_t payload_len)
{
    if (!ch || !ch->initialized) return GSP_ERR_INVAL;
    if (payload_len > 0 && !payload) return GSP_ERR_INVAL;

    /* Compute pages needed; reject anything > 16 pages (GSP cap). */
    if (payload_len > 16u * GSP_PAGE_SIZE) return GSP_ERR_INVAL;
    uint32_t pages = gsp_rpc_pages_for_payload(
        (uint32_t)(payload_len + sizeof(struct gsp_rpc_hdr)));
    if (pages > 16) return GSP_ERR_INVAL;

    /* Free-space check. */
    uint32_t wptr = *ch->cmdq.wptr;
    uint32_t rptr = *ch->cmdq.rptr;
    if (gsp_rpc_free_pages(wptr, rptr, ch->cmdq.page_count) < pages)
        return GSP_ERR_NOSPC;

    /* Until GSP-RM is alive on the RISC-V core, the consumer never
     * advances rptr — sending here would silently fill the ring.
     * Refuse to send until gsp_init_done is observed. */
    if (!ch->gsp_init_done) return GSP_ERR_NOSYS;

    /* Write element header + RPC header + payload into the cmdq
     * starting at wptr. (Implementation pending hardware-side
     * correctness review — landed alongside the GSP_INIT_DONE wait
     * once the test-pc bringup completes.) */
    (void)function; (void)payload; (void)payload_len;
    return GSP_ERR_NOSYS;
}

int gsp_rpc_wait(struct gsp_rpc_channel *ch, uint32_t function,
                 uint32_t timeout_us,
                 void *out, size_t max_out, size_t *out_len)
{
    if (!ch || !ch->initialized) return GSP_ERR_INVAL;
    (void)function; (void)timeout_us; (void)out; (void)max_out;
    if (out_len) *out_len = 0;

    /* Same gating as gsp_rpc_send. The polling loop will live here
     * once the GSP-side queue advances. */
    if (!ch->gsp_init_done) return GSP_ERR_NOSYS;
    return GSP_ERR_NOSYS;
}
