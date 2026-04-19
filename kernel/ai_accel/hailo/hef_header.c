/*
 * hef_header.c — Hailo Executable Format outer-header validator.
 *
 * Implements hef_parse_outer_header() against the layout documented
 * in `hailort/libhailort/src/hef/hef_internal.hpp` (lines 92-172)
 * and in `docs/reference/hailo-driver-notes.md` §7.1.
 *
 * All outer fields are big-endian on disk; we read them via explicit
 * byte-shuffle so we don't depend on <arpa/inet.h> or host byte
 * order.
 */

#include "hef_header.h"
#include <string.h>

/* Big-endian loaders (portable across all endian hosts). */
static uint32_t be_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8)
         |  (uint32_t)p[3];
}

static uint64_t be_u64(const uint8_t *p)
{
    return ((uint64_t)be_u32(p) << 32) | (uint64_t)be_u32(p + 4);
}

/*
 * Per-version trailer sizes (bytes immediately following the
 * 12-byte common header).
 * v0: 4 (reserved) + 16 (MD5)                          = 20 bytes
 * v1: 4 (CRC) + 8 (ccws_size) + 4 (reserved)           = 16 bytes
 * v2: 4 (CRC) + 16 (file_hash) + 8 (reserved/padding)
 *     + 4 (more padding)                               = 32 bytes
 * v3: 4 (CRC) + 16 (file_hash) + 8 (ccws_size_ext)
 *     + 16 (additional metadata)                       = 40 bytes
 *
 * V2 trailer confirmed against a hex dump of a DFC 3.33.1 output:
 * proto body starts exactly 44 bytes into the file, i.e. 12 common +
 * 32 trailer. The trailing 12 bytes (after CRC+hash) are zero-padded;
 * we don't know their semantics today and don't need them — they're
 * preserved as "reserved" for layout-size accounting only.
 *
 * V3 is speculative — adjust when a real v3 `.hef` is available.
 */
static size_t trailer_size(uint32_t version)
{
    switch (version) {
    case HEF_VERSION_V0: return 20;
    case HEF_VERSION_V1: return 16;
    case HEF_VERSION_V2: return 32;
    case HEF_VERSION_V3: return 40;
    default:             return 0;
    }
}

/* Common header size (magic + version + proto_size), before the
 * version-specific trailer. */
#define HEF_COMMON_HEADER_SIZE  12

int hef_parse_outer_header(const void *blob, size_t size,
                           struct hef_outer_header *out)
{
    if (!blob || !out) return HEF_ERR_SHORT;
    if (size < HEF_COMMON_HEADER_SIZE) return HEF_ERR_SHORT;

    const uint8_t *p = (const uint8_t *)blob;

    uint32_t magic      = be_u32(p +  0);
    uint32_t version    = be_u32(p +  4);
    uint32_t proto_size = be_u32(p +  8);

    if (magic != HEF_MAGIC)       return HEF_ERR_BAD_MAGIC;
    if (version > HEF_VERSION_MAX) return HEF_ERR_BAD_VERSION;

    size_t tsize = trailer_size(version);
    if (tsize == 0) return HEF_ERR_BAD_VERSION;

    size_t header_total = HEF_COMMON_HEADER_SIZE + tsize;
    if (size < header_total) return HEF_ERR_SHORT;

    /* Sanity-check proto_size: must fit within the remaining blob
     * after the header, and must be non-zero (a `.hef` with zero
     * proto is meaningless). Upper bound is generous — real files
     * are tens of MB. */
    if (proto_size == 0) return HEF_ERR_BAD_SIZE;
    if (proto_size > HEF_PROTO_MAX_SIZE) return HEF_ERR_BAD_SIZE;

    size_t proto_end = header_total + proto_size;
    if (proto_end > size) return HEF_ERR_TRUNCATED;

    memset(out, 0, sizeof(*out));
    out->version      = version;
    out->proto_size   = proto_size;
    out->proto_offset = (uint32_t)header_total;

    /* Version-specific trailer. */
    const uint8_t *t = p + HEF_COMMON_HEADER_SIZE;
    switch (version) {
    case HEF_VERSION_V0:
        /* v0: u32 reserved, u8[16] expected_md5 */
        memcpy(out->md5, t + 4, 16);
        break;

    case HEF_VERSION_V1: {
        /* v1: u32 crc, u64 ccws_size, u32 reserved */
        out->crc       = be_u32(t + 0);
        out->ccws_size = be_u64(t + 4);
        /* CCWS follows the proto body immediately. */
        out->ccws_offset = proto_end;
        /* Overflow-safe: ccws_size is attacker-controlled u64 and
         * the straightforward (offset + size) compare wraps on
         * near-max values. Rearrange to (size - offset) > ccws_size
         * after confirming proto_end <= size (already guaranteed
         * above). */
        if (out->ccws_size > (uint64_t)(size - proto_end)) {
            return HEF_ERR_TRUNCATED;
        }
        break;
    }

    case HEF_VERSION_V2:
    case HEF_VERSION_V3: {
        /* v2/v3 trailer (confirmed against DFC 3.33.1 output):
         *   u32 crc
         *   u8[16] file_hash
         * No CCWS size field in the header — the CCWS block (if any)
         * starts at `proto_end` and runs to end-of-file. Derive the
         * size from the supplied `size` parameter so callers that
         * pass the true file length get a usable ccws_offset +
         * ccws_size without additional parsing.
         *
         * Older Hailo SDKs emitted different v2/v3 layouts (CCWS
         * size embedded in the header, plus padding). If a future
         * decode fails with TRUNCATED here, add per-DFC-version
         * handling keyed on the bit pattern of the hash field.
         */
        out->crc       = be_u32(t + 0);
        memcpy(out->md5, t + 4, 16);            /* file_hash, stored in md5 */
        out->ccws_offset = proto_end;
        out->ccws_size   = (size > proto_end) ? (size - proto_end) : 0;
        break;
    }
    }

    return HEF_OK;
}
