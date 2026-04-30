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
 * Values come from `../slmos-reference-cache/tegra-l4t/l4t-tegra234-camera.dtsi`
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

/* IMX219 frame buffer carveout — 4 MB at 0xA1000000.
 *
 * Visible to:
 *   - `kernel/mm/pmm.c` — to split Jetson region 1 around the
 *     carveout so the buddy allocator never hands these pages out.
 *   - `kernel/drivers/camrtc/camrtc.c` — to expose the IOVA via
 *     `camrtc_frame_buffer_iova()` and pin the in-aperture
 *     constraints with `_Static_assert`.
 *
 * Sized for IMX219 binning-mode RAW10 (1640×1232) written by VI5
 * as 16-bit-per-pixel via TEGRA_IMAGE_FORMAT_T_R16 = 1640 × 2 ×
 * 1232 = 4,040,960 bytes round-up to 4 MB. Must lie inside RCE's
 * VM1 IOVA aperture (0xA0000000..0xC0000000) since SMMU bypass
 * post-kexec means IOVA == phys for the camera path. */
#define CAMRTC_FRAME_BUFFER_PHYS  0xA1000000u
#define CAMRTC_FRAME_BUFFER_SIZE  0x00400000u  /* 4 MB */
#define CAMRTC_FRAME_BUFFER_END   (CAMRTC_FRAME_BUFFER_PHYS + CAMRTC_FRAME_BUFFER_SIZE)

/* IMX219 binning-mode dimensions written as T_R16 (16 bpp). */
#define IMX219_BINNED_WIDTH            1640u
#define IMX219_BINNED_HEIGHT           1232u
#define IMX219_BINNED_BYTES_PER_PIXEL  2u            /* T_R16 packs RAW10 into u16 */
#define IMX219_BINNED_STRIDE           (IMX219_BINNED_WIDTH * IMX219_BINNED_BYTES_PER_PIXEL)
#define IMX219_BINNED_FRAME_BYTES      (IMX219_BINNED_STRIDE * IMX219_BINNED_HEIGHT)
