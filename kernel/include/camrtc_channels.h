/*
 * camrtc_channels.h — Tegra234 Camera RTCPU IVC channel-setup ABI.
 *
 * Direct port of the subset SLM-OS needs from
 * `docs/reference/l4t-camrtc-channels.h`. The TLV struct layout, tag
 * value, and channel error codes are wire-format ABI between the AP
 * and the RCE firmware — they cannot be reordered or have their
 * sizes changed without breaking the protocol.
 *
 * SLM-OS uses these to construct a `CAMRTC_HSP_CH_SETUP` config
 * block: an array of `struct camrtc_tlv_ivc_setup` entries describing
 * each (group, service) channel, terminated by a zero-tag entry.
 * The block lives at the start of a DRAM region; SLM-OS sends the
 * region's physical address (>> 8) as the CH_SETUP message param,
 * and RCE walks the TLVs to bind each channel's IVC rings.
 */

#pragma once

#include <stdint.h>

/* CAMRTC_TAG_IVC_SETUP — packed 'IVC-SETU' as a u64 little-endian.
 * Wire-equal to the L4T `CAMRTC_TAG64('I','V','C','-','S','E','T','U')`
 * macro at `docs/reference/l4t-camrtc-channels.h:30`. Pre-computed
 * here so the `_Static_assert` in test_camera.c can pin it.
 * Byte-by-byte: I=0x49 V=0x56 C=0x43 -=0x2D S=0x53 E=0x45 T=0x54 U=0x55. */
#define CAMRTC_TAG_IVC_SETUP    ((uint64_t)0x55544553ULL << 32 | \
                                 (uint64_t)0x2D435649ULL)

/*
 * One IVC channel description in the CH_SETUP config block. Layout
 * is wire-format ABI — 80 bytes total, no padding manipulation
 * allowed. Fields in order:
 *
 *   tag             8 B  CAMRTC_TAG_IVC_SETUP per entry, 0 to terminate
 *   len             8 B  sizeof(struct) — RCE uses this to skip
 *                        unrecognized TLVs forward-compat
 *   rx_iova         8 B  Phys/IOVA of the RX queue (RCE → AP)
 *   rx_frame_size   4 B  Bytes per frame (multiple of TEGRA_IVC_ALIGN)
 *   rx_nframes      4 B  Frame count (power of two)
 *   tx_iova         8 B  Phys/IOVA of the TX queue (AP → RCE)
 *   tx_frame_size   4 B  Bytes per frame
 *   tx_nframes      4 B  Frame count
 *   channel_group   4 B  Bit position in the SS[0] group mask (0..14)
 *   ivc_version     4 B  Usually 0 for the camera-rtcpu protocol
 *   ivc_service[32] 32 B Channel name ("capture-control", "capture")
 */
struct camrtc_tlv_ivc_setup {
    uint64_t tag;
    uint64_t len;
    uint64_t rx_iova;
    uint32_t rx_frame_size;
    uint32_t rx_nframes;
    uint64_t tx_iova;
    uint32_t tx_frame_size;
    uint32_t tx_nframes;
    uint32_t channel_group;
    uint32_t ivc_version;
    char     ivc_service[32];
};

/* CH_SETUP response status codes (RCE → AP, in the response param). */
#define RTCPU_CH_SUCCESS            0u
#define RTCPU_CH_ERR_NO_SERVICE     128u
#define RTCPU_CH_ERR_ALREADY        129u
#define RTCPU_CH_ERR_UNKNOWN_TAG    130u
#define RTCPU_CH_ERR_INVALID_IOVA   131u
#define RTCPU_CH_ERR_INVALID_PARAM  132u

/*
 * The region's first 4096 bytes hold the TLV array (per L4T
 * `CAMRTC_IVC_CONFIG_SIZE`). The IVC ring buffers start at this
 * offset. An array of `(4096 - 8) / sizeof(struct camrtc_tlv_ivc_setup)`
 * entries fits in the config area before the terminator (≈ 51
 * channels), but practical SLM-OS configurations use 1-2.
 */
#define CAMRTC_IVC_CONFIG_SIZE      4096u

/*
 * tegra-ivc ring header: 64-byte tx half (count + state) followed by
 * 64-byte rx half (count). Frames live immediately after the header,
 * `frame_size`-sized, `nframes` of them. Total queue size =
 * 128 + nframes*frame_size, rounded to TEGRA_IVC_ALIGN (already
 * 128-byte aligned by construction since 128 = 2 * TEGRA_IVC_ALIGN).
 */
#define TEGRA_IVC_ALIGN             64u
#define TEGRA_IVC_HEADER_SIZE       (2u * TEGRA_IVC_ALIGN)  /* 128 */
