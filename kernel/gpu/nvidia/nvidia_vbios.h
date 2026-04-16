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
#define VBIOS_BIT_ID_I             0x49    /* 'I': Init scripts */
#define VBIOS_BIT_ID_B             0x42    /* 'B': BIOS info */
#define VBIOS_BIT_ID_P             0x50    /* 'P': Performance / pstates */
#define VBIOS_BIT_ID_FWSEC         0x85    /* Historic — pre-release
                                             * Turing listed FWSEC here.
                                             * Absent on production Ampere. */
#define VBIOS_BIT_ID_FALCON_DATA   0x70    /* 'p': BIT_TOKEN_FALCON_DATA —
                                             * points at the PMU ucode
                                             * descriptor table. On
                                             * Turing+ / Ampere this is
                                             * the real FWSEC discovery
                                             * path. */

/* Application IDs used inside the FALCON_UCODE_TABLE_ENTRY. Matches
 * the openrm / nova-core constants. */
#define VBIOS_FALCON_APPID_FWSEC_PROD    0x85  /* production FWSEC */
#define VBIOS_FALCON_APPID_FWSEC_DBG     0x45  /* debug-signed FWSEC */
#define VBIOS_FALCON_APPID_FIRMWARE_SEC  0x05  /* license ucode */

/* PCI sub-image code-type byte (PCIR/NPDS offset 0x14). */
#define VBIOS_CODE_TYPE_X86          0x00  /* legacy x86 VGA BIOS */
#define VBIOS_CODE_TYPE_EFI          0x03  /* UEFI driver */
#define VBIOS_CODE_TYPE_VBIOS_EXT    0xE0  /* NVIDIA-specific (FwSec) */

/* Maximum sub-images we record. Shipping VBIOSes have at most 4
 * (PciAt + EFI + two FwSec) — 8 is a comfortable headroom. */
#define VBIOS_MAX_SUBIMAGES          8

/*
 * One PCIR/NPDS sub-image in the PCI Option ROM chain. Both signatures
 * use the same 24-byte layout; NPDS is NVIDIA's private alternate of
 * PCIR and appears for code_type=0xE0 (FwSec) images.
 */
struct nvidia_vbios_subimage {
    uint32_t offset;       /* byte offset into full image */
    uint32_t length;       /* NPDE sub_image_len (preferred) else PCIR img_len, bytes */
    uint8_t  code_type;    /* VBIOS_CODE_TYPE_* */
};

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

    /* PCI Option ROM sub-image chain. Walked honoring both PCIR and
     * NPDS signatures and using NPDE extensions for sub_image_len /
     * last_image when present. */
    struct nvidia_vbios_subimage subimages[VBIOS_MAX_SUBIMAGES];
    uint8_t        num_subimages;

    /* Indices into subimages[] for the images FWSEC discovery needs
     * on Turing+/Ampere. -1 if the card doesn't have them (Pascal has
     * no FwSec images; nova-core expects two — we accept 1–2). */
    int8_t         pciat_idx;
    int8_t         first_fwsec_idx;
    int8_t         second_fwsec_idx;

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
 * Extract the FWSEC ucode required for GSP-RM bringup on Turing+
 * and Ampere cards. On success writes *out_data / *out_size pointing
 * into the VBIOS image at the ucode start.
 *
 * Lookup path:
 *   1. Try the historic top-level BIT id 0x85 (pre-release Turing).
 *   2. Follow BIT id 0x70 (BIT_TOKEN_FALCON_DATA) → u32 FalconUcodeTablePtr.
 *   3. Apply the nova-core two-subtraction (minus PciAt length,
 *      optionally minus FwSec#1 length) to get an offset into the
 *      right FwSec sub-image.
 *   4. Read FALCON_UCODE_TABLE_HDR_V1, walk entries, match
 *      ApplicationID == VBIOS_FALCON_APPID_FWSEC_PROD (0x85).
 *   5. Apply the same subtraction to the entry's DescPtr to get
 *      the FALCON_UCODE_DESC_V{2,3} header offset.
 *   6. Payload size = desc_header_size + sig_count * 384 + imem + dmem.
 *
 * Returns 0 on success. Returns -1 for any failure: no BIT 'p',
 * out-of-range pointer (common when our ROM dump is smaller than
 * the VBIOS declares — see docs/archive/investigations/x86-64-gsp-fwsec-investigation.md),
 * no FWSEC_PROD entry, or malformed descriptor.
 */
int nvidia_vbios_get_fwsec(const struct nvidia_vbios *vb,
                           const uint8_t **out_data, uint32_t *out_size);

/*
 * Same ucode, split into the sections needed by E3.4 Falcon bringup.
 * Values come from the FalconUCodeDescV3 header inside the VBIOS.
 *
 *   desc             V3 header (44 bytes) — caller uses this to
 *                    read per-descriptor fields for BROM setup
 *   sigs             signature block (sig_count × 384 bytes)
 *   sigs_size        total signature bytes
 *   imem             IMEM payload (runs on Falcon as code)
 *   imem_size        imem_load_size from descriptor
 *   dmem             DMEM payload (data section)
 *   dmem_size        dmem_load_size from descriptor
 *   interface_off    byte offset into @dmem where the app-interface
 *                    table lives — caller walks it to find DMEMMAPPER
 *   engine_id        EngineIdMask from descriptor (goes to BROM)
 *   ucode_id         UcodeId from descriptor (goes to BROM)
 *   pkc_data_off     PKCDataOffset from descriptor — DMEM byte offset
 *                    where signatures are placed for BROM validation
 *   imem_virt_base   Falcon virtual address where IMEM is mapped
 *                    (BOOTVEC for falcon_start)
 *
 * Returns 0 on success. -1 if FWSEC couldn't be found (Pascal card,
 * truncated ROM) or if the descriptor version isn't V3 (only V3 is
 * supported for this split — V2 has a different signature layout
 * that isn't used on Ampere).
 */
struct nvidia_vbios_fwsec_parts {
    const uint8_t *desc;
    const uint8_t *sigs;
    uint32_t       sigs_size;
    const uint8_t *imem;
    uint32_t       imem_size;
    const uint8_t *dmem;
    uint32_t       dmem_size;
    uint32_t       interface_off;
    uint32_t       engine_id;
    uint32_t       ucode_id;
    uint32_t       pkc_data_off;
    uint32_t       imem_virt_base;
};

int nvidia_vbios_get_fwsec_parts(const struct nvidia_vbios *vb,
                                 struct nvidia_vbios_fwsec_parts *out);

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
