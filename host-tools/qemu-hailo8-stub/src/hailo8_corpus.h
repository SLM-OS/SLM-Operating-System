/*
 * Hailo RE corpus loader/lookup — pure C, no QEMU deps.
 *
 * Implements the on-disk JSONL schema defined in
 * docs/hailo-re-corpus-format.md. Both the QEMU stub device
 * (hw/misc/hailo8.c) and the standalone unit tests link against this
 * module.
 *
 * Lookup is keyed on the strictly-monotonic `seq` only. The caller
 * performs shape and value comparison against the returned entry so the
 * device model retains policy control (when to halt, what stdout line
 * to emit) without leaking QEMU types into the parser.
 *
 * Anchor: issue #795 Task 0.2.
 */

#ifndef HAILO8_CORPUS_H
#define HAILO8_CORPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAILO_CORPUS_ERRBUF_MIN 256

#define HAILO_CORPUS_MAX_SOURCE  31
#define HAILO_CORPUS_MAX_SHA     63
#define HAILO_CORPUS_MAX_TIME    31
#define HAILO_CORPUS_MAX_NOTE    255

/*
 * Upper bound on seq values accepted by the loader / push paths. The
 * seq_index is an int32 array sized to max_seq; without this cap, a
 * malformed corpus with `seq=10000000000` would request tens of GB of
 * index memory. 16M entries (64 MB index) is well above any realistic
 * single capture session (typical configure() flows are O(10k) ops).
 */
#define HAILO_CORPUS_MAX_SEQ     (1u << 24)

typedef struct hailo_op_entry {
    uint64_t seq;                                /* >= 1 */
    int      bar;                                /* 0, 2, 4 in practice */
    uint64_t offset;
    uint32_t size;                               /* 1, 2, 4, or 8 */
    bool     is_write;                           /* true = "write", false = "read" */
    uint64_t value;                              /* little-endian decoded value */

    bool     msi_after_present;                  /* worktree extension; see README */
    uint32_t msi_after_vector;

    char source[HAILO_CORPUS_MAX_SOURCE + 1];
    char validated_at_commit[HAILO_CORPUS_MAX_SHA + 1];
    char validated_at[HAILO_CORPUS_MAX_TIME + 1];
    char note[HAILO_CORPUS_MAX_NOTE + 1];
} hailo_op_entry_t;

#define HAILO_CORPUS_MAX_PATH    1023
#define HAILO_CORPUS_MAX_KIND    15

/*
 * Upper bound on the bytes-per-region cap, used to reject pathological
 * corpus inputs early. 16 MiB matches Hailo-8 BAR4 — the largest backing
 * artifact we'd ever map (a full firmware blob is ~160 KB; anything
 * approaching the cap is almost certainly malformed).
 */
#define HAILO_CORPUS_MAX_REGION_BYTES  (16ull * 1024 * 1024)

/*
 * Phase 4 region rule — short-circuits per-seq capture for a contiguous BAR
 * range whose bytes come from a known artifact. See
 * docs/hailo-re-corpus-format.md §"Region rule (Phase 4 compression)".
 *
 * Currently only `source.kind="file"` is supported. The file is read in full
 * at load time into `data` (regions are small relative to fw blobs that fit
 * in BAR4 = 16 MiB).
 */
typedef struct hailo_region {
    int       bar;            /* 0, 2, or 4 */
    uint64_t  start;          /* BAR offset, inclusive */
    uint64_t  end;            /* BAR offset, exclusive (half-open) */

    char      source_kind[HAILO_CORPUS_MAX_KIND + 1];   /* "file" */
    char      source_path[HAILO_CORPUS_MAX_PATH + 1];
    uint64_t  source_offset;  /* byte offset within source_path */

    /* Optional seq window — region rule fires only for accesses whose
     * `seq` is in [applies_from_seq, applies_to_seq], inclusive. Lets
     * multiple regions on the same BAR with overlapping offset ranges
     * coexist as long as their seq windows are disjoint (e.g. multi-pass
     * firmware uploads that re-use the same BAR window for different
     * bytes). Defaults: applies_from_seq=1, applies_to_seq=UINT64_MAX
     * (i.e. no seq filter — original Phase 4 behavior). */
    uint64_t  applies_from_seq;
    uint64_t  applies_to_seq;

    /* Loaded artifact bytes. Owned by the region; freed in
     * hailo_corpus_free. `data[i]` corresponds to BAR offset
     * `start + i` (after subtracting source_offset). */
    uint8_t  *data;
    size_t    data_size;      /* exactly end - start */

    char      validated_at_commit[HAILO_CORPUS_MAX_SHA + 1];
    char      validated_at[HAILO_CORPUS_MAX_TIME + 1];
} hailo_region_t;

