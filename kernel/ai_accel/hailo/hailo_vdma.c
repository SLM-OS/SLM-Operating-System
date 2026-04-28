/*
 * hailo_vdma.c — Hailo VDMA descriptor-list allocator.
 *
 * See hailo_vdma.h for the wire-format rationale and sizing rules.
 * This file owns just the list allocator; descriptor programming
 * and channel start/stop land in follow-up commits on the Phase 5.4
 * stack.
 */

#include "hailo_vdma.h"
#include "hailo.h"
#include "hailo_internal.h"
#include "debug.h"
#include "uart.h"
#include <string.h>

#ifdef HAILO_WIRE_DEBUG
/* Phase 8 cache-flush verification probe (2026-04-23): need direct
 * access to dc ivac (invalidate-only, no clean) so we can confirm
 * the bytes fw will DMA-read are actually in DRAM after our
 * cache_clean. The hailo_platform vtable only exposes clean +
 * civac (clean-and-invalidate) — neither tells us if a clean
 * propagated to DRAM, since civac would write back stale-cache
 * content too. cache_discard_range uses dc ivac. */
#include "cache.h"
#endif

/* Power-of-2 check: n is a power of 2 iff n != 0 and n & (n-1) == 0. */
static inline bool is_power_of_two(uint32_t n)
{
    return (n != 0) && ((n & (n - 1u)) == 0);
}

uint32_t hailo_vdma_desc_list_alloc_size(uint32_t desc_count)
{
    /* Raw buffer = desc_count * 16. Round up to the 64 KB DMA
     * alignment so the VDMA engine's HOST_DESC_BASE_ADDR truncation
     * doesn't land in the middle of a descriptor. For typical
     * desc_count values (64..4096) the rounded size dominates;
     * the trade-off is acceptable given the 64 KB align is
     * firmware-mandated. */
    uint32_t raw = desc_count * (uint32_t)sizeof(struct hailo_vdma_descriptor);
    uint32_t aligned = (raw + HAILO_VDMA_DESC_LIST_ALIGN - 1u)
                     & ~(HAILO_VDMA_DESC_LIST_ALIGN - 1u);
    return aligned;
}

/* Shared body — see hailo_vdma_desc_list_alloc{,_low} below. */
static int desc_list_alloc_inner(uint32_t desc_count,
                                 uint16_t desc_page_size,
                                 bool is_circular,
                                 bool prefer_low,
                                 struct hailo_vdma_desc_list *out)
{
    if (!out) return HAILO_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    /* Caller contract: desc_count is power-of-2 in [MIN, MAX].
     * HW-fixed — the VDMA engine uses (index & mask) to walk the
     * ring and a non-power-of-2 list would corrupt on wraparound.
     * MIN=2 because a list of one is meaningless (just single-shot).
     * MAX=65536 because num_avail/num_proc are 16-bit counters. */
    if (desc_count < HAILO_VDMA_MIN_DESC_COUNT
     || desc_count > HAILO_VDMA_MAX_DESC_COUNT
     || !is_power_of_two(desc_count)) {
        return HAILO_ERR_INVAL;
    }
    if (desc_page_size == 0) return HAILO_ERR_INVAL;

    if (!hailo_platform || !hailo_platform->dma_alloc
                        || !hailo_platform->dma_free) {
        return HAILO_ERR_NODEV;
    }

    uint32_t alloc_size = hailo_vdma_desc_list_alloc_size(desc_count);

    uint64_t iova = 0;
    /* Pick allocator: low-bias variant if requested AND available,
     * else fall back to default. */
    void *(*alloc_fn)(size_t, size_t, uint64_t *) = hailo_platform->dma_alloc;
    if (prefer_low && hailo_platform->dma_alloc_low) {
        alloc_fn = hailo_platform->dma_alloc_low;
    }
    void *cpu = alloc_fn((size_t)alloc_size,
                         (size_t)HAILO_VDMA_DESC_LIST_ALIGN,
                         &iova);
    if (!cpu) return HAILO_ERR_NOMEM;
    /* Defensive: HOST_DESC_BASE_ADDR truncates low 16 bits, so an
     * unaligned return silently corrupts the device-side pointer.
     * Reject rather than propagate. */
    if ((uintptr_t)cpu & (HAILO_VDMA_DESC_LIST_ALIGN - 1u)) {
        WARN("hailo: desc-list allocator returned unaligned ptr %p "
             "(need %u)", cpu, HAILO_VDMA_DESC_LIST_ALIGN);
        hailo_platform->dma_free(cpu, (size_t)alloc_size,
                                 (size_t)HAILO_VDMA_DESC_LIST_ALIGN);
        return HAILO_ERR_NOMEM;
    }

    /* Zero-init the whole buffer. A descriptor with all-zero fields
     * is inert (page size 0 → no transfer), which is what we want
     * until the caller programs real contents. */
    memset(cpu, 0, alloc_size);

    out->descs            = (struct hailo_vdma_descriptor *)cpu;
    out->iova             = iova;
    out->desc_count       = desc_count;
    out->desc_count_mask  = desc_count - 1u;
    out->desc_page_size   = desc_page_size;
    out->is_circular      = is_circular;
    return HAILO_OK;
}

