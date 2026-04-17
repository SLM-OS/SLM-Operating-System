/*
 * fdt.c — General-purpose Flattened Device Tree reader.
 *
 * See `kernel/include/fdt.h` for API contract and invariants.
 *
 * Implementation notes:
 *   - Structural token constants (FDT_MAGIC / FDT_BEGIN_NODE / etc.)
 *     are owned by `kernel/include/dtb.h` — we include it here rather
 *     than redefining them, so the two DTB consumers can't drift.
 *   - The walker is an iterative depth-tracker with no recursion and
 *     no dynamic allocation: bounded stack, bounded work per token.
 *   - Path matching is exact-string against node names including
 *     unit addresses (the `@...` suffix). Callers that want alias
 *     resolution can read `/aliases/<name>` with `fdt_get_property`
 *     and pass the resulting path back in.
 *   - All accesses go through `read_be32` which does a byte-wise
 *     load — handles unaligned DTB pointers safely.
 */

#include "fdt.h"
#include "dtb.h"       /* shared structural constants */
#include <string.h>

static inline uint32_t read_be32(const void *p)
{
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) |
           ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |
           ((uint32_t)b[3]);
}

static inline uint32_t align4(uint32_t v)
{
    return (v + 3u) & ~3u;
}

int fdt_init(struct fdt_handle *h, const void *blob)
{
    if (!h || !blob) {
        return FDT_LIB_E_INVALID;
    }

    const struct fdt_header *hdr = blob;
    uint32_t magic = read_be32(&hdr->magic);
    if (magic != FDT_MAGIC) {
        return FDT_LIB_E_BADMAGIC;
    }

    uint32_t version = read_be32(&hdr->version);
    if (version < FDT_VERSION) {
        return FDT_LIB_E_BADVERSION;
    }

    uint32_t totalsize    = read_be32(&hdr->totalsize);
    uint32_t off_struct   = read_be32(&hdr->off_dt_struct);
    uint32_t size_struct  = read_be32(&hdr->size_dt_struct);
    uint32_t off_strings  = read_be32(&hdr->off_dt_strings);
    uint32_t size_strings = read_be32(&hdr->size_dt_strings);

    /* Bounds-check: both blocks must fit inside the declared total
     * size. Overflow-safe (unsigned additions checked against total). */
    if (off_struct  + size_struct  < off_struct  ||
        off_strings + size_strings < off_strings ||
        off_struct  + size_struct  > totalsize   ||
        off_strings + size_strings > totalsize) {
        return FDT_LIB_E_BADSTRUCT;
    }

    h->blob         = (const uint8_t *)blob;
    h->totalsize    = totalsize;
    h->off_struct   = off_struct;
    h->size_struct  = size_struct;
    h->off_strings  = off_strings;
    h->size_strings = size_strings;
    h->version      = version;
    return FDT_LIB_OK;
}

/*
 * Compare a node name against one path component. A DTB node name
 * is a null-terminated string; `comp` is a substring of the path
 * without a null terminator (delimited by `/` or path end). A match
 * requires both exact length and byte-for-byte equality, so unit
 * addresses in the path (`@...`) must appear verbatim.
 */
static int name_matches(const char *name, const char *comp, size_t comp_len)
{
    size_t nlen = strlen(name);
    return nlen == comp_len && memcmp(name, comp, comp_len) == 0;
}

