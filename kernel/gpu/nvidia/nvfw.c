/*
 * nvfw.c — NVIDIA firmware wrapper parser (E3.3).
 *
 * See nvfw.h for the interface. Implementation is a series of
 * bounds-checked reads from the raw blob. We never dereference a
 * pointer we haven't range-checked against @size — firmware blobs
 * come from disk / .incbin and we want to fail closed on any
 * corrupt framing, not crash the harness / kernel.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "nvfw.h"

/* All nvfw fields are little-endian on disk. */
static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Bounds-checked u32 read from @bytes at @off. Returns 0 on success
 * and writes *out. Returns -1 if the 4-byte read would run past end. */
static int read_u32(const uint8_t *bytes, size_t size, size_t off, uint32_t *out)
{
    if (off + 4 > size) return -1;
    *out = rd32le(&bytes[off]);
    return 0;
}

/* nvfw_bin_hdr (24 bytes). */
#define NVFW_BIN_HDR_SIZE           24u
#define NVFW_BIN_HDR_OFF_MAGIC      0u
#define NVFW_BIN_HDR_OFF_VER        4u
#define NVFW_BIN_HDR_OFF_SIZE       8u
#define NVFW_BIN_HDR_OFF_HEADER     12u
#define NVFW_BIN_HDR_OFF_DATA_OFF   16u
#define NVFW_BIN_HDR_OFF_DATA_SIZE  20u

/* nvfw_hs_header_v2 (36 bytes). Field order from nouveau include/nvfw/hs.h. */
#define NVFW_HS_HDR_SIZE             36u
#define NVFW_HS_HDR_OFF_SIG_PROD_OFF  0u
#define NVFW_HS_HDR_OFF_SIG_PROD_SZ   4u
#define NVFW_HS_HDR_OFF_PATCH_LOC     8u
#define NVFW_HS_HDR_OFF_PATCH_SIG    12u
#define NVFW_HS_HDR_OFF_META_OFF     16u
#define NVFW_HS_HDR_OFF_META_SZ      20u
#define NVFW_HS_HDR_OFF_NUM_SIG      24u
#define NVFW_HS_HDR_OFF_HEADER_OFF   28u
#define NVFW_HS_HDR_OFF_HEADER_SZ    32u

/* nvfw_hs_load_header_v2: fixed 20 bytes + num_apps * 16 bytes. */
#define NVFW_LOAD_HDR_FIXED_SIZE     20u
#define NVFW_LOAD_HDR_OFF_OS_CODE_OFF  0u
#define NVFW_LOAD_HDR_OFF_OS_CODE_SZ   4u
#define NVFW_LOAD_HDR_OFF_OS_DATA_OFF  8u
#define NVFW_LOAD_HDR_OFF_OS_DATA_SZ  12u
#define NVFW_LOAD_HDR_OFF_NUM_APPS    16u
#define NVFW_LOAD_HDR_APP_ENTRY_SIZE  16u

/* Meta-data layout: 3 u32 = fuse_ver, engine_id, ucode_id. */
#define NVFW_META_MIN_SIZE           12u

