/*
 * nvidia_vbios.c — Shared NVIDIA VBIOS / BIT table parser (E2).
 *
 * Runs on every platform that has GSP-RM bringup wired in: x86-64
 * bare-metal (via PCI expansion ROM), and the Linux userspace
 * harness (via /sys/bus/pci/.../rom). Jetson has no VBIOS in this
 * format — its FWSEC-equivalent setup comes from QSPI via the
 * pre-boot firmware and does not route through this parser.
 *
 * Tested against a real GP104 (GTX 1070) VBIOS dump via the
 * standalone test vector check in kernel/tests/test_nvidia_vbios.c.
 * FWSEC extraction (type 0x85) is Ampere-specific and will be
 * validated on test-pc via the gsp-harness once Linux is up.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "nvidia_vbios.h"

/* VBIOS layout constants — verified against a real VBIOS dump. */
#define PCI_ROM_SIG_0       0x55
#define PCI_ROM_SIG_1       0xAA
#define BIT_SIG_0           0xFF
#define BIT_SIG_1           0xB8
#define BIT_ID_STR          "BIT"   /* followed by NUL in the dump */

/* BIT header layout (12 bytes total for the versions we support):
 *   +0  u16  0xFFB8         signature
 *   +2  4B   "BIT\0"        identifier
 *   +6  u8   reserved / version
 *   +7  u8   reserved / version
 *   +8  u8   hdr_size
 *   +9  u8   entry_size
 *   +10 u8   num_entries
 *   +11 u8   checksum
 *
 * Each entry:
 *   +0  u8   id
 *   +1  u8   version
 *   +2  u16  data_length
 *   +4  u16  data_offset (within the full VBIOS image)
 */
#define BIT_HDR_SIZE_MIN    12
#define BIT_ENTRY_SIZE      6
#define BIT_MAX_ENTRIES     64      /* defensive cap */

/* PCI Option ROM sub-image constants. openrm 595+ and nova-core
 * both accept PCIR and NPDS signatures; earlier openrm (535.113.01)
 * only accepts PCIR and would miss NPDS-format FwSec images. We
 * accept both. */
#define PCIR_SIG_0          'P'
#define PCIR_SIG_1          'C'
#define PCIR_SIG_2          'I'
#define PCIR_SIG_3          'R'
#define NPDS_SIG_0          'N'
#define NPDS_SIG_1          'P'
#define NPDS_SIG_2          'D'
#define NPDS_SIG_3          'S'
#define NPDE_SIG_0          'N'
#define NPDE_SIG_1          'P'
#define NPDE_SIG_2          'D'
#define NPDE_SIG_3          'E'

/* PCIR/NPDS common offsets (both layouts are byte-identical). */
#define PCIR_OFF_PCIR_LEN       0x0A    /* u16 — length of this struct */
#define PCIR_OFF_IMG_LEN        0x10    /* u16 — image length, 512-byte blocks */
#define PCIR_OFF_CODE_TYPE      0x14    /* u8  — 0x00 x86 / 0x03 EFI / 0xE0 FwSec */
#define PCIR_OFF_LAST_IMAGE     0x15    /* u8  — bit 7 = last */

/* NPDE extension offsets. */
#define NPDE_OFF_LEN            0x06    /* u16 — length of this NPDE */
#define NPDE_OFF_SUB_IMG_LEN    0x08    /* u16 — sub-image length, 512-byte blocks */
#define NPDE_OFF_LAST_IMAGE     0x0A    /* u8  — bit 7 = last (preferred over PCIR) */

/* Falcon ucode descriptor table (pointed to by the BIT 'p' entry's
 * u32 FalconUcodeTablePtr, after the nova-core arithmetic).
 *
 * FALCON_UCODE_TABLE_HDR_V1 (6 bytes):
 *   +0 u8 version    (must be 1)
 *   +1 u8 hdr_size   (must be 6)
 *   +2 u8 entry_size (must be 6)
 *   +3 u8 entry_count
 *   +4 u8 desc_version  (2 on Turing TU10x, 3 on Ampere GA10x)
 *   +5 u8 desc_size     (60 for V2, 44 for V3)
 *
 * Each FALCON_UCODE_TABLE_ENTRY_V1 (6 bytes):
 *   +0 u8  application_id (0x85 = FWSEC_PROD, 0x45 = FWSEC_DBG)
 *   +1 u8  target_id
 *   +2 u32 desc_ptr       (same ptr-space as FalconUcodeTablePtr)
 */
