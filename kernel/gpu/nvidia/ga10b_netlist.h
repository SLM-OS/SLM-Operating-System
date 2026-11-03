/*
 * ga10b_netlist.h - Parser for the GA10B "NET?_img" netlist container
 * (#1088).
 *
 * A netlist image carries the FECS/GPCCS context-switch ucode
 * (code + data) and the software init lists (bundle, method, ctx
 * load, ...) that nvgpu feeds to GR. The container is a plain table
 * of regions; this module reads it the same way nvgpu's
 * nvgpu_netlist_init_ctx_vars_fw() does (L4T R36.4.x,
 * common/netlist/netlist.c:574-635), with bounds checks nvgpu omits.
 *
 * Pure and hardware independent: no allocation, no copies. Every
 * view points into the caller's blob, which must outlive the views.
 *
 * Wire format (little endian, netlist_priv.h:103-117):
 *
 *   struct netlist_image_header { u32 version; u32 regions; };
 *   struct netlist_region { u32 region_id; u32 data_size;
 *                           u32 data_offset; } regions[regions];
 *
 * data_offset is relative to the start of the image
 * (netlist.c:613: src = (u8 *)netlist + data_offset).
 *
 * --------------------------------------------------------------------
 * Which NET image nvgpu uses on silicon (prod GA10B)
 * --------------------------------------------------------------------
 *
 * nvgpu uses NETC_img_prod_encrypted.bin, and only that file. NETA,
 * NETB and NETD are never opened on silicon. How it decides:
 *
 *  1. hal_ga10b.c:1299-1300 installs ga10b_netlist_get_name and
 *     ga10b_netlist_is_firmware_defined.
 *  2. netlist_ga10b.h:201-202 defines GA10B_NETLIST_DBG_IMAGE_FW_NAME /
 *     GA10B_NETLIST_PROD_IMAGE_FW_NAME as the NETC images ("NETC is FNL
 *     for ga10b", netlist_ga10b.h:200). netlist_ga10b_fusa.c:150-156
 *     therefore makes is_fw_defined() return true.
 *  3. netlist.c:585-589: with is_fw_defined() true the loop starts at
 *     net = NETLIST_FINAL (-1) with max_netlist_num = 0 and
 *     major_v_hw = ~0, so it runs exactly once, for NETLIST_FINAL.
 *  4. netlist_ga10b_fusa.c:37-43 (silicon path, since
 *     nvgpu_platform_is_simulation() is false): NETLIST_FINAL maps to
 *     NETC_img_debug_encrypted.bin if pmu.is_debug_mode_enabled()
 *     (a fuse) and to NETC_img_prod_encrypted.bin otherwise.
 *  5. netlist.c:566-572 nvgpu_netlist_is_valid(): for NETLIST_FINAL the
 *     MAJORV region is NOT compared against the FECS ctx-state-store
 *     major revision. The MAJORV / NETLIST_NUM regions are parsed
 *     (netlist.c:281-289) but only logged.
 *
 * The slot loop with the MAJORV == FECS major-rev check
 * (netlist.c:590-595, get_fecs_ctx_state_store_major_rev_id) only runs
 * when is_fw_defined() is false, which never happens for GA10B; the
 * sim-only names NETA..NETD_img.bin (netlist_defs.h:28-31) are for
 * emulation. ga10b_netlist_is_valid() below mirrors the check anyway
 * so a caller can apply exactly nvgpu's rule.
 *
 * Assumption: production-fused board (is_debug_mode_enabled() false),
 * hence the "_prod_" name. jetson-nano-1/-2 are production parts.
 */
