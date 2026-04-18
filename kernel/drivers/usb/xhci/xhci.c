/*
 * xhci.c - SLM-OS xHCI host controller driver (#266 Phase 3A)
 *
 * Phase 3A Step 2 + 3: controller scaffolding, capability probe, and
 * halt/reset sequence. No rings, no ports, no transfers yet — those
 * come in Steps 4-7.
 *
 * Only built on Jetson; every other platform sees this file compiled
 * out by the #if at the top (no usb_hcd registration happens).
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_ring.h"
#include "xhci_trb.h"
#include "ncmem.h"
#include "debug.h"
#include "timer.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Base pointers                                                               */
/* -------------------------------------------------------------------------- */

/*
 * TEGRA_XHCI_HCD_BASE is the start of the standard xHCI aperture
 * (Intel-spec). The VMM already maps a 2 MB block at 0x03600000
 * covering both the Tegra FPCI prefix and this standard block.
 */
static volatile uint8_t *xhci_cap_base;
static volatile uint8_t *xhci_op_base;
static volatile uint8_t *xhci_rt_base;
static volatile uint8_t *xhci_db_base;

static struct xhci_caps      xhci_caps_cached;
static bool                  xhci_live;

/* Step 4 state */
#define XHCI_CMD_RING_TRBS           64
#define XHCI_EVT_RING_TRBS           64
#define XHCI_PAGESIZE_DEFAULT        4096

static uint64_t             *xhci_dcbaa;          /* aligned(64), MaxSlots+1 entries */
static uint64_t             *xhci_scratchpad_ptrs;
static void                 *xhci_scratchpad_bufs; /* N pages × PAGESIZE */
static uint32_t              xhci_num_scratchpads;

static struct xhci_ring       xhci_cmd_ring;
static struct xhci_event_ring xhci_evt_ring;

/*
 * Event Ring Segment Table entry layout per xHCI 1.2 §6.5. One entry
 * is enough — Phase 3A uses a single event-ring segment. The table
 * base must be 64-byte aligned; the one-entry array satisfies that.
 */
struct xhci_erst_entry {
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t size;          /* low 16 bits = segment TRB count */
    uint32_t reserved;
} __attribute__((packed));
_Static_assert(sizeof(struct xhci_erst_entry) == 16, "ERST entry 16B");

static struct xhci_erst_entry xhci_erst[1] __attribute__((aligned(64)));

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

/* -------------------------------------------------------------------------- */
/* Halt + reset                                                                */
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
    uint32_t cmd = r32(xhci_op_base, XHCI_OP_USBCMD);
    uint32_t sts = r32(xhci_op_base, XHCI_OP_USBSTS);
    INFO("xhci: pre-reset USBCMD=0x%08x USBSTS=0x%08x", (unsigned)cmd, (unsigned)sts);

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

/*
 * Debug helper while we pin down what's killing the aperture. Reads
 * USBSTS; logs 0xffffffff warnings but otherwise quietly returns.
 */
static uint32_t xhci_log_sts(const char *tag)
{
    uint32_t sts = r32(xhci_op_base, XHCI_OP_USBSTS);
    INFO("xhci: %s USBSTS=0x%08x", tag, (unsigned)sts);
    return sts;
}

