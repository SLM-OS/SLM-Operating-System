/*
 * ga10b_acr_blob.h - GA10B non-WPR LS ucode blob + flcn2_acr_desc
 * (#1088).
 *
 * The ACR HS ucode running on the GSP RISC-V core copies a
 * "non-WPR ucode blob" from sysmem into the WPR1 carveout,
 * authenticates and decrypts each LS falcon image in it, and
 * bootstraps those falcons. This module builds that blob exactly as
 * nvgpu_acr_prepare_ucode_blob() does (L4T R36.4.x,
 * common/acr/acr_blob_construct.c:1399-1472) for the configuration
 * SLM-OS mirrors, and the flcn2_acr_desc that points ACR at it
 * (ga10b_acr_patch_wpr_info_to_ucode, common/acr/acr_sw_ga10b.c:
 * 205-354, non-recovery branch).
 *
 * Configuration (all from nvgpu, with the runtime state assumed):
 *
 *  - CONFIG_NVGPU_NON_FUSA, so prepare_ucode_blob is the v1
 *    nvgpu_acr_prepare_ucode_blob (acr_sw_ga10b.c:530-534) and
 *    patch_wpr_info_to_ucode is ga10b_acr_patch_wpr_info_to_ucode.
 *  - GSP is a falcon2/RISC-V core, so NVGPU_ACR_NEXT_CORE_ENABLED and
 *    NVGPU_PKC_LS_SIG_ENABLED are set (acr_sw_ga10b.c:554-558): the
 *    PKC path (lsf_ucode_desc_wrapper, lsf_lsb_header_v1) is used.
 *  - g->support_ls_pmu == false (SLM-OS does not run the LS PMU):
 *    no PMU image in the blob and no ls_pmu_desc; FECS and GPCCS have
 *    is_lazy_bootstrap = false (acr_sw_ga10b.c:417, 443), so ACR
 *    bootstraps them itself.
 *  - NVGPU_SUPPORT_MIG disabled (assumption; nvgpu enables MIG only on
 *    request): FECS is_priv_load = false (acr_sw_ga10b.c:418-422),
 *    GPCCS is_priv_load = true (acr_sw_ga10b.c:444), gpu_mode MIG bit
 *    clear.
 *  - Not simulation, not emulate mode: gpu_mode = 0.
 *  - NVGPU_SEC_SECUREGPCCS enabled (set by
 *    ga10b_fuse_check_priv_security when the priv_sec_en fuse is 1,
 *    hal/fuse/fuse_ga10b.c:302-319; true on production Orin). Without
 *    it nvgpu_acr_lsf_gpccs_ucode_details returns -ENOENT and blob
 *    construction fails (acr_blob_construct.c:309-311).
 *  - CONFIG_NVGPU_LS_PMU=y in the Linux build
 *    (Makefile.linux.configs:27), so UCODE_PARAMS = 1
 *    (acr_blob_construct.h:38-43): every bl_dmem_desc gets argc = 1.
 *    FECS/GPCCS have no get_cmd_line_args_offset, so argv = 0.
 *  - CONFIG_NVGPU_DGPU may be set on Linux but NVGPU_SUPPORT_MULTIPLE_WPR
 *    is not enabled for GA10B and FALCON_ID_SEC2 is not in the LSF
 *    mask, so the sub-WPR paths add nothing.
 *  - acr->lsf[GPCCS].falcon_dma_idx is never assigned by
 *    ga10b_acr_lsf_gpccs (acr_sw_ga10b.c:429-449) and g->acr is
 *    kzalloc'd (acr.c:131), so GPCCS ctx_dma = 0. FECS uses
 *    GK20A_PMU_DMAIDX_UCODE = 0 (acr_sw_ga10b.c:407, pmu.h:193).
 *
 * Blob layout (offsets relative to the blob start == WPR start, since
 * wpr_offset = 0):
 *
 *   0x000  lsf_wpr_header[0]  = GPCCS
 *   0x018  lsf_wpr_header[1]  = FECS
 *   0x030  lsf_wpr_header[2]  = terminator (falcon_id = 0xFFFFFFFF)
 *   then per falcon, in the same order (GPCCS, FECS):
 *     ALIGN(.., 256)   lsf_lsb_header_v1 (2296 B)
 *     ALIGN(.., 4096)  ucode image (boot | code | data from the surface)
 *     ALIGN(.., 256)   flcn_bl_dmem_desc (84 B in a 256 B slot)
 *
 * Order: lsfm_discover_ucode_images walks falcon ids 0..FALCON_ID_END
 * (acr_blob_construct.c:780-809), finding FECS (2) then GPCCS (3), and
 * lsfm_add_ucode_img PREPENDS each node (acr_blob_construct.c:728-729),
 * so every later walk sees GPCCS first.
 *
 * bl_dmem_desc DMA addresses are WPR addresses (wpr_base + ucode_off +
 * ...; acr_blob_construct.c:1077-1089) because ACR moves the blob into
 * WPR before the falcons load from it. The flcn2_acr_desc instead
 * carries the blob's own sysmem address.
 *
 * Pure: no allocation; output goes to caller buffers. Little-endian
 * only (structs are memcpy'd in host byte order, as nvgpu does).
 */
