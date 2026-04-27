/*
 * camrtc.c — Tegra234 Camera RTCPU (RCE) HSP-VM transport.
 *
 * Implements the HSP-VM mailbox protocol L4T's `tegra-camera-rtcpu`
 * driver uses, distilled from `docs/reference/l4t-rtcpu-hsp-combo.c`
 * + `docs/reference/l4t-camrtc-commands.h`. SLM-OS only needs the
 * HELLO / PROTOCOL / RESUME boot-sync (steps 7 of the L4T bring-up)
 * to establish a session; CH_SETUP and IVC ring construction land
 * in a follow-up commit.
 *
 * HSP register layout (matches the BPMP HSP layout SLM-OS already
 * drives at TEGRA234_BPMP_HSP_BASE):
 *
 *   HSP_BASE + 0x000   common region (HSP_DIMENSIONING at +0x380)
 *   HSP_BASE + 0x10000 first shared mailbox (32 KB per SM)
 *                       SM[i] base = HSP_BASE + 0x10000 + i * 0x8000
 *   ...                first shared semaphore after num_sm SMs:
 *                       SS[i] base = SM_END + i * 0x10000
 *
 * Per-SM register: SHRD_MBOX at offset 0x0:
 *   bit 31      FULL flag (write to set, read to check)
 *   bits[30:0]  payload (CAMRTC_HSP_MSG-encoded)
 *
 * Per-SS register: SHRD_SEM at offset 0x0:
 *   bits[31:16] VM→FW group bits
 *   bits[15:0]  FW→VM group bits
 *   +0x04 SHRD_SEM_SET (write-1-to-set)
 *   +0x08 SHRD_SEM_CLR (write-1-to-clear)
 */

#include "camrtc.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>

#include "bpmp.h"
#include "camrtc_channels.h"
#include "debug.h"
#include "platform.h"
#include "tegra234_clocks.h"
#include "timer.h"

/* ---- HSP layout constants (mirror kernel/drivers/bpmp/hsp.c) ---- */
#define HSP_DIMENSIONING_REG      0x380u
#define HSP_COMMON_REGION_SIZE    0x10000u
#define HSP_SM_SIZE               0x8000u
#define HSP_SS_SIZE               0x10000u

#define HSP_DIM_NUM_SM_SHIFT      0u
#define HSP_DIM_NUM_SM_MASK       0xFu
#define HSP_DIM_NUM_SS_SHIFT      4u
#define HSP_DIM_NUM_SS_MASK       0xFu
#define HSP_DIM_NUM_AS_SHIFT      8u
#define HSP_DIM_NUM_AS_MASK       0xFu

/* Per-SM registers. */
#define HSP_SM_SHRD_MBOX          0x0u
#define HSP_SM_SHRD_MBOX_FULL     (1u << 31)
#define HSP_SM_PAYLOAD_MASK       0x7FFFFFFFu     /* 31-bit payload */

/* RCE PM regs (rce-pm window at TEGRA234_RCE_PM_BASE). */
#define RCE_PM_PWR_STATUS_0       0x20u
#define RCE_PM_R5_CTRL_0          0x40u
#define RCE_PM_FWLOADDONE         (1u << 1)
#define RCE_PM_WFIPIPESTOPPED     (1u << 21)

/* CAMRTC_HSP_MSG opcodes used only inside the driver (the boot-sync
 * sequence). Public opcodes that callers reference (IRQ, PING,
 * FW_HASH, CH_SETUP) live in `camrtc.h`. Full set in
 * `docs/reference/l4t-camrtc-commands.h:42-77`. */
#define CAMRTC_HSP_HELLO          0x40u
#define CAMRTC_HSP_BYE            0x41u
#define CAMRTC_HSP_RESUME         0x42u
#define CAMRTC_HSP_SUSPEND        0x43u
#define CAMRTC_HSP_PROTOCOL       0x47u

#define CAMRTC_HSP_MSG_ID_SHIFT   24u
#define CAMRTC_HSP_MSG_ID_MASK    0x7Fu
#define CAMRTC_HSP_MSG_PARAM_MASK 0xFFFFFFu

#define RTCPU_DRIVER_SM6_VERSION  6u
#define RTCPU_FW_INVALID_VERSION  0xFFFFFFu

/* Per-SM mailbox indices used by the camera-rtcpu HSP-VM-1 client.
 * The L4T DT for hsp-vm1 ties:
 *   vm-tx = SM 0  (CCPLEX → RCE)
 *   vm-rx = SM 1  (RCE → CCPLEX)
 *   vm-ss = SS 0  (group bits)
 * Defaults verified live against `tegra234-camera.dtsi` hsp-vm1 node. */
#define CAMRTC_VM_TX_SM_IDX       0u
#define CAMRTC_VM_RX_SM_IDX       1u
#define CAMRTC_VM_SS_IDX          0u

/* SS register layout (per docs/reference/linux-tegra-hsp.c). */
#define HSP_SS_SHRD_SEM           0x0u
#define HSP_SS_SHRD_SEM_SET       0x4u
#define HSP_SS_SHRD_SEM_CLR       0x8u

