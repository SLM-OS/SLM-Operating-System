/*
 * ga10b_grctx.h - GA10B GR context: global context buffers, golden
 * context image and per-channel GR context, built by SLM-OS without
 * Linux (#1088).
 *
 * Follows nvgpu L4T R36.4.x common/gr/obj_ctx.c:
 *   nvgpu_gr_obj_ctx_init_golden_context_image (obj_ctx.c:1137)
 *     -> prepare_inst_block / prepare_gr_ctx / commit_global_ctx_buffers
 *        (patch) / commit_inst / alloc_golden_ctx_image (obj_ctx.c:845):
 *        init_hw_state (obj_ctx.c:610), commit_hw_state (:691),
 *        init_zcull, save_golden_ctx (:772), set_current_ctx_invalid
 *   nvgpu_gr_obj_ctx_alloc (obj_ctx.c:1262) for each channel
 * with the GA10B HALs from hal/init/hal_ga10b.c. The register sequence
 * of the golden image creation was checked write for write against an
 * MMIO trace of nvgpu's first power-on on jetson-nano-2 (lines
 * 17509-25213 of ~/slmos-ref/derivatives/nvgpu-traces/ga10b-r36.4.7-
 * cold-poweron-2026-11-03.trace): only GPU VAs differ, the clock-gating
 * (BLCG) writes are skipped and the read-only capture_gfx_regs is
 * dropped.
 *
 * L4T builds nvgpu with CONFIG_NVGPU_GRAPHICS, GFXP, CILP, DEBUGGER,
 * SET_FALCON_ACCESS_MAP, HAL_NON_FUSA and NON_FUSA
 * (Makefile.shared.configs), so the golden image includes the graphics
 * state (RTV circular buffer, zcull mode) and the per-channel context
 * gets the priv access map and the MME ptimer patch. Everything here
 * matches that build.
 */
#ifndef GPU_NVIDIA_GA10B_GRCTX_H
#define GPU_NVIDIA_GA10B_GRCTX_H

#include "ga10b_chan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ga10b_netlist;

/* ---- GR configuration of this SKU ---- */

/* Orin Nano: GPC0 with 4 TPCs on 2 PES (2 + 2), GPC1 floorswept; the
 * unfused chip has 2 GPCs x 4 TPCs, which is nvgpu's max_tpc_count
 * (sizes the attribute buffer). Checked against the trace: the
 * attribute CB is 0x100000 bytes (tex_rm_cb_1 = 0x2000 x 128 B, line
 * 18168) and both PPCs get beta size 0x1000 (lines 18171/18177). */
struct ga10b_gr_config {
    uint32_t gpc_count;
    uint32_t tpc_count;
    uint32_t max_tpc_count;
    uint32_t ppc_count;          /* PPCs in GPC0 */
    uint32_t pes_tpc_count[2];
};
void ga10b_gr_config_default(struct ga10b_gr_config *c);

/* ---- Global context buffers (common/gr/gr.c:58 gr_alloc_global_ctx_buffers) ---- */

enum ga10b_gr_global_buf {
    GA10B_GR_BUF_BUNDLE_CB = 0,     /* NVGPU_GR_GLOBAL_CTX_CIRCULAR */
    GA10B_GR_BUF_PAGEPOOL,
    GA10B_GR_BUF_ATTRIB_CB,
    GA10B_GR_BUF_RTV_CB,            /* graphics: tu104_gr_init_get_rtv_cb_size */
    GA10B_GR_BUF_PRIV_ACCESS_MAP,
    GA10B_GR_BUF_COUNT,
};

/* gm20b_gr_init_get_global_ctx_cb_buffer_size: bundle_cb_size_div_256b
 * prod 0x30 x 256. */
#define GA10B_GR_BUNDLE_CB_DIV256       0x30u
#define GA10B_GR_BUNDLE_CB_BYTES        (GA10B_GR_BUNDLE_CB_DIV256 * 256u)
/* gm20b_gr_init_get_global_ctx_pagepool_buffer_size: hwmax 0x200
 * pages x 256. */
