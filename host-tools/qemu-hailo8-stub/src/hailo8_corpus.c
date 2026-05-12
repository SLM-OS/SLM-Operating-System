/*
 * Hailo RE corpus parser/index. See hailo8_corpus.h for the API contract.
 *
 * Parser scope is intentionally narrow: each line is a flat JSON object —
 * no nesting, no arrays, no escapes beyond \" and \\. The on-disk schema
 * in docs/hailo-re-corpus-format.md never produces anything richer, so a
 * 300-line hand-rolled state machine beats pulling in a JSON dependency
 * for both the standalone test binary and the in-QEMU build.
 */

#include "hailo8_corpus.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
    if (!errbuf || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static void copy_str(char *dst, size_t dstlen, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dstlen) {
        n = dstlen - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int hailo_value_to_hex(uint64_t value, uint32_t size, char *out, size_t outlen)
{
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return -1;
    }
    if (outlen < (size_t)size * 2 + 1) {
        return -1;
    }
    static const char hex_digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < size; i++) {
        uint8_t b = (uint8_t)((value >> (i * 8)) & 0xFF);
        out[i * 2 + 0] = hex_digits[(b >> 4) & 0xF];
        out[i * 2 + 1] = hex_digits[b & 0xF];
    }
    out[size * 2] = '\0';
    return 0;
}

int hailo_value_from_hex(const char *hex, uint32_t size, uint64_t *out)
{
    if (!hex || !out) {
        return -1;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return -1;
    }
    size_t expect = (size_t)size * 2;
    if (strlen(hex) != expect) {
        return -1;
    }
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++) {
        int hi, lo;
        char ch_hi = hex[i * 2 + 0];
        char ch_lo = hex[i * 2 + 1];
        if (ch_hi >= '0' && ch_hi <= '9') {
            hi = ch_hi - '0';
        } else if (ch_hi >= 'a' && ch_hi <= 'f') {
            hi = 10 + (ch_hi - 'a');
        } else if (ch_hi >= 'A' && ch_hi <= 'F') {
            hi = 10 + (ch_hi - 'A');
        } else {
            return -1;
        }
        if (ch_lo >= '0' && ch_lo <= '9') {
            lo = ch_lo - '0';
        } else if (ch_lo >= 'a' && ch_lo <= 'f') {
            lo = 10 + (ch_lo - 'a');
        } else if (ch_lo >= 'A' && ch_lo <= 'F') {
            lo = 10 + (ch_lo - 'A');
        } else {
            return -1;
        }
        v |= ((uint64_t)((hi << 4) | lo)) << (i * 8);
    }
    *out = v;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Flat-JSON line parser                                                       */
/*                                                                             */
/* Grammar accepted (each line independently):                                 */
/*   line   := '{' WS pair (WS ',' WS pair)* WS '}' WS                         */
/*   pair   := '"' key '"' WS ':' WS value                                     */
/*   value  := number | string | 'null' | 'true' | 'false'                     */
/*   number := optional '-' followed by digits                                 */
/*   string := '"' chars '"'  (only \" and \\ escapes recognized)              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
} json_cursor_t;

static void skip_ws(json_cursor_t *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static int parse_key(json_cursor_t *c, char *out, size_t outlen)
{
    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') return -1;
    c->p++;
    size_t i = 0;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end) {
            c->p++;
        }
        if (i + 1 >= outlen) return -1;
        out[i++] = *c->p++;
    }
    if (c->p >= c->end) return -1;
    c->p++;
    out[i] = '\0';
    return 0;
}

/* parse_value: returns 0 on success. Fills exactly one of (str_out, int_out,
 * is_null). If is_null is non-NULL and value is `null`, sets *is_null=1.
 * Strings come back with simple escape handling for \" and \\. */
