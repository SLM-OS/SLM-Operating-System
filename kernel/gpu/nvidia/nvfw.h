/*
 * nvfw.h — NVIDIA HS-signed firmware file parser (E3.3).
 *
 * NVIDIA ships a family of firmware formats under /lib/firmware/
 * nvidia/<chip>/. This parser covers the **heavy-signed** variant:
 *
 *   nvfw_bin_hdr           outermost wrapper, locates everything else
 *   nvfw_hs_header_v2      heavy-signed header: sig tables, patch
 *                          locations, metadata (engine_id/ucode_id)
 *   nvfw_hs_load_header_v2 loader header: IMEM/DMEM layout, app list
 *
 * Concretely:
 *
 *   | File                 | Format                      | Parser     |
 *   |----------------------|-----------------------------|------------|
 *   | booter_load-*.bin    | bin_hdr + hs_header_v2      | THIS       |
 *   | booter_unload-*.bin  | bin_hdr + hs_header_v2      | THIS       |
 *   | bootloader-*.bin     | bin_hdr + nvfw_bl_desc      | TBD (E3.4) |
 *   | gsp-*.bin            | ELF                         | N/A        |
 *
 * The actual IMEM|DMEM ucode payload the Falcon executes lives at
 * `bin_hdr.data_offset` in the file. The headers tell the BROM
 * where to find signatures, what to patch where, and how the ucode
 * wants its memory laid out inside Falcon IMEM.
 *
 * Struct layouts taken verbatim from
 * `drivers/gpu/drm/nouveau/include/nvfw/{fw,hs}.h` — verified
 * against R535 / 535.113.01 GSP blobs shipped with Linux kernel
 * and NVIDIA's proprietary driver.
 *
 * nvfw_parse() rejects non-HS files (bootloader.bin, gsp.bin)
 * cleanly — the caller chooses the right parser per-file.
 */

#ifndef GPU_NVIDIA_NVFW_H
#define GPU_NVIDIA_NVFW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Known bin_magic values. 0x10DE is what R535 GSP firmware ships
 * as (NVIDIA vendor id as the magic). 0x3B1D14F0 is an older
 * nouveau-specific variant — we accept both because the parser is
 * shared across blobs of both shapes. */
#define NVFW_BIN_MAGIC_STD      0x000010DEu   /* R535 */
#define NVFW_BIN_MAGIC_NOUVEAU  0x3B1D14F0u   /* older variant */

/* Defensive cap: any file larger than 256 MB is almost certainly
 * corrupt framing. gsp.bin is ~38 MB, everything else is < 100 KB. */
#define NVFW_MAX_FILE_SIZE      (256u * 1024u * 1024u)

/* Per-app entry inside nvfw_hs_load_header_v2. 16 bytes.
 *   offset       IMEM byte offset where the app is loaded
 *   size         IMEM bytes used
 *   data_offset  DMEM byte offset where the app's data lives
 *   data_size    DMEM bytes used */
struct nvfw_app {
    uint32_t offset;
    uint32_t size;
    uint32_t data_offset;
    uint32_t data_size;
};

/* Max apps we record. Real R535 blobs ship with <= 4 apps — 8 is
 * comfortable headroom without inflating the parsed view. */
#define NVFW_MAX_APPS           8

/*
 * Parsed view of an nvfw-wrapped firmware file. The raw bytes are
 * NOT copied — the caller keeps them alive for as long as it uses
 * any of the offsets here.
 */
struct nvfw_image {
    const uint8_t *bytes;     /* original file buffer */
    size_t         size;

    /* nvfw_bin_hdr fields. */
    uint32_t bin_magic;
    uint32_t bin_ver;
    uint32_t data_offset;     /* file byte offset of IMEM|DMEM payload */
    uint32_t data_size;
    uint32_t hs_header_offset;

    /* nvfw_hs_header_v2 fields.
     *
     * patch_loc / patch_sig: for bin_magic = 0x10DE the hs_header
     * stores an offset to where the ACTUAL value is stored in the
     * blob (double indirection); for 0x3B1D14F0 they're the values
     * directly. We resolve the indirection at parse time so callers
     * get the final u32 value either way. */
    uint32_t sig_prod_offset;
    uint32_t sig_prod_size;
    uint32_t patch_loc;
    uint32_t patch_sig;
    uint32_t num_sig;
    uint32_t meta_data_offset;
    uint32_t meta_data_size;
    uint32_t load_header_offset;

    /* nvfw_hs_load_header_v2 fields. */
    uint32_t os_code_offset;
    uint32_t os_code_size;
    uint32_t os_data_offset;
    uint32_t os_data_size;
    uint32_t num_apps;
    struct nvfw_app apps[NVFW_MAX_APPS];

    /* Meta-data (if present — determined by meta_data_size >= 12):
     *   +0  u32  fuse_ver      — lock-out value in fuses
     *   +4  u32  engine_id     — target engine bitmask (SEC2, GSP...)
     *   +8  u32  ucode_id      — per-engine unique id for signature select
     *
     * These go into the SEC2 BROM at 0x198 / 0x19C when launching
     * the booter; see kernel/gpu/nvidia/falcon.h. */
    uint32_t fuse_ver;
    uint32_t engine_id;
    uint32_t ucode_id;
    bool     has_meta;

    bool parsed_ok;
};

/*
 * Parse an nvfw-wrapped firmware blob.
 *
 * Returns 0 on success. Negative on any structural failure —
 * unknown magic, header offset out of range, data past file end,
 * declared sizes inconsistent with file size.
 *
 * Does NOT validate signatures. The BROM on the target Falcon
 * validates those at ucode-launch time; doing it here would just
 * duplicate work and require RSA-3K infrastructure we don't need
 * (the GPU is the trust anchor for this chain, not us).
 */
int nvfw_parse(const uint8_t *bytes, size_t size, struct nvfw_image *out);

#endif /* GPU_NVIDIA_NVFW_H */