#ifndef GPU_NVIDIA_GA10B_ACR_BLOB_H
#define GPU_NVIDIA_GA10B_ACR_BLOB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "ga10b_ctxsw_ucode.h"

/* ---- Falcon ids (include/nvgpu/falcon.h:120-141) ---- */
#define GA10B_FALCON_ID_PMU             0u
#define GA10B_FALCON_ID_GSPLITE         1u
#define GA10B_FALCON_ID_FECS            2u
#define GA10B_FALCON_ID_GPCCS           3u
#define GA10B_FALCON_ID_PMU_NEXT_CORE   13u
#define GA10B_FALCON_ID_END             17u
#define GA10B_FALCON_ID_INVALID         0xFFFFFFFFu

/* ---- Alignments (nvgpu_acr_interface.h:58-75) ---- */
#define GA10B_LSF_WPR_HEADER_ALIGNMENT      256u
#define GA10B_LSF_LSB_HEADER_ALIGNMENT      256u
#define GA10B_LSF_BL_DATA_ALIGNMENT         256u
#define GA10B_LSF_BL_DATA_SIZE_ALIGNMENT    256u
#define GA10B_LSF_BL_CODE_SIZE_ALIGNMENT    256u
#define GA10B_LSF_DATA_SIZE_ALIGNMENT       256u
#define GA10B_LSF_CODE_SIZE_ALIGNMENT       256u
#define GA10B_LSF_UCODE_DATA_ALIGNMENT      4096u

/* nvgpu_acr_interface.h:133 */
#define GA10B_LSF_IMAGE_STATUS_COPY         1u

/* nvgpu_acr_interface.h:350-357 */
#define GA10B_NV_FLCN_ACR_LSF_FLAG_LOAD_CODE_AT_0_TRUE   (1u << 0)
#define GA10B_NV_FLCN_ACR_LSF_FLAG_DMACTL_REQ_CTX_TRUE   (1u << 2)
#define GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_TRUE  (1u << 3)
#define GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_FALSE 0u

/* acr_blob_construct.h:31-37 */
#define GA10B_APP_IMEM_OFFSET               0u
#define GA10B_APP_IMEM_ENTRY                0u
#define GA10B_APP_DMEM_OFFSET               0u
#define GA10B_APP_RESIDENT_CODE_OFFSET      0u
#define GA10B_LSB_HDR_DATA_SIZE             0u
#define GA10B_BL_START_OFFSET               0u
/* acr_blob_construct.h:38-40 with CONFIG_NVGPU_LS_PMU=y */
#define GA10B_UCODE_PARAMS                  1u

/* acr_sw_ga10b.c:44 */
#define GA10B_ACR_WPR_OFFSET                0u
/* nvgpu_acr_interface.h:747-750 */
#define GA10B_ACR_MIG_MODE                  (1u << 8)
#define GA10B_ACR_SIMULATION_MODE           (1u << 16)
/* include/nvgpu/grmgr.h:33 */
#define GA10B_ACR_EMULATE_MODE_MASK         0xFFu

