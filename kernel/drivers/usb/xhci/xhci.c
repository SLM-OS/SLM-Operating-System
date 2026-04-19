/*
 * xhci.c - SLM-OS xHCI host controller driver (#266 Phase 3A)
 *
 * Phase 3A Step 2 + 3: controller scaffolding, capability probe, and
 * halt/reset sequence. No rings, no ports, no transfers yet — those
 * come in Steps 4-7.
 *
 * Only built on Jetson; every other platform sees this file compiled
 * out by the #if at the top (no usb_hcd registration happens).
 *
 * File structure (~620 lines in the Jetson-gated body):
 *   1. Base pointers + static state
 *   2. Register access helpers (r8/r16/r32/w32)
 *   3. Halt + reset sequence
 *   4. Capability parse
 *   5. Scratchpad + DCBAA + ERST allocation (NC memory)
 *   6. MMIO register programming (xhci_program_registers)
 *   7. Controller start + NO_OP round-trip
 *   8. Public API (xhci_init, xhci_dump_info, xhci_get_caps)
 *
 * Natural split points if this grows past ~1000 lines (Steps 5-7
 * add port scan + ENABLE_SLOT/ADDRESS_DEVICE + transfer TRBs):
 *   - xhci_mem.c: sections 5
 *   - xhci_regs.c: sections 2, 3, 6
 *   - xhci_cmd.c: section 7 + future transfer path
 *   - xhci.c: sections 1, 4, 8
 *
 * Split is deliberately DEFERRED while Phase 3A remains mothballed
 * (#285). Splitting ~620 lines of unchanging code for no behaviour
 * change is churn; the structure is small enough to navigate with
 * section banners; and if #266 is revived via #286, the seams above
 * are already visible for whoever picks it up. Linux, U-Boot, and
 * Haiku all keep their xHCI core driver as a single TU at similar
 * size, so the "single file" shape is also the idiomatic one.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "xhci.h"
#include "xhci_internal.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "xhci_trb.h"
#include "xhci_tegra.h"
#include "usb.h"
#include "ncmem.h"
#include "debug.h"
#include "timer.h"
#include "spinlock.h"   /* dsb()/dmb() cross-platform barrier macros */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Base pointers                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Intel-spec xHCI aperture base pointers.
 *
 * TEGRA_XHCI_HCD_BASE is the start of the standard xHCI aperture
 * (Intel-spec). The VMM already maps a 2 MB block at 0x03600000
 * covering the Tegra FPCI prefix, the standard xHCI block, and the
 * BAR2 wrapper aperture.
 */
/* Non-static: shared with xhci_device.c / xhci_xfer.c via xhci_internal.h. */
volatile uint8_t *xhci_cap_base;
volatile uint8_t *xhci_op_base;
volatile uint8_t *xhci_rt_base;
volatile uint8_t *xhci_db_base;

/*
 * Tegra-specific wrapper aperture base pointers. Populated in
 * xhci_init after the FPCI dev/vendor sanity check passes.
 *
 * FPCI: PCI-style config window + ARU mailbox + CSB paging window.
 *       Must be programmed (XUSB_CFG_4 BAR0, XUSB_CFG_7 BAR2,
 *       XUSB_CFG_1 bus-master) before the xHCI op registers will
 *       respond to RUN=1 — see tegra_xusb_config in
 *       docs/reference/linux-xhci-tegra.c:786-830.
 *
 * BAR2: Tegra234-unique wrapper aperture carrying the mailbox
 *       (MBOX_CMD/DATA_IN/DATA_OUT/OWNER), IFR DMA config, CSB
 *       paging (ARU_C11_CSBRANGE at 0x09c, CSB_BASE_ADDR at 0x2000),
 *       and the firmware-scratch window used by the IFR boot path
 *       to expose its timestamp/header.
 */
static volatile uint8_t *xhci_fpci_base;
static volatile uint8_t *xhci_bar2_base;

/* Non-static: shared with xhci_device.c / xhci_xfer.c. */
struct xhci_caps      xhci_caps_cached;
bool                  xhci_live;

/* Step 4 state */
#define XHCI_CMD_RING_TRBS           64
#define XHCI_EVT_RING_TRBS           64
#define XHCI_PAGESIZE_DEFAULT        4096

/* Non-static: DCBAA + ring state shared across TUs. */
uint64_t             *xhci_dcbaa;          /* aligned(64), MaxSlots+1 entries */
static uint64_t             *xhci_scratchpad_ptrs;
static void                 *xhci_scratchpad_bufs; /* N pages × PAGESIZE */
static uint32_t              xhci_num_scratchpads;

struct xhci_ring       xhci_cmd_ring;
struct xhci_event_ring xhci_evt_ring;

/*
 * Event Ring Segment Table entry layout per xHCI 1.2 §6.5. One entry
 * is enough — Phase 3A uses a single event-ring segment.
 *
 * ERST is DMA-read by the host controller, so the backing storage
 * MUST be either NC memory or clean-to-PoC before we program ERSTBA.
 * We allocate from ncmem to match DCBAA / scratchpads / rings — every
 * other HC-visible buffer in this driver goes through ncmem_alloc, so
 * ERST is kept consistent. A cacheable static would need an explicit
 * cache_clean_range before every ERSTBA write.
 */
struct xhci_erst_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;          /* low 16 bits = segment TRB count */
    uint32_t reserved;
};
_Static_assert(sizeof(struct xhci_erst_entry) == 16, "ERST entry 16B");
/*
 * If this layout ever changes (new fields, reordering, explicit
 * alignment), update the offset mirror in
 * `kernel/tests/test_xhci_ring.c::test_erst_entry_layout` to match.
 * The static_assert above catches size drift; the test catches
 * field-offset drift because it re-declares a local copy.
 */

static struct xhci_erst_entry *xhci_erst;

/* -------------------------------------------------------------------------- */
/* Register access helpers                                                     */
/* -------------------------------------------------------------------------- */

static uint8_t  r8(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint8_t *)(base + off);
}
static uint16_t r16(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint16_t *)(base + off);
}
static uint32_t r32(const volatile uint8_t *base, uint32_t off)
{
    return *(const volatile uint32_t *)(base + off);
}
static void w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/*
 * Thin named wrappers for the Tegra wrapper apertures. They just
 * forward to r32/w32 with the right base pointer, but having a
 * named accessor per aperture makes call sites read the same way
 * Linux's fpci_readl/bar2_readl do — which matters when porting
 * from the reference driver under docs/reference/.
 */
static uint32_t fpci_r32(uint32_t off) { return r32(xhci_fpci_base, off); }
static void     fpci_w32(uint32_t off, uint32_t v) { w32(xhci_fpci_base, off, v); }
static uint32_t bar2_r32(uint32_t off) { return r32(xhci_bar2_base, off); }
static void     bar2_w32(uint32_t off, uint32_t v) { w32(xhci_bar2_base, off, v); }

/* -------------------------------------------------------------------------- */
/* Polling utility (used by Tegra-section Falcon probes + halt/reset below)    */
/* -------------------------------------------------------------------------- */

/*
 * Spin-poll a register bit until (val & mask) == expected or the
 * timeout expires. Uses CNTPCT-based wall-clock so a locked-up
 * controller cannot wedge the boot path.
 *
 * Returns 0 on success, -1 on timeout.
 */
