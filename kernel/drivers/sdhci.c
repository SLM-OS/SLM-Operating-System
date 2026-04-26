/*
 * sdhci.c — Generic SDHCI v3 / SD Host Controller driver.
 *
 * Stage 3 of the dynamic-kernel-replace plan (#369). Targets the
 * SDHCI v3 register layout and SD Physical Layer 3.0 / Card
 * Specification 6.0 protocols.
 *
 * Scope (intentional):
 *   - Legacy 25 MHz SDR only. No HS200/HS400/UHS/CQE/TRIM/tuning.
 *   - PIO data transfer (no ADMA2). FAT32 cluster size for admin
 *     writes is small; DMA setup overhead isn't worth the win.
 *   - Polled status — no IRQ delivery, since hardware IRQs on Pi 5
 *     are still cooperative-only (see `docs/pi5-baremetal-status.md`).
 *   - SDHC / SDXC primary target (real Pi 5 lab card is SDHC); SDSC
 *     also supported via byte-offset addressing (CCS bit from
 *     ACMD41 selects the addressing mode at probe time).
 *
 * Concurrency model:
 *   - Single `spinlock_t` per controller, held across the full
 *     command-issue + PIO-transfer window. Multi-block writes can
 *     hold the lock for several ms on real hardware. This is fine
 *     for the admin-speed kernel-staging workload the plan targets;
 *     a high-throughput caller would need a sleep-capable mutex or
 *     an IRQ-driven path.
 *   - `spin_lock_irqsave` keeps IRQs disabled while the lock is
 *     held — safe with polled status, no `yield()` reentrancy.
 *
 * References:
 *   - SD Host Controller Simplified Specification 3.0 (Feb 2014)
 *   - SD Physical Layer Simplified Specification 6.0 (Apr 2017)
 *   - linux-sdhci-brcmstb-digest.md (Risk 1 trace)
 *
 * Hardware-side BCM2712 quirks deferred to Stage 5 (#371): cfginit,
 * CPRMAN clock-gate, controller timing.
 */

#include "platform.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "sdhci.h"
#include "blkdev.h"
#include "pmm.h"
#include "spinlock.h"
#include "timer.h"
#include "debug.h"

#if !defined(PLATFORM_X86_64)
/* PCIe enumeration (used by `sdhci_create_qemu_pci`) — currently
 * available on QEMU virt + RASPI5 + JETSON ARM64 builds; the
 * x86-64 PCI path lives in pci.h, which we don't reach for. */
#include "pcie.h"
#endif

/* ---- SDHCI register offsets (per spec). ----
 *
 * All accesses are little-endian MMIO. Widths (8/16/32) match the
 * spec table; a wrong-width access can either silently succeed or
 * trigger a controller error depending on the chip, so respect the
 * width even when bit 31 is unused. */
#define SDHCI_DMA_ADDR              0x00    /* 32 */
#define SDHCI_BLOCK_SIZE            0x04    /* 16 */
#define SDHCI_BLOCK_COUNT           0x06    /* 16 */
#define SDHCI_ARGUMENT              0x08    /* 32 */
#define SDHCI_TRANSFER_MODE         0x0C    /* 16 */
#define SDHCI_COMMAND               0x0E    /* 16 */
#define SDHCI_RESPONSE0             0x10    /* 32 */
#define SDHCI_RESPONSE1             0x14    /* 32 */
#define SDHCI_RESPONSE2             0x18    /* 32 */
#define SDHCI_RESPONSE3             0x1C    /* 32 */
#define SDHCI_BUFFER                0x20    /* 32 — PIO data port */
#define SDHCI_PRESENT_STATE         0x24    /* 32 */
#define SDHCI_HOST_CONTROL          0x28    /*  8 */
#define SDHCI_POWER_CONTROL         0x29    /*  8 */
#define SDHCI_BLOCK_GAP             0x2A    /*  8 */
#define SDHCI_WAKEUP                0x2B    /*  8 */
#define SDHCI_CLOCK_CONTROL         0x2C    /* 16 */
#define SDHCI_TIMEOUT_CONTROL       0x2E    /*  8 */
#define SDHCI_SOFTWARE_RESET        0x2F    /*  8 */
/* INT_STATUS / INT_ENABLE / SIGNAL_ENABLE are spec'd as paired
 * 16-bit registers (Normal at +0x00, Error at +0x02), but every
 * SDHCI v3 controller (BCM2712, QEMU's sdhci-pci, all the brcmstb
 * SoCs) supports 32-bit access to the combined dword. Linux's
 * sdhci.c reads them as 32-bit; we follow the same canonical
 * pattern. Bits 0..15 are the Normal half, bits 16..31 are the
 * Error half — see SDHCI_INT_TIMEOUT etc. below for the layout. */
#define SDHCI_INT_STATUS            0x30    /* 32 — combined N+E */
#define SDHCI_INT_ENABLE            0x34    /* 32 */
#define SDHCI_SIGNAL_ENABLE         0x38    /* 32 */
#define SDHCI_CAPABILITIES          0x40    /* 32 */
#define SDHCI_HOST_VERSION          0xFE    /* 16 */

/* SOFTWARE_RESET (8) bits */
#define SDHCI_RESET_ALL             0x01
#define SDHCI_RESET_CMD             0x02
#define SDHCI_RESET_DAT             0x04

/* CLOCK_CONTROL (16) bits */
#define SDHCI_CLOCK_INT_EN          0x0001
#define SDHCI_CLOCK_INT_STABLE      0x0002
#define SDHCI_CLOCK_CARD_EN         0x0004
/* SDCLK frequency select: bits[15:8] (low 8) + bits[7:6] (extended hi 2)
 * for 10-bit divider in SDHCI v3.0. */
#define SDHCI_CLOCK_DIV_SHIFT       8
#define SDHCI_CLOCK_DIV_HI_SHIFT    6

/* POWER_CONTROL (8) bits */
#define SDHCI_POWER_ON              0x01
#define SDHCI_POWER_180             0x0A    /* 1.8V */
#define SDHCI_POWER_300             0x0C    /* 3.0V */
#define SDHCI_POWER_330             0x0E    /* 3.3V */

/* HOST_CONTROL (8) bits */
#define SDHCI_HOST_4BIT             0x02
#define SDHCI_HOST_HISPD            0x04

/* PRESENT_STATE (32) bits */
#define SDHCI_STATE_CMD_INHIBIT     (1u << 0)
#define SDHCI_STATE_DAT_INHIBIT     (1u << 1)
#define SDHCI_STATE_DAT_ACTIVE      (1u << 2)
#define SDHCI_STATE_WRITE_TRANSFER  (1u << 8)
#define SDHCI_STATE_READ_TRANSFER   (1u << 9)
#define SDHCI_STATE_BUF_WRITE_RDY   (1u << 10)
#define SDHCI_STATE_BUF_READ_RDY    (1u << 11)
#define SDHCI_STATE_CARD_INSERTED   (1u << 16)

