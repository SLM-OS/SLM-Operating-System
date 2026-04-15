/*
 * rpc.h — GSP-RM RPC message ring (E4).
 *
 * Once GSP-RM is running on the RISC-V core (post E3.4.e) the host
 * communicates with it via a pair of single-producer / single-consumer
 * page-granular ring buffers in shared system memory:
 *
 *   - cmdq: host writes commands → GSP reads
 *   - msgq: GSP writes responses + events → host reads
 *
 * Each ring is GSP_PAGE_SIZE-aligned and owns its own
 * write-pointer / read-pointer pair stored at the top of the ring.
 *
 * Layout reference: nouveau-gsp-rpc-r535.c (`r535_gsp_msgq_*`,
 * `r535_gsp_cmdq_push`) + openrm
 * `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h`.
 *
 * Status (E4 milestone): the ring layout, allocation, and host-side
 * pointer arithmetic are implemented and unit-tested with a mock
 * platform vtable. The "send/wait" entry points exist but produce
 * a deterministic ENOSYS until GSP-RM is actually running on
 * hardware (post E3.4.e), at which point the RPC handshake +
 * GSP_INIT_DONE wait will be enabled.
 */

#ifndef GPU_NVIDIA_RPC_H
#define GPU_NVIDIA_RPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* GSP page size — fixed at 4 KB regardless of host page size. */
#define GSP_PAGE_SIZE          4096u

/* Ring sizing. Nouveau uses 0x40000 (256 KB / 64 pages) per ring;
 * we match for protocol compatibility. */
#define GSP_CMDQ_SIZE          0x40000u
#define GSP_MSGQ_SIZE          0x40000u
#define GSP_RING_PAGE_COUNT    (GSP_CMDQ_SIZE / GSP_PAGE_SIZE)

/* GSP message-element header (precedes every cmdq/msgq entry).
 * Mirrors `struct r535_gsp_msg` in nouveau. AEAD auth fields are
 * present in the layout but unused on x86-64 (HCC mode disabled). */
struct gsp_msg_hdr {
    uint8_t  auth_tag[16];
    uint8_t  aad[16];
    uint32_t checksum;
    uint32_t sequence;
    uint32_t elem_count;
    uint32_t pad;
    /* variable-size payload follows */
};

/* GSP RPC header (inside element data). Mirrors `struct nvfw_gsp_rpc`. */
struct gsp_rpc_hdr {
    uint32_t header_version;
    uint32_t signature;        /* 'C' 'P' 'R' 'V' = 0x56504352 */
    uint32_t length;           /* total bytes including this header */
    uint32_t function;         /* NV_VGPU_MSG_* opcode */
    uint32_t rpc_result;
    uint32_t rpc_result_private;
    uint32_t sequence;
    uint32_t spare_or_gfid;
    /* params follow */
};

/* RPC opcodes we care about for E4 / E5. Full list:
 *   nouveau-gsp-rpc-r535.c references nvrm/rpcfn.h. We only need
 *   GSP_INIT_DONE for E4 milestone and a handful for E5 compute. */
#define NV_VGPU_MSG_EVENT_GSP_INIT_DONE          0x1004u

/* Per-ring runtime state. cmdq/msgq each get one of these. The
 * pointer fields point into the shared-memory region — they're
 * volatile because GSP updates them asynchronously. */
struct gsp_ring {
    volatile uint32_t *wptr;       /* host writes (cmdq) / reads (msgq) */
    volatile uint32_t *rptr;       /* host reads (cmdq) / writes (msgq) */
    uint8_t           *base;       /* ring data start (page 1 onward) */
    uint32_t           page_count; /* number of GSP_PAGE_SIZE pages */
    uint32_t           seq;        /* monotonic sequence (cmdq only) */
};

/* Top-level RPC channel. Owns the shared-mem allocation, both rings,
 * and a small bounce buffer for assembling/disassembling multi-page
 * messages. */
struct gsp_rpc_channel {
    void     *shm_va;              /* host VA of shared region */
    uint64_t  shm_iova;            /* GPU-visible phys addr */
    size_t    shm_size;            /* total bytes */

    struct gsp_ring cmdq;
    struct gsp_ring msgq;

    bool      initialized;
    bool      gsp_init_done;       /* set once GSP_INIT_DONE arrives */
};

/*
 * Allocate the shared memory region and lay out the cmdq/msgq rings
 * at canonical offsets. Initializes head/tail pointers to 0. Does
 * NOT contact GSP — the caller is responsible for handing the IOVA
 * to GSP via the libos arg struct after this returns.
 *
 * Returns 0 on success. -1 on allocation failure or if @ch is NULL.
 */
int gsp_rpc_init(struct gsp_rpc_channel *ch);

/*
 * Tear down a channel: free DMA-mapped memory and reset all fields.
 * Safe to call on a partially-initialized or zeroed channel.
 */
void gsp_rpc_dtor(struct gsp_rpc_channel *ch);