static int poll_reg32(volatile uint8_t *base, uint32_t off,
                      uint32_t mask, uint32_t expected,
                      uint32_t timeout_ms)
{
    uint64_t start = timer_get_count();
    uint64_t freq  = timer_get_frequency();
    uint64_t ticks = (freq * timeout_ms) / 1000;

    for (;;) {
        if ((r32(base, off) & mask) == expected)
            return 0;
        if (timer_get_count() - start > ticks)
            return -1;
    }
}

/* -------------------------------------------------------------------------- */
/* Tegra234 FPCI wrapper programming (tegra_xusb_config equivalent)            */
/* -------------------------------------------------------------------------- */

/*
 * Port of Linux's tegra_xusb_config (linux-xhci-tegra.c:786-830) for
 * Tegra234 (has_ipfs = false, has_bar2 = true). Three register writes:
 *
 *   XUSB_CFG_4  — BAR0 address (points the Falcon at the XUSB MMIO
 *                 aperture). Bits [31:15] latch the base; the low
 *                 15 bits are reserved (32 KB alignment).
 *   XUSB_CFG_7  — BAR2 address (Tegra234-only — earlier Tegras skip
 *                 this). Bits [31:16] latch the base; low 16 bits
 *                 reserved (64 KB alignment).
 *   XUSB_CFG_1  — OR in IO_SPACE_EN | MEM_SPACE_EN | BUS_MASTER_EN so
 *                 the Falcon can issue DMA.
 *
 * Linux's flow runs this once during probe, after clocks/PHYs/power
 * domains are up. Post-kexec, first-hardware testing (jetson-nano-1,
 * feature/xhci-ifr-bringup) showed Linux leaves ALL THREE of these
 * registers correctly programmed — CFG_1 already has BUS_MASTER_EN,
 * CFG_4 points at the XUSB aperture, CFG_7 points at BAR2. The
 * original hypothesis that RUN=1 wedged because BUS_MASTER was
 * cleared turned out to be wrong.
 *
 * This function is kept as a defensive no-op: read-modify-write
 * preserves any existing bits and re-asserts the required ones. If
 * a future Jetson variant or kernel version DOES clear one of these
 * bits on kexec teardown, we'll set it here before RUN=1 runs. The
 * real #285 blocker is elsewhere (Falcon liveness / IFR state /
 * SMMU — investigated by Phase 3A.2.4 onward).
 *
 * Returns 0 always — the writes have no observable completion code
 * beyond "subsequent HCD reads are valid", which is verified by the
 * CAPLENGTH read at the top of xhci_init.
 */
