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
 * Firmware-supplied side-band info (parsed at dtb_parse time)
 * ============================================================================ */

/* /memreserve/ entry from the FDT header reserve map. Firmware uses these
 * to declare physical regions the kernel must NOT allocate from — typically
 * VPU / firmware-shared carveouts. */
typedef struct {
    uint64_t addr;
    uint64_t size;
} dtb_memreserve_t;

#define DTB_MAX_MEMRESERVES 8

/* /chosen entropy + bootloader metadata. Populated by dtb_parse. */
typedef struct {
    /* Firmware-supplied entropy. Each `*_len` is the byte length actually
     * read from the DTB; the `rng_seed` / `kaslr_seed` arrays may hold a
     * partial copy if the source property exceeds the static buffer.
     * Zero `_len` means the property was absent or unreadable. */
    uint8_t  rng_seed[64];
    uint32_t rng_seed_len;
    uint8_t  kaslr_seed[16];
    uint32_t kaslr_seed_len;

    /* Bootloader metadata (Pi firmware /chosen/bootloader). Strings are
     * NUL-terminated; numerics are 0 when absent. */
    char     bootloader_version[80];
    uint32_t bootloader_capabilities;
    uint32_t bootloader_build_timestamp;
    uint32_t bootloader_update_timestamp;

    /* Kernel command line — /chosen/bootargs. Populated from
     * cmdline.txt by the Pi 5 firmware (and from -append on QEMU).
     * NUL-terminated; empty when the property is absent.
     *
     * Sizing note: Pi 5 firmware silently prepends ~225 bytes of
     * standard args (reboot=w coherent_pool=1M 8250.nr_uarts=N
     * pci=pcie_bus_safe cgroup_disable=memory numa_policy=interleave
     * nvme.max_host_mem_size_mb=0 smsc95xx.macaddr=... vc_mem.mem_*=...)
     * AHEAD of cmdline.txt content. A 1024-byte buffer leaves ~800 B
     * for user tokens — comfortably above anything we'd reasonably
     * type. Truncation past the buffer is silent — any caller that
     * runs out of room before finding its key should look at
     * sizeof(bootargs). Verified empirically on pi-5-1 with Pi
     * firmware capabilities=0x7f, 2026-05-10. */
    char     bootargs[1024];
} dtb_chosen_t;

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
 * Get the raw (firmware-supplied) DTB blob pointer.
 *
 * This is the pointer that was passed to `dtb_parse` at boot — the
 * beginning of the flattened tree, ready to hand to `fdt_init` for
 * path-based lookup. Returns NULL if dtb_parse was never called
 * successfully (e.g. x86-64 with no DTB, or bad-magic failure).
 *
 * Backed by a file-static set by `dtb_parse`. The firmware places
 * the DTB at a fixed physical address that stays valid for the
 * lifetime of the kernel, so the returned pointer is stable.
 */
const void *dtb_get_blob(void);

/*
 * Validate DTB header only (quick check).
 *
 * @param dtb  Pointer to DTB in memory
 * @return     FDT_OK if valid header, negative error code otherwise
 */
int dtb_validate(const void *dtb);

/*
 * Get the firmware-supplied /memreserve/ list. Filled in by dtb_parse.
 *
 * Copies up to `max` entries into `out` and returns the number written.
 * Returns 0 if the DTB had no reserve map or dtb_parse hasn't run.
 * Entries are physical [addr, addr+size) regions — pass to PMM init so
 * those pages are never handed out.
 */
int dtb_get_memreserves(dtb_memreserve_t *out, int max);

/*
 * Get the firmware-supplied /chosen entropy + bootloader info. Returns
 * a stable pointer to a static struct populated by dtb_parse. All fields
 * are zero when the corresponding DTB property is absent.
 */
const dtb_chosen_t *dtb_get_chosen(void);

#endif /* DTB_H */
