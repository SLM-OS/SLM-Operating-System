/**
 * test_blob_helpers.h - Shared blob-construction helpers for eviction tests.
 *
 * Both `test_eviction.c` and `test_blob_autoload.c` build SLM-OS managed
 * blobs (outer "SEMB" header + inner payload) to exercise the validator,
 * staging, and autoload paths. Before this header, each file carried
 * its own copy of `fnv1a32`, the outer-blob writer, and the eviction
 * MLP payload writer — three identical helpers in two places, kept in
 * sync only by code review. PR #432 consolidated them here.
 *
 * `static inline` so each TU gets its own copy and no symbol collides;
 * also exempts the helpers from `-Werror=unused-function` when a TU
 * (e.g. a default build of `test_blob_autoload.c` without
 * CONFIG_AI_SCHEDULER) only uses a subset of them.
 */
#ifndef TEST_BLOB_HELPERS_H
#define TEST_BLOB_HELPERS_H

#include "../include/string.h"

#include <stddef.h>
#include <stdint.h>

/* Outer-blob header size written by `build_outer_blob`. Caller buffers
 * size as `BLOB_OUTER_HEADER_BYTES + payload_len`. */
#define BLOB_OUTER_HEADER_BYTES 24

/* Total payload byte count emitted by `build_eviction_mlp_payload`:
 * 8-byte payload header + 4417 fp32 weights = 17676 bytes. The
 * `static_assert` inside the helper pins the math against the layer
 * dimensions; if a future MLP topology widens those dimensions both
 * sides have to move together. */
#define EVICTION_MLP_PAYLOAD_BYTES 17676

static inline uint32_t fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static inline size_t build_outer_blob(uint16_t kind_id, const uint8_t *payload,
                                      size_t payload_len, uint8_t *out,
                                      size_t out_cap)
{
    /* Fires if a maintainer changes the constant without updating the
     * literal byte writes below. Does NOT catch the inverse (adding a
     * new `out[24] = ...` write without bumping the constant) — that
     * case is on the reviewer. */
    static_assert(BLOB_OUTER_HEADER_BYTES == 24,
                  "build_outer_blob writes 24 bytes at out[0..23]; "
                  "any change to BLOB_OUTER_HEADER_BYTES must be "
                  "matched by the writes below");

    uint32_t checksum = fnv1a32(payload, payload_len);
    size_t total = BLOB_OUTER_HEADER_BYTES + payload_len;
    if (out_cap < total) return 0;

    out[0] = 'S'; out[1] = 'E'; out[2] = 'M'; out[3] = 'B';
    out[4] = 1; out[5] = 0;                     /* version */
    out[6] = (uint8_t)(kind_id & 0xFF);
    out[7] = (uint8_t)(kind_id >> 8);
    out[8] = 1; out[9] = 0;                     /* feature schema */
    out[10] = 0; out[11] = 0;                   /* reserved */
    out[12] = (uint8_t)(payload_len & 0xFF);
    out[13] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[14] = (uint8_t)((payload_len >> 16) & 0xFF);
    out[15] = (uint8_t)((payload_len >> 24) & 0xFF);
    out[16] = (uint8_t)(checksum & 0xFF);
    out[17] = (uint8_t)((checksum >> 8) & 0xFF);
    out[18] = (uint8_t)((checksum >> 16) & 0xFF);
    out[19] = (uint8_t)((checksum >> 24) & 0xFF);
    out[20] = 0; out[21] = 0; out[22] = 0; out[23] = 0; /* reserved */
    memcpy(out + BLOB_OUTER_HEADER_BYTES, payload, payload_len);
    return total;
}

static inline size_t build_eviction_mlp_payload(uint32_t out_weight_bits,
                                                uint8_t *out,
                                                size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 8,
        L1_IN = 27,
        L1_OUT = 64,
        L2_OUT = 32,
        L3_OUT = 16,
        OUT_DIM = 1,
        W_L1_LEN = L1_OUT * L1_IN,
        B_L1_LEN = L1_OUT,
        W_L2_LEN = L2_OUT * L1_OUT,
        B_L2_LEN = L2_OUT,
        W_L3_LEN = L3_OUT * L2_OUT,
        B_L3_LEN = L3_OUT,
        W_OUT_LEN = OUT_DIM * L3_OUT,
        B_OUT_LEN = OUT_DIM,
        FLOAT_COUNT = W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN
                    + W_L3_LEN + B_L3_LEN + W_OUT_LEN + B_OUT_LEN,
        TOTAL = PAYLOAD_HEADER_LEN + FLOAT_COUNT * 4
    };
    static_assert(TOTAL == EVICTION_MLP_PAYLOAD_BYTES,
                  "EVICTION_MLP_PAYLOAD_BYTES is out of sync with the "
                  "helper's layer dimensions; both must update together");
    size_t cursor = 0;
    size_t idx = 0;

    if (out_cap < TOTAL) return 0;
    memset(out, 0, TOTAL);
    out[0] = 'M'; out[1] = 'L'; out[2] = 'P'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 0; out[7] = 0;
    cursor = PAYLOAD_HEADER_LEN;

#define WRITE_U32_LE(bits)                                                   \
    do {                                                                     \
        uint32_t bits_ = (bits);                                             \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                           \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                    \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                   \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                   \
        cursor += 4;                                                         \
    } while (0)

    for (idx = 0; idx < FLOAT_COUNT; idx++) {
        uint32_t bits = 0u;
        if (idx == 0) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN) bits = 0x3F800000u;
        if (idx == W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN
                + W_L3_LEN + B_L3_LEN) {
            bits = out_weight_bits;
        }
        WRITE_U32_LE(bits);
    }
#undef WRITE_U32_LE

    return cursor;
}

#endif /* TEST_BLOB_HELPERS_H */