static int tegra_xusb_config(void)
{
    INFO("xhci: pre-config CFG_1=0x%08x CFG_4=0x%08x CFG_7=0x%08x",
         (unsigned)fpci_r32(XUSB_CFG_1),
         (unsigned)fpci_r32(XUSB_CFG_4),
         (unsigned)fpci_r32(XUSB_CFG_7));

    /*
     * BAR0 — point the Falcon at the XUSB MMIO aperture base. Linux's
     * tegra_xusb_config uses `tegra->hcd->rsrc_start`, which for
     * Tegra234 comes from the DT `reg` property and starts at
     * 0x03600000 (the FPCI wrapper base) NOT 0x03610000 (the xHCI
     * CAPLENGTH offset inside the aperture). First-hardware test of
     * this function confirmed that attempting to program CFG_4 with
     * 0x03610000 is silently rejected — bit 16 is hardwired in the
     * BAR decoder on this SoC. Use FPCI_BASE to match what Linux
     * actually writes.
     */
    uint32_t cfg4 = fpci_r32(XUSB_CFG_4);
    cfg4 &= ~(XUSB_BASE_ADDR_MASK << XUSB_BASE_ADDR_SHIFT);
    cfg4 |= (uint32_t)TEGRA_XHCI_FPCI_BASE
            & (XUSB_BASE_ADDR_MASK << XUSB_BASE_ADDR_SHIFT);
    fpci_w32(XUSB_CFG_4, cfg4);

    /* BAR2 — Tegra234-specific wrapper aperture. */
    uint32_t cfg7 = fpci_r32(XUSB_CFG_7);
    cfg7 &= ~(XUSB_BASE2_ADDR_MASK << XUSB_BASE2_ADDR_SHIFT);
    cfg7 |= (uint32_t)TEGRA_XHCI_BAR2_BASE
            & (XUSB_BASE2_ADDR_MASK << XUSB_BASE2_ADDR_SHIFT);
    fpci_w32(XUSB_CFG_7, cfg7);

    /* Linux sleeps 100-200us between BAR programming and the bus-
     * master enable. The hardware needs time to latch the new BARs
     * before the next wrapper access uses them. timer_busy_wait_us
     * is CNTPCT-backed and safe pre-scheduler. */
    timer_busy_wait_us(200);

    /* Enable IO space, memory space, and bus master. The last one is
     * the bit we most expect to have been cleared by Linux's kexec
     * teardown — it's what lets the Falcon initiate DMA reads of
     * DCBAA / scratchpad / command-ring state. */
    uint32_t cfg1 = fpci_r32(XUSB_CFG_1);
    cfg1 |= XUSB_IO_SPACE_EN | XUSB_MEM_SPACE_EN | XUSB_BUS_MASTER_EN;
    fpci_w32(XUSB_CFG_1, cfg1);

    /*
     * Ensure the FPCI writes have drained before any subsequent HCD
     * access (e.g. CAPLENGTH read) that depends on BAR0 being
     * programmed. FPCI is Device-nGnRE, HCD is also Device, but
     * they're separate apertures — a DSB pins down the ordering.
     */
    dsb(sy);

    INFO("xhci: post-config CFG_1=0x%08x CFG_4=0x%08x CFG_7=0x%08x",
         (unsigned)fpci_r32(XUSB_CFG_1),
         (unsigned)fpci_r32(XUSB_CFG_4),
         (unsigned)fpci_r32(XUSB_CFG_7));
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Tegra234 IFR bringup — Falcon liveness probes                               */
/* -------------------------------------------------------------------------- */

/*
 * CSB paging window (Tegra234, via BAR2). Port of Linux's
 * bar2_csb_readl / bar2_csb_writel (linux-xhci-tegra.c:393-411).
 *
 * Falcon internal registers ("Control Status Bus") are not mapped
 * directly; they're accessed through a windowed paging scheme:
 *
 *   1. Compute the 23-bit page number and 9-bit offset-within-page
 *      from the 32-bit CSB address.
 *   2. Write the page number to XUSB_BAR2_ARU_C11_CSBRANGE at
 *      BAR2 + 0x9c.
 *   3. Read/write the 32-bit data at BAR2 + 0x2000 + offset.
 *
 * On Tegra234, this replaces the FPCI-based `fpci_csb_*` path that
 * earlier Tegras (T210/T186/T194) use — .has_bar2 = true in
 * tegra234_soc.
 *
 * CAUTION: BAR2 writes were observed to trigger a SNOC Write Error
 * in task 3A.2.5 when poking BAR2+0x1000 (the mailbox IOCTL
 * register). The CSB paging window lives at BAR2+0x9c (control) and
 * BAR2+0x2000+ofs (data) — different offsets, possibly under
 * different access control. Deployed as READ-ONLY probe only; the
 * writer is provided but __attribute__((unused)) until a known-safe
 * use case emerges.
 */
static uint32_t bar2_csb_r32(uint32_t csb_off)
{
    uint32_t page = xusb_csb_page_select(csb_off);
    uint32_t ofs  = xusb_csb_page_offset(csb_off);
    bar2_w32(XUSB_BAR2_ARU_C11_CSBRANGE, page);
    dsb(sy);
    return bar2_r32(XUSB_BAR2_CSB_BASE_ADDR + ofs);
}

__attribute__((unused))
static void bar2_csb_w32(uint32_t csb_off, uint32_t value)
{
    uint32_t page = xusb_csb_page_select(csb_off);
    uint32_t ofs  = xusb_csb_page_offset(csb_off);
    bar2_w32(XUSB_BAR2_ARU_C11_CSBRANGE, page);
    dsb(sy);
    bar2_w32(XUSB_BAR2_CSB_BASE_ADDR + ofs, value);
}

/*
 * Port of Linux's tegra_xusb_wait_for_falcon (linux-xhci-tegra.c:986-
 * 1003). Poll USBSTS for STS_CNR (Controller Not Ready) to clear,
 * 1 ms interval, 200 ms timeout. Linux uses this as the "Falcon is
 * alive and ready to accept further config" handshake.
 *
 * On Tegra234, the IFR boots the Falcon automatically when clocks +
 * power domains come up. Linux's probe waits for CNR to go clear
 * before trusting any xHCI-level state — same thing applies to SLM-
 * OS post-kexec.
 *
 * Returns 0 if CNR clears within 200 ms, -1 on timeout. Timeout here
 * is strong evidence the Falcon didn't survive the kexec handoff.
 *
 * Currently unused — kept for reference; see the big CAUTION comment
 * in xhci_init where the call site was disabled after the BAR2
 * mailbox probe triggered a TF-A RAS uncorrectable.
 */
__attribute__((unused))
static int tegra_xusb_wait_for_falcon(void)
{
    INFO("xhci: waiting for Falcon (USBSTS.CNR clear)...");
    if (poll_reg32(xhci_op_base, XHCI_OP_USBSTS,
                   XHCI_STS_CNR, 0, 200) != 0) {
        uint32_t sts = r32(xhci_op_base, XHCI_OP_USBSTS);
        WARN("xhci: Falcon wait timeout — USBSTS=0x%08x "
             "(CNR=%u, HCE=%u)",
             (unsigned)sts,
             (sts & XHCI_STS_CNR) ? 1 : 0,
             (sts & XHCI_STS_HCE) ? 1 : 0);
        return -1;
    }
    INFO("xhci: Falcon ready (USBSTS.CNR clear)");
    return 0;
}

/*
 * Port of Linux's tegra_xusb_read_firmware_header (linux-xhci-tegra.c:
 * 1098-1110). The IFR exposes its firmware image header through a
 * debug-scratch IOCTL window on BAR2:
 *
 *   1. Write a command word to XUSB_BAR2_ARU_FW_SCRATCH (BAR2 +
 *      0x1000) encoding the IOCTL type (FW_IOCTL_CFGTBL_READ = 17)
 *      in bits [31:24] and the byte offset into the header in bits
 *      [23:0].
 *   2. Read the response from XUSB_BAR2_ARU_SMI_ARU_FW_SCRATCH_DATA0
 *      (BAR2 + 0x01c).
 *
 * The wrapper handles this synchronously — Linux doesn't poll for a
 * ready bit — so it's the cheapest possible "Falcon mailbox alive?"
 * probe. A plausible response (e.g. a Unix timestamp in the 2023-
 * 2025 range when offset = fwimg_created_time) means the Falcon is
 * fully alive at the mailbox layer. An unchanged 0 / 0xFFFFFFFF /
 * zero value means the mailbox is dead or unplumbed.
 *
 * The caller is responsible for checking `tegra_xusb_wait_for_falcon`
 * returned 0 first — without that, the mailbox behaviour is undefined.
 *
 * DANGEROUS ON SLM-OS AT NS EL2 POST-KEXEC: the BAR2+0x1000 write
 * triggers a SNOC Write Error that TF-A traps as RAS Uncorrectable
 * and responds by powering off the core. Do not call this from
 * anywhere the board can't tolerate a crash. Kept as reference so
 * a future investigator with a way to whitelist this stream-ID (or
 * bypass the SNOC firewall) has the exact sequence ready to re-try.
 */
__attribute__((unused))
static uint32_t tegra_xusb_read_firmware_header(uint32_t byte_offset)
{
    /*
     * Bounds check mirroring linux-xhci-tegra.c:1104 — the IOCTL
     * window only decodes header offsets within the known struct,
     * and Linux returns 0 for OOB requests rather than issuing the
     * read. Match that contract so the unused-function hazard
     * doesn't silently turn into an unbounded-offset hazard when
     * someone re-enables this path for Path 1 / Path 3 of §10.
     */
    if (byte_offset >= XUSB_FW_HDR_SIZE)
        return 0;

    uint32_t cmd = (uint32_t)XUSB_FW_IOCTL_CFGTBL_READ
                   << XUSB_FW_IOCTL_TYPE_SHIFT;
    cmd |= byte_offset;
    bar2_w32(XUSB_BAR2_ARU_FW_SCRATCH, cmd);
    /*
     * DSB so the BAR2 write has been issued before we read the
     * response register. Both apertures are Device-nGnRE so they're
     * ordered by the architecture for accesses to the same window,
     * but they're different windows here (write to 0x1000, read from
     * 0x01c) and the wrapper routes the IOCTL across them — belt-
     * and-suspenders.
     */
    dsb(sy);
    return bar2_r32(XUSB_BAR2_ARU_SMI_ARU_FW_SCRATCH_DATA0);
}

/* -------------------------------------------------------------------------- */
/* Halt + reset                                                                */
/* -------------------------------------------------------------------------- */

static int xhci_halt(void)
{
    uint32_t cmd = r32(xhci_op_base, XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RUN) {
        /* Clear Run/Stop — controller takes up to 16 ms to halt per
         * xHCI 1.2 §4.1. */
        w32(xhci_op_base, XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RUN);
    }
    if (poll_reg32(xhci_op_base, XHCI_OP_USBSTS,
                   XHCI_STS_HCH, XHCI_STS_HCH, 50) != 0) {
        WARN("xhci: halt timeout — USBSTS=0x%08x",
             (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));
        return -1;
    }
    return 0;
}

/*
 * Tegra-aware "reset". The standard xHCI HCRST bit does NOT work on
 * Tegra from NS EL2 — empirical result from this session:
 *
 *   Before HCRST: HCIVERSION=0x0120, HCCPARAMS1=0x0180ff05 (live)
 *   After HCRST:  the ENTIRE aperture reads 0xffffffff — the
 *                 controller has entered a Falcon-level reset that
 *                 only BPMP can complete, and BPMP is unreachable.
 *
 * Writing HCRST is therefore a one-way trip to a dead controller
 * post-kexec. We must NOT touch it. Instead:
 *
 *   1. Confirm the controller is already halted by Linux (HCH=1).
 *      Everything we'll do in Step 4 (DCBAAP / CRCR / interrupter
 *      setup) assumes a halted controller, and Linux's runtime-PM
 *      dance leaves exactly that state.
 *   2. Later steps will clear Linux's ring pointers by writing the
 *      relevant registers directly. That doesn't require HCRST.
 *
 * Tracked as #282 — if Step 4+ finds Linux's halted state is too
 * dirty to build on, we'll need a wrapper-level reset (likely via
 * the pre-kexec BPMP debugfs path rather than from SLM-OS itself).
 *
 * Returns 0 on success, negative if the controller isn't halted
 * (which would be surprising given the kexec helper's state).
 */
static int xhci_reset(void)
{
    uint32_t sts = r32(xhci_op_base, XHCI_OP_USBSTS);
    INFO("xhci: pre-reset USBCMD=0x%08x USBSTS=0x%08x",
         (unsigned)r32(xhci_op_base, XHCI_OP_USBCMD), (unsigned)sts);

    if (!(sts & XHCI_STS_HCH)) {
        WARN("xhci: controller not halted (USBSTS=0x%08x) — "
             "the kexec helper should have left HCH set", (unsigned)sts);
        return -1;
    }
    INFO("xhci: skipping HCRST on Tegra (Falcon-reset is BPMP-only); "
         "using Linux's halted state as the init baseline");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Capability parse                                                            */
/* -------------------------------------------------------------------------- */

static void xhci_parse_caps(struct xhci_caps *c)
{
    c->cap_length  = r8 (xhci_cap_base, XHCI_CAP_CAPLENGTH);
    c->hci_version = r16(xhci_cap_base, XHCI_CAP_HCIVERSION);
    c->hcs_params1 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS1);
    c->hcs_params2 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS2);
    c->hcs_params3 = r32(xhci_cap_base, XHCI_CAP_HCSPARAMS3);
    c->hcc_params1 = r32(xhci_cap_base, XHCI_CAP_HCCPARAMS1);
    c->db_off      = r32(xhci_cap_base, XHCI_CAP_DBOFF);
    c->rts_off     = r32(xhci_cap_base, XHCI_CAP_RTSOFF);
    c->hcc_params2 = r32(xhci_cap_base, XHCI_CAP_HCCPARAMS2);

    c->max_slots = (uint8_t) XHCI_HCS1_MAX_SLOTS(c->hcs_params1);
    c->max_ports = (uint8_t) XHCI_HCS1_MAX_PORTS(c->hcs_params1);
    c->max_intrs = (uint16_t)XHCI_HCS1_MAX_INTRS(c->hcs_params1);
    c->ac64      = XHCI_HCC1_AC64(c->hcc_params1) != 0;
    c->ctx_64    = XHCI_HCC1_CSZ(c->hcc_params1) != 0;
    c->xecp      = (uint16_t)XHCI_HCC1_XECP(c->hcc_params1);
}

