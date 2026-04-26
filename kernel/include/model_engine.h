/*
 * model_engine.h - Model kind/engine registry (admin & telemetry, M5).
 *
 * Spec §10 promises a `model launch <name>` command that reads a
 * `/mnt/models/<name>.meta` sidecar, looks up the right engine for the
 * declared `kind`, and instantiates the model. M5 ships the registry,
 * the .meta parser, and the launch surface; only the `mnist` and `raw`
 * kinds have real engine paths today. `hailo` and `ggml` are stubs
 * that return ENOSYS until their respective drivers / runtimes land.
 *
 * Kinds:
 *   RAW    — "load and report; do not run". Useful for shipping a
 *            blob to the device for a separate consumer (e.g. policy
 *            hot-swap target). `launch` is a no-op success.
 *   MNIST  — built-in MNIST graph. `launch` calls into the existing
 *            `rust_model_load_builtin_mnist` path and returns the
 *            assigned slot index as task_id.
 *   HAILO  — Hailo-8 NPU (M5 stub).
 *   GGML   — generic ggml runtime (M5 stub).
 */

#ifndef MODEL_ENGINE_H
#define MODEL_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum model_kind {
    MODEL_KIND_RAW   = 0,
    MODEL_KIND_MNIST = 1,
    MODEL_KIND_HAILO = 2,
    MODEL_KIND_GGML  = 3,
    MODEL_KIND_COUNT,
};

/* Stable lowercase name for a kind. NULL for out-of-range input. */
const char *model_kind_name(enum model_kind kind);

/* Lookup a kind by lowercase ASCII name. Returns MODEL_KIND_COUNT
 * when the name is not recognised. */
enum model_kind model_kind_from_name(const char *name);

/* Per-engine status. Read via `model_engine_status()` to render the
 * `model engines` command. */
enum model_engine_state {
    MODEL_ENGINE_READY    = 0,  /* implemented and usable */
    MODEL_ENGINE_NOSYS    = 1,  /* registered name + ENOSYS stub */
    MODEL_ENGINE_DISABLED = 2,  /* compiled-out on this build */
};

struct model_engine_info {
    const char *name;            /* "raw" / "mnist" / "hailo" / "ggml" */
    enum model_kind kind;
    enum model_engine_state state;
    const char *summary;         /* short status string for shell tabular output */
};

/* Return the static info for `kind`. NULL on out-of-range. */
const struct model_engine_info *model_engine_info_get(enum model_kind kind);

/* Snapshot all engines into `out` (caller-supplied array of MODEL_KIND_COUNT
 * pointers). Returns the populated count. */
size_t model_engine_info_list(const struct model_engine_info *out[MODEL_KIND_COUNT]);

/* Result codes for `model_engine_launch`. Negative for failure. */
#define MODEL_LAUNCH_OK            0
#define MODEL_LAUNCH_ERR_NOMETA   (-1)  /* /mnt/models/<name>.meta missing or unreadable */
#define MODEL_LAUNCH_ERR_BADMETA  (-2)  /* meta file present but malformed */
#define MODEL_LAUNCH_ERR_BADKIND  (-3)  /* meta declares an unknown kind */
#define MODEL_LAUNCH_ERR_NOSYS    (-4)  /* engine for kind is a stub */
#define MODEL_LAUNCH_ERR_FAILED   (-5)  /* engine ran but returned failure */

/* Parsed .meta sidecar layout. The on-disk format is one
 * key=value pair per line; whitespace ignored. Strings and
 * counts are bounded so the entire structure fits on a small
 * stack allocation. */
#define MODEL_META_NAME_LEN     32u
#define MODEL_META_SHA256_LEN   65u  /* 64 hex chars + nul */

struct model_meta {
    char     name[MODEL_META_NAME_LEN];
    enum model_kind kind;
    uint64_t size;            /* bytes */
    char     sha256[MODEL_META_SHA256_LEN];  /* lowercase hex; "" if absent */
    uint64_t uploaded_ts_ms;  /* 0 if absent */
};

/* Parse a raw .meta sidecar buffer (key=value lines). On success,
 * fills `out`; returns 0. On malformed input, returns
 * MODEL_LAUNCH_ERR_BADMETA. The buffer is read-only; the parser does
 * not write back. */
int model_meta_parse(const char *buf, size_t len, struct model_meta *out);

/* Read /mnt/models/<name>.meta from VFS and parse it.  Returns
 * MODEL_LAUNCH_OK on success or one of the negative codes above. */
int model_meta_read(const char *name, struct model_meta *out);

/* Dispatch launch. Returns MODEL_LAUNCH_OK and sets `*out_task_id` on
 * success; otherwise returns one of the negative codes and leaves
 * `*out_task_id` untouched. The output is also surfaced via the
 * shell command's printout. */
int model_engine_launch(const char *name, int *out_task_id);

#endif /* MODEL_ENGINE_H */