int hailo_vdma_desc_list_alloc(uint32_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
                               struct hailo_vdma_desc_list *out)
{
    return desc_list_alloc_inner(desc_count, desc_page_size, is_circular,
                                 /*prefer_low=*/false, out);
}

int hailo_vdma_desc_list_alloc_low(uint32_t desc_count,
                                   uint16_t desc_page_size,
                                   bool is_circular,
                                   struct hailo_vdma_desc_list *out)
{
    return desc_list_alloc_inner(desc_count, desc_page_size, is_circular,
                                 /*prefer_low=*/true, out);
}

void hailo_vdma_desc_list_free(struct hailo_vdma_desc_list *list)
{
    if (!list || !list->descs) return;
    if (hailo_platform && hailo_platform->dma_free) {
        uint32_t alloc_size = hailo_vdma_desc_list_alloc_size(list->desc_count);
        hailo_platform->dma_free(list->descs,
                                 (size_t)alloc_size,
                                 (size_t)HAILO_VDMA_DESC_LIST_ALIGN);
    }
    memset(list, 0, sizeof(*list));
}

/* -------------------------------------------------------------------------- */
/* Descriptor programming                                                      */
/* -------------------------------------------------------------------------- */

/* Bit layout constants from the reference
 * (docs/reference/hailo-vdma-common.c:39-41). Firmware-fixed. */
#define HAILO_VDMA_DESC_PAGE_SIZE_SHIFT 8u
#define HAILO_VDMA_DESC_DESC_CONTROL    0x02u
#define HAILO_VDMA_DESC_ADDR_L_MASK     0xFFFFFFC0u

/* Control-byte flags that go into the LAST descriptor of a transfer
 * (hailo-vdma-common.c:50-54 + get_interrupts_bitmask L203-223). The
 * reference ORs these into the last-desc PageSize_DescControl when
 * the caller asks for DOMAIN_DEVICE or DOMAIN_HOST completion
 * signaling.
 *
 * For H2D boundary submits the hypothesis is that at least
 * DESC_REQUEST_IRQ_PROCESSED (0x04) is load-bearing even with
 * poll-based waits — it may be the marker firmware's
 * BURST_CREDITS_TASK uses to know "this is the last desc of a
 * transfer, advance the per-channel batch counter". Today our
 * descriptors only set DESCRIPTOR_DESC_CONTROL (0x02). */
#define HAILO_VDMA_DESC_REQUEST_IRQ_PROCESSED  (1u << 2)   /* 0x04 */
#define HAILO_VDMA_DESC_REQUEST_IRQ_ERR        (1u << 3)   /* 0x08 */
#define HAILO_VDMA_DESC_DEVICE_IRQ_BITMASK     (1u << 4)   /* 0x10 */
#define HAILO_VDMA_DESC_HOST_IRQ_BITMASK       (1u << 5)   /* 0x20 */

/* DOMAIN_DEVICE: last-desc bits mirror the reference's
 * get_interrupts_bitmask(DEVICE) output — 0x10 (device bitmask) |
 * 0x04 | 0x08 (always ORed when any domain is selected). Total
 * 0x1C plus the base 0x02 = 0x1E. */
#define HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_DEVICE \
    (HAILO_VDMA_DESC_DESC_CONTROL | \
     HAILO_VDMA_DESC_DEVICE_IRQ_BITMASK | \
     HAILO_VDMA_DESC_REQUEST_IRQ_PROCESSED | \
     HAILO_VDMA_DESC_REQUEST_IRQ_ERR)

/* DOMAIN_BOTH: both device-side and host-side bitmasks set. 0x02 |
 * 0x10 | 0x20 | 0x04 | 0x08 = 0x3E. Not used on the current submit
 * path; retained for reference. */
#define HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_BOTH \
    (HAILO_VDMA_DESC_DESC_CONTROL | \
     HAILO_VDMA_DESC_DEVICE_IRQ_BITMASK | \
     HAILO_VDMA_DESC_HOST_IRQ_BITMASK | \
     HAILO_VDMA_DESC_REQUEST_IRQ_PROCESSED | \
     HAILO_VDMA_DESC_REQUEST_IRQ_ERR)

/* DOMAIN_HOST: host-side IRQ bitmask + REQ_IRQ_{PROCESSED,ERR} +
 * DESC_CONTROL. 0x20 | 0x04 | 0x08 | 0x02 = 0x2E. This is what
 * HailoRT's hailo_pci driver actually emits on per-transfer last
 * descriptors for Hailo-8L boundary channels (verified via
 * instrumented pr_info on Pi OS 2026-04-22; see
 * docs/reference/hailort-v4.23.0-vdma-mnist-pi5.txt). */
#define HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_HOST \
    (HAILO_VDMA_DESC_DESC_CONTROL | \
     HAILO_VDMA_DESC_HOST_IRQ_BITMASK | \
     HAILO_VDMA_DESC_REQUEST_IRQ_PROCESSED | \
     HAILO_VDMA_DESC_REQUEST_IRQ_ERR)