#define GA10B_GR_PAGEPOOL_PAGES         0x200u
#define GA10B_GR_PAGEPOOL_BYTES         (GA10B_GR_PAGEPOOL_PAGES * 256u)
/* tu104_gr_init_get_rtv_cb_size: (0x800 + 0) x 256. */
#define GA10B_GR_RTV_CB_DIV256          0x800u
#define GA10B_GR_RTV_CB_BYTES           (GA10B_GR_RTV_CB_DIV256 * 256u)
/* NVGPU_GR_GLOBAL_CTX_PRIV_ACCESS_MAP_SIZE (global_ctx.h:46). */
#define GA10B_GR_PRIV_ACCESS_MAP_BYTES  (512u * 1024u)
/* beta / alpha CB default sizes and granularity (hw_gr_ga10b.h). */
#define GA10B_GR_ATTRIB_CB_DEFAULT      0x800u
#define GA10B_GR_ALPHA_CB_DEFAULT       0x800u
#define GA10B_GR_CB_GRANULARITY         0x20u
/* ctxsw_prog_fecs_header_size_in_bytes_v (subcontext header). */
#define GA10B_GR_FECS_HEADER_BYTES      0x300u

static_assert(GA10B_GR_BUNDLE_CB_BYTES == 0x3000u, "bundle CB 12 KB");
static_assert(GA10B_GR_PAGEPOOL_BYTES == 0x20000u, "pagepool 128 KB");
static_assert(GA10B_GR_RTV_CB_BYTES == 0x80000u, "RTV CB 512 KB");

/* gv11b_gr_init_get_global_attr_cb_size. */
uint32_t ga10b_gr_attrib_cb_bytes(const struct ga10b_gr_config *c);
/* gv11b_gr_init_get_patch_slots x PATCH_CTX_SLOTS_REQUIRED_PER_ENTRY,
 * as bytes (nvgpu passes the slot product as the allocation size,
 * obj_ctx.c:939): 2048 slots -> 4096 bytes for 4 TPCs, 64 VEIDs. */
uint32_t ga10b_gr_patch_ctx_bytes(const struct ga10b_gr_config *c);
uint32_t ga10b_gr_global_buf_bytes(const struct ga10b_gr_config *c,
                                   enum ga10b_gr_global_buf b);

/* gr_init_access_map with ga10b_gr_init_get_access_map's 35 registers:
 * bit (reg >> 2) set in a zeroed 512 KB map. */
void ga10b_gr_build_priv_access_map(uint8_t *map, size_t map_bytes);
extern const uint32_t ga10b_gr_priv_access_regs[];
extern const unsigned ga10b_gr_priv_access_regs_count;

/* ---- Context register writes: direct (MMIO) or patch buffer ---- */

/* nvgpu_gr_ctx_patch_write: with `patch` NULL the write goes to BAR0,
 * otherwise (addr, data) is appended to the patch buffer. */
struct ga10b_ctx_writer {
    const struct ga10b_gpu_ops *ops;
    uint32_t *patch;            /* patch buffer, NULL for direct writes */
    uint32_t patch_words;       /* capacity in u32 */
    uint32_t count;             /* patch entries (patch_ctx.data_count) */
    bool overflow;
};
void ga10b_ctx_write(struct ga10b_ctx_writer *w, uint32_t addr, uint32_t data);

/* GPU VAs of the global buffers in one VM. */
struct ga10b_gr_global_vas {
    uint64_t va[GA10B_GR_BUF_COUNT];
};

/* nvgpu_gr_obj_ctx_commit_global_ctx_buffers with support_gfx: the 34
 * writes of trace lines 18154-18187 (pagepool, bundle CB, attribute CB,
 * CB manager, RTV CB), same order. */
void ga10b_gr_commit_global_ctx_buffers(struct ga10b_ctx_writer *w,
                                        const struct ga10b_gr_config *c,
                                        const struct ga10b_gr_global_vas *v);
/* gp10b_gr_init_commit_global_cb_manager (15 writes for 2 PPCs). */
void ga10b_gr_commit_cb_manager(struct ga10b_ctx_writer *w, const struct ga10b_gr_config *c);
#define GA10B_GR_GLOBAL_COMMIT_WRITES   34u
#define GA10B_GR_CB_MANAGER_WRITES      15u

/* ---- Context image header fields (hw_ctxsw_prog_ga10b.h) ---- */