/* Generous timeouts. RCE round-trips are typically a few microseconds;
 * a generous 100 ms ceiling lets a stale-message HELLO loop converge
 * even after several discarded frames without spending forever. */
#define CAMRTC_HELLO_TIMEOUT_US      100000u
#define CAMRTC_HANDSHAKE_TIMEOUT_US  100000u

/* CH_SETUP needs a more generous ceiling than the boot-sync messages:
 * RCE has to walk the TLV array, validate IOVAs against its VM1
 * aperture, and allocate IVC bookkeeping. L4T's `cmd_timeout`
 * defaults to 2 s for all camrtc-hsp commands; 1 s is enough headroom
 * to absorb a once-in-a-blue-moon scheduling stall in RCE without
 * making `rcediag` feel hung. */
#define CAMRTC_CH_SETUP_TIMEOUT_US   1000000u

/* ---- MMIO accessors ---- */

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v = *(volatile uint32_t *)addr;
    /* Order MMIO read against any subsequent write. Mirrors the
     * BPMP-side HSP accessor pattern. */
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void mmio_write32(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- HSP base + mailbox addressing ---- */

static bool       g_initialised;
static uintptr_t  g_vm_tx_addr;   /* SHRD_MBOX of TX mailbox */
static uintptr_t  g_vm_rx_addr;   /* SHRD_MBOX of RX mailbox */

static uintptr_t hsp_sm_addr(uintptr_t hsp_base, uint32_t idx)
{
    return hsp_base + HSP_COMMON_REGION_SIZE + idx * HSP_SM_SIZE
         + HSP_SM_SHRD_MBOX;
}

/* Compute SS[idx] base. SS region follows all num_sm SMs. */
static uintptr_t hsp_ss_base(uintptr_t hsp_base, uint32_t num_sm,
                             uint32_t ss_idx)
{
    return hsp_base + HSP_COMMON_REGION_SIZE
         + num_sm * HSP_SM_SIZE
         + ss_idx * HSP_SS_SIZE;
}

/* ---- CAMRTC_HSP_MSG codec ---- */

static inline uint32_t camrtc_msg_pack(uint32_t id, uint32_t param)
{
    /* The 24-bit param is masked; the id occupies bits[30:24]. The
     * FULL bit (31) is added by the SM TX path, not by the codec. */
    return ((id & CAMRTC_HSP_MSG_ID_MASK) << CAMRTC_HSP_MSG_ID_SHIFT)
         | (param & CAMRTC_HSP_MSG_PARAM_MASK);
}

static inline uint32_t camrtc_msg_id(uint32_t msg)
{
    return (msg >> CAMRTC_HSP_MSG_ID_SHIFT) & CAMRTC_HSP_MSG_ID_MASK;
}

static inline uint32_t camrtc_msg_param(uint32_t msg)
{
    return msg & CAMRTC_HSP_MSG_PARAM_MASK;
}

/* ---- Shared-mailbox TX / RX primitives ---- */

/* Wait for VM-TX to drain (FULL bit clears) — RCE has consumed the
 * previous message. Returns 0 on success, -1 on timeout. */
static int sm_tx_wait_empty(uint32_t timeout_us)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0u) return -1;
    uint64_t deadline = timer_get_count()
                      + (timeout_us * freq + 999999ULL) / 1000000ULL;
    for (;;) {
        uint32_t v = mmio_read32(g_vm_tx_addr);
        if ((v & HSP_SM_SHRD_MBOX_FULL) == 0u) return 0;
        if (timer_get_count() >= deadline) return -1;
    }
}

/* Push a message into VM-TX. The caller must have called
 * sm_tx_wait_empty first to confirm the mailbox is drained. */
static void sm_tx_send(uint32_t msg)
{
    /* FULL-bit set in the same write that delivers the payload. */
    mmio_write32(g_vm_tx_addr, msg | HSP_SM_SHRD_MBOX_FULL);
}

/* Poll VM-RX for an incoming message. On success returns 0 and stores
 * the 31-bit payload (FULL bit stripped) into *out_msg; the FULL bit
 * is cleared in hardware as a side-effect of the read+writeback (we
 * write 0 to ack so the sender can post the next message). */
static int sm_rx_recv(uint32_t *out_msg, uint32_t timeout_us)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0u) return -1;
    uint64_t deadline = timer_get_count()
                      + (timeout_us * freq + 999999ULL) / 1000000ULL;
    for (;;) {
        uint32_t v = mmio_read32(g_vm_rx_addr);
        if ((v & HSP_SM_SHRD_MBOX_FULL) != 0u) {
            *out_msg = v & HSP_SM_PAYLOAD_MASK;
            /* Ack — clear FULL bit so RCE's next TX can land. */
            mmio_write32(g_vm_rx_addr, 0u);
            return 0;
        }
        if (timer_get_count() >= deadline) return -1;
    }
}