/* PKC sizes (nvgpu_acr_interface.h:205, 257-281) */
#define GA10B_LSF_FALCON_DEPMAP_SIZE            11u
#define GA10B_LSF_UCODE_COMPONENT_INDEX_MAX     2u
#define GA10B_LSF_SIGNATURE_SIZE_PKC_BYTE       (384u + 128u)
#define GA10B_LS_ENCRYPTION_AES_CBC_IV_SIZE     16u
#define GA10B_FLCN_SIG_SIZE                     4u
#define GA10B_NVGPU_FLCN_ACR_MAX_REGIONS        2u

/* ---- Interface structures (exact nvgpu layouts) ---- */

/* struct acr_generic_header, nvgpu_acr_interface.h:289-293 */
struct ga10b_acr_generic_header {
    uint16_t identifier;
    uint16_t version;
    uint32_t size;
};

/* struct lsf_ucode_desc_v2, nvgpu_acr_interface.h:299-322 */
struct ga10b_lsf_ucode_desc_v2 {
    uint32_t falcon_id;
    uint8_t  b_prd_present;
    uint8_t  b_dbg_present;
    uint16_t reserved;
    uint32_t sig_size;
    uint8_t  prod_sig[GA10B_LSF_UCODE_COMPONENT_INDEX_MAX][GA10B_LSF_SIGNATURE_SIZE_PKC_BYTE];
    uint8_t  debug_sig[GA10B_LSF_UCODE_COMPONENT_INDEX_MAX][GA10B_LSF_SIGNATURE_SIZE_PKC_BYTE];
    uint16_t sig_algo_ver;
    uint16_t sig_algo;
    uint16_t hash_algo_ver;
    uint16_t hash_algo;
    uint32_t sig_algo_padding_type;
    uint8_t  dep_map[GA10B_LSF_FALCON_DEPMAP_SIZE * 8u];
    uint32_t dep_map_count;
    uint8_t  b_supports_versioning;
    uint8_t  pad[3];
    uint32_t ls_ucode_version;
    uint32_t ls_ucode_id;
    uint32_t b_ucode_ls_encrypted;
    uint32_t ls_encalgo_type;
    uint32_t ls_enc_algo_ver;
    uint8_t  ls_enc_iv[GA10B_LS_ENCRYPTION_AES_CBC_IV_SIZE];
    uint8_t  rsvd[36];
};

/* struct lsf_ucode_desc_wrapper, nvgpu_acr_interface.h:328-334. This is
 * exactly the content of fecs/gpccs_pkc_sig_encrypt.bin (2248 B). */
struct ga10b_lsf_ucode_desc_wrapper {
    struct ga10b_acr_generic_header generic_hdr;
    union {
        struct ga10b_lsf_ucode_desc_v2 lsf_ucode_desc_v2;
    };
};

/* struct lsf_wpr_header, nvgpu_acr_interface.h:152-171 */
struct ga10b_lsf_wpr_header {
    uint32_t falcon_id;
    uint32_t lsb_offset;
    uint32_t bootstrap_owner;
    uint32_t lazy_bootstrap;
    uint32_t bin_version;
    uint32_t status;
};

/* struct lsf_lsb_header_v1, nvgpu_acr_interface.h:444-485 */
struct ga10b_lsf_lsb_header_v1 {
    struct ga10b_lsf_ucode_desc_wrapper signature;
    uint32_t ucode_off;
    uint32_t ucode_size;
    uint32_t data_size;
    uint32_t bl_code_size;
    uint32_t bl_imem_off;
    uint32_t bl_data_off;
    uint32_t bl_data_size;
    uint32_t app_code_off;
    uint32_t app_code_size;
    uint32_t app_data_off;
    uint32_t app_data_size;
    uint32_t flags;
};

/* The numeric tail of lsf_lsb_header_v1 (everything after the
 * signature); layout-identical to those fields, used to carry the
 * computed values without a 2.3 KB struct. */
struct ga10b_lsf_lsb_fields {
    uint32_t ucode_off;
    uint32_t ucode_size;
    uint32_t data_size;
    uint32_t bl_code_size;
    uint32_t bl_imem_off;
    uint32_t bl_data_off;
    uint32_t bl_data_size;
    uint32_t app_code_off;
    uint32_t app_code_size;
    uint32_t app_data_off;
    uint32_t app_data_size;
    uint32_t flags;
};

/* struct falc_u64, include/nvgpu/flcnif_cmn.h:31-34 */
struct ga10b_falc_u64 {
    uint32_t lo;
    uint32_t hi;
};