#define GA10B_CTXSW_CTL                     0x00cu
#define GA10B_CTXSW_CTL_TYPE_PER_VEID_HDR   0x21u
#define GA10B_CTXSW_PATCH_COUNT             0x010u
#define GA10B_CTXSW_PATCH_ADR_LO            0x014u
#define GA10B_CTXSW_PATCH_ADR_HI            0x018u
#define GA10B_CTXSW_ZCULL                   0x01cu
#define GA10B_CTXSW_ZCULL_MODE_NO_CTXSW     0x1u
#define GA10B_CTXSW_ZCULL_PTR               0x020u
#define GA10B_CTXSW_PM                      0x028u
#define GA10B_CTXSW_PM_PTR                  0x02cu
#define GA10B_CTXSW_MISC_OPTIONS            0x03cu
#define GA10B_CTXSW_ZCULL_PTR_HI            0x060u
#define GA10B_CTXSW_CONTEXT_BUFFER_PTR_HI   0x078u
#define GA10B_CTXSW_CONTEXT_BUFFER_PTR      0x07cu
#define GA10B_CTXSW_COMPUTE_PREEMPT_OPTIONS 0x084u
#define GA10B_CTXSW_PM_PTR_HI               0x094u
#define GA10B_CTXSW_PRIV_MAP_CONFIG         0x0a0u
#define GA10B_CTXSW_PRIV_MAP_USE_MAP        0x2u
#define GA10B_CTXSW_PRIV_MAP_ADDR_LO        0x0a4u
#define GA10B_CTXSW_PRIV_MAP_ADDR_HI        0x0a8u
#define GA10B_CTXSW_NUM_WFI_SAVE_OPS        0x0d0u
#define GA10B_CTXSW_NUM_CTA_SAVE_OPS        0x0d4u
#define GA10B_CTXSW_NUM_GFXP_SAVE_OPS       0x0d8u
#define GA10B_CTXSW_NUM_CILP_SAVE_OPS       0x0dcu
#define GA10B_CTXSW_NUM_SAVE_OPS            0x0f4u
#define GA10B_CTXSW_NUM_RESTORE_OPS         0x0f8u