#define FALCON_TABLE_HDR_SIZE     6
#define FALCON_TABLE_ENTRY_SIZE   6

/* FALCON_UCODE_DESC header (both V2 and V3 start with this u32):
 *   bits  0:0  version-available flag
 *   bits 15:8  descriptor version (2 or 3)
 *   bits 31:16 descriptor size in bytes
 */
#define FALCON_DESC_VER_SHIFT     8
#define FALCON_DESC_VER_MASK      0xFFu
#define FALCON_DESC_SIZE_SHIFT    16
#define FALCON_DESC_SIZE_MASK     0xFFFFu

/* V3 descriptor (44 bytes, Ampere / GA10x) — fields we need to
 * compute payload size and populate Falcon BROM registers.
 *
 * Field map (all u32 unless noted):
 *   0x00  Hdr            (bits 15:8 version, 31:16 size)
 *   0x04  StoredSize
 *   0x08  PKCDataOffset  (DMEM byte offset of signature block)
 *   0x0C  InterfaceOffset (DMEM byte offset of app-interface table)
 *   0x10  IMEMPhysBase
 *   0x14  IMEMLoadSize   ← size of IMEM section in bytes
 *   0x18  IMEMVirtBase   ← BOOTVEC value
 *   0x1C  DMEMPhysBase
 *   0x20  DMEMLoadSize   ← size of DMEM section in bytes
 *   0x24  u16 EngineIdMask   (BROM ENGIDMASK)
 *   0x26  u8  UcodeId        (BROM UCODE_ID)
 *   0x27  u8  SignatureCount
 *   ...
 */
#define FALCON_DESC_V3_PKC_DATA_OFF     8
#define FALCON_DESC_V3_INTERFACE_OFF    12
#define FALCON_DESC_V3_IMEM_LOAD_SIZE   20
#define FALCON_DESC_V3_IMEM_VIRT_BASE   24
#define FALCON_DESC_V3_DMEM_LOAD_SIZE   32
#define FALCON_DESC_V3_ENGINE_ID_MASK   36     /* u16 */
#define FALCON_DESC_V3_UCODE_ID         38     /* u8 */
#define FALCON_DESC_V3_SIG_COUNT        39
#define FALCON_DESC_V3_SIZE             44

/* V2 descriptor (60 bytes, Turing TU10x) — analogous offsets. */
#define FALCON_DESC_V2_IMEM_LOAD_SIZE   24
#define FALCON_DESC_V2_DMEM_LOAD_SIZE   48
/* V2 has no signature count field; signatures (if any) are inlined. */

/* BCRT30 RSA-3K signature block length (when present). */
#define FALCON_SIGNATURE_SIZE           384

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/*
 * Walk the PCIR/NPDS sub-image chain starting at offset 0 of @image.
 * Fills @out->subimages[] and populates the pciat_idx / first_fwsec_idx /
 * second_fwsec_idx helpers. Returns 0 on success (valid chain, at least
 * one sub-image), -1 on malformed structure.
 *
 * Follows openrm 595's s_locateExpansionRoms (also matches nova-core
 * 2026-04-14 mainline):
 *   - Either PCIR or NPDS at the pcir_ptr offset is valid.
 *   - If NPDE is present immediately after PCIR/NPDS (16-byte aligned),
 *     use its sub_image_len and last_image bit instead of PCIR's.
 *   - Stop at the first image marked "last" in whichever source wins.
 */
static int walk_subimages(const uint8_t *image, size_t image_size,
                          struct nvidia_vbios *out)
{
    uint32_t off = 0;
    out->num_subimages = 0;
    out->pciat_idx        = -1;
    out->first_fwsec_idx  = -1;
    out->second_fwsec_idx = -1;