typedef struct hailo_corpus {
    hailo_op_entry_t *entries;
    size_t            n_entries;
    size_t            cap_entries;

    /* O(1) seq → entries[] index. -1 sentinel for "no entry at this seq". */
    int32_t *seq_index;
    size_t   seq_index_cap;
    uint64_t max_seq;

    /* Phase 4 region rules. Linear search on lookup; typical corpus has
     * O(1)-O(10) regions, so a tree would be overkill. */
    hailo_region_t *regions;
    size_t          n_regions;
    size_t          cap_regions;

    /* Header fields (informational). */
    int  format_version;
    char hailort_version[32];
    char fw_version[32];
    char capture_host[64];

    /* Append handle — non-NULL when the corpus was opened writable.
     * Owns the FILE*; closed by hailo_corpus_free. */
    char  path[1024];
    FILE *append_fp;
} hailo_corpus_t;

/*
 * Load a corpus from disk. Lines that fail to parse cause the load to fail
 * — the corpus is the load-bearing contract; we don't paper over bad data.
 *
 * `path` is required. If `append_writable` is true, the file is also opened
 * for append so hailo_corpus_append_write() can extend it. Pass false from
 * unit tests that should not mutate fixture data.
 *
 * Returns NULL on error and writes a short reason to errbuf.
 */
hailo_corpus_t *hailo_corpus_load(const char *path,
                                  bool append_writable,
                                  char *errbuf, size_t errlen);

/*
 * Build an empty in-memory corpus — no header, no entries. Useful for unit
 * tests that want to drive the lookup API without touching the filesystem.
 */
hailo_corpus_t *hailo_corpus_new_memory(void);

void hailo_corpus_free(hailo_corpus_t *c);

/*
 * Return the entry at `seq`, or NULL if none. seq must be >= 1.
 */
const hailo_op_entry_t *hailo_corpus_get(const hailo_corpus_t *c, uint64_t seq);

/*
 * Phase 4 region lookup. Returns the region covering the half-open range
 * `[offset, offset + size)` on `bar` AND whose seq window includes `seq`,
 * or NULL if no such region exists. The covering region must contain the
 * ENTIRE access width — a partial overlap counts as NOT covered, since
 * serving a fraction of an access from the artifact and the rest from …
 * nowhere is incoherent. The seq filter lets multiple regions on the
 * same BAR cover the same offset range across disjoint seq windows
 * (e.g. multi-pass firmware uploads).
 */
const hailo_region_t *hailo_corpus_region_lookup(const hailo_corpus_t *c,
                                                 int bar, uint64_t offset,
                                                 uint32_t size, uint64_t seq);

/*
 * Extract `size` bytes from the region's loaded artifact starting at BAR
 * offset `bar_offset`, decode as a little-endian unsigned integer (matching
 * the corpus value semantics), and return via *out_value. Returns 0 on
 * success, -1 if `(bar_offset, size)` is not fully inside `[rgn->start,
 * rgn->end)`. Caller is expected to have already validated coverage via
 * hailo_corpus_region_lookup; this is a belt-and-suspenders check.
 */
int hailo_region_get_value(const hailo_region_t *rgn,
                           uint64_t bar_offset, uint32_t size,
                           uint64_t *out_value);

/*
 * Append a freshly-captured write entry. Writes one JSONL line to the open
 * append handle (with fflush) and updates the in-memory index. Caller is
 * expected to verify there is no existing entry at `seq` before calling.
 *
 * Returns 0 on success, -1 on failure with reason in errbuf.
 */
int hailo_corpus_append_write(hailo_corpus_t *c,
                              uint64_t seq, int bar,
                              uint64_t offset, uint32_t size,
                              uint64_t value,
                              char *errbuf, size_t errlen);

/*
 * In-memory variant of append_write used by tests — same effect on the
 * index, no file I/O. Returns 0 / -1 like append_write.
 */
int hailo_corpus_inject_entry(hailo_corpus_t *c,
                              const hailo_op_entry_t *entry,
                              char *errbuf, size_t errlen);

/*
 * Encode a uint64_t value of `size` bytes as a little-endian hex string,
 * lowercase, no `0x` prefix, exactly size*2 + 1 bytes including NUL.
 * `out` must have room for size*2 + 1 bytes. Returns 0 on success, -1 on
 * invalid size.
 */
int hailo_value_to_hex(uint64_t value, uint32_t size, char *out, size_t outlen);

/*
 * Decode the inverse of hailo_value_to_hex. Returns 0 on success.
 */
int hailo_value_from_hex(const char *hex, uint32_t size, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HAILO8_CORPUS_H */
