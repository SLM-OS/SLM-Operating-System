/*
 * i2c_tegra.c — Tegra234 HSI2C controller driver (polled).
 *
 * Reference: Linux v6.12 `drivers/i2c/busses/i2c-tegra.c`, cached at
 * `~/slmos-ref/linux/linux-i2c-tegra.c`. SLM-OS port is intentionally
 * minimal — polled, no DMA, no IRQ, single-master, 7-bit addressing,
 * standard-mode (100 kHz). Sufficient for IMX219 register access.
 *
 * Packet-mode wire protocol (per Tegra234 TRM § HSI2C):
 *   1. Push a 12-byte (3 × u32) packet header into TX_FIFO:
 *        word 0: PACKET_HEADER0 — protocol=I2C, packet_id=1
 *        word 1: msg_len - 1 (byte count)
 *        word 2: I2C_HEADER  — IE_ENABLE | (slave<<1) | flags
 *   2. For writes: push the data bytes (LE-packed into u32 words).
 *      For reads:  poll RX_FIFO for the response bytes.
 *   3. Wait for I2C_INT_PACKET_XFER_COMPLETE in I2C_INT_STATUS.
 *      Check NO_ACK / ARBITRATION_LOST for transport errors.
 *   4. Clear status, flush FIFOs, ready for next message.
 *
 * Multi-message transfers (e.g. read-after-write for register reads)
 * set I2C_HEADER_REPEAT_START on all but the last message so the
 * controller skips STOP+START between them.
 */

#include "i2c_tegra.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stddef.h>
#include <stdint.h>

#include "bpmp.h"
#include "platform.h"
#include "spinlock.h"
#include "tegra234_clocks.h"
#include "timer.h"
#include "uart.h"

/* ---- Register offsets (subset; full set in linux-i2c-tegra.c) ---- */

#define I2C_CNFG                 0x000u
#define I2C_TX_FIFO              0x050u
#define I2C_RX_FIFO              0x054u
#define I2C_INT_MASK             0x064u
#define I2C_INT_STATUS           0x068u
#define I2C_CLK_DIVISOR          0x06Cu
/* Tegra194/234-only: MST_FIFO_* replaces the legacy FIFO_CONTROL/STATUS
 * at 0x05C/0x060. The legacy registers exist but stay zeroed; using them
 * leaves the controller in an unconfigured state where a READ packet
 * completes (PACKET_XFER_COMPLETE fires) without the controller ever
 * popping the slave's response byte into RX_FIFO. */
#define I2C_CONFIG_LOAD          0x08Cu
#define I2C_INTERFACE_TIMING_0   0x094u
#define I2C_MST_FIFO_CONTROL     0x0B4u
#define I2C_MST_FIFO_STATUS      0x0B8u

/* I2C_CNFG bits */
#define I2C_CNFG_PACKET_MODE_EN  (1u << 10)
#define I2C_CNFG_NEW_MASTER_FSM  (1u << 11)
#define I2C_CNFG_DEBOUNCE_CNT_2  (2u << 12)         /* DEBOUNCE_CNT field, 2 cycles */

/* I2C_MST_FIFO_CONTROL bits (T194/T234) */
#define I2C_MST_FIFO_CTRL_RX_FLUSH   (1u << 0)
#define I2C_MST_FIFO_CTRL_TX_FLUSH   (1u << 1)
#define I2C_MST_FIFO_CTRL_RX_TRIG(x) (((x) - 1u) << 4)   /* bits[10:4] */
#define I2C_MST_FIFO_CTRL_TX_TRIG(x) (((x) - 1u) << 16)  /* bits[22:16] */

/* I2C_MST_FIFO_STATUS layout (T194/T234) */
#define I2C_MST_FIFO_STATUS_RX_MASK  0xFFu              /* bits[7:0]: RX byte count */
#define I2C_MST_FIFO_STATUS_TX_MASK  0xFF0000u          /* bits[23:16]: TX free count */
#define I2C_MST_FIFO_STATUS_TX_SHIFT 16u

