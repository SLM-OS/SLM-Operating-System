/*
 * fdt.h — General-purpose Flattened Device Tree reader.
 *
 * Read-only, no-allocation API over a firmware-supplied FDT blob.
 * The public surface is path-based node lookup + property read, plus
 * a one-shot convenience. Node iteration helpers can be added here
 * later; everything lives under `kernel/lib/fdt/` so new consumers
 * just add themselves — no platform-specific plumbing needed.
 *
 * Relationship to `kernel/include/dtb.h`:
 *   dtb.h owns the *high-level* `fdt_info_t` struct populated once
 *   at boot with RAM / UART / GIC / timer / CPU counts — extracted
 *   with a bespoke walker.
 *   fdt.h is the *general-purpose* lookup: any property on any node
 *   from any consumer, at any time after `dtb_parse` has run.
 *
 * The two live side-by-side because the boot-time extractor is a
 * closed set (small, predictable, runs before drivers) while the
 * general lookup is for ad-hoc queries drivers make at init time
 * (e.g. MACB reading `local-mac-address`). Both operate on the same
 * flattened tree but answer different questions; neither owns the
 * other.
 *
 * Endianness: the FDT format stores all multi-byte integers big-
 * endian. Property *values* are returned as raw big-endian bytes —
 * callers doing numeric reads must byte-swap themselves (the
 * `fdt_get_u32` convenience does this for the common case).
 *
 * References:
 *   - Devicetree Specification v0.4: https://devicetree.org/
 *   - Linux libfdt: scripts/dtc/libfdt/
 */

#ifndef FDT_H
#define FDT_H

#include <stdint.h>
#include <stddef.h>

/* Return codes. 0 on success, negative on error. */
#define FDT_LIB_OK              0
#define FDT_LIB_E_BADMAGIC      -1  /* header magic not 0xd00dfeed */
#define FDT_LIB_E_BADVERSION    -2  /* version below supported floor */
#define FDT_LIB_E_NOTFOUND      -3  /* path or property not found */
#define FDT_LIB_E_BADSTRUCT     -4  /* malformed structure block */
#define FDT_LIB_E_INVALID       -5  /* NULL argument or bad input */

/*
 * Cached pointers into a validated FDT blob. Create with fdt_init.
 * Cheap to hold: no allocation, no mutation. Safe to construct one
 * per caller or to share a single handle across the kernel. Does
 * NOT copy the blob — the caller must ensure `blob` remains mapped
 * and unmodified for the handle's lifetime (always true for the
 * firmware-supplied DTB, which sits at a fixed physical address).
 */
struct fdt_handle {
    const uint8_t *blob;           /* pointer to fdt_header (native ptr) */
    uint32_t totalsize;            /* CPU-byte-order copies of the header */
    uint32_t off_struct;
    uint32_t size_struct;
    uint32_t off_strings;
    uint32_t size_strings;
    uint32_t version;
};

/*
 * Validate a flattened device tree and cache its offsets in `h`.
 * Checks magic (0xd00dfeed), version (>= 16), and bounds on the
 * struct + strings blocks so a malformed header cannot send the
 * walker past the blob. Does not walk the tree.
 *
 * Returns FDT_LIB_OK / FDT_LIB_E_BADMAGIC / FDT_LIB_E_BADVERSION /
 * FDT_LIB_E_INVALID.
 */
int fdt_init(struct fdt_handle *h, const void *blob);

/*
 * Find a node by absolute path.
 *
 * `path` starts with "/" and uses DTB-spec node names including unit
 * addresses, e.g. "/axi/pcie@1000120000/rp1/ethernet@100000". Passing
 * just "/" selects the root node.
 *
 * On success writes the byte offset of the node's FDT_BEGIN_NODE
 * token, relative to the start of the struct block, into
 * `*out_offset`. That offset is the handle you pass to
 * `fdt_get_property` below.
 *
 * Returns FDT_LIB_OK / FDT_LIB_E_NOTFOUND / FDT_LIB_E_BADSTRUCT /
 * FDT_LIB_E_INVALID.
 */
int fdt_find_node_by_path(const struct fdt_handle *h, const char *path,
                          int *out_offset);

/*
 * Read a property from a specific node.
 *
 * `node_offset` comes from `fdt_find_node_by_path`. `*out_data` is
 * set to a pointer INTO THE DTB (read-only; do not modify or free)
 * and `*out_len` to the property length in bytes. Values are raw —
 * integer properties are big-endian and must be swapped by the
 * caller (see `fdt_get_u32` for the single-cell case).
 *
 * Returns FDT_LIB_OK / FDT_LIB_E_NOTFOUND / FDT_LIB_E_BADSTRUCT /
 * FDT_LIB_E_INVALID.
 */
int fdt_get_property(const struct fdt_handle *h, int node_offset,
                     const char *prop_name,
                     const void **out_data, uint32_t *out_len);

/*
 * One-shot: lookup-then-read. Equivalent to fdt_find_node_by_path +
 * fdt_get_property, returning the earliest error.
 */
int fdt_get_property_by_path(const struct fdt_handle *h,
                             const char *node_path,
                             const char *prop_name,
                             const void **out_data, uint32_t *out_len);

/*
 * Convenience: read a single big-endian u32 property value. Returns
 * FDT_LIB_E_BADSTRUCT if the property exists but isn't exactly 4
 * bytes; otherwise delegates error codes to `fdt_get_property_by_path`.
 */
int fdt_get_u32(const struct fdt_handle *h, const char *node_path,
                const char *prop_name, uint32_t *out_value);

#endif /* FDT_H */