/* struct flcn_bl_dmem_desc, nvgpu_acr_interface.h:544-575 */
struct ga10b_flcn_bl_dmem_desc {
    uint32_t reserved[GA10B_FLCN_SIG_SIZE];
    uint32_t signature[GA10B_FLCN_SIG_SIZE];
    uint32_t ctx_dma;
    struct ga10b_falc_u64 code_dma_base;
    uint32_t non_sec_code_off;
    uint32_t non_sec_code_size;
    uint32_t sec_code_off;
    uint32_t sec_code_size;
    uint32_t code_entry_point;
    struct ga10b_falc_u64 data_dma_base;
    uint32_t data_size;
    uint32_t argc;
    uint32_t argv;
};

/* struct flcn_acr_region_prop, nvgpu_acr_interface.h:653-670 */
struct ga10b_flcn_acr_region_prop {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t region_id;
    uint32_t read_mask;
    uint32_t write_mask;
    uint32_t client_mask;
    uint32_t shadowmMem_startaddress;
};

/* struct flcn_acr_regions, nvgpu_acr_interface.h:677-684 */
struct ga10b_flcn_acr_regions {
    uint32_t no_regions;
    struct ga10b_flcn_acr_region_prop region_props[GA10B_NVGPU_FLCN_ACR_MAX_REGIONS];
};

/* struct flcn2_acr_desc, nvgpu_acr_interface.h:752-779. Natural
 * alignment: the u64 at 0x48 makes the struct 8-aligned, 96 B. */
struct ga10b_flcn2_acr_desc {
    uint32_t wpr_region_id;
    uint32_t wpr_offset;
    struct ga10b_flcn_acr_regions regions;
    uint32_t nonwpr_ucode_blob_size;
    uint64_t nonwpr_ucode_blob_start;
    uint64_t ls_pmu_desc;
    uint32_t gpu_mode;
};

/* struct ls_falcon_ucode_desc, acr_blob_construct.h:54-79. nvgpu
 * builds one per CTXSW falcon in nvgpu_acr_lsf_{fecs,gpccs}_ucode_details;
 * only the fields assigned there are non-zero. */
struct ga10b_ls_falcon_ucode_desc {
    uint32_t descriptor_size;
    uint32_t image_size;
    uint32_t tools_version;
    uint32_t app_version;
    char     date[64];
    uint32_t bootloader_start_offset;
    uint32_t bootloader_size;
    uint32_t bootloader_imem_offset;
    uint32_t bootloader_entry_point;
    uint32_t app_start_offset;
    uint32_t app_size;
    uint32_t app_imem_offset;
    uint32_t app_imem_entry;
    uint32_t app_dmem_offset;
    uint32_t app_resident_code_offset;
    uint32_t app_resident_code_size;
    uint32_t app_resident_data_offset;
    uint32_t app_resident_data_size;
    uint32_t nb_imem_overlays;
    uint32_t nb_dmem_overlays;
    struct { uint32_t start; uint32_t size; } load_ovl[64];
    uint32_t compressed;
};

/* ---- Layout pins ---- */
static_assert(sizeof(struct ga10b_acr_generic_header) == 8, "acr_generic_header");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, sig_size) == 8, "v2.sig_size");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, prod_sig) == 12, "v2.prod_sig");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, debug_sig) == 1036, "v2.debug_sig");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, sig_algo_ver) == 2060, "v2.sig_algo_ver");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, sig_algo_padding_type) == 2068, "v2.pad_type");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, dep_map) == 2072, "v2.dep_map");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, dep_map_count) == 2160, "v2.dep_map_count");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, ls_ucode_version) == 2168, "v2.ls_ucode_version");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_v2, ls_enc_iv) == 2188, "v2.ls_enc_iv");
static_assert(sizeof(struct ga10b_lsf_ucode_desc_v2) == 2240, "lsf_ucode_desc_v2");
static_assert(offsetof(struct ga10b_lsf_ucode_desc_wrapper, lsf_ucode_desc_v2) == 8, "wrapper.v2");
static_assert(sizeof(struct ga10b_lsf_ucode_desc_wrapper) == 2248, "lsf_ucode_desc_wrapper");
static_assert(sizeof(struct ga10b_lsf_wpr_header) == 24, "lsf_wpr_header");
static_assert(offsetof(struct ga10b_lsf_lsb_header_v1, ucode_off) == 2248, "lsb_v1.ucode_off");
static_assert(offsetof(struct ga10b_lsf_lsb_header_v1, bl_data_off) == 2268, "lsb_v1.bl_data_off");
static_assert(offsetof(struct ga10b_lsf_lsb_header_v1, flags) == 2292, "lsb_v1.flags");
static_assert(sizeof(struct ga10b_lsf_lsb_header_v1) == 2296, "lsf_lsb_header_v1");
static_assert(sizeof(struct ga10b_lsf_lsb_fields) ==
              sizeof(struct ga10b_lsf_lsb_header_v1) - offsetof(struct ga10b_lsf_lsb_header_v1, ucode_off),
              "lsb fields tail");