/* ---- Coherency + ordering discipline ----
 *
 * On ARM64 (Jetson), the host CPU and the GSP RISC-V core observe
 * shared sysmem through different paths: CPU through L1+L2 caches,
 * GSP through the SoC fabric (or coherent PCIe on x86). Two failure
 * modes the GSP-RM RPC code must defend against:
 *
 *  1. **Stale data** — CPU writes a payload, then bumps wptr, but
 *     the payload sits in a dirty cache line until eviction. The
 *     GSP, reading via the coherent path, may see an advanced wptr
 *     pointing at memory that still holds the previous message.
 *     **Fix**: cache_clean(payload_page, len) before publishing wptr.
 *
 *  2. **Reorder** — CPU writes payload + cache_clean, then writes
 *     wptr, but absent a barrier the wptr write may surface to the
 *     coherent fabric before the cache_clean of the payload region
 *     completes. **Fix**: mb() (`dsb sy` on ARM64, `mfence` on x86)
 *     between the cache_clean and the wptr publish.
 *
 * The publish/consume contract:
 *
 *   Producer (host → cmdq, or GSP → msgq, mirrored on the consumer
 *   side):
 *     1. Write payload bytes into ring page(s).
 *     2. cache_clean(payload, len) + mb().
 *     3. Update wptr cell.
 *     4. cache_clean(wptr, 4) + mb().
 *     5. (optional) MMIO doorbell to wake the consumer.
 *
 *   Consumer:
 *     1. cache_invalidate(rptr_cell_or_wptr_view, 4) + mb().
 *     2. Read updated index.
 *     3. cache_invalidate(payload_page, len) + mb().
 *     4. Read payload.
 *     5. Update own rptr (writes follow the producer rules above).
 *
 * On x86-64 (PCIe coherent): cache_* are no-ops, mb() is `mfence`.
 * On ARM64 (Jetson): cache_* walk PoC, mb() is `dsb sy`.
 *
 * `gsp_rpc_publish_word()` below packages the "write a u32 cell +
 * flush + barrier" sequence so future send/wait implementations
 * can't accidentally drop the barrier.
 */

/*
 * Publish a 32-bit shared-memory cell with the right
 * coherency + barrier discipline for cross-domain visibility.
 * Use this for wptr/rptr bumps and any other GSP-visible flag the
 * host writes — the open-coded "*ptr = v; cache_clean(ptr,4); mb()"
 * pattern would be repeated everywhere otherwise and is easy to
 * get wrong. No-op-fast on x86-64 (cache_clean / mb are stubs).
 */
void gsp_rpc_publish_word(volatile uint32_t *cell, uint32_t value);

/*
 * Snapshot a 32-bit shared-memory cell with the right invalidate +
 * barrier sequence for reading something the GSP wrote. Use this
 * for the wptr-of-msgq / rptr-of-cmdq updates the GSP performs.
 * Returns the freshly-invalidated value.
 */
uint32_t gsp_rpc_snapshot_word(volatile uint32_t *cell);

/*
 * Send an RPC. Builds the element header + RPC header + payload
 * directly into the cmdq pages, advances writePtr.
 *
 * @function   NV_VGPU_MSG_FUNCTION_* opcode.
 * @payload    pointer to params; copied into the ring.
 * @payload_len bytes in @payload; sum (header + payload) must fit in
 *              GSP_MSG_MAX_SIZE = 16 * GSP_PAGE_SIZE = 64 KB.
 *
 * Returns 0 on success, -1 if the ring is full or arguments invalid.
 *
 * Currently returns -1 + sets @ch->initialized to false until the
 * GSP-side msgq wiring lands (E4 hardware step). The pure host-side
 * arithmetic of "would this fit?" is exercised via tests against a
 * mock channel in test_rpc.c.
 */
int gsp_rpc_send(struct gsp_rpc_channel *ch, uint32_t function,
                 const void *payload, size_t payload_len);

/*
 * Wait up to @timeout_us for an RPC matching @function on msgq.
 * On success copies up to @max_out bytes of the payload into @out
 * and writes the actual length into *@out_len.
 *
 * Returns 0 on success, -1 on timeout or any framing error.
 */
int gsp_rpc_wait(struct gsp_rpc_channel *ch, uint32_t function,
                 uint32_t timeout_us,
                 void *out, size_t max_out, size_t *out_len);

/* ---- Pure-logic helpers exposed for unit testing ---- */

/*
 * Compute the number of GSP_PAGE_SIZE pages needed to hold an RPC
 * of @rpc_payload_len bytes (excluding the message-element header,
 * which is part of the per-page framing). Used by send/recv to
 * advance write/read pointers by the right page count.
 *
 * Pure function; no I/O.
 */
uint32_t gsp_rpc_pages_for_payload(uint32_t rpc_payload_len);

/*
 * Compute the next ring position after writing @advance pages
 * starting at @start_page within a ring of @page_count pages. Wraps
 * modulo @page_count. Pure function; no I/O.
 */
uint32_t gsp_rpc_advance_ptr(uint32_t start_page, uint32_t advance,
                             uint32_t page_count);

/*
 * Compute "free pages" available to a producer when the ring write
 * pointer is at @wptr and the consumer's read pointer is at @rptr.
 * The single empty slot needed to disambiguate full from empty is
 * subtracted (so a 64-page ring reports max 63 free).
 *
 * Pure function; no I/O.
 */
uint32_t gsp_rpc_free_pages(uint32_t wptr, uint32_t rptr,
                            uint32_t page_count);

#endif /* GPU_NVIDIA_RPC_H */