/* -------------------------------------------------------------------------- */
/* Runtime + doorbell register helpers                                         */
/* -------------------------------------------------------------------------- */

/*
 * Interrupter 0 register block at xhci_rt_base + 0x20. Subsequent
 * interrupters are +0x20 each. Phase 3A uses only IR0.
 */
#define XHCI_IR0_OFFSET             0x20
#define XHCI_IR_IMAN                0x00
#define XHCI_IR_IMOD                0x04
#define XHCI_IR_ERSTSZ              0x08
#define XHCI_IR_ERSTBA              0x10    /* 64-bit */
#define XHCI_IR_ERDP                0x18    /* 64-bit */

/* Doorbell array: DB[n] = db_base + 4 * n. DB[0] = command ring. */
#define XHCI_DB_COMMAND             0

/* -------------------------------------------------------------------------- */
/* Step 4: scratchpad + DCBAA + rings + NO_OP                                  */
/* -------------------------------------------------------------------------- */

static uint32_t xhci_page_size_bytes(void)
{
    /* PAGESIZE register: bit n set means page size 2^(n+12) is
     * supported. Controller picks one and reports it; we honour it
     * for scratchpad allocation. */
    uint32_t ps = r32(xhci_op_base, XHCI_OP_PAGESIZE);
    if (ps == 0)
        return XHCI_PAGESIZE_DEFAULT;
    /* Find the first set bit (lowest supported). */
    for (unsigned i = 0; i < 16; i++) {
        if (ps & (1u << i))
            return 1u << (i + 12);
    }
    return XHCI_PAGESIZE_DEFAULT;
}

/*
 * Decode Max Scratchpad Buffers from HCSPARAMS2. xHCI 1.2 §5.3.4:
 * MSB[Hi] bits [25:21], MSB[Lo] bits [31:27]. Value is (Hi << 5) | Lo.
 */
static uint32_t xhci_max_scratchpads(uint32_t hcs_params2)
{
    uint32_t hi = (hcs_params2 >> 21) & 0x1F;
    uint32_t lo = (hcs_params2 >> 27) & 0x1F;
    return (hi << 5) | lo;
}

/*
 * Allocate the scratchpad buffer array + its buffers. Called only if
 * the controller reports non-zero MaxScratchpads. All allocations
 * are NC memory so the HC's DMA sees them without cache maintenance;
 * scratchpads are the HC's private work area, never read by us.
 */
static int xhci_alloc_scratchpads(uint32_t count)
{
    xhci_num_scratchpads = count;
    if (count == 0)
        return 0;

    size_t ptr_bytes = (size_t)count * sizeof(uint64_t);
    xhci_scratchpad_ptrs = ncmem_alloc(ptr_bytes, 64);
    if (xhci_scratchpad_ptrs == NULL) {
        WARN("xhci: scratchpad-ptr alloc failed (%zu bytes)", ptr_bytes);
        return -1;
    }
    memset(xhci_scratchpad_ptrs, 0, ptr_bytes);

    uint32_t page_size = xhci_page_size_bytes();
    size_t buf_bytes = (size_t)count * page_size;
    xhci_scratchpad_bufs = ncmem_alloc(buf_bytes, page_size);
    if (xhci_scratchpad_bufs == NULL) {
        WARN("xhci: scratchpad-buf alloc failed (%zu bytes)", buf_bytes);
        return -1;
    }
    memset(xhci_scratchpad_bufs, 0, buf_bytes);

    uintptr_t base = (uintptr_t)xhci_scratchpad_bufs;
    for (uint32_t i = 0; i < count; i++)
        xhci_scratchpad_ptrs[i] = base + (uint64_t)i * page_size;

    INFO("xhci: allocated %u scratchpad%s (%u bytes each)",
         count, count == 1 ? "" : "s", (unsigned)page_size);
    return 0;
}

/*
 * Allocate the single-entry Event Ring Segment Table from NC memory.
 * The HC DMA-reads ERST to locate the event ring, so it has to be
 * either NC or clean-to-PoC; NC matches the rest of the HC-visible
 * allocations in this driver and avoids needing per-update cache
 * maintenance before each ERSTBA write.
 */
static int xhci_alloc_erst(void)
{
    xhci_erst = ncmem_alloc(sizeof(*xhci_erst), 64);
    if (xhci_erst == NULL) {
        WARN("xhci: ERST alloc failed (%zu bytes)", sizeof(*xhci_erst));
        return -1;
    }
    memset(xhci_erst, 0, sizeof(*xhci_erst));
    return 0;
}

/*
 * Allocate the Device Context Base Address Array. Slot 0 holds the
 * pointer to the scratchpad buffer array (when MaxScratchpads > 0).
 * Slots 1..MaxSlots hold Device Context pointers once ENABLE_SLOT
 * commands succeed — all zero at this point.
 */
