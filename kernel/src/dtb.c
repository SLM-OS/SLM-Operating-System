/*
 * dtb.c - Device Tree Blob (DTB) Parser
 *
 * Minimal parser for Flattened Device Tree format.
 * Extracts only the hardware configuration needed by SLM-OS.
 */

#include "dtb.h"
#include "platform.h"
#include "uart.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Global State
 * ============================================================================ */

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

/* Simple strcmp - returns 0 if equal */
static int fdt_strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

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

/* String length */
static size_t fdt_strlen(const char *s)
{
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
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
    p->pos += fdt_strlen(name) + 1;
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
                size_t len = fdt_strlen(name);
                if (len >= sizeof(current_node)) {
                    len = sizeof(current_node) - 1;
                }
                for (size_t i = 0; i < len; i++) {
                    current_node[i] = name[i];
                }
                current_node[len] = '\0';
                target_depth = 1;
            } else if (p->depth == 1 && fdt_strcmp(current_node, "cpus") == 0) {
                /* Inside /cpus - track cpu@ nodes */
                if (fdt_strstart(name, "cpu@")) {
                    info->cpu_count++;
                }
            }

            p->pos += fdt_strlen(name) + 1;
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
            if (fdt_strcmp(prop_name, "#address-cells") == 0 && len == 4) {
                p->address_cells = fdt_read_cell32(data, 0);
            } else if (fdt_strcmp(prop_name, "#size-cells") == 0 && len == 4) {
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
        if (fdt_strcmp(prop_name, "reg") == 0) {
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
        if (fdt_strcmp(prop_name, "reg") == 0 && len >= 8) {
            /* First reg entry is base address */
            if (p->address_cells == 2) {
                info->uart_base = fdt_read_cell64(data, 0);
            } else {
                info->uart_base = fdt_read_cell32(data, 0);
            }
        } else if (fdt_strcmp(prop_name, "interrupts") == 0 && len >= 12) {
            /* interrupts = <type irq flags> - IRQ is second cell */
            info->uart_irq = fdt_read_cell32(data, 1) + 32; /* SPI offset */
        }
    }

    /* GIC: intc@... or gic@... or interrupt-controller@... */
    if (fdt_strstart(node_name, "intc") ||
        fdt_strstart(node_name, "gic") ||
        fdt_strstart(node_name, "interrupt-controller")) {
        if (fdt_strcmp(prop_name, "reg") == 0 && len >= 16) {
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
        if (fdt_strcmp(prop_name, "interrupts") == 0 && len >= 12) {
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

    return FDT_OK;
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
}

const fdt_info_t *dtb_get_info(void)
{
    return &g_fdt_info;
}