#ifndef GPU_NVIDIA_GA10B_NETLIST_H
#define GPU_NVIDIA_GA10B_NETLIST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Region ids (netlist_priv.h:34-100) ---- */
#define GA10B_NETLIST_REGIONID_FECS_UCODE_DATA                  0u
#define GA10B_NETLIST_REGIONID_FECS_UCODE_INST                  1u
#define GA10B_NETLIST_REGIONID_GPCCS_UCODE_DATA                 2u
#define GA10B_NETLIST_REGIONID_GPCCS_UCODE_INST                 3u
#define GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT                   4u
#define GA10B_NETLIST_REGIONID_SW_CTX_LOAD                      5u
#define GA10B_NETLIST_REGIONID_SW_NON_CTX_LOAD                  6u
#define GA10B_NETLIST_REGIONID_SW_METHOD_INIT                   7u
#define GA10B_NETLIST_REGIONID_CTXREG_SYS                       8u
#define GA10B_NETLIST_REGIONID_CTXREG_GPC                       9u
#define GA10B_NETLIST_REGIONID_CTXREG_TPC                       10u
#define GA10B_NETLIST_REGIONID_CTXREG_ZCULL_GPC                 11u
#define GA10B_NETLIST_REGIONID_CTXREG_PM_SYS                    12u
#define GA10B_NETLIST_REGIONID_CTXREG_PM_GPC                    13u
#define GA10B_NETLIST_REGIONID_CTXREG_PM_TPC                    14u
#define GA10B_NETLIST_REGIONID_MAJORV                           15u
#define GA10B_NETLIST_REGIONID_BUFFER_SIZE                      16u
#define GA10B_NETLIST_REGIONID_CTXSW_REG_BASE_INDEX             17u
#define GA10B_NETLIST_REGIONID_NETLIST_NUM                      18u
#define GA10B_NETLIST_REGIONID_CTXREG_PPC                       19u
#define GA10B_NETLIST_REGIONID_CTXREG_PMPPC                     20u
#define GA10B_NETLIST_REGIONID_NVPERF_CTXREG_SYS                21u
#define GA10B_NETLIST_REGIONID_NVPERF_FBP_CTXREGS               22u
#define GA10B_NETLIST_REGIONID_NVPERF_CTXREG_GPC                23u
#define GA10B_NETLIST_REGIONID_NVPERF_FBP_ROUTER                24u
#define GA10B_NETLIST_REGIONID_NVPERF_GPC_ROUTER                25u
#define GA10B_NETLIST_REGIONID_CTXREG_PMLTC                     26u
#define GA10B_NETLIST_REGIONID_CTXREG_PMFBPA                    27u
#define GA10B_NETLIST_REGIONID_SWVEIDBUNDLEINIT                 28u
#define GA10B_NETLIST_REGIONID_NVPERF_SYS_ROUTER                29u
#define GA10B_NETLIST_REGIONID_NVPERF_PMA                       30u
#define GA10B_NETLIST_REGIONID_CTXREG_PMROP                     31u
#define GA10B_NETLIST_REGIONID_CTXREG_PMUCGPC                   32u
#define GA10B_NETLIST_REGIONID_CTXREG_ETPC                      33u
#define GA10B_NETLIST_REGIONID_SW_BUNDLE64_INIT                 34u
#define GA10B_NETLIST_REGIONID_NVPERF_PMCAU                     35u
#define GA10B_NETLIST_REGIONID_CTXREG_SYS_COMPUTE               36u
#define GA10B_NETLIST_REGIONID_CTXREG_SYS_GFX                   37u
#define GA10B_NETLIST_REGIONID_CTXREG_GPC_COMPUTE               38u
#define GA10B_NETLIST_REGIONID_CTXREG_GPC_GFX                   39u
#define GA10B_NETLIST_REGIONID_CTXREG_TPC_COMPUTE               40u
#define GA10B_NETLIST_REGIONID_CTXREG_TPC_GFX                   41u
#define GA10B_NETLIST_REGIONID_CTXREG_PPC_COMPUTE               42u
#define GA10B_NETLIST_REGIONID_CTXREG_PPC_GFX                   43u
#define GA10B_NETLIST_REGIONID_CTXREG_ETPC_COMPUTE              44u
#define GA10B_NETLIST_REGIONID_CTXREG_ETPC_GFX                  45u
#define GA10B_NETLIST_REGIONID_SW_NON_CTX_LOCAL_COMPUTE_LOAD    48u
#define GA10B_NETLIST_REGIONID_SW_NON_CTX_LOCAL_GFX_LOAD        49u
#define GA10B_NETLIST_REGIONID_SW_NON_CTX_GLOBAL_COMPUTE_LOAD   50u
#define GA10B_NETLIST_REGIONID_SW_NON_CTX_GLOBAL_GFX_LOAD       51u
#define GA10B_NETLIST_REGIONID_NVPERF_SYS_CONTROL               52u
#define GA10B_NETLIST_REGIONID_NVPERF_FBP_CONTROL               53u
#define GA10B_NETLIST_REGIONID_NVPERF_GPC_CONTROL               54u
#define GA10B_NETLIST_REGIONID_NVPERF_PMA_CONTROL               55u
#define GA10B_NETLIST_REGIONID_CTXREG_LTS_BC                    57u
#define GA10B_NETLIST_REGIONID_CTXREG_LTS_UC                    58u