/* I2C_CONFIG_LOAD: write MSTR_CONFIG_LOAD, poll until cleared. Required
 * after writing CNFG/CLK_DIVISOR/INTERFACE_TIMING_* on T194/T234 — the
 * controller staging registers don't take effect otherwise. */
#define I2C_MSTR_CONFIG_LOAD     (1u << 0)

/* I2C_INTERFACE_TIMING_0 layout: TLOW in bits[5:0], THIGH in bits[13:8]. */
#define I2C_INTERFACE_TIMING_TLOW_SHIFT   0u
#define I2C_INTERFACE_TIMING_THIGH_SHIFT  8u

/* Standard-mode (100 kHz) clock-divisor / interface-timing values from
 * Linux's tegra194_i2c_hw struct (`~/slmos-ref/linux/linux-i2c-tegra.c`). */
#define I2C_T194_CLK_DIVISOR_STD_MODE  0x4Fu
#define I2C_T194_TLOW_STD_MODE         0x08u
#define I2C_T194_THIGH_STD_MODE        0x07u

/* I2C_INT_STATUS / INT_MASK bits */
#define I2C_INT_RX_FIFO_DATA_REQ      (1u << 0)
#define I2C_INT_TX_FIFO_DATA_REQ      (1u << 1)
#define I2C_INT_ARBITRATION_LOST      (1u << 2)
#define I2C_INT_NO_ACK                (1u << 3)
#define I2C_INT_PACKET_XFER_COMPLETE  (1u << 7)
#define I2C_INT_ALL_PACKET_BITS \
    (I2C_INT_PACKET_XFER_COMPLETE | I2C_INT_NO_ACK | I2C_INT_ARBITRATION_LOST)

/* PACKET_HEADER0 fields */
#define PACKET_HEADER0_PROTOCOL_I2C   1u
#define PACKET_HEADER0_PROTOCOL_SHIFT 4u
#define PACKET_HEADER0_PACKET_ID_SHIFT 16u
#define PACKET_HEADER0_PACKET_ID_DEFAULT \
    ((PACKET_HEADER0_PROTOCOL_I2C << PACKET_HEADER0_PROTOCOL_SHIFT) \
     | (1u << PACKET_HEADER0_PACKET_ID_SHIFT))      /* protocol=I2C, pid=1 */

/* I2C_HEADER (the third word of every transfer's packet header) */
#define I2C_HEADER_REPEAT_START      (1u << 16)
/* IE_ENABLE is set on every packet despite I2C_INT_MASK = 0 (we poll).
 * Tegra TRM requires the IE bit for INT_STATUS to latch
 * PACKET_XFER_COMPLETE / NO_ACK / ARBITRATION_LOST — the mask only
 * suppresses CPU IRQ delivery, not the status register update. */
#define I2C_HEADER_IE_ENABLE         (1u << 17)
#define I2C_HEADER_READ              (1u << 19)
#define I2C_HEADER_SLAVE_ADDR_SHIFT  1u             /* 7-bit addr in bits[7:1] */

/* Standard-mode clock-divisor word: STD_FAST_MODE goes in bits[31:16],
 * HSMODE in bits[15:0]. Tegra194/234 standard-mode value is 0x4F (≈ 80);
 * the older 0x19 (Tegra210 generation) leaves bus timing too fast for
 * the controller's internal load-config to settle, so packet completion
 * fires before the slave's response byte is captured into RX_FIFO. */
#define I2C_CLK_DIVISOR_VALUE \
    (((uint32_t)I2C_T194_CLK_DIVISOR_STD_MODE << 16) | 1u)

#define I2C_INTERFACE_TIMING_VALUE \
    (((uint32_t)I2C_T194_THIGH_STD_MODE << I2C_INTERFACE_TIMING_THIGH_SHIFT) \
   | ((uint32_t)I2C_T194_TLOW_STD_MODE  << I2C_INTERFACE_TIMING_TLOW_SHIFT))

#define POLL_TIMEOUT_US  100000u                    /* 100 ms — generous */

/* ---- MMIO accessors ---- */

static inline uint32_t i2c_read(struct tegra_i2c_bus *bus, uint32_t off)
{
    return *(volatile uint32_t *)(bus->base + off);
}