int fdt_find_node_by_path(const struct fdt_handle *h, const char *path,
                          int *out_offset)
{
    if (!h || !path || !out_offset || path[0] != '/') {
        return FDT_LIB_E_INVALID;
    }

    const uint8_t *struct_base = h->blob + h->off_struct;
    const uint8_t *p = struct_base;
    const uint8_t *end = struct_base + h->size_struct;

    /* The very first token must be BEGIN_NODE for the root (name
     * string is usually empty). Consume it unconditionally — it
     * represents the "/" depth. */
    if (p + 4 > end) {
        return FDT_LIB_E_BADSTRUCT;
    }
    if (read_be32(p) != FDT_BEGIN_NODE) {
        return FDT_LIB_E_BADSTRUCT;
    }
    int root_offset = 0;
    p += 4;
    /* Skip root name (null-terminated, padded to u32). */
    size_t rlen = strlen((const char *)p);
    p += align4((uint32_t)(rlen + 1));

    /* Root-only lookup. */
    if (path[1] == '\0') {
        *out_offset = root_offset;
        return FDT_LIB_OK;
    }

    /* `cur` points at the next component to match (without leading /).
     * depth tracks how many BEGIN_NODEs we're inside relative to root.
     * matched_depth tracks how many of those match the path prefix —
     * when they're equal we're on the path; when they diverge we're
     * in a subtree to skip. */
    const char *cur = path + 1;
    int depth = 0;
    int matched_depth = 0;

    while (p + 4 <= end) {
        uint32_t tok = read_be32(p);
        p += 4;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *nname = (const char *)p;
            size_t nlen = strlen(nname);
            int node_offset = (int)(p - 4 - struct_base);
            p += align4((uint32_t)(nlen + 1));
            depth++;

            /* Only inspect siblings at the level we're still
             * matching; deeper nodes in unmatched subtrees are just
             * skipped. */
            if (depth == matched_depth + 1 && *cur) {
                /* Extract the current component length. */
                const char *slash = cur;
                while (*slash && *slash != '/') slash++;
                size_t clen = (size_t)(slash - cur);

                if (name_matches(nname, cur, clen)) {
                    matched_depth++;
                    cur = (*slash == '/') ? slash + 1 : slash; /* past '/' */
                    if (*cur == '\0') {
                        /* Full path consumed — this is our target. */
                        *out_offset = node_offset;
                        return FDT_LIB_OK;
                    }
                }
            }
            break;
        }

        case FDT_END_NODE:
            if (depth == 0) {
                /* Unbalanced END_NODE — walked past the root. */
                return FDT_LIB_E_BADSTRUCT;
            }
            if (depth == matched_depth) {
                matched_depth--;
            }
            depth--;
            break;

        case FDT_PROP: {
            /* Skip over property: 4-byte len, 4-byte nameoff, len
             * bytes of value, align to 4. */
            if (p + 8 > end) {
                return FDT_LIB_E_BADSTRUCT;
            }
            uint32_t len = read_be32(p);
            p += 8;
            p += align4(len);
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return FDT_LIB_E_NOTFOUND;

        default:
            return FDT_LIB_E_BADSTRUCT;
        }
    }

    return FDT_LIB_E_BADSTRUCT;
}

int fdt_get_property(const struct fdt_handle *h, int node_offset,
                     const char *prop_name,
                     const void **out_data, uint32_t *out_len)
{
    if (!h || !prop_name || !out_data || !out_len || node_offset < 0) {
        return FDT_LIB_E_INVALID;
    }

    const uint8_t *struct_base = h->blob + h->off_struct;
    const uint8_t *end = struct_base + h->size_struct;
    const char *strings = (const char *)(h->blob + h->off_strings);
    const uint8_t *p = struct_base + node_offset;

    if (p + 4 > end || read_be32(p) != FDT_BEGIN_NODE) {
        return FDT_LIB_E_BADSTRUCT;
    }
    p += 4;
    /* Skip node name. */
    size_t nlen = strlen((const char *)p);
    p += align4((uint32_t)(nlen + 1));

    /* Scan properties + NOPs until we hit a child BEGIN_NODE or our
     * own END_NODE. We deliberately do NOT recurse into children —
     * the caller wants properties of *this* node. */
    while (p + 4 <= end) {
        uint32_t tok = read_be32(p);
        p += 4;

        switch (tok) {
        case FDT_PROP: {
            if (p + 8 > end) {
                return FDT_LIB_E_BADSTRUCT;
            }
            uint32_t len     = read_be32(p);
            uint32_t nameoff = read_be32(p + 4);
            p += 8;
            const char *pname = strings + nameoff;
            /* Bound-check the strings offset. */
            if (pname + 1 > (const char *)(h->blob + h->off_strings + h->size_strings)) {
                return FDT_LIB_E_BADSTRUCT;
            }
            if (strcmp(pname, prop_name) == 0) {
                *out_data = p;
                *out_len  = len;
                return FDT_LIB_OK;
            }
            p += align4(len);
            break;
        }

        case FDT_NOP:
            break;

        case FDT_BEGIN_NODE:
        case FDT_END_NODE:
        case FDT_END:
            /* Node's property list is over — property not present. */
            return FDT_LIB_E_NOTFOUND;

        default:
            return FDT_LIB_E_BADSTRUCT;
        }
    }

    return FDT_LIB_E_BADSTRUCT;
}

int fdt_get_property_by_path(const struct fdt_handle *h,
                             const char *node_path,
                             const char *prop_name,
                             const void **out_data, uint32_t *out_len)
{
    int node_off;
    int rc = fdt_find_node_by_path(h, node_path, &node_off);
    if (rc != FDT_LIB_OK) {
        return rc;
    }
    return fdt_get_property(h, node_off, prop_name, out_data, out_len);
}

int fdt_get_u32(const struct fdt_handle *h, const char *node_path,
                const char *prop_name, uint32_t *out_value)
{
    if (!out_value) {
        return FDT_LIB_E_INVALID;
    }
    const void *data;
    uint32_t len;
    int rc = fdt_get_property_by_path(h, node_path, prop_name, &data, &len);
    if (rc != FDT_LIB_OK) {
        return rc;
    }
    if (len != 4) {
        return FDT_LIB_E_BADSTRUCT;
    }
    *out_value = read_be32(data);
    return FDT_LIB_OK;
}
