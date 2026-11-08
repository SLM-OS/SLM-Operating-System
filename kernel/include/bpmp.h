/*
 * bpmp.h - Public API for Tegra234 BPMP IPC
 *
 * Provides clock and reset control via MRQ (Message Request) to the
 * BPMP R5 firmware running on Tegra234's boot/power management
 * processor. The BPMP survives kexec transitions because it's a
 * separate CPU — SLM-OS's IPC client reconnects via an IVC handshake
 * (Sync → Ack → Established) over shared SYSRAM + HSP doorbell.
 *
 * Implementation: kernel/drivers/bpmp/{hsp,ivc,mrq,bpmp}.c — one file
 * per layer. Clients only need this header.
 *
 * References:
 *   docs/archive/plans/jetson-bpmp-ipc-plan.md            SLM-OS port design
 *   ~/slmos-ref/linux/linux-bpmp-abi.h         MRQ opcodes + payload formats
 *   ~/slmos-ref/tegra-l4t/edk2-nvidia-bpmpipc*     UEFI port target
 */

#ifndef BPMP_H
#define BPMP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * MRQ opcodes and MRQ_CLK / MRQ_RESET sub-command values are mirrored
 * from the private mrq.h so shell diagnostics + callers outside the
 * bpmp/ subdirectory can build requests without needing an implementation
 * header. If you add a new MRQ wrapper, update both places.
 */
#define MRQ_PING                0
#define MRQ_QUERY_TAG           1
#define MRQ_CLK                 22
#define MRQ_RESET               20

#define CMD_CLK_GET_RATE        1
#define CMD_CLK_SET_RATE        2
#define CMD_CLK_ROUND_RATE      3
#define CMD_CLK_GET_PARENT      4
#define CMD_CLK_SET_PARENT      5
#define CMD_CLK_IS_ENABLED      6
#define CMD_CLK_ENABLE          7
#define CMD_CLK_DISABLE         8

#define CMD_RESET_ASSERT        1
#define CMD_RESET_DEASSERT      2
#define CMD_RESET_MODULE        3

/*
 * Initialise the BPMP IPC stack. Requires:
 *   - HSP_TOP_BASE mapped as Device MMIO (already set up by vmm_init
 *     on Jetson via the TCU_RX_MBOX mapping).
 *   - BPMP_TX_BASE / BPMP_RX_BASE mapped as Normal Non-Cacheable (new
 *     mapping added in kernel/mm/vmm.c for Jetson).
 *
 * Blocks for up to 500 ms during the IVC handshake. Returns 0 on
 * success; negative on any failure (HSP read returns 0xffffffff, peer
 * stuck in Sync, etc.).
 *
 * Idempotent: a second call returns 0 without re-running the handshake.
 *
 * Concurrency note for all functions below: internally serialised by
 * a spinlock held for the full MRQ round-trip (commit → ring → poll →
 * consume). Safe to call from any task context on any CPU. Do NOT
 * call from an ISR — MRQs typically round-trip in well under 1 ms
 * (MRQ_PING is microseconds, MRQ_CLK/RESET/PG a few ms at most), but
 * the watchdog timeout is 100 ms and IRQs stay disabled for the full
 * critical section.
 */
int bpmp_init(void);

/* Round-trip an MRQ_PING. True iff BPMP replies with err=0. */
bool bpmp_is_available(void);

/*
 * Clock control wrappers. clock_id values are from
 * include/dt-bindings/clock/tegra234-clock.h (a subset is defined in
 * kernel/include/platform.h). All return 0 on success, or:
 *   negative  transport error (mrq_send couldn't complete)
 *   positive  nonzero BPMP-side err code (from mrq_response.err)
 */
int bpmp_clk_enable(uint32_t clock_id);
int bpmp_clk_disable(uint32_t clock_id);

/*
 * Query whether a clock is currently enabled (CMD_CLK_IS_ENABLED).
 *   clock_id   Tegra234 clock ID.
 *   state_out  Filled with 0 (disabled) or 1 (enabled) on success.
 * Returns 0 on success, or the same error convention as bpmp_clk_enable.
 */
int bpmp_clk_is_enabled(uint32_t clock_id, int *state_out);

/*
 * Ask the bandwidth manager for a memory clock floor on behalf of a
 * client (TEGRA_ICC_BPMP_*), MRQ_BWMGR_INT. *rate_hz_out gets the memory
 * clock BPMP set. Same return convention as bpmp_clk_enable.
 */
#define BPMP_BWMGR_CLIENT_GPU 5u  /* TEGRA_ICC_BPMP_GPU */
int bpmp_emc_floor_khz(uint32_t client_id, uint32_t floor_khz, uint64_t *rate_hz_out);

