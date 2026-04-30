/*
 * hef_header.h — Hailo Executable Format outer-header parser.
 *
 * A `.hef` file has two parts (../slmos-reference-cache/derivatives/notes/hailo-driver-notes.md §7):
 *
 *   1. Flat outer header: magic (`0x01484546` = "HEF\x01"), a
 *      version tag, the protobuf body size, and a version-specific
 *      trailer (MD5 for v0, CRC + CCWS size for v1, ...). All outer
 *      fields are **big-endian** — userspace reads them with htonl().
 *
 *   2. Protobuf body of `hef_proto_size` bytes (top-level message
 *      `ProtoHEFHef`). Parsed with nanopb via `kernel/lib/nanopb/`.
 *
 *   3. (v1+) CCWS block: flat binary weights payload, located at
 *      (hef_proto_size + padding) after the proto body.
 *
 * This header covers the flat part only. The protobuf decode is
 * handled separately by `hef_parser.c` once hef.pb.c/h are
 * generated from hef.proto. Keeping them split means we can
 * validate the outer header — rejecting a truncated or
 * version-mismatched file — before invoking nanopb.
 *
 * Reference: hef.cpp:479-523 in hailort, cached as
 * ../slmos-reference-cache/hailo/hailo-hef-parser-head.cpp (partial).
 */

#ifndef AI_ACCEL_HEF_HEADER_H
#define AI_ACCEL_HEF_HEADER_H

#include <stdint.h>
#include <stddef.h>

/* Magic constant — `'\x01', 'H', 'E', 'F'` in network byte order.
 * Value read back as a host u32 (after ntohl) matches
 * `0x01484546`. */
#define HEF_MAGIC              0x01484546u

/* Supported versions. hailo-hef-internal.hpp lines 119-144 define
 * per-version trailers. */
#define HEF_VERSION_V0         0
#define HEF_VERSION_V1         1
#define HEF_VERSION_V2         2
#define HEF_VERSION_V3         3
#define HEF_VERSION_MAX        3

/* Upper bound on proto body size — defense against a non-.hef file
 * that happens to pass the magic test. Real files are tens of MB;
 * 256 MB is a generous ceiling that leaves headroom for future
 * models without letting a corrupted size field trigger a huge
 * bogus allocation or pointer-arithmetic overflow downstream. */
#define HEF_PROTO_MAX_SIZE     0x10000000u   /* 256 MB */

/* Outer-header error codes (separate from hailo core so callers
 * can distinguish "bad file" from "device IO"). */
#define HEF_OK                 0
#define HEF_ERR_SHORT         (-1)    /* blob < header size */
#define HEF_ERR_BAD_MAGIC     (-2)
#define HEF_ERR_BAD_VERSION   (-3)
#define HEF_ERR_BAD_SIZE      (-4)    /* proto_size or ccws_size oob */
#define HEF_ERR_TRUNCATED     (-5)    /* blob too small for declared body */

/*
 * Decoded outer header — values already byte-swapped to host order.
 * A caller that wants raw bytes should re-read from the blob; we
 * keep only what the parser needs downstream.
 */
struct hef_outer_header {
    uint32_t version;
    uint32_t proto_size;     /* length of the protobuf body in bytes */
    uint64_t ccws_size;      /* v1+: CCWS block size (0 on v0) */
    uint32_t crc;            /* v1+: CRC over header+proto (0 on v0) */
    uint8_t  md5[16];        /* v0: expected-MD5 of proto body */

    /* Byte offset of the protobuf body from the start of the blob.
     * The body is bracketed by [proto_offset, proto_offset + proto_size). */
    uint32_t proto_offset;

    /* Byte offset of the CCWS block (0 if v0 or no CCWS). */
    uint64_t ccws_offset;
};

/*
 * Parse and validate the outer header. Does not touch the protobuf
 * body. Returns HEF_OK and populates *out on success, or a negative
 * error otherwise.
 *
 * @blob: pointer to the start of the `.hef` file in memory.
 * @size: total size of the blob (must cover header + proto + ccws
 *        if v1+ otherwise just header + proto).
 */
int hef_parse_outer_header(const void *blob, size_t size,
                           struct hef_outer_header *out);

#endif /* AI_ACCEL_HEF_HEADER_H */
