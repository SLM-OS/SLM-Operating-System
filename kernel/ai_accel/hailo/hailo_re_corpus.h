/*
 * hailo_re_corpus.h — JSONL corpus reader for the Hailo BAR4 RE loop
 * (#795 Phase 0 Task 0.4).
 *
 * The corpus is the shared on-disk ledger defined in
 * docs/hailo-re-corpus-format.md. This module parses the subset
 * SLM-OS needs to drive `hailo replay-step <N>`:
 *
 *   - one header line (line 1)
 *   - any number of `type="op"` lines (sequence-ordered BAR R/W)
 *   - an optional `type="trailer"` line at end of stream
 *
 * What this module does NOT do:
 *   - mutate the corpus file (the driver script is the single writer)
 *   - parse `note` strings or other free-form annotations
 *   - validate `validated_at_commit` shape — `hailo replay-step` checks
 *     only that the seq=N entry's value is JSON null
 *   - tolerate values larger than 32 bits — v1 corpus is size=4 only,
 *     and the parser stores `value` as a `uint32_t`. Non-size-4 ops
 *     parse successfully but `replay-step` rejects them at issue time.
 *
 * Hand-rolled scanner because the kernel has no JSON library and the
 * line shapes are constrained. The scanner skips characters inside
 * JSON string literals so a stray `"seq": 7` substring inside a
 * `note` field cannot be confused for the real seq key.
 */

#ifndef AI_ACCEL_HAILO_RE_CORPUS_H
#define AI_ACCEL_HAILO_RE_CORPUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction of a recorded BAR access. Stored as an enum rather than a
 * string so callers can switch over the value cheaply. */
enum hailo_re_dir {
    HAILO_RE_DIR_READ  = 0,
    HAILO_RE_DIR_WRITE = 1,
};

/* One parsed `type="op"` entry. Fields mirror docs/hailo-re-corpus-format.md
 * §"Operation entries"; everything we don't consume at replay time
 * (source, validated_at_commit, validated_at, note) is discarded. */
struct hailo_re_op {
    uint32_t seq;
    uint32_t offset;
    uint32_t value;            /* size=4 LE-decoded; 0 for unparsed widths */
    uint8_t  bar;              /* 0, 2, or 4 (Hailo-8 BAR layout) */
    uint8_t  size;             /* 1/2/4/8 — only 4 is replayable today */
    uint8_t  dir;              /* hailo_re_dir */
    uint8_t  validated;        /* 1 if validated_at_commit was a non-null string */
};

enum hailo_re_corpus_error {
    HAILO_RE_CORPUS_OK            =  0,
    HAILO_RE_CORPUS_E_PARSE       = -1,   /* malformed JSON line shape */
    HAILO_RE_CORPUS_E_NO_HEADER   = -2,   /* line 1 missing/wrong type */
    HAILO_RE_CORPUS_E_NONMONOTONIC = -3,  /* seq did not strictly increase */
    HAILO_RE_CORPUS_E_OVERFLOW    = -4,   /* op_capacity exhausted */
    HAILO_RE_CORPUS_E_BAD_FIELD   = -5,   /* required field missing or bad value */
};

/* Parsed corpus state. The op buffer is caller-owned (typically a
 * PMM allocation) — the parser only writes into it. */
struct hailo_re_corpus {
    uint32_t format_version;   /* header format_version */
    uint32_t op_count;         /* number of populated entries in `ops` */
    uint32_t op_capacity;      /* capacity of `ops` (caller-set) */
    struct hailo_re_op *ops;   /* caller-owned, parser-populated */
    bool has_header;
    bool has_trailer;
    uint32_t skipped_unknown;  /* lines whose `type` was neither header,
                                * op, nor trailer — warned, not fatal */
};

/* Parse `text` of `len` bytes into `c`. The caller pre-populates
 * `c->ops` and `c->op_capacity`; the parser fills `op_count` plus
 * the rest of the fields.
 *
 * Returns HAILO_RE_CORPUS_OK on success, or a negative error code.
 * On error, `c->op_count` reflects the number of ops parsed before
 * the failure so the caller can pinpoint the offending entry. */
int hailo_re_corpus_parse(const char *text, size_t len,
                          struct hailo_re_corpus *c);

/* Returns a pointer to the op with `seq == target`, or NULL if no
 * such entry exists. O(log N) (binary search; ops are monotonic). */
const struct hailo_re_op *
hailo_re_corpus_find_seq(const struct hailo_re_corpus *c, uint32_t target);

/* Format a 32-bit value as a lowercase little-endian hex string with
 * exactly `2*size` characters, matching the corpus format spec's
 * `value` field encoding (byte 0 first, byte size-1 last). The caller's
 * buffer must have room for `2*size + 1` characters (NUL-terminator
 * included). Used by `hailo replay-step` to render the
 * `HAILO_RE_CORPUS_RESPONSE` value field and any `HAILO_RE_CORPUS_DIVERGENCE`
 * expected/observed fields. Exposed so test code can assert against
 * the production implementation rather than reimplementing it. */
void hailo_re_format_le_hex(uint32_t value, uint8_t size, char *out);

#ifdef __cplusplus
}
#endif

#endif /* AI_ACCEL_HAILO_RE_CORPUS_H */