    for (uint8_t i = 0; i < VBIOS_MAX_SUBIMAGES; i++) {
        /* End-of-buffer or insufficient room for an image header —
         * only a hard error on image 0; otherwise treat what we've
         * walked so far as the complete chain. This is the common
         * case when the Linux kernel's `/sys/.../rom` interface
         * truncates at PCIR LAST and the rest of the chain isn't
         * served. */
        if (off + 0x1A > image_size) {
            if (i == 0) return -1;
            break;
        }

        /* The first sub-image must start with 0x55AA. Subsequent
         * sub-images may use NPDS-only layout and start with an
         * arbitrary byte (e.g. the "VN" block seen on GA107). So we
         * require 0x55AA only for image 0. */
        if (i == 0 && (image[off] != PCI_ROM_SIG_0 ||
                       image[off+1] != PCI_ROM_SIG_1))
            return -1;

        uint16_t pcir_ptr = rd16(&image[off + 0x18]);
        size_t pcir = (size_t)off + pcir_ptr;
        if (pcir + 0x18 > image_size) {
            if (i == 0) return -1;
            break;
        }

        int is_pcir = (image[pcir]   == PCIR_SIG_0 &&
                       image[pcir+1] == PCIR_SIG_1 &&
                       image[pcir+2] == PCIR_SIG_2 &&
                       image[pcir+3] == PCIR_SIG_3);
        int is_npds = (image[pcir]   == NPDS_SIG_0 &&
                       image[pcir+1] == NPDS_SIG_1 &&
                       image[pcir+2] == NPDS_SIG_2 &&
                       image[pcir+3] == NPDS_SIG_3);
        if (!is_pcir && !is_npds) {
            /* End of chain — anything after the last valid image
             * is padding or unrecognized data. Not an error. */
            break;
        }

        uint16_t pcir_len  = rd16(&image[pcir + PCIR_OFF_PCIR_LEN]);
        uint16_t img_blks  = rd16(&image[pcir + PCIR_OFF_IMG_LEN]);
        uint8_t  code_type = image[pcir + PCIR_OFF_CODE_TYPE];
        uint8_t  indicator = image[pcir + PCIR_OFF_LAST_IMAGE];

        uint32_t img_len   = (uint32_t)img_blks * 512;
        uint32_t sub_len   = img_len;
        int      last      = (indicator & 0x80) != 0;

        /* NPDE sits immediately after PCIR/NPDS, padded to 16-byte
         * alignment. Overrides image length and last-image flag. */
        size_t npde = (pcir + pcir_len + 0xF) & ~(size_t)0xF;
        if (npde + 4 < image_size &&
            image[npde]   == NPDE_SIG_0 &&
            image[npde+1] == NPDE_SIG_1 &&
            image[npde+2] == NPDE_SIG_2 &&
            image[npde+3] == NPDE_SIG_3) {
            uint16_t npde_len = rd16(&image[npde + NPDE_OFF_LEN]);
            uint16_t sub_blks = rd16(&image[npde + NPDE_OFF_SUB_IMG_LEN]);
            sub_len = (uint32_t)sub_blks * 512;
            if (npde_len >= 0x0B) {
                uint8_t last_byte = image[npde + NPDE_OFF_LAST_IMAGE];
                last = (last_byte & 0x80) != 0;
            }
        }

        /* Record the image. Treat the claimed length as declarative —
         * truncation by the reader (our /dev/mem dump is capped at the
         * ROM BAR size, which may be smaller than what NPDS declares)
         * is a valid state that downstream code must handle. */
        out->subimages[i].offset    = off;
        out->subimages[i].length    = sub_len;
        out->subimages[i].code_type = code_type;
        out->num_subimages = (uint8_t)(i + 1);

        if (code_type == VBIOS_CODE_TYPE_X86 && out->pciat_idx < 0)
            out->pciat_idx = (int8_t)i;
        else if (code_type == VBIOS_CODE_TYPE_VBIOS_EXT) {
            if (out->first_fwsec_idx < 0)
                out->first_fwsec_idx = (int8_t)i;
            else if (out->second_fwsec_idx < 0)
                out->second_fwsec_idx = (int8_t)i;
        }

        if (last || sub_len == 0) break;
        off += sub_len;
    }

