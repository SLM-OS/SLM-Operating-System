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

uint32_t gsp_rpc_pages_for_payload(uint32_t rpc_payload_len)
{
    /* Element header is part of the framing in the first page; a
     * payload of 0 still needs 1 page. Each subsequent page holds
     * GSP_PAGE_SIZE - element_header_size bytes — for sizing
     * purposes we round generously: every page after the first holds
     * up to GSP_PAGE_SIZE bytes (the header overhead is bounded). */
    if (rpc_payload_len == 0) return 1;
    uint32_t total = sizeof(struct gsp_msg_hdr) + rpc_payload_len;
    return (total + GSP_PAGE_SIZE - 1) / GSP_PAGE_SIZE;
}

uint32_t gsp_rpc_advance_ptr(uint32_t start_page, uint32_t advance,
                             uint32_t page_count)
{
    if (page_count == 0) return 0;
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
    uint32_t free = rptr + page_count - wptr - 1u;
    if (free >= page_count) free -= page_count;
    return free;
}

int gsp_rpc_init(struct gsp_rpc_channel *ch)
{
    if (!ch) return GSP_ERR_INVAL;
    if (!gsp_platform || !gsp_platform->dma_alloc || !gsp_platform->dma_free)
        return GSP_ERR_INVAL;

    memset(ch, 0, sizeof(*ch));

    /* Total size: header page + cmdq + msgq. */
    ch->shm_size = SHM_PTR_PAGE_BYTES + GSP_CMDQ_SIZE + GSP_MSGQ_SIZE;

    /* Allocate page-aligned. The GSP needs the full block contiguous
     * in IOVA space. */
    ch->shm_va = gsp_platform->dma_alloc(ch->shm_size, GSP_PAGE_SIZE,
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

    *ch->cmdq.wptr = 0;
    *ch->cmdq.rptr = 0;
    *ch->msgq.wptr = 0;
    *ch->msgq.rptr = 0;
    ch->cmdq.seq = 1;

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
