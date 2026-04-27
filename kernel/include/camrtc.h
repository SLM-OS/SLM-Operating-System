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
 * CAMRTC_HSP_MSG opcode subset that callers outside the driver
 * actually use. Full set (and protocol comments) lives in
 * `docs/reference/l4t-camrtc-commands.h:42-77`. Driver-internal
 * constants for HELLO / PROTOCOL / RESUME stay file-local.
 */
#define CAMRTC_HSP_IRQ            0x00u
#define CAMRTC_HSP_PING           0x45u
#define CAMRTC_HSP_FW_HASH        0x46u
#define CAMRTC_HSP_CH_SETUP       0x44u

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
 * RX messages with opcode < CAMRTC_HSP_HELLO (0x40) — including
 * CAMRTC_HSP_IRQ (0x00) IVC notifications — are unidirectional per
 * the L4T `rtcpu-hsp-combo.c:151-159` contract. They are drained
 * transparently here while we wait for the matching response, so a
 * coincident IRQ notification can't make the call return -3.
 *
 * Returns 0 on success, -1 on bad arguments (uninitialised),
 * -2 on timeout (TX-drain, RX-recv, or RX-recv during stale drain
 * — all three log a `WARN` line),
 * -3 only if RCE replied with a *different* response opcode
 * (>= 0x40, not the requested msg_id) — also `WARN`-logged.
 */
int camrtc_send_msg(uint32_t msg_id, uint32_t param,
                    uint32_t *resp_param, uint32_t timeout_us);

/*
 * Fire-and-forget mailbox send for the unidirectional opcodes
 * (CAMRTC_HSP_IRQ in particular — used by the IVC ring transport
 * to wake RCE after advancing a ring counter). Drains the TX
 * mailbox first so the message lands cleanly, then writes
 * `CAMRTC_HSP_MSG(msg_id, param)` to VM-TX and returns immediately
 * — no response correlation, no RX read.
 *
 * Returns 0 on success, -1 if uninitialised, -2 on TX-drain
 * timeout (also `WARN`-logged).
 */
int camrtc_send_irq(uint32_t msg_id, uint32_t param,
                    uint32_t timeout_us);

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

/*
 * Hardware Task 3 / 4: stand up the IMX219 IVC channel pair.
 * Single CH_SETUP message binds two channels with the same group=1
 * SS[0] notify bit but different ring geometry:
 *
 *   - "capture-control" (`tegra234-camera.dtsi` ivccontrol@3)
 *       64 frames × 320 B — carries CAPTURE_PHY_STREAM_OPEN_REQ /
 *       CAPTURE_CSI_STREAM_SET_CONFIG_REQ / CAPTURE_CHANNEL_SETUP_REQ
 *       and their RESP messages.
 *
 *   - "capture" (`tegra234-camera.dtsi` ivccapture@4)
 *       64 frames × 64 B (L4T uses 512 frames; SLM-OS uses 64 to fit
 *       the existing 64 KB region carveout — single-shot use today)
 *       carries CAPTURE_REQUEST_REQ / CAPTURE_STATUS_IND.
 *
 * What this does:
 *   1. Uses a fixed 64 KB region at 0xBDFE0000 (inside the existing
 *      NC mapping at 0xBDE00000-0xBDFFFFFF) for the CH_SETUP TLV
 *      config block (4 KB) plus four tegra-IVC queues (capture-
 *      control rx + tx + capture rx + tx). Total ~52 KB used.
 *   2. Builds two `camrtc_tlv_ivc_setup` entries (capture-control
 *      then capture) plus a zero-tag terminator at offset 0 of the
 *      region, with rx/tx IOVAs pointing at the queue buffers
 *      later in the region.
 *   3. Zero-initialises both queue headers (count + state).
 *   4. Sends `CAMRTC_HSP_CH_SETUP(region_phys >> 8)` over the
 *      established HSP-VM session and waits for RCE's response.
 *
 * On success, RCE has bound both (group=1, service=...) tuples to
 * the rx/tx ring IOVAs and is ready to read/write frames on either.
 *
 * Pre-condition: `camrtc_init()` has returned 0. The HSP-VM session
 * must be established — CH_SETUP travels over the same mailbox.
 *
 * On Tegra234 post-kexec, Linux disables SMMU translation as part
 * of the handoff, so RCE sees physical addresses directly. The
 * 0xBDFE0000 region is a *physical* address inside RCE's VM1 IOVA
 * aperture (0xA0000000..0xC0000000) by construction — no SMMU
 * programming needed from SLM-OS.
 *
 * Returns 0 on success, negative on error:
 *   -2  HSP-VM CH_SETUP round-trip failed (timeout or wrong msg_id),
 *       OR the function was called before `camrtc_init()` succeeded.
 *   -3  RCE rejected the setup — see WARN log for the
 *       RTCPU_CH_ERR_* code (128..132 per camrtc_channels.h).
 *
 * Idempotent: subsequent calls after a successful one return 0
 * immediately without re-sending CH_SETUP — RCE returns
 * RTCPU_CH_ERR_ALREADY (129) for a duplicate bind, which would
 * make a partial-init recovery in the caller fail permanently.
 *
 * Name kept as `_capture_control` for backward compatibility with the
 * existing `csidiag` shell command and the `camrtc_capture_init`
 * call site; the function actually binds *both* channels.
 */
int camrtc_ch_setup_capture_control(void);

/*
 * Diagnostic accessor: physical address of the CH_SETUP region from
 * the most recent successful `camrtc_ch_setup_capture_control` call,
 * or 0 if not yet attempted / failed. Used by the `rcediag` shell
 * command to print the region's location for follow-up `peek`
 * inspection.
 */
uintptr_t camrtc_ch_setup_region_phys(void);

/*
 * Diagnostic accessors: physical IOVAs of the "capture" channel's
 * rx (RCE→AP) and tx (AP→RCE) ring buffers from the most recent
 * successful `camrtc_ch_setup_capture_control` call, or 0 if not
 * yet attempted / failed. The capture-control IOVAs are derivable
 * from the region base (rx = region + 4096; tx = rx + control
 * queue size); the capture IOVAs follow at offsets that depend on
 * the control queue size, so it's cleaner to expose them directly.
 *
 * Used by `camrtc_capture_init` (in camrtc_capture.c) to drive
 * `camrtc_ivc_init` for the second channel after CH_SETUP succeeds.
 */
uintptr_t camrtc_ch_setup_capture_rx_iova(void);
uintptr_t camrtc_ch_setup_capture_tx_iova(void);