static inline void i2c_write(struct tegra_i2c_bus *bus, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(bus->base + off) = v;
    /* Linux issues a read-back on every write to defeat write merging
     * and force the access to actually leave the CPU; mirror that. */
    (void)i2c_read(bus, I2C_CNFG);
}

/* ---- Module-scope bus instances ---- */

struct tegra_i2c_bus tegra_i2c_cam_bus = {
    .base     = TEGRA234_CAM_I2C_BASE,
    .clk_id   = TEGRA234_CLK_I2C2,
    .reset_id = (int32_t)TEGRA234_RESET_I2C2,
    .name     = "cam_i2c",
    .lock     = SPINLOCK_INIT,
};

/* ---- Init helpers ---- */

static int wait_until_clear(struct tegra_i2c_bus *bus, uint32_t off,
                            uint32_t bits, uint64_t timeout_us)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0u) return -2;
    uint64_t deadline = timer_get_count() + (timeout_us * freq + 999999ULL) / 1000000ULL;
    while ((i2c_read(bus, off) & bits) != 0u) {
        if (timer_get_count() >= deadline) return -2;
    }
    return 0;
}

static int wait_until_set(struct tegra_i2c_bus *bus, uint32_t off,
                          uint32_t bits, uint64_t timeout_us,
                          uint32_t *out_val)
{
    uint64_t freq = timer_get_frequency();
    if (freq == 0u) return -2;
    uint64_t deadline = timer_get_count() + (timeout_us * freq + 999999ULL) / 1000000ULL;
    for (;;) {
        uint32_t v = i2c_read(bus, off);
        if ((v & bits) != 0u) {
            if (out_val) *out_val = v;
            return 0;
        }
        if (timer_get_count() >= deadline) {
            if (out_val) *out_val = v;
            return -2;
        }
    }
}

static int flush_fifos(struct tegra_i2c_bus *bus)
{
    /* Read-modify-write so we don't clobber TX_TRIG/RX_TRIG that
     * tegra_i2c_init has already programmed. The flush bits are
     * write-1-to-trigger, hardware-cleared when the flush completes. */
    uint32_t v = i2c_read(bus, I2C_MST_FIFO_CONTROL);
    v |= I2C_MST_FIFO_CTRL_TX_FLUSH | I2C_MST_FIFO_CTRL_RX_FLUSH;
    i2c_write(bus, I2C_MST_FIFO_CONTROL, v);
    return wait_until_clear(bus, I2C_MST_FIFO_CONTROL,
                            I2C_MST_FIFO_CTRL_TX_FLUSH
                          | I2C_MST_FIFO_CTRL_RX_FLUSH,
                            10000u);   /* 10 ms */
}

/* Tegra194/234: writes to I2C_CNFG / I2C_CLK_DIVISOR / I2C_INTERFACE_TIMING_*
 * land in staging registers and don't take effect until MSTR_CONFIG_LOAD
 * is written and self-clears. Skipping this is the bug that caused
 * IMX219 reads to complete with PACKET_XFER_COMPLETE but RX_FIFO empty —
 * the controller's bus-timing FSM was running on stale (zero) defaults. */
static int wait_for_config_load(struct tegra_i2c_bus *bus)
{
    i2c_write(bus, I2C_CONFIG_LOAD, I2C_MSTR_CONFIG_LOAD);
    return wait_until_clear(bus, I2C_CONFIG_LOAD,
                            I2C_MSTR_CONFIG_LOAD,
                            1000000u);  /* 1 s, matches Linux */
}

