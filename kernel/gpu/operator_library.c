/*
 * operator_library.c - Parser for the packed operator-library format (#663)
 *
 * Read-only validator + lookup. The parser does not allocate; it
 * populates an `operator_library` handle with non-owning pointers
 * into the caller's blob buffer. See operator_library.h for the
 * wire format.
 *
 * Validation order (matches sched_model_parse_dense_blob in
 * runtime_model.c — same defensive sequence the in-tree blob
 * formats already use):
 *
 *   1. NULL / minimum-length checks
 *   2. Magic / version / schema / reserved-bit checks
 *   3. payload_len matches declared total
 *   4. FNV-1a checksum over payload
 *   5. Inner header op_count + reserved
 *   6. Per-entry layout check (sass_offset + sass_size <= sass_region_len)
 *
 * Any failure returns the most specific OPERATOR_LIBRARY_ERR_* and
 * leaves the output handle zeroed so callers can branch on rc and
 * trust *out either fully or not at all.
 */

#include "operator_library.h"

#include "string.h"

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p)
{
    return (uint64_t)read_u32_le(p)
        | ((uint64_t)read_u32_le(p + 4) << 32);
}

uint32_t operator_library_checksum32(const uint8_t *data, size_t len)
{
    /* FNV-1a, same constants and order as
     * runtime_model.c::checksum32. Pinned in operator_library.h
     * so the Python builder script and any future C-side validator
     * agree bit-for-bit. */
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

int operator_library_open(struct operator_library *out,
                          const uint8_t *data, size_t len)
{
    if (!out || !data) return OPERATOR_LIBRARY_ERR_NULL;
    memset(out, 0, sizeof(*out));

    if (len < OPERATOR_LIBRARY_OUTER_HEADER_LEN) {
        return OPERATOR_LIBRARY_ERR_TRUNC;
    }

    /* Outer header. */
    if (read_u32_le(data) != OPERATOR_LIBRARY_MAGIC) {
        return OPERATOR_LIBRARY_ERR_MAGIC;
    }
    if (read_u16_le(data + 4) != OPERATOR_LIBRARY_VERSION_V1) {
        return OPERATOR_LIBRARY_ERR_VERSION;
    }
    /* kind_id (bytes 6..7) is reserved for the library format —
     * must be 0. Future layouts may repurpose it for a per-blob
     * discriminator (e.g. "this library targets sm_87 only") but
     * that's a schema-version bump, not a quiet repurpose. */
    if (read_u16_le(data + 6) != 0) {
        return OPERATOR_LIBRARY_ERR_RESERVED;
    }
    if (read_u16_le(data + 8) != OPERATOR_LIBRARY_SCHEMA_V1) {
        return OPERATOR_LIBRARY_ERR_VERSION;
    }
    if (read_u16_le(data + 10) != 0) {
        return OPERATOR_LIBRARY_ERR_RESERVED;
    }
    uint32_t payload_len = read_u32_le(data + 12);
    uint32_t checksum    = read_u32_le(data + 16);
    if (read_u32_le(data + 20) != 0) {
        return OPERATOR_LIBRARY_ERR_RESERVED;
    }
    if (len != (size_t)OPERATOR_LIBRARY_OUTER_HEADER_LEN + payload_len) {
        return OPERATOR_LIBRARY_ERR_TRUNC;
    }

    const uint8_t *payload = data + OPERATOR_LIBRARY_OUTER_HEADER_LEN;
    if (operator_library_checksum32(payload, payload_len) != checksum) {
        return OPERATOR_LIBRARY_ERR_CHECKSUM;
    }

    /* Inner header. */
    if (payload_len < OPERATOR_LIBRARY_INNER_HEADER_LEN) {
        return OPERATOR_LIBRARY_ERR_TRUNC;
    }
    uint32_t op_count = read_u32_le(payload);
    if (read_u32_le(payload + 4) != 0) {
        return OPERATOR_LIBRARY_ERR_RESERVED;
    }

    /* Entries array + SASS region layout. */
    size_t entries_bytes = (size_t)op_count * OPERATOR_LIBRARY_ENTRY_LEN;
    size_t header_total  = (size_t)OPERATOR_LIBRARY_INNER_HEADER_LEN
                         + entries_bytes;
    if (header_total > payload_len) {
        return OPERATOR_LIBRARY_ERR_LAYOUT;
    }

    const uint8_t *entries     = payload + OPERATOR_LIBRARY_INNER_HEADER_LEN;
    const uint8_t *sass_region = entries + entries_bytes;
    size_t         sass_region_len = payload_len - header_total;

    /* Per-entry bounds check. Catches malformed builders that
     * declared a SASS region too small for one of the entries. */
    for (uint32_t i = 0; i < op_count; i++) {
        const uint8_t *e = entries + (size_t)i * OPERATOR_LIBRARY_ENTRY_LEN;
        /* bytes 12..15 (flags) reserved-must-be-zero in v1. */
        if (read_u32_le(e + 12) != 0) {
            return OPERATOR_LIBRARY_ERR_RESERVED;
        }
        uint64_t sass_offset = read_u64_le(e + 16);
        uint64_t sass_size   = read_u64_le(e + 24);
        /* Defensive: sass_offset + sass_size must not overflow
         * size_t and must lie within the SASS region. */
        if (sass_offset > sass_region_len) {
            return OPERATOR_LIBRARY_ERR_LAYOUT;
        }
        if (sass_size > sass_region_len - (size_t)sass_offset) {
            return OPERATOR_LIBRARY_ERR_LAYOUT;
        }
    }

    out->data            = data;
    out->data_len        = len;
    out->op_count        = op_count;
    out->entries         = entries;
    out->sass_region     = sass_region;
    out->sass_region_len = sass_region_len;
    return 0;
}

int operator_library_lookup(const struct operator_library *lib,
                            uint32_t op_kind, uint32_t tier, uint32_t dtype,
                            const uint8_t **out_sass, size_t *out_size)
{
    if (!lib || !lib->entries) return -1;

    for (uint32_t i = 0; i < lib->op_count; i++) {
        const uint8_t *e = lib->entries
                         + (size_t)i * OPERATOR_LIBRARY_ENTRY_LEN;
        if (read_u32_le(e)     != op_kind) continue;
        if (read_u32_le(e + 4) != tier)    continue;
        if (read_u32_le(e + 8) != dtype)   continue;

        uint64_t sass_offset = read_u64_le(e + 16);
        uint64_t sass_size   = read_u64_le(e + 24);
        if (out_sass) *out_sass = lib->sass_region + sass_offset;
        if (out_size) *out_size = (size_t)sass_size;
        return 0;
    }
    return -1;
}