static int parse_value(json_cursor_t *c,
                       char *str_out, size_t str_outlen,
                       int64_t *int_out,
                       int *is_str, int *is_int, int *is_null, int *is_bool, int *bool_val)
{
    *is_str = *is_int = *is_null = *is_bool = 0;
    skip_ws(c);
    if (c->p >= c->end) return -1;

    if (*c->p == '"') {
        c->p++;
        size_t i = 0;
        while (c->p < c->end && *c->p != '"') {
            char ch = *c->p++;
            if (ch == '\\' && c->p < c->end) {
                char esc = *c->p++;
                if (esc == '"') {
                    ch = '"';
                } else if (esc == '\\') {
                    ch = '\\';
                } else if (esc == 'n') {
                    ch = '\n';
                } else if (esc == 't') {
                    ch = '\t';
                } else {
                    ch = esc;
                }
            }
            /* Always advance via the loop's c->p++ above; only store into
             * str_out when there's room. Strings beyond outlen are silently
             * truncated — `note` fields can be arbitrarily long. */
            if (str_out && i + 1 < str_outlen) {
                str_out[i++] = ch;
            }
        }
        if (c->p >= c->end) {
            return -1;
        }
        c->p++;
        if (str_out) {
            str_out[i < str_outlen ? i : str_outlen - 1] = '\0';
        }
        *is_str = 1;
        return 0;
    }

    if (c->end - c->p >= 4 && memcmp(c->p, "null", 4) == 0) {
        c->p += 4;
        *is_null = 1;
        return 0;
    }
    if (c->end - c->p >= 4 && memcmp(c->p, "true", 4) == 0) {
        c->p += 4;
        *is_bool = 1;
        if (bool_val) *bool_val = 1;
        return 0;
    }
    if (c->end - c->p >= 5 && memcmp(c->p, "false", 5) == 0) {
        c->p += 5;
        *is_bool = 1;
        if (bool_val) *bool_val = 0;
        return 0;
    }

    /* Number (signed integer; the spec never uses floats). */
    int neg = 0;
    if (*c->p == '-') {
        neg = 1;
        c->p++;
    }
    if (c->p >= c->end || !isdigit((unsigned char)*c->p)) {
        return -1;
    }
    int64_t v = 0;
    while (c->p < c->end && isdigit((unsigned char)*c->p)) {
        int d = *c->p - '0';
        /* Guard against int64 overflow (signed overflow is UB). 20-digit
         * literals like 99999999999999999999 would otherwise wrap silently. */
        if (v > (INT64_MAX - d) / 10) {
            return -1;
        }
        v = v * 10 + d;
        c->p++;
    }
    if (neg) {
        v = -v;
    }
    if (int_out) {
        *int_out = v;
    }
    *is_int = 1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Index management                                                            */
/* -------------------------------------------------------------------------- */

static int ensure_seq_index(hailo_corpus_t *c, uint64_t seq)
{
    if (seq < c->seq_index_cap) {
        return 0;
    }
    size_t new_cap = c->seq_index_cap ? c->seq_index_cap : 64;
    while (new_cap <= seq) {
        new_cap *= 2;
    }
    int32_t *ni = realloc(c->seq_index, new_cap * sizeof(int32_t));
    if (!ni) return -1;
    for (size_t i = c->seq_index_cap; i < new_cap; i++) {
        ni[i] = -1;
    }
    c->seq_index = ni;
    c->seq_index_cap = new_cap;
    return 0;
}

static int push_entry(hailo_corpus_t *c, const hailo_op_entry_t *e,
                      char *errbuf, size_t errlen)
{
    if (e->seq == 0 || e->seq >= HAILO_CORPUS_MAX_SEQ) {
        set_err(errbuf, errlen,
                "seq must be in [1, %u), got %llu",
                HAILO_CORPUS_MAX_SEQ,
                (unsigned long long)e->seq);
        return -1;
    }
    if (ensure_seq_index(c, e->seq) != 0) {
        set_err(errbuf, errlen, "out of memory growing seq index");
        return -1;
    }
    if (c->seq_index[e->seq] != -1) {
        set_err(errbuf, errlen,
                "duplicate entry at seq=%llu",
                (unsigned long long)e->seq);
        return -1;
    }
    if (c->n_entries == c->cap_entries) {
        size_t new_cap = c->cap_entries ? c->cap_entries * 2 : 64;
        hailo_op_entry_t *ne = realloc(c->entries,
                                       new_cap * sizeof(hailo_op_entry_t));
        if (!ne) {
            set_err(errbuf, errlen, "out of memory growing entries");
            return -1;
        }
        c->entries = ne;
        c->cap_entries = new_cap;
    }
    c->entries[c->n_entries] = *e;
    c->seq_index[e->seq] = (int32_t)c->n_entries;
    c->n_entries++;
    if (e->seq > c->max_seq) {
        c->max_seq = e->seq;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Line parsers (header, op, trailer)                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    /* op-line fields populated during parsing */
    hailo_op_entry_t entry;
    int has_seq, has_bar, has_offset, has_size, has_dir, has_value;
    char dir_str[16];
    char value_hex[32];
} op_parse_t;

static int parse_op_line(const char *line, size_t linelen,
                         op_parse_t *op,
                         char *errbuf, size_t errlen)
{
    memset(op, 0, sizeof(*op));
    json_cursor_t cur = { .p = line, .end = line + linelen };
    skip_ws(&cur);
    if (cur.p >= cur.end || *cur.p != '{') {
        set_err(errbuf, errlen, "line does not start with '{'");
        return -1;
    }
    cur.p++;

    while (1) {
        skip_ws(&cur);
        if (cur.p >= cur.end) {
            set_err(errbuf, errlen, "truncated object");
            return -1;
        }
        if (*cur.p == '}') {
            cur.p++;
            break;
        }

        char key[64];
        if (parse_key(&cur, key, sizeof(key)) != 0) {
            set_err(errbuf, errlen, "bad key");
            return -1;
        }
        skip_ws(&cur);
        if (cur.p >= cur.end || *cur.p != ':') {
            set_err(errbuf, errlen, "missing ':' after key '%s'", key);
            return -1;
        }
        cur.p++;

        char strbuf[512];
        int64_t intval = 0;
        int is_str = 0, is_int = 0, is_null = 0, is_bool = 0, bool_val = 0;
        if (parse_value(&cur, strbuf, sizeof(strbuf),
                        &intval, &is_str, &is_int, &is_null, &is_bool, &bool_val) != 0) {
            set_err(errbuf, errlen, "bad value for key '%s'", key);
            return -1;
        }

        if (strcmp(key, "type") == 0 && is_str) {
            copy_str(op->entry.source, sizeof(op->entry.source), "");
            if (strcmp(strbuf, "op") != 0) {
                set_err(errbuf, errlen, "expected type='op', got '%s'", strbuf);
                return -1;
            }
        } else if (strcmp(key, "seq") == 0 && is_int) {
            op->entry.seq = (uint64_t)intval;
            op->has_seq = 1;
        } else if (strcmp(key, "bar") == 0 && is_int) {
            op->entry.bar = (int)intval;
            op->has_bar = 1;
        } else if (strcmp(key, "offset") == 0 && is_int) {
            op->entry.offset = (uint64_t)intval;
            op->has_offset = 1;
        } else if (strcmp(key, "size") == 0 && is_int) {
            op->entry.size = (uint32_t)intval;
            op->has_size = 1;
        } else if (strcmp(key, "dir") == 0 && is_str) {
            copy_str(op->dir_str, sizeof(op->dir_str), strbuf);
            op->has_dir = 1;
        } else if (strcmp(key, "value") == 0 && is_str) {
            copy_str(op->value_hex, sizeof(op->value_hex), strbuf);
            op->has_value = 1;
        } else if (strcmp(key, "source") == 0 && is_str) {
            copy_str(op->entry.source, sizeof(op->entry.source), strbuf);
        } else if (strcmp(key, "validated_at_commit") == 0) {
            if (is_str) {
                copy_str(op->entry.validated_at_commit,
                         sizeof(op->entry.validated_at_commit), strbuf);
            }
        } else if (strcmp(key, "validated_at") == 0) {
            if (is_str) {
                copy_str(op->entry.validated_at,
                         sizeof(op->entry.validated_at), strbuf);
            }
        } else if (strcmp(key, "note") == 0 && is_str) {
            copy_str(op->entry.note, sizeof(op->entry.note), strbuf);
        } else if (strcmp(key, "msi_after") == 0 && is_int) {
            op->entry.msi_after_present = true;
            op->entry.msi_after_vector = (uint32_t)intval;
        }
        /* unknown keys silently tolerated per spec §Provenance */

        skip_ws(&cur);
        if (cur.p < cur.end && *cur.p == ',') {
            cur.p++;
            continue;
        }
        skip_ws(&cur);
        if (cur.p < cur.end && *cur.p == '}') {
            cur.p++;
            break;
        }
    }

    if (!op->has_seq || !op->has_bar || !op->has_offset ||
        !op->has_size || !op->has_dir || !op->has_value) {
        set_err(errbuf, errlen, "op line missing required field "
                "(seq/bar/offset/size/dir/value)");
        return -1;
    }

    if (strcmp(op->dir_str, "read") == 0) {
        op->entry.is_write = false;
    } else if (strcmp(op->dir_str, "write") == 0) {
        op->entry.is_write = true;
    } else {
        set_err(errbuf, errlen, "bad dir '%s'", op->dir_str);
        return -1;
    }

    if (hailo_value_from_hex(op->value_hex, op->entry.size,
                             &op->entry.value) != 0) {
        set_err(errbuf, errlen, "bad value hex '%s' for size=%u",
                op->value_hex, op->entry.size);
        return -1;
    }

    return 0;
}

static int parse_header_line(hailo_corpus_t *c,
                             const char *line, size_t linelen,
                             char *errbuf, size_t errlen)
{
    json_cursor_t cur = { .p = line, .end = line + linelen };
    skip_ws(&cur);
    if (cur.p >= cur.end || *cur.p != '{') {
        set_err(errbuf, errlen, "header line not an object");
        return -1;
    }
    cur.p++;

    while (1) {
        skip_ws(&cur);
        if (cur.p >= cur.end) {
            set_err(errbuf, errlen, "truncated header");
            return -1;
        }
        if (*cur.p == '}') {
            cur.p++;
            break;
        }

        char key[64];
        if (parse_key(&cur, key, sizeof(key)) != 0) {
            set_err(errbuf, errlen, "bad header key");
            return -1;
        }
        skip_ws(&cur);
        if (cur.p >= cur.end || *cur.p != ':') {
            set_err(errbuf, errlen, "missing ':' in header for '%s'", key);
            return -1;
        }
        cur.p++;

        char strbuf[256];
        int64_t intval = 0;
        int is_str = 0, is_int = 0, is_null = 0, is_bool = 0, bool_val = 0;
        if (parse_value(&cur, strbuf, sizeof(strbuf),
                        &intval, &is_str, &is_int, &is_null, &is_bool, &bool_val) != 0) {
            set_err(errbuf, errlen, "bad header value for '%s'", key);
            return -1;
        }
        if (strcmp(key, "format_version") == 0 && is_int) {
            c->format_version = (int)intval;
        } else if (strcmp(key, "hailort_version") == 0 && is_str) {
            copy_str(c->hailort_version, sizeof(c->hailort_version), strbuf);
        } else if (strcmp(key, "fw_version") == 0 && is_str) {
            copy_str(c->fw_version, sizeof(c->fw_version), strbuf);
        } else if (strcmp(key, "capture_host") == 0 && is_str) {
            copy_str(c->capture_host, sizeof(c->capture_host), strbuf);
        }

        skip_ws(&cur);
        if (cur.p < cur.end && *cur.p == ',') {
            cur.p++;
            continue;
        }
        skip_ws(&cur);
        if (cur.p < cur.end && *cur.p == '}') {
            cur.p++;
            break;
        }
    }
    return 0;
}

/* Detect the `type` field of a line — header / op / trailer / other.
 * Writes the value into the caller's buffer (null-terminated) and returns
 * 0 on success. Returns -1 when no `"type": "..."` pair is present. The
 * caller buffer is preferred over a static internal one so future
 * call sites that retain two type strings concurrently don't trip on
 * mutated state. */
static int line_type_field(const char *line, size_t linelen,
                           char *out, size_t outlen)
{
    if (!out || outlen == 0) {
        return -1;
    }
    out[0] = '\0';
    const char *p = line;
    const char *end = line + linelen;
    /* `p + 6 <= end` avoids the `end - 6` pointer-underflow case when
     * `linelen < 6` (pointer arithmetic that lands outside the array's
     * one-past-the-end region is implementation-defined per C11 6.5.6). */
    while (p + 6 <= end) {
        if (*p == '"' && memcmp(p, "\"type\"", 6) == 0) {
            p += 6;
            while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) {
                p++;
            }
            if (p < end && *p == '"') {
                p++;
                size_t i = 0;
                while (p < end && *p != '"' && i + 1 < outlen) {
                    out[i++] = *p++;
                }
                out[i] = '\0';
                return 0;
            }
            return -1;
        }
        p++;
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

hailo_corpus_t *hailo_corpus_new_memory(void)
{
    hailo_corpus_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->format_version = 1;
    return c;
}

void hailo_corpus_free(hailo_corpus_t *c)
{
    if (!c) return;
    if (c->append_fp) {
        fclose(c->append_fp);
    }
    free(c->entries);
    free(c->seq_index);
    free(c);
}

hailo_corpus_t *hailo_corpus_load(const char *path,
                                  bool append_writable,
                                  char *errbuf, size_t errlen)
{
    if (!path || !*path) {
        set_err(errbuf, errlen, "path required");
        return NULL;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        set_err(errbuf, errlen, "open(%s) failed: %s", path, strerror(errno));
        return NULL;
    }
    hailo_corpus_t *c = hailo_corpus_new_memory();
    if (!c) {
        fclose(fp);
        set_err(errbuf, errlen, "out of memory");
        return NULL;
    }
    copy_str(c->path, sizeof(c->path), path);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int header_seen = 0;
    int lineno = 0;
    while ((n = getline(&line, &cap, fp)) != -1) {
        lineno++;
        size_t len = (size_t)n;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;
        if (line[0] == '#') continue;   /* tolerate hand-edited comments */

        char type[16];
        if (line_type_field(line, len, type, sizeof(type)) != 0) {
            set_err(errbuf, errlen, "line %d: no 'type' field", lineno);
            goto fail;
        }
        if (strcmp(type, "header") == 0) {
            if (header_seen) {
                set_err(errbuf, errlen,
                        "line %d: duplicate header", lineno);
                goto fail;
            }
            if (parse_header_line(c, line, len, errbuf, errlen) != 0) {
                goto fail;
            }
            header_seen = 1;
        } else if (strcmp(type, "op") == 0) {
            op_parse_t op;
            if (parse_op_line(line, len, &op, errbuf, errlen) != 0) {
                goto fail;
            }
            if (push_entry(c, &op.entry, errbuf, errlen) != 0) {
                goto fail;
            }
        } else if (strcmp(type, "trailer") == 0) {
            /* Trailer is informational; ignore for now. */
        } else {
            /* Unknown type — tolerate forward-compat. */
        }
    }
    free(line);
    fclose(fp);

    if (append_writable) {
        c->append_fp = fopen(path, "a");
        if (!c->append_fp) {
            set_err(errbuf, errlen, "open(%s, 'a') failed: %s",
                    path, strerror(errno));
            hailo_corpus_free(c);
            return NULL;
        }
    }
    return c;

fail:
    free(line);
    fclose(fp);
    hailo_corpus_free(c);
    return NULL;
}

const hailo_op_entry_t *hailo_corpus_get(const hailo_corpus_t *c, uint64_t seq)
{
    if (!c || seq == 0 || seq >= c->seq_index_cap) return NULL;
    int32_t idx = c->seq_index[seq];
    if (idx < 0) return NULL;
    return &c->entries[idx];
}

int hailo_corpus_inject_entry(hailo_corpus_t *c,
                              const hailo_op_entry_t *entry,
                              char *errbuf, size_t errlen)
{
    if (!c || !entry) {
        set_err(errbuf, errlen, "null arg");
        return -1;
    }
    return push_entry(c, entry, errbuf, errlen);
}

int hailo_corpus_append_write(hailo_corpus_t *c,
                              uint64_t seq, int bar,
                              uint64_t offset, uint32_t size,
                              uint64_t value,
                              char *errbuf, size_t errlen)
{
    hailo_op_entry_t e = (hailo_op_entry_t){0};
    e.seq = seq;
    e.bar = bar;
    e.offset = offset;
    e.size = size;
    e.is_write = true;
    e.value = value;
    copy_str(e.source, sizeof(e.source), "qemu_capture");

    char hex[32];
    if (hailo_value_to_hex(value, size, hex, sizeof(hex)) != 0) {
        set_err(errbuf, errlen, "bad size=%u for hex encode", size);
        return -1;
    }

    if (c->append_fp) {
        int rc = fprintf(c->append_fp,
                "{\"type\":\"op\",\"seq\":%llu,\"bar\":%d,\"offset\":%llu,"
                "\"size\":%u,\"dir\":\"write\",\"value\":\"%s\","
                "\"source\":\"qemu_capture\","
                "\"validated_at_commit\":null,\"validated_at\":null}\n",
                (unsigned long long)seq, bar,
                (unsigned long long)offset, size, hex);
        if (rc < 0) {
            set_err(errbuf, errlen, "fprintf: %s", strerror(errno));
            return -1;
        }
        if (fflush(c->append_fp) != 0) {
            set_err(errbuf, errlen, "fflush: %s", strerror(errno));
            return -1;
        }
    }

    return push_entry(c, &e, errbuf, errlen);
}
