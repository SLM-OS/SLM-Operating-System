/*
 * dtb.c - Device Tree Blob (DTB) Parser
 *
 * Minimal parser for Flattened Device Tree format.
 * Extracts only the hardware configuration needed by SLM-OS.
 */

#include "dtb.h"
#include "fdt.h"
#include "platform.h"
#include "uart.h"
#include "string.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Global State
 * ============================================================================ */

/* Raw DTB blob pointer saved at `dtb_parse` time, exposed via
 * `dtb_get_blob()` for drivers that want ad-hoc FDT lookups using
 * the general-purpose reader in `kernel/lib/fdt/`. Stays NULL if
 * parsing fails or no DTB was supplied (e.g. x86-64). */
static const void *g_dtb_blob = NULL;

/* /memreserve/ list parsed from the FDT header reserve map. */
static dtb_memreserve_t g_memreserves[DTB_MAX_MEMRESERVES];
static int              g_n_memreserves = 0;

/* /chosen entropy + bootloader info. Zero-initialized; fields stay zero
 * if the corresponding DTB property is absent. */
static dtb_chosen_t g_chosen = {0};

/* Parsed DTB info, initialized with platform.h defaults */
static fdt_info_t g_fdt_info = {
    .ram_base = RAM_BASE,
    .ram_size = RAM_SIZE,
    .uart_base = UART_BASE,
    .uart_irq = UART_IRQ,
    .gic_dist_base = GIC_DIST_BASE,
#if defined(GIC_VERSION) && GIC_VERSION == 3
    .gic_cpu_base = GIC_REDIST_BASE,  /* GICv3: use redistributor base */
#else
    .gic_cpu_base = GIC_CPU_BASE,     /* GICv2: use CPU interface base */
#endif
    .cpu_count = CPU_MAX,
    .timer_irq = TIMER_IRQ,
    .valid = false
};

/* ============================================================================
 * Byte Order Conversion (DTB is big-endian, ARM64 is little-endian)
 * ============================================================================ */

static inline uint32_t be32_to_cpu(uint32_t be)
{
    return ((be & 0xff000000) >> 24) |
           ((be & 0x00ff0000) >> 8) |
           ((be & 0x0000ff00) << 8) |
           ((be & 0x000000ff) << 24);
}

static inline uint64_t be64_to_cpu(uint64_t be)
{
    return ((uint64_t)be32_to_cpu((uint32_t)be) << 32) |
           (uint64_t)be32_to_cpu((uint32_t)(be >> 32));
}

/* ============================================================================
 * String Helpers
 * ============================================================================ */