/*
 * Set a clock's rate in Hz (CMD_CLK_SET_RATE). BPMP rounds to the
 * closest hardware-supported rate; the actual rate it programmed is
 * returned via *actual_hz_out (may be NULL if caller doesn't care).
 *
 *   clock_id        Tegra234 clock ID.
 *   rate_hz         Requested rate in Hz (e.g. 24000000 for 24 MHz).
 *   actual_hz_out   Optional: filled with the rate BPMP actually
 *                   programmed (the closest supported value).
 *
 * Returns 0 on success, or the same error convention as
 * bpmp_clk_enable. The clock must be enabled separately via
 * bpmp_clk_enable; SET_RATE doesn't auto-enable.
 */
int bpmp_clk_set_rate(uint32_t clock_id, uint64_t rate_hz,
                      uint64_t *actual_hz_out);

/* The clock's current rate in Hz. 0 on success. */
int bpmp_clk_get_rate(uint32_t clock_id, uint64_t *rate_hz_out);

/*
 * Reset control wrappers. Same error convention as bpmp_clk_*.
 */
int bpmp_reset_assert(uint32_t reset_id);
int bpmp_reset_deassert(uint32_t reset_id);

/*
 * MRQ_UPHY / CMD_UPHY_PCIE_CONTROLLER_STATE — power up (or down) the
 * UPHY brick backing a Tegra PCIe controller. Required for PCIe RC
 * bring-up from bare metal; Linux's tegra_pcie_bpmp_set_ctrl_state
 * does exactly this before any APPL/DBI programming.
 *
 *   pcie_controller_id  Tegra234: 0..10. C8 (the Super Dev Kit's
 *                       RTL8168 path) is id=8.
 *   enable              1 to power up, 0 to power down.
 *
 * Returns 0 on success. Negative = transport error; positive = BPMP-side
 * err code from mrq_response.err.
 */
int bpmp_uphy_pcie_controller_state(uint32_t pcie_controller_id,
                                    bool enable);

/*
 * MRQ_PG / CMD_PG_SET_STATE — turn a BPMP-managed power domain on or off.
 *   domain_id  TEGRA234_POWER_DOMAIN_* (e.g. PCIEX4CA = 13 for PCIe C8).
 *   on         true = PG_STATE_ON (1), false = PG_STATE_OFF (0).
 * Returns 0 on success (including "already in that state"), negative
 * on transport failure, positive on BPMP-reported err code.
 */
int bpmp_pg_set_state(uint32_t domain_id, bool on);

/*
 * BPMP thermal zones on Tegra234 (the `thermal-sensors` cells of the
 * L4T device tree's thermal-zones, dt-bindings/thermal/tegra234-bpmp-
 * thermal.h). cv0-cv2 (2-4) read "unavailable" on Orin Nano.
 */
#define TEGRA234_BPMP_THERMAL_ZONE_CPU   0u
#define TEGRA234_BPMP_THERMAL_ZONE_GPU   1u
#define TEGRA234_BPMP_THERMAL_ZONE_SOC0  5u
#define TEGRA234_BPMP_THERMAL_ZONE_SOC1  6u
#define TEGRA234_BPMP_THERMAL_ZONE_SOC2  7u
#define TEGRA234_BPMP_THERMAL_ZONE_TJ    8u   /* hottest junction */

/*
 * MRQ_THERMAL / CMD_THERMAL_GET_TEMP request (linux-bpmp-abi.h
 * struct mrq_thermal_host_to_bpmp_request): the sub-command word and a
 * union whose largest member (set_trip: zone, low, high, enabled) is
 * 16 bytes. Linux sends the whole 20-byte struct; so does SLM-OS.
 */
struct bpmp_thermal_request {
    uint32_t type;
    uint32_t zone;
    uint32_t pad[3];
};

/*
 * MRQ_THERMAL / CMD_THERMAL_GET_TEMP — read a thermal zone (#1093).
 *   zone      TEGRA234_BPMP_THERMAL_ZONE_*.
 *   temp_mc   filled with the temperature in millidegrees Celsius.
 * Returns 0 on success, negative on transport failure (or no BPMP:
 * non-Jetson stub), positive on a BPMP err code (e.g. a zone with no
 * sensor driver).
 */
int bpmp_thermal_get_temp(uint32_t zone, int32_t *temp_mc);

/*
 * Post-request hook (#1161). When set, every public wrapper above that
 * sends a request (clk enable/disable/set_rate, bwmgr floor, reset,
 * uphy, pg, thermal) calls hook(op, id, rc) after the round-trip, outside
 * the MRQ lock. bpmp_clk_get_rate and bpmp_is_available do not call it.
 * The hook may itself call BPMP wrappers; nested calls do not re-enter
 * it. NULL removes it. Boot-time diagnostic: set it only while a single
 * CPU runs.
 */
typedef void (*bpmp_post_op_hook_t)(const char *op, uint32_t id, int rc);
void bpmp_set_post_op_hook(bpmp_post_op_hook_t hook);

#endif /* BPMP_H */
