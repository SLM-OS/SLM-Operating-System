/*
 * kernel/drivers/bpmp/hsp.c - HSP doorbell layer for BPMP IPC (Tegra234)
 *
 * Implements the doorbell-based notification half of the BPMP IPC stack.
 * The IVC/MRQ layers sit on top of this.
 *
 * The HSP controller's doorbell blocks are not at a fixed offset — their
 * location depends on how many shared mailboxes, shared semaphores, and
 * arbitrated semaphores precede them. Those counts are encoded in the
 * HSP_DIMENSIONING register at HSP_BASE + 0x380. SLM-OS's previous BPMP
 * driver used a hardcoded offset (0x10000) that only happens to be right
 * when all three counts are zero; Tegra234 populates all three, so the
 * hardcode mis-targeted the doorbell by ~0x130000 bytes.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "hsp.h"
#include "debug.h"
#include "timer.h"      /* timer_busy_wait_us — truthful polling cadence */
#include <stdbool.h>
#include <stdint.h>

/* HSP common-region offsets. */
#define HSP_DIMENSIONING_REG    0x380

/* HSP layout constants — see docs/archive/plans/jetson-bpmp-ipc-plan.md "Doorbell-offset
 * calculation" section and ~/slmos-ref/linux/linux-tegra-hsp.c:289. */
#define HSP_COMMON_REGION_SIZE  0x10000   /* 64 KB common regs */
#define HSP_SM_SIZE             0x8000    /* 32 KB per shared mailbox */
#define HSP_SS_SIZE             0x10000   /* 64 KB per shared semaphore */
#define HSP_AS_SIZE             0x10000   /* 64 KB per arbitrated semaphore */

/* HSP_DIMENSIONING bit layout. */
#define HSP_DIM_NUM_SM_SHIFT    0
#define HSP_DIM_NUM_SM_MASK     0xF
#define HSP_DIM_NUM_SS_SHIFT    4
#define HSP_DIM_NUM_SS_MASK     0xF
#define HSP_DIM_NUM_AS_SHIFT    8
#define HSP_DIM_NUM_AS_MASK     0xF

static bool       g_hsp_initialised;
static uintptr_t  g_hsp_base;
static uint32_t   g_hsp_dim_raw;
static uintptr_t  g_bpmp_doorbell_addr;
static uintptr_t  g_ccplex_doorbell_addr;

/*
 * MMIO accessors. The Tegra convention (see kernel CLAUDE.md "UART LSR
 * Read After Kexec") is to issue `dsb sy` *before* every read so a
 * speculatively-reordered earlier load can't return stale data —
 * uart_tegra.c does this for LSR/RBR. Polling registers like
 * HSP_DB_REG_PENDING / HSP_DB_REG_ENABLE need the same treatment to be
 * reliable post-kexec on Jetson.
 *
 * Writes get a trailing `dsb sy` so subsequent code observes the side
 * effect (e.g. doorbell ring) before continuing.
 */
static inline uint32_t hsp_read32(uintptr_t addr)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint32_t *)addr;
}

static inline void hsp_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Compute the base address of the doorbell region inside HSP.
 * Matches edk2-nvidia's HspDoorbellInit and Linux's
 * tegra_hsp_doorbell_setup.
 */
static uintptr_t hsp_doorbell_region_base(uintptr_t hsp_base, uint32_t dim)
{
    uint32_t num_sm = (dim >> HSP_DIM_NUM_SM_SHIFT) & HSP_DIM_NUM_SM_MASK;
    uint32_t num_ss = (dim >> HSP_DIM_NUM_SS_SHIFT) & HSP_DIM_NUM_SS_MASK;
    uint32_t num_as = (dim >> HSP_DIM_NUM_AS_SHIFT) & HSP_DIM_NUM_AS_MASK;

    uintptr_t off = HSP_COMMON_REGION_SIZE;
    off += (uintptr_t)num_sm * HSP_SM_SIZE;
    off += (uintptr_t)num_ss * HSP_SS_SIZE;
    off += (uintptr_t)num_as * HSP_AS_SIZE;

    return hsp_base + off;
}