    /* A VBIOS always has at least a PciAt image. FwSec images are
     * Turing+ only — their absence is not an error here; it just means
     * get_fwsec will return -1. */
    if (out->pciat_idx < 0) return -1;
    return 0;
}

/*
 * Scan the VBIOS image for the BIT signature. BIT always appears
 * in the first 8 KB of a well-formed VBIOS, but different board
 * vendors place it at slightly different offsets, so we scan.
 */
static int find_bit_signature(const uint8_t *image, size_t image_size,
                              uint32_t *out_off)
{
    if (image_size < 64)
        return -1;
    /* Cap the search at 64 KB — BIT has never been observed past
     * that, and scanning the whole ROM wastes cycles on large
     * dumps. */
    size_t limit = image_size > 64 * 1024 ? 64 * 1024 : image_size;
    for (size_t i = 0; i + 8 < limit; i++) {
        if (image[i]   == BIT_SIG_0 &&
            image[i+1] == BIT_SIG_1 &&
            image[i+2] == 'B' &&
            image[i+3] == 'I' &&
            image[i+4] == 'T' &&
            image[i+5] == 0) {
            *out_off = (uint32_t)i;
            return 0;
        }
    }
    return -1;
}

int nvidia_vbios_parse(const uint8_t *image, size_t image_size,
                       struct nvidia_vbios *out)
{
    if (!image || !out) return -1;
    out->parsed_ok = false;

    if (image_size < 64 || image_size > NVIDIA_VBIOS_MAX_SIZE)
        return -1;

    /* PCI expansion ROM signature. Required before we trust any
     * downstream offsets. */
    if (image[0] != PCI_ROM_SIG_0 || image[1] != PCI_ROM_SIG_1)
        return -1;

    uint32_t bit_off = 0;
    if (find_bit_signature(image, image_size, &bit_off) < 0)
        return -1;

    /* Defensive arithmetic — find_bit_signature caps at 64 KB and
     * image_size at NVIDIA_VBIOS_MAX_SIZE (1 MB), so with
     * BIT_HDR_SIZE_MIN (12) this can't overflow size_t on any
     * platform where size_t ≥ 32 bits. But we spell out each
     * addend in `size_t` space anyway so future tightening
     * (raising the 1 MB cap, etc.) can't regress this check. */
    if (bit_off > image_size ||
        (size_t)bit_off + BIT_HDR_SIZE_MIN > image_size)
        return -1;

    uint8_t hdr_size    = image[bit_off + 8];
    uint8_t entry_size  = image[bit_off + 9];
    uint8_t num_entries = image[bit_off + 10];

    if (hdr_size    < BIT_HDR_SIZE_MIN) return -1;
    if (entry_size  != BIT_ENTRY_SIZE)  return -1;
    if (num_entries > BIT_MAX_ENTRIES)  return -1;

    /* Overflow-safe: hdr_size is a byte (≤255), num_entries ≤ 64,
     * entry_size = 6. Max addend is 64*6 = 384. All comparisons in
     * size_t space to survive future tightening. */
    size_t hdr_bytes    = (size_t)bit_off + hdr_size;
    size_t entries_end  = hdr_bytes + (size_t)num_entries * entry_size;
    if (hdr_bytes < (size_t)bit_off)       return -1;  /* paranoia */
    if (entries_end < hdr_bytes)           return -1;  /* paranoia */
    if (entries_end > image_size)          return -1;

    out->image       = image;
    out->image_size  = image_size;
    out->bit_offset  = bit_off;
    out->hdr_size    = hdr_size;
    out->entry_size  = entry_size;
    out->num_entries = num_entries;

    /* Walk the PCIR/NPDS sub-image chain and record offsets. Failure
     * here means the Option ROM chain is malformed — reject outright
     * so callers don't try to extract FWSEC from a corrupt image.
     * (A Pascal-shape VBIOS with no FwSec images walks successfully;
     * first_fwsec_idx just stays -1.) */
    if (walk_subimages(image, image_size, out) < 0) {
        out->num_subimages = 0;
        return -1;
    }

