/*
 * hailo_re_corpus.c — JSONL corpus reader (#795 Phase 0 Task 0.4).
 *
 * Implementation notes:
 *
 *   - Lines are split on `\n`; the optional trailing `\r` on `\r\n`
 *     files is tolerated. Blank lines are skipped.
 *
 *   - Each line is scanned independently with a tiny string-aware
 *     state machine. The scanner tracks whether the cursor is inside
 *     a JSON string literal so a `"seq": 7` substring buried inside a
 *     `note` cannot be mistaken for the real `seq` key.
 *
 *   - The required-field check fires per-op (seq/bar/offset/size/dir/
 *     value); a missing field returns HAILO_RE_CORPUS_E_BAD_FIELD with
 *     `op_count` pointing at the failure. `source`, `validated_at_commit`,
 *     `validated_at`, and `note` are not required by SLM-OS today.
 */

#include "hailo_re_corpus.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Tiny scan helpers                                                           */
/* -------------------------------------------------------------------------- */

static inline bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

/* Decode a single ASCII hex digit. Returns -1 on invalid. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Find the value of a top-level JSON key inside one line. Skips over
 * string literals so the key match cannot trigger inside a `note`
 * value. On success returns a pointer to the first non-ws character
 * after the colon — the caller decides whether to parse it as a
 * number, string, or null.
 *
 * NOT a general-purpose JSON locator: it assumes the file is the
 * single-line dicts produced by the Phase 0 driver (no nested
 * objects, no arrays). That's enough for the corpus format spec.
 */
static const char *find_value_after_key(const char *line, size_t len,
                                        const char *key)
{
    const size_t klen = strlen(key);
    const char *p = line;
    const char *e = line + len;
    bool in_string = false;

    while (p < e) {
        if (in_string) {
            if (*p == '\\' && p + 1 < e) {
                p += 2;
                continue;
            }
            if (*p == '"') {
                in_string = false;
            }
            p++;
            continue;
        }

        if (*p == '"') {
            /* Candidate key opener. Match `"<klen bytes>"` exactly. */
            if ((size_t)(e - (p + 1)) >= klen + 1
                && memcmp(p + 1, key, klen) == 0
                && p[1 + klen] == '"') {
                const char *q = p + klen + 2;   /* past closing quote */
                while (q < e && (is_ws(*q) || *q == ':')) q++;
                return q < e ? q : NULL;
            }
            /* Otherwise: enter string-literal mode and keep scanning. */
            in_string = true;
        }
        p++;
    }
    return NULL;
}

/* Parse a JSON string value at `v` (which must point at the opening
 * `"`). Writes up to `cap-1` characters into `out` and NUL-terminates.
 * Returns true on success, false if `v` doesn't point at `"` or the
 * string is unterminated. Long values are truncated. */
static bool parse_string_value(const char *v, const char *end,
                               char *out, size_t cap)
{
    if (v >= end || *v != '"') return false;
    v++;
    size_t i = 0;
    while (v < end && *v != '"') {
        if (*v == '\\' && v + 1 < end) {
            /* Minimal escape handling — the corpus producer doesn't
             * emit fancy escapes in keys we read, but `\\` and `\"`
             * must be passed through to keep the in-string skipper
             * honest. We collapse two-character escapes to the
             * trailing char. */
            if (i + 1 < cap) out[i++] = v[1];
            v += 2;
            continue;
        }
        if (i + 1 < cap) out[i++] = *v;
        v++;
    }
    if (v >= end) return false;
    out[i] = '\0';
    return true;
}

/* Parse a JSON unsigned integer at `v`. Hands back the number in
 * *out and the position immediately past the last digit in *end_out
 * (may be NULL if the caller doesn't care). Returns true on success.
 * Rejects negative numbers — `seq`, `bar`, `offset`, and `size` are
 * all non-negative in the spec. */
static bool parse_uint_value(const char *v, const char *end, uint64_t *out,
                             const char **end_out)
{
    if (v >= end) return false;
    if (*v < '0' || *v > '9') return false;
    uint64_t acc = 0;
    while (v < end && *v >= '0' && *v <= '9') {
        /* Cap at 2^32-1 — every spec-defined uint field fits in u32,
         * but we use u64 internally so the bound check is one
         * comparison rather than a saturating multiply. Overflow
         * past u64 is rejected so a corpus producer that emits e.g.
         * a stray 30-digit number doesn't silently wrap. */
        if (acc > (UINT64_MAX - 9) / 10) return false;
        acc = acc * 10 + (uint64_t)(*v - '0');
        v++;
    }
    if (out) *out = acc;
    if (end_out) *end_out = v;
    return true;
}