/* INT_STATUS (32) — Normal int status low half, Error int status high half */
#define SDHCI_INT_CMD_COMPLETE      (1u << 0)
#define SDHCI_INT_TRANSFER_COMPLETE (1u << 1)
#define SDHCI_INT_BLOCK_GAP         (1u << 2)
#define SDHCI_INT_DMA               (1u << 3)
#define SDHCI_INT_BUF_WRITE_RDY     (1u << 4)
#define SDHCI_INT_BUF_READ_RDY      (1u << 5)
#define SDHCI_INT_CARD_INSERT       (1u << 6)
#define SDHCI_INT_CARD_REMOVE       (1u << 7)
#define SDHCI_INT_ERROR             (1u << 15)
#define SDHCI_INT_TIMEOUT           (1u << 16)  /* Cmd timeout */
#define SDHCI_INT_CRC               (1u << 17)
#define SDHCI_INT_END_BIT           (1u << 18)
#define SDHCI_INT_INDEX             (1u << 19)
#define SDHCI_INT_DATA_TIMEOUT      (1u << 20)
#define SDHCI_INT_DATA_CRC          (1u << 21)
#define SDHCI_INT_DATA_END_BIT      (1u << 22)
#define SDHCI_INT_BUS_POWER         (1u << 23)
#define SDHCI_INT_AUTO_CMD12        (1u << 24)

#define SDHCI_INT_ERROR_MASK        0x017F0000u
#define SDHCI_INT_NORMAL_MASK       0x0000FFFFu

/* TRANSFER_MODE (16) bits */
#define SDHCI_TRNS_DMA              0x0001
#define SDHCI_TRNS_BLK_CNT_EN       0x0002
#define SDHCI_TRNS_AUTO_CMD12       0x0004
#define SDHCI_TRNS_READ             0x0010
#define SDHCI_TRNS_MULTI            0x0020

/* COMMAND (16) bits */
#define SDHCI_CMD_RESP_NONE         0x00
#define SDHCI_CMD_RESP_136          0x01    /* R2 */
#define SDHCI_CMD_RESP_48           0x02    /* R1, R3, R4, R5, R6 */
#define SDHCI_CMD_RESP_48_BUSY      0x03    /* R1b, R5b */
#define SDHCI_CMD_CRC_CHK           0x08
#define SDHCI_CMD_INDEX_CHK         0x10
#define SDHCI_CMD_DATA              0x20
#define SDHCI_CMD_INDEX_SHIFT       8

/* CAPABILITIES (32) bits */
#define SDHCI_CAP_BASE_CLK_SHIFT    8       /* MHz */
#define SDHCI_CAP_BASE_CLK_MASK     0xFF
#define SDHCI_CAP_VOLT_330          (1u << 24)

/* SD card commands (per Physical Layer Spec). */
#define MMC_GO_IDLE_STATE           0       /* CMD0,  no resp */
#define MMC_ALL_SEND_CID            2       /* CMD2,  R2 */
#define MMC_SEND_RELATIVE_ADDR      3       /* CMD3,  R6 */
#define MMC_SELECT_CARD             7       /* CMD7,  R1b */
#define SD_SEND_IF_COND             8       /* CMD8,  R7 */
#define MMC_SEND_CSD                9       /* CMD9,  R2 */
#define MMC_SET_BLOCKLEN            16      /* CMD16, R1 */
#define MMC_READ_SINGLE_BLOCK       17      /* CMD17, R1 (data) */
#define MMC_READ_MULTIPLE_BLOCK     18      /* CMD18, R1 (data) */
#define MMC_WRITE_BLOCK             24      /* CMD24, R1 (data) */
#define MMC_WRITE_MULTIPLE_BLOCK    25      /* CMD25, R1 (data) */
#define MMC_APP_CMD                 55      /* CMD55, R1 (prefix) */
#define SD_APP_OP_COND              41      /* ACMD41, R3 */

/* CMD8 argument: VHS=1 (2.7..3.6V), check pattern 0xAA. */
#define SD_IF_COND_VHS_27_36        0x100
#define SD_IF_COND_PATTERN          0xAA

/* ACMD41 argument: HCS=1 (host supports SDHC), 3.3V window. */
#define SD_OCR_HCS                  (1u << 30)
#define SD_OCR_VOLT_330_340         (1u << 21)
#define SD_OCR_BUSY                 (1u << 31)  /* in response */

/* SDHCI sector size — fixed at 512 by FatFs (FF_MIN_SS == FF_MAX_SS).
 * Production cards are 512-byte addressable (SDHC). */
#define SD_BLOCK_SIZE               512u

/* Generous wall-clock budget for any single SDHCI op. Real round
 * trips are sub-millisecond; this catches genuinely wedged hardware
 * without making boot slow. */
#define SDHCI_OP_TIMEOUT_US         (500u * 1000u)
#define SDHCI_INIT_TIMEOUT_US       (1000u * 1000u)   /* ACMD41 polling */

/* ---- Driver state. ---- */

struct sdhci_priv {
    uintptr_t mmio_base;
    uint32_t  rca;            /* Card relative address from CMD3 */
    uint32_t  capacity_blocks; /* From CMD9 CSD */
    bool      is_high_capacity; /* SDHC/SDXC: arg is block index.
                                 * SDSC:      arg is byte offset.
                                 * Set from ACMD41's CCS response bit. */
    spinlock_t lock;
};

/* Allocation footprint: descriptor + private state in one page. */
struct sdhci_alloc {
    struct blkdev   dev;
    struct sdhci_priv priv;
};

/* ---- MMIO helpers. ---- */

static inline uint8_t sdhci_readb(struct sdhci_priv *p, uint32_t off)
{
    return *(volatile uint8_t *)(p->mmio_base + off);
}

static inline uint16_t sdhci_readw(struct sdhci_priv *p, uint32_t off)
{
    return *(volatile uint16_t *)(p->mmio_base + off);
}

static inline uint32_t sdhci_readl(struct sdhci_priv *p, uint32_t off)
{
    return *(volatile uint32_t *)(p->mmio_base + off);
}

static inline void sdhci_writeb(struct sdhci_priv *p, uint32_t off, uint8_t v)
{
    *(volatile uint8_t *)(p->mmio_base + off) = v;
}

static inline void sdhci_writew(struct sdhci_priv *p, uint32_t off, uint16_t v)
{
    *(volatile uint16_t *)(p->mmio_base + off) = v;
}

static inline void sdhci_writel(struct sdhci_priv *p, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(p->mmio_base + off) = v;
}

/* ---- Polling helpers. ---- */

/*
 * Spin until `(read(reg) & mask) == desired` or the deadline elapses.
 * Width-aware so we don't accidentally sign-extend or read garbage
 * outside the spec'd register width.
 */
static int sdhci_wait_l(struct sdhci_priv *p, uint32_t reg,
                        uint32_t mask, uint32_t desired,
                        uint32_t budget_us)
{
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              ((uint64_t)budget_us * freq + 999999ULL) / 1000000ULL;
    while (timer_get_count() < deadline) {
        if ((sdhci_readl(p, reg) & mask) == desired) {
            return 0;
        }
        timer_busy_wait_us(10);
    }
    return -1;
}

static int sdhci_wait_b(struct sdhci_priv *p, uint32_t reg,
                        uint8_t mask, uint8_t desired,
                        uint32_t budget_us)
{
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              ((uint64_t)budget_us * freq + 999999ULL) / 1000000ULL;
    while (timer_get_count() < deadline) {
        if ((sdhci_readb(p, reg) & mask) == desired) {
            return 0;
        }
        timer_busy_wait_us(10);
    }
    return -1;
}

/* ---- Init helpers. ---- */

/*
 * Pre-command lines-free check. SDHCI requires CMD_INHIBIT clear
 * before issuing any new command; DAT_INHIBIT must also be clear if
 * the next command uses the data lines (R1b or data transfer).
 */
static int sdhci_wait_lines_free(struct sdhci_priv *p, bool need_dat)
{
    uint32_t mask = SDHCI_STATE_CMD_INHIBIT;
    if (need_dat) {
        mask |= SDHCI_STATE_DAT_INHIBIT;
    }
    return sdhci_wait_l(p, SDHCI_PRESENT_STATE, mask, 0, SDHCI_OP_TIMEOUT_US);
}