    out->parsed_ok   = true;
    return 0;
}

int nvidia_vbios_find_entry(const struct nvidia_vbios *vb,
                            uint8_t id, int version,
                            uint32_t *out_data_off, uint32_t *out_data_len)
{
    if (!vb || !vb->parsed_ok) return -1;

    const uint8_t *image = vb->image;
    uint32_t entry_base = vb->bit_offset + vb->hdr_size;
    /* Region occupied by the BIT header + entry list. Entry data is
     * rejected if it points into this range — a malformed entry that
     * aliases back onto the BIT table bytes could let a caller read
     * structural metadata as if it were payload. */
    uint32_t bit_region_start = vb->bit_offset;
    uint32_t bit_region_end   = entry_base + (uint32_t)vb->num_entries * vb->entry_size;

    for (uint8_t i = 0; i < vb->num_entries; i++) {
        uint32_t e = entry_base + (uint32_t)i * vb->entry_size;
        uint8_t  eid = image[e];
        uint8_t  ever = image[e + 1];
        uint16_t elen = rd16(&image[e + 2]);
        uint16_t eoff = rd16(&image[e + 4]);

        if (eid != id) continue;
        if (version >= 0 && (int)ever != version) continue;

        /* Size-t arithmetic so the bound check survives a future
         * enlargement of entry length to u32. Also guards against
         * the pathological case where eoff + elen wraps a 16-bit
         * intermediate — we promote both to size_t explicitly. */
        size_t data_end = (size_t)eoff + (size_t)elen;
        if (data_end < (size_t)eoff) return -1;          /* overflow */
        if (data_end > vb->image_size) return -1;        /* past end  */

        /* Reject entries whose data region overlaps the BIT header
         * or entry list. A real VBIOS never does this (pointers go
         * forward, past the table), but a crafted image could. */
        if (elen > 0 && eoff < bit_region_end &&
            (eoff + elen) > bit_region_start) {
            return -1;
        }

        if (out_data_off) *out_data_off = eoff;
        if (out_data_len) *out_data_len = elen;
        return 0;
    }
    return -1;
}

/*
 * Translate a "concatenated PciAt|FwSec1|FwSec2 buffer" offset into a
 * full-image absolute offset. This is the nova-core algorithm
 * (`setup_falcon_data` in drivers/gpu/nova-core/vbios.rs): the VBIOS
 * pointers are expressed as if PciAt and all FwSec images were glued
 * together with the EFI image stripped out. We walk back the stripping
 * and land on the real byte in the dump.
 *
 * Writes the FwSec sub-image index the offset resolves to, so callers
 * can validate the pointer stays inside that image even when the
 * image's declared length exceeds what our ROM dump actually holds
 * (a common gotcha on GA107 where the ROM BAR is 512 KB but NPDS
 * declares more).
 *
 * Returns 0 on success with *out_abs set. Returns -1 if the offset
 * resolves to a position past any FwSec image we recorded (including
 * "past the end of our truncated dump").
 */
static int falcon_ptr_resolve(const struct nvidia_vbios *vb,
                              uint32_t ptr,
                              uint32_t *out_abs, int *out_fwsec_idx)
{
    if (vb->pciat_idx < 0 || vb->first_fwsec_idx < 0) return -1;

    uint32_t pciat_len = vb->subimages[vb->pciat_idx].length;
    if (ptr < pciat_len) {
        /* Pointer targets inside PciAt — rare / unexpected on
         * Turing+; openrm and nova-core both interpret FalconData
         * pointers as living in the FwSec chain. Reject. */
        return -1;
    }

    uint32_t off = ptr - pciat_len;

    const struct nvidia_vbios_subimage *fw1 = &vb->subimages[vb->first_fwsec_idx];
    if (off < fw1->length) {
        /* Inside FwSec1. */
        uint32_t abs = fw1->offset + off;
        if (abs >= vb->image_size) return -1;
        if (out_abs)        *out_abs = abs;
        if (out_fwsec_idx)  *out_fwsec_idx = vb->first_fwsec_idx;
        return 0;
    }

    if (vb->second_fwsec_idx < 0) return -1;
    off -= fw1->length;

    const struct nvidia_vbios_subimage *fw2 = &vb->subimages[vb->second_fwsec_idx];
    if (off >= fw2->length) return -1;

    uint32_t abs = fw2->offset + off;
    if (abs >= vb->image_size) return -1;

    if (out_abs)        *out_abs = abs;
    if (out_fwsec_idx)  *out_fwsec_idx = vb->second_fwsec_idx;
    return 0;
}

