/*
 * bpmp.c - BPMP (Boot and Power Management Processor) driver for Tegra234
 *
 * Minimal implementation of BPMP IPC for clock/reset control.
 * This is a simplified version that doesn't use the full IVC protocol,
 * instead using the pre-initialized channels left by firmware.
 */

#include "bpmp.h"
#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

#if defined(PLATFORM_JETSON_ORIN_NANO)

/* ============================================================================
 * HSP (Hardware Synchronization Primitives) Registers
 *
 * The HSP provides doorbells for inter-processor notification.
 * Tegra234 uses HSP_TOP at 0x03C00000.
 * ============================================================================ */

/* HSP register offsets (from Linux tegra-hsp.c) */
#define HSP_INT_IE(x)           (0x100 + ((x) * 4))
#define HSP_INT_IV              0x300
#define HSP_INT_IR              0x304

/*
 * Doorbell registers are at a calculated offset based on HSP configuration.
 * For Tegra234, doorbells start at offset 0x10000 with 0x100 stride.
 * BPMP doorbell (index 3) is at offset 0x10300.
 * CCPLEX doorbell (index 0) is at offset 0x10000.
 */
#define HSP_DB_BASE_OFFSET      0x10000
#define HSP_DB_STRIDE           0x100
#define HSP_DB_TRIGGER          0x0
#define HSP_DB_ENABLE           0x4
#define HSP_DB_RAW              0x8
#define HSP_DB_PENDING          0xC

/* Doorbell master IDs */
#define HSP_DB_MASTER_CCPLEX    0
#define HSP_DB_MASTER_BPMP      3

/* ============================================================================
 * IVC (Inter-VM Communication) Channel Layout
 *
 * The TX and RX buffers are pre-configured by firmware in SYSRAM.
 * Each channel has a header followed by data frames.
 *
 * IVC channel header (32 bytes):
 *   [0x00] w_count  - Write count (incremented by writer)
 *   [0x04] r_count  - Read count (incremented by reader)
 *   [0x08] w_notify - Write notify flag
 *   [0x0C] r_notify - Read notify flag
 *   [0x10] frame_size - Size of each data frame
 *   [0x14] nframes  - Number of frames in channel
 *   [0x18] reserved[2]
 *
 * After header: nframes * frame_size bytes of data
 * ============================================================================ */

/* IVC header structure */
struct ivc_channel_header {
    uint32_t w_count;       /* Write counter */
    uint32_t r_count;       /* Read counter */
    uint32_t w_notify;      /* Write notification pending */
    uint32_t r_notify;      /* Read notification pending */
    uint32_t frame_size;    /* Size of each frame */
    uint32_t nframes;       /* Number of frames */
    uint32_t reserved[2];
};

/* Offsets within IVC channel */
#define IVC_HEADER_SIZE         32
#define IVC_DATA_OFFSET         64      /* Frames start at 64-byte aligned offset */

/* Message buffer size (from Linux tegra-bpmp-ivc.h) */
#define MSG_SZ                  128
#define MSG_DATA_SZ             120     /* MSG_SZ - 8 byte header */

/* ============================================================================
 * Global State
 * ============================================================================ */

static volatile uint32_t *g_hsp_base;
static volatile uint8_t *g_tx_base;
static volatile uint8_t *g_rx_base;
static bool g_bpmp_initialized;

/* ============================================================================
 * Memory Access Helpers
 * ============================================================================ */

static inline void mmio_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t mmio_read32(volatile void *addr)
{
    uint32_t val = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return val;
}

/* ============================================================================
 * HSP Doorbell Functions
 * ============================================================================ */

static volatile uint32_t *hsp_db_reg(int master, int offset)
{
    uint32_t db_offset = HSP_DB_BASE_OFFSET + (master * HSP_DB_STRIDE) + offset;
    return (volatile uint32_t *)((uint8_t *)g_hsp_base + db_offset);
}

/* Ring the BPMP doorbell to notify it of a pending message */
static void hsp_ring_bpmp(void)
{
    mmio_write32(hsp_db_reg(HSP_DB_MASTER_BPMP, HSP_DB_TRIGGER), 1);
}

/* Check if CCPLEX doorbell is pending (BPMP rang us) */
static bool hsp_ccplex_pending(void)
{
    return (mmio_read32(hsp_db_reg(HSP_DB_MASTER_CCPLEX, HSP_DB_PENDING)) &
            (1 << HSP_DB_MASTER_BPMP)) != 0;
}

