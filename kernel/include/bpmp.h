/*
 * bpmp.h - BPMP (Boot and Power Management Processor) driver for Tegra234
 *
 * Provides minimal IPC with BPMP for clock and reset control.
 * Uses HSP (Hardware Synchronization Primitives) doorbell and
 * IVC (Inter-VM Communication) protocol over shared SRAM.
 *
 * References:
 * - Linux kernel: drivers/firmware/tegra/bpmp.c
 * - Linux kernel: drivers/mailbox/tegra-hsp.c
 * - Linux kernel: include/soc/tegra/bpmp-abi.h
 */

#ifndef BPMP_H
#define BPMP_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * MRQ (Message Request) Commands
 * ============================================================================ */

/* MRQ command types */
#define MRQ_PING        0
#define MRQ_QUERY_TAG   1
#define MRQ_CLK         22
#define MRQ_RESET       20

/* MRQ_CLK sub-commands */
#define CMD_CLK_GET_RATE        1
#define CMD_CLK_SET_RATE        2
#define CMD_CLK_ROUND_RATE      3
#define CMD_CLK_GET_PARENT      4
#define CMD_CLK_SET_PARENT      5
#define CMD_CLK_IS_ENABLED      6
#define CMD_CLK_ENABLE          7
#define CMD_CLK_DISABLE         8
#define CMD_CLK_GET_ALL_INFO    14
#define CMD_CLK_GET_MAX_CLK_ID  15

/* MRQ_RESET sub-commands */
#define CMD_RESET_ASSERT        1
#define CMD_RESET_DEASSERT      2
#define CMD_RESET_MODULE        3

/* ============================================================================
 * IVC Message Structure
 *
 * The IVC channel uses a simple request/response format.
 * TX buffer: CPU writes request, BPMP reads
 * RX buffer: BPMP writes response, CPU reads
 * ============================================================================ */

/* IVC frame header (at start of each message) */
struct ivc_frame_header {
    uint32_t mrq;           /* MRQ command type */
    uint32_t flags;         /* Message flags */
};

/* MRQ_CLK request format */
struct mrq_clk_request {
    uint32_t cmd_and_id;    /* (cmd << 24) | clock_id */
    uint32_t reserved[3];   /* Padding for alignment */
};

/* MRQ_CLK response format */
struct mrq_clk_response {
    int32_t result;         /* 0 = success, negative = error */
    uint32_t reserved[3];
};

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Initialize BPMP communication.
 * Sets up HSP doorbell and IVC channels.
 * Returns 0 on success, negative on error.
 */
int bpmp_init(void);

/*
 * Check if BPMP is available and responding.
 * Uses MRQ_PING to verify communication.
 * Returns true if BPMP is available.
 */
bool bpmp_is_available(void);

/*
 * Enable a clock via BPMP.
 * clock_id: The Tegra234 clock ID (e.g., TEGRA234_CLK_UARTA = 155)
 * Returns 0 on success, negative on error.
 */
int bpmp_clk_enable(uint32_t clock_id);

/*
 * Disable a clock via BPMP.
 * Returns 0 on success, negative on error.
 */
int bpmp_clk_disable(uint32_t clock_id);

/*
 * Assert a reset via BPMP.
 * reset_id: The Tegra234 reset ID
 * Returns 0 on success, negative on error.
 */
int bpmp_reset_assert(uint32_t reset_id);

/*
 * Deassert a reset via BPMP.
 * Returns 0 on success, negative on error.
 */
int bpmp_reset_deassert(uint32_t reset_id);

#endif /* BPMP_H */
