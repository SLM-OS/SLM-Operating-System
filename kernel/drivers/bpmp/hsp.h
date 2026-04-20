/*
 * kernel/drivers/bpmp/hsp.h - BPMP-side HSP doorbell (private header)
 *
 * The HSP (Hardware Synchronization Primitives) controller is a single
 * MMIO block that multiplexes several IPC primitives between the Tegra234
 * masters (CCPLEX, BPMP, SCE, SPE, ...). The layout is:
 *
 *   HSP_BASE + 0x000   common registers (HSP_DIMENSIONING at +0x380)
 *   HSP_BASE + 0x10000 first SharedMailbox (32 KB each; count from DIMENSIONING)
 *              +       ... SharedSemaphores (64 KB each)
 *              +       ... ArbitratedSemaphores (64 KB each)
 *   <dynamic>          Doorbell block 0, 1, 2, 3, 4, 5, 6, 7 at 0x100 stride
 *
 * This driver reads DIMENSIONING at init and computes the BPMP doorbell
 * block address at HSP_BASE + (1 + num_sm/2 + num_ss + num_as) * 0x10000
 *                           + HSP_DB_BPMP * 0x100.
 *
 * References:
 *   docs/reference/edk2-nvidia-hspdoorbell.c (HspDoorbellInit)
 *   docs/reference/linux-tegra-hsp.c         (tegra_hsp_doorbell_setup)
 */

#ifndef DRIVERS_BPMP_HSP_H
#define DRIVERS_BPMP_HSP_H

#include <stdbool.h>
#include <stdint.h>

/* Doorbell register offsets within a single doorbell block. */
#define HSP_DB_REG_TRIGGER  0x0
#define HSP_DB_REG_ENABLE   0x4
#define HSP_DB_REG_RAW      0x8
#define HSP_DB_REG_PENDING  0xC

/* Physical doorbell block indexes (Tegra186/194/234 share this layout). */
#define HSP_DB_IDX_DPMU     0
#define HSP_DB_IDX_CCPLEX   1
#define HSP_DB_IDX_CCPLEX_TZ 2
#define HSP_DB_IDX_BPMP     3
#define HSP_DB_IDX_SPE      4
#define HSP_DB_IDX_SCE      5
#define HSP_DB_IDX_APE      6
#define HSP_DB_IDX_DBG      7

/*
 * Master bit positions inside the ENABLE / RAW / PENDING registers of any
 * doorbell block. If CCPLEX rings the BPMP doorbell (block 3), BPMP's
 * PENDING register shows bit HSP_DB_MASTER_CCPLEX set. Conversely, for
 * CCPLEX to check "did BPMP notify me?", it reads PENDING on block 1
 * (CCPLEX's own block) and checks bit HSP_DB_MASTER_BPMP.
 */
#define HSP_DB_MASTER_CCPLEX    17
#define HSP_DB_MASTER_BPMP      19

/*
 * Tegra234 HSP doorbell stride — from
 *   docs/reference/linux-tegra-hsp.c:959 (tegra234_hsp_soc.reg_stride).
 */
#define HSP_DB_BLOCK_STRIDE     0x100

/*
 * Initialise the HSP driver and compute the BPMP doorbell location.
 *   hsp_base — MMIO base of the HSP-@0x03C00000 controller (HSP_TOP_BASE).
 * Returns 0 on success, negative on error.
 */
int hsp_init(uintptr_t hsp_base);

/* Accessors for callers that want to log the probed values (e.g. hspdiag). */
uint32_t  hsp_dimensioning_raw(void);
uintptr_t hsp_bpmp_doorbell_addr(void);
uintptr_t hsp_ccplex_doorbell_addr(void);

/*
 * Ring the BPMP doorbell.
 * Returns:
 *    0         BPMP notified.
 *   -1         HSP not initialised.
 *   -2         BPMP has not authorised CCPLEX to ring (ENABLE bit clear).
 */
int hsp_ring_bpmp(void);

/*
 * Check whether the CCPLEX-side doorbell has a pending notification from
 * BPMP (BPMP wrote TRIGGER on the CCPLEX doorbell block). Reading the
 * CCPLEX PENDING register's HSP_DB_MASTER_BPMP bit reports this.
 */
bool hsp_bpmp_has_notified_ccplex(void);

/*
 * Acknowledge a pending BPMP notification (write the BPMP bit back into
 * PENDING on the CCPLEX doorbell block).
 */
void hsp_ack_bpmp_notification(void);

/*
 * Wait up to timeout_us for BPMP to authorise CCPLEX as a doorbell sender.
 * BPMP writes HSP_DB_MASTER_CCPLEX into the BPMP doorbell's ENABLE register
 * at its own boot time; the wait may block post-kexec while BPMP re-arms.
 * Returns 0 on success, -1 on timeout.
 */
int hsp_wait_bpmp_doorbell_enabled(uint32_t timeout_us);

#endif /* DRIVERS_BPMP_HSP_H */