/* ---- Slot indices (netlist_defs.h:34-39) ---- */
#define GA10B_NETLIST_FINAL     (-1)
#define GA10B_NETLIST_SLOT_A    0
#define GA10B_NETLIST_SLOT_B    1
#define GA10B_NETLIST_SLOT_C    2
#define GA10B_NETLIST_SLOT_D    3
#define GA10B_NETLIST_MAX       4

/* Silicon image names (netlist_ga10b.h:190-202). */
#define GA10B_NETLIST_PROD_IMAGE_FW_NAME  "NETC_img_prod_encrypted.bin"
#define GA10B_NETLIST_DBG_IMAGE_FW_NAME   "NETC_img_debug_encrypted.bin"

/* ---- Wire structures ---- */

/* netlist_priv.h:109-112 */
struct ga10b_netlist_image_header {
    uint32_t version;
    uint32_t regions;
};

/* netlist_priv.h:103-107 */
struct ga10b_netlist_region {
    uint32_t region_id;
    uint32_t data_size;
    uint32_t data_offset;
};

/* include/nvgpu/netlist.h:37-64 */
struct ga10b_netlist_av {
    uint32_t addr;
    uint32_t value;
};

struct ga10b_netlist_av64 {
    uint32_t addr;
    uint32_t value_lo;
    uint32_t value_hi;
};

struct ga10b_netlist_aiv {
    uint32_t addr;
    uint32_t index;
    uint32_t value;
};

static_assert(sizeof(struct ga10b_netlist_image_header) == 8, "netlist header");
static_assert(sizeof(struct ga10b_netlist_region) == 12, "netlist region");
static_assert(sizeof(struct ga10b_netlist_av) == 8, "netlist_av");
static_assert(sizeof(struct ga10b_netlist_av64) == 12, "netlist_av64");
static_assert(sizeof(struct ga10b_netlist_aiv) == 12, "netlist_aiv");

/* ---- Error codes ---- */
#define GA10B_NETLIST_OK              0
#define GA10B_NETLIST_ERR_ARG        (-1)  /* NULL pointer / bad index */
#define GA10B_NETLIST_ERR_TRUNCATED  (-2)  /* blob shorter than header/table */
#define GA10B_NETLIST_ERR_BOUNDS     (-3)  /* region data outside the blob */
#define GA10B_NETLIST_ERR_NOT_FOUND  (-4)  /* region id not present */
#define GA10B_NETLIST_ERR_ALIGN      (-5)  /* region data not 4-byte aligned */
#define GA10B_NETLIST_ERR_SIZE       (-6)  /* region too small for a scalar */

/* Parsed handle; non-owning. */
struct ga10b_netlist {
    const uint8_t *data;
    size_t size;
    uint32_t version;
    uint32_t region_count;
};

/* Raw region data: pointer + size into the caller's blob. */
struct ga10b_netlist_blob_view {
    const uint8_t *data;
    uint32_t size;
};