/* Generate a 24-bit cookie. Anything random-ish is fine — RCE just
 * echoes it back so we can ignore stale messages from a previous
 * (Linux) session. timer_get_count() is monotonic and changes
 * frequently; the lower 24 bits are sufficiently entropic.
 *
 * Avoid 0: L4T's `camrtc_hsp_vm_cookie` (cached at
 * `docs/reference/l4t-rtcpu-hsp-combo.c:310`) explicitly increments
 * past 0. Whether RCE treats cookie==0 specially is undocumented,
 * so mirror the defensive check rather than discover an edge case
 * the hard way. */
static uint32_t camrtc_make_cookie(void)
{
    uint64_t now = timer_get_count();
    uint32_t value = (uint32_t)((now >> 5) & CAMRTC_HSP_MSG_PARAM_MASK);
    if (value == 0u) value = 1u;
    return value;
}

/* ---- Public API: handshake ---- */

int camrtc_init(void)
{
    if (g_initialised) return 0;

    /* Re-engage RCE before talking HSP-VM. Linux's kexec
     * `.shutdown` callback for `tegra-camera-rtcpu` does the
     * inverse of this: sends `CAMRTC_HSP_BYE` to RCE, then calls
     * `tegra_camrtc_poweroff` which asserts `RESET_RCE_ALL` and
     * disables the rce clocks (cached at
     * `docs/reference/l4t-tegra-camera-rtcpu.c:893,1402`). After
     * kexec, R5 stops executing (clock-gated) even though
     * `R5_CTRL_0.FWLOADDONE` stays set. SLM-OS's HELLO writes to
     * SM[0] then sit forever because the HSP-VM ISR isn't running.
     *
     * Mirroring `tegra_camrtc_poweron` (RCE clocks on, reset
     * deasserted) restarts the firmware in place — no FW reload
     * needed because the RCE carveout in DRAM is preserved across
     * kexec. Verified live on jetson-nano-1: after this sequence
     * the firmware prints its boot line ("Camera-FW on
     * t234-rce-safe ready") on the shared console and the HELLO
     * + PROTOCOL + RESUME handshake completes. Each MRQ is
     * idempotent, so the cost on a healthy RCE is one BPMP
     * round-trip per call. */
    int rc = bpmp_clk_enable(TEGRA234_CLK_RCE_CPU_NIC);
    if (rc != 0) {
        WARN("camrtc: bpmp_clk_enable(RCE_CPU_NIC) rc=%d", rc);
        return -2;
    }
    rc = bpmp_clk_enable(TEGRA234_CLK_RCE_NIC);
    if (rc != 0) {
        WARN("camrtc: bpmp_clk_enable(RCE_NIC) rc=%d", rc);
        return -2;
    }
    rc = bpmp_clk_enable(TEGRA234_CLK_RCE_CPU);
    if (rc != 0) {
        WARN("camrtc: bpmp_clk_enable(RCE_CPU) rc=%d", rc);
        return -2;
    }
    rc = bpmp_reset_deassert(TEGRA234_RESET_RCE_ALL);
    if (rc != 0) {
        WARN("camrtc: bpmp_reset_deassert(RCE_ALL) rc=%d", rc);
        return -2;
    }
    /* Give the firmware a moment to re-initialise. ~10 ms is
     * empirically generous — the live trace shows the boot line
     * within 1.2 ms of clock-on on jetson-nano-1. */
    timer_busy_wait_us(10000u);

    uintptr_t hsp_base = (uintptr_t)TEGRA234_RCE_HSP_BASE;
    uintptr_t pm_base  = (uintptr_t)TEGRA234_RCE_PM_BASE;

    /* Probe HSP_DIMENSIONING. 0xFFFFFFFF means CBB firewall blocked
     * the read (or HSP isn't powered). Any other value means we have
     * MMIO. */
    uint32_t dim = mmio_read32(hsp_base + HSP_DIMENSIONING_REG);
    if (dim == 0xFFFFFFFFu || dim == 0u) {
        WARN("camrtc: hsp_rce DIMENSIONING=0x%x — block unreachable",
             (unsigned)dim);
        return -1;
    }
    uint32_t num_sm = (dim >> HSP_DIM_NUM_SM_SHIFT) & HSP_DIM_NUM_SM_MASK;
    if (num_sm < 2u) {
        WARN("camrtc: hsp_rce only has %u shared mailboxes "
             "(need ≥ 2)", (unsigned)num_sm);
        return -1;
    }
    g_vm_tx_addr = hsp_sm_addr(hsp_base, CAMRTC_VM_TX_SM_IDX);
    g_vm_rx_addr = hsp_sm_addr(hsp_base, CAMRTC_VM_RX_SM_IDX);
    INFO("camrtc: hsp_rce DIMENSIONING=0x%08x (SM=%u) — TX@0x%lx RX@0x%lx",
         (unsigned)dim, (unsigned)num_sm,
         (unsigned long)g_vm_tx_addr, (unsigned long)g_vm_rx_addr);

    /* Probe RCE firmware state. FWLOADDONE must be set. */
    uint32_t r5_ctrl = mmio_read32(pm_base + RCE_PM_R5_CTRL_0);
    if ((r5_ctrl & RCE_PM_FWLOADDONE) == 0u) {
        WARN("camrtc: R5_CTRL_0=0x%x — RCE firmware not loaded "
             "(bootloader didn't release R5)", (unsigned)r5_ctrl);
        return -2;
    }
    INFO("camrtc: R5_CTRL_0=0x%x (FWLOADDONE=1)", (unsigned)r5_ctrl);

    /* HELLO handshake. Loop discards stale traffic from a previous
     * (Linux) session until the cookie matches. L4T's
     * camrtc_hsp_vm_hello uses the same loop shape. */
    uint32_t cookie = camrtc_make_cookie();
    uint32_t request = camrtc_msg_pack(CAMRTC_HSP_HELLO, cookie);

    /* Drain any stale RX message before posting our request. The
     * pre-handshake state from kexec usually has FULL clear, but
     * defensively clear it anyway. */
    uint32_t rx_pre = mmio_read32(g_vm_rx_addr);
    if ((rx_pre & HSP_SM_SHRD_MBOX_FULL) != 0u) {
        INFO("camrtc: stale RX 0x%x cleared before HELLO",
             (unsigned)(rx_pre & HSP_SM_PAYLOAD_MASK));
        mmio_write32(g_vm_rx_addr, 0u);
    }

    /* Drain stale SS bits left by the previous (Linux) session. The
     * camrtc_hsp_rx_full_notify path on Linux clears FW-side bits
     * after observing them; if Linux idle-suspended mid-stream,
     * leftover bits could keep RCE in a state where it's waiting
     * for the AP to clear them before processing new HSP-VM
     * messages. Clear all 32 bits (SET register convention: 1 ⇒
     * clear). */
    uintptr_t ss_addr = hsp_ss_base(hsp_base, num_sm, CAMRTC_VM_SS_IDX);
    uint32_t ss_pre = mmio_read32(ss_addr + HSP_SS_SHRD_SEM);
    if (ss_pre != 0u) {
        INFO("camrtc: stale SS[%u]=0x%x cleared before HELLO",
             (unsigned)CAMRTC_VM_SS_IDX, (unsigned)ss_pre);
        mmio_write32(ss_addr + HSP_SS_SHRD_SEM_CLR, 0xFFFFFFFFu);
    }

    /* IRQ wake first — RCE's HSP-VM ISR may need a "you have a
     * message" wake before it processes higher-level opcodes after
     * being idle in WFI. L4T's `camrtc_hsp_vm_send_irqmsg` uses
     * this for IVC ring notifications; trying it before HELLO to
     * see if it kicks RCE out of WFI without leaving a session-
     * state record. The IRQ message is one-way (no response
     * expected), so we just write it and immediately follow with
     * HELLO. */
    if (sm_tx_wait_empty(CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: VM-TX never drained for IRQ wake");
        return -3;
    }
    sm_tx_send(camrtc_msg_pack(CAMRTC_HSP_IRQ, 1u));
    /* Give RCE a moment to drain the IRQ message before posting
     * HELLO. If RCE drains within 2ms, we know the wake-up path
     * works and the HELLO that follows should also be processed. */
    timer_busy_wait_us(2000u);
    uint32_t tx_after_irq = mmio_read32(g_vm_tx_addr);
    INFO("camrtc: 2ms after IRQ wake — TX=0x%08x (FULL=%u)",
         (unsigned)tx_after_irq,
         (unsigned)((tx_after_irq & HSP_SM_SHRD_MBOX_FULL) >> 31));

    if (sm_tx_wait_empty(CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: VM-TX never drained for HELLO");
        return -3;
    }
    sm_tx_send(request);

    /* Diagnostic: wait briefly and snapshot TX/RX state so we can
     * tell "RCE never saw the message" (TX FULL still set) from
     * "RCE saw it but didn't reply" (TX FULL cleared, RX still 0). */
    timer_busy_wait_us(2000u);
    uint32_t tx_after = mmio_read32(g_vm_tx_addr);
    uint32_t rx_after = mmio_read32(g_vm_rx_addr);
    INFO("camrtc: 2ms after HELLO write — TX=0x%08x (FULL=%u) RX=0x%08x (FULL=%u)",
         (unsigned)tx_after,
         (unsigned)((tx_after & HSP_SM_SHRD_MBOX_FULL) >> 31),
         (unsigned)rx_after,
         (unsigned)((rx_after & HSP_SM_SHRD_MBOX_FULL) >> 31));

    /* Wait for HELLO echo with matching cookie. Discard non-HELLO
     * traffic and HELLO with mismatched cookies. */
    uint64_t freq = timer_get_frequency();
    uint64_t deadline = timer_get_count()
                      + (CAMRTC_HELLO_TIMEOUT_US * freq + 999999ULL)
                          / 1000000ULL;
    for (;;) {
        uint32_t resp = 0;
        int rx_rc = sm_rx_recv(&resp, 5000u);
        if (rx_rc != 0) {
            if (timer_get_count() >= deadline) {
                WARN("camrtc: HELLO response timeout (cookie=0x%x)",
                     (unsigned)cookie);
                return -3;
            }
            continue;
        }
        if (camrtc_msg_id(resp) == CAMRTC_HSP_HELLO
            && camrtc_msg_param(resp) == cookie) {
            INFO("camrtc: HELLO echo matched (cookie=0x%x)",
                 (unsigned)cookie);
            break;
        }
        INFO("camrtc: discarding stale RX 0x%x (waiting for HELLO 0x%x)",
             (unsigned)resp, (unsigned)cookie);
        if (timer_get_count() >= deadline) {
            WARN("camrtc: HELLO response timeout draining stale traffic");
            return -3;
        }
    }

    /* PROTOCOL exchange — confirm RCE speaks SM6 (the version in the
     * cached L4T R35 driver). */
    request = camrtc_msg_pack(CAMRTC_HSP_PROTOCOL, RTCPU_DRIVER_SM6_VERSION);
    if (sm_tx_wait_empty(CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: VM-TX never drained for PROTOCOL");
        return -3;
    }
    sm_tx_send(request);
    uint32_t resp = 0;
    if (sm_rx_recv(&resp, CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: PROTOCOL response timeout");
        return -3;
    }
    uint32_t fw_version = camrtc_msg_param(resp);
    if (camrtc_msg_id(resp) != CAMRTC_HSP_PROTOCOL
        || fw_version == RTCPU_FW_INVALID_VERSION) {
        WARN("camrtc: PROTOCOL mismatch (resp=0x%x, expected SM6)",
             (unsigned)resp);
        return -4;
    }
    INFO("camrtc: RCE FW protocol version=%u (SM6 expected)",
         (unsigned)fw_version);

    /* RESUME — activates camera HW gating. Cookie reused from HELLO
     * (L4T does the same). */
    request = camrtc_msg_pack(CAMRTC_HSP_RESUME, cookie);
    if (sm_tx_wait_empty(CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: VM-TX never drained for RESUME");
        return -5;
    }
    sm_tx_send(request);
    resp = 0;
    if (sm_rx_recv(&resp, CAMRTC_HANDSHAKE_TIMEOUT_US) != 0) {
        WARN("camrtc: RESUME response timeout");
        return -5;
    }
    if (camrtc_msg_id(resp) != CAMRTC_HSP_RESUME) {
        WARN("camrtc: RESUME unexpected resp=0x%x", (unsigned)resp);
        return -5;
    }
    INFO("camrtc: RESUME ack (status=0x%x)",
         (unsigned)camrtc_msg_param(resp));

    g_initialised = true;
    return 0;
}

int camrtc_send_msg(uint32_t msg_id, uint32_t param,
                    uint32_t *resp_param, uint32_t timeout_us)
{
    if (!g_initialised) return -1;

    uint32_t request = camrtc_msg_pack(msg_id, param);
    if (sm_tx_wait_empty(timeout_us) != 0) {
        WARN("camrtc: VM-TX never drained for msg_id=0x%x "
             "(timeout=%uus)", (unsigned)msg_id, (unsigned)timeout_us);
        return -2;
    }
    sm_tx_send(request);

    /* Drain unidirectional traffic while waiting for the matching
     * response. Per L4T `rtcpu-hsp-combo.c:151-159`, RX messages
     * with opcode == CAMRTC_HSP_IRQ (0x00) are IVC-group
     * notifications and any opcode < CAMRTC_HSP_HELLO (0x40) is a
     * unidirectional notification — only opcodes >= 0x40 are
     * responses to outbound commands. RCE commonly emits an IRQ
     * before answering CH_SETUP (the SS[0] semaphore wake also
     * triggers a mailbox-side IRQ message), so a single recv that
     * insists on the matching opcode races against that path. */
    uint64_t freq = timer_get_frequency();
    uint64_t ticks_per_us = freq / 1000000u;
    if (ticks_per_us == 0) ticks_per_us = 1u;
    uint64_t deadline = timer_get_count()
                      + (uint64_t)timeout_us * ticks_per_us;
    for (;;) {
        uint32_t resp = 0;
        uint32_t remaining_us = 0;
        uint64_t now = timer_get_count();
        if (now < deadline) {
            remaining_us = (uint32_t)((deadline - now) / ticks_per_us);
            if (remaining_us == 0) remaining_us = 1u;
        }
        if (sm_rx_recv(&resp, remaining_us) != 0) {
            WARN("camrtc: VM-RX timeout waiting for response to "
                 "msg_id=0x%x (timeout=%uus)",
                 (unsigned)msg_id, (unsigned)timeout_us);
            return -2;
        }
        uint32_t resp_id = camrtc_msg_id(resp);
        if (resp_id == msg_id) {
            if (resp_param) *resp_param = camrtc_msg_param(resp);
            return 0;
        }
        if (resp_id < CAMRTC_HSP_HELLO) {
            /* Unidirectional notification (IRQ or other) — drain
             * and keep waiting for the real response. */
            INFO("camrtc: drained unidirectional RX 0x%x while "
                 "waiting for response to 0x%x",
                 (unsigned)resp, (unsigned)msg_id);
            if (timer_get_count() >= deadline) {
                WARN("camrtc: VM-RX timeout draining stale traffic "
                     "for msg_id=0x%x", (unsigned)msg_id);
                return -2;
            }
            continue;
        }
        WARN("camrtc: unexpected response id 0x%x (sent 0x%x)",
             (unsigned)resp_id, (unsigned)msg_id);
        return -3;
    }
}

int camrtc_send_irq(uint32_t msg_id, uint32_t param, uint32_t timeout_us)
{
    if (!g_initialised) return -1;

    if (sm_tx_wait_empty(timeout_us) != 0) {
        WARN("camrtc: VM-TX never drained for IRQ msg_id=0x%x "
             "(timeout=%uus)", (unsigned)msg_id, (unsigned)timeout_us);
        return -2;
    }
    sm_tx_send(camrtc_msg_pack(msg_id, param));
    return 0;
}

int camrtc_diag_dump(void)
{
    uintptr_t hsp_base = (uintptr_t)TEGRA234_RCE_HSP_BASE;
    uintptr_t pm_base  = (uintptr_t)TEGRA234_RCE_PM_BASE;

    uint32_t dim       = mmio_read32(hsp_base + HSP_DIMENSIONING_REG);
    uint32_t r5_ctrl   = mmio_read32(pm_base + RCE_PM_R5_CTRL_0);
    uint32_t pwr_stat  = mmio_read32(pm_base + RCE_PM_PWR_STATUS_0);
    uint32_t tx_sm     = mmio_read32(hsp_sm_addr(hsp_base, CAMRTC_VM_TX_SM_IDX));
    uint32_t rx_sm     = mmio_read32(hsp_sm_addr(hsp_base, CAMRTC_VM_RX_SM_IDX));

    INFO("camrtc: hsp_rce DIMENSIONING=0x%08x (SM=%u SS=%u)",
         (unsigned)dim,
         (unsigned)((dim >> HSP_DIM_NUM_SM_SHIFT) & HSP_DIM_NUM_SM_MASK),
         (unsigned)((dim >> HSP_DIM_NUM_SS_SHIFT) & HSP_DIM_NUM_SS_MASK));
    INFO("camrtc: rce-pm R5_CTRL_0=0x%08x (FWLOADDONE=%u)",
         (unsigned)r5_ctrl, (unsigned)((r5_ctrl & RCE_PM_FWLOADDONE) >> 1));
    INFO("camrtc: rce-pm PWR_STATUS_0=0x%08x (WFIPIPESTOPPED=%u)",
         (unsigned)pwr_stat,
         (unsigned)((pwr_stat & RCE_PM_WFIPIPESTOPPED) >> 21));
    INFO("camrtc: VM-TX SHRD_MBOX=0x%08x (FULL=%u)",
         (unsigned)tx_sm, (unsigned)((tx_sm & HSP_SM_SHRD_MBOX_FULL) >> 31));
    INFO("camrtc: VM-RX SHRD_MBOX=0x%08x (FULL=%u)",
         (unsigned)rx_sm, (unsigned)((rx_sm & HSP_SM_SHRD_MBOX_FULL) >> 31));
    return 0;
}

/* ---- CH_SETUP: capture-control IVC channel ---- */

/* Wire-format channel parameters for the IMX219 capture-control
 * channel. Match `tegra234-camera.dtsi` ivccontrol@3:
 *   nvidia,service     = "capture-control"
 *   nvidia,group       = <1>
 *   nvidia,frame-count = <64>
 *   nvidia,frame-size  = <320>
 *   nvidia,version     = (omitted → 0) */
#define CAMRTC_CTRL_GROUP        1u
#define CAMRTC_CTRL_NFRAMES      64u
#define CAMRTC_CTRL_FRAME_SIZE   320u
#define CAMRTC_CTRL_VERSION      0u

/* Per-direction queue: header (128 B) + nframes * frame_size. Both
 * directions are equal-sized in this protocol. */
#define CAMRTC_CTRL_QUEUE_BYTES  \
    (TEGRA_IVC_HEADER_SIZE + CAMRTC_CTRL_NFRAMES * CAMRTC_CTRL_FRAME_SIZE)

/* Region layout: 4 KB config block + rx queue + tx queue.
 *   = 4096 + 2 * (128 + 64*320)
 *   = 4096 + 2 * 20608
 *   = 45312 bytes
 * Round up to 64 KB so the region is aligned to the L4T-DT
 * `nvidia,ivc-channels = <... 0x10000>` size convention. */
#define CAMRTC_CTRL_REGION_BYTES \
    (CAMRTC_IVC_CONFIG_SIZE + 2u * CAMRTC_CTRL_QUEUE_BYTES)
#define CAMRTC_CTRL_REGION_RESERVED  0x10000u  /* 64 KB */

/* Fixed physical address for the IVC config + ring region.
 *
 * Two constraints:
 *  1. RCE only accepts CH_SETUP IOVAs inside its compiled-in VM1
 *     aperture 0xA0000000..0xC0000000 (per
 *     `docs/reference/l4t-binding-nvidia-tegra194-rce.txt:51-53`).
 *     With SMMU translation disabled by Linux pre-kexec, RCE sees
 *     physical addresses directly, so the region must be a
 *     physical page in that aperture.
 *  2. AP↔RCE coherency: cacheable kernel-linear mappings at
 *     0xA0000000 didn't propagate AP writes to DRAM in time for
 *     RCE to consume frames (verified by peeking the rx ring
 *     after a send and seeing RCE's count stuck at 0 even though
 *     the SS notify went through). The cacheable path with DC
 *     CVAC also failed empirically. So the region lives at
 *     0xBDFE0000 — inside the existing 2 MB NC (Normal Non-
 *     Cacheable, MAIR index 2) mapping at 0xBDE00000-0xBDFFFFFF
 *     set up by `vmm_init`. NC bypasses L1/L2 entirely; AP writes
 *     hit DRAM immediately and RCE reads see them without any
 *     cache maintenance. The 64 KB region sits well past the
 *     scheduler's bump allocator high-water (~25 KB at
 *     NC_MEM_BASE) and below the diagnostic trace slot at
 *     NC_MEM_END - 256.
 */
#define CAMRTC_CTRL_REGION_PHYS    0xBDFE0000u

static uintptr_t g_ch_setup_region_phys;

uintptr_t camrtc_ch_setup_region_phys(void)
{
    return g_ch_setup_region_phys;
}

int camrtc_ch_setup_capture_control(void)
{
    if (!g_initialised) {
        WARN("camrtc: ch_setup called before camrtc_init");
        return -2;
    }

    /* Region lives at a fixed physical address in RCE's VM1 IOVA
     * aperture (carved out of PMM in `kernel/mm/pmm.c`). The kernel
     * linear map identity-maps low DRAM, so the physical address is
     * also the virtual address callers can dereference. */
    uintptr_t region_phys = CAMRTC_CTRL_REGION_PHYS;

    /* CH_SETUP encodes the region IOVA shifted right by 8. The
     * 24-bit MSG param holds bits[31:8] of the IOVA. RCE rejects
     * addresses outside its built-in VM1 aperture
     * 0xA0000000..0xC0000000 with RTCPU_CH_ERR_INVALID_IOVA. Pin
     * that the chosen `CAMRTC_CTRL_REGION_PHYS` falls inside both
     * the RCE aperture *and* the existing NC mapping at
     * 0xBDE00000-0xBDFFFFFF. */
    _Static_assert(CAMRTC_CTRL_REGION_PHYS >= 0xA0000000u,
                   "CH_SETUP region must lie at or above the RCE VM1 "
                   "aperture base (0xA0000000)");
    _Static_assert(CAMRTC_CTRL_REGION_PHYS + CAMRTC_CTRL_REGION_RESERVED
                   <= 0xC0000000u,
                   "CH_SETUP region must end at or below the RCE VM1 "
                   "aperture top (0xC0000000)");
    _Static_assert(CAMRTC_CTRL_REGION_PHYS >= 0xBDE00000u,
                   "CH_SETUP region must lie inside the NC mapping "
                   "(NC base = 0xBDE00000)");
    _Static_assert(CAMRTC_CTRL_REGION_PHYS + CAMRTC_CTRL_REGION_RESERVED
                   <= 0xBE000000u,
                   "CH_SETUP region must lie inside the NC mapping "
                   "(NC end = 0xBE000000)");
    uint64_t iova_shifted = (uint64_t)region_phys >> 8;

    /* Zero the entire region so the TLV terminator + IVC ring
     * headers start clean. PMM doesn't guarantee zeroed pages.
     * Region base is 4 KB aligned and the size is divisible by 8
     * (45312 bytes = 4096 + 2*(128 + 64*320)) — pinned by the
     * static_asserts below — so word-at-a-time writes are safe. */
    _Static_assert((CAMRTC_CTRL_REGION_PHYS & 7u) == 0u,
                   "CH_SETUP region must be 8-byte aligned for the zero loop");
    _Static_assert((CAMRTC_CTRL_REGION_BYTES & 7u) == 0u,
                   "CH_SETUP region size must be 8-byte multiple for the zero loop");
    _Static_assert(CAMRTC_CTRL_REGION_BYTES <= CAMRTC_CTRL_REGION_RESERVED,
                   "CH_SETUP region must fit inside its PMM carveout");
    volatile uint64_t *region_words = (volatile uint64_t *)region_phys;
    for (uint32_t i = 0; i < CAMRTC_CTRL_REGION_BYTES / sizeof(uint64_t); i++) {
        region_words[i] = 0;
    }

    /* Build the TLV entry at offset 0. The rx queue starts at
     * +CAMRTC_IVC_CONFIG_SIZE, the tx queue starts at
     * +CAMRTC_IVC_CONFIG_SIZE + CAMRTC_CTRL_QUEUE_BYTES. */
    uintptr_t rx_iova = region_phys + CAMRTC_IVC_CONFIG_SIZE;
    uintptr_t tx_iova = rx_iova + CAMRTC_CTRL_QUEUE_BYTES;

    volatile struct camrtc_tlv_ivc_setup *tlv =
        (volatile struct camrtc_tlv_ivc_setup *)region_phys;
    tlv->tag           = CAMRTC_TAG_IVC_SETUP;
    tlv->len           = sizeof(struct camrtc_tlv_ivc_setup);
    tlv->rx_iova       = (uint64_t)rx_iova;
    tlv->rx_frame_size = CAMRTC_CTRL_FRAME_SIZE;
    tlv->rx_nframes    = CAMRTC_CTRL_NFRAMES;
    tlv->tx_iova       = (uint64_t)tx_iova;
    tlv->tx_frame_size = CAMRTC_CTRL_FRAME_SIZE;
    tlv->tx_nframes    = CAMRTC_CTRL_NFRAMES;
    tlv->channel_group = CAMRTC_CTRL_GROUP;
    tlv->ivc_version   = CAMRTC_CTRL_VERSION;
    /* Inline strcpy: "capture-control\0" — 16 bytes including NUL,
     * fits in the 32-byte field. Manual copy because <string.h> is
     * libc-only on bare metal. The static_assert keeps a future
     * rename to a longer service name from silently overflowing into
     * the next TLV (today's terminator). */
    static const char ctrl_name[] = "capture-control";
    _Static_assert(sizeof(ctrl_name) <= sizeof(((struct camrtc_tlv_ivc_setup *)0)->ivc_service),
                   "capture-control service name must fit in the 32-byte ivc_service field");
    for (uint32_t i = 0; i < sizeof(ctrl_name); i++) {
        tlv->ivc_service[i] = ctrl_name[i];
    }

    /* Terminator entry — tag = 0. The region was zeroed above so
     * the terminator is already in place; this comment makes that
     * explicit so a future maintainer doesn't add a redundant
     * zero-write here that masks an upstream bug. */

    /* Memory barrier: make sure all the TLV writes are visible
     * before RCE reads the region. RCE accesses physical memory
     * through the (post-kexec, bypass) SMMU, so a DSB is sufficient
     * — no cache maintenance needed here because PMM pages are in
     * the cacheable kernel mapping and Tegra234 CCPLEX shares
     * coherent fabric with RCE for system DRAM. */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Send CH_SETUP. Generous timeout: RCE has to walk the TLV
     * array, validate IOVAs, allocate its bookkeeping. L4T uses
     * the same `cmd_timeout` (default 2 s) for all camrtc-hsp
     * commands; we use 1 s here. */
    INFO("camrtc: CH_SETUP region @ phys=0x%lx (>>8 = 0x%06lx) "
         "rx@0x%lx tx@0x%lx",
         (unsigned long)region_phys, (unsigned long)iova_shifted,
         (unsigned long)rx_iova, (unsigned long)tx_iova);

    uint32_t status = 0xFFFFFFu;
    int rc = camrtc_send_msg(CAMRTC_HSP_CH_SETUP,
                             (uint32_t)iova_shifted,
                             &status, CAMRTC_CH_SETUP_TIMEOUT_US);
    if (rc != 0) {
        WARN("camrtc: CH_SETUP send failed rc=%d", rc);
        return -2;
    }
    if (status != RTCPU_CH_SUCCESS) {
        WARN("camrtc: CH_SETUP rejected by RCE — status=%u "
             "(see RTCPU_CH_ERR_* in camrtc_channels.h)",
             (unsigned)status);
        return -3;
    }

    g_ch_setup_region_phys = region_phys;
    INFO("camrtc: CH_SETUP OK — capture-control bound to "
         "(group=%u, rx@0x%lx, tx@0x%lx)",
         (unsigned)CAMRTC_CTRL_GROUP,
         (unsigned long)rx_iova, (unsigned long)tx_iova);
    return 0;
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int camrtc_init(void) { return -1; }
int camrtc_send_msg(uint32_t msg_id, uint32_t param,
                    uint32_t *resp_param, uint32_t timeout_us)
{
    (void)msg_id; (void)param; (void)timeout_us;
    if (resp_param) *resp_param = 0;
    return -1;
}
int camrtc_send_irq(uint32_t msg_id, uint32_t param, uint32_t timeout_us)
{
    (void)msg_id; (void)param; (void)timeout_us;
    return -1;
}
int camrtc_diag_dump(void) { return -1; }
int camrtc_ch_setup_capture_control(void) { return -1; }
uintptr_t camrtc_ch_setup_region_phys(void) { return 0; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