/* Clear CCPLEX doorbell pending bit */
static void hsp_ccplex_clear(void)
{
    mmio_write32(hsp_db_reg(HSP_DB_MASTER_CCPLEX, HSP_DB_PENDING),
                 1 << HSP_DB_MASTER_BPMP);
}

/* ============================================================================
 * IVC Channel Functions
 *
 * Simplified IVC that assumes single-frame channels set up by firmware.
 * Real IVC has ring buffers with multiple frames - we use just one.
 * ============================================================================ */

static volatile struct ivc_channel_header *tx_header(void)
{
    return (volatile struct ivc_channel_header *)g_tx_base;
}

static volatile struct ivc_channel_header *rx_header(void)
{
    return (volatile struct ivc_channel_header *)g_rx_base;
}

static volatile uint8_t *tx_frame(void)
{
    return g_tx_base + IVC_DATA_OFFSET;
}

static volatile uint8_t *rx_frame(void)
{
    return g_rx_base + IVC_DATA_OFFSET;
}

/* Check if TX channel has space for a new message */
static bool ivc_can_write(void)
{
    volatile struct ivc_channel_header *h = tx_header();
    /* Simple check: can write if w_count == r_count (empty) */
    return (mmio_read32(&h->w_count) - mmio_read32(&h->r_count)) == 0;
}

/* Check if RX channel has a message to read */
static bool ivc_can_read(void)
{
    volatile struct ivc_channel_header *h = rx_header();
    /* Can read if w_count != r_count */
    return mmio_read32(&h->w_count) != mmio_read32(&h->r_count);
}

/* Write a message to TX channel */
static void ivc_write(const void *data, uint32_t len)
{
    volatile struct ivc_channel_header *h = tx_header();
    volatile uint8_t *frame = tx_frame();
    const uint8_t *src = data;

    /* Copy data to frame */
    for (uint32_t i = 0; i < len && i < MSG_SZ; i++) {
        frame[i] = src[i];
    }

    /* Memory barrier before updating counter */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Increment write count to signal message is ready */
    mmio_write32(&h->w_count, mmio_read32(&h->w_count) + 1);
}

/* Read a message from RX channel */
static void ivc_read(void *data, uint32_t len)
{
    volatile struct ivc_channel_header *h = rx_header();
    volatile uint8_t *frame = rx_frame();
    uint8_t *dst = data;

    /* Memory barrier before reading */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Copy data from frame */
    for (uint32_t i = 0; i < len && i < MSG_SZ; i++) {
        dst[i] = frame[i];
    }

    /* Increment read count to acknowledge message */
    mmio_write32(&h->r_count, mmio_read32(&h->r_count) + 1);
}

/* ============================================================================
 * MRQ Message Functions
 * ============================================================================ */