void hailo_vdma_program_descriptor(struct hailo_vdma_descriptor *desc,
                                   uint64_t dma_address,
                                   uint16_t page_size,
                                   uint8_t  data_id)
{
    desc->page_size_desc_control =
        ((uint32_t)page_size << HAILO_VDMA_DESC_PAGE_SIZE_SHIFT)
        + HAILO_VDMA_DESC_DESC_CONTROL;
    desc->addr_l_rsvd_data_id =
        ((uint32_t)(dma_address & HAILO_VDMA_DESC_ADDR_L_MASK))
        | (uint32_t)data_id;
    desc->addr_h                   = (uint32_t)(dma_address >> 32);
    desc->remaining_page_size_status = 0;
}

int hailo_vdma_program_buffer(struct hailo_vdma_desc_list *list,
                              uint32_t starting_desc,
                              uint64_t buffer_iova,
                              uint32_t buffer_size,
                              uint8_t  data_id)
{
    if (!list || !list->descs) return HAILO_ERR_INVAL;
    if (buffer_size == 0) return HAILO_ERR_INVAL;
    if (starting_desc >= list->desc_count && !list->is_circular) {
        return HAILO_ERR_INVAL;
    }

    const uint16_t page_size = list->desc_page_size;
    if (page_size == 0) return HAILO_ERR_INVAL;

    /* DIV_ROUND_UP(buffer_size, page_size). Number of descriptors
     * needed to cover the whole buffer, including any residue in
     * the last descriptor. */
    const uint32_t descs_needed = (buffer_size + page_size - 1u) / page_size;
    const uint32_t residue      = buffer_size % page_size;

    /* For a non-circular list, all descriptors must fit in
     * [starting_desc, desc_count). Circular lists wrap via mask. */
    if (!list->is_circular
     && starting_desc + descs_needed > list->desc_count) {
        return HAILO_ERR_INVAL;
    }

    uint64_t dma_address = buffer_iova;
    for (uint32_t i = 0; i < descs_needed - 1u; i++) {
        uint32_t slot = (starting_desc + i) & list->desc_count_mask;
        hailo_vdma_program_descriptor(&list->descs[slot],
                                      dma_address, page_size, data_id);
        dma_address += page_size;
    }
    /* Last descriptor: residue size if the buffer isn't an exact
     * multiple of page_size, otherwise a full page. Matches the
     * reference driver's pattern at hailo-vdma-common.c:196-198. */
    uint32_t last_slot = (starting_desc + descs_needed - 1u)
                       & list->desc_count_mask;
    uint16_t last_size = (residue == 0) ? page_size : (uint16_t)residue;
    hailo_vdma_program_descriptor(&list->descs[last_slot],
                                  dma_address, last_size, data_id);

    /* #253 last-desc IRQ bits: use DOMAIN_HOST (0x2E), not DEVICE
     * (0x1E). Verified 2026-04-22 via instrumented hailo_pci on
     * Pi OS (docs/reference/hailort-v4.23.0-vdma-mnist-pi5.txt):
     * every per-transfer last-desc ps_ctrl ends in 0x2e, meaning
     * HailoRT uses host_interrupts_bitmask (0x20) | REQ_IRQ_PROCESSED
     * (0x04) | REQ_IRQ_ERR (0x08) | DESC_CONTROL (0x02). Our earlier
     * DOMAIN_DEVICE pick was a mis-read of the reference domain
     * enum. */
    {
        struct hailo_vdma_descriptor *last = &list->descs[last_slot];
        uint32_t ps_ctrl = last->page_size_desc_control;
        ps_ctrl = (ps_ctrl & ~0xFFu) |
                  (uint32_t)HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_HOST;
        last->page_size_desc_control = ps_ctrl;
    }

    /* PMM allocates cacheable kernel memory, so descriptor writes land
     * in L1/L2. BCM2712's PCIe engine claims cache coherency via
     * ACE-Lite but empirically firmware reads stale zeros unless we
     * push the writes to the PoC.
     *
     * Order the descriptor stores against the upcoming cache_clean.
     * `dsb ishst` publishes the prior writes in the Inner-Shareable
     * domain so DC CVAC operates on the values we just wrote (a
     * subsequent doorbell ring then provides the device-visibility
     * barrier — see hailo_vdma_channel_start). Without this, an
     * out-of-order CVAC could clean a stale cacheline whose updated
     * contents are still pending in the store buffer.
     *
     * Bug fix 2026-04-22: this flush previously used `list->descs` as
     * the base, ignoring `starting_desc`. That worked for the standard
     * single-shot inference path (starting_desc=0) but corrupted any
     * caller that programs at a non-zero starting_desc — e.g. the
     * Phase 8 OUT prefetch fill writes desc[1..7] then this flush
     * cleans desc[0..0] for each of those calls, leaving desc[1..7]
     * dirty in cache while DRAM holds stale zeros that fw would read
     * on prefetch.
     *
     * Now correctly flushes the contiguous range
     * [first_slot, first_slot + descs_needed). Wrapping (circular
     * desc lists) is rare for boundary submits and would still be
     * partially handled, but worth a follow-up if circular usage
     * grows.
     *
     * TODO: if a future regression shows the PCIe window actually
     * snoops caches correctly, drop this flush — it costs ~microseconds
     * per submit. Today we can't distinguish "snoop works but fw logic
     * still wrong" from "snoop broken" without this baseline. */
#if defined(__aarch64__)
    __asm__ volatile("dsb ishst" ::: "memory");
#endif
    if (hailo_platform && hailo_platform->cache_clean) {
        uint32_t first_slot = starting_desc & list->desc_count_mask;
        hailo_platform->cache_clean(&list->descs[first_slot],
            (size_t)descs_needed * sizeof(struct hailo_vdma_descriptor));

#ifdef HAILO_WIRE_DEBUG
        /* Verify cache_clean propagated all the way to DRAM.
         *
         * The fw DMA-reads our descriptor list from physical RAM via
         * the BCM2712 PCIe inbound translation. If our cacheline got
         * cleaned only as far as L2 (not to DRAM), or if cache_clean
         * silently no-op'd, fw would read stale zeros and silently
         * give up — which exactly matches the Phase 8 ch=2 symptom.
         *
         * To probe: cache_discard_range uses `dc ivac` (invalidate
         * WITHOUT clean). After cache_clean we issue dc ivac so the
         * cached copy is gone; the next CPU read of the same address
         * is then forced to fetch from DRAM. If the read returns
         * what we wrote, cache_clean propagated correctly — DRAM
         * has the right bytes for fw. If we see zeros or garbage,
         * the cache flush isn't reaching DRAM and that's the bug.
         *
         * Snapshot first, since reading after dc ivac fetches from
         * DRAM (which is what we're testing). */
        struct hailo_vdma_descriptor expected = list->descs[first_slot];
        cache_discard_range(&list->descs[first_slot],
            sizeof(struct hailo_vdma_descriptor));
        struct hailo_vdma_descriptor actual = list->descs[first_slot];
        bool ok = (expected.page_size_desc_control == actual.page_size_desc_control)
               && (expected.addr_l_rsvd_data_id   == actual.addr_l_rsvd_data_id)
               && (expected.addr_h                == actual.addr_h)
               && (expected.remaining_page_size_status == actual.remaining_page_size_status);
        uart_printf("[vdma-cache] desc[%u] %s: ps_ctrl(c=0x%08x d=0x%08x) "
                    "addr_l(c=0x%08x d=0x%08x) addr_h(c=0x%08x d=0x%08x)\r\n",
                    first_slot, ok ? "DRAM-OK" : "DRAM-MISMATCH",
                    expected.page_size_desc_control,
                    actual.page_size_desc_control,
                    expected.addr_l_rsvd_data_id,
                    actual.addr_l_rsvd_data_id,
                    expected.addr_h,
                    actual.addr_h);
#endif
    }

    return (int)descs_needed;
}