static inline uint32_t ga10b_ctx_rd(const void *img, uint32_t off)
{
    const uint8_t *p = (const uint8_t *)img + off;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline void ga10b_ctx_wr(void *img, uint32_t off, uint32_t v)
{
    uint8_t *p = (uint8_t *)img + off;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* nvgpu_gr_subctx_load_ctx_header (+ set_pm_ptr 0, PM ctxsw off):
 * priv access map, patch buffer, zcull ptr 0, context buffer pointer,
 * ctl = type_per_veid_header. */
void ga10b_ctx_header_load(void *hdr, uint64_t priv_map_va, uint64_t patch_va,
                           uint64_t gr_ctx_va);
/* What nvgpu_gr_ctx_load_golden_ctx_image does to the copied image:
 * zero the save/restore op counters, priv access map use_map + VA,
 * verif features off, patch count + VA, PM mode no_ctxsw + PM ptr 0. */
void ga10b_ctx_image_after_golden_load(void *img, uint64_t priv_map_va,
                                       uint32_t patch_count, uint64_t patch_va);
/* nvgpu_gr_ctx_set_preemption_modes for compute: CTA -> 1, CILP -> 2,
 * WFI writes nothing. */
void ga10b_ctx_image_set_compute_preempt(void *img, enum ga10b_compute_preempt m);

/* ---- FECS methods (gm20b_gr_falcon_submit_fecs_method_op) ---- */

#define GA10B_FECS_METHOD_BIND_POINTER          0x03u
#define GA10B_FECS_METHOD_WFI_GOLDEN_SAVE       0x09u
#define GA10B_FECS_METHOD_DISCOVER_IMAGE_SIZE   0x10u
#define GA10B_FECS_METHOD_DISCOVER_ZCULL_SIZE   0x16u
#define GA10B_FECS_METHOD_DISCOVER_PREEMPT_SIZE 0x1au
#define GA10B_FECS_METHOD_DISCOVER_PM_SIZE      0x25u

/* gm20b_gr_falcon_get_fecs_current_ctx_data: inst >> 12 | ncoh | valid. */
static inline uint32_t ga10b_fecs_current_ctx_data(uint64_t inst_phys)
{
    return (uint32_t)((inst_phys >> 12) & 0x0fffffffu) | 0x30000000u | 0x80000000u;
}

/* ---- State ---- */

struct ga10b_grctx {
    struct ga10b_gr_config cfg;
    const struct ga10b_netlist *netlist;    /* NETC (sw_ctx_load, bundles, methods) */
    uint32_t golden_size;       /* FECS DISCOVER_IMAGE_SIZE */
    uint32_t pm_size;           /* FECS DISCOVER_PM_IMAGE_SIZE (debugger) */
    uint32_t zcull_size;        /* FECS DISCOVER_ZCULL_IMAGE_SIZE (graphics) */
    uint32_t preempt_size;      /* FECS DISCOVER_PREEMPTION_IMAGE_SIZE (GFXP) */
    struct ga10b_dma global[GA10B_GR_BUF_COUNT];
    struct ga10b_dma golden;    /* CPU-only copy of the golden image */
    bool golden_ready;
    uint32_t poll_timeout_us;
};

/* Per-channel GR context (one TSG, one subcontext). */
struct ga10b_gr_chan_ctx {
    struct ga10b_dma gr_ctx;
    struct ga10b_dma patch;
    struct ga10b_dma header;    /* subcontext (per-VEID) header */
    uint64_t gr_ctx_va;
    uint64_t patch_va;
    uint64_t header_va;
    struct ga10b_gr_global_vas global;
    uint32_t patch_count;
    uint32_t patch_base;        /* patch_count after the global buffer commits */
};

#define GA10B_GRCTX_OK              0
#define GA10B_GRCTX_ERR_ARG        (-1)
#define GA10B_GRCTX_ERR_NOMEM      (-2)
#define GA10B_GRCTX_ERR_TIMEOUT    (-3)
#define GA10B_GRCTX_ERR_FECS       (-4)   /* FECS method failed */
#define GA10B_GRCTX_ERR_NETLIST    (-5)
#define GA10B_GRCTX_ERR_VM         (-6)
#define GA10B_GRCTX_ERR_STATE      (-7)

/* `golden_size` is the FECS DISCOVER_IMAGE_SIZE answer (0x7d500 on
 * jetson-nano-2, trace line 11558). The netlist must outlive `g`. */
int ga10b_grctx_init(struct ga10b_grctx *g, const struct ga10b_netlist *nl,
                     uint32_t golden_size);

/* After FECS start, beside ga10b_cold's GR init step:
 * the remaining ctxsw image size queries of nvgpu_gr_falcon_init_ctx_state
 * (PM 0x25, zcull 0x16, preemption 0x1a; trace lines 11560-11583) and
 * ga100_gr_init_set_sm_l1tag_surface_collector (trace 11767), which the
 * constant table does not contain. The ELPG reglist methods 0x30-0x32
 * (nvgpu_gr_falcon_bind_fecs_elpg) are not sent: they bind the PMU's
 * power-gating buffer and SLM-OS does not enable ELPG. */
int ga10b_grctx_init_support(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g);

/* Allocate the global buffers and fill the priv access map. */
int ga10b_grctx_alloc_global(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g);

/* Build the golden context image in a private VM and keep a copy.
 * Frees the temporary VM, instance block and buffers before returning
 * (as nvgpu does) and leaves FECS current_ctx invalid. */
int ga10b_grctx_golden_create(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g);

/* Per-channel context (ALLOC_OBJ_CTX + SET_PREEMPTION_MODE): map the
 * global buffers and new gr_ctx/patch/header buffers into `vm`, patch
 * the global buffer VAs, load the golden image, set the subcontext
 * header and point the instance block (`inst_words`, CPU view) at it.
 * The caller cleans the instance block. */
int ga10b_grctx_chan_alloc(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g,
                           struct ga10b_vm *vm, uint32_t *inst_words, uint32_t veid,
                           enum ga10b_compute_preempt preempt,
                           struct ga10b_gr_chan_ctx *c);
void ga10b_grctx_chan_free(const struct ga10b_gpu_ops *ops, struct ga10b_gr_chan_ctx *c);

/* Put a channel context back to what ga10b_grctx_chan_alloc left: the
 * golden image, the patch list as at allocation and the subcontext
 * header, in the buffers and mappings it already has (#1171). Recovery
 * uses it after a step timeout, whose saved context may hold the hung
 * work (a pending semaphore acquire, a faulted SM). Writes memory only,
 * plus two GR register reads (the MME timer bits). The caller makes
 * sure FECS is not using the context. */
int ga10b_grctx_chan_reset(const struct ga10b_gpu_ops *ops, const struct ga10b_grctx *g,
                           enum ga10b_compute_preempt preempt, struct ga10b_gr_chan_ctx *c);

/* fb_flush + l2_flush_dirty + l2_system_invalidate (gk20a_mm_*). */
int ga10b_gr_l2_flush(const struct ga10b_gpu_ops *ops, bool invalidate);

/* Constant fs_state programming repeated during golden creation
 * (nvgpu_gr_fs_state_init, trace lines 18194-18266; identical to the
 * init-support copy except that RMWs read back their earlier result). */
extern const struct ga10b_reg_write ga10b_grctx_fs_state_writes[];
extern const unsigned ga10b_grctx_fs_state_writes_count;

#endif /* GPU_NVIDIA_GA10B_GRCTX_H */