/* Send an MRQ request and wait for response */
static int bpmp_send_mrq(uint32_t mrq, const void *tx_data, uint32_t tx_len,
                         void *rx_data, uint32_t rx_len)
{
    uint8_t msg[MSG_SZ] = {0};
    struct ivc_frame_header *hdr = (struct ivc_frame_header *)msg;

    /* Build message header */
    hdr->mrq = mrq;
    hdr->flags = 0;

    /* Copy TX data after header */
    if (tx_data && tx_len > 0) {
        const uint8_t *src = tx_data;
        for (uint32_t i = 0; i < tx_len && i < MSG_DATA_SZ; i++) {
            msg[8 + i] = src[i];
        }
    }

    /* Wait for TX channel to be ready */
    uint32_t timeout = 100000;
    while (!ivc_can_write() && timeout > 0) {
        timeout--;
    }
    if (timeout == 0) {
        return -1;  /* TX timeout */
    }

    /* Send message */
    ivc_write(msg, MSG_SZ);

    /* Ring BPMP doorbell */
    hsp_ring_bpmp();

    /* Wait for response */
    timeout = 100000;
    while (!ivc_can_read() && timeout > 0) {
        /* Check for doorbell notification */
        if (hsp_ccplex_pending()) {
            hsp_ccplex_clear();
        }
        timeout--;
    }
    if (timeout == 0) {
        return -2;  /* RX timeout */
    }

    /* Read response */
    uint8_t resp[MSG_SZ] = {0};
    ivc_read(resp, MSG_SZ);

    /* Copy RX data (skip header) */
    if (rx_data && rx_len > 0) {
        uint8_t *dst = rx_data;
        for (uint32_t i = 0; i < rx_len && i < MSG_DATA_SZ; i++) {
            dst[i] = resp[8 + i];
        }
    }

    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int bpmp_init(void)
{
    /* Map hardware addresses */
    g_hsp_base = (volatile uint32_t *)HSP_TOP_BASE;
    g_tx_base = (volatile uint8_t *)BPMP_TX_BASE;
    g_rx_base = (volatile uint8_t *)BPMP_RX_BASE;

    /* Verify HSP is accessible (read doorbell enable) */
    uint32_t db_enable = mmio_read32(hsp_db_reg(HSP_DB_MASTER_CCPLEX, HSP_DB_ENABLE));
    if (db_enable == 0) {
        /* HSP not properly configured by firmware */
        return -1;
    }

    /*
     * After kexec, the IVC channels may be in a corrupted state.
     * Reset the channel counters to force a clean state.
     * BPMP should handle out-of-sync counters gracefully.
     */
    volatile struct ivc_channel_header *tx_h = tx_header();
    volatile struct ivc_channel_header *rx_h = rx_header();

    /* Sync TX channel: set r_count = w_count (mark all messages as read) */
    mmio_write32(&tx_h->r_count, mmio_read32(&tx_h->w_count));

    /* Sync RX channel: set r_count = w_count (mark all messages as read) */
    mmio_write32(&rx_h->r_count, mmio_read32(&rx_h->w_count));

    /* Clear any pending doorbells */
    hsp_ccplex_clear();

    g_bpmp_initialized = true;
    return 0;
}

bool bpmp_is_available(void)
{
    if (!g_bpmp_initialized) {
        return false;
    }

    /* Try MRQ_PING */
    uint32_t challenge = 0x12345678;
    uint32_t response = 0;

    int ret = bpmp_send_mrq(MRQ_PING, &challenge, sizeof(challenge),
                            &response, sizeof(response));
    if (ret != 0) {
        return false;
    }

    /* BPMP should echo the challenge back */
    return (response == challenge);
}

int bpmp_clk_enable(uint32_t clock_id)
{
    if (!g_bpmp_initialized) {
        return -1;
    }

    struct mrq_clk_request req = {0};
    struct mrq_clk_response resp = {0};

    /* Build clock enable request: (CMD_CLK_ENABLE << 24) | clock_id */
    req.cmd_and_id = (CMD_CLK_ENABLE << 24) | (clock_id & 0xFFFFFF);

    int ret = bpmp_send_mrq(MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return resp.result;
}

int bpmp_clk_disable(uint32_t clock_id)
{
    if (!g_bpmp_initialized) {
        return -1;
    }

    struct mrq_clk_request req = {0};
    struct mrq_clk_response resp = {0};

    req.cmd_and_id = (CMD_CLK_DISABLE << 24) | (clock_id & 0xFFFFFF);

    int ret = bpmp_send_mrq(MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return resp.result;
}

int bpmp_reset_assert(uint32_t reset_id)
{
    if (!g_bpmp_initialized) {
        return -1;
    }

    uint32_t req[2] = {CMD_RESET_ASSERT, reset_id};
    int32_t resp = 0;

    int ret = bpmp_send_mrq(MRQ_RESET, req, sizeof(req), &resp, sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return resp;
}

int bpmp_reset_deassert(uint32_t reset_id)
{
    if (!g_bpmp_initialized) {
        return -1;
    }

    uint32_t req[2] = {CMD_RESET_DEASSERT, reset_id};
    int32_t resp = 0;

    int ret = bpmp_send_mrq(MRQ_RESET, req, sizeof(req), &resp, sizeof(resp));
    if (ret != 0) {
        return ret;
    }

    return resp;
}

#else /* !PLATFORM_JETSON_ORIN_NANO */

/* Stub implementations for non-Jetson platforms */

int bpmp_init(void)
{
    return 0;  /* No BPMP on QEMU */
}

bool bpmp_is_available(void)
{
    return false;
}

int bpmp_clk_enable(uint32_t clock_id)
{
    (void)clock_id;
    return 0;  /* Always succeed on QEMU */
}

int bpmp_clk_disable(uint32_t clock_id)
{
    (void)clock_id;
    return 0;
}

int bpmp_reset_assert(uint32_t reset_id)
{
    (void)reset_id;
    return 0;
}

int bpmp_reset_deassert(uint32_t reset_id)
{
    (void)reset_id;
    return 0;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
