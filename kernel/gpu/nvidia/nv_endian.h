/*
 * nv_endian.h — Byte-swap helpers shared across the NVIDIA parsers.
 *
 * Every on-disk NVIDIA format this codebase parses is little-endian:
 *
 *   - Expansion-ROM header, PCIR / NPDS / NPDE records, BIT table,
 *     FALCON_UCODE_DESC (V2 and V3), and the sub-image pointer at
 *     offset 0x18 inside each image (see `nvidia_vbios.c`).
 *   - `nvfw_bin_hdr` / `hs_header_v2` / `load_header` and every field
 *     inside them (see `nvfw.c`).
 *   - FWSEC patch-location tables, WPR2 metadata, and the raw IMEM /
 *     DMEM payloads copied into the Falcon (`bringup.c` writes back
 *     with `wr32le` for the same reason).
 *
 * SLM-OS runs on LE hosts only (x86-64, ARM64 in LE mode). The byte-
 * construction helpers below compile to one or two instructions on
 * either, so the portability cost is zero. A compile-time assertion
 * refuses to build on a big-endian host because `wr32le()` in
 * `bringup.c` and the DMA memcpys that feed raw ucode into Falcon
 * MMIO both assume the host already lays out u32s LE.
 *
 * Issues #160 and #164 asked to consolidate the per-file copies of
 * these helpers into one shared header with the endianness contract
 * stated up-front; this file is that consolidation.
 */

#ifndef GPU_NVIDIA_NV_ENDIAN_H
#define GPU_NVIDIA_NV_ENDIAN_H

#include <stdint.h>

/*
 * Host-endianness contract. See the file header above for rationale.
 * The check is conditional on the __BYTE_ORDER__ macro existing so
 * toolchains that don't define it (rare on our targets) still build.
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
    "NVIDIA parsers assume a little-endian host. The rd16le / rd32le "
    "helpers byte-swap correctly on either, but bringup.c's wr32le() "
    "and the DMA-buffer memcpys write raw ucode payloads assuming the "
    "host is LE. Porting to a BE target requires auditing those sites "
    "before enabling this build.");
#endif

/*
 * Load a little-endian u16 from a raw byte buffer. Safe to call on
 * unaligned pointers — the byte construction is strictly defined and
 * does not rely on the host's native load semantics.
 */
static inline uint16_t nv_rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/*
 * Load a little-endian u32 from a raw byte buffer. Same unaligned-safe
 * semantics as nv_rd16le.
 */
static inline uint32_t nv_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/*
 * Store a u32 to a raw byte buffer in little-endian byte order.
 * Used when building Falcon register writes / patch-location rewrites
 * where the destination is a byte buffer that will be DMAed verbatim
 * into GPU DMEM.
 */
static inline void nv_wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

#endif /* GPU_NVIDIA_NV_ENDIAN_H */
