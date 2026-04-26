/*
 * model_engine.c - Model engine registry + .meta parser + launch
 * dispatch (admin & telemetry suite, M5).
 *
 * The registry is a static array indexed by `enum model_kind`. The
 * launch path reads a `/mnt/models/<name>.meta` text sidecar
 * (one key=value pair per line), looks up the right engine, and
 * dispatches. Only `mnist` and `raw` have real launch paths today;
 * `hailo` and `ggml` return MODEL_LAUNCH_ERR_NOSYS until those
 * runtimes land.
 */

#include "model_engine.h"
#include "vfs.h"
#include "string.h"
#include "pmm.h"
#include "slm_ffi.h"

extern int rust_model_load_builtin_mnist(void);

/* ===== Engine registry ============================================== */

static const struct model_engine_info k_engines[MODEL_KIND_COUNT] = {
    [MODEL_KIND_RAW] = {
        .name    = "raw",
        .kind    = MODEL_KIND_RAW,
        .state   = MODEL_ENGINE_READY,
        .summary = "load-and-report; never instantiated",
    },
    [MODEL_KIND_MNIST] = {
        .name    = "mnist",
        .kind    = MODEL_KIND_MNIST,
        .state   = MODEL_ENGINE_READY,
        .summary = "built-in MNIST graph (rust_model_load_builtin_mnist)",
    },
    [MODEL_KIND_HAILO] = {
        .name    = "hailo",
        .kind    = MODEL_KIND_HAILO,
        .state   = MODEL_ENGINE_NOSYS,
        .summary = "Hailo-8 HEF runtime (M5 stub — needs PR for HEF launch)",
    },
    [MODEL_KIND_GGML] = {
        .name    = "ggml",
        .kind    = MODEL_KIND_GGML,
        .state   = MODEL_ENGINE_NOSYS,
        .summary = "generic ggml runtime (M5 stub — separate spec)",
    },
};

const char *model_kind_name(enum model_kind kind)
{
    if ((unsigned)kind >= MODEL_KIND_COUNT) return NULL;
    return k_engines[kind].name;
}

enum model_kind model_kind_from_name(const char *name)
{
    if (!name) return MODEL_KIND_COUNT;
    for (unsigned i = 0; i < MODEL_KIND_COUNT; i++) {
        if (strcmp(name, k_engines[i].name) == 0)
            return (enum model_kind)i;
    }
    return MODEL_KIND_COUNT;
}

const struct model_engine_info *model_engine_info_get(enum model_kind kind)
{
    if ((unsigned)kind >= MODEL_KIND_COUNT) return NULL;
    return &k_engines[kind];
}

size_t model_engine_info_list(const struct model_engine_info *out[MODEL_KIND_COUNT])
{
    if (!out) return 0;
    for (size_t i = 0; i < MODEL_KIND_COUNT; i++) {
        out[i] = &k_engines[i];
    }
    return MODEL_KIND_COUNT;
}

/* ===== .meta parser ================================================= */

/* Skip leading ASCII whitespace (space + tab; line endings handled
 * by the line-splitter). */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

/* Compare two strings of length `len` to a nul-terminated literal. */
static bool span_eq_lit(const char *s, size_t len, const char *lit)
{
    for (size_t i = 0; i < len; i++) {
        if (lit[i] == '\0' || s[i] != lit[i]) return false;
    }
    return lit[len] == '\0';
}

/* Parse an unsigned decimal value from `[s, end)`. Returns 0 on
 * success; -1 on bad digit or overflow. */
static int parse_u64(const char *s, const char *end, uint64_t *out)
{
    uint64_t v = 0;
    if (s >= end) return -1;
    for (; s < end; s++) {
        if (*s < '0' || *s > '9') return -1;
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10ull) return -1;
        v = v * 10ull + d;
    }
    *out = v;
    return 0;
}

/* Copy a span (read-only, length-bounded) into a fixed-size dst[].
 * Truncates on overflow; always nul-terminates. */
static void copy_span(char *dst, size_t cap, const char *s, size_t len)
{
    if (cap == 0) return;
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = s[i];
    dst[n] = '\0';
}