/* -------------------------------------------------------------------------- */
/* Channel start / stop / submit                                               */
/* -------------------------------------------------------------------------- */

/* Channel-control bit layout (`hailo-vdma-common.c:20-30`). Control
 * byte lives in bits [7:0] of the CHANNEL_BASE_DWORD. */
#define HAILO_VDMA_CTRL_START            0x01u
#define HAILO_VDMA_CTRL_ABORT_PAUSE      0x02u

/* ceil_log2 helper — returns the smallest k such that (1 << k) >= n. */
static uint8_t vdma_ceil_log2(uint32_t n)
{
    uint8_t k = 0;
    uint32_t v = 1;
    while (v < n) { v <<= 1; k++; }
    return k;
}

static uint32_t channel_base(uint8_t index)
{
    return (uint32_t)index * HAILO_VDMA_CHANNEL_STRIDE;
}

/* Return the offset WITHIN a channel's 32-byte register block for
 * the HOST-side register half. Per hailo-vdma-common.c:576
 * (get_channel_regs): H2D channels (the low HAILO_VDMA_H2D_CHANNEL_COUNT
 * ids, i.e. in HAILO_PCIE_DMA_SRC_CHANNELS_BITMASK) keep host regs
 * at HOST_REGS_OFFSET_H2D; D2H channels have host regs at
 * HOST_REGS_OFFSET_D2H because the 32-byte window is laid out
 * {device, host} for D2H vs {host, device} for H2D. Writes that
 * should land on the host-side (num_avail, CONTROL, ALIGNED_ADDR_L,
 * ADDR_H, NUM_PROC) go through this offset; missing it silently
 * updates the device-side mirror and the transfer never starts
 * (Phase 8 #253, 2026-04-22). */
static uint32_t channel_host_regs_offset(uint8_t channel_index)
{
    return (channel_index < HAILO_VDMA_H2D_CHANNEL_COUNT)
         ? HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_H2D
         : HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_D2H;
}

/* Convenience: full BAR2 byte offset of the HOST-side `reg_off` for
 * `channel_index`. Centralizes the channel_base + host_offset + reg
 * arithmetic so call sites can't accidentally drop the host_offset
 * adjustment (the exact bug class Phase 8 #253 introduced). */
static uint32_t channel_host_reg(uint8_t channel_index, uint32_t reg_off)
{
    return channel_base(channel_index)
         + channel_host_regs_offset(channel_index)
         + reg_off;
}

/* Read the HOST-side BASE_DWORD. */
static uint32_t channel_read_base_dword(uint8_t channel_index)
{
    return hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_BASE_DWORD));
}