/*
 * FWSEC ucode discovery on Turing+ / Ampere via the BIT 'p' entry
 * (BIT_TOKEN_FALCON_DATA, id 0x70). See the header comment on
 * nvidia_vbios_get_fwsec for the full algorithm.
 *
 * Pre-Turing cards (Pascal and earlier) don't have FwSec images — the
 * sub-image walker leaves first_fwsec_idx = -1 and the function
 * returns -1 quietly.
 *
 * This function also handles the "historic id 0x85" case as a
 * forward-compat fallback — any future card that does ship FWSEC
 * under that id works without a rebuild.
 */
int nvidia_vbios_get_fwsec(const struct nvidia_vbios *vb,
                           const uint8_t **out_data, uint32_t *out_size)
{
    if (!vb || !vb->parsed_ok) return -1;
    if (out_data) *out_data = NULL;
    if (out_size) *out_size = 0;

    /* Historic path — harmless lookup, misses on every production
     * Turing/Ampere card we've seen. */
    {
        uint32_t data_off = 0, data_len = 0;
        if (nvidia_vbios_find_entry(vb, VBIOS_BIT_ID_FWSEC, -1,
                                    &data_off, &data_len) == 0
            && data_len > 0) {
            if (out_data) *out_data = vb->image + data_off;
            if (out_size) *out_size = data_len;
            return 0;
        }
    }

    /* Modern path — BIT 'p' → FalconUcodeTablePtr. */
    uint32_t ftp_entry_off = 0, ftp_entry_len = 0;
    if (nvidia_vbios_find_entry(vb, VBIOS_BIT_ID_FALCON_DATA, -1,
                                &ftp_entry_off, &ftp_entry_len) < 0)
        return -1;
    if (ftp_entry_len < 4) return -1;
    if ((size_t)ftp_entry_off + 4 > vb->image_size) return -1;

    uint32_t falcon_ucode_table_ptr = rd32(&vb->image[ftp_entry_off]);

    /* Resolve through the PciAt|FwSec1|FwSec2 concatenation. */
    uint32_t tbl_abs = 0;
    int tbl_fwsec_idx = -1;
    if (falcon_ptr_resolve(vb, falcon_ucode_table_ptr, &tbl_abs, &tbl_fwsec_idx) < 0)
        return -1;
    if ((size_t)tbl_abs + FALCON_TABLE_HDR_SIZE > vb->image_size) return -1;

    const uint8_t *tbl = &vb->image[tbl_abs];
    uint8_t t_version   = tbl[0];
    uint8_t t_hdr_size  = tbl[1];
    uint8_t t_entry_sz  = tbl[2];
    uint8_t t_count     = tbl[3];
    uint8_t t_desc_ver  = tbl[4];
    uint8_t t_desc_sz   = tbl[5];

    if (t_version != 1 || t_hdr_size != FALCON_TABLE_HDR_SIZE ||
        t_entry_sz != FALCON_TABLE_ENTRY_SIZE) return -1;
    if (t_count == 0 || t_count > 32) return -1;       /* sanity cap */
    /* DescVersion / DescSize in the TABLE header are informational —
     * they hint at the most common descriptor format used by entries
     * but the authoritative version for each descriptor lives in the
     * per-entry header (bits 15:8). Validated 2026-04-14 against a
     * real RTX 3050: TABLE header reports desc_ver=1 desc_sz=0x30,
     * while FWSEC_PROD entry's actual descriptor is V3. Trust the
     * per-descriptor header. */
    (void)t_desc_ver;
    (void)t_desc_sz;

    /* Entries immediately follow the header. */
    uint32_t entries_start = tbl_abs + FALCON_TABLE_HDR_SIZE;
    uint32_t entries_end   = entries_start + (uint32_t)t_count * FALCON_TABLE_ENTRY_SIZE;
    if (entries_end > vb->image_size) return -1;

    /* Find FWSEC_PROD (0x85). Debug-signed FWSEC_DBG (0x45) is not
     * used on production cards — if neither is found, return -1. */
    uint32_t fwsec_desc_ptr = 0;
    bool found = false;
    for (uint8_t i = 0; i < t_count; i++) {
        const uint8_t *e = &vb->image[entries_start + i * FALCON_TABLE_ENTRY_SIZE];
        uint8_t app_id = e[0];
        if (app_id != VBIOS_FALCON_APPID_FWSEC_PROD) continue;
        fwsec_desc_ptr = rd32(&e[2]);
        found = true;
        break;
    }
    if (!found) return -1;

    /* DescPtr is in the same PciAt|FwSec1|FwSec2 space — same
     * two-subtraction resolution. */
    uint32_t desc_abs = 0;
    int desc_fwsec_idx = -1;
    if (falcon_ptr_resolve(vb, fwsec_desc_ptr, &desc_abs, &desc_fwsec_idx) < 0)
        return -1;
    if ((size_t)desc_abs + 4 > vb->image_size) return -1;

    /* Descriptor header tells us which version + size we're reading.
     *   bits  0:0  version-available flag (must be 1 — "unavailable"
     *              marker is an openrm skip-this-entry signal)
     *   bits 15:8  descriptor version (2 = Turing V2, 3 = Ampere V3)
     *   bits 31:16 total descriptor + signature block size in bytes
     *
     * dsize can legitimately be several KB (V3 header is 44 bytes +
     * 4 × 384-byte RSA-3K signatures = 1,580 bytes on real GA107).
     * Do NOT cap it at a small value — openrm only checks
     * `dsize >= version-specific-minimum`. 64 KB upper bound is
     * defense-in-depth against corrupt input. */
    uint32_t dhdr = rd32(&vb->image[desc_abs]);
    if ((dhdr & 1u) == 0) return -1;    /* version-available flag */
    uint32_t dver = (dhdr >> FALCON_DESC_VER_SHIFT) & FALCON_DESC_VER_MASK;
    uint32_t dsize = (dhdr >> FALCON_DESC_SIZE_SHIFT) & FALCON_DESC_SIZE_MASK;

    if (dver != 2 && dver != 3) return -1;
    if (dsize < 16 || dsize > 64u * 1024u) return -1;
    if ((size_t)desc_abs + dsize > vb->image_size) return -1;

    /* Compute total FWSEC payload size = descriptor (includes
     * signatures on V3) + IMEM + DMEM. openrm does the same: see
     * `signaturesTotalSize = descSize - FALCON_UCODE_DESC_V3_SIZE_44`
     * and `pUcode->size = imemLoadSize + dmemLoadSize`.
     *
     * The ucode bytes themselves live at a separate location in the
     * VBIOS (pointed at by a separate field in the descriptor) —
     * openrm reads desc at desc_ptr, then ucode at imageOffset
     * computed from fields inside the descriptor. For SLM-OS we
     * return the descriptor payload — a future E3 step will compute
     * the separate ucode imageOffset when we actually run FWSEC
     * on SEC2 Falcon. */
    uint32_t imem_load = 0, dmem_load = 0;
    if (dver == 3) {
        if (dsize < 44) return -1;
        imem_load = rd32(&vb->image[desc_abs + FALCON_DESC_V3_IMEM_LOAD_SIZE]);
        dmem_load = rd32(&vb->image[desc_abs + FALCON_DESC_V3_DMEM_LOAD_SIZE]);
    } else {
        if (dsize < 60) return -1;
        imem_load = rd32(&vb->image[desc_abs + FALCON_DESC_V2_IMEM_LOAD_SIZE]);
        dmem_load = rd32(&vb->image[desc_abs + FALCON_DESC_V2_DMEM_LOAD_SIZE]);
    }

    /* Guard against unreasonable sizes. 4 MB per segment is well
     * over anything NVIDIA ships. */
    if (imem_load > 4u * 1024u * 1024u ||
        dmem_load > 4u * 1024u * 1024u) return -1;

    uint32_t payload_size = dsize + imem_load + dmem_load;
    if (payload_size < dsize) return -1;   /* overflow paranoia */

    if ((size_t)desc_abs + payload_size > vb->image_size) return -1;

    if (out_data) *out_data = &vb->image[desc_abs];
    if (out_size) *out_size = payload_size;
    return 0;
}

