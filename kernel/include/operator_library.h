/*
 * operator_library.h - Packed SASS-kernel library format + parser (#663)
 *
 * The operator library is a single packed binary holding multiple
 * SASS kernel blobs indexed by (op_kind, tier, dtype). Per-model
 * launchers (today: scripts/gpu-kernel-mnist.c; future: the runtime
 * loader from #657) reference operators by their triple instead of
 * bundling individual SASS files. One library binary, many models.
 *
 * Wire format (all little-endian):
 *
 *   [outer header — 24 bytes — same shape as sched_model 'SEMB']
 *     bytes  0..3   magic "OPLB"
 *     bytes  4..5   version       u16  (OPERATOR_LIBRARY_VERSION_V1 = 1)
 *     bytes  6..7   kind_id       u16  (reserved, must be 0)
 *     bytes  8..9   schema_ver    u16  (OPERATOR_LIBRARY_SCHEMA_V1 = 1)
 *     bytes 10..11  reserved      u16  (must be 0)
 *     bytes 12..15  payload_len   u32  (bytes after this header)
 *     bytes 16..19  checksum      u32  (FNV-1a over payload)
 *     bytes 20..23  reserved      u32  (must be 0)
 *
 *   [inner header — 8 bytes — start of payload]
 *     bytes  0..3   op_count      u32
 *     bytes  4..7   reserved      u32  (must be 0)
 *
 *   [op_count × 32-byte entries]
 *     bytes  0..3   op_kind       u32  (enum slm_gpu_op_kind)
 *     bytes  4..7   tier          u32  (SLM_GPU_TIER_*)
 *     bytes  8..11  dtype         u32  (enum slm_gpu_dtype)
 *     bytes 12..15  flags         u32  (reserved, must be 0)
 *     bytes 16..23  sass_offset   u64  (bytes from start of SASS region)
 *     bytes 24..31  sass_size     u64  (bytes)
 *
 *   [SASS region — concatenated kernel blobs]
 *
 * Reusing the 24-byte sched_model outer-header layout (different
 * magic) means a generic blob validator works on both formats. Same
 * applies to the future GPU pipeline blob (#657, magic "GPLB").
 *
 * The parser is read-only and CPU-side. It walks a library blob in
 * memory and exposes a lookup function returning a pointer + length
 * into the SASS region. Uploading SASS to GPU memory is the next
 * layer's responsibility (per-model launcher today; runtime loader
 * #657 once GMMU prereqs #665/#666 land).
 */

#ifndef OPERATOR_LIBRARY_H
#define OPERATOR_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#include "gpu_handoff.h"

#define OPERATOR_LIBRARY_MAGIC          0x424C504Fu  /* "OPLB" little-endian */
#define OPERATOR_LIBRARY_VERSION_V1     1u
#define OPERATOR_LIBRARY_SCHEMA_V1      1u
#define OPERATOR_LIBRARY_OUTER_HEADER_LEN 24u
#define OPERATOR_LIBRARY_INNER_HEADER_LEN 8u
#define OPERATOR_LIBRARY_ENTRY_LEN      32u

/* Parser handle. Populated by operator_library_open(). The pointers
 * inside are non-owning views into the caller's blob buffer; the
 * caller must keep `data` alive for the lifetime of the handle. */
struct operator_library {
    const uint8_t *data;
    size_t         data_len;
    uint32_t       op_count;
    /* Pointer to the first 32-byte entry (start of entries array). */
    const uint8_t *entries;
    /* Pointer to the start of the SASS region. */
    const uint8_t *sass_region;
    size_t         sass_region_len;
};

/* Parser error codes. Negative for the kernel return-int convention. */
#define OPERATOR_LIBRARY_ERR_NULL      (-1)  /* NULL pointer arg          */
#define OPERATOR_LIBRARY_ERR_TRUNC     (-2)  /* shorter than declared     */
#define OPERATOR_LIBRARY_ERR_MAGIC     (-3)  /* magic mismatch            */
#define OPERATOR_LIBRARY_ERR_VERSION   (-4)  /* version / schema mismatch */
#define OPERATOR_LIBRARY_ERR_RESERVED  (-5)  /* reserved field non-zero   */
#define OPERATOR_LIBRARY_ERR_CHECKSUM  (-6)  /* checksum mismatch         */
#define OPERATOR_LIBRARY_ERR_LAYOUT    (-7)  /* offsets out of range      */

/*
 * Parse a packed library blob. Validates the outer header (magic,
 * version, schema, reserved bits, total length, FNV-1a checksum)
 * and the inner header (op_count, reserved). Then walks the entries
 * array to confirm every entry's (sass_offset, sass_size) lies
 * entirely within the declared SASS region.
 *
 * On success returns 0 and *out is populated with non-owning
 * pointers into `data`. On any validation failure returns one of
 * OPERATOR_LIBRARY_ERR_* and *out is left zeroed.
 */
int operator_library_open(struct operator_library *out,
                          const uint8_t *data, size_t len);

/*
 * Look up an entry by (op_kind, tier, dtype). On success, returns 0
 * and *out_sass / *out_size point to a region inside the library
 * blob. On miss, returns -1; *out_sass and *out_size are unchanged.
 *
 * Linear scan — N is small (a few dozen entries even with full op +
 * tier + dtype matrix) so a bsearch isn't worth the indexing cost.
 */
int operator_library_lookup(const struct operator_library *lib,
                            uint32_t op_kind, uint32_t tier, uint32_t dtype,
                            const uint8_t **out_sass, size_t *out_size);

/* Compute the FNV-1a 32-bit checksum used in the outer header. Same
 * algorithm as kernel/sched/ai/runtime_model.c — exposed so tooling
 * (operator_library builder, validator) can reuse it without
 * duplicating the constants. */
uint32_t operator_library_checksum32(const uint8_t *data, size_t len);

#endif /* OPERATOR_LIBRARY_H */