/* Write the HOST-side BASE_DWORD (control+depth_id+num_avail packed). */
static void channel_write_base_dword(uint8_t channel_index, uint32_t value)
{
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_BASE_DWORD),
        value);
}

int hailo_vdma_channel_start(uint8_t channel_index,
                             const struct hailo_vdma_desc_list *list,
                             uint8_t data_id)
{
    if (!list || !list->descs) return HAILO_ERR_INVAL;
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return HAILO_ERR_INVAL;
    if (list->iova & 0xFFFFu) return HAILO_ERR_INVAL;  /* 64 KB-aligned */
    if (!hailo_platform) return HAILO_ERR_NODEV;

    /* Stop any prior activity on the channel before reprogramming. */
    hailo_vdma_channel_stop(channel_index);

    /* desc_depth = ceil_log2(desc_count). Reference maps depth==16
     * to depth==0 for the "full list" case (hailo-vdma-common.c:874). */
    uint8_t depth = vdma_ceil_log2(list->desc_count);
    if (depth == 16) depth = 0;

    /* Write sequence matches hailo_vdma_start_channel
     * (hailo-vdma-common.c:859-894):
     *
     *   1. ALIGNED_ADDR_L dword: bits [31:16] = address_l (the high
     *      16 bits of the list's 32-bit-low iova). RMW to preserve
     *      any other fields in that dword.
     *   2. ADDR_H dword: bits [31:0] = address_h (high 32 bits of
     *      iova).
     *   3. BASE_DWORD: write (depth << 11) | (data_id << 8). This
     *      also clears CONTROL (bits [7:0]), matching what the
     *      reference does at line 892.
     *   4. BASE_DWORD RMW: set CONTROL to START while preserving
     *      the freshly-written DEPTH + DATA_ID bits.
     */
    uint16_t addr_l = (uint16_t)((list->iova >> 16) & 0xFFFFu);
    uint32_t addr_h = (uint32_t)(list->iova >> 32);

    uint32_t aligned = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L));
    aligned = (aligned & 0x0000FFFFu) | ((uint32_t)addr_l << 16);
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L),
        aligned);
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_ADDR_H),
        addr_h);

    uint32_t depth_id_dword =
        ((uint32_t)depth << HAILO_VDMA_CHANNEL_DESC_DEPTH_SHIFT)
      | ((uint32_t)data_id << HAILO_VDMA_CHANNEL_DATA_ID_SHIFT);
    channel_write_base_dword(channel_index, depth_id_dword);

    /* Issue the START control bit. Read-modify-write the base dword
     * so we don't clobber DEPTH + DATA_ID just written. */
    uint32_t cur = channel_read_base_dword(channel_index);
    cur = (cur & ~0xFFu) | HAILO_VDMA_CTRL_START;
    channel_write_base_dword(channel_index, cur);
    hailo_platform->mb();
    return HAILO_OK;
}

void hailo_vdma_channel_stop(uint8_t channel_index)
{
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return;
    if (!hailo_platform) return;

    uint32_t cur = channel_read_base_dword(channel_index);
    uint8_t ctrl = (uint8_t)(cur & 0xFFu);

    /* If already in ABORT_PAUSE, nothing to do. Matches
     * hailo-vdma-common.c:937. */
    if ((ctrl & 0x03u) == HAILO_VDMA_CTRL_ABORT_PAUSE) return;

    /* Pause → abort-pause sequence. Writes the new control byte
     * into bits[7:0] of the base dword. */
    cur = (cur & ~0xFFu) | HAILO_VDMA_CTRL_ABORT_PAUSE;
    channel_write_base_dword(channel_index, cur);
    hailo_platform->mb();
}

int hailo_vdma_submit_and_wait(uint8_t channel_index,
                               uint16_t new_num_avail,
                               uint32_t timeout_us)
{
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return HAILO_ERR_INVAL;
    if (!hailo_platform) return HAILO_ERR_NODEV;

    /* Diagnostic snapshot before num_avail write. */
    uint32_t base_pre = channel_read_base_dword(channel_index);
    uint32_t proc_pre = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));

    /* Write new_num_avail into bits [31:16] of the base dword.
     * Read-modify-write to preserve CONTROL and DEPTH_ID. */
    uint32_t cur = (base_pre & 0x0000FFFFu)
        | ((uint32_t)new_num_avail << HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT);
    channel_write_base_dword(channel_index, cur);
    hailo_platform->mb();

    uint32_t base_post = channel_read_base_dword(channel_index);

#ifdef HAILO_WIRE_DEBUG
    uart_printf("[vdma] ch=%u new_avail=%u base_pre=0x%08x "
                "base_post=0x%08x proc_pre=0x%08x\r\n",
                (unsigned)channel_index, (unsigned)new_num_avail,
                (unsigned)base_pre, (unsigned)base_post,
                (unsigned)proc_pre);
#else
    (void)base_pre;
    (void)base_post;
    (void)proc_pre;