int nvfw_parse(const uint8_t *bytes, size_t size, struct nvfw_image *out)
{
    if (!bytes || !out) return -1;
    out->parsed_ok = false;
    out->has_meta  = false;

    if (size < NVFW_BIN_HDR_SIZE || size > NVFW_MAX_FILE_SIZE) return -1;

    /* ---- nvfw_bin_hdr ---- */
    uint32_t bin_magic, bin_ver, bin_size, header_offset, data_offset, data_size;

    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_MAGIC,     &bin_magic) < 0) return -1;
    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_VER,       &bin_ver) < 0)   return -1;
    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_SIZE,      &bin_size) < 0)  return -1;
    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_HEADER,    &header_offset) < 0) return -1;
    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_DATA_OFF,  &data_offset) < 0) return -1;
    if (read_u32(bytes, size, NVFW_BIN_HDR_OFF_DATA_SIZE, &data_size) < 0) return -1;

    if (bin_magic != NVFW_BIN_MAGIC_STD &&
        bin_magic != NVFW_BIN_MAGIC_NOUVEAU) return -1;

    /* bin_size is informational (nova-core explicitly ignores it).
     * We still require the IMEM/DMEM payload to fit inside @size. */
    (void)bin_size;

    if ((size_t)data_offset + data_size > size) return -1;
    if ((size_t)header_offset + NVFW_HS_HDR_SIZE > size) return -1;

    /* ---- nvfw_hs_header_v2 ---- */
    const uint8_t *hs = &bytes[header_offset];
    out->sig_prod_offset   = rd32le(hs + NVFW_HS_HDR_OFF_SIG_PROD_OFF);
    out->sig_prod_size     = rd32le(hs + NVFW_HS_HDR_OFF_SIG_PROD_SZ);
    uint32_t patch_loc_raw = rd32le(hs + NVFW_HS_HDR_OFF_PATCH_LOC);
    uint32_t patch_sig_raw = rd32le(hs + NVFW_HS_HDR_OFF_PATCH_SIG);
    out->meta_data_offset  = rd32le(hs + NVFW_HS_HDR_OFF_META_OFF);
    out->meta_data_size    = rd32le(hs + NVFW_HS_HDR_OFF_META_SZ);
    out->num_sig           = rd32le(hs + NVFW_HS_HDR_OFF_NUM_SIG);
    out->load_header_offset= rd32le(hs + NVFW_HS_HDR_OFF_HEADER_OFF);

    /* patch_loc / patch_sig resolution:
     *   0x10DE magic   → the hs_header stores a file offset to where
     *                     the actual u32 lives. Deref once.
     *   0x3B1D14F0     → values are inline.  */
    uint32_t patch_loc = patch_loc_raw;
    uint32_t patch_sig = patch_sig_raw;
    if (bin_magic == NVFW_BIN_MAGIC_STD) {
        if (read_u32(bytes, size, patch_loc_raw, &patch_loc) < 0) return -1;
        if (read_u32(bytes, size, patch_sig_raw, &patch_sig) < 0) return -1;
    }
    out->patch_loc = patch_loc;
    out->patch_sig = patch_sig;

    /* Sanity: signature table must fit. */
    if ((size_t)out->sig_prod_offset + out->sig_prod_size > size) return -1;

    /* Meta-data is optional. Present when meta_data_size >= 12. */
    if (out->meta_data_size >= NVFW_META_MIN_SIZE) {
        if ((size_t)out->meta_data_offset + out->meta_data_size > size) return -1;
        const uint8_t *meta = &bytes[out->meta_data_offset];
        out->fuse_ver  = rd32le(meta + 0);
        out->engine_id = rd32le(meta + 4);
        out->ucode_id  = rd32le(meta + 8);
        out->has_meta  = true;
    }

    /* ---- nvfw_hs_load_header_v2 ---- */
    if ((size_t)out->load_header_offset + NVFW_LOAD_HDR_FIXED_SIZE > size) return -1;

    const uint8_t *ld = &bytes[out->load_header_offset];
    out->os_code_offset = rd32le(ld + NVFW_LOAD_HDR_OFF_OS_CODE_OFF);
    out->os_code_size   = rd32le(ld + NVFW_LOAD_HDR_OFF_OS_CODE_SZ);
    out->os_data_offset = rd32le(ld + NVFW_LOAD_HDR_OFF_OS_DATA_OFF);
    out->os_data_size   = rd32le(ld + NVFW_LOAD_HDR_OFF_OS_DATA_SZ);
    out->num_apps       = rd32le(ld + NVFW_LOAD_HDR_OFF_NUM_APPS);

    if (out->num_apps > NVFW_MAX_APPS) return -1;

    size_t apps_end = (size_t)out->load_header_offset
                    + NVFW_LOAD_HDR_FIXED_SIZE
                    + (size_t)out->num_apps * NVFW_LOAD_HDR_APP_ENTRY_SIZE;
    if (apps_end > size) return -1;

    for (uint32_t i = 0; i < out->num_apps; i++) {
        const uint8_t *app = ld + NVFW_LOAD_HDR_FIXED_SIZE
                           + i * NVFW_LOAD_HDR_APP_ENTRY_SIZE;
        out->apps[i].offset      = rd32le(app + 0);
        out->apps[i].size        = rd32le(app + 4);
        out->apps[i].data_offset = rd32le(app + 8);
        out->apps[i].data_size   = rd32le(app + 12);
    }

    out->bytes          = bytes;
    out->size           = size;
    out->bin_magic      = bin_magic;
    out->bin_ver        = bin_ver;
    out->data_offset    = data_offset;
    out->data_size      = data_size;
    out->hs_header_offset = header_offset;
    out->parsed_ok      = true;
    return 0;
}