int nvidia_vbios_get_fwsec_parts(const struct nvidia_vbios *vb,
                                 struct nvidia_vbios_fwsec_parts *out)
{
    if (!vb || !vb->parsed_ok || !out) return -1;

    /* Use the existing lookup to get the descriptor pointer. The
     * returned payload pointer IS the descriptor — we then split it
     * into desc / sigs / imem / dmem using the V3 header fields. */
    const uint8_t *payload = NULL;
    uint32_t payload_size = 0;
    if (nvidia_vbios_get_fwsec(vb, &payload, &payload_size) < 0) return -1;
    if (!payload || payload_size < FALCON_DESC_V3_SIZE) return -1;

    /* Parse the V3 header. */
    uint32_t dhdr  = rd32(payload);
    if ((dhdr & 1u) == 0) return -1;
    uint32_t dver  = (dhdr >> FALCON_DESC_VER_SHIFT)  & FALCON_DESC_VER_MASK;
    uint32_t dsize = (dhdr >> FALCON_DESC_SIZE_SHIFT) & FALCON_DESC_SIZE_MASK;

    /* V3 only — the split for V2 has a different signature layout
     * that we don't need on Ampere. V2 support would be a separate
     * path if we ever target Turing TU10x. */
    if (dver != 3 || dsize < FALCON_DESC_V3_SIZE) return -1;