/*
 * Enable internal clock at the requested SD-bus frequency. Returns 0
 * on success. SDHCI v3 supports a 10-bit divider (1, 2, 4, 8, ...,
 * up to 1024). The divider divides the controller's base clock to
 * produce SD-bus SCK.
 *
 * The base clock comes from CAPABILITIES[15:8] (in MHz). For QEMU
 * sdhci-pci this defaults to 50 MHz; for BCM2712 EMMC2 it's 200 MHz
 * out of CPRMAN (deferred to Stage 5 to actually program; the
 * default firmware-left value is acceptable for QEMU + the first
 * Pi 5 hardware bring-up).
 */
static int sdhci_set_clock(struct sdhci_priv *p, uint32_t target_hz)
{
    /* Stop card clock first. */
    sdhci_writew(p, SDHCI_CLOCK_CONTROL, 0);

    uint32_t cap = sdhci_readl(p, SDHCI_CAPABILITIES);
    uint32_t base_mhz = (cap >> SDHCI_CAP_BASE_CLK_SHIFT)
                      & SDHCI_CAP_BASE_CLK_MASK;
    if (base_mhz == 0) {
        ERROR("sdhci: capabilities reports base_clk=0; controller dead?");
        return -1;
    }
    uint32_t base_hz = base_mhz * 1000000u;

    /* Find smallest divider div s.t. base/div <= target. SDHCI v3:
     * div = 0 means base clock unchanged; div = 1 means divide by 2;
     * div = N means divide by 2*N. Round up so we never exceed
     * target. */
    uint32_t div = 0;
    if (target_hz < base_hz) {
        for (div = 1; div < 0x3FF; div++) {
            if (base_hz / (2u * div) <= target_hz) {
                break;
            }
        }
    }

    /* Pack 10-bit divider: low 8 in bits[15:8], high 2 in bits[7:6]. */
    uint16_t clk = SDHCI_CLOCK_INT_EN;
    clk |= (uint16_t)((div & 0xFFu) << SDHCI_CLOCK_DIV_SHIFT);
    clk |= (uint16_t)(((div >> 8) & 0x3u) << SDHCI_CLOCK_DIV_HI_SHIFT);
    sdhci_writew(p, SDHCI_CLOCK_CONTROL, clk);

    /* Wait for internal clock stable. */
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              ((uint64_t)SDHCI_OP_TIMEOUT_US * freq + 999999ULL) / 1000000ULL;
    while (timer_get_count() < deadline) {
        uint16_t cs = sdhci_readw(p, SDHCI_CLOCK_CONTROL);
        if (cs & SDHCI_CLOCK_INT_STABLE) {
            /* Enable card clock (gates SCK to the SD bus). */
            sdhci_writew(p, SDHCI_CLOCK_CONTROL, cs | SDHCI_CLOCK_CARD_EN);
            return 0;
        }
        timer_busy_wait_us(10);
    }
    ERROR("sdhci: internal clock never stabilized");
    return -1;
}

/* ---- Command issue + response read. ---- */

struct sdhci_cmd {
    uint8_t  index;          /* CMD index (0..63) */
    uint8_t  resp_type;      /* SDHCI_CMD_RESP_* */
    uint32_t argument;
    bool     data;           /* true = command transfers data */
    bool     read;           /* if data: true=read, false=write */
    uint16_t block_count;    /* if data */
    uint32_t resp[4];        /* OUT: for R2 the full 128 bits */
};

/*
 * Issue a single SDHCI command. Caller fills `cmd` (in fields) and
 * `cmd.resp[]` is populated on return. Returns 0 on success, -1 on
 * timeout / CRC / index-mismatch / any error int.
 *
 * For data commands the caller must follow up with sdhci_pio_read /
 * sdhci_pio_write to drain the buffer port.
 */