static int xhci_alloc_dcbaa(uint8_t max_slots)
{
    size_t entries = (size_t)max_slots + 1;  /* +1 for slot 0 */
    size_t bytes   = entries * sizeof(uint64_t);
    xhci_dcbaa = ncmem_alloc(bytes, 64);
    if (xhci_dcbaa == NULL) {
        WARN("xhci: DCBAA alloc failed (%zu bytes)", bytes);
        return -1;
    }
    memset(xhci_dcbaa, 0, bytes);

    if (xhci_num_scratchpads > 0 && xhci_scratchpad_ptrs != NULL)
        xhci_dcbaa[0] = (uint64_t)(uintptr_t)xhci_scratchpad_ptrs;

    return 0;
}

/* Write the controller registers that point at our rings + DCBAA. */
static void xhci_program_registers(uint8_t max_slots)
{
    /*
     * Record Linux's leftover pointers so the serial log captures
     * what state we're trampling. If Linux's original DCBAAP / CRCR /
     * ERSTBA were valid SMMU-mapped addresses, replacing them with
     * our NC-memory addresses may produce an SMMU fault that wedges
     * the controller — this dump pins down exactly where that
     * happens. SLM-OS's uart_printf doesn't speak `%llx` so dump
     * each dword separately.
     */
    uint32_t orig_dcbaap_lo = r32(xhci_op_base, XHCI_OP_DCBAAP);
    uint32_t orig_dcbaap_hi = r32(xhci_op_base, XHCI_OP_DCBAAP + 4);
    uint32_t orig_crcr_lo   = r32(xhci_op_base, XHCI_OP_CRCR);
    uint32_t orig_crcr_hi   = r32(xhci_op_base, XHCI_OP_CRCR + 4);
    INFO("xhci: pre-program DCBAAP=%08x%08x CRCR=%08x%08x CONFIG=0x%08x USBSTS=0x%08x",
         (unsigned)orig_dcbaap_hi, (unsigned)orig_dcbaap_lo,
         (unsigned)orig_crcr_hi, (unsigned)orig_crcr_lo,
         (unsigned)r32(xhci_op_base, XHCI_OP_CONFIG),
         (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));

    /*
     * ARM64 requires an explicit DSB between Normal-Non-Cacheable
     * stores and Device-nGnRE stores (ARM ARM B2.7.2). Everything
     * the HC will DMA-read is already populated at this point
     * (DCBAA via xhci_alloc_dcbaa, scratchpad pointers via
     * xhci_alloc_scratchpads, ERST below, command/event ring TRBs
     * via the ring_alloc calls) — those stores went to NC memory
     * and may still be sitting in the write-combining buffer. The
     * DSB drains all prior stores to the Point of System before
     * the MMIO writes below tell the HC to go read them. Without
     * this, the HC can read stale zeros and fail to start.
     */
    dsb(sy);

    /* DCBAAP (64-bit). Write low first, then high per spec ordering
     * advice — HC latches on the high-dword write. */
    uint64_t dcbaap = (uint64_t)(uintptr_t)xhci_dcbaa;
    w32(xhci_op_base, XHCI_OP_DCBAAP,     (uint32_t)(dcbaap & 0xFFFFFFFFu));
    w32(xhci_op_base, XHCI_OP_DCBAAP + 4, (uint32_t)(dcbaap >> 32));

    /* CRCR: ring base (64-bit) + Ring Cycle State (bit 0). Must be
     * written as two 32-bit stores with high last. */
    uint64_t cmd_ring_ptr = xhci_cmd_ring.phys | 1 /* RCS = initial PCS */;
    w32(xhci_op_base, XHCI_OP_CRCR,     (uint32_t)(cmd_ring_ptr & 0xFFFFFFFFu));
    w32(xhci_op_base, XHCI_OP_CRCR + 4, (uint32_t)(cmd_ring_ptr >> 32));

    /* CONFIG: MaxSlotsEnabled in low byte. Enable all reported slots
     * — we'll allocate device contexts on demand. */
    w32(xhci_op_base, XHCI_OP_CONFIG, max_slots);

    /* Interrupter 0 setup. */
    volatile uint8_t *ir0 = xhci_rt_base + XHCI_IR0_OFFSET;
    xhci_erst->base_lo  = (uint32_t)(xhci_evt_ring.phys & 0xFFFFFFFFu);
    xhci_erst->base_hi  = (uint32_t)(xhci_evt_ring.phys >> 32);
    xhci_erst->size     = xhci_evt_ring.num_trbs;
    xhci_erst->reserved = 0;

    /*
     * Drain the ERST field stores (Normal NC) before any MMIO write
     * to the interrupter. The ERSTBA write below tells the HC to
     * DMA-read ERST; without this DSB the MMIO can reach the HC
     * before the NC stores have drained through the write buffer.
     */
    dsb(sy);

    /* ERSTSZ = 1 (one segment). */
    w32(ir0, XHCI_IR_ERSTSZ, 1);
    /* ERDP must be programmed BEFORE ERSTBA per xHCI 1.2 §5.5.2.3.2 —
     * writing ERSTBA enables the event ring. */
    uint64_t erdp = xhci_event_ring_dequeue_phys(&xhci_evt_ring);
    w32(ir0, XHCI_IR_ERDP,     (uint32_t)(erdp & 0xFFFFFFFFu));
    w32(ir0, XHCI_IR_ERDP + 4, (uint32_t)(erdp >> 32));
    uint64_t erstba = (uint64_t)(uintptr_t)xhci_erst;
    w32(ir0, XHCI_IR_ERSTBA,     (uint32_t)(erstba & 0xFFFFFFFFu));
    w32(ir0, XHCI_IR_ERSTBA + 4, (uint32_t)(erstba >> 32));

    /* Leave IMAN.IE = 0 (polling, no IRQs in Phase 3A). IMOD stays
     * at its reset value. */
}

