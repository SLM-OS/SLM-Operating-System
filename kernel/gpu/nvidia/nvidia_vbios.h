/*
 * nvidia_vbios.h — Shared NVIDIA VBIOS parser API (E2 / Phase E).
 *
 * The VBIOS (Video BIOS) lives in SPI flash on the GPU card. PC
 * firmware copies it into RAM during POST, making it reachable via
 * the PCI expansion ROM BAR. Nouveau and NVIDIA's open-gpu-kernel-
 * modules both parse the same structures from the same bytes.
 *
 * Used by GSP-RM bringup (kernel/gpu/nvidia/gsp.c) to locate FWSEC
 * firmware (type 0x85 BIT entry) on Turing/Ampere GPUs. FWSEC is the
 * "Firmware Secure Bootloader" ucode that establishes the Write
 * Protected Region before GSP-RM loads.
 *
 * Integrated GPUs (Jetson) have no VBIOS in the traditional sense —
 * their firmware runtime services come from QSPI via the pre-boot
 * firmware. Platforms with no VBIOS return an error from
 * vbios_load_and_parse().
 *
 * Layout (Pascal through Ampere, verified against a GTX 1070 VBIOS
 * and documented in NVIDIA open-gpu-kernel-modules headers):
 *
 *   Offset  Content
 *   0x00    PCI expansion ROM header: 0x55AA, image-size-in-512B
 *   ...     x86 init code + NVIDIA-specific tables
 *   0x18    PCI Data Structure pointer (PCIR)
 *   ...
 *   <var>   BIT signature (0xFFB8 "BIT\0") + entry list
 */

#ifndef GPU_NVIDIA_VBIOS_H
#define GPU_NVIDIA_VBIOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Maximum VBIOS size we accept. Current NVIDIA VBIOSes are 128–256 KB.
 * 1 MB is a comfortable cap — anything larger is almost certainly
 * a dump-side framing error. */
#define NVIDIA_VBIOS_MAX_SIZE  (1024u * 1024u)

/* BIT entry IDs used by this codebase. The general parser handles
 * any ID — these are the ones we care about today. */
#define VBIOS_BIT_ID_I      0x49    /* 'I': Init scripts */
#define VBIOS_BIT_ID_B      0x42    /* 'B': BIOS info */
#define VBIOS_BIT_ID_P      0x50    /* 'P': Performance / pstates */
#define VBIOS_BIT_ID_FWSEC  0x85    /* Reserved id; pre-release Turing
                                     * docs listed FWSEC here. Empirically
                                     * absent on production Ampere VBIOS
                                     * (validated 2026-04-14 against an
                                     * ASUS RTX 3050 6GB / GA107). Real
                                     * FWSEC discovery on Turing+ walks
                                     * PMU ucode descriptors via the 'I'
                                     * (init scripts) BIT entry — see
                                     * GitHub issue tracking E3 prereq. */

/*
 * Parsed VBIOS state. Callers treat this as opaque; the fields are
 * exposed for test harnesses and diagnostics.
 */
struct nvidia_vbios {
    const uint8_t *image;       /* Pointer to the ROM dump */
    size_t         image_size;

    /* BIT header location within the image. */
    uint32_t       bit_offset;

    /* Parsed BIT header fields. */
    uint8_t        hdr_size;
    uint8_t        entry_size;
    uint8_t        num_entries;

    bool           parsed_ok;
};

/*
 * Parse the BIT table from an in-memory VBIOS image. Fills @out with
 * the offsets needed to resolve entries later via vbios_find_entry().
 *
 * Does NOT copy @image — the caller must keep it alive for the
 * lifetime of @out.
 *
 * Returns 0 on success. Negative on any failure (no 0x55AA signature,
 * no BIT table, out-of-range entries, etc.) with @out->parsed_ok left
 * false.
 */
int nvidia_vbios_parse(const uint8_t *image, size_t image_size,
                       struct nvidia_vbios *out);

/*
 * Look up a BIT entry by id and version. Some IDs appear multiple
 * times with different versions; pass @version = -1 to match any.
 *
 * Writes the in-VBIOS offset and length of the entry's DATA (not the
 * entry header) to *out_data_off / *out_data_len.
 *
 * Returns 0 on success, -1 if no matching entry.
 */
int nvidia_vbios_find_entry(const struct nvidia_vbios *vb,
                            uint8_t id, int version,
                            uint32_t *out_data_off, uint32_t *out_data_len);

/*
 * Extract the FWSEC ucode pointed to by the type-0x85 BIT entry
 * (Turing+ only). On success writes *out_data and *out_size to point
 * into the VBIOS image at the FWSEC start.
 *
 * Returns 0 on success, -ENOENT if no FWSEC entry (pre-Turing GPU),
 * -EINVAL on malformed entry.
 */
int nvidia_vbios_get_fwsec(const struct nvidia_vbios *vb,
                           const uint8_t **out_data, uint32_t *out_size);

/* ---- Platform-side VBIOS access ----
 *
 * Bare-metal x86-64 reads the expansion ROM via the PCI BAR; Linux
 * userspace reads it via /sys/bus/pci/devices/.../rom. Both routes
 * produce a raw image that gets fed to nvidia_vbios_parse().
 *
 * Implemented per-platform — this header just declares the symbol.
 */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size);

#endif /* GPU_NVIDIA_VBIOS_H */