int model_meta_parse(const char *buf, size_t len, struct model_meta *out)
{
    if (!buf || !out) return MODEL_LAUNCH_ERR_BADMETA;
    /* Zero-init so missing optional fields default to empty/0. */
    for (size_t i = 0; i < sizeof(*out); i++) ((char *)out)[i] = 0;
    out->kind = MODEL_KIND_COUNT;  /* sentinel: kind not yet seen */

    const char *p   = buf;
    const char *end = buf + len;
    while (p < end) {
        /* Find line end. */
        const char *line = p;
        while (p < end && *p != '\n' && *p != '\r') p++;
        const char *line_end = p;
        /* Consume the newline(s). */
        while (p < end && (*p == '\n' || *p == '\r')) p++;

        /* Trim leading whitespace; skip blank lines and comments. */
        line = skip_ws(line, line_end);
        if (line == line_end || *line == '#') continue;

        /* Find '='. */
        const char *eq = line;
        while (eq < line_end && *eq != '=') eq++;
        if (eq == line_end) return MODEL_LAUNCH_ERR_BADMETA;

        /* Trim trailing whitespace from the key. */
        const char *key_end = eq;
        while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
        size_t key_len = (size_t)(key_end - line);

        /* Value: everything after '=' up to end-of-line, leading WS
         * trimmed. Trailing WS also trimmed. */
        const char *val = skip_ws(eq + 1, line_end);
        const char *val_end = line_end;
        while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
        size_t val_len = (size_t)(val_end - val);

        if (span_eq_lit(line, key_len, "name")) {
            copy_span(out->name, MODEL_META_NAME_LEN, val, val_len);
        } else if (span_eq_lit(line, key_len, "kind")) {
            char kbuf[16];
            copy_span(kbuf, sizeof(kbuf), val, val_len);
            enum model_kind k = model_kind_from_name(kbuf);
            if (k == MODEL_KIND_COUNT) return MODEL_LAUNCH_ERR_BADKIND;
            out->kind = k;
        } else if (span_eq_lit(line, key_len, "size")) {
            if (parse_u64(val, val_end, &out->size) != 0)
                return MODEL_LAUNCH_ERR_BADMETA;
        } else if (span_eq_lit(line, key_len, "sha256")) {
            copy_span(out->sha256, MODEL_META_SHA256_LEN, val, val_len);
        } else if (span_eq_lit(line, key_len, "uploaded_ts_ms")) {
            if (parse_u64(val, val_end, &out->uploaded_ts_ms) != 0)
                return MODEL_LAUNCH_ERR_BADMETA;
        }
        /* Unknown keys silently ignored — forward-compat. */
    }

    if (out->kind == MODEL_KIND_COUNT) {
        /* `kind` is mandatory. */
        return MODEL_LAUNCH_ERR_BADMETA;
    }
    return MODEL_LAUNCH_OK;
}

/* ===== Read /mnt/models/<name>.meta from VFS ======================== */

int model_meta_read(const char *name, struct model_meta *out)
{
    if (!name || !out) return MODEL_LAUNCH_ERR_BADMETA;

    /* Build the path. /mnt/models/<name>.meta — bounded by VFS_MAX_PATH. */
    char path[VFS_MAX_PATH];
    const char prefix[] = "/mnt/models/";
    const char suffix[] = ".meta";
    size_t plen = sizeof(prefix) - 1;
    size_t slen = sizeof(suffix) - 1;
    size_t nlen = 0;
    while (name[nlen]) nlen++;

    if (plen + nlen + slen + 1 > sizeof(path)) return MODEL_LAUNCH_ERR_BADMETA;
    for (size_t i = 0; i < plen; i++) path[i] = prefix[i];
    for (size_t i = 0; i < nlen; i++) path[plen + i] = name[i];
    for (size_t i = 0; i < slen; i++) path[plen + nlen + i] = suffix[i];
    path[plen + nlen + slen] = '\0';

    struct vfs_entry_info info;
    if (vfs_stat_path(path, &info) != 0 || info.type != 0) {
        return MODEL_LAUNCH_ERR_NOMETA;
    }
    /* Cap at 4 KB — the .meta is a tiny key=value sidecar; anything
     * bigger is malformed input we don't need to consume. */
    if (info.size == 0 || info.size > 4096u) {
        return MODEL_LAUNCH_ERR_BADMETA;
    }

    /* Allocate a one-page staging buffer to hold the file contents. */
    char *buf = (char *)pmm_alloc_pages(1);
    if (!buf) return MODEL_LAUNCH_ERR_BADMETA;

    int rd = vfs_read_path(path, buf, info.size, 0);
    if (rd != (int)info.size) {
        pmm_free_pages((uint8_t *)buf, 1);
        return MODEL_LAUNCH_ERR_BADMETA;
    }

    int rc = model_meta_parse(buf, (size_t)rd, out);
    pmm_free_pages((uint8_t *)buf, 1);
    return rc;
}

/* ===== Launch dispatch ============================================== */

int model_engine_launch(const char *name, int *out_task_id)
{
    if (!name) return MODEL_LAUNCH_ERR_BADMETA;

    struct model_meta meta;
    int rc = model_meta_read(name, &meta);
    if (rc != MODEL_LAUNCH_OK) return rc;

    switch (meta.kind) {
    case MODEL_KIND_RAW:
        /* Per spec §10.2: "raw is load-and-report; do not run". The
         * .blob is on disk; the engine has nothing to instantiate.
         * Return a sentinel task_id of 0 so callers can distinguish
         * "ran" from "failed". */
        if (out_task_id) *out_task_id = 0;
        return MODEL_LAUNCH_OK;

    case MODEL_KIND_MNIST: {
        int slot = rust_model_load_builtin_mnist();
        if (slot < 0) return MODEL_LAUNCH_ERR_FAILED;
        if (out_task_id) *out_task_id = slot;
        return MODEL_LAUNCH_OK;
    }

    case MODEL_KIND_HAILO:
    case MODEL_KIND_GGML:
        return MODEL_LAUNCH_ERR_NOSYS;

    default:
        return MODEL_LAUNCH_ERR_BADKIND;
    }
}
