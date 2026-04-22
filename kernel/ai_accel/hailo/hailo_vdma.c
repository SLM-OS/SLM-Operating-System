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

int hailo_vdma_desc_list_alloc(uint32_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
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
    void *cpu = hailo_platform->dma_alloc((size_t)alloc_size,
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
 * 0x10 | 0x20 | 0x04 | 0x08 = 0x3E. HailoRT uses this for async
 * transfers that both (a) signal the NPU that data arrived and
 * (b) signal the host when the transfer completes. Maximum-signal
 * variant — if DOMAIN_DEVICE alone doesn't unblock the submit, this
 * rules out "wrong domain selected" as a variable. */
#define HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_BOTH \
    (HAILO_VDMA_DESC_DESC_CONTROL | \
     HAILO_VDMA_DESC_DEVICE_IRQ_BITMASK | \
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

    /* #253 last-desc IRQ bits: OR the DOMAIN_DEVICE bitmask into the
     * final descriptor's control byte, mirroring reference
     * bind_and_program_descriptors_list (hailo-vdma-common.c:313-314).
     * Hypothesis: firmware's BURST_CREDITS_TASK uses
     * DESC_REQUEST_IRQ_PROCESSED (0x04) as the transfer-end marker
     * even when the host is polling. Plain 0x02-only descriptors may
     * get silently treated as "not last", which would explain why our
     * num_proc stays at 0 despite num_avail being accepted. */
    {
        struct hailo_vdma_descriptor *last = &list->descs[last_slot];
        uint32_t ps_ctrl = last->page_size_desc_control;
        ps_ctrl = (ps_ctrl & ~0xFFu) |
                  (uint32_t)HAILO_VDMA_LAST_DESC_CTRL_DOMAIN_DEVICE;
        last->page_size_desc_control = ps_ctrl;
    }

    /* PMM allocates cacheable kernel memory, so descriptor writes land
     * in L1/L2. BCM2712's PCIe engine claims cache coherency via
     * ACE-Lite but empirically firmware reads stale zeros unless we
     * push the writes to the PoC. One clean covers all programmed
     * slots since they were written sequentially.
     *
     * TODO: if a future regression shows the PCIe window actually
     * snoops caches correctly, drop this flush — it costs ~microseconds
     * per submit. Today we can't distinguish "snoop works but fw logic
     * still wrong" from "snoop broken" without this baseline. */
    if (hailo_platform && hailo_platform->cache_clean) {
        hailo_platform->cache_clean(list->descs,
            (size_t)descs_needed * sizeof(struct hailo_vdma_descriptor));
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

/* Read the CHANNEL_BASE_DWORD and return the 32-bit value. */
static uint32_t channel_read_base_dword(uint8_t channel_index)
{
    return hailo_platform->read32(HAILO_BAR_VDMA,
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_BASE_DWORD);
}

/* Write the CHANNEL_BASE_DWORD (control+depth_id+num_avail packed). */
static void channel_write_base_dword(uint8_t channel_index, uint32_t value)
{
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_BASE_DWORD,
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
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L);
    aligned = (aligned & 0x0000FFFFu) | ((uint32_t)addr_l << 16);
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L,
        aligned);
    hailo_platform->write32(HAILO_BAR_VDMA,
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_ADDR_H,
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
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);

    /* Write new_num_avail into bits [31:16] of the base dword.
     * Read-modify-write to preserve CONTROL and DEPTH_ID. */
    uint32_t cur = (base_pre & 0x0000FFFFu)
        | ((uint32_t)new_num_avail << HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT);
    channel_write_base_dword(channel_index, cur);
    hailo_platform->mb();

    uint32_t base_post = channel_read_base_dword(channel_index);

    uart_printf("[vdma] ch=%u new_avail=%u base_pre=0x%08x "
                "base_post=0x%08x proc_pre=0x%08x\r\n",
                (unsigned)channel_index, (unsigned)new_num_avail,
                (unsigned)base_pre, (unsigned)base_post,
                (unsigned)proc_pre);

    /* Poll NUM_PROC (low 16 bits of NUM_PROC_DWORD). Yield via
     * udelay between polls to stay cooperative on Pi 5. */
    const uint32_t poll_interval_us = 100u;
    uint32_t elapsed = 0;
    while (elapsed < timeout_us) {
        uint32_t proc_dword = hailo_platform->read32(HAILO_BAR_VDMA,
            channel_base(channel_index)
            + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);
        uint16_t num_proc = (uint16_t)(proc_dword & 0xFFFFu);
        if (num_proc == new_num_avail) {
            uart_printf("[vdma] ch=%u done after %u us proc=0x%08x\r\n",
                        (unsigned)channel_index, (unsigned)elapsed,
                        (unsigned)proc_dword);
            return HAILO_OK;
        }
        hailo_platform->udelay(poll_interval_us);
        elapsed += poll_interval_us;
    }
    uint32_t proc_end = hailo_platform->read32(HAILO_BAR_VDMA,
        channel_base(channel_index) + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);
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

void hailo_vdma_dump_channel_regs(uint8_t channel_index, const char *label)
{
    if (!hailo_platform || channel_index >= HAILO_VDMA_MAX_CHANNELS) return;
    uint32_t base = channel_base(channel_index);
    uint32_t base_dw = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_BASE_DWORD);
    uint32_t proc_dw = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_NUM_PROC_DWORD);
    uint32_t addr_l  = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L);
    uint32_t addr_h  = hailo_platform->read32(HAILO_BAR_VDMA,
        base + HAILO_VDMA_CHANNEL_ADDR_H);
    uart_printf("[chan] %s ch=%u base=0x%08x (ctrl=0x%02x data_id=%u "
                "depth=%u num_avail=%u) proc=0x%08x (proc=%u ongoing=%u) "
                "aligned_addr_l=0x%08x addr_h=0x%08x\r\n",
                label ? label : "",
                (unsigned)channel_index, (unsigned)base_dw,
                (unsigned)(base_dw & 0xFFu),
                (unsigned)((base_dw >> HAILO_VDMA_CHANNEL_DATA_ID_SHIFT) & 0x7u),
                (unsigned)((base_dw >> HAILO_VDMA_CHANNEL_DESC_DEPTH_SHIFT) & 0xFu),
                (unsigned)(base_dw >> HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT),
                (unsigned)proc_dw,
                (unsigned)(proc_dw & 0xFFFFu),
                (unsigned)(proc_dw >> 16),
                (unsigned)addr_l, (unsigned)addr_h);
}