#endif

    /* Poll NUM_PROC (low 16 bits of NUM_PROC_DWORD). Yield via
     * udelay between polls to stay cooperative on Pi 5.
     *
     * #253 diagnostic: on every change to proc_dword OR base_dword
     * (either side of the channel block), log the transition. Also
     * emit a heartbeat snapshot every 50 ms so long stalls show a
     * flat-line pattern rather than silence. This lets us tell:
     *   - whether fw moved ongoing/proc at all during the wait
     *   - whether fw changed CONTROL (e.g. to PAUSE on error)
     *   - whether avail somehow got clobbered
     * Gated behind HAILO_WIRE_DEBUG; OFF builds keep the tight loop.
     *
     * Caveat: the WIRE_DEBUG path issues 4 MMIO reads per 100 µs
     * iteration (host proc/base + device proc/base mirrors). Across
     * a multi-second wait that is hundreds of thousands of PCIe
     * round-trips to the NPU; each read is a posted-completion
     * exchange and may itself perturb fw's internal state. For
     * timing-sensitive bugs prefer adding a single targeted log
     * over enabling this whole block. The merge-ready build does
     * not define HAILO_WIRE_DEBUG. */
    const uint32_t poll_interval_us = 100u;
    uint32_t elapsed = 0;
#ifdef HAILO_WIRE_DEBUG
    uint32_t last_proc_dword = 0xFFFFFFFFu;
    uint32_t last_base_dword_host = 0xFFFFFFFFu;
    uint32_t last_proc_dword_dev  = 0xFFFFFFFFu;
    uint32_t last_base_dword_dev  = 0xFFFFFFFFu;
    uint32_t heartbeat_us = 0;
    /* Device-side mirror lives in the OTHER half of the 32-byte
     * channel block from where host regs sit. */
    const uint32_t dev_off =
        (channel_host_regs_offset(channel_index)
            == HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_H2D)
            ? HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_D2H
            : HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_H2D;
#endif
    while (elapsed < timeout_us) {
        uint32_t proc_dword = hailo_platform->read32(HAILO_BAR_VDMA,
            channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));
        uint16_t num_proc = (uint16_t)(proc_dword & 0xFFFFu);
        if (num_proc == new_num_avail) {
            uart_printf("[vdma] ch=%u done after %u us proc=0x%08x\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)proc_dword);
            return HAILO_OK;
        }
#ifdef HAILO_WIRE_DEBUG
        /* Change-of-state logging. */
        if (proc_dword != last_proc_dword) {
            uart_printf("[vdma-poll] ch=%u t=%u us host proc_dword "
                        "0x%08x -> 0x%08x (proc=%u ongoing=%u)\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)last_proc_dword, (unsigned)proc_dword,
                        (unsigned)(proc_dword & 0xFFFFu),
                        (unsigned)(proc_dword >> 16));
            last_proc_dword = proc_dword;
        }
        uint32_t base_dword = channel_read_base_dword(channel_index);
        if (base_dword != last_base_dword_host) {
            uart_printf("[vdma-poll] ch=%u t=%u us host base_dword "
                        "0x%08x -> 0x%08x (ctrl=0x%02x avail=%u)\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)last_base_dword_host, (unsigned)base_dword,
                        (unsigned)(base_dword & 0xFFu),
                        (unsigned)(base_dword >> HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT));
            last_base_dword_host = base_dword;
        }
        /* Also sample the opposite-side (device-side) mirror to spot
         * fw progress there — fw may advance its internal counters
         * before publishing to the host-side window. */
        uint32_t proc_dev = hailo_platform->read32(HAILO_BAR_VDMA,
            channel_base(channel_index) + dev_off
            + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);
        uint32_t base_dev = hailo_platform->read32(HAILO_BAR_VDMA,
            channel_base(channel_index) + dev_off
            + HAILO_VDMA_CHANNEL_BASE_DWORD);
        if (proc_dev != last_proc_dword_dev) {
            uart_printf("[vdma-poll] ch=%u t=%u us dev  proc_dword "
                        "0x%08x -> 0x%08x\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)last_proc_dword_dev, (unsigned)proc_dev);
            last_proc_dword_dev = proc_dev;
        }
        if (base_dev != last_base_dword_dev) {
            uart_printf("[vdma-poll] ch=%u t=%u us dev  base_dword "
                        "0x%08x -> 0x%08x\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)last_base_dword_dev, (unsigned)base_dev);
            last_base_dword_dev = base_dev;
        }
        /* 50 ms heartbeat so we can see poll density in the log. */
        heartbeat_us += poll_interval_us;
        if (heartbeat_us >= 50000u) {
            uart_printf("[vdma-poll] ch=%u t=%u us heartbeat "
                        "host(proc=0x%08x base=0x%08x) dev(proc=0x%08x base=0x%08x)\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)proc_dword, (unsigned)base_dword,
                        (unsigned)proc_dev, (unsigned)base_dev);
            heartbeat_us = 0;
        }
#endif
        hailo_platform->udelay(poll_interval_us);
        elapsed += poll_interval_us;
    }
    uint32_t proc_end = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));
    uint32_t base_end = channel_read_base_dword(channel_index);
    uart_printf("[vdma] ch=%u TIMEOUT proc_end=0x%08x base_end=0x%08x\r\n",
                (unsigned)channel_index, (unsigned)proc_end,
                (unsigned)base_end);
    return HAILO_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------- */