static int xhci_start_controller(void)
{
    /* Set Run/Stop = 1. Poll USBSTS.HCH to drop. */
    uint32_t cmd = r32(xhci_op_base, XHCI_OP_USBCMD);
    w32(xhci_op_base, XHCI_OP_USBCMD, cmd | XHCI_CMD_RUN);
    if (poll_reg32(xhci_op_base, XHCI_OP_USBSTS,
                   XHCI_STS_HCH, 0, 500) != 0) {
        WARN("xhci: controller failed to start (USBSTS=0x%08x)",
             (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Cross-TU helpers (exported via xhci_internal.h)                             */
/* -------------------------------------------------------------------------- */

uint32_t xhci_op_r32(uint32_t off)
{
    return r32(xhci_op_base, off);
}

void xhci_op_w32(uint32_t off, uint32_t val)
{
    w32(xhci_op_base, off, val);
}

/*
 * Doorbell: four-byte register per device slot, plus slot 0 for the
 * command ring. `db_index` is 0 for command, 1..MaxSlots for devices.
 * `target` is 0 on the command doorbell; on device doorbells it is
 * the DCI of the endpoint being kicked.
 *
 * Two barriers frame the doorbell write — the DSB before drains any
 * NC-memory producer stores (TRB enqueue) to the Point of System
 * before the HC reads them, and the DSB after makes the MMIO write
 * visible before we start polling the event ring.
 */
void xhci_ring_doorbell(uint8_t db_index, uint8_t target)
{
    /* Doorbell register layout: low 8 bits = target (DB Target field),
     * high 16 bits = stream ID (unused in Phase 3A — no streaming EPs). */
    uint32_t value = target;
    dsb(sy);
    w32(xhci_db_base, 4u * db_index, value);
    dsb(sy);
}

/*
 * Drain every Transfer / Command-Completion event currently visible
 * on the event ring. Transfer Events route through
 * xhci_xfer_on_transfer_event; Command Completions update the
 * per-slot shared state below so xhci_cmd_submit_and_wait can see
 * them. Returns the number of events consumed.
 */

/*
 * Pending-command handshake between this file and the event consumer.
 * Only one command is ever in flight at a time — usb_core runs the
 * HCD ops from a single thread during init, and all driver paths
 * serialise through xhci_cmd_submit_and_wait. That lets the pending
 * state live as a pair of scalars instead of a per-command queue.
 */
static uintptr_t xhci_cmd_pending_phys;
static bool      xhci_cmd_completed;
static uint8_t   xhci_cmd_completion_cc;
static uint8_t   xhci_cmd_completion_slot;

int xhci_event_ring_drain(void)
{
    struct xhci_trb evt;
    int consumed = 0;
    volatile uint8_t *ir0 = xhci_rt_base + XHCI_IR0_OFFSET;

    while (xhci_event_ring_peek(&xhci_evt_ring, &evt)) {
        uint32_t type = XHCI_TRB_TYPE_GET(evt.control);
        switch (type) {
        case XHCI_TRB_EVT_CMD_COMPLETION: {
            uintptr_t evt_phys = (uintptr_t)evt.param_lo |
                                 ((uintptr_t)evt.param_hi << 32);
            if (xhci_cmd_pending_phys != 0 &&
                evt_phys == xhci_cmd_pending_phys) {
                xhci_cmd_completion_cc   = XHCI_CC_GET(evt.status);
                xhci_cmd_completion_slot = XHCI_TRB_SLOT_GET(evt.control);
                xhci_cmd_completed       = true;
            } else {
                INFO("xhci: stale command completion @0x%lx "
                     "(expected 0x%lx)",
                     (unsigned long)evt_phys,
                     (unsigned long)xhci_cmd_pending_phys);
            }
            break;
        }
        case XHCI_TRB_EVT_TRANSFER:
            xhci_xfer_on_transfer_event(&evt);
            break;
        case XHCI_TRB_EVT_PORT_STATUS:
            /* Acknowledged by reading PORTSC in port_status / port_reset;
             * nothing to do here beyond consuming the event. */
            break;
        default:
            /* Bandwidth-request, doorbell, host-controller events are
             * informational — log at verbose level for diagnostics. */
            INFO("xhci: unhandled event type %u (status=0x%08x)",
                 type, (unsigned)evt.status);
            break;
        }
        consumed++;
    }

    if (consumed > 0) {
        /* Bit 3 (EHB) is Event Handler Busy — write-1-to-clear in
         * polled mode, harmless either way. */
        uint64_t erdp = xhci_event_ring_dequeue_phys(&xhci_evt_ring) | (1u << 3);
        w32(ir0, XHCI_IR_ERDP,     (uint32_t)(erdp & 0xFFFFFFFFu));
        w32(ir0, XHCI_IR_ERDP + 4, (uint32_t)(erdp >> 32));
    }

    return consumed;
}

int xhci_cmd_submit_and_wait(const struct xhci_trb *cmd,
                             uint8_t *cc_out,
                             uint8_t *slot_out,
                             uint32_t timeout_ms)
{
    if (cmd == NULL)
        return -1;

    struct xhci_trb *slot = xhci_ring_enqueue(&xhci_cmd_ring, cmd);
    if (slot == NULL) {
        WARN("xhci: command ring enqueue failed");
        return -1;
    }

    xhci_cmd_pending_phys    = (uintptr_t)slot;
    xhci_cmd_completed       = false;
    xhci_cmd_completion_cc   = 0;
    xhci_cmd_completion_slot = 0;

    xhci_ring_doorbell(XHCI_DB_COMMAND, 0);

    uint64_t freq  = timer_get_frequency();
    uint64_t start = timer_get_count();
    uint64_t ticks = ((uint64_t)timeout_ms * freq) / 1000u;

    while (!xhci_cmd_completed) {
        (void)xhci_event_ring_drain();
        if (xhci_cmd_completed)
            break;
        if (timer_get_count() - start >= ticks) {
            WARN("xhci: command timeout (type=%u, USBSTS=0x%08x)",
                 (unsigned)XHCI_TRB_TYPE_GET(cmd->control),
                 (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));
            xhci_cmd_pending_phys = 0;
            return -1;
        }
    }

    if (cc_out   != NULL) *cc_out   = xhci_cmd_completion_cc;
    if (slot_out != NULL) *slot_out = xhci_cmd_completion_slot;
    xhci_cmd_pending_phys = 0;
    return 0;
}

/*
 * Step 4's NO_OP round-trip, re-expressed on top of
 * xhci_cmd_submit_and_wait. Kept as a diagnostic called from
 * xhci_init; the round-trip is the definitive check that every
 * ring / doorbell / ERDP write we programmed is actually reaching
 * the HC and coming back.
 */
static int xhci_send_noop(void)
{
    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP);

    uint8_t cc = 0;
    int rc = xhci_cmd_submit_and_wait(&cmd, &cc, NULL, 500);
    if (rc != 0)
        return rc;
    if (cc == XHCI_CC_SUCCESS) {
        INFO("xhci: NO_OP round-trip OK (cc=SUCCESS)");
        return 0;
    }
    WARN("xhci: NO_OP completed with cc=%u", cc);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int xhci_init(void)
{
    xhci_live = false;

    /*
     * Grab the virtual pointers for all three apertures. The VMM's
     * 2 MB mapping at 0x03600000 covers FPCI (at base), the Intel-
     * spec xHCI block at +0x10000, and the BAR2 wrapper at +0x50000.
     * Identity-mapped on Jetson so physical == virtual at EL2.
     */
    xhci_cap_base  = (volatile uint8_t *)(uintptr_t)TEGRA_XHCI_HCD_BASE;
    xhci_fpci_base = (volatile uint8_t *)(uintptr_t)TEGRA_XHCI_FPCI_BASE;
    xhci_bar2_base = (volatile uint8_t *)(uintptr_t)TEGRA_XHCI_BAR2_BASE;

    /* Sanity check: if the clocks aren't held, CAPLENGTH reads as
     * 0xFF (low byte of 0xFFFFFFFF). Bail early with a clear
     * message rather than writing to a dead aperture. */
    uint32_t first = r32(xhci_cap_base, 0);
    if (first == 0xFFFFFFFFu) {
        WARN("xhci: aperture reads 0xffffffff — clocks gated "
             "(did slmos-kexec run without --no-usb-hold?)");
        return -1;
    }

    /*
     * FPCI + BAR2 reachability probe. FPCI config space at offset 0
     * is a PCI-header dev/vendor word with a known value (Linux
     * reports 0x229810de for Tegra XHCI). BAR2 at offset 0 has no
     * published expected value, but 0xFFFFFFFF indicates a dead
     * aperture (same failure mode as the xHCI sanity check above).
     *
     * These reads don't change any state — they just confirm we can
     * reach the Tegra wrapper apertures before Phase 3A.2.2 starts
     * programming XUSB_CFG_1/4/7.
     */
    uint32_t fpci_id = fpci_r32(XUSB_FPCI_DEV_VENDOR_ID);
    uint32_t bar2_hd = bar2_r32(0);
    INFO("xhci: FPCI dev/vendor=0x%08x (expect 0x%08x), BAR2[0]=0x%08x",
         (unsigned)fpci_id, (unsigned)XUSB_FPCI_DEV_VENDOR_EXPECTED,
         (unsigned)bar2_hd);
    if (fpci_id == 0xFFFFFFFFu) {
        WARN("xhci: FPCI aperture reads 0xffffffff — wrapper clock/"
             "powergate down?");
        return -1;
    }
    if (fpci_id != XUSB_FPCI_DEV_VENDOR_EXPECTED) {
        /* Non-fatal: log and continue. NVIDIA could ship a SKU with a
         * different dev/vendor ID, and Phase 3A.2.2 onward doesn't
         * depend on this exact value — it's informational. */
        WARN("xhci: FPCI dev/vendor 0x%08x unexpected (not fatal)",
             (unsigned)fpci_id);
    }
    if (bar2_hd == 0xFFFFFFFFu) {
        WARN("xhci: BAR2 aperture reads 0xffffffff — wrapper clock/"
             "powergate down?");
        return -1;
    }

    /*
     * Program the FPCI wrapper BEFORE any further HCD access. Linux's
     * tegra_xusb_config runs at this point in its own probe. First-
     * hardware testing showed Linux leaves the wrapper correctly
     * programmed post-kexec, so this is a defensive no-op — but we
     * re-assert the required bits in case a future kernel version
     * or SKU changes that. See tegra_xusb_config's header comment.
     */
    (void)tegra_xusb_config();

    /*
     * Falcon liveness probes DISABLED at init-time — see CAUTION.
     *
     * The Linux tegra_xusb_init_ifr_firmware pattern (linux-xhci-
     * tegra.c:1112-1126) calls `tegra_xusb_wait_for_falcon` (poll
     * USBSTS.CNR) then `tegra_xusb_read_firmware_header` (write to
     * XUSB_BAR2_ARU_FW_SCRATCH at BAR2+0x1000, read back from
     * BAR2+0x01c). The functions are defined above for reference,
     * but calling them from SLM-OS at NS EL2 post-kexec is UNSAFE:
     *
     *   CAUTION: on jetson-nano-1 with feature/xhci-ifr-bringup
     *   task 3A.2.5, writing to BAR2+0x1000 triggered a SNOC Write
     *   Error (Tegra interconnect RAS Uncorrectable):
     *
     *     ERROR: RAS Uncorrectable Error in IOB, base=0xe010000
     *     ERROR:   SERR = Error response from slave: 0x12
     *     ERROR:   IERR = IHI (GIC ACE-Lite) Interface Error: 0x3
     *     ERROR: RAS Uncorrectable Error in ACI, base=0xe01a000
     *     ERROR:   IERR = SNOC Write Error: 0xd
     *
     *   TF-A at EL3 traps the error and powers off the CPU core —
     *   same failure class as the nvgpu post-kexec RAS error that
     *   the slmos-kexec helper works around with runtime_pm_suspend.
     *   No equivalent pre-kexec path exists for xusb yet.
     *
     *   Root cause (hypothesis): BAR2's ARU mailbox region is
     *   access-controlled at the interconnect level; Linux
     *   reaches it only after padctl/PHY/IFR setup Linux already
     *   finished before our kexec-inherited code runs. From NS EL2
     *   post-kexec the aperture appears readable at offset 0 but
     *   writes to the mailbox IOCTL register fault the interconnect.
     *
     * For now: skip the mailbox IOCTL calls. USBSTS.CNR observed =
     * 0 on hardware so Falcon liveness is evidenced indirectly.
     */

    /*
     * CSB paging probe (task 3A.2.3). Different BAR2 offsets
     * (0x9c + 0x2000+ofs) than the mailbox region that triggered
     * RAS, so worth trying read-only. Reading XUSB_FALC_CPUCTL
     * reveals Falcon internal state:
     *   bit 1 = STARTCPU (software-set to start the CPU)
     *   bit 4 = STATE_HALTED (Falcon hit a HALT instruction)
     *   bit 5 = STATE_STOPPED (Falcon is in STOPPED state)
     * Plausible values on a live IFR-booted Falcon: 0x00000020
     * (STATE_STOPPED) or 0x00000002 (STARTCPU) depending on where
     * in the IFR boot sequence Linux left the controller.
     *
     * If this read triggers the same RAS uncorrectable that the
     * mailbox write did, we've hit a hard wall via BAR2 entirely
     * and Phase 3A pivots fully to the SMMU/padctl direction.
     */
    uint32_t cpuctl = bar2_csb_r32(XUSB_FALC_CPUCTL);
    INFO("xhci: CSB probe FALC_CPUCTL=0x%08x "
         "(STARTCPU=%u HALTED=%u STOPPED=%u)",
         (unsigned)cpuctl,
         (cpuctl & XUSB_FALC_CPUCTL_STARTCPU) ? 1 : 0,
         (cpuctl & XUSB_FALC_CPUCTL_STATE_HALTED) ? 1 : 0,
         (cpuctl & XUSB_FALC_CPUCTL_STATE_STOPPED) ? 1 : 0);
    uint32_t apmap = bar2_csb_r32(XUSB_CSB_MP_APMAP);
    INFO("xhci: CSB probe MP_APMAP=0x%08x (BOOTPATH=%u)",
         (unsigned)apmap,
         (apmap & XUSB_CSB_MP_APMAP_BOOTPATH) ? 1 : 0);
    /*
     * Disambiguate: is CSB paging control actually writable, or is
     * BAR2 silently swallowing writes to 0x9c the same way the
     * mailbox region at 0x1000 noisily rejects them? The APMAP read
     * above left page=0x80c in the CSBRANGE register (by design).
     * Reading it back tells us whether the write stuck.
     */
    uint32_t csbrange = bar2_r32(XUSB_BAR2_ARU_C11_CSBRANGE);
    INFO("xhci: CSB probe CSBRANGE readback=0x%08x "
         "(expected 0x0000080c — MP_APMAP's page)",
         (unsigned)csbrange);

    uint8_t caplen = (uint8_t)(first & 0xFF);
    if (caplen < 0x20 || caplen > 0x80) {
        WARN("xhci: implausible CAPLENGTH 0x%02x — bail", caplen);
        return -1;
    }

    xhci_op_base = xhci_cap_base + caplen;
    xhci_parse_caps(&xhci_caps_cached);

    xhci_rt_base = xhci_cap_base + xhci_caps_cached.rts_off;
    xhci_db_base = xhci_cap_base + xhci_caps_cached.db_off;

    INFO("xhci: HCI v%x.%x, %u slots, %u ports, %u intrs, %s addr, %u-byte ctx",
         (xhci_caps_cached.hci_version >> 8) & 0xFF,
         xhci_caps_cached.hci_version & 0xFF,
         xhci_caps_cached.max_slots,
         xhci_caps_cached.max_ports,
         xhci_caps_cached.max_intrs,
         xhci_caps_cached.ac64 ? "64-bit" : "32-bit",
         xhci_caps_cached.ctx_64 ? 64 : 32);

    if (xhci_halt() != 0)
        return -1;
    if (xhci_reset() != 0)
        return -1;

    /* -----------------------------------------------------------------
     * Step 4: scratchpads + DCBAA + rings + NO_OP round-trip
     * -----------------------------------------------------------------
     */
    xhci_num_scratchpads = xhci_max_scratchpads(xhci_caps_cached.hcs_params2);
    if (xhci_alloc_scratchpads(xhci_num_scratchpads) != 0)
        return -1;
    if (xhci_alloc_dcbaa(xhci_caps_cached.max_slots) != 0)
        return -1;
    if (xhci_alloc_erst() != 0)
        return -1;
    if (xhci_ring_alloc(&xhci_cmd_ring, XHCI_CMD_RING_TRBS) != 0)
        return -1;
    if (xhci_event_ring_alloc(&xhci_evt_ring, XHCI_EVT_RING_TRBS) != 0)
        return -1;

    xhci_program_registers(xhci_caps_cached.max_slots);

    if (xhci_start_controller() != 0)
        return -1;
    INFO("xhci: controller running (USBSTS=0x%08x)",
         (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));

    if (xhci_send_noop() != 0) {
        /* Don't give up — the driver is partially usable even without
         * NO_OP if the caller only wants capability info. But mark
         * xhci_live so the shell dump works. */
        WARN("xhci: command/event ring round-trip failed — HCD "
             "registration skipped, usb_core will see no device");
        xhci_live = true;
        return 0;
    }

    xhci_live = true;

    /*
     * Register with usb_core so the single-device enumeration path
     * runs next. usb_core_start() drives port_status → port_reset →
     * device_open → the control-transfer descriptor sweep → SET_CONFIGURATION,
     * then calls endpoint_configure for each non-EP0 endpoint. Errors
     * are non-fatal here: the xHCI driver keeps running and the shell
     * diagnostic still works.
     */
    usb_core_register_hcd(&xhci_hcd);
    INFO("xhci: registered with usb_core — running Phase 3A Steps 5-7");

    /*
     * Call usb_core_start() so the HCD's start() runs, but do NOT
     * enumerate yet: on kexec, the device Linux already enumerated
     * is in a state that fails the first EP0 control transfer with
     * cc=4. We hide that stale device from usb_core in
     * xhci_hcd_port_status until the user physically re-plugs the
     * dongle, at which point net_poll → usb_core_hotplug_poll drives
     * the real enumeration. See issue #309 for the permanent fix
     * that eliminates the re-plug.
     */
    int rc = usb_core_start();
    if (rc != 0)
        WARN("xhci: usb_core_start returned %d", rc);
    /* The state machine in xhci_hcd_port_status defers enumeration
     * until a fresh CCS rising edge. In the pre-existing-device case
     * (kexec with a Linux-enumerated dongle, or future direct boot
     * with an already-plugged device) that means the user must
     * unplug and re-insert; the "attached at init" log above says so.
     * On a clean boot with nothing plugged in, this log is the only
     * hint that a future insertion will enumerate. */
    INFO("xhci: hot-plug ready — waiting for USB device attach "
         "(see #309 for the re-plug requirement on kexec boots)");
    return 0;
}

bool xhci_dump_info(void)
{
    if (!xhci_live)
        return false;

    const struct xhci_caps *c = &xhci_caps_cached;
    uart_printf("xhci @ %p (op @ +0x%02x, rt @ +0x%x, db @ +0x%x)\r\n",
                xhci_cap_base, (unsigned)c->cap_length,
                (unsigned)c->rts_off, (unsigned)c->db_off);
    uart_printf("  HCIVERSION %x.%x  HCCPARAMS1 0x%08x  HCCPARAMS2 0x%08x\r\n",
                (c->hci_version >> 8) & 0xFF, c->hci_version & 0xFF,
                (unsigned)c->hcc_params1, (unsigned)c->hcc_params2);
    uart_printf("  HCSPARAMS1 0x%08x  HCSPARAMS2 0x%08x  HCSPARAMS3 0x%08x\r\n",
                (unsigned)c->hcs_params1, (unsigned)c->hcs_params2,
                (unsigned)c->hcs_params3);
    uart_printf("  %u slots, %u ports, %u interrupters, %s addressing, %u-byte contexts\r\n",
                c->max_slots, c->max_ports, c->max_intrs,
                c->ac64 ? "64-bit" : "32-bit",
                c->ctx_64 ? 64 : 32);
    uart_printf("  USBCMD 0x%08x  USBSTS 0x%08x  PAGESIZE 0x%08x\r\n",
                (unsigned)r32(xhci_op_base, XHCI_OP_USBCMD),
                (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS),
                (unsigned)r32(xhci_op_base, XHCI_OP_PAGESIZE));
    uart_printf("  FPCI @ %p dev/vendor=0x%08x CFG_1=0x%08x CFG_4=0x%08x CFG_7=0x%08x\r\n",
                xhci_fpci_base,
                (unsigned)fpci_r32(XUSB_FPCI_DEV_VENDOR_ID),
                (unsigned)fpci_r32(XUSB_CFG_1),
                (unsigned)fpci_r32(XUSB_CFG_4),
                (unsigned)fpci_r32(XUSB_CFG_7));
    /*
     * BAR2 reads below are safe — the RAS hazard documented in the
     * big CAUTION block in xhci_init is on *writes* to BAR2+0x1000
     * (XUSB_BAR2_ARU_FW_SCRATCH), not reads from the response
     * register at BAR2+0x01c or from BAR2[0]. Three post-kexec
     * deploys on jetson-nano-1 confirmed these reads are non-RAS.
     */
    uart_printf("  BAR2 @ %p [0]=0x%08x FW_SCRATCH_DATA0=0x%08x\r\n",
                xhci_bar2_base,
                (unsigned)bar2_r32(0),
                (unsigned)bar2_r32(XUSB_BAR2_ARU_SMI_ARU_FW_SCRATCH_DATA0));
    return true;
}

void xhci_get_caps(struct xhci_caps *out)
{
    if (out == NULL)
        return;
    if (xhci_live)
        memcpy(out, &xhci_caps_cached, sizeof(*out));
    else
        memset(out, 0, sizeof(*out));
}

/* -------------------------------------------------------------------------- */
/* HCD op table (entries defined across xhci.c / xhci_device.c / xhci_xfer.c)  */
/* -------------------------------------------------------------------------- */

/*
 * Controller-wide start/stop: xhci_init already halted, allocated,
 * programmed, and RUN=1'd the controller before registering the HCD,
 * so the HCD's own `start` is a no-op. Kept non-NULL so usb_core_start
 * has something to call.
 */
int xhci_hcd_start(void)
{
    return xhci_live ? 0 : -1;
}

const struct usb_hcd xhci_hcd = {
    .name                = "xhci-tegra234",
    .start               = xhci_hcd_start,
    .port_status         = xhci_hcd_port_status,
    .port_reset          = xhci_hcd_port_reset,
    .device_open         = xhci_hcd_device_open,
    .device_close        = xhci_hcd_device_close,
    .endpoint_configure  = xhci_hcd_endpoint_configure,
    .submit_urb          = xhci_hcd_submit_urb,
    .cancel_urb          = xhci_hcd_cancel_urb,
    .poll                = xhci_hcd_poll,
};

#else  /* !PLATFORM_JETSON_ORIN_NANO */

/*
 * Non-Jetson stubs. The Phase-3A XHCI driver is Jetson-only; other
 * platforms get a driver that reports "not live" so any shared code
 * that queries xhci_dump_info() compiles and behaves consistently.
 */

#include "xhci.h"
#include <string.h>

int  xhci_init(void)                     { return 0; }
bool xhci_dump_info(void)                { return false; }
void xhci_get_caps(struct xhci_caps *out) { if (out) memset(out, 0, sizeof(*out)); }

#endif /* PLATFORM_JETSON_ORIN_NANO */
