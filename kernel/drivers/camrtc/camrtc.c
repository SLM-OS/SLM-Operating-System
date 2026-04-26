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

#include "debug.h"
#include "platform.h"
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

/* CAMRTC_HSP_MSG opcodes (subset — full set in
 * docs/reference/l4t-camrtc-commands.h). */
#define CAMRTC_HSP_HELLO          0x40u
#define CAMRTC_HSP_BYE            0x41u
#define CAMRTC_HSP_RESUME         0x42u
#define CAMRTC_HSP_SUSPEND        0x43u
#define CAMRTC_HSP_CH_SETUP       0x44u
#define CAMRTC_HSP_PING           0x45u
#define CAMRTC_HSP_FW_HASH        0x46u
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
 * frequently; the lower 24 bits are sufficiently entropic. */
static uint32_t camrtc_make_cookie(void)
{
    uint64_t now = timer_get_count();
    return (uint32_t)((now >> 5) & CAMRTC_HSP_MSG_PARAM_MASK);
}

/* ---- Public API: handshake ---- */

int camrtc_init(void)
{
    if (g_initialised) return 0;

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
        int rc = sm_rx_recv(&resp, 5000u);
        if (rc != 0) {
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
    if (sm_tx_wait_empty(timeout_us) != 0) return -2;
    sm_tx_send(request);

    uint32_t resp = 0;
    if (sm_rx_recv(&resp, timeout_us) != 0) return -2;

    if (camrtc_msg_id(resp) != msg_id) {
        WARN("camrtc: unexpected response id 0x%x (sent 0x%x)",
             (unsigned)camrtc_msg_id(resp), (unsigned)msg_id);
        return -3;
    }
    if (resp_param) *resp_param = camrtc_msg_param(resp);
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

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

int camrtc_init(void) { return -1; }
int camrtc_send_msg(uint32_t msg_id, uint32_t param,
                    uint32_t *resp_param, uint32_t timeout_us)
{
    (void)msg_id; (void)param; (void)timeout_us;
    if (resp_param) *resp_param = 0;
    return -1;
}
int camrtc_diag_dump(void) { return -1; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