/* Check if s starts with prefix */
static bool fdt_strstart(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * DTB Navigation Helpers
 * ============================================================================ */

/* Align offset to 4-byte boundary */
static inline uint32_t fdt_align(uint32_t offset)
{
    return (offset + 3) & ~3;
}

/* Get string from strings block */
static const char *fdt_get_string(const void *dtb, uint32_t offset)
{
    const struct fdt_header *hdr = dtb;
    uint32_t strings_off = be32_to_cpu(hdr->off_dt_strings);
    return (const char *)dtb + strings_off + offset;
}

/* ============================================================================
 * Property Parsing
 * ============================================================================ */

/* Property header in structure block */
struct fdt_prop_header {
    uint32_t len;       /* Length of value */
    uint32_t nameoff;   /* Offset into strings block */
};

/* Read a 32-bit cell from property data */
static uint32_t fdt_read_cell32(const uint8_t *data, int cell_index)
{
    const uint32_t *cells = (const uint32_t *)data;
    return be32_to_cpu(cells[cell_index]);
}

/* Read a 64-bit value from property data (2 cells) */
static uint64_t fdt_read_cell64(const uint8_t *data, int cell_index)
{
    uint64_t high = fdt_read_cell32(data, cell_index);
    uint64_t low = fdt_read_cell32(data, cell_index + 1);
    return (high << 32) | low;
}

/* ============================================================================
 * Structure Block Parser
 *
 * The structure block contains a sequence of tokens:
 *   FDT_BEGIN_NODE + name + padding + properties/children + FDT_END_NODE
 *   FDT_PROP + len + nameoff + value + padding
 *   FDT_END (end of tree)
 * ============================================================================ */

/* Parser context */
struct fdt_parser {
    const void *dtb;
    const uint8_t *struct_base;
    const uint8_t *struct_end;
    uint32_t pos;           /* Current position in structure block */
    int depth;              /* Current node depth */

    /* Current node state */
    uint32_t address_cells; /* #address-cells (default 2) */
    uint32_t size_cells;    /* #size-cells (default 1) */
};

/* Initialize parser */
static int fdt_parser_init(struct fdt_parser *p, const void *dtb)
{
    const struct fdt_header *hdr = dtb;

    p->dtb = dtb;
    p->struct_base = (const uint8_t *)dtb + be32_to_cpu(hdr->off_dt_struct);
    p->struct_end = p->struct_base + be32_to_cpu(hdr->size_dt_struct);
    p->pos = 0;
    p->depth = 0;
    p->address_cells = 2;
    p->size_cells = 1;

    return FDT_OK;
}

/* Get current token */
static uint32_t fdt_token(struct fdt_parser *p)
{
    if (p->struct_base + p->pos + 4 > p->struct_end) {
        return FDT_END;
    }
    return be32_to_cpu(*(const uint32_t *)(p->struct_base + p->pos));
}

/* Skip to next token after node name (currently unused, but retained for future use) */
#if 0
static void fdt_skip_name(struct fdt_parser *p)
{
    p->pos += 4; /* Skip token */
    const char *name = (const char *)(p->struct_base + p->pos);
    p->pos += strlen(name) + 1;
    p->pos = fdt_align(p->pos);
}
#endif

/* Parse one node and its properties, calling callback for each property */
typedef void (*prop_callback_t)(const char *node_name, const char *prop_name,
                                const uint8_t *data, uint32_t len,
                                struct fdt_parser *p, fdt_info_t *info);

static void fdt_walk_tree(struct fdt_parser *p, fdt_info_t *info,
                          prop_callback_t callback)
{
    char current_node[64] = "";
    int target_depth = 0;

    while (p->pos < (p->struct_end - p->struct_base)) {
        uint32_t token = fdt_token(p);

        switch (token) {
        case FDT_BEGIN_NODE: {
            p->pos += 4;
            const char *name = (const char *)(p->struct_base + p->pos);

            /* Track node path at depth 1 (direct children of root) */
            if (p->depth == 0 && name[0] != '\0') {
                /* This is a root child - store its name */
                size_t len = strlen(name);
                if (len >= sizeof(current_node)) {
                    len = sizeof(current_node) - 1;
                }
                for (size_t i = 0; i < len; i++) {
                    current_node[i] = name[i];
                }
                current_node[len] = '\0';
                target_depth = 1;
            } else if (p->depth == 1 && strcmp(current_node, "cpus") == 0) {
                /* Inside /cpus - track cpu@ nodes */
                if (fdt_strstart(name, "cpu@")) {
                    info->cpu_count++;
                }
            }

            p->pos += strlen(name) + 1;
            p->pos = fdt_align(p->pos);
            p->depth++;
            break;
        }

        case FDT_END_NODE:
            p->pos += 4;
            p->depth--;
            if (p->depth == 0) {
                current_node[0] = '\0';
            }
            break;

        case FDT_PROP: {
            p->pos += 4;
            const struct fdt_prop_header *prop =
                (const struct fdt_prop_header *)(p->struct_base + p->pos);
            uint32_t len = be32_to_cpu(prop->len);
            uint32_t nameoff = be32_to_cpu(prop->nameoff);

            p->pos += sizeof(struct fdt_prop_header);
            const uint8_t *data = p->struct_base + p->pos;
            const char *prop_name = fdt_get_string(p->dtb, nameoff);

            /* Update address/size cells if found */
            if (strcmp(prop_name, "#address-cells") == 0 && len == 4) {
                p->address_cells = fdt_read_cell32(data, 0);
            } else if (strcmp(prop_name, "#size-cells") == 0 && len == 4) {
                p->size_cells = fdt_read_cell32(data, 0);
            }

            /* Call property callback */
            if (callback && p->depth == target_depth + 1) {
                callback(current_node, prop_name, data, len, p, info);
            }

            p->pos += len;
            p->pos = fdt_align(p->pos);
            break;
        }

        case FDT_NOP:
            p->pos += 4;
            break;

        case FDT_END:
            return;

        default:
            /* Unknown token - abort */
            return;
        }
    }
}

/* ============================================================================
 * Property Extraction Callback
 * ============================================================================ */

static void extract_property(const char *node_name, const char *prop_name,
                             const uint8_t *data, uint32_t len,
                             struct fdt_parser *p, fdt_info_t *info)
{
    /* Memory node: memory@... */
    if (fdt_strstart(node_name, "memory")) {
        if (strcmp(prop_name, "reg") == 0) {
            /* reg = <base size> using address-cells and size-cells */
            if (p->address_cells == 2 && len >= 8) {
                info->ram_base = fdt_read_cell64(data, 0);
            } else if (p->address_cells == 1 && len >= 4) {
                info->ram_base = fdt_read_cell32(data, 0);
            }

            uint32_t size_offset = p->address_cells;
            if (p->size_cells == 2 && len >= (size_offset + 2) * 4) {
                info->ram_size = fdt_read_cell64(data, (int)size_offset);
            } else if (p->size_cells == 1 && len >= (size_offset + 1) * 4) {
                info->ram_size = fdt_read_cell32(data, (int)size_offset);
            }
        }
    }

    /* PL011 UART: pl011@... or uart@... or serial@... */
    if (fdt_strstart(node_name, "pl011") ||
        fdt_strstart(node_name, "uart") ||
        fdt_strstart(node_name, "serial")) {
        if (strcmp(prop_name, "reg") == 0 && len >= 8) {
            /* First reg entry is base address */
            if (p->address_cells == 2) {
                info->uart_base = fdt_read_cell64(data, 0);
            } else {
                info->uart_base = fdt_read_cell32(data, 0);
            }
        } else if (strcmp(prop_name, "interrupts") == 0 && len >= 12) {
            /* interrupts = <type irq flags> - IRQ is second cell */
            info->uart_irq = fdt_read_cell32(data, 1) + 32; /* SPI offset */
        }
    }

    /* GIC: intc@... or gic@... or interrupt-controller@... */
    if (fdt_strstart(node_name, "intc") ||
        fdt_strstart(node_name, "gic") ||
        fdt_strstart(node_name, "interrupt-controller")) {
        if (strcmp(prop_name, "reg") == 0 && len >= 16) {
            /* reg = <dist_base dist_size cpu_base cpu_size ...> */
            if (p->address_cells == 2) {
                info->gic_dist_base = fdt_read_cell64(data, 0);
                uint32_t cpu_offset = p->address_cells + p->size_cells;
                if (len >= (cpu_offset + 2) * 4) {
                    info->gic_cpu_base = fdt_read_cell64(data, (int)cpu_offset);
                }
            } else {
                info->gic_dist_base = fdt_read_cell32(data, 0);
                uint32_t cpu_offset = p->address_cells + p->size_cells;
                if (len >= (cpu_offset + 1) * 4) {
                    info->gic_cpu_base = fdt_read_cell32(data, (int)cpu_offset);
                }
            }
        }
    }

    /* Timer: timer@... */
    if (fdt_strstart(node_name, "timer")) {
        if (strcmp(prop_name, "interrupts") == 0 && len >= 12) {
            /* Look for virtual timer (usually third entry) */
            /* interrupts = <type irq flags> repeated for each timer */
            /* Secure, Non-secure, Virtual, Hypervisor */
            if (len >= 36) {
                /* Virtual timer is third entry (offset 6 cells) */
                info->timer_irq = fdt_read_cell32(data, 7); /* IRQ number */
            } else {
                /* Use first timer IRQ if only one present */
                info->timer_irq = fdt_read_cell32(data, 1);
            }
        }
    }
}

/* ============================================================================
 * /memreserve/ + /chosen parsers (firmware-supplied side-band info)
 * ============================================================================ */

/* Walk the FDT header's reserve map. Each entry is two big-endian uint64
 * (addr, size); the list ends with a (0, 0) pair. This is what `dtc`
 * emits for `/memreserve/` directives at DTS file scope.
 *
 * Bounds checks include explicit u32 overflow guards (`a + b < a`) so a
 * crafted header with `off` near UINT32_MAX can't wrap past the totalsize
 * limit. Mirrors the same checks `fdt_init` does (kernel/lib/fdt/fdt.c). */
static void parse_memreserves_from_header(const void *dtb)
{
    const struct fdt_header *hdr = dtb;
    uint32_t off = be32_to_cpu(hdr->off_mem_rsvmap);
    if (off == 0) return;

    uint32_t total = be32_to_cpu(hdr->totalsize);
    if (off + 16 < off || off + 16 > total) return;

    const uint8_t *base = (const uint8_t *)dtb + off;
    while (g_n_memreserves < DTB_MAX_MEMRESERVES) {
        uint32_t entry_off = off + (uint32_t)g_n_memreserves * 16;
        /* entry_off must not wrap and (entry_off + 16) must be in-bounds. */
        if (entry_off < off ||
            entry_off + 16 < entry_off ||
            entry_off + 16 > total) break;
        const uint8_t *p = base + (uint32_t)g_n_memreserves * 16;
        uint64_t addr = be64_to_cpu(*(const uint64_t *)(p + 0));
        uint64_t size = be64_to_cpu(*(const uint64_t *)(p + 8));
        if (addr == 0 && size == 0) break;
        g_memreserves[g_n_memreserves].addr = addr;
        g_memreserves[g_n_memreserves].size = size;
        g_n_memreserves++;
    }
}

/* Pi firmware encodes its VPU carveout as a `memreserve` property of the
 * root node — non-standard but empirically used. Property content is a
 * sequence of (big-endian u32 addr, u32 size) cell pairs, totaling 8
 * bytes per reservation, NOT the 16-byte u64 pairs used in the FDT
 * header reserve map. Empirical evidence (pi-5-1 with EEPROM
 * `pieeprom-2024-09-23.bin`):
 *   off_mem_rsvmap → zero-only terminator
 *   / { memreserve = <0x3fc00000 0x400000>; } → 8 bytes of property data
 *
 * Walked directly off the structure block here rather than via
 * `fdt_get_property_by_path` to avoid an early-boot dependency on the
 * full path-walking machinery — this needs to run before banner output
 * and silent failures here have no diagnostic surface. The walk is
 * tightly bounded: we only inspect properties of the root node (offset
 * 0 → first FDT_BEGIN_NODE), stopping at the first non-property token.
 * Reading both this AND the FDT header reserve map keeps SLM-OS correct
 * under either convention without detecting which one firmware picked. */
static void parse_memreserves_from_root_property(const void *dtb)
{
    const struct fdt_header *hdr = dtb;
    uint32_t total       = be32_to_cpu(hdr->totalsize);
    uint32_t off_struct  = be32_to_cpu(hdr->off_dt_struct);
    uint32_t size_struct = be32_to_cpu(hdr->size_dt_struct);
    uint32_t off_strings = be32_to_cpu(hdr->off_dt_strings);
    uint32_t size_strings = be32_to_cpu(hdr->size_dt_strings);

    /* Guard against malformed headers — same overflow-safe checks
     * `fdt_init` does (off + size must not wrap and must fit in total). */
    if (off_struct + size_struct < off_struct ||
        off_struct + size_struct > total) return;
    if (off_strings + size_strings < off_strings ||
        off_strings + size_strings > total) return;

    const uint8_t *p   = (const uint8_t *)dtb + off_struct;
    const uint8_t *end = p + size_struct;
    const char *strings = (const char *)dtb + off_strings;

    /* Root must open with FDT_BEGIN_NODE + (typically empty) name. */
    if (p + 4 > end || be32_to_cpu(*(const uint32_t *)p) != FDT_BEGIN_NODE) return;
    p += 4;
    /* Skip root name (null-terminated, padded to 4 bytes). */
    while (p < end && *p != '\0') p++;
    if (p >= end) return;
    p++;
    p = (const uint8_t *)dtb + fdt_align((uint32_t)(p - (const uint8_t *)dtb));
    if (p >= end) return;

    /* Walk root's properties only — stop at the first child BEGIN_NODE
     * or our own END_NODE. */
    while (p + 4 <= end) {
        uint32_t tok = be32_to_cpu(*(const uint32_t *)p);
        p += 4;
        if (tok == FDT_NOP) continue;
        if (tok != FDT_PROP) break;
        if (p + 8 > end) break;
        uint32_t plen     = be32_to_cpu(*(const uint32_t *)p);
        uint32_t pnameoff = be32_to_cpu(*(const uint32_t *)(p + 4));
        p += 8;
        if (pnameoff >= size_strings) break;
        const char *pname = strings + pnameoff;
        if (p + plen > end) break;

        /* Match "memreserve". Property name is bounded by the strings
         * block; we stop at NUL or block end. */
        bool match = true;
        const char *want = "memreserve";
        for (uint32_t i = 0; ; i++) {
            char c = (pnameoff + i < size_strings) ? pname[i] : '\0';
            if (c != want[i]) { match = false; break; }
            if (c == '\0') break;
        }

        if (match) {
            /* Cells: u32 addr, u32 size. Pair length = 8 bytes. */
            for (uint32_t off = 0; off + 8 <= plen; off += 8) {
                if (g_n_memreserves >= DTB_MAX_MEMRESERVES) break;
                uint32_t addr = be32_to_cpu(*(const uint32_t *)(p + off + 0));
                uint32_t size = be32_to_cpu(*(const uint32_t *)(p + off + 4));
                if (addr == 0 && size == 0) continue;
                g_memreserves[g_n_memreserves].addr = addr;
                g_memreserves[g_n_memreserves].size = size;
                g_n_memreserves++;
            }
            break;  /* Found and processed; stop scanning. */
        }

        /* Advance past the property value, padded to 4-byte alignment.
         * Guard against `align4(plen) < plen` overflow on a malformed
         * DTB — without this, a crafted plen near UINT32_MAX wraps to
         * 0 and the outer walk spins forever. Matches the same guard
         * `fdt_get_property` does in kernel/lib/fdt/fdt.c. */
        uint32_t skip = fdt_align(plen);
        if (skip < plen) break;
        p += skip;
    }
}

static void parse_memreserves(const void *dtb)
{
    g_n_memreserves = 0;
    parse_memreserves_from_header(dtb);
    parse_memreserves_from_root_property(dtb);
}

static void copy_chosen_bytes(const struct fdt_handle *h, const char *path,
                              const char *prop, uint8_t *out, uint32_t cap,
                              uint32_t *out_len)
{
    *out_len = 0;
    const void *data = NULL;
    uint32_t len = 0;
    if (fdt_get_property_by_path(h, path, prop, &data, &len) != FDT_LIB_OK) {
        return;
    }
    uint32_t n = len < cap ? len : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = ((const uint8_t *)data)[i];
    *out_len = n;
}

static void copy_chosen_string(const struct fdt_handle *h, const char *path,
                               const char *prop, char *out, uint32_t cap)
{
    /* Reject zero-cap before any write. The current callsite passes a
     * compile-time positive cap, but the function should be defensive
     * against future callers — out[0]='\0' on a zero-byte buffer would
     * be an out-of-bounds write. */
    if (cap == 0) return;
    out[0] = '\0';
    const void *data = NULL;
    uint32_t len = 0;
    if (fdt_get_property_by_path(h, path, prop, &data, &len) != FDT_LIB_OK) {
        return;
    }
    if (len == 0) return;
    uint32_t n = len < cap - 1 ? len : cap - 1;
    for (uint32_t i = 0; i < n; i++) out[i] = ((const char *)data)[i];
    out[n] = '\0';
}

static void parse_chosen(const void *dtb)
{
    g_chosen = (dtb_chosen_t){0};

    struct fdt_handle h;
    if (fdt_init(&h, dtb) != FDT_LIB_OK) return;

    copy_chosen_bytes(&h, "/chosen", "rng-seed",
                      g_chosen.rng_seed, sizeof(g_chosen.rng_seed),
                      &g_chosen.rng_seed_len);
    copy_chosen_bytes(&h, "/chosen", "kaslr-seed",
                      g_chosen.kaslr_seed, sizeof(g_chosen.kaslr_seed),
                      &g_chosen.kaslr_seed_len);
    copy_chosen_string(&h, "/chosen/bootloader", "version",
                       g_chosen.bootloader_version,
                       sizeof(g_chosen.bootloader_version));
    (void)fdt_get_u32(&h, "/chosen/bootloader", "capabilities",
                      &g_chosen.bootloader_capabilities);
    (void)fdt_get_u32(&h, "/chosen/bootloader", "build-timestamp",
                      &g_chosen.bootloader_build_timestamp);
    (void)fdt_get_u32(&h, "/chosen/bootloader", "update-timestamp",
                      &g_chosen.bootloader_update_timestamp);
    copy_chosen_string(&h, "/chosen", "bootargs",
                       g_chosen.bootargs, sizeof(g_chosen.bootargs));
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int dtb_validate(const void *dtb)
{
    if (!dtb) {
        return FDT_ERR_BADPTR;
    }

    const struct fdt_header *hdr = dtb;
    uint32_t magic = be32_to_cpu(hdr->magic);

    if (magic != FDT_MAGIC) {
        return FDT_ERR_BADMAGIC;
    }

    uint32_t version = be32_to_cpu(hdr->version);
    if (version < FDT_VERSION) {
        return FDT_ERR_BADVERSION;
    }

    /* Bounds-check structure block: off_dt_struct + size_dt_struct must
     * not overflow and must lie within totalsize. Defends against crafted
     * headers that could cause the parser to walk past the DTB into
     * arbitrary memory. */
    uint32_t off = be32_to_cpu(hdr->off_dt_struct);
    uint32_t sz  = be32_to_cpu(hdr->size_dt_struct);
    uint32_t total = be32_to_cpu(hdr->totalsize);
    if (off + sz < off || off + sz > total) {
        return FDT_ERR_BADSTRUCT;
    }

    return FDT_OK;
}

int dtb_parse(const void *dtb, fdt_info_t *info)
{
    if (!info) {
        return FDT_ERR_BADPTR;
    }

    /* Start with platform defaults */
    info->ram_base = RAM_BASE;
    info->ram_size = RAM_SIZE;
    info->uart_base = UART_BASE;
    info->uart_irq = UART_IRQ;
    info->gic_dist_base = GIC_DIST_BASE;
#if defined(GIC_VERSION) && GIC_VERSION == 3
    info->gic_cpu_base = GIC_REDIST_BASE;  /* GICv3: use redistributor base */
#else
    info->gic_cpu_base = GIC_CPU_BASE;     /* GICv2: use CPU interface base */
#endif
    info->cpu_count = 0;  /* Will be counted from /cpus */
    info->timer_irq = TIMER_IRQ;
    info->valid = false;

    /* Validate DTB */
    int err = dtb_validate(dtb);
    if (err != FDT_OK) {
        /* Use platform defaults */
        info->cpu_count = CPU_MAX;
        return err;
    }

    /* Parse structure block */
    struct fdt_parser parser;
    fdt_parser_init(&parser, dtb);
    fdt_walk_tree(&parser, info, extract_property);

    /* If no CPUs found, use default */
    if (info->cpu_count == 0) {
        info->cpu_count = CPU_MAX;
    }

    info->valid = true;

    /* Update global state */
    g_fdt_info = *info;
    g_dtb_blob = dtb;

    /* Side-band info: /memreserve/ list + /chosen entropy and bootloader
     * metadata. Failures here are non-fatal — they leave the corresponding
     * globals at zero, and consumers (PMM, RNG, boot banner) treat absent
     * as "skip this enrichment." */
    parse_memreserves(dtb);
    parse_chosen(dtb);

    return FDT_OK;
}

const void *dtb_get_blob(void)
{
    return g_dtb_blob;
}

int dtb_get_memreserves(dtb_memreserve_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = g_n_memreserves < max ? g_n_memreserves : max;
    for (int i = 0; i < n; i++) out[i] = g_memreserves[i];
    return n;
}

const dtb_chosen_t *dtb_get_chosen(void)
{
    return &g_chosen;
}

void dtb_print_info(const fdt_info_t *info)
{
    if (!info) {
        uart_printf("DTB: (null)\n");
        return;
    }

    uart_printf("Device Tree Info:\n");
    uart_printf("  Valid:     %s\n", info->valid ? "yes" : "no (using defaults)");
    uart_printf("  RAM:       0x%lx - 0x%lx (%lu MB)\n",
                info->ram_base,
                info->ram_base + info->ram_size,
                info->ram_size / (1024 * 1024));
    uart_printf("  UART:      0x%lx (IRQ %u)\n",
                info->uart_base, info->uart_irq);
    uart_printf("  GIC dist:  0x%lx\n", info->gic_dist_base);
    uart_printf("  GIC cpu:   0x%lx\n", info->gic_cpu_base);
    uart_printf("  CPUs:      %u\n", info->cpu_count);
    uart_printf("  Timer IRQ: %u\n", info->timer_irq);

    /* Firmware-supplied side-band info (parsed at dtb_parse time). */
    if (g_n_memreserves > 0) {
        uart_printf("  /memreserve/:\n");
        for (int i = 0; i < g_n_memreserves; i++) {
            uart_printf("    [%d] 0x%lx + 0x%lx\n", i,
                        (unsigned long)g_memreserves[i].addr,
                        (unsigned long)g_memreserves[i].size);
        }
    }
    if (g_chosen.bootloader_version[0] != '\0') {
        uart_printf("  Bootloader:\n");
        uart_printf("    version:      %s\n", g_chosen.bootloader_version);
        uart_printf("    capabilities: 0x%lx\n",
                    (unsigned long)g_chosen.bootloader_capabilities);
        uart_printf("    build-ts:     0x%lx\n",
                    (unsigned long)g_chosen.bootloader_build_timestamp);
    }
    if (g_chosen.rng_seed_len > 0 || g_chosen.kaslr_seed_len > 0) {
        uart_printf("  Firmware entropy: rng-seed=%u B, kaslr-seed=%u B\n",
                    (unsigned)g_chosen.rng_seed_len,
                    (unsigned)g_chosen.kaslr_seed_len);
    }
}

const fdt_info_t *dtb_get_info(void)
{
    return &g_fdt_info;
}