/* True iff the JSON value is the literal `null`. Requires the byte
 * after the four-char match to be either end-of-line or a non-alpha
 * structural character so a hypothetical `"validated_at_commit":
 * "nullable"` isn't silently treated as JSON `null`. */
static bool value_is_null(const char *v, const char *end)
{
    if ((end - v) < 4 || memcmp(v, "null", 4) != 0) return false;
    if ((end - v) == 4) return true;
    char after = v[4];
    return (after == ',' || after == '}' || after == ' '
            || after == '\t' || after == '\r' || after == '\n');
}

/* Decode a value-field hex string of exactly 2*size lowercase chars
 * into a uint32 in little-endian order. Returns true on success. For
 * size == 8 we still write the low 32 bits — `hailo replay-step`
 * rejects size != 4 explicitly, so a size=8 entry never reaches MMIO.
 */
static bool parse_hex_value(const char *v, const char *end, uint8_t size,
                            uint32_t *out)
{
    if (v >= end || *v != '"') return false;
    v++;
    const char *q = v;
    while (q < end && *q != '"') q++;
    if (q >= end) return false;
    size_t len = (size_t)(q - v);
    if (len != (size_t)size * 2u) return false;

    uint64_t acc = 0;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_digit(v[i]);
        int lo = hex_digit(v[i + 1]);
        if (hi < 0 || lo < 0) return false;
        uint64_t byte = (uint64_t)((hi << 4) | lo);
        size_t byte_index = i / 2;
        acc |= byte << (byte_index * 8u);
    }
    *out = (uint32_t)(acc & 0xFFFFFFFFu);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Per-line dispatch                                                            */
/* -------------------------------------------------------------------------- */

static int parse_header_line(const char *line, size_t len,
                             struct hailo_re_corpus *c)
{
    const char *v = find_value_after_key(line, len, "format_version");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    uint64_t fv = 0;
    if (!parse_uint_value(v, line + len, &fv, NULL)) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    c->format_version = (uint32_t)fv;
    c->has_header = true;
    return HAILO_RE_CORPUS_OK;
}

static int parse_op_line(const char *line, size_t len,
                         struct hailo_re_corpus *c)
{
    if (c->op_count >= c->op_capacity) {
        return HAILO_RE_CORPUS_E_OVERFLOW;
    }
    struct hailo_re_op op = {0};
    const char *end = line + len;
    const char *v;

    v = find_value_after_key(line, len, "seq");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    uint64_t seq = 0;
    if (!parse_uint_value(v, end, &seq, NULL) || seq > UINT32_MAX) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    op.seq = (uint32_t)seq;
    /* File order is unconstrained — the QEMU stub auto-appends writes
     * at EOF in capture time order, which is monotonic within one run
     * but not necessarily across runs. The post-parse pass sorts the
     * ops array and rejects duplicates. See PR #829 for the matching
     * fix on the host-side Python loader. */

    v = find_value_after_key(line, len, "bar");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    uint64_t bar = 0;
    if (!parse_uint_value(v, end, &bar, NULL) || bar > 6u) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    op.bar = (uint8_t)bar;

    v = find_value_after_key(line, len, "offset");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    uint64_t off = 0;
    if (!parse_uint_value(v, end, &off, NULL) || off > UINT32_MAX) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    op.offset = (uint32_t)off;

    v = find_value_after_key(line, len, "size");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    uint64_t sz = 0;
    if (!parse_uint_value(v, end, &sz, NULL)) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    op.size = (uint8_t)sz;

    v = find_value_after_key(line, len, "dir");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    char dir_buf[16];
    if (!parse_string_value(v, end, dir_buf, sizeof(dir_buf))) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }
    if (strcmp(dir_buf, "read") == 0) {
        op.dir = HAILO_RE_DIR_READ;
    } else if (strcmp(dir_buf, "write") == 0) {
        op.dir = HAILO_RE_DIR_WRITE;
    } else {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }

    v = find_value_after_key(line, len, "value");
    if (!v) return HAILO_RE_CORPUS_E_BAD_FIELD;
    if (!parse_hex_value(v, end, op.size, &op.value)) {
        return HAILO_RE_CORPUS_E_BAD_FIELD;
    }

    /* validated_at_commit is optional from SLM-OS's perspective — a
     * non-null string means the entry has already been validated on
     * real hardware. `replay-step` uses this to refuse re-validating
     * a seq=N entry that is already stamped. */
    v = find_value_after_key(line, len, "validated_at_commit");
    if (v) {
        op.validated = value_is_null(v, end) ? 0u : 1u;
    }

    c->ops[c->op_count++] = op;
    return HAILO_RE_CORPUS_OK;
}