int tegra_i2c_init(struct tegra_i2c_bus *bus)
{
    if (!bus) return -1;

    irq_flags_t flags = spin_lock_irqsave(&bus->lock);

    /* Idempotent — bpmp_clk_enable is a no-op if Linux already left the
     * clock running, which is the normal post-kexec state for cam_i2c.
     * BPMP IPC is itself lock-free polled, so holding `bus->lock`
     * across the call is safe (no deadlock window). */
    int rc = bpmp_init();
    if (rc != 0) { spin_unlock_irqrestore(&bus->lock, flags); return -2; }
    rc = bpmp_clk_enable(bus->clk_id);
    if (rc != 0) { spin_unlock_irqrestore(&bus->lock, flags); return -2; }
    if (bus->reset_id >= 0) {
        rc = bpmp_reset_deassert((uint32_t)bus->reset_id);
        if (rc != 0) {
            spin_unlock_irqrestore(&bus->lock, flags);
            return -3;
        }
    }

    /* Mask all interrupts (we poll). */
    i2c_write(bus, I2C_INT_MASK, 0u);

    /* Configure controller. NEW_MASTER_FSM + PACKET_MODE_EN are the
     * Tegra-194/234 baseline; DEBOUNCE_CNT=2 matches Linux's default. */
    i2c_write(bus, I2C_CNFG,
              I2C_CNFG_NEW_MASTER_FSM | I2C_CNFG_PACKET_MODE_EN
              | I2C_CNFG_DEBOUNCE_CNT_2);

    i2c_write(bus, I2C_CLK_DIVISOR, I2C_CLK_DIVISOR_VALUE);

    /* T194/T234 needs the interface-timing register written explicitly;
     * the chip default is 0 which produces an invalid bus-timing FSM. */
    i2c_write(bus, I2C_INTERFACE_TIMING_0, I2C_INTERFACE_TIMING_VALUE);

    /* Program MST_FIFO trigger thresholds before issuing the flush —
     * flush_fifos() does an RMW that preserves these. RX_TRIG=1 fires
     * RX_FIFO_DATA_REQ as soon as one byte lands; TX_TRIG=8 fires
     * TX_FIFO_DATA_REQ when ≥ 8 slots are free. */
    i2c_write(bus, I2C_MST_FIFO_CONTROL,
              I2C_MST_FIFO_CTRL_RX_TRIG(1) | I2C_MST_FIFO_CTRL_TX_TRIG(8));

    /* Clear any latched interrupt status (write-1-to-clear semantics). */
    i2c_write(bus, I2C_INT_STATUS, 0xFFFFFFFFu);

    int flush_rc = flush_fifos(bus);
    if (flush_rc != 0) {
        spin_unlock_irqrestore(&bus->lock, flags);
        return -4;
    }

    /* Commit the staging-register writes above. Without this, the
     * controller continues to run on whatever Linux (or the chip
     * default) had loaded, and our timing/divisor values are ignored. */
    int load_rc = wait_for_config_load(bus);
    spin_unlock_irqrestore(&bus->lock, flags);
    return load_rc != 0 ? -4 : 0;
}

/* ---- Packet-mode transfer engine ---- */

/* Push a packet header for one message of `len` bytes. */
static void push_packet_header(struct tegra_i2c_bus *bus,
                               uint8_t  slave_7bit,
                               uint32_t len,
                               int      is_read,
                               int      repeat_start)
{
    uint32_t hdr2 = I2C_HEADER_IE_ENABLE
                  | ((uint32_t)slave_7bit << I2C_HEADER_SLAVE_ADDR_SHIFT);
    if (is_read)      hdr2 |= I2C_HEADER_READ;
    if (repeat_start) hdr2 |= I2C_HEADER_REPEAT_START;

    i2c_write(bus, I2C_TX_FIFO, PACKET_HEADER0_PACKET_ID_DEFAULT);
    i2c_write(bus, I2C_TX_FIFO, len - 1u);   /* msg length - 1 */
    i2c_write(bus, I2C_TX_FIFO, hdr2);
}

/* Push `len` payload bytes into TX_FIFO, packed LE into u32 words.
 * Tail bytes (len % 4 != 0) ride in the upper bits of the final word
 * — the controller knows from the packet header how many bytes are
 * actually meaningful and ignores the rest. No length cap; a future
 * sensor-mode-set burst can call this with len up to 32 KB before
 * the FIFO depth becomes a concern. */
