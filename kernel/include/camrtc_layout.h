/*
 * camrtc_layout.h — Tegra234 Camera RTCPU IVC channel geometry.
 *
 * The two IVC channels SLM-OS binds via `CAMRTC_HSP_CH_SETUP`
 * ("capture-control" + "capture") need the same nframes / frame_size
 * / group values to be visible to:
 *
 *   - `kernel/drivers/camrtc/camrtc.c` — to size the CH_SETUP region
 *     and write the TLV array.
 *   - `kernel/drivers/camrtc/camrtc_capture.c` — to drive
 *     `camrtc_ivc_init` for both channels with matching geometry.
 *
 * Without a shared header these constants would be duplicated in
 * both .c files, with no compile-time guarantee that they agree.
 * If they drift, RCE binds the rings with one geometry (per the TLV)
 * but the AP-side `camrtc_ivc_send` / `camrtc_ivc_recv` walks them
 * with another — a silent corruption mode that the hardware-only
 * `csidiag` smoke test wouldn't immediately catch on a small
 * change.
 *
 * Values come from `docs/reference/l4t-tegra234-camera.dtsi`
 * `ivccontrol@3` (capture-control) and `ivccapture@4` (capture).
 * SLM-OS uses 64 frames for the capture ring instead of L4T's 512
 * because we only issue single-shot requests today; growing this
 * when streaming lands is a one-line change here that the region-
 * size `_Static_assert` in `kernel/tests/test_camera.c` will catch
 * if it overflows the 64 KB carveout.
 */

#pragma once

/* Both channels share the same SS[0] notify bit (group=1). */
#define CAMRTC_GROUP_CAPTURE     1u
#define CAMRTC_VERSION           0u

/* "capture-control" — `tegra234-camera.dtsi` ivccontrol@3.
 *   nvidia,frame-count = <64>
 *   nvidia,frame-size  = <320> */
#define CAMRTC_CTRL_GROUP        CAMRTC_GROUP_CAPTURE
#define CAMRTC_CTRL_NFRAMES      64u
#define CAMRTC_CTRL_FRAME_SIZE   320u
#define CAMRTC_CTRL_VERSION      CAMRTC_VERSION

/* "capture" — `tegra234-camera.dtsi` ivccapture@4.
 *   nvidia,frame-count = <64>   (L4T uses 512 — see header comment)
 *   nvidia,frame-size  = <64> */
#define CAMRTC_CAP_GROUP         CAMRTC_GROUP_CAPTURE
#define CAMRTC_CAP_NFRAMES       64u
#define CAMRTC_CAP_FRAME_SIZE    64u
#define CAMRTC_CAP_VERSION       CAMRTC_VERSION