static_assert(offsetof(struct ga10b_flcn_bl_dmem_desc, ctx_dma) == 32, "bl.ctx_dma");
static_assert(offsetof(struct ga10b_flcn_bl_dmem_desc, code_dma_base) == 36, "bl.code_dma_base");
static_assert(offsetof(struct ga10b_flcn_bl_dmem_desc, code_entry_point) == 60, "bl.code_entry_point");
static_assert(offsetof(struct ga10b_flcn_bl_dmem_desc, data_dma_base) == 64, "bl.data_dma_base");
static_assert(offsetof(struct ga10b_flcn_bl_dmem_desc, argv) == 80, "bl.argv");
static_assert(sizeof(struct ga10b_flcn_bl_dmem_desc) == 84, "flcn_bl_dmem_desc");
static_assert(sizeof(struct ga10b_flcn_acr_region_prop) == 28, "flcn_acr_region_prop");
static_assert(sizeof(struct ga10b_flcn_acr_regions) == 60, "flcn_acr_regions");
static_assert(offsetof(struct ga10b_flcn2_acr_desc, regions) == 8, "acr2.regions");
static_assert(offsetof(struct ga10b_flcn2_acr_desc, nonwpr_ucode_blob_size) == 68, "acr2.blob_size");
static_assert(offsetof(struct ga10b_flcn2_acr_desc, nonwpr_ucode_blob_start) == 72, "acr2.blob_start");
static_assert(offsetof(struct ga10b_flcn2_acr_desc, ls_pmu_desc) == 80, "acr2.ls_pmu_desc");
static_assert(offsetof(struct ga10b_flcn2_acr_desc, gpu_mode) == 88, "acr2.gpu_mode");
static_assert(sizeof(struct ga10b_flcn2_acr_desc) == 96, "flcn2_acr_desc");
static_assert(offsetof(struct ga10b_ls_falcon_ucode_desc, bootloader_start_offset) == 80, "desc.bl_start");
static_assert(sizeof(struct ga10b_ls_falcon_ucode_desc) == 80 + 15 * 4 + 64 * 8 + 4, "ls_falcon_ucode_desc");

/* ---- WPR carveout info (nvgpu_acr_wpr_info_sys -> ga10b_fb_read_wpr_info) ---- */

/* BAR0 registers nvgpu reads (hw_fb_ga10b.h:305-310). */
#define GA10B_FB_MMU_WPR1_ADDR_LO   0x001fa81cu
#define GA10B_FB_MMU_WPR1_ADDR_HI   0x001fa820u

struct ga10b_wpr_info {
    uint64_t wpr_base;   /* byte address of WPR1 start */
    uint64_t size;       /* end - start, as nvgpu computes it */
};

/* ga10b_fb_read_wpr_info (hal/fb/fb_ga10b_fusa.c:184-204):
 *   val   = (reg >> 4) & 0x0fffffff            (fb_mmu_wpr1_addr_*_val_v)
 *   addr  = hi32_lo32_to_u64(val >> (32 - 12), (u32)(val << 12))
 *         = (u64)val << 12                     (alignment_v() = 12, 4 KB units)
 *   size  = addr(hi) - addr(lo)
 * jetson-nano-2: lo 0x027d0000, hi 0x027d7e00 -> base 0x27d000000,
 * end 0x27d7e0000, size 0x7e0000. Note nvgpu treats the HI register
 * as the end address (exclusive in its arithmetic). Returns false
 * (and zeroes *out) if hi < lo, where nvgpu_safe_sub_u64 would BUG. */