/* Diagnostic dumps — Phase 8 inference-submit investigation (#253)           */
/* -------------------------------------------------------------------------- */

void hailo_vdma_dump_desc_list(const struct hailo_vdma_desc_list *list,
                               const char *label,
                               uint32_t max_descs)
{
    if (!list || !list->descs || !label) return;
    uart_printf("[desc] %s list iova=0x%lx page_size=%u "
                "desc_count=%u circular=%d\r\n",
                label, (uint64_t)list->iova,
                (unsigned)list->desc_page_size,
                (unsigned)list->desc_count,
                (int)list->is_circular);
    uint32_t n = (max_descs < list->desc_count) ? max_descs : list->desc_count;
    for (uint32_t i = 0; i < n; i++) {
        const struct hailo_vdma_descriptor *d = &list->descs[i];
        uint32_t ps_ctrl = d->page_size_desc_control;
        uint32_t ctrl    = ps_ctrl & 0xFFu;
        uint32_t page    = ps_ctrl >> HAILO_VDMA_DESC_PAGE_SIZE_SHIFT;
        uint32_t addr_l  = d->addr_l_rsvd_data_id & HAILO_VDMA_DESC_ADDR_L_MASK;
        uint32_t data_id = d->addr_l_rsvd_data_id & 0x3Fu;
        uart_printf("[desc] %s[%u] ps_ctrl=0x%08x (page=%u ctrl=0x%02x) "
                    "addr_l=0x%08x data_id=0x%02x addr_h=0x%08x "
                    "rem_status=0x%08x\r\n",
                    label, (unsigned)i,
                    (unsigned)ps_ctrl, (unsigned)page, (unsigned)ctrl,
                    (unsigned)addr_l, (unsigned)data_id,
                    (unsigned)d->addr_h,
                    (unsigned)d->remaining_page_size_status);
    }
}

void hailo_vdma_dump_desc_status(const struct hailo_vdma_desc_list *list,
                                 const char *label,
                                 uint32_t max_descs)
{
    if (!list || !list->descs || !label) return;
    uint32_t n = (max_descs < list->desc_count) ? max_descs : list->desc_count;
    if (n == 0) return;

    /* Descriptor write-backs come from device DMA, bypassing host
     * cache. Without invalidate, we'd see the zero we wrote at
     * program time even if fw has since updated DRAM. */
    if (hailo_platform && hailo_platform->cache_invalidate) {
        hailo_platform->cache_invalidate(list->descs,
            (size_t)n * sizeof(struct hailo_vdma_descriptor));
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t rps = list->descs[i].remaining_page_size_status;
        uint8_t  status = (uint8_t)(rps & 0xFFu);
        const char *done = (status & 0x01u) ? "DONE" : "    ";
        const char *err  = (status & 0x02u) ? "ERR " : "    ";
        uart_printf("[desc-status] %s desc[%u] status=0x%02x %s %s "
                    "rps_full=0x%08x\r\n",
                    label, (unsigned)i, (unsigned)status, done, err,
                    (unsigned)rps);
    }
}

/* Dump one 16-byte register block (host-side OR device-side) with
 * the BASE_DWORD decoded into its named subfields. `side_offset` is
 * 0 for the block at `channel_base + 0` and 0x10 for the block at
 * `+0x10`. Which side that corresponds to (host vs device) depends
 * on the channel's direction: H2D (ch 0..15) puts host-side at +0
 * and device-side at +0x10; D2H (ch 16..31) swaps them. */
static void dump_channel_block(uint8_t channel_index, uint32_t side_offset,
                               const char *block_label)
{
    uint32_t base = channel_base(channel_index) + side_offset;
    uint32_t base_dw = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_BASE_DWORD);
    uint32_t proc_dw = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);
    uint32_t addr_l  = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L);
    uint32_t addr_h  = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_ADDR_H);
    uart_printf("[chan] %s ch=%u +0x%02x base=0x%08x "
                "(ctrl=0x%02x did=%u depth=%u avail=%u) "
                "proc=0x%08x (proc=%u ongoing=%u) "
                "addr_l=0x%08x addr_h=0x%08x\r\n",
                block_label,
                (unsigned)channel_index, (unsigned)side_offset,
                (unsigned)base_dw,
                (unsigned)(base_dw & 0xFFu),
                (unsigned)((base_dw >> HAILO_VDMA_CHANNEL_DATA_ID_SHIFT) & 0x7u),
                (unsigned)((base_dw >> HAILO_VDMA_CHANNEL_DESC_DEPTH_SHIFT) & 0xFu),
                (unsigned)(base_dw >> HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT),
                (unsigned)proc_dw,
                (unsigned)(proc_dw & 0xFFFFu),
                (unsigned)(proc_dw >> 16),
                (unsigned)addr_l, (unsigned)addr_h);
}

int hailo_vdma_channel_wait_armed(uint8_t channel_index, uint32_t timeout_us)
{
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return HAILO_ERR_INVAL;
    if (!hailo_platform) return HAILO_ERR_NODEV;

    const uint32_t poll_us = 100u;
    uint32_t elapsed = 0;
    while (elapsed < timeout_us) {
        uint32_t base = channel_read_base_dword(channel_index);
        if ((base & 0xFFu) == HAILO_VDMA_CTRL_START) return HAILO_OK;
        hailo_platform->udelay(poll_us);
        elapsed += poll_us;
    }
    return HAILO_ERR_TIMEOUT;
}

