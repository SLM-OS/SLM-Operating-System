/*
 * dtb.h - Device Tree Blob (DTB) Parser
 *
 * Minimal parser for Flattened Device Tree (FDT) format.
 * Extracts hardware configuration from DTB passed by bootloader/QEMU.
 *
 * References:
 * - Devicetree Specification v0.4: https://devicetree.org/
 * - Linux kernel: scripts/dtc/libfdt/
 */

#ifndef DTB_H
#define DTB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ============================================================================
 * FDT Header Constants
 * ============================================================================ */

#define FDT_MAGIC           0xd00dfeed  /* Big-endian magic number */
#define FDT_VERSION         17          /* Minimum supported version */

/* Structure block tokens */
#define FDT_BEGIN_NODE      0x01
#define FDT_END_NODE        0x02
#define FDT_PROP            0x03
#define FDT_NOP             0x04
#define FDT_END             0x09

/* ============================================================================
 * FDT Header Structure
 *
 * All fields are big-endian (need byte swap on LE ARM64).
 * ============================================================================ */

struct fdt_header {
    uint32_t magic;             /* FDT_MAGIC */
    uint32_t totalsize;         /* Total size of DTB */
    uint32_t off_dt_struct;     /* Offset to structure block */
    uint32_t off_dt_strings;    /* Offset to strings block */
    uint32_t off_mem_rsvmap;    /* Offset to memory reservation block */
    uint32_t version;           /* DTB version */
    uint32_t last_comp_version; /* Last compatible version */
    uint32_t boot_cpuid_phys;   /* Physical CPU ID of boot processor */
    uint32_t size_dt_strings;   /* Size of strings block */
    uint32_t size_dt_struct;    /* Size of structure block */
};

/* ============================================================================
 * Parsed Platform Info
 *
 * Contains extracted hardware configuration from DTB.
 * Falls back to platform.h defaults if parsing fails.
 * ============================================================================ */

typedef struct {
    /* Memory */
    uint64_t ram_base;          /* Physical RAM base address */
    uint64_t ram_size;          /* RAM size in bytes */

    /* UART */
    uint64_t uart_base;         /* UART base address */
    uint32_t uart_irq;          /* UART interrupt number */

    /* GIC (Generic Interrupt Controller) */
    uint64_t gic_dist_base;     /* GIC distributor base */
    uint64_t gic_cpu_base;      /* GIC CPU interface base */

    /* CPUs */
    uint32_t cpu_count;         /* Number of CPU cores */

    /* Timer */
    uint32_t timer_irq;         /* Timer interrupt (PPI) */

    /* Status */
    bool valid;                 /* True if parsing succeeded */
} fdt_info_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define FDT_OK              0
#define FDT_ERR_BADMAGIC    -1  /* Invalid magic number */
#define FDT_ERR_BADVERSION  -2  /* Unsupported version */
#define FDT_ERR_BADSTRUCT   -3  /* Malformed structure block */
#define FDT_ERR_NOTFOUND    -4  /* Node/property not found */
#define FDT_ERR_BADPTR      -5  /* NULL pointer */

/* ============================================================================
 * Public Functions
 * ============================================================================ */

/*
 * Parse a Device Tree Blob and extract platform info.
 *
 * @param dtb   Pointer to DTB in memory (from bootloader)
 * @param info  Output structure for parsed values
 * @return      FDT_OK on success, negative error code on failure
 *
 * On failure, info->valid is set to false and platform.h defaults
 * should be used instead.
 */
int dtb_parse(const void *dtb, fdt_info_t *info);

/*
 * Print parsed DTB info to UART for debugging.
 *
 * @param info  Parsed platform info structure
 */
void dtb_print_info(const fdt_info_t *info);

/*
 * Get the global parsed FDT info.
 *
 * This returns a pointer to the fdt_info_t populated during boot.
 * If DTB parsing failed, the structure contains platform.h defaults.
 *
 * @return  Pointer to global fdt_info_t (never NULL)
 */
const fdt_info_t *dtb_get_info(void);

/*
 * Validate DTB header only (quick check).
 *
 * @param dtb  Pointer to DTB in memory
 * @return     FDT_OK if valid header, negative error code otherwise
 */
int dtb_validate(const void *dtb);

#endif /* DTB_H */