/* Insertion sort the ops array by seq, then detect duplicates in one
 * pass. Insertion sort is fine here: typical corpora have N≈2k ops and
 * most are already in seq order (the C-side QEMU stub appends each run
 * monotonically; only cross-run gap-fills land out of place). Worst
 * case is O(N²) but practical workload is O(N + K) where K is the
 * count of out-of-order entries. Returns OK or E_DUPLICATE. */
static int sort_ops_and_check_unique(struct hailo_re_corpus *c)
{
    for (uint32_t i = 1; i < c->op_count; i++) {
        struct hailo_re_op key = c->ops[i];
        uint32_t j = i;
        while (j > 0 && c->ops[j - 1].seq > key.seq) {
            c->ops[j] = c->ops[j - 1];
            j--;
        }
        c->ops[j] = key;
    }
    for (uint32_t i = 1; i < c->op_count; i++) {
        if (c->ops[i].seq == c->ops[i - 1].seq) {
            return HAILO_RE_CORPUS_E_DUPLICATE;
        }
    }
    return HAILO_RE_CORPUS_OK;
}

/* -------------------------------------------------------------------------- */
/* Top-level driver                                                            */
/* -------------------------------------------------------------------------- */

int hailo_re_corpus_parse(const char *text, size_t len,
                          struct hailo_re_corpus *c)
{
    if (!text || !c || !c->ops || c->op_capacity == 0) {
        return HAILO_RE_CORPUS_E_PARSE;
    }

    c->format_version = 0;
    c->op_count = 0;
    c->has_header = false;
    c->has_trailer = false;
    c->skipped_unknown = 0;

    bool saw_first_nonempty = false;
    const char *p = text;
    const char *end = text + len;

    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        size_t line_len = (size_t)(p - line);
        if (p < end) p++;   /* consume the newline */

        /* Tolerate \r\n line endings — strip the trailing \r before
         * the rest of the parser sees the line. */
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

        /* Skip leading whitespace + blank lines. */
        size_t lead = 0;
        while (lead < line_len && is_ws(line[lead])) lead++;
        if (lead == line_len) continue;
        line += lead;
        line_len -= lead;

        const char *tv = find_value_after_key(line, line_len, "type");
        if (!tv) return HAILO_RE_CORPUS_E_PARSE;

        char type_buf[16];
        if (!parse_string_value(tv, line + line_len, type_buf, sizeof(type_buf))) {
            return HAILO_RE_CORPUS_E_PARSE;
        }

        if (!saw_first_nonempty) {
            saw_first_nonempty = true;
            if (strcmp(type_buf, "header") != 0) {
                return HAILO_RE_CORPUS_E_NO_HEADER;
            }
        }

        if (strcmp(type_buf, "header") == 0) {
            int hrc = parse_header_line(line, line_len, c);
            if (hrc != HAILO_RE_CORPUS_OK) return hrc;
        } else if (strcmp(type_buf, "op") == 0) {
            int orc = parse_op_line(line, line_len, c);
            if (orc != HAILO_RE_CORPUS_OK) return orc;
        } else if (strcmp(type_buf, "trailer") == 0) {
            c->has_trailer = true;
        } else {
            /* Unknown line types: warn-and-continue, per the format
             * spec's tolerance rule. */
            c->skipped_unknown++;
        }
    }

    if (!c->has_header) return HAILO_RE_CORPUS_E_NO_HEADER;
    /* Post-parse: sort by seq so the binary search in
     * hailo_re_corpus_find_seq stays correct, and reject duplicates. */
    int src = sort_ops_and_check_unique(c);
    if (src != HAILO_RE_CORPUS_OK) return src;
    return HAILO_RE_CORPUS_OK;
}

const struct hailo_re_op *
hailo_re_corpus_find_seq(const struct hailo_re_corpus *c, uint32_t target)
{
    if (!c || !c->ops || c->op_count == 0) return NULL;
    /* Binary search — ops are strictly monotonic. */
    uint32_t lo = 0;
    uint32_t hi = c->op_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint32_t s = c->ops[mid].seq;
        if (s == target) return &c->ops[mid];
        if (s < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

void hailo_re_format_le_hex(uint32_t value, uint8_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (uint8_t i = 0; i < size; i++) {
        uint8_t byte = (uint8_t)((value >> (i * 8u)) & 0xFFu);
        out[i * 2u]     = digits[(byte >> 4) & 0xFu];
        out[i * 2u + 1] = digits[byte & 0xFu];
    }
    out[size * 2u] = '\0';
}