int hsp_init(uintptr_t hsp_base)
{
    if (hsp_base == 0) {
        return -1;
    }

    /* Read the dimensioning register once and cache it; the HSP layout
     * is fixed at tapeout and never changes post-boot. */
    uint32_t dim = hsp_read32(hsp_base + HSP_DIMENSIONING_REG);
    if (dim == 0xFFFFFFFF) {
        /* MMIO is not mapped, or the HSP block is powered down. */
        return -2;
    }

    uintptr_t db_base = hsp_doorbell_region_base(hsp_base, dim);

    g_hsp_base              = hsp_base;
    g_hsp_dim_raw           = dim;
    g_ccplex_doorbell_addr  = db_base + (HSP_DB_IDX_CCPLEX * HSP_DB_BLOCK_STRIDE);
    g_bpmp_doorbell_addr    = db_base + (HSP_DB_IDX_BPMP   * HSP_DB_BLOCK_STRIDE);
    g_hsp_initialised       = true;

    INFO("HSP@0x%lx: DIMENSIONING=0x%08x (SM=%u SS=%u AS=%u)",
         (unsigned long)hsp_base,
         (unsigned)dim,
         (unsigned)((dim >> HSP_DIM_NUM_SM_SHIFT) & HSP_DIM_NUM_SM_MASK),
         (unsigned)((dim >> HSP_DIM_NUM_SS_SHIFT) & HSP_DIM_NUM_SS_MASK),
         (unsigned)((dim >> HSP_DIM_NUM_AS_SHIFT) & HSP_DIM_NUM_AS_MASK));
    INFO("HSP doorbells: CCPLEX@0x%lx BPMP@0x%lx",
         (unsigned long)g_ccplex_doorbell_addr,
         (unsigned long)g_bpmp_doorbell_addr);

    return 0;
}

uint32_t hsp_dimensioning_raw(void)      { return g_hsp_dim_raw; }
uintptr_t hsp_bpmp_doorbell_addr(void)   { return g_bpmp_doorbell_addr; }
uintptr_t hsp_ccplex_doorbell_addr(void) { return g_ccplex_doorbell_addr; }

int hsp_ring_bpmp(void)
{
    if (!g_hsp_initialised) {
        return -1;
    }

    /* Confirm BPMP has authorised CCPLEX as a sender. When BPMP's firmware
     * is idle or being reset, the enable bit is clear and a TRIGGER write
     * silently drops. edk2-nvidia returns EFI_NOT_READY in this case. */
    uint32_t enable = hsp_read32(g_bpmp_doorbell_addr + HSP_DB_REG_ENABLE);
    if ((enable & (1u << HSP_DB_MASTER_CCPLEX)) == 0) {
        return -2;
    }

    hsp_write32(g_bpmp_doorbell_addr + HSP_DB_REG_TRIGGER, 1);
    return 0;
}

bool hsp_bpmp_has_notified_ccplex(void)
{
    if (!g_hsp_initialised) {
        return false;
    }

    uint32_t pending = hsp_read32(g_ccplex_doorbell_addr + HSP_DB_REG_PENDING);
    return (pending & (1u << HSP_DB_MASTER_BPMP)) != 0;
}

void hsp_ack_bpmp_notification(void)
{
    if (!g_hsp_initialised) {
        return;
    }

    /* Write-1-to-clear on PENDING — only the BPMP bit, don't clobber
     * pending notifications from other masters. */
    hsp_write32(g_ccplex_doorbell_addr + HSP_DB_REG_PENDING,
                1u << HSP_DB_MASTER_BPMP);
}

int hsp_wait_bpmp_doorbell_enabled(uint32_t timeout_us)
{
    if (!g_hsp_initialised) {
        return -1;
    }

    /* Poll in 1 us steps using the ARM generic timer (CNTPCT_EL0).
     * timer_busy_wait_us is callable from any context and gives us a
     * truthful timeout — the previous "spin 100 iterations for ~1 us"
     * approximation depended on compiler optimization and cpu freq. */
    uint32_t remaining = timeout_us;
    while (remaining > 0) {
        uint32_t enable = hsp_read32(g_bpmp_doorbell_addr + HSP_DB_REG_ENABLE);
        if ((enable & (1u << HSP_DB_MASTER_CCPLEX)) != 0) {
            return 0;
        }

        timer_busy_wait_us(1);
        remaining--;
    }

    return -1;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