static void push_payload(struct tegra_i2c_bus *bus,
                         const uint8_t *buf, uint32_t len)
{
    uint32_t i = 0u;
    while (i < len) {
        uint32_t w = 0u;
        uint32_t chunk = (len - i) > 4u ? 4u : (len - i);
        for (uint32_t k = 0; k < chunk; k++) {
            w |= (uint32_t)buf[i + k] << (k * 8u);
        }
        i2c_write(bus, I2C_TX_FIFO, w);
        i += chunk;
    }
}

/* Wait for the current packet to complete. Returns 0 on success, or
 * -3 (NACK) / -4 (arbitration lost) / -2 (timeout). */
static int wait_packet_complete(struct tegra_i2c_bus *bus)
{
    uint32_t status = 0;
    int rc = wait_until_set(bus, I2C_INT_STATUS, I2C_INT_ALL_PACKET_BITS,
                            POLL_TIMEOUT_US, &status);
    /* Always clear whatever set bits we observed, even on timeout. */
    i2c_write(bus, I2C_INT_STATUS, status);
    if (rc != 0) return -2;
    if (status & I2C_INT_ARBITRATION_LOST) return -4;
    if (status & I2C_INT_NO_ACK)           return -3;
    if (status & I2C_INT_PACKET_XFER_COMPLETE) return 0;
    return -2;
}

/* Maximum payload bytes per packet, computed from the controller's
 * FIFO depth: TX_FIFO is 64 bytes (16 words) and each packet header
 * eats 12 bytes (3 words), leaving 52 bytes for data before the
 * controller's hardware drain rate is the only thing keeping us from
 * a FIFO overrun. The IMX219 register-write path passes 3 bytes; this
 * cap is here to keep a future burst-write caller correct-by-default. */
#define I2C_PACKET_PAYLOAD_MAX  52u

/* Issue one write message. `repeat_start` controls whether the
 * controller follows this message with REPEAT-START (1) or STOP (0). */
static int xfer_write(struct tegra_i2c_bus *bus, uint8_t slave,
                      const uint8_t *buf, uint32_t len, int repeat_start)
{
    if (len == 0u || len > I2C_PACKET_PAYLOAD_MAX) return -1;
    if (flush_fifos(bus) != 0) return -2;
    push_packet_header(bus, slave, len, 0 /*is_read*/, repeat_start);
    push_payload(bus, buf, len);
    return wait_packet_complete(bus);
}

/* Issue one read message of `len` bytes (≤ 4). REPEAT-START is implied
 * if the previous message left the bus claimed; STOP is issued at the
 * end of this read. */
static int xfer_read(struct tegra_i2c_bus *bus, uint8_t slave,
                     uint8_t *buf, uint32_t len)
{
    if (len == 0u || len > 4u) return -1;
    /* Don't flush before a read — we may have just left the bus
     * mid-transaction with REPEAT_START from the preceding write. */
    push_packet_header(bus, slave, len, 1 /*is_read*/, 0 /*no rs after read*/);

    int rc = wait_packet_complete(bus);
    if (rc != 0) return rc;

    /* RX FIFO has the response bytes packed LE in u32 words. We
     * always read exactly one word here because len ≤ 4. */
    uint32_t fs = i2c_read(bus, I2C_MST_FIFO_STATUS);
    uint32_t rx_count = fs & I2C_MST_FIFO_STATUS_RX_MASK;
    if (rx_count == 0u) return -5;

    uint32_t w = i2c_read(bus, I2C_RX_FIFO);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((w >> (i * 8u)) & 0xFFu);
    }
    return 0;
}

/* ---- Public 16-bit-register API ---- */

int tegra_i2c_write_reg16(struct tegra_i2c_bus *bus, uint8_t slave,
                          uint16_t reg, uint8_t val)
{
    if (!bus || slave > 0x7Fu) return -1;
    uint8_t buf[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFu),
        val,
    };
    irq_flags_t flags = spin_lock_irqsave(&bus->lock);
    int rc = xfer_write(bus, slave, buf, 3u, 0 /*final, send STOP*/);
    spin_unlock_irqrestore(&bus->lock, flags);
    return rc;
}

