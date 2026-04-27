/*
 * camrtc.h — Tegra234 Camera RTCPU (RCE) HSP-VM transport + diagnostics.
 *
 * Wraps the camera-RTCPU HSP-VM protocol (CAMRTC_HSP_HELLO /
 * PROTOCOL / RESUME / CH_SETUP / PING) so SLM-OS can talk to the R5
 * camera firmware that's already running in the camera complex post-
 * kexec. This module is the prerequisite for everything camera-RTCPU
 * mediates on Tegra234 — NVCSI configuration, VI single-shot capture,
 * and frame-done notifications all flow through HSP-VM IVC messages
 * sent on top of this transport.
 *
 * Architecture (see `docs/jetson-camera-rtcpu-ivc-driver-notes.md`):
 *
 *   - HSP block at TEGRA234_RCE_HSP_BASE (0x0B950000) — separate
 *     controller from the BPMP HSP. Same `nvidia,tegra186-hsp` layout
 *     SLM-OS already drives for BPMP (common regs + N shared
 *     mailboxes + N shared semaphores), but the protocol on top is
 *     shared-mailbox + shared-semaphore (NOT doorbells).
 *   - Two shared mailboxes form the VM↔RCE channel pair:
 *       SM[0] = VM-TX (CCPLEX → RCE)
 *       SM[1] = VM-RX (RCE → CCPLEX)
 *     Mailbox payload is a 32-bit value: `CAMRTC_HSP_MSG(id, param)`
 *     packing an 8-bit opcode in bits[30:24] (bit 31 = FULL flag) and
 *     a 24-bit parameter in bits[23:0].
 *   - One shared semaphore (SS[0]) carries 16 group-pending bits
 *     (FW→VM) plus 15 group-pending bits (VM→FW). Used later for
 *     IVC ring notifications; not needed for the basic handshake.
 *
 * RCE state inheritance from kexec (verified live on jetson-nano-1,
 * 2026-04-26):
 *   - `0x0B9F0040 (R5_CTRL_0) bit 1 = FWLOADDONE` set → RCE firmware
 *     was loaded by the bootloader and the R5 was released.
 *   - `0x0B9F0020 (PWR_STATUS_0) bit 21 = WFIPIPESTOPPED` set → R5
 *     is in WFI, idle waiting for an HSP message.
 *   - SM[0] / SM[1] / SS[0] all peek cleanly with FULL bit clear
 *     (no stale messages in flight).
 * Linux's kexec `.shutdown` callback for `tegra-camera-rtcpu`
 * sends `CAMRTC_HSP_BYE` to RCE and then asserts `RESET_RCE_ALL`
 * + disables the rce clocks (cached at
 * `docs/reference/l4t-tegra-camera-rtcpu.c:893,1402`). Even though
 * `R5_CTRL_0.FWLOADDONE` stays set, R5 is clock-gated and the
 * HSP-VM ISR is dead. `camrtc_init` re-engages RCE by mirroring
 * `tegra_camrtc_poweron` (RCE clocks on, `RESET_RCE_ALL`
 * deasserted) — the firmware restarts in place from its
 * unzeroed DRAM carveout and HELLO succeeds. Resolves issue #438.
 *
 * Concurrency contract: the camrtc_* APIs are NOT thread-safe.
 * Module-scope statics carry the SM addresses + initialised flag;
 * two CPUs / two tasks calling `camrtc_init` or `camrtc_send_msg`
 * concurrently would race the mailbox state. Today's only caller
 * is the `rcediag` shell command (single-CPU shell context). If a
 * future caller (Lua binding, capture-control pipeline) needs
 * concurrent access, wrap the API calls in a per-bus mutex.
 *
 * Jetson-only (`PLATFORM_JETSON_ORIN_NANO`); other platforms link
 * stubs that return -1 from every function.
 */

#pragma once

#include <stdint.h>

/*
 * Probe RCE state and run the HSP-VM HELLO + PROTOCOL handshake. On
 * success, the RCE firmware is confirmed alive, the protocol version
 * is matched (RTCPU_DRIVER_SM6_VERSION), and the camera HW is
 * activated via RESUME. Subsequent calls are no-ops (idempotent).
 *
 * Returns 0 on success, negative on error:
 *   -1  HSP block not reachable (CBB firewall / bad MMIO mapping)
 *       — peek of HSP_DIMENSIONING returned 0xFFFFFFFF
 *   -2  BPMP MRQ failed during the poweron sequence (clock enable
 *       on `RCE_CPU_NIC` / `RCE_NIC` / `RCE_CPU`, or reset deassert
 *       on `RESET_RCE_ALL`), OR the post-poweron R5_CTRL_0
 *       FWLOADDONE bit is not set (RCE firmware genuinely absent —
 *       e.g. bootloader didn't load it). The WARN log line names
 *       the specific failure.
 *   -3  HELLO timed out (RCE didn't echo the cookie within ~100 ms)
 *   -4  PROTOCOL mismatch (RCE replied with an unsupported version
 *       or RTCPU_FW_INVALID_VERSION = 0xFFFFFF)
 *   -5  RESUME timed out
 */
int camrtc_init(void);

/*
 * Send a CAMRTC_HSP_MSG round-trip. Writes
 * `CAMRTC_HSP_MSG(msg_id, param)` to VM-TX, polls VM-RX until a
 * message with the same `msg_id` arrives or the timeout expires,
 * stores the response's 24-bit parameter in `*resp_param` (may be
 * NULL if caller doesn't care).
 *
 *   msg_id          One of CAMRTC_HSP_PING / FW_HASH / CH_SETUP / etc.
 *   param           24-bit parameter (high 8 bits ignored).
 *   resp_param      Optional: filled with the matching response's
 *                   24-bit parameter.
 *   timeout_us      Max time to wait for the response, in microseconds.
 *
 * Returns 0 on success, -1 on bad arguments (uninitialised),
 * -2 on timeout, -3 if a wrong-msg_id response arrived first
 * (caller must drain stale traffic before retrying).
 */
int camrtc_send_msg(uint32_t msg_id, uint32_t param,
                    uint32_t *resp_param, uint32_t timeout_us);

/*
 * Diagnostic dump: print HSP_DIMENSIONING, R5_CTRL_0 (FWLOADDONE bit),
 * PWR_STATUS_0 (WFIPIPESTOPPED bit), VM-TX SM contents, VM-RX SM
 * contents, and SS[0] value. Used by the `rcediag` shell command.
 * Non-destructive — does not send any HSP messages.
 *
 * Returns 0 always (the dump is best-effort; if any peek faults the
 * exception handler logs the abort and the caller resumes here).
 */
int camrtc_diag_dump(void);