static int sdhci_send_cmd(struct sdhci_priv *p, struct sdhci_cmd *cmd)
{
    bool need_dat = cmd->data || (cmd->resp_type == SDHCI_CMD_RESP_48_BUSY);
    if (sdhci_wait_lines_free(p, need_dat) < 0) {
        ERROR("sdhci: cmd%u — lines busy at issue", cmd->index);
        return -1;
    }

    /* Clear all interrupt status bits we're about to wait on. */
    sdhci_writel(p, SDHCI_INT_STATUS, 0xFFFFFFFFu);

    /* Block size + count for data commands. */
    if (cmd->data) {
        sdhci_writew(p, SDHCI_BLOCK_SIZE, SD_BLOCK_SIZE);
        sdhci_writew(p, SDHCI_BLOCK_COUNT, cmd->block_count);

        uint16_t mode = SDHCI_TRNS_BLK_CNT_EN;
        if (cmd->read) mode |= SDHCI_TRNS_READ;
        if (cmd->block_count > 1) mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
        sdhci_writew(p, SDHCI_TRANSFER_MODE, mode);
    }

    sdhci_writel(p, SDHCI_ARGUMENT, cmd->argument);

    uint16_t cmd_reg = (uint16_t)(cmd->index << SDHCI_CMD_INDEX_SHIFT)
                     | cmd->resp_type;
    /* CRC + index check on R1/R5/R6/R7. R2 gets CRC only (no index
     * because R2 is the CID/CSD payload). R3/R4 skip both. R1b is
     * R1+busy. */
    if (cmd->resp_type == SDHCI_CMD_RESP_136) {
        cmd_reg |= SDHCI_CMD_CRC_CHK;
    } else if (cmd->resp_type == SDHCI_CMD_RESP_48
            || cmd->resp_type == SDHCI_CMD_RESP_48_BUSY) {
        /* Skip CRC/index for ACMD41 (R3 — OCR has no CRC) and CMD8
         * (R7 also passes through unchanged). Index-check is
         * load-bearing for R1/R5/R6 — use it where it applies. */
        if (cmd->index != SD_APP_OP_COND
         && cmd->index != SD_SEND_IF_COND) {
            cmd_reg |= SDHCI_CMD_CRC_CHK | SDHCI_CMD_INDEX_CHK;
        }
    }
    if (cmd->data) cmd_reg |= SDHCI_CMD_DATA;
    sdhci_writew(p, SDHCI_COMMAND, cmd_reg);

    /* Wait for command-complete or any error. */
    const uint64_t freq     = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              ((uint64_t)SDHCI_OP_TIMEOUT_US * freq + 999999ULL) / 1000000ULL;
    uint32_t status = 0;
    while (timer_get_count() < deadline) {
        status = sdhci_readl(p, SDHCI_INT_STATUS);
        if (status & (SDHCI_INT_CMD_COMPLETE | SDHCI_INT_ERROR)) {
            break;
        }
        timer_busy_wait_us(2);
    }

    if (status & SDHCI_INT_ERROR) {
        ERROR("sdhci: cmd%u error int_status=0x%08x", cmd->index, status);
        sdhci_writel(p, SDHCI_INT_STATUS, status);
        /* Reset CMD line so the next command isn't blocked. */
        sdhci_writeb(p, SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
        sdhci_wait_b(p, SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD, 0,
                     SDHCI_OP_TIMEOUT_US);
        return -1;
    }
    if (!(status & SDHCI_INT_CMD_COMPLETE)) {
        ERROR("sdhci: cmd%u no completion (status=0x%08x)", cmd->index, status);
        return -1;
    }
    /* Ack just the bits we waited on; data-completion bit (if any)
     * is left pending for the PIO loop to observe. */
    sdhci_writel(p, SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    /* Read response. Layout differs for R2 (long, 128b in RESP[3..0]
     * with bit shift) vs R1/R3/R6/R7 (48b in RESP0). */
    if (cmd->resp_type == SDHCI_CMD_RESP_136) {
        cmd->resp[0] = sdhci_readl(p, SDHCI_RESPONSE0);
        cmd->resp[1] = sdhci_readl(p, SDHCI_RESPONSE1);
        cmd->resp[2] = sdhci_readl(p, SDHCI_RESPONSE2);
        cmd->resp[3] = sdhci_readl(p, SDHCI_RESPONSE3);
    } else if (cmd->resp_type != SDHCI_CMD_RESP_NONE) {
        cmd->resp[0] = sdhci_readl(p, SDHCI_RESPONSE0);
    }
    return 0;
}

/* ---- PIO data transfer. ---- */

static int sdhci_pio_read(struct sdhci_priv *p, void *buf, uint16_t blocks)
{
    uint32_t *out = buf;
    for (uint16_t b = 0; b < blocks; b++) {
        if (sdhci_wait_l(p, SDHCI_INT_STATUS, SDHCI_INT_BUF_READ_RDY,
                         SDHCI_INT_BUF_READ_RDY, SDHCI_OP_TIMEOUT_US) < 0) {
            ERROR("sdhci: pio_read block %u — buf-rdy timeout", b);
            return -1;
        }
        sdhci_writel(p, SDHCI_INT_STATUS, SDHCI_INT_BUF_READ_RDY);
        for (uint32_t i = 0; i < SD_BLOCK_SIZE / 4; i++) {
            *out++ = sdhci_readl(p, SDHCI_BUFFER);
        }
    }
    /* Wait for transfer-complete + ack. */
    if (sdhci_wait_l(p, SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE,
                     SDHCI_INT_TRANSFER_COMPLETE, SDHCI_OP_TIMEOUT_US) < 0) {
        ERROR("sdhci: pio_read — xfer-complete timeout");
        return -1;
    }
    sdhci_writel(p, SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
    return 0;
}

static int sdhci_pio_write(struct sdhci_priv *p, const void *buf, uint16_t blocks)
{
    const uint32_t *in = buf;
    for (uint16_t b = 0; b < blocks; b++) {
        if (sdhci_wait_l(p, SDHCI_INT_STATUS, SDHCI_INT_BUF_WRITE_RDY,
                         SDHCI_INT_BUF_WRITE_RDY, SDHCI_OP_TIMEOUT_US) < 0) {
            ERROR("sdhci: pio_write block %u — buf-rdy timeout", b);
            return -1;
        }
        sdhci_writel(p, SDHCI_INT_STATUS, SDHCI_INT_BUF_WRITE_RDY);
        for (uint32_t i = 0; i < SD_BLOCK_SIZE / 4; i++) {
            sdhci_writel(p, SDHCI_BUFFER, *in++);
        }
    }
    if (sdhci_wait_l(p, SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE,
                     SDHCI_INT_TRANSFER_COMPLETE, SDHCI_OP_TIMEOUT_US) < 0) {
        ERROR("sdhci: pio_write — xfer-complete timeout");
        return -1;
    }
    sdhci_writel(p, SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
    return 0;
}

/* ---- Card init sequence. ---- */

/*
 * Parse the CSD response (R2, 128 bits) for capacity in 512-byte
 * blocks. Handles both CSD v1.0 (SDSC, used by QEMU's sd-card model
 * for any size) and CSD v2.0 (SDHC/SDXC, used by real cards ≥ 2 GB).
 *
 * SDHCI register layout per SD Host Controller Simplified Spec 3.0
 * §2.2.6 — the host strips the framing/CRC bytes and presents
 * CSD[127:0] across RESPONSE0..3:
 *   resp[3] (RESPONSE3) = CSD[127:96]
 *   resp[2] (RESPONSE2) = CSD[ 95:64]
 *   resp[1] (RESPONSE1) = CSD[ 63:32]
 *   resp[0] (RESPONSE0) = CSD[ 31: 0]
 *
 * Capacity formulas:
 *   v1.0 (csd_struct=0):
 *     C_SIZE       = CSD[73:62]    (12 bits)
 *     C_SIZE_MULT  = CSD[49:47]    ( 3 bits)
 *     READ_BL_LEN  = CSD[83:80]    ( 4 bits, log2 of card-block size)
 *     blocks       = (C_SIZE+1) << (C_SIZE_MULT+2)  [in card-blocks]
 *     bytes        = blocks << READ_BL_LEN
 *     ⇒ 512-byte blocks = bytes >> 9
 *
 *   v2.0 (csd_struct=1):
 *     C_SIZE       = CSD[69:48]    (22 bits)
 *     bytes        = (C_SIZE+1) << 19  (= * 512 KB)
 *     ⇒ 512-byte blocks = (C_SIZE+1) << 10  (* 1024)
 */
static uint32_t sdhci_parse_csd_blocks(const uint32_t resp[4])
{
    /* csd_struct = CSD[127:126] = resp[3] bits [31:30]. */
    uint8_t csd_struct = (uint8_t)((resp[3] >> 30) & 0x3u);

    if (csd_struct == 1) {
        /* SDHC/SDXC. C_SIZE = CSD[69:48], straddling resp[2] / resp[1]:
         *   resp[2] bits [5:0]   = CSD[69:64] = C_SIZE[21:16]
         *   resp[1] bits [31:16] = CSD[63:48] = C_SIZE[15:0]
         */
        uint32_t c_size = ((resp[2] & 0x3Fu) << 16)
                        | ((resp[1] >> 16) & 0xFFFFu);
        return (c_size + 1u) * 1024u;
    }

    if (csd_struct == 0) {
        /* SDSC. Smaller cards (and QEMU's sd-card model regardless
         * of size). Field bit positions:
         *   READ_BL_LEN = CSD[83:80] = (resp[2] >> 16) & 0xF
         *   C_SIZE      = CSD[73:62] (12 bits, straddles resp[2]/resp[1]):
         *     resp[2] bits [ 9:0]  = CSD[73:64] = C_SIZE[11:2]
         *     resp[1] bits [31:30] = CSD[63:62] = C_SIZE[ 1:0]
         *   C_SIZE_MULT = CSD[49:47] (3 bits, in resp[1] bits [17:15])
         */
        uint32_t read_bl_len = (resp[2] >> 16) & 0xFu;
        uint32_t c_size      = ((resp[2] & 0x3FFu) << 2)
                             | ((resp[1] >> 30) & 0x3u);
        uint32_t c_size_mult = (resp[1] >> 15) & 0x7u;

        /* Sanity-check: READ_BL_LEN of 9..11 covers all standard
         * SDSC card-block sizes (512 / 1024 / 2048 bytes). */
        if (read_bl_len < 9 || read_bl_len > 11) {
            WARN("sdhci: CSD v1 READ_BL_LEN=%u out of [9..11]; defaulting to 64 MB",
                 read_bl_len);
            return (64u * 1024u * 1024u) / SD_BLOCK_SIZE;
        }
        /* total bytes = (C_SIZE+1) << (C_SIZE_MULT+2+READ_BL_LEN);
         * 512-byte blocks = total bytes >> 9. So:
         *   512-blocks = (C_SIZE+1) << (C_SIZE_MULT + 2 + READ_BL_LEN - 9)
         */
        uint32_t shift = c_size_mult + 2u + read_bl_len - 9u;
        return (c_size + 1u) << shift;
    }

    WARN("sdhci: unknown csd_struct=%u; defaulting to 64 MB", csd_struct);
    return (64u * 1024u * 1024u) / SD_BLOCK_SIZE;
}

/*
 * Bring the card from reset to TRAN state. Standard SD init
 * sequence (Physical Layer Spec 4.2):
 *   CMD0 (idle) → CMD8 (voltage check) → ACMD41 (volt nego, HCS)
 *   → CMD2 (CID) → CMD3 (RCA) → CMD9 (CSD) → CMD7 (select)
 *   → CMD16 (block-len, harmless on SDHC).
 */
static int sdhci_card_init(struct sdhci_priv *p)
{
    struct sdhci_cmd c;

    /* CMD0: GO_IDLE_STATE. */
    memset(&c, 0, sizeof(c));
    c.index = MMC_GO_IDLE_STATE;
    c.resp_type = SDHCI_CMD_RESP_NONE;
    if (sdhci_send_cmd(p, &c) < 0) return -1;

    /* CMD8: SEND_IF_COND with 0x1AA. R7 echoes the pattern. */
    memset(&c, 0, sizeof(c));
    c.index = SD_SEND_IF_COND;
    c.resp_type = SDHCI_CMD_RESP_48;
    c.argument = SD_IF_COND_VHS_27_36 | SD_IF_COND_PATTERN;
    if (sdhci_send_cmd(p, &c) < 0) {
        ERROR("sdhci: CMD8 failed — pre-2.0 card unsupported");
        return -1;
    }
    if ((c.resp[0] & 0xFFu) != SD_IF_COND_PATTERN) {
        ERROR("sdhci: CMD8 echo bad: 0x%08x", c.resp[0]);
        return -1;
    }

    /* ACMD41: poll until ready (busy bit set in response). */
    const uint64_t freq    = timer_get_frequency();
    const uint64_t deadline = timer_get_count() +
                              ((uint64_t)SDHCI_INIT_TIMEOUT_US * freq + 999999ULL) / 1000000ULL;
    while (timer_get_count() < deadline) {
        memset(&c, 0, sizeof(c));
        c.index = MMC_APP_CMD;
        c.resp_type = SDHCI_CMD_RESP_48;
        c.argument = 0;
        if (sdhci_send_cmd(p, &c) < 0) return -1;

        memset(&c, 0, sizeof(c));
        c.index = SD_APP_OP_COND;
        c.resp_type = SDHCI_CMD_RESP_48;
        c.argument = SD_OCR_HCS | SD_OCR_VOLT_330_340;
        if (sdhci_send_cmd(p, &c) < 0) return -1;
        if (c.resp[0] & SD_OCR_BUSY) {
            break;
        }
        timer_busy_wait_us(1000);
    }
    if (!(c.resp[0] & SD_OCR_BUSY)) {
        ERROR("sdhci: ACMD41 — card never reported ready");
        return -1;
    }

    /* CCS bit (Card Capacity Status) shares bit 30 with the request's
     * HCS (Host Capacity Support). Set in the response = SDHC/SDXC
     * (block-addressed). Cleared = SDSC (byte-addressed within the
     * block-length set by CMD16). The driver scales CMD17/18/24/25
     * arguments accordingly in sdhci_blkdev_read/prog. */
    p->is_high_capacity = (c.resp[0] & SD_OCR_HCS) != 0;
    INFO("sdhci: card type = %s",
         p->is_high_capacity ? "SDHC/SDXC (block-addressed)"
                             : "SDSC (byte-addressed)");

    /* CMD2: ALL_SEND_CID (R2). We don't parse CID — just ack. */
    memset(&c, 0, sizeof(c));
    c.index = MMC_ALL_SEND_CID;
    c.resp_type = SDHCI_CMD_RESP_136;
    if (sdhci_send_cmd(p, &c) < 0) return -1;

    /* CMD3: SEND_RELATIVE_ADDR (R6). RCA is in resp[0][31:16]. */
    memset(&c, 0, sizeof(c));
    c.index = MMC_SEND_RELATIVE_ADDR;
    c.resp_type = SDHCI_CMD_RESP_48;
    if (sdhci_send_cmd(p, &c) < 0) return -1;
    p->rca = c.resp[0] & 0xFFFF0000u;
    INFO("sdhci: card RCA=0x%08x", p->rca);

    /* CMD9: SEND_CSD (R2). Parse for capacity. */
    memset(&c, 0, sizeof(c));
    c.index = MMC_SEND_CSD;
    c.resp_type = SDHCI_CMD_RESP_136;
    c.argument = p->rca;
    if (sdhci_send_cmd(p, &c) < 0) return -1;
    p->capacity_blocks = sdhci_parse_csd_blocks(c.resp);
    INFO("sdhci: card capacity = %u blocks (%u MB)",
         p->capacity_blocks, p->capacity_blocks / 2048);

    /* CMD7: SELECT_CARD (R1b). */
    memset(&c, 0, sizeof(c));
    c.index = MMC_SELECT_CARD;
    c.resp_type = SDHCI_CMD_RESP_48_BUSY;
    c.argument = p->rca;
    if (sdhci_send_cmd(p, &c) < 0) return -1;

    /* CMD16: SET_BLOCKLEN. Harmless on SDHC (which is fixed at
     * 512), required for SDSC. Unconditional for safety. */
    memset(&c, 0, sizeof(c));
    c.index = MMC_SET_BLOCKLEN;
    c.resp_type = SDHCI_CMD_RESP_48;
    c.argument = SD_BLOCK_SIZE;
    if (sdhci_send_cmd(p, &c) < 0) return -1;

    /* Switch to 25 MHz transfer clock. */
    if (sdhci_set_clock(p, 25000000u) < 0) {
        ERROR("sdhci: failed to switch to 25 MHz");
        return -1;
    }
    return 0;
}

/* ---- blkdev_ops backends. ---- */

/*
 * Translate a 512-byte-block index into the CMD17/18/24/25 argument
 * for the connected card type:
 *   SDHC/SDXC (high_capacity): argument is the block index directly.
 *   SDSC: argument is the byte offset (block * SD_BLOCK_SIZE).
 * Returns false on overflow (SDSC byte address > 4 GiB — the 32-bit
 * argument wraps); `*arg_out` is undefined in that case. Caller
 * surfaces overflow as BLKDEV_ERR_INVAL.
 */
static bool sdhci_arg_for_block(const struct sdhci_priv *p, uint32_t block,
                                uint32_t *arg_out)
{
    if (p->is_high_capacity) {
        *arg_out = block;
        return true;
    }
    uint64_t byte_addr = (uint64_t)block * (uint64_t)SD_BLOCK_SIZE;
    if (byte_addr > UINT32_MAX) {
        return false;
    }
    *arg_out = (uint32_t)byte_addr;
    return true;
}

static int sdhci_blkdev_read(struct blkdev *dev, uint32_t block,
                             uint32_t off, void *buffer, uint32_t size)
{
    struct sdhci_priv *p = dev->priv;
    /* Stage 2 diskio shim only ever calls with off==0 and
     * size = N * block_size. Reject the partial-block case rather
     * than buffer-then-copy in this PR — covered in Stage 4 if a
     * caller needs it. */
    if (off != 0 || (size % SD_BLOCK_SIZE) != 0) {
        return BLKDEV_ERR_INVAL;
    }
    uint32_t blocks = size / SD_BLOCK_SIZE;
    if (blocks == 0) return BLKDEV_OK;
    if (blocks > 0xFFFFu) return BLKDEV_ERR_INVAL;
    if ((uint64_t)block + blocks > p->capacity_blocks) {
        return BLKDEV_ERR_INVAL;
    }
    uint32_t arg;
    if (!sdhci_arg_for_block(p, block, &arg)) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&p->lock);

    struct sdhci_cmd c;
    memset(&c, 0, sizeof(c));
    c.index = (blocks > 1) ? MMC_READ_MULTIPLE_BLOCK : MMC_READ_SINGLE_BLOCK;
    c.resp_type = SDHCI_CMD_RESP_48;
    c.argument = arg;
    c.data = true;
    c.read = true;
    c.block_count = (uint16_t)blocks;
    int rc = sdhci_send_cmd(p, &c);
    if (rc == 0) {
        rc = sdhci_pio_read(p, buffer, (uint16_t)blocks);
    }

    spin_unlock_irqrestore(&p->lock, flags);
    return (rc == 0) ? BLKDEV_OK : BLKDEV_ERR_IO;
}

static int sdhci_blkdev_prog(struct blkdev *dev, uint32_t block,
                             uint32_t off, const void *buffer, uint32_t size)
{
    struct sdhci_priv *p = dev->priv;
    if (off != 0 || (size % SD_BLOCK_SIZE) != 0) {
        return BLKDEV_ERR_INVAL;
    }
    uint32_t blocks = size / SD_BLOCK_SIZE;
    if (blocks == 0) return BLKDEV_OK;
    if (blocks > 0xFFFFu) return BLKDEV_ERR_INVAL;
    if ((uint64_t)block + blocks > p->capacity_blocks) {
        return BLKDEV_ERR_INVAL;
    }
    uint32_t arg;
    if (!sdhci_arg_for_block(p, block, &arg)) {
        return BLKDEV_ERR_INVAL;
    }

    irq_flags_t flags = spin_lock_irqsave(&p->lock);

    struct sdhci_cmd c;
    memset(&c, 0, sizeof(c));
    c.index = (blocks > 1) ? MMC_WRITE_MULTIPLE_BLOCK : MMC_WRITE_BLOCK;
    c.resp_type = SDHCI_CMD_RESP_48;
    c.argument = arg;
    c.data = true;
    c.read = false;
    c.block_count = (uint16_t)blocks;
    int rc = sdhci_send_cmd(p, &c);
    if (rc == 0) {
        rc = sdhci_pio_write(p, buffer, (uint16_t)blocks);
    }

    spin_unlock_irqrestore(&p->lock, flags);
    return (rc == 0) ? BLKDEV_OK : BLKDEV_ERR_IO;
}

static int sdhci_blkdev_erase(struct blkdev *dev, uint32_t block)
{
    /* No discrete erase — overwriting suffices on SD. Block alloc
     * comes from the FS layer; stub here so littlefs-style backends
     * that call erase don't choke. */
    (void)dev;
    (void)block;
    return BLKDEV_OK;
}

static int sdhci_blkdev_sync(struct blkdev *dev)
{
    /* No write cache to flush — every CMD24/25 lands on the card
     * before transfer-complete fires. */
    (void)dev;
    return BLKDEV_OK;
}

static const struct blkdev_ops sdhci_ops = {
    .read  = sdhci_blkdev_read,
    .prog  = sdhci_blkdev_prog,
    .erase = sdhci_blkdev_erase,
    .sync  = sdhci_blkdev_sync,
};

/* ---- Public init/teardown. ---- */

static void sdhci_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

struct blkdev *sdhci_create(const char *name, uintptr_t mmio_base)
{
    if (!name || !mmio_base) {
        ERROR("sdhci: invalid args (name=%p, mmio=0x%lx)",
              (void *)name, (unsigned long)mmio_base);
        return NULL;
    }

    /* One page covers the alloc; descriptor + priv fit comfortably. */
    size_t alloc_pages = (sizeof(struct sdhci_alloc) + PAGE_SIZE - 1) / PAGE_SIZE;
    struct sdhci_alloc *a = pmm_alloc_pages(alloc_pages);
    if (!a) {
        ERROR("sdhci: PMM exhausted");
        return NULL;
    }
    memset(a, 0, sizeof(*a));

    struct sdhci_priv *p = &a->priv;
    p->mmio_base = mmio_base;
    spin_init(&p->lock);

    INFO("sdhci: probing %s @ 0x%lx (HC version=0x%04x, caps=0x%08x)",
         name, (unsigned long)mmio_base,
         sdhci_readw(p, SDHCI_HOST_VERSION),
         sdhci_readl(p, SDHCI_CAPABILITIES));

    /* Software reset all. */
    sdhci_writeb(p, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL);
    if (sdhci_wait_b(p, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, 0,
                     SDHCI_OP_TIMEOUT_US) < 0) {
        ERROR("sdhci: soft-reset never cleared — wrong base or dead controller");
        pmm_free_pages(a, alloc_pages);
        return NULL;
    }

    /* Power on at 3.3V. Skip if controller doesn't report 3.3V
     * support in capabilities (rare; QEMU defaults to all rails). */
    uint32_t cap = sdhci_readl(p, SDHCI_CAPABILITIES);
    if (!(cap & SDHCI_CAP_VOLT_330)) {
        ERROR("sdhci: controller doesn't report 3.3V support (cap=0x%08x)", cap);
        pmm_free_pages(a, alloc_pages);
        return NULL;
    }
    sdhci_writeb(p, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);

    /* Default timeout register to maximum (0x0E). */
    sdhci_writeb(p, SDHCI_TIMEOUT_CONTROL, 0x0E);

    /* Enable error + normal int status latching (we still poll). */
    sdhci_writel(p, SDHCI_INT_ENABLE,
                 SDHCI_INT_NORMAL_MASK | SDHCI_INT_ERROR_MASK);
    sdhci_writel(p, SDHCI_SIGNAL_ENABLE, 0);

    /* ID-phase clock at 400 kHz (mandatory per Physical Layer Spec). */
    if (sdhci_set_clock(p, 400000u) < 0) {
        pmm_free_pages(a, alloc_pages);
        return NULL;
    }

    /* Wait 74 SD-bus clocks (~200 µs at 400 kHz) before CMD0 — spec
     * requirement for the card's internal init. */
    timer_busy_wait_us(1000);

    if (sdhci_card_init(p) < 0) {
        pmm_free_pages(a, alloc_pages);
        return NULL;
    }

    /* Populate the blkdev descriptor. */
    struct blkdev *dev = &a->dev;
    sdhci_strcpy(dev->name, name, BLKDEV_MAX_NAME);
    dev->read_size   = 1;
    dev->prog_size   = 1;
    dev->block_size  = SD_BLOCK_SIZE;
    dev->block_count = p->capacity_blocks;
    dev->ops         = &sdhci_ops;
    dev->priv        = p;
    dev->registered  = false;

    INFO("sdhci: %s ready (%u blocks @ %u bytes)",
         name, dev->block_count, dev->block_size);
    return dev;
}

void sdhci_destroy(struct blkdev *dev)
{
    if (!dev) return;
    /* Recover the alloc. dev points to the embedded `dev` field at
     * offset 0 of struct sdhci_alloc, so the alloc base is dev. */
    struct sdhci_alloc *a = (struct sdhci_alloc *)dev;
    INFO("sdhci: destroy %s", dev->name);
    size_t alloc_pages = (sizeof(struct sdhci_alloc) + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(a, alloc_pages);
}

/* ---- QEMU sdhci-pci convenience init. ---- */

#if !defined(PLATFORM_X86_64)

/* PCI class code for SD Host Controller per the PCI Code and ID
 * Assignment Specification: class 0x08 (Base System Peripherals),
 * subclass 0x05 (SD Host controller). prog_if 0x01 = "DMA-only";
 * 0x00 = "no DMA / vendor-specific". QEMU's sdhci-pci uses class
 * 0x080501; we match by (class, subclass) only and ignore prog_if
 * since we drive the device in PIO mode. */
#define PCI_CLASS_SYSTEM_PERIPHERAL  0x08
#define PCI_SUBCLASS_SD_HOST         0x05

struct blkdev *sdhci_create_qemu_pci(const char *name)
{
    const struct pcie_device *d = pcie_find_class(PCI_CLASS_SYSTEM_PERIPHERAL,
                                                  PCI_SUBCLASS_SD_HOST);
    if (!d) {
        /* Not an error — caller is expected to fall back to ramdisk
         * when this is missing (e.g. test runs without
         * `-device sdhci-pci`). */
        return NULL;
    }

    /* sdhci-pci puts its register window at BAR0. Standard SDHCI v3
     * register layout — what `sdhci_create()` already understands. */
    if (!(d->bar_flags[0] & PCIE_BAR_PRESENT)) {
        ERROR("sdhci-pci: BAR0 not present on %02x:%02x.%u",
              d->bus, d->dev, d->func);
        return NULL;
    }
    uint64_t bar0_size = 0;
    void *mmio = pcie_map_bar(d, 0, &bar0_size);
    if (!mmio) {
        ERROR("sdhci-pci: pcie_map_bar(BAR0) failed");
        return NULL;
    }
    if (bar0_size < 0x100) {
        ERROR("sdhci-pci: BAR0 size %lu < 256 — wrong register window?",
              (unsigned long)bar0_size);
        return NULL;
    }

    /* SDHCI uses memory-mapped registers only; no DMA in this PR.
     * Bus master is unnecessary for PIO transfers but is enabled
     * anyway so a future ADMA2 upgrade doesn't trip on it. */
    pcie_enable_bus_master(d);

    INFO("sdhci-pci: %02x:%02x.%u vendor=0x%04x dev=0x%04x BAR0=0x%lx (%lu B)",
         d->bus, d->dev, d->func, d->vendor_id, d->device_id,
         (unsigned long)d->bar[0], (unsigned long)bar0_size);

    return sdhci_create(name, (uintptr_t)mmio);
}

#else  /* PLATFORM_X86_64 */

struct blkdev *sdhci_create_qemu_pci(const char *name)
{
    /* x86-64 builds use the legacy `pci.h` path, not pcie.h. The
     * dynamic-kernel-replace plan is Pi 5-targeted; there's no
     * production motive to wire SDHCI on x86-64 yet. */
    (void)name;
    return NULL;
}

#endif

/* ---- Pi 5 BCM2712 EMMC2 convenience init. ---- */

#if defined(PLATFORM_RASPI5)

#include "bcm_mailbox.h"
#include "bcm_mailbox_proto.h"
#include "uart.h"

/* AON GPIO base + register offsets are in `platform.h` so this
 * driver and `kernel/src/main.c` (ACT LED) reference one source of
 * truth. SD-card-specific bit names live here — they're a
 * driver-local detail. */
#define AON_GPIO_BIT_SD_VCC    (1u << 4)
#define AON_GPIO_BIT_SD_IO_1V8 (1u << 3)

/* SDIO_CFG bank register offsets (relative to BCM2712_EMMC2_CFG_BASE).
 * Pinned to docs/reference/rpi-linux-sdhci-brcmstb.c (rpi-6.12.y)
 * lines 37-51. Linux re-uses these offsets across all brcmstb-family
 * SDHCI bindings; the bcm2712 path uses them via cfginit_2712. */
#define SDIO_CFG_CTRL                       0x00u
#define   SDIO_CFG_CTRL_SDCD_N_TEST_LEV     (1u << 30)  /* 0 = card present */
#define   SDIO_CFG_CTRL_SDCD_N_TEST_EN      (1u << 31)  /* override CD line */
#define SDIO_CFG_CQ_CAPABILITY              0x4Cu
#define   SDIO_CFG_CQ_CAPABILITY_FMUL_SHIFT 12u

/* Base clock advisory written into SDIO_CFG_CQ_CAPABILITY. The DT
 * for bcm2712 declares `clk_emmc2` as a 200 MHz fixed-clock — there
 * is no actual programmable PLL behind it on the Pi 5, just a
 * constant the firmware leaves running. Linux passes this value to
 * the controller in MHz so the timeout calculations come out right.
 * 200 MHz matches what cfginit_2712's `clk_get_rate(pltfm_host->clk)`
 * would return. */
#define BCM2712_EMMC2_BASE_CLK_MHZ          200u

/*
 * Apply the BCM2712-specific SDHCI cfginit, mirroring Linux's
 * sdhci_brcmstb_cfginit_2712 (docs/reference/rpi-linux-sdhci-brcmstb.c
 * lines 260-296), trimmed to what SLM-OS actually needs:
 *
 *   - Force card-detect via SDIO_CFG_CTRL — the lab fixture's SDWire
 *     and any production card the admin command writes to are not
 *     hot-removable from SLM-OS's point of view. Setting
 *     SDCD_N_TEST_EN with SDCD_N_TEST_LEV cleared makes the
 *     controller report "card present" regardless of the CD line.
 *   - Write the controller's base-clock advisory in MHz to
 *     SDIO_CFG_CQ_CAPABILITY. This is what the firmware tells the
 *     SDHCI core to use for timeout calculations; without it the
 *     core defaults to 0 and command timeouts come out wrong.
 *
 * Skipped vs. Linux's full cfginit:
 *   - SDIO_CFG_MAX_50MHZ_MODE only matters for UHS-I / HS400, which
 *     SLM-OS doesn't support (legacy 25 MHz SDR only — see this
 *     file's header comment).
 *   - The dynamic FMUL fields above the base-clock are derived from
 *     a clock SLM-OS doesn't have a framework to query; the static
 *     200 MHz from the DT fixed-clock is correct for both lab and
 *     production Pi 5 boards.
 */
static void bcm2712_emmc2_cfginit(void)
{
    volatile uint32_t *ctrl_reg =
        (volatile uint32_t *)(BCM2712_EMMC2_CFG_BASE + SDIO_CFG_CTRL);
    volatile uint32_t *cqcap_reg =
        (volatile uint32_t *)(BCM2712_EMMC2_CFG_BASE + SDIO_CFG_CQ_CAPABILITY);

    /* Force CD: enable the test-level override and clear the level
     * bit (i.e. report 'present', the active-low n_TEST_LEV bit
     * cleared). Read-modify-write so the Pi firmware's other strap
     * bits in this register stay intact. */
    uint32_t ctrl = *ctrl_reg;
    ctrl &= ~SDIO_CFG_CTRL_SDCD_N_TEST_LEV;
    ctrl |=  SDIO_CFG_CTRL_SDCD_N_TEST_EN;
    *ctrl_reg = ctrl;

    /* Base-clock advisory in MHz, plus the FMUL hint Linux always
     * sets (3 << 12) — same constant as
     * sdhci_brcmstb_cfginit_2712. */
    *cqcap_reg = (3u << SDIO_CFG_CQ_CAPABILITY_FMUL_SHIFT)
               | BCM2712_EMMC2_BASE_CLK_MHZ;
}

/*
 * Drive the AON GPIO regulators that power the SD card slot.
 * `bcm2712-rpi-5-b.dts:79-101` declares two regulators tied to AON
 * GPIO pins:
 *   - `sd_vcc_reg`     on pin 4 (regulator-fixed, regulator-always-on,
 *                      enabled when pin is driven HIGH)
 *   - `sd_io_1v8_reg`  on pin 3 (regulator-gpio, mapping `<3300000 0;
 *                      1800000 1>` — drive LOW for 3.3 V, HIGH for 1.8 V)
 *
 * Linux marks them `regulator-always-on`, so the Pi firmware leaves
 * them in the "on / 3.3 V" state across the Linux handoff and the
 * sdhci driver inherits a powered card. For SLM-OS bare-metal handoff
 * the firmware doesn't honor regulator framework annotations, and the
 * pins land in their POR state with VCC off — which is why the SDHCI
 * host registers are unreachable until we drive the regulators
 * ourselves.
 */
static void bcm2712_aon_gpio_drive_sd_regulators(void)
{
    volatile uint32_t *iodir = (volatile uint32_t *)BCM2712_AON_GPIO_IODIR;
    volatile uint32_t *data  = (volatile uint32_t *)BCM2712_AON_GPIO_DATA;

    /* Drive: pin 4 high (VCC on), pin 3 low (3.3 V mode). Set DATA
     * before flipping IODIR so the pin doesn't briefly drive its
     * residual value when it transitions to output.
     *
     * On the lab Pi 5, an empirical readback showed the firmware
     * already leaves these pins in the desired state at SLM-OS
     * handoff (DATA=0x16057 has bit 4 set + bit 3 clear; IODIR=0x1FDE3
     * has both pins as outputs). The writes here are belt-and-
     * suspenders for boards / firmware revisions that don't honor
     * the regulator-boot-on annotations in the dts. */
    uint32_t dval = *data;
    dval |=  AON_GPIO_BIT_SD_VCC;     /* drive bit 4 high */
    dval &= ~AON_GPIO_BIT_SD_IO_1V8;  /* drive bit 3 low (3.3 V) */
    *data = dval;

    /* Mark both pins as outputs. brcmstb IODIR uses 0 = output,
     * 1 = input — opposite of the legacy bcm2835 GPIO convention. */
    uint32_t ival = *iodir;
    ival &= ~(AON_GPIO_BIT_SD_VCC | AON_GPIO_BIT_SD_IO_1V8);
    *iodir = ival;
    __asm__ volatile("dsb sy" ::: "memory");
}

struct blkdev *sdhci_create_bcm2712(void)
{
    /*
     * Issue #414: the Pi firmware does NOT auto-bring-up EMMC2 for
     * SLM-OS bare-metal handoff the way it does for a Linux launch.
     * On bare-metal the controller is gated; the first MMIO read at
     * BCM2712_EMMC2_BASE hangs the AXI fabric until the firmware
     * brings it up. Empirically confirmed from the shell — bare
     * `peek 0x1000FFF000` wedges the kernel.
     *
     * This routine assembles the bring-up steps Linux's sdhci-brcmstb
     * driver gets transparently from clock + regulator + pinctrl
     * frameworks. Each step has been verified individually on the
     * Pi 5 lab fixture:
     *   1. AON GPIO regulators (sd_vcc on pin 4, 3.3 V on pin 3) —
     *      firmware-left state was already correct on our boards
     *      but we re-assert defensively.
     *   2. Mailbox SET_CLOCK_STATE(clock_id=12 / EMMC2, on=1) —
     *      firmware reports the clock toggled 0 → 1.
     *   3. SDIO_CFG_* writes (force-CD, base-clock advisory).
     *
     * NOTE (#414 partial): even with all three steps, real-Pi-5
     * `kernel status` still hangs in the CFG bank's first read
     * (0x1000FFF400). Firmware reports the clock as on but
     * GET_CLOCK_RATE_MEASURED returns ~1.07 GHz — inconsistent with
     * the dtsi's `clk_emmc2: clock-frequency = <200000000>` fixed-
     * clock declaration. The remaining gap is a Pi 5-specific
     * gating mechanism not documented in upstream Linux's
     * sdhci-brcmstb / clk-raspberrypi sources. Tracking under #414;
     * the bring-up infrastructure here is correct for Pi 4 EMMC and
     * a starting point for further Pi 5 investigation.
     */
    /* SDIO1 busisol register: do NOT touch.
     *
     * Empirical: Pi firmware leaves it at 0x00006001 (read live via
     * an earlier diagnostic build). Per Linux's
     * `bcm2712_init_sd_express`, bits 13:14 (the 0x6000 mask) are the
     * PCIe-sideband isolation: SET = SD mode, CLEARED = PCIe mode.
     * Bit 0 (the 0x0001) is the SD-clock isolation enable. Together
     * 0x6001 is the correct "SD-card mode, controller live" value.
     *
     * Writing 0 to busisol (an earlier #414 attempt) clears bits
     * 13:14 and switches the controller to PCIe-sideband mode, which
     * is the WRONG direction for normal SD-card operation and is
     * itself a possible cause of the AXI hang on the host bank.
     */

    /* Drive the SD card VCC + IO voltage regulators on the AON GPIO
     * bank. On the lab Pi 5 the firmware already leaves these in
     * the correct state, but this is belt-and-suspenders for boards
     * / firmware revisions that don't honor the regulator-boot-on
     * dts annotations. */
    uart_puts("[INFO] sdhci_bcm2712: driving AON GPIO regulators (SD VCC on, 3.3V)\n");
    bcm2712_aon_gpio_drive_sd_regulators();

    /* Note: empirically (via the `mboxclk` shell diagnostic) the Pi
     * firmware leaves clock id 1 (EMMC) running at 200 MHz across
     * SLM-OS handoff — this call is idempotent and verifies the
     * mailbox is responsive, but does not actually toggle a gated
     * clock on Pi 5. */
    uart_puts("[INFO] sdhci_bcm2712: confirming EMMC clock state via mailbox\n");
    int rc = bcm_mailbox_set_clock_state(BCM_CLOCK_EMMC, /*on=*/true);
    if (rc != 0) {
        uart_puts("[ERROR] sdhci_bcm2712: mailbox SET_CLOCK_STATE(EMMC, on) "
                  "failed — abort\n");
        return NULL;
    }

    /* Set the operating rate. Linux's brcmstb sdhci driver pulls
     * this from `clk_get_rate(pltfm_host->clk)` which on Pi 5 resolves
     * to a 200 MHz fixed-clock — and `bcm2712.dtsi` declares
     * `clk_emmc` as `fixed-clock`, so this rate is constant by
     * definition. The firmware accordingly rejects SET_CLOCK_RATE on
     * this id (no programmable PLL behind it). Treat the failure as
     * advisory: log it but continue, since clock 1 is already
     * running at 200 MHz at SLM-OS handoff. */
    uint32_t actual_hz = 0;
    rc = bcm_mailbox_set_clock_rate(BCM_CLOCK_EMMC,
                                    /*requested_hz=*/200000000u,
                                    &actual_hz);
    if (rc != 0) {
        uart_puts("[INFO] sdhci_bcm2712: SET_CLOCK_RATE not supported for EMMC "
                  "(fixed-clock per dtsi) — continuing on existing clock\n");
    } else {
        uart_puts("[INFO] sdhci_bcm2712: SET_CLOCK_RATE accepted; "
                  "applying SDIO_CFG cfginit\n");
    }

    /* Apply the BCM2712 SDIO_CFG_* writes. Linux does this in
     * cfginit_2712 between mapping the controller and reading
     * SDHCI capabilities; same order in SLM-OS — write the CFG
     * bank first, then have sdhci_create() do the host-register
     * probe. */
    bcm2712_emmc2_cfginit();
    uart_puts("[INFO] sdhci_bcm2712: cfginit done; entering generic probe\n");

    return sdhci_create("emmc2", BCM2712_EMMC2_BASE);
}

#endif /* PLATFORM_RASPI5 */