/* u32 list (ucode inst/data). nvgpu rounds the element count up
 * (netlist.c:85-87: count = (len + 3) / 4) and copies `len` bytes into
 * a zeroed buffer, so a partial last word reads with zero padding.
 * `count` matches nvgpu; only `size` bytes at `l` are in the blob, so
 * index `l` only below size / 4 and treat a partial tail as
 * zero-padded. */
struct ga10b_netlist_u32_view {
    const uint32_t *l;
    uint32_t count;     /* nvgpu count, rounded up */
    uint32_t size;      /* region data_size in bytes */
};

/* av / av64 / aiv lists: nvgpu truncates the count
 * (netlist.c:100, 113, 126: count = len / sizeof(element)). */
struct ga10b_netlist_av_view {
    const struct ga10b_netlist_av *l;
    uint32_t count;
};

struct ga10b_netlist_av64_view {
    const struct ga10b_netlist_av64 *l;
    uint32_t count;
};

struct ga10b_netlist_aiv_view {
    const struct ga10b_netlist_aiv *l;
    uint32_t count;
};

/* Validate the header, the region table and every region's
 * [data_offset, data_offset + data_size) against `size`. On failure
 * *out is zeroed. */
int ga10b_netlist_open(struct ga10b_netlist *out, const void *blob, size_t size);

/* Region table entry `idx` (0 <= idx < region_count). */
int ga10b_netlist_region_at(const struct ga10b_netlist *nl, uint32_t idx,
                            struct ga10b_netlist_region *out);

/* Find a region by id. If the id appears more than once the LAST
 * entry wins, as in nvgpu, whose region loop (netlist.c:612-622)
 * overwrites the list for each occurrence. */
int ga10b_netlist_find(const struct ga10b_netlist *nl, uint32_t region_id,
                       struct ga10b_netlist_blob_view *out);

/* Typed views. The element pointer must be 4-byte aligned in memory
 * (blob base + data_offset), otherwise GA10B_NETLIST_ERR_ALIGN. */
int ga10b_netlist_get_u32_list(const struct ga10b_netlist *nl, uint32_t region_id,
                               struct ga10b_netlist_u32_view *out);
int ga10b_netlist_get_av_list(const struct ga10b_netlist *nl, uint32_t region_id,
                              struct ga10b_netlist_av_view *out);
int ga10b_netlist_get_av64_list(const struct ga10b_netlist *nl, uint32_t region_id,
                                struct ga10b_netlist_av64_view *out);
int ga10b_netlist_get_aiv_list(const struct ga10b_netlist *nl, uint32_t region_id,
                               struct ga10b_netlist_aiv_view *out);

/* Scalar regions (MAJORV, BUFFER_SIZE, CTXSW_REG_BASE_INDEX,
 * NETLIST_NUM): nvgpu memcpy's the first 4 bytes (netlist.c:268-289).
 * Regions shorter than 4 bytes are rejected (nvgpu would overread). */
int ga10b_netlist_get_u32(const struct ga10b_netlist *nl, uint32_t region_id,
                          uint32_t *out);

/* nvgpu_netlist_is_valid() (netlist.c:566-572): NETLIST_FINAL is
 * always valid; a slot image is valid only if its MAJORV equals the
 * FECS ctx-state-store major revision read from hardware. */
static inline bool ga10b_netlist_is_valid(int net, uint32_t major_v,
                                          uint32_t major_v_hw)
{
    return !((net != GA10B_NETLIST_FINAL) && (major_v != major_v_hw));
}

/* Image nvgpu opens on GA10B silicon (see the header comment). */
static inline const char *ga10b_netlist_silicon_image_name(bool debug_mode)
{
    return debug_mode ? GA10B_NETLIST_DBG_IMAGE_FW_NAME
                      : GA10B_NETLIST_PROD_IMAGE_FW_NAME;
}

/* Short name of a region id for logs; "?" if unknown. */
const char *ga10b_netlist_region_name(uint32_t region_id);

#endif /* GPU_NVIDIA_GA10B_NETLIST_H */
