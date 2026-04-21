/*
 * kernel/drivers/bpmp/mrq.h - MRQ (Message Request) transport (private header)
 *
 * Sits on top of ivc.c (channel framing) and hsp.c (doorbells). Offers
 * one entry point — blocking mrq_send — that wraps the full request→
 * doorbell→wait→consume round-trip.
 *
 * MRQ opcodes and payload structs live here. All opcodes we currently
 * use fit the minimum 120-byte payload limit; the MRQs needed by Step
 * 3 of the PCIe revival plan (MRQ_UPHY, MRQ_POWERGATE, longer MRQ_CLK
 * sub-commands) will extend these definitions.
 */

#ifndef DRIVERS_BPMP_MRQ_H
#define DRIVERS_BPMP_MRQ_H

#include <stdint.h>

/* Opcodes (from docs/reference/linux-bpmp-abi.h). */
#define MRQ_PING        0
#define MRQ_QUERY_TAG   1
#define MRQ_CLK         22
#define MRQ_RESET       20
#define MRQ_UPHY        69
#define MRQ_PG          66

/*
 * MRQ_UPHY sub-commands (linux-bpmp-abi.h enum mrq_uphy_cmd).
 * Only the two commands we need for PCIe controller bring-up are
 * defined here.
 */
#define CMD_UPHY_PCIE_EP_CONTROLLER_PLL_INIT  3
#define CMD_UPHY_PCIE_CONTROLLER_STATE        4
#define CMD_UPHY_PCIE_EP_CONTROLLER_PLL_OFF   5

/* MRQ_PG sub-commands (linux-bpmp-abi.h enum mrq_pg_cmd). */
#define CMD_PG_QUERY_ABI    0
#define CMD_PG_SET_STATE    1
#define CMD_PG_GET_STATE    2

/* pg_states — legal values for MRQ_PG SET_STATE. */
#define PG_STATE_OFF        0
#define PG_STATE_ON         1

/*
 * MRQ_CLK sub-commands. The CLK request format packs (cmd << 24) |
 * clock_id into the first 32-bit word.
 */
#define CMD_CLK_GET_RATE        1
#define CMD_CLK_SET_RATE        2
#define CMD_CLK_ROUND_RATE      3
#define CMD_CLK_GET_PARENT      4
#define CMD_CLK_SET_PARENT      5
#define CMD_CLK_IS_ENABLED      6
#define CMD_CLK_ENABLE          7
#define CMD_CLK_DISABLE         8

/* MRQ_RESET sub-commands. */
#define CMD_RESET_ASSERT        1
#define CMD_RESET_DEASSERT      2
#define CMD_RESET_MODULE        3

/*
 * Initialise the MRQ transport layer. Must be called AFTER hsp_init()
 * and after the BPMP SYSRAM regions are mapped Non-Cacheable.
 *
 *   tx_sram  Kernel VA of the CPU→BPMP IVC channel (4 KB in SYSRAM).
 *   rx_sram  Kernel VA of the BPMP→CPU IVC channel.
 *
 * Runs the IVC handshake. Returns 0 on success.
 */
int mrq_init(uintptr_t tx_sram, uintptr_t rx_sram);

/* True once mrq_init() has returned success. */
bool mrq_is_ready(void);

/*
 * Send an MRQ and block for the response.
 *
 *   mrq        opcode (MRQ_PING / MRQ_CLK / MRQ_RESET / ...)
 *   tx_data    payload bytes (may be NULL if tx_len = 0)
 *   tx_len     bytes in tx_data, must be ≤ IVC_DATA_MAX (120)
 *   rx_data    output buffer (may be NULL if rx_len = 0)
 *   rx_len     max bytes to copy into rx_data
 *   err_out    if non-NULL, filled with the mrq_response.err field
 *              from the wire (maps to BPMP-side error code)
 *
 * Returns:
 *    0         Round-trip succeeded; check *err_out for BPMP-side
 *              success/failure.
 *   -1         Not initialised.
 *   -2         TX channel busy (prior frame not yet acknowledged).
 *   -3         TX commit rejected (typically wrong IVC state).
 *   -4         Timed out waiting for response.
 *   -5         RX consume failed.
 */
int mrq_send(uint32_t mrq,
             const void *tx_data, uint32_t tx_len,
             void *rx_data, uint32_t rx_len,
             int32_t *err_out);

#endif /* DRIVERS_BPMP_MRQ_H */