bool ga10b_acr_wpr_info_from_regs(uint32_t wpr1_addr_lo, uint32_t wpr1_addr_hi,
                                  struct ga10b_wpr_info *out);

/* ---- Blob builder ---- */

#define GA10B_ACR_BLOB_MAX_FALCONS  2u

struct ga10b_acr_blob_inputs {
    /* ctxsw surface + segment table from ga10b_ctxsw_ucode_build(). */
    const uint8_t *surface;
    size_t surface_size;
    const struct ga10b_ctxsw_ucode_info *ucode;
    /* fecs/gpccs_pkc_sig_encrypt.bin (nvgpu copies
     * min(sizeof(wrapper), file size) into a zeroed wrapper,
     * acr_blob_construct.c:228-229, 383-384). */
    const uint8_t *fecs_sig;  size_t fecs_sig_size;
    const uint8_t *gpccs_sig; size_t gpccs_sig_size;
    /* WPR1 start (ga10b_acr_wpr_info_from_regs). */
    uint64_t wpr_base;
};

/* Per managed falcon, in nvgpu list order (GPCCS, then FECS). */
struct ga10b_acr_blob_falcon {
    uint32_t falcon_id;
    struct ga10b_ls_falcon_ucode_desc desc;
    uint32_t src_offset;        /* p_img->data offset into the surface */
    uint32_t data_size;         /* p_img->data_size: bytes copied */
    struct ga10b_lsf_wpr_header wpr_header;
    struct ga10b_lsf_lsb_fields lsb;
    struct ga10b_flcn_bl_dmem_desc bl_desc;
    uint32_t full_ucode_size;
};

struct ga10b_acr_blob_layout {
    uint32_t managed_flcn_cnt;
    struct ga10b_acr_blob_falcon flcn[GA10B_ACR_BLOB_MAX_FALCONS];
    uint32_t terminator_offset; /* managed_flcn_cnt * sizeof(wpr header) */
    uint32_t wpr_size;          /* blob size == ucode_blob.size */
    uint64_t wpr_base;
};

#define GA10B_ACR_OK              0
#define GA10B_ACR_ERR_ARG        (-1)
#define GA10B_ACR_ERR_SURFACE    (-2)  /* image would read past the surface */
#define GA10B_ACR_ERR_OVERFLOW   (-3)
#define GA10B_ACR_ERR_BUF_SMALL  (-4)

/* Compute the full layout (headers, offsets, bl descs) without
 * writing a blob. */
int ga10b_acr_blob_layout(const struct ga10b_acr_blob_inputs *in,
                          struct ga10b_acr_blob_layout *out);

/* Build the blob into `buf` (zeroed first, like nvgpu's GFP_ZERO
 * allocation). `out` is filled whenever the layout is valid; returns
 * GA10B_ACR_ERR_BUF_SMALL if buf_size < out->wpr_size. */
int ga10b_acr_blob_build(const struct ga10b_acr_blob_inputs *in,
                         void *buf, size_t buf_size,
                         struct ga10b_acr_blob_layout *out);

/* ga10b_acr_patch_wpr_info_to_ucode, cold boot (is_recovery = false),
 * first call (acr_falcon2_sysmem_desc not yet allocated), with
 * support_ls_pmu = false, MIG off, not simulation/emulation:
 *   wpr_region_id = 0, regions.no_regions = 1, region_props zero,
 *   wpr_offset = 0, nonwpr_ucode_blob_start = blob_addr,
 *   nonwpr_ucode_blob_size = blob_size, ls_pmu_desc = 0, gpu_mode = 0.
 * Every field nvgpu does not assign stays zero because the desc lives
 * in the kzalloc'd struct nvgpu_acr (acr.c:131). `blob_addr` is what
 * nvgpu_mem_get_addr() returns for ucode_blob: the address the GSP
 * DMAs from (physical when no IOMMU translates GSP accesses).
 * `blob_size` is ucode_blob.size == wpr_size (linux-dma.c:272 keeps
 * the unrounded request size). */
void ga10b_acr_build_flcn2_desc(struct ga10b_flcn2_acr_desc *d,
                                uint64_t blob_addr, uint32_t blob_size);

#endif /* GPU_NVIDIA_GA10B_ACR_BLOB_H */
