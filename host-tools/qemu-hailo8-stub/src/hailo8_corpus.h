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

typedef struct hailo_corpus {
    hailo_op_entry_t *entries;
    size_t            n_entries;
    size_t            cap_entries;

    /* O(1) seq → entries[] index. -1 sentinel for "no entry at this seq". */
    int32_t *seq_index;
    size_t   seq_index_cap;
    uint64_t max_seq;

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