    uint32_t pkc_data_off   = rd32(payload + FALCON_DESC_V3_PKC_DATA_OFF);
    uint32_t interface_off  = rd32(payload + FALCON_DESC_V3_INTERFACE_OFF);
    uint32_t imem_load_size = rd32(payload + FALCON_DESC_V3_IMEM_LOAD_SIZE);
    uint32_t imem_virt_base = rd32(payload + FALCON_DESC_V3_IMEM_VIRT_BASE);
    uint32_t dmem_load_size = rd32(payload + FALCON_DESC_V3_DMEM_LOAD_SIZE);
    uint16_t engine_id_mask = (uint16_t)rd16(payload + FALCON_DESC_V3_ENGINE_ID_MASK);
    uint8_t  ucode_id       = payload[FALCON_DESC_V3_UCODE_ID];
    uint8_t  sig_count      = payload[FALCON_DESC_V3_SIG_COUNT];

    /* Layout in the payload buffer:
     *   [0 .. dsize)                               descriptor header + extras
     *   [dsize .. dsize + sig_count*384)           signature block
     *   [imem_start .. imem_start + imem_load_size)  IMEM
     *   [dmem_start .. dmem_start + dmem_load_size)  DMEM
     *
     * openrm's naming: "descSize" in the header = dsize = 44 + sigs.
     * So IMEM starts exactly at dsize, DMEM at dsize + imem_load_size. */
    uint32_t sigs_size = (uint32_t)sig_count * 384u;
    if (dsize < FALCON_DESC_V3_SIZE + sigs_size) return -1;
    uint32_t imem_off = dsize;
    uint32_t dmem_off = imem_off + imem_load_size;
    uint32_t total    = dmem_off + dmem_load_size;
    if (total > payload_size) return -1;

    out->desc            = payload;
    out->sigs            = payload + FALCON_DESC_V3_SIZE;
    out->sigs_size       = sigs_size;
    out->imem            = payload + imem_off;
    out->imem_size       = imem_load_size;
    out->dmem            = payload + dmem_off;
    out->dmem_size       = dmem_load_size;
    out->interface_off   = interface_off;
    out->engine_id       = engine_id_mask;
    out->ucode_id        = ucode_id;
    out->pkc_data_off    = pkc_data_off;
    out->imem_virt_base  = imem_virt_base;
    return 0;
}