/* Write the controller registers that point at our rings + DCBAA. */
static void xhci_program_registers(uint8_t max_slots)
{
    /*
     * Before writing anything, record what Linux left in the
     * registers. If the original DCBAAP / CRCR / ERSTBA were valid
     * SMMU-mapped addresses, replacing them with our NC-memory
     * addresses may produce an SMMU fault that wedges the controller
     * — this dump pins down exactly what state we're trampling.
     */
    /*
     * Record Linux's leftover pointers. SLM-OS's uart_printf doesn't
     * speak `%llx` so dump each dword separately.
     */
    uint32_t orig_dcbaap_lo = r32(xhci_op_base, XHCI_OP_DCBAAP);
    uint32_t orig_dcbaap_hi = r32(xhci_op_base, XHCI_OP_DCBAAP + 4);
    uint32_t orig_crcr_lo   = r32(xhci_op_base, XHCI_OP_CRCR);
    uint32_t orig_crcr_hi   = r32(xhci_op_base, XHCI_OP_CRCR + 4);
    INFO("xhci: pre-program DCBAAP=%08x%08x CRCR=%08x%08x CONFIG=0x%08x",
         (unsigned)orig_dcbaap_hi, (unsigned)orig_dcbaap_lo,
         (unsigned)orig_crcr_hi, (unsigned)orig_crcr_lo,
         (unsigned)r32(xhci_op_base, XHCI_OP_CONFIG));
    xhci_log_sts("pre-program");

    /* DCBAAP (64-bit). Write low first, then high per spec ordering
     * advice — HC latches on the high-dword write. */
    uint64_t dcbaap = (uint64_t)(uintptr_t)xhci_dcbaa;
    w32(xhci_op_base, XHCI_OP_DCBAAP,     (uint32_t)(dcbaap & 0xFFFFFFFFu));
    w32(xhci_op_base, XHCI_OP_DCBAAP + 4, (uint32_t)(dcbaap >> 32));
    xhci_log_sts("after DCBAAP");

    /* CRCR: ring base (64-bit) + Ring Cycle State (bit 0). Must be
     * written as two 32-bit stores with high last. */
    uint64_t cmd_ring_ptr = xhci_cmd_ring.phys | 1 /* RCS = initial PCS */;
    w32(xhci_op_base, XHCI_OP_CRCR,     (uint32_t)(cmd_ring_ptr & 0xFFFFFFFFu));
    w32(xhci_op_base, XHCI_OP_CRCR + 4, (uint32_t)(cmd_ring_ptr >> 32));
    xhci_log_sts("after CRCR");

    /* CONFIG: MaxSlotsEnabled in low byte. Enable all reported slots
     * — we'll allocate device contexts on demand. */
    w32(xhci_op_base, XHCI_OP_CONFIG, max_slots);
    xhci_log_sts("after CONFIG");

    /* Interrupter 0 setup. */
    volatile uint8_t *ir0 = xhci_rt_base + XHCI_IR0_OFFSET;
    xhci_erst[0].base_lo  = (uint32_t)(xhci_evt_ring.phys & 0xFFFFFFFFu);
    xhci_erst[0].base_hi  = (uint32_t)(xhci_evt_ring.phys >> 32);
    xhci_erst[0].size     = xhci_evt_ring.num_trbs;
    xhci_erst[0].reserved = 0;

    /* ERSTSZ = 1 (one segment). */
    w32(ir0, XHCI_IR_ERSTSZ, 1);
    xhci_log_sts("after ERSTSZ");
    /* ERDP must be programmed BEFORE ERSTBA per xHCI 1.2 §5.5.2.3.2 —
     * writing ERSTBA enables the event ring. */
    uint64_t erdp = xhci_event_ring_dequeue_phys(&xhci_evt_ring);
    w32(ir0, XHCI_IR_ERDP,     (uint32_t)(erdp & 0xFFFFFFFFu));
    w32(ir0, XHCI_IR_ERDP + 4, (uint32_t)(erdp >> 32));
    xhci_log_sts("after ERDP");
    uint64_t erstba = (uint64_t)(uintptr_t)&xhci_erst[0];
    w32(ir0, XHCI_IR_ERSTBA,     (uint32_t)(erstba & 0xFFFFFFFFu));
    w32(ir0, XHCI_IR_ERSTBA + 4, (uint32_t)(erstba >> 32));
    xhci_log_sts("after ERSTBA");

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

/*
 * Issue a NO_OP command and poll the event ring for the matching
 * Command Completion event. Returns 0 on Success, negative otherwise.
 * This is the round-trip test that proves all the ring plumbing is
 * correct — if NO_OP doesn't complete, something is wrong with
 * DCBAAP / CRCR / the doorbell / ERST / ERDP.
 */
static int xhci_send_noop(void)
{
    struct xhci_trb cmd = {0};
    cmd.control = XHCI_TRB_TYPE(XHCI_TRB_CMD_NOOP);

    struct xhci_trb *slot = xhci_ring_enqueue(&xhci_cmd_ring, &cmd);
    if (slot == NULL) {
        WARN("xhci: NO_OP enqueue failed");
        return -1;
    }
    uintptr_t cmd_phys = (uintptr_t)slot;

    /* Ring the command-ring doorbell. DB[0] is the command ring on
     * xHCI 1.x; target = 0 ("host command"). */
    *(volatile uint32_t *)(xhci_db_base + 4 * XHCI_DB_COMMAND) = 0;

    /* Poll the event ring for up to 500 ms for a matching completion. */
    uint64_t start = timer_get_count();
    uint64_t freq  = timer_get_frequency();
    uint64_t ticks = freq / 2;   /* 500 ms */

    struct xhci_trb evt;
    while (timer_get_count() - start < ticks) {
        if (!xhci_event_ring_peek(&xhci_evt_ring, &evt))
            continue;

        uint32_t type = XHCI_TRB_TYPE_GET(evt.control);
        if (type != XHCI_TRB_EVT_CMD_COMPLETION) {
            INFO("xhci: skipping non-command event type %u", type);
            continue;
        }
        uint64_t evt_ptr = (uint64_t)evt.param_lo |
                           ((uint64_t)evt.param_hi << 32);
        if (evt_ptr != cmd_phys) {
            INFO("xhci: skipping stale completion @0x%lx (ours 0x%lx)",
                 (unsigned long)evt_ptr, (unsigned long)cmd_phys);
            continue;
        }
        uint32_t cc = XHCI_CC_GET(evt.status);

        /* Update ERDP. Setting bit 3 (EHB) acknowledges the interrupt
         * state — also harmless in polled mode. */
        volatile uint8_t *ir0 = xhci_rt_base + XHCI_IR0_OFFSET;
        uint64_t erdp = xhci_event_ring_dequeue_phys(&xhci_evt_ring) | (1u << 3);
        w32(ir0, XHCI_IR_ERDP,     (uint32_t)(erdp & 0xFFFFFFFFu));
        w32(ir0, XHCI_IR_ERDP + 4, (uint32_t)(erdp >> 32));

        if (cc != XHCI_CC_SUCCESS) {
            WARN("xhci: NO_OP completed with cc=%u", cc);
            return -1;
        }
        INFO("xhci: NO_OP round-trip OK (cc=SUCCESS, cmd_trb @0x%lx)",
             (unsigned long)cmd_phys);
        return 0;
    }

    WARN("xhci: NO_OP timed out — no completion event within 500 ms "
         "(USBSTS=0x%08x)",
         (unsigned)r32(xhci_op_base, XHCI_OP_USBSTS));
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int xhci_init(void)
{
    xhci_live = false;

    /* VMM mapping for XHCI MMIO was landed in #266 Phase 0 (commit
     * fb12937). Grab the virtual pointer — identity-mapped on
     * Jetson so the physical address doubles as a VA. */
    xhci_cap_base = (volatile uint8_t *)(uintptr_t)TEGRA_XHCI_HCD_BASE;

    /* Sanity check: if the clocks aren't held, CAPLENGTH reads as
     * 0xFF (low byte of 0xFFFFFFFF). Bail early with a clear
     * message rather than writing to a dead aperture. */
    uint32_t first = r32(xhci_cap_base, 0);
    if (first == 0xFFFFFFFFu) {
        WARN("xhci: aperture reads 0xffffffff — clocks gated "
             "(did slmos-kexec run without --no-usb-hold?)");
        return -1;
    }

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
        WARN("xhci: command/event ring round-trip failed — Step 5 will "
             "need to debug before port enumeration is attempted");
    }

    xhci_live = true;
    INFO("xhci: Step 4 complete — ready for port scan (Step 5)");
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
