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
 * Per-version trailer sizes.
 * v0: 4 (reserved) + 16 (MD5) = 20 bytes
 * v1: 4 (CRC) + 8 (ccws_size) + 4 (reserved) = 16 bytes
 * v2: 4 (CRC) + 8 (ccws_size) + 4 (reserved) + 4 (padding_bytes) = 20 bytes
 * v3: 4 (CRC) + 8 (ccws_size_with_padding) + 16 (additional metadata) = 28 bytes
 *
 * (v2/v3 layouts are approximate from the userspace parser — refine
 * when Phase 4 gets a real v2/v3 `.hef` to decode.)
 */
static size_t trailer_size(uint32_t version)
{
    switch (version) {
    case HEF_VERSION_V0: return 20;
    case HEF_VERSION_V1: return 16;
    case HEF_VERSION_V2: return 20;
    case HEF_VERSION_V3: return 28;
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
        /* v2/v3 carry the same `crc` + `ccws_size` prefix; deeper
         * fields (padding length, hef hash) are not needed by the
         * loader today. */
        out->crc       = be_u32(t + 0);
        out->ccws_size = be_u64(t + 4);
        out->ccws_offset = proto_end;
        if (out->ccws_size > (uint64_t)(size - proto_end)) {
            return HEF_ERR_TRUNCATED;
        }
        break;
    }
    }

    return HEF_OK;
}