int hailo_vdma_write_num_avail(uint8_t channel_index, uint16_t num_avail)
{
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return HAILO_ERR_INVAL;
    if (!hailo_platform) return HAILO_ERR_NODEV;

    /* channel_read_base_dword / channel_write_base_dword apply the
     * host-regs offset internally (see channel_host_regs_offset). */
    uint32_t base_pre  = channel_read_base_dword(channel_index);
    uint32_t base_post = (base_pre & 0x0000FFFFu)
        | ((uint32_t)num_avail << HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT);
    channel_write_base_dword(channel_index, base_post);
    hailo_platform->mb();

#ifdef HAILO_WIRE_DEBUG
    uart_printf("[vdma] ch=%u write_avail host_off=0x%02x pre=0x%08x "
                "post=0x%08x avail=%u\r\n",
                (unsigned)channel_index,
                (unsigned)channel_host_regs_offset(channel_index),
                (unsigned)base_pre, (unsigned)base_post,
                (unsigned)num_avail);
#else
    (void)base_pre;
    (void)base_post;
#endif
    return HAILO_OK;
}

int hailo_vdma_arm_first_desc_irq(struct hailo_vdma_desc_list *list,
                                  uint32_t starting_desc,
                                  uint32_t ctrl_mask)
{
    if (!list || !list->descs) return HAILO_ERR_INVAL;
    if (starting_desc >= list->desc_count) return HAILO_ERR_INVAL;

    uint32_t slot = starting_desc & list->desc_count_mask;
    struct hailo_vdma_descriptor *d = &list->descs[slot];
    d->page_size_desc_control |= (ctrl_mask & 0xFFu);

    if (hailo_platform && hailo_platform->cache_clean) {
        hailo_platform->cache_clean(d, sizeof(*d));
    }
    return HAILO_OK;
}

int hailo_vdma_channel_wait_proc(uint8_t channel_index,
                                 uint16_t target_num_proc,
                                 uint32_t timeout_us)
{
    if (channel_index >= HAILO_VDMA_MAX_CHANNELS) return HAILO_ERR_INVAL;
    if (!hailo_platform) return HAILO_ERR_NODEV;

#ifdef HAILO_WIRE_DEBUG
    uint32_t proc_pre = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));
    uart_printf("[vdma] ch=%u wait_proc target=%u proc_pre=0x%08x\r\n",
                (unsigned)channel_index, (unsigned)target_num_proc,
                (unsigned)proc_pre);
#endif

    /* target_num_proc is the ABSOLUTE num_proc value fw should reach
     * once all descriptors in the channel's desc list have been
     * processed. Tolerate off-by-one (some fw revisions don't
     * increment proc for the final LAST_DESC_CTRL descriptor). */
    const uint32_t poll_interval_us = 100u;
    uint32_t elapsed = 0;
    while (elapsed < timeout_us) {
        uint32_t proc_dword = hailo_platform->read32(HAILO_BAR_VDMA,
            channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));
        uint16_t num_proc = (uint16_t)(proc_dword & 0xFFFFu);
        if (num_proc >= target_num_proc
            || (target_num_proc > 0 && num_proc == target_num_proc - 1)) {
            /* Flag the off-by-one path so future regressions where fw
             * legitimately stalls one short don't silently pass —
             * LAST_DESC_CTRL quirk was the only known trigger. */
            if (target_num_proc > 0 && num_proc == target_num_proc - 1) {
                WARN("[vdma] ch=%u wait_proc accepted off-by-one "
                     "(target=%u, proc=%u) — LAST_DESC_CTRL quirk?",
                     (unsigned)channel_index,
                     (unsigned)target_num_proc, (unsigned)num_proc);
            }
            uart_printf("[vdma] ch=%u wait_proc done after %u us "
                        "proc=0x%08x\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)proc_dword);
            return HAILO_OK;
        }
        hailo_platform->udelay(poll_interval_us);
        elapsed += poll_interval_us;
    }
    uint32_t proc_end = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_host_reg(channel_index, HAILO_VDMA_CHANNEL_NUM_PROC_DWORD));
    uart_printf("[vdma] ch=%u wait_proc TIMEOUT proc_end=0x%08x\r\n",
                (unsigned)channel_index, (unsigned)proc_end);
    return HAILO_ERR_TIMEOUT;
}

void hailo_vdma_dump_channel_regs(uint8_t channel_index, const char *label)
{
    if (!hailo_platform || channel_index >= HAILO_VDMA_MAX_CHANNELS) return;
    /* Dump both halves of the channel's 32-byte window. Direction
     * labels (host/device) are inferred from HAILO_PCIE_DMA_SRC_CHANNELS_
     * BITMASK=0x0000FFFF at the call site; here we just say "+0x00"
     * and "+0x10" so readers can decode against the direction. */
    dump_channel_block(channel_index, 0x00u, label ? label : "");
    dump_channel_block(channel_index, 0x10u, label ? label : "");
}