int tegra_i2c_write_reg16_val16(struct tegra_i2c_bus *bus, uint8_t slave,
                                uint16_t reg, uint16_t val)
{
    if (!bus || slave > 0x7Fu) return -1;
    /* IMX219 (and most CCI sensors) latch multi-byte values
     * big-endian: high byte at the lower-numbered register, low
     * byte at reg+1. Same wire pattern L4T's `cci_write` uses
     * via CCI_REG16. */
    uint8_t buf[4] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFu),
        (uint8_t)(val >> 8),
        (uint8_t)(val & 0xFFu),
    };
    irq_flags_t flags = spin_lock_irqsave(&bus->lock);
    int rc = xfer_write(bus, slave, buf, 4u, 0 /*final, send STOP*/);
    spin_unlock_irqrestore(&bus->lock, flags);
    return rc;
}

int tegra_i2c_read_reg16(struct tegra_i2c_bus *bus, uint8_t slave,
                         uint16_t reg, uint8_t *out)
{
    if (!bus || !out || slave > 0x7Fu) return -1;
    uint8_t addr[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFFu),
    };
    irq_flags_t flags = spin_lock_irqsave(&bus->lock);
    int rc = xfer_write(bus, slave, addr, 2u, 1 /*REPEAT_START*/);
    if (rc == 0) rc = xfer_read(bus, slave, out, 1u);
    spin_unlock_irqrestore(&bus->lock, flags);
    return rc;
}

/* Live snapshot of the controller's status registers. No locking — the
 * caller is expected to be the failure-path diagnostic in the IMX219
 * shell command, single-CPU shell context, with the controller idle
 * after a returned error. */
void tegra_i2c_dump_status(struct tegra_i2c_bus *bus,
                           struct tegra_i2c_regdump_entry *out, uint32_t n)
{
    static const struct {
        const char *name;
        uint32_t    off;
    } regs[] = {
        { "CNFG",            0x000 },
        { "STATUS",          0x01C },  /* I2C controller bus state */
        { "INT_STATUS",      0x068 },
        { "CLK_DIVISOR",     0x06C },
        { "CONFIG_LOAD",     0x08C },
        { "INTERFACE_TIM_0", 0x094 },
        { "MST_FIFO_CTRL",   0x0B4 },
        { "MST_FIFO_STAT",   0x0B8 },
        { "FIFO_STATUS",     0x060 },  /* legacy view, useful for diff */
        { "INT_MASK",        0x064 },
    };
    uint32_t cap = (uint32_t)(sizeof(regs) / sizeof(regs[0]));
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) {
        out[i].name  = regs[i].name;
        out[i].value = i2c_read(bus, regs[i].off);
    }
}

#else /* !PLATFORM_JETSON_ORIN_NANO — stubs for cross-platform builds */

/* No bus instance on non-Jetson; declare a placeholder so test files
 * that reference the symbol link cleanly when the test builds for
 * QEMU. The stubs return errors so a stray caller fails loudly. */
struct tegra_i2c_bus tegra_i2c_cam_bus = {
    .base     = 0,
    .clk_id   = 0,
    .reset_id = -1,
    .name     = "stub",
    .lock     = SPINLOCK_INIT,
};

int tegra_i2c_init(struct tegra_i2c_bus *bus) { (void)bus; return -1; }
int tegra_i2c_write_reg16(struct tegra_i2c_bus *bus, uint8_t slave,
                          uint16_t reg, uint8_t val)
{ (void)bus; (void)slave; (void)reg; (void)val; return -1; }
int tegra_i2c_write_reg16_val16(struct tegra_i2c_bus *bus, uint8_t slave,
                                uint16_t reg, uint16_t val)
{ (void)bus; (void)slave; (void)reg; (void)val; return -1; }
int tegra_i2c_read_reg16(struct tegra_i2c_bus *bus, uint8_t slave,
                         uint16_t reg, uint8_t *out)
{ (void)bus; (void)slave; (void)reg; (void)out; return -1; }

void tegra_i2c_dump_status(struct tegra_i2c_bus *bus,
                           struct tegra_i2c_regdump_entry *out, uint32_t n)
{ (void)bus; (void)out; (void)n; }

#endif /* PLATFORM_JETSON_ORIN_NANO */
