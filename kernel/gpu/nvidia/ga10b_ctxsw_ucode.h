/*
 * ga10b_ctxsw_ucode.h - FECS + GPCCS ctxsw ucode surface (#1088).
 *
 * Builds the single "ctxsw ucode surface" nvgpu assembles in
 * nvgpu_gr_falcon_init_ctxsw_ucode() (L4T R36.4.x,
 * common/gr/gr_falcon.c:300-391). The surface holds, in order:
 *
 *   FECS  boot | FECS  code | FECS  data |
 *   GPCCS boot | GPCCS code | GPCCS data
 *
 * each segment starting on a 256-byte boundary
 * (gr_falcon.c:248-257, NVGPU_ALIGN(offset + size, 256)). Padding
 * between segments is zero (nvgpu allocates the surface with
 * GFP_ZERO, os/linux/linux-dma.c:248, and writes only segment bytes).
 *
 *   boot: fecs/gpccs_encrypt_prod.bin = struct
 *         nvgpu_ctxsw_bootloader_desc (16 B) + boot image
 *         (gr_falcon.c:319-337); segment size = ALIGN(desc.size, 4)
 *         (gr_falcon.c:264).
 *   code: netlist FECS/GPCCS_UCODE_INST, size = count * 4
 *   data: netlist FECS/GPCCS_UCODE_DATA, size = count * 4
 *         (gr_falcon.c:340-351; count rounds up, netlist.c:85-87).
 *
 * The surface is the ucode source for the ACR LS blob
 * (ga10b_acr_blob.h); nothing loads it into the falcons directly on
 * the ACR path.
 *
 * Pure: no allocation, output goes to a caller buffer.
 */
#ifndef GPU_NVIDIA_GA10B_CTXSW_UCODE_H
#define GPU_NVIDIA_GA10B_CTXSW_UCODE_H

#include <stddef.h>
#include <stdint.h>

#include "ga10b_netlist.h"

/* gr_falcon_priv.h struct nvgpu_ctxsw_bootloader_desc */
struct ga10b_ctxsw_bootloader_desc {
    uint32_t start_offset;  /* unused by nvgpu */
    uint32_t size;          /* boot image bytes */
    uint32_t imem_offset;
    uint32_t entry_point;
};
static_assert(sizeof(struct ga10b_ctxsw_bootloader_desc) == 16, "bootloader desc");

/* include/nvgpu/gr/gr_falcon.h:106-129 */
struct ga10b_ctxsw_ucode_segment {
    uint32_t offset;
    uint32_t size;
};

struct ga10b_ctxsw_ucode_segments {
    uint32_t boot_entry;
    uint32_t boot_imem_offset;
    uint32_t boot_signature;   /* sum of boot u32 words, gr_falcon.c:292-296 */
    struct ga10b_ctxsw_ucode_segment boot;
    struct ga10b_ctxsw_ucode_segment code;
    struct ga10b_ctxsw_ucode_segment data;
};

struct ga10b_ctxsw_ucode_info {
    struct ga10b_ctxsw_ucode_segments fecs;
    struct ga10b_ctxsw_ucode_segments gpccs;
    uint32_t surface_size;     /* bytes nvgpu allocates (ucode_size) */
};

/* Inputs: the two *_encrypt_prod.bin files and the four netlist
 * regions (raw bytes; sizes need not be multiples of 4). */
struct ga10b_ctxsw_ucode_inputs {
    const uint8_t *fecs_fw;   size_t fecs_fw_size;
    const uint8_t *gpccs_fw;  size_t gpccs_fw_size;
    const uint8_t *fecs_inst;  uint32_t fecs_inst_size;
    const uint8_t *fecs_data;  uint32_t fecs_data_size;
    const uint8_t *gpccs_inst; uint32_t gpccs_inst_size;
    const uint8_t *gpccs_data; uint32_t gpccs_data_size;
};

#define GA10B_CTXSW_OK              0
#define GA10B_CTXSW_ERR_ARG        (-1)
#define GA10B_CTXSW_ERR_FW_SIZE    (-2)  /* fw shorter than desc + boot image */
#define GA10B_CTXSW_ERR_NETLIST    (-3)  /* ucode region missing */
#define GA10B_CTXSW_ERR_OVERFLOW   (-4)  /* layout exceeds 32 bits */
#define GA10B_CTXSW_ERR_BUF_SMALL  (-5)

/* Segment alignment, gr_falcon.c:256. */
#define GA10B_CTXSW_SEGMENT_ALIGN   256u

/* Fill the four ucode pointers from a parsed netlist. The fw fields
 * are left untouched. */
int ga10b_ctxsw_ucode_inputs_from_netlist(const struct ga10b_netlist *nl,
                                          struct ga10b_ctxsw_ucode_inputs *in);

/* Compute the segment table and surface size (no output buffer).
 * boot_signature is computed too. */
int ga10b_ctxsw_ucode_layout(const struct ga10b_ctxsw_ucode_inputs *in,
                             struct ga10b_ctxsw_ucode_info *out);

/* Build the surface into `buf` (zeroed first, then segments written).
 * Fails with GA10B_CTXSW_ERR_BUF_SMALL if buf_size < surface_size;
 * `out` is filled in either case when the layout itself is valid. */
int ga10b_ctxsw_ucode_build(const struct ga10b_ctxsw_ucode_inputs *in,
                            void *buf, size_t buf_size,
                            struct ga10b_ctxsw_ucode_info *out);

#endif /* GPU_NVIDIA_GA10B_CTXSW_UCODE_H */
