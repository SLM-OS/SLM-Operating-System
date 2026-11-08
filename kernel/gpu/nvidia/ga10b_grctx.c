/*
 * ga10b_grctx.c - GA10B global context buffers, golden context image
 * and per-channel GR context (#1088). See ga10b_grctx.h.
 *
 * Pure code plus ops callbacks (builds on every platform, tested with
 * fake ops). nvgpu references are L4T R36.4.x paths under
 * drivers/gpu/nvgpu; "trace N" is a line of the cold power-on MMIO
 * trace named in the header.
 */

#include "ga10b_grctx.h"
#include "ga10b_netlist.h"

#include "string.h"

/* ---- Registers (hw_gr_ga10b.h; addresses also seen in the trace) ---- */
#define GR_STATUS                       0x400700u
#define GR_STATUS_BUSY                  0x1u
#define GR_STATUS_FE_METHOD_LOWER       0x4u
#define GR_DEBUG_2                      0x400088u
#define GR_DEBUG_2_GFXP_WFI_UNIT_M      (1u << 27)
#define GR_DEBUG_2_GFX_AUTO_GO_IDLE     (1u << 17)
#define GR_DEBUG_2_COMPUTE_AUTO_GO_IDLE (1u << 18)
#define GR_PIPE_BUNDLE_ADDRESS          0x400200u
#define GR_PIPE_BUNDLE_DATA             0x400204u
#define GR_PIPE_BUNDLE_CONFIG           0x400208u
#define GR_PIPE_BUNDLE_DATA_HI          0x40020cu
#define GR_PIPE_MODE_OVERRIDE_ENABLED   0x80000000u
#define GR_GO_IDLE_BUNDLE               0xe100u   /* gr_init_gm20b.h:29 */
#define GR_FE_GO_IDLE_TIMEOUT           0x404154u
#define GR_FE_GO_IDLE_TIMEOUT_PROD      0x1800u
#define GR_FE_COMPUTE_GO_IDLE_TIMEOUT   0x404160u
#define GR_FE_COMPUTE_GO_IDLE_INIT      0x40u
#define GR_FE_PWR_MODE                  0x404170u
#define GR_FE_PWR_MODE_REQ_SEND         0x10u
#define GR_FE_PWR_MODE_FORCE_ON         0x2u
#define GR_FE_PWR_MODE_REQ              (1u << 4)
#define GR_MME_SHADOW_RAM_INDEX         0x404488u
#define GR_MME_SHADOW_RAM_DATA          0x40448cu
#define GR_MME_SHADOW_WRITE_TRIGGER     0x80000000u
#define GR_PRI_MME_CONFIG               0x404498u
#define GR_PRI_MME_FE1_CONFIG           0x40a798u
#define GR_MME_PTIMER_ENABLE            0x2u
#define GR_FECS_CTXSW_RESET_CTL         0x409614u
#define GR_GPCCS_CTXSW_RESET_CTL        0x41a614u
#define GR_FECS_METHOD_DATA             0x409500u
#define GR_FECS_METHOD_PUSH             0x409504u
#define GR_FECS_CTXSW_MAILBOX(i)        (0x409800u + (i) * 4u)
#define GR_FECS_CURRENT_CTX             0x409b00u
/* Context-image header words the CPU patches before golden save
 * (zcull mode/pointers): clean the first 64 words. */
#define GA10B_CTXSW_HDR_CLEAN_BYTES     (64u * 4u)
static_assert(GA10B_CTXSW_ZCULL_PTR_HI < GA10B_CTXSW_HDR_CLEAN_BYTES,
              "zcull header words must lie inside the cleaned range");
#define GR_SM0_WARP_ESR_REPORT_MASK     0x504728u
#define GR_SM0_GLOBAL_ESR_REPORT_MASK   0x50472cu
#define GR_SMS_HWW_GLOBAL_ESR           0x419eb4u
#define GR_SMS_WARP_ESR_REPORT_MASK     0x419ea8u
#define GR_SMS_GLOBAL_ESR_REPORT_MASK   0x419eacu
/* get_sm_hww_{warp,global}_esr_report_mask (gr_intr_ga10b_fusa.c),
 * OR of the *_report_f() fields; trace 18092-18093. */
#define GA10B_SM_WARP_ESR_REPORT        0x0f85eb64u
#define GA10B_SM_GLOBAL_ESR_REPORT      0x00001174u
#define GR_PE_VAF                       0x41980cu
#define GR_PE_VAF_FAST_MODE_SWITCH      0x10u
#define GR_PES_VSC_VPC                  0x41be08u
#define GR_PES_VSC_VPC_FAST_MODE_SWITCH 0x4u
#define GR_PD_AB_DIST_CFG0              0x4064c0u
#define GR_PD_AB_DIST_CFG0_TIMESLICE_EN 0x80000000u
#define GR_GPC0_TPC0_SM_L1TAG_CTRL      0x5043f0u
#define GR_GPCS_TPCS_SM_L1TAG_CTRL      0x419bf0u
#define GR_L1TAG_SURFACE_CUT_COLLECTOR  0x20000000u

/* Global buffer commit registers. */
#define GR_SCC_BUNDLE_CB_BASE           0x408004u
#define GR_SCC_BUNDLE_CB_SIZE           0x408008u
#define GR_SCC_PAGEPOOL_BASE            0x40800cu
#define GR_SCC_PAGEPOOL                 0x408010u
#define GR_SCC_RM_RTV_CB_BASE           0x408070u
#define GR_SCC_RM_RTV_CB_SIZE           0x408074u
#define GR_SCC_RM_GFXP_RESERVE          0x408078u
#define GR_GPCS_GCC_PAGEPOOL_BASE       0x419004u
#define GR_GPCS_GCC_PAGEPOOL            0x419008u
#define GR_GPCS_GCC_RM_RTV_CB_BASE      0x419034u
#define GR_GPCS_SWDX_BUNDLE_CB_BASE     0x418e24u
#define GR_GPCS_SWDX_BUNDLE_CB_SIZE     0x418e28u
#define GR_GPCS_SWDX_TC_BETA_CB_SIZE(i) (0x418ea0u + (i) * 4u)
#define GR_PD_AB_DIST_CFG1              0x4064c4u
#define GR_PD_AB_DIST_CFG2              0x4064c8u
#define GR_DS_TGA_CONSTRAINTLOGIC_BETA  0x405830u
#define GR_DS_TGA_CONSTRAINTLOGIC_ALPHA 0x40585cu
#define GR_GPCS_SETUP_ATTRIB_CB_BASE    0x418810u
#define GR_GPCS_TPCS_PE_PIN_CB_BASE     0x419848u
#define GR_GPCS_TPCS_MPC_VTG_CB_BASE    0x419c2cu
#define GR_GPCS_TPCS_TEX_RM_CB_0        0x419e00u
#define GR_GPCS_TPCS_TEX_RM_CB_1        0x419e04u
#define GR_GPC0_PPC0_CBM_BETA_CB_SIZE   0x5030c0u
#define GR_GPC0_PPC0_CBM_BETA_CB_OFFSET 0x5030f4u
#define GR_GPC0_PPC0_CBM_BETA_STEADY    0x5030f0u
#define GR_GPC0_PPC0_CBM_ALPHA_CB_SIZE  0x5030e4u
#define GR_GPC0_PPC0_CBM_ALPHA_CB_OFFSET 0x5030f8u
#define GR_PPC_IN_GPC_STRIDE            0x200u   /* proj_ppc_in_gpc_stride_v */
#define GR_PD_AB_TOKEN_LIMIT_INIT       0x480u
#define GR_PD_AB_MIN_GPM_FIFO_DEPTH     0x480u
#define GR_PD_AB_STATE_LIMIT_GRAN       0x20u
#define GR_PD_AB_MAX_OUTPUT_GRAN        0x80u
#define GR_PD_AB_MAX_BATCHES_INIT       0xffffu
#define GR_VALID_BIT31                  0x80000000u
#define GR_VALID_BIT28                  0x10000000u

/* Flush unit (hw_flush_gk20a.h, trace 25214-25220). */
#define FLUSH_FB_FLUSH                  0x070000u
#define FLUSH_L2_SYSTEM_INVALIDATE      0x070004u
#define FLUSH_L2_FLUSH_DIRTY            0x070010u
#define FLUSH_PENDING_OUTSTANDING       0x3u

/* ==================================================================
 * Configuration and sizes
 * ================================================================== */

void ga10b_gr_config_default(struct ga10b_gr_config *c)
{
    c->gpc_count = 1;
    c->tpc_count = 4;
    c->max_tpc_count = 8;
    c->ppc_count = 2;
    c->pes_tpc_count[0] = 2;
    c->pes_tpc_count[1] = 2;
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* gv11b_gr_init_get_attrib_cb_size / get_alpha_cb_size. */
static uint32_t attrib_cb_size(const struct ga10b_gr_config *c)
{
    return min_u32(GA10B_GR_ATTRIB_CB_DEFAULT, 0x3fffffu / c->tpc_count);
}
static uint32_t alpha_cb_size(const struct ga10b_gr_config *c)
{
    return min_u32(GA10B_GR_ALPHA_CB_DEFAULT, 0xffffu / c->tpc_count);
}

uint32_t ga10b_gr_attrib_cb_bytes(const struct ga10b_gr_config *c)
{
    uint32_t size = attrib_cb_size(c) * GA10B_GR_CB_GRANULARITY * c->max_tpc_count +
                    alpha_cb_size(c) * GA10B_GR_CB_GRANULARITY * c->max_tpc_count;
    return (size + 127u) & ~127u;
}

uint32_t ga10b_gr_patch_ctx_bytes(const struct ga10b_gr_config *c)
{
    const uint32_t slots_per_page = 4096u / (2u * 4u);  /* PATCH_CTX_SLOTS_PER_PAGE */
    uint32_t size = 1u + (c->tpc_count + 31u) / 32u + (c->tpc_count + 3u) / 4u;
    size *= GA10B_MAX_SUBCTX;
    size += 2u;
    size += slots_per_page;                              /* gm20b_gr_init_get_patch_slots */
    size = (size + slots_per_page - 1u) / slots_per_page * slots_per_page;
    size += 2u * slots_per_page;
    return size * 2u;                                    /* PATCH_CTX_SLOTS_REQUIRED_PER_ENTRY */
}

uint32_t ga10b_gr_global_buf_bytes(const struct ga10b_gr_config *c,
                                   enum ga10b_gr_global_buf b)
{
    switch (b) {
    case GA10B_GR_BUF_BUNDLE_CB:       return GA10B_GR_BUNDLE_CB_BYTES;
    case GA10B_GR_BUF_PAGEPOOL:        return GA10B_GR_PAGEPOOL_BYTES;
    case GA10B_GR_BUF_ATTRIB_CB:       return ga10b_gr_attrib_cb_bytes(c);
    case GA10B_GR_BUF_RTV_CB:          return GA10B_GR_RTV_CB_BYTES;
    case GA10B_GR_BUF_PRIV_ACCESS_MAP: return GA10B_GR_PRIV_ACCESS_MAP_BYTES;
    default:                           return 0;
    }
}

/* ga10b_gr_init_get_access_map (gr_init_ga10b.c), sorted. */
const uint32_t ga10b_gr_priv_access_regs[] = {
    0x418380, 0x418800, 0x418830, 0x4188b0, 0x4188fc, 0x418e00, 0x418e40,
    0x418e44, 0x418e48, 0x418e4c, 0x418e50, 0x418e58, 0x418e5c, 0x418e60,
    0x418e64, 0x418e68, 0x418e6c, 0x418e70, 0x418e74, 0x418e78, 0x418e7c,
    0x418e80, 0x418e84, 0x418e88, 0x418e8c, 0x418e90, 0x418e94, 0x419864,
    0x419a04, 0x419a08, 0x419b48, 0x419ba4, 0x419e84, 0x419ea8, 0x419eac,
};
const unsigned ga10b_gr_priv_access_regs_count =
    sizeof(ga10b_gr_priv_access_regs) / sizeof(ga10b_gr_priv_access_regs[0]);

void ga10b_gr_build_priv_access_map(uint8_t *map, size_t map_bytes)
{
    memset(map, 0, map_bytes);
    for (unsigned i = 0; i < ga10b_gr_priv_access_regs_count; i++) {
        uint32_t bit = ga10b_gr_priv_access_regs[i] >> 2;
        if ((bit >> 3) < map_bytes) map[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
    }
}

/* ==================================================================
 * Context writes
 * ================================================================== */

void ga10b_ctx_write(struct ga10b_ctx_writer *w, uint32_t addr, uint32_t data)
{
    if (!w->patch) {
        w->ops->wr32(w->ops->ctx, addr, data);
        return;
    }
    /* nvgpu_gr_ctx_patch_write: slot = count * 2, max = words - 2. */
    uint32_t slot = w->count * 2u;
    if (w->patch_words < 2u || slot > w->patch_words - 2u) {
        w->overflow = true;
        return;
    }
    w->patch[slot] = addr;
    w->patch[slot + 1u] = data;
    w->count++;
}

void ga10b_gr_commit_cb_manager(struct ga10b_ctx_writer *w, const struct ga10b_gr_config *c)
{
    uint32_t attrib_default = GA10B_GR_ATTRIB_CB_DEFAULT;
    uint32_t alpha_default = GA10B_GR_ALPHA_CB_DEFAULT;
    uint32_t attrib_size = attrib_cb_size(c);     /* attrib_size_in_chunk (no GFXP) */
    uint32_t alpha_size = alpha_cb_size(c);

    ga10b_ctx_write(w, GR_DS_TGA_CONSTRAINTLOGIC_BETA, attrib_default);
    ga10b_ctx_write(w, GR_DS_TGA_CONSTRAINTLOGIC_ALPHA, alpha_default);
    uint32_t max_output = alpha_default * GA10B_GR_CB_GRANULARITY / GR_PD_AB_MAX_OUTPUT_GRAN;
    ga10b_ctx_write(w, GR_PD_AB_DIST_CFG1, ((max_output & 0xffffu) << 16) |
                                           GR_PD_AB_MAX_BATCHES_INIT);

    uint32_t alpha_off = 0;
    uint32_t attrib_off = alpha_off + c->tpc_count * alpha_size;
    /* One GPC (gpc_stride * 0); PES index = ppc. */
    for (uint32_t ppc = 0; ppc < c->ppc_count && ppc < 2u; ppc++) {
        uint32_t n = c->pes_tpc_count[ppc];
        uint32_t o = GR_PPC_IN_GPC_STRIDE * ppc;
        uint32_t beta = attrib_default * n;
        uint32_t alpha = alpha_default * n;
        uint32_t steady = attrib_default * n;
        ga10b_ctx_write(w, GR_GPC0_PPC0_CBM_BETA_CB_SIZE + o, beta);
        ga10b_ctx_write(w, GR_GPC0_PPC0_CBM_BETA_CB_OFFSET + o, attrib_off);
        ga10b_ctx_write(w, GR_GPC0_PPC0_CBM_BETA_STEADY + o, steady);
        attrib_off += attrib_size * n;
        ga10b_ctx_write(w, GR_GPC0_PPC0_CBM_ALPHA_CB_SIZE + o, alpha);
        ga10b_ctx_write(w, GR_GPC0_PPC0_CBM_ALPHA_CB_OFFSET + o, alpha_off);
        alpha_off += alpha_size * n;
        ga10b_ctx_write(w, GR_GPCS_SWDX_TC_BETA_CB_SIZE(ppc), steady & 0x3fffffu);
    }
}

void ga10b_gr_commit_global_ctx_buffers(struct ga10b_ctx_writer *w,
                                        const struct ga10b_gr_config *c,
                                        const struct ga10b_gr_global_vas *v)
{
    /* gp10b_gr_init_commit_global_pagepool (global_ctx): size in 256 B
     * pages, the hwmax value encoded as 0. */
    uint32_t pp = (uint32_t)(v->va[GA10B_GR_BUF_PAGEPOOL] >> 8);
    uint32_t pp_pages = GA10B_GR_PAGEPOOL_BYTES / 256u;
    if (pp_pages == GA10B_GR_PAGEPOOL_PAGES) pp_pages = 0;
    ga10b_ctx_write(w, GR_SCC_PAGEPOOL_BASE, pp);
    ga10b_ctx_write(w, GR_SCC_PAGEPOOL, (pp_pages & 0x3ffu) | GR_VALID_BIT31);
    ga10b_ctx_write(w, GR_GPCS_GCC_PAGEPOOL_BASE, pp);
    ga10b_ctx_write(w, GR_GPCS_GCC_PAGEPOOL, pp_pages & 0x3ffu);

    /* ga10b_gr_init_commit_global_bundle_cb. */
    uint32_t cb = (uint32_t)(v->va[GA10B_GR_BUF_BUNDLE_CB] >> 8);
    uint32_t sz = GA10B_GR_BUNDLE_CB_DIV256;
    ga10b_ctx_write(w, GR_SCC_BUNDLE_CB_BASE, cb);
    ga10b_ctx_write(w, GR_SCC_BUNDLE_CB_SIZE, (sz & 0x7ffu) | GR_VALID_BIT31);
    ga10b_ctx_write(w, GR_GPCS_SWDX_BUNDLE_CB_BASE, cb);
    ga10b_ctx_write(w, GR_GPCS_SWDX_BUNDLE_CB_SIZE, (sz & 0x7ffu) | GR_VALID_BIT31);
    uint32_t state = min_u32(GA10B_GR_BUNDLE_CB_DIV256 * 256u / GR_PD_AB_STATE_LIMIT_GRAN,
                             GR_PD_AB_MIN_GPM_FIFO_DEPTH);
    ga10b_ctx_write(w, GR_PD_AB_DIST_CFG2, (GR_PD_AB_TOKEN_LIMIT_INIT & 0x3fffu) |
                                           ((state & 0x3fffu) << 16));

    /* gv11b_gr_init_commit_global_attrib_cb (calls the gm20b one first). */
    uint32_t at = (uint32_t)(v->va[GA10B_GR_BUF_ATTRIB_CB] >> 12);
    ga10b_ctx_write(w, GR_GPCS_SETUP_ATTRIB_CB_BASE, (at & 0x0fffffffu) | GR_VALID_BIT31);
    ga10b_ctx_write(w, GR_GPCS_TPCS_PE_PIN_CB_BASE, (at & 0x0fffffffu) | GR_VALID_BIT28);
    /* Written twice on purpose: gm20b_gr_init_commit_global_attrib_cb
     * (gr_init_gm20b_fusa.c:436) and then gv11b's override
     * (gr_init_gv11b_fusa.c:741) both patch it; the patch-buffer count
     * the tests pin against the trace includes both entries. */
    ga10b_ctx_write(w, GR_GPCS_TPCS_MPC_VTG_CB_BASE, (at & 0x0fffffffu) | GR_VALID_BIT28);
    ga10b_ctx_write(w, GR_GPCS_TPCS_MPC_VTG_CB_BASE, (at & 0x0fffffffu) | GR_VALID_BIT28);
    ga10b_ctx_write(w, GR_GPCS_TPCS_TEX_RM_CB_0, at);
    ga10b_ctx_write(w, GR_GPCS_TPCS_TEX_RM_CB_1,
                    ((ga10b_gr_attrib_cb_bytes(c) / 128u) & 0x1fffffu) | GR_VALID_BIT31);

    ga10b_gr_commit_cb_manager(w, c);

    /* tu104_gr_init_commit_rtv_cb. */
    uint32_t rtv = (uint32_t)(v->va[GA10B_GR_BUF_RTV_CB] >> 8);
    ga10b_ctx_write(w, GR_SCC_RM_RTV_CB_BASE, rtv);
    ga10b_ctx_write(w, GR_SCC_RM_RTV_CB_SIZE, GA10B_GR_RTV_CB_DIV256 & 0x7fffu);
    ga10b_ctx_write(w, GR_GPCS_GCC_RM_RTV_CB_BASE, rtv);
    ga10b_ctx_write(w, GR_SCC_RM_GFXP_RESERVE, 0u);
}

/* ==================================================================
 * Context image fields
 * ================================================================== */

void ga10b_ctx_header_load(void *hdr, uint64_t priv_map_va, uint64_t patch_va,
                           uint64_t gr_ctx_va)
{
    /* nvgpu_gr_obj_ctx_set_pm_ctx_gpu_va -> subctx set_pm_ptr(0). */
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PM_PTR, 0u);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PM_PTR_HI, 0u);
    /* nvgpu_gr_subctx_load_ctx_header. */
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PRIV_MAP_ADDR_LO, (uint32_t)priv_map_va);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PRIV_MAP_ADDR_HI, (uint32_t)(priv_map_va >> 32));
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PATCH_ADR_LO, (uint32_t)patch_va);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_PATCH_ADR_HI, (uint32_t)(patch_va >> 32));
    ga10b_ctx_wr(hdr, GA10B_CTXSW_ZCULL_PTR, 0u);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_ZCULL_PTR_HI, 0u);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_CONTEXT_BUFFER_PTR_HI, (uint32_t)(gr_ctx_va >> 32));
    ga10b_ctx_wr(hdr, GA10B_CTXSW_CONTEXT_BUFFER_PTR, (uint32_t)gr_ctx_va);
    ga10b_ctx_wr(hdr, GA10B_CTXSW_CTL, GA10B_CTXSW_CTL_TYPE_PER_VEID_HDR);
}

void ga10b_ctx_image_after_golden_load(void *img, uint64_t priv_map_va,
                                       uint32_t patch_count, uint64_t patch_va)
{
    /* gp10b_ctxsw_prog_init_ctxsw_hdr_data (+ gm20b). */
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_WFI_SAVE_OPS, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_CTA_SAVE_OPS, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_GFXP_SAVE_OPS, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_CILP_SAVE_OPS, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_SAVE_OPS, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_NUM_RESTORE_OPS, 0u);
    /* priv access map: g->allow_all defaults false -> use_map. */
    ga10b_ctx_wr(img, GA10B_CTXSW_PRIV_MAP_CONFIG, GA10B_CTXSW_PRIV_MAP_USE_MAP);
    ga10b_ctx_wr(img, GA10B_CTXSW_PRIV_MAP_ADDR_LO, (uint32_t)priv_map_va);
    ga10b_ctx_wr(img, GA10B_CTXSW_PRIV_MAP_ADDR_HI, (uint32_t)(priv_map_va >> 32));
    /* gm20b_ctxsw_prog_disable_verif_features. */
    ga10b_ctx_wr(img, GA10B_CTXSW_MISC_OPTIONS,
                 ga10b_ctx_rd(img, GA10B_CTXSW_MISC_OPTIONS) & ~(1u << 3));
    ga10b_ctx_wr(img, GA10B_CTXSW_PATCH_COUNT, patch_count);
    ga10b_ctx_wr(img, GA10B_CTXSW_PATCH_ADR_LO, (uint32_t)patch_va);
    ga10b_ctx_wr(img, GA10B_CTXSW_PATCH_ADR_HI, (uint32_t)(patch_va >> 32));
    /* PM ctxsw off: mode no_ctxsw, pm ptr 0. */
    ga10b_ctx_wr(img, GA10B_CTXSW_PM, ga10b_ctx_rd(img, GA10B_CTXSW_PM) & ~0x7u);
    ga10b_ctx_wr(img, GA10B_CTXSW_PM_PTR, 0u);
    ga10b_ctx_wr(img, GA10B_CTXSW_PM_PTR_HI, 0u);
}

void ga10b_ctx_image_set_compute_preempt(void *img, enum ga10b_compute_preempt m)
{
    if (m == GA10B_PREEMPT_CTA) {
        ga10b_ctx_wr(img, GA10B_CTXSW_COMPUTE_PREEMPT_OPTIONS, 0x1u);
    } else if (m == GA10B_PREEMPT_CILP) {
        ga10b_ctx_wr(img, GA10B_CTXSW_COMPUTE_PREEMPT_OPTIONS, 0x2u);
    }
}

/* ==================================================================
 * Polling and FECS methods
 * ================================================================== */

static uint32_t rd(const struct ga10b_gpu_ops *o, uint32_t r) { return o->rd32(o->ctx, r); }
static void wr(const struct ga10b_gpu_ops *o, uint32_t r, uint32_t v) { o->wr32(o->ctx, r, v); }

static int poll_mask_clear(const struct ga10b_gpu_ops *o, uint32_t reg, uint32_t mask,
                           uint32_t timeout_us)
{
    for (uint32_t t = 0; t <= timeout_us; t += 10u) {
        if ((rd(o, reg) & mask) == 0u) return GA10B_GRCTX_OK;
        o->udelay(o->ctx, 10u);
    }
    return GA10B_GRCTX_ERR_TIMEOUT;
}

/* ga10b_gr_init_wait_idle / gm20b_gr_init_wait_fe_idle. */
static int wait_idle(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g)
{
    return poll_mask_clear(o, GR_STATUS, GR_STATUS_BUSY, g->poll_timeout_us);
}
static int wait_fe_idle(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g)
{
    return poll_mask_clear(o, GR_STATUS, GR_STATUS_FE_METHOD_LOWER, g->poll_timeout_us);
}

enum fecs_cond { COND_SKIP, COND_EQUAL, COND_NOT_EQUAL, COND_AND };

struct fecs_op {
    uint32_t mb_id, mb_data, clr;
    uint32_t addr, data;
    enum fecs_cond ok_cond, fail_cond;
    uint32_t ok, fail;
};

static bool cond_hit(enum fecs_cond c, uint32_t reg, uint32_t v)
{
    switch (c) {
    case COND_EQUAL:     return reg == v;
    case COND_NOT_EQUAL: return reg != v;
    case COND_AND:       return (reg & v) != 0u;
    default:             return false;
    }
}

static int fecs_submit(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g,
                       const struct fecs_op *op, uint32_t *ret)
{
    if (op->mb_id != 0u) wr(o, GR_FECS_CTXSW_MAILBOX(op->mb_id), op->mb_data);
    /* ga10b_gr_falcon_fecs_ctxsw_clear_mailbox: RMW clear of mailbox 0. */
    wr(o, GR_FECS_CTXSW_MAILBOX(0), rd(o, GR_FECS_CTXSW_MAILBOX(0)) & ~op->clr);
    wr(o, GR_FECS_METHOD_DATA, op->data);
    wr(o, GR_FECS_METHOD_PUSH, op->addr & 0xfffu);
    uint32_t id = (op->mb_id == 4u) ? 0u : op->mb_id;
    for (uint32_t t = 0; t <= g->poll_timeout_us; t += 10u) {
        uint32_t r = rd(o, GR_FECS_CTXSW_MAILBOX(id));
        if (ret) *ret = r;
        if (op->ok_cond == COND_SKIP && op->fail_cond == COND_SKIP) return GA10B_GRCTX_OK;
        bool ok = cond_hit(op->ok_cond, r, op->ok);
        bool fail = cond_hit(op->fail_cond, r, op->fail);
        if (fail) return GA10B_GRCTX_ERR_FECS;
        if (ok) return GA10B_GRCTX_OK;
        o->udelay(o->ctx, 10u);
    }
    return GA10B_GRCTX_ERR_TIMEOUT;
}

/* Image size queries: data 0, clear all, wait for mailbox 0 != 0. */
static int fecs_query(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g,
                      uint32_t method, uint32_t *ret)
{
    struct fecs_op op = { .clr = ~0u, .addr = method, .ok_cond = COND_NOT_EQUAL,
                          .ok = 0u, .fail_cond = COND_SKIP };
    return fecs_submit(o, g, &op, ret);
}

int ga10b_gr_l2_flush(const struct ga10b_gpu_ops *ops, bool invalidate)
{
    static const uint32_t regs[3] = { FLUSH_FB_FLUSH, FLUSH_L2_FLUSH_DIRTY,
                                      FLUSH_L2_SYSTEM_INVALIDATE };
    unsigned n = invalidate ? 3u : 2u;
    for (unsigned i = 0; i < n; i++) {
        wr(ops, regs[i], 0x1u);
        if (poll_mask_clear(ops, regs[i], FLUSH_PENDING_OUTSTANDING, 100000u) != 0) {
            return GA10B_GRCTX_ERR_TIMEOUT;
        }
    }
    return GA10B_GRCTX_OK;
}

/* ==================================================================
 * Init support and global buffers
 * ================================================================== */

int ga10b_grctx_init(struct ga10b_grctx *g, const struct ga10b_netlist *nl,
                     uint32_t golden_size)
{
    if (!g || !nl || golden_size == 0u || (golden_size & 3u)) return GA10B_GRCTX_ERR_ARG;
    memset(g, 0, sizeof(*g));
    ga10b_gr_config_default(&g->cfg);
    g->netlist = nl;
    g->golden_size = golden_size;
    g->poll_timeout_us = 3000000u;   /* nvgpu_get_poll_timeout default 3 s */
    return GA10B_GRCTX_OK;
}

int ga10b_grctx_init_support(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g)
{
    if (!ops || !g) return GA10B_GRCTX_ERR_ARG;
    int rc = fecs_query(ops, g, GA10B_FECS_METHOD_DISCOVER_PM_SIZE, &g->pm_size);
    if (rc == 0) rc = fecs_query(ops, g, GA10B_FECS_METHOD_DISCOVER_ZCULL_SIZE, &g->zcull_size);
    if (rc == 0) rc = fecs_query(ops, g, GA10B_FECS_METHOD_DISCOVER_PREEMPT_SIZE, &g->preempt_size);
    if (rc != 0) return rc;
    wr(ops, GR_GPCS_TPCS_SM_L1TAG_CTRL,
       rd(ops, GR_GPC0_TPC0_SM_L1TAG_CTRL) | GR_L1TAG_SURFACE_CUT_COLLECTOR);
    return wait_idle(ops, g);
}

int ga10b_grctx_alloc_global(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g)
{
    if (!ops || !g) return GA10B_GRCTX_ERR_ARG;
    for (unsigned b = 0; b < GA10B_GR_BUF_COUNT; b++) {
        if (g->global[b].cpu) continue;
        uint32_t sz = ga10b_gr_global_buf_bytes(&g->cfg, (enum ga10b_gr_global_buf)b);
        if (ga10b_dma_alloc(ops, sz, &g->global[b]) != GA10B_CHAN_OK) {
            return GA10B_GRCTX_ERR_NOMEM;
        }
        if (b == GA10B_GR_BUF_PRIV_ACCESS_MAP) {
            ga10b_gr_build_priv_access_map((uint8_t *)g->global[b].cpu, g->global[b].size);
        }
        ga10b_dma_clean(ops, &g->global[b]);
    }
    return GA10B_GRCTX_OK;
}

/* ==================================================================
 * Shared by golden and channel contexts
 * ================================================================== */

/* Mapping attributes (nvgpu_gr_global_ctx_init_ctx_buffers_mapping_flags,
 * nvgpu_gr_ctx_init_ctx_buffers_mapping_flags): all ctx mappings are
 * privileged; the global buffers are GPU-cacheable except the priv
 * access map and the RTV CB; gr_ctx, patch and subctx header are not. */
static uint32_t global_map_flags(unsigned b)
{
    uint32_t f = GA10B_MAP_PRIV;
    if (b != GA10B_GR_BUF_PRIV_ACCESS_MAP && b != GA10B_GR_BUF_RTV_CB) {
        f |= GA10B_MAP_CACHEABLE;
    }
    return f;
}

/* Allocate and map gr_ctx / patch / header, map the global buffers,
 * patch the global buffer commits and load the subcontext header
 * (obj_ctx.c:1189-1194 for golden, :1290-1305 for channels). */
static int ctx_prepare(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g,
                       struct ga10b_vm *vm, uint32_t *inst, struct ga10b_gr_chan_ctx *c)
{
    memset(c, 0, sizeof(*c));
    if (ga10b_dma_alloc(ops, GA10B_GR_FECS_HEADER_BYTES, &c->header) != 0 ||
        ga10b_dma_alloc(ops, g->golden_size, &c->gr_ctx) != 0 ||
        ga10b_dma_alloc(ops, ga10b_gr_patch_ctx_bytes(&g->cfg), &c->patch) != 0) {
        return GA10B_GRCTX_ERR_NOMEM;
    }
    if (ga10b_vm_map_dma(ops, vm, &c->header, GA10B_MAP_PRIV, true, &c->header_va) != 0 ||
        ga10b_vm_map_dma(ops, vm, &c->gr_ctx, GA10B_MAP_PRIV, true, &c->gr_ctx_va) != 0 ||
        ga10b_vm_map_dma(ops, vm, &c->patch, GA10B_MAP_PRIV, true, &c->patch_va) != 0) {
        return GA10B_GRCTX_ERR_VM;
    }
    for (unsigned b = 0; b < GA10B_GR_BUF_COUNT; b++) {
        if (!g->global[b].cpu) return GA10B_GRCTX_ERR_STATE;
        if (ga10b_vm_map_dma(ops, vm, &g->global[b], global_map_flags(b), true,
                             &c->global.va[b]) != 0) {
            return GA10B_GRCTX_ERR_VM;
        }
    }

    /* commit_global_ctx_buffers(patch = true): patch_write_begin reads
     * the count from the (zeroed) image, patch_write_end stores it. */
    struct ga10b_ctx_writer w = {
        .ops = ops, .patch = (uint32_t *)c->patch.cpu,
        .patch_words = c->patch.size / 4u,
        .count = ga10b_ctx_rd(c->gr_ctx.cpu, GA10B_CTXSW_PATCH_COUNT),
    };
    ga10b_gr_commit_global_ctx_buffers(&w, &g->cfg, &c->global);
    if (w.overflow) return GA10B_GRCTX_ERR_STATE;
    c->patch_count = w.count;
    c->patch_base = w.count;
    ga10b_ctx_wr(c->gr_ctx.cpu, GA10B_CTXSW_PATCH_COUNT, c->patch_count);

    /* commit_inst: subctx header, then the inst block GR pointer. */
    ga10b_ctx_header_load(c->header.cpu, c->global.va[GA10B_GR_BUF_PRIV_ACCESS_MAP],
                          c->patch_va, c->gr_ctx_va);
    ga10b_inst_set_gr_ptr(inst, c->header_va);
    return GA10B_GRCTX_OK;
}

void ga10b_grctx_chan_free(const struct ga10b_gpu_ops *ops, struct ga10b_gr_chan_ctx *c)
{
    if (!ops || !c) return;
    ga10b_dma_free(ops, &c->patch);
    ga10b_dma_free(ops, &c->gr_ctx);
    ga10b_dma_free(ops, &c->header);
}

/* ==================================================================
 * Golden context image
 * ================================================================== */

/* Generated by scripts/tools/gen-ga10b-reg-table.py from trace lines
 * 18194..18266; do not edit by hand. ga10b_gr_init_fs_state RMWs,
 * SM id numbering (TPC SM_CFG / GPM_PD_SM_ID), PD TPC-per-GPC, ROP
 * mapping, PD skip table, CWD GPC/TPC count and SM id table. */
const struct ga10b_reg_write ga10b_grctx_fs_state_writes[] = {
    /* ga10b_gr_init_fs_state */
    { 0x419bd8u, 0x0000013fu },
    { 0x419ba4u, 0x003d17ebu },
    { 0x400080u, 0x00300bd2u },
    { 0x419c04u, 0x000006f0u },
    /* nvgpu_gr_ctx_patch_write */
    { 0x505e08u, 0x00000000u },
    { 0x500c1cu, 0x00000000u },
    { 0x505e08u, 0x00000000u },
    { 0x500c1cu, 0x00000000u },
    { 0x504e08u, 0x00000001u },
    { 0x500c14u, 0x00000001u },
    { 0x504e08u, 0x00000001u },
    { 0x500c14u, 0x00000001u },
    { 0x505608u, 0x00000002u },
    { 0x500c18u, 0x00000002u },
    { 0x505608u, 0x00000002u },
    { 0x500c18u, 0x00000002u },
    { 0x504608u, 0x00000003u },
    { 0x500c10u, 0x00000003u },
    { 0x504608u, 0x00000003u },
    { 0x500c10u, 0x00000003u },
    /* gm20b_gr_init_pd_tpc_per_gpc */
    { 0x406028u, 0x00000004u },
    { 0x405870u, 0x00000004u },
    { 0x40602cu, 0x00000000u },
    { 0x405874u, 0x00000000u },
    { 0x406030u, 0x00000000u },
    { 0x405878u, 0x00000000u },
    { 0x406034u, 0x00000000u },
    { 0x40587cu, 0x00000000u },
    /* gv11b_gr_init_rop_mapping */
    { 0x418bb8u, 0x00000403u },
    { 0x418b08u, 0x00000000u },
    { 0x41bf00u, 0x00000000u },
    { 0x40780cu, 0x00000000u },
    { 0x418b0cu, 0x00000000u },
    { 0x41bf04u, 0x00000000u },
    { 0x407810u, 0x00000000u },
    { 0x41bfd0u, 0x00000403u },
    { 0x41bfb0u, 0x00000002u },
    { 0x41bfb4u, 0x00000000u },
    { 0x41bfb8u, 0x00000000u },
    { 0x41bfbcu, 0x00000000u },
    { 0x41bfc0u, 0x00000000u },
    { 0x4078bcu, 0x00000403u },
    /* gm20b_gr_init_pd_skip_table_gpc */
    { 0x4064d0u, 0x00000000u },
    { 0x4064d4u, 0x00000000u },
    { 0x4064d8u, 0x00000000u },
    { 0x4064dcu, 0x00000000u },
    { 0x4064e0u, 0x00000000u },
    { 0x4064e4u, 0x00000000u },
    { 0x4064e8u, 0x00000000u },
    { 0x4064ecu, 0x00000000u },
    /* gm20b_gr_init_cwd_gpcs_tpcs_num */
    { 0x405b00u, 0x00000401u },
    /* nvgpu_gr_ctx_patch_write */
    { 0x405b60u, 0x00010203u },
    { 0x405ba0u, 0x00010203u },
    { 0x405ba4u, 0x00000000u },
    { 0x405ba8u, 0x00000000u },
    { 0x405bacu, 0x00000000u },
    { 0x405bb0u, 0x00000000u },
    { 0x405bb4u, 0x00000000u },
    { 0x405bb8u, 0x00000000u },
    { 0x405bbcu, 0x00000000u },
    { 0x405bc0u, 0x00000000u },
    { 0x405bc4u, 0x00000000u },
    { 0x405bc8u, 0x00000000u },
    { 0x405bccu, 0x00000000u },
    { 0x405bd0u, 0x00000000u },
    { 0x405bd4u, 0x00000000u },
    { 0x405bd8u, 0x00000000u },
    { 0x405bdcu, 0x00000000u },
};
const unsigned ga10b_grctx_fs_state_writes_count =
    sizeof(ga10b_grctx_fs_state_writes) / sizeof(ga10b_grctx_fs_state_writes[0]);

/* gm20b_gr_init_fe_pwr_mode_force_on. */
static int fe_pwr_mode(const struct ga10b_gpu_ops *o, bool force_on)
{
    wr(o, GR_FE_PWR_MODE, GR_FE_PWR_MODE_REQ_SEND | (force_on ? GR_FE_PWR_MODE_FORCE_ON : 0u));
    for (unsigned i = 0; i <= 2000u / 10u; i++) {
        if ((rd(o, GR_FE_PWR_MODE) & GR_FE_PWR_MODE_REQ) == 0u) return GA10B_GRCTX_OK;
        o->udelay(o->ctx, 10u);
    }
    return GA10B_GRCTX_ERR_TIMEOUT;
}

/* ga10b_gr_init_override_context_reset (trace 17511-17518). */
static void override_context_reset(const struct ga10b_gpu_ops *o)
{
    wr(o, GR_FECS_CTXSW_RESET_CTL, 0x10u);   /* engine_reset_disabled, context_reset_enabled */
    wr(o, GR_GPCCS_CTXSW_RESET_CTL, 0x20u);  /* gpc_reset_disabled, ctx+zcull reset enabled */
    o->udelay(o->ctx, 10u);
    (void)rd(o, GR_FECS_CTXSW_RESET_CTL);
    (void)rd(o, GR_GPCCS_CTXSW_RESET_CTL);
    wr(o, GR_FECS_CTXSW_RESET_CTL, 0x110u);
    wr(o, GR_GPCCS_CTXSW_RESET_CTL, 0xa20u);
    o->udelay(o->ctx, 10u);
    (void)rd(o, GR_FECS_CTXSW_RESET_CTL);
    (void)rd(o, GR_GPCCS_CTXSW_RESET_CTL);
}

static void rmw(const struct ga10b_gpu_ops *o, uint32_t reg, uint32_t clear, uint32_t set)
{
    wr(o, reg, (rd(o, reg) & ~clear) | set);
}

/* gm20b_gr_init_load_sw_bundle_init. */
static int load_bundles(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g,
                        const struct ga10b_netlist_av_view *v)
{
    uint32_t last = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        if (i == 0u || last != v->l[i].value) {
            wr(o, GR_PIPE_BUNDLE_DATA, v->l[i].value);
            last = v->l[i].value;
        }
        wr(o, GR_PIPE_BUNDLE_ADDRESS, v->l[i].addr);
        if ((v->l[i].addr & 0xffffu) == GR_GO_IDLE_BUNDLE && wait_idle(o, g) != 0) {
            return GA10B_GRCTX_ERR_TIMEOUT;
        }
        if (wait_fe_idle(o, g) != 0) return GA10B_GRCTX_ERR_TIMEOUT;
    }
    return GA10B_GRCTX_OK;
}

/* gv11b_gr_init_load_sw_veid_bundle: non-go-idle bundles go to all 64
 * VEIDs (gr_pipe_bundle_address_veid_f = veid << 20, trace 20384). */
static int load_veid_bundles(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g,
                             const struct ga10b_netlist_av_view *v)
{
    uint32_t last = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        if (i == 0u || last != v->l[i].value) {
            wr(o, GR_PIPE_BUNDLE_DATA, v->l[i].value);
            last = v->l[i].value;
        }
        if ((v->l[i].addr & 0xffffu) == GR_GO_IDLE_BUNDLE) {
            wr(o, GR_PIPE_BUNDLE_ADDRESS, v->l[i].addr);
            if (wait_idle(o, g) != 0) return GA10B_GRCTX_ERR_TIMEOUT;
        } else {
            for (uint32_t j = 0; j < GA10B_MAX_SUBCTX; j++) {
                wr(o, GR_PIPE_BUNDLE_ADDRESS, v->l[i].addr | ((j & 0x3fu) << 20));
                if (wait_fe_idle(o, g) != 0) return GA10B_GRCTX_ERR_TIMEOUT;
            }
        }
    }
    return GA10B_GRCTX_OK;
}

/* tu104_gr_init_load_sw_bundle64 (silicon: wait FE idle after each). */
static int load_bundles64(const struct ga10b_gpu_ops *o, const struct ga10b_grctx *g,
                          const struct ga10b_netlist_av64_view *v)
{
    uint32_t last_lo = 0, last_hi = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        if (i == 0u || last_lo != v->l[i].value_lo || last_hi != v->l[i].value_hi) {
            wr(o, GR_PIPE_BUNDLE_DATA, v->l[i].value_lo);
            wr(o, GR_PIPE_BUNDLE_DATA_HI, v->l[i].value_hi);
            last_lo = v->l[i].value_lo;
            last_hi = v->l[i].value_hi;
        }
        wr(o, GR_PIPE_BUNDLE_ADDRESS, v->l[i].addr);
        int rc = ((v->l[i].addr & 0xffffu) == GR_GO_IDLE_BUNDLE) ? wait_idle(o, g)
                                                                 : wait_fe_idle(o, g);
        if (rc != 0) return GA10B_GRCTX_ERR_TIMEOUT;
    }
    return GA10B_GRCTX_OK;
}

/* gm20b_gr_init_load_method_init. */
static void load_method_init(const struct ga10b_gpu_ops *o, const struct ga10b_netlist_av_view *v)
{
    uint32_t last = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        if (i == 0u || v->l[i].value != last) {
            wr(o, GR_MME_SHADOW_RAM_DATA, v->l[i].value);
            last = v->l[i].value;
        }
        wr(o, GR_MME_SHADOW_RAM_INDEX, GR_MME_SHADOW_WRITE_TRIGGER | v->l[i].addr);
    }
}

/* A region the netlist does not carry is an empty list (nvgpu leaves
 * the list zero-length). */
static int get_av(const struct ga10b_netlist *nl, uint32_t id, struct ga10b_netlist_av_view *v)
{
    int rc = ga10b_netlist_get_av_list(nl, id, v);
    if (rc == GA10B_NETLIST_ERR_NOT_FOUND) {
        v->l = NULL;
        v->count = 0;
        return 0;
    }
    return rc == GA10B_NETLIST_OK ? 0 : GA10B_GRCTX_ERR_NETLIST;
}

/* nvgpu_gr_obj_ctx_alloc_golden_ctx_image, register part. */
static int golden_hw(const struct ga10b_gpu_ops *o, struct ga10b_grctx *g,
                     const struct ga10b_gr_chan_ctx *c, uint64_t inst_phys)
{
    struct ga10b_netlist_aiv_view ctx_load;
    struct ga10b_netlist_av_view bundle, veid_bundle, method;
    struct ga10b_netlist_av64_view bundle64;
    int rc = ga10b_netlist_get_aiv_list(g->netlist, GA10B_NETLIST_REGIONID_SW_CTX_LOAD, &ctx_load);
    if (rc == GA10B_NETLIST_ERR_NOT_FOUND) {
        ctx_load.l = NULL;
        ctx_load.count = 0;
    } else if (rc != GA10B_NETLIST_OK) {
        return GA10B_GRCTX_ERR_NETLIST;
    }
    if (get_av(g->netlist, GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT, &bundle) ||
        get_av(g->netlist, GA10B_NETLIST_REGIONID_SWVEIDBUNDLEINIT, &veid_bundle) ||
        get_av(g->netlist, GA10B_NETLIST_REGIONID_SW_METHOD_INIT, &method)) {
        return GA10B_GRCTX_ERR_NETLIST;
    }
    rc = ga10b_netlist_get_av64_list(g->netlist, GA10B_NETLIST_REGIONID_SW_BUNDLE64_INIT, &bundle64);
    if (rc == GA10B_NETLIST_ERR_NOT_FOUND) {
        bundle64.l = NULL;
        bundle64.count = 0;
    } else if (rc != GA10B_NETLIST_OK) {
        return GA10B_GRCTX_ERR_NETLIST;
    }

    /* Reject a bad SW_CTX_LOAD address before touching the hardware. */
    for (uint32_t i = 0; i < ctx_load.count; i++) {
        if (ctx_load.l[i].addr >= GA10B_BAR0_SIZE || (ctx_load.l[i].addr & 3u)) {
            return GA10B_GRCTX_ERR_NETLIST;
        }
    }

    /* ---- init_hw_state (obj_ctx.c:610) ---- */
    if ((rc = fe_pwr_mode(o, true)) != 0) return rc;
    override_context_reset(o);
    if ((rc = fe_pwr_mode(o, false)) != 0) return rc;

    struct fecs_op bind = { .clr = 0x30u, .addr = GA10B_FECS_METHOD_BIND_POINTER,
                            .data = ga10b_fecs_current_ctx_data(inst_phys),
                            .ok_cond = COND_AND, .ok = 0x10u,
                            .fail_cond = COND_AND, .fail = 0x20u };
    if ((rc = fecs_submit(o, g, &bind, NULL)) != 0) return rc;
    if ((rc = wait_idle(o, g)) != 0) return rc;

    for (uint32_t i = 0; i < ctx_load.count; i++) {
        wr(o, ctx_load.l[i].addr, ctx_load.l[i].value);
    }

    /* ga10b_gr_intr_set_hww_esr_report_mask. */
    uint32_t warp = rd(o, GR_SM0_WARP_ESR_REPORT_MASK);
    uint32_t glob = rd(o, GR_SM0_GLOBAL_ESR_REPORT_MASK);
    wr(o, GR_SMS_HWW_GLOBAL_ESR, 0xffffffffu);
    wr(o, GR_SMS_WARP_ESR_REPORT_MASK, warp | GA10B_SM_WARP_ESR_REPORT);
    wr(o, GR_SMS_GLOBAL_ESR_REPORT_MASK, glob | GA10B_SM_GLOBAL_ESR_REPORT);
    /* gv11b_gr_init_preemption_state: gfxp_wfi_timeout_unit = usec. */
    rmw(o, GR_DEBUG_2, GR_DEBUG_2_GFXP_WFI_UNIT_M, 0u);
    /* nvgpu_cg_blcg_gr_load_enable skipped (clock gating only). */
    if ((rc = wait_idle(o, g)) != 0) return rc;

    /* ---- commit_hw_state (obj_ctx.c:691) ---- */
    wr(o, GR_FE_GO_IDLE_TIMEOUT, 0u);
    wr(o, GR_FE_COMPUTE_GO_IDLE_TIMEOUT, 0u);
    struct ga10b_ctx_writer direct = { .ops = o };
    ga10b_gr_commit_global_ctx_buffers(&direct, &g->cfg, &c->global);
    /* ga10b_gr_init_commit_global_timeslice. */
    uint32_t vaf = rd(o, GR_PE_VAF) | GR_PE_VAF_FAST_MODE_SWITCH;
    uint32_t vsc = rd(o, GR_PES_VSC_VPC) | GR_PES_VSC_VPC_FAST_MODE_SWITCH;
    wr(o, GR_PE_VAF, vaf);
    wr(o, GR_PES_VSC_VPC, vsc);
    rmw(o, GR_PD_AB_DIST_CFG0, 0u, GR_PD_AB_DIST_CFG0_TIMESLICE_EN);
    /* nvgpu_gr_fs_state_init. */
    for (unsigned i = 0; i < ga10b_grctx_fs_state_writes_count; i++) {
        wr(o, ga10b_grctx_fs_state_writes[i].offset, ga10b_grctx_fs_state_writes[i].value);
    }
    if ((rc = wait_idle(o, g)) != 0) goto restore;
    rmw(o, GR_DEBUG_2, GR_DEBUG_2_GFX_AUTO_GO_IDLE | GR_DEBUG_2_COMPUTE_AUTO_GO_IDLE, 0u);

    /* nvgpu_gr_obj_ctx_alloc_sw_bundle. */
    wr(o, GR_PIPE_BUNDLE_CONFIG, GR_PIPE_MODE_OVERRIDE_ENABLED);
    rc = load_bundles(o, g, &bundle);
    if (rc == 0) rc = load_veid_bundles(o, g, &veid_bundle);
    if (rc == 0) rc = load_bundles64(o, g, &bundle64);
    wr(o, GR_PIPE_BUNDLE_CONFIG, 0u);
    if (rc == 0) rc = wait_idle(o, g);
    if (rc != 0) goto restore;

    rmw(o, GR_DEBUG_2, 0u, GR_DEBUG_2_GFX_AUTO_GO_IDLE | GR_DEBUG_2_COMPUTE_AUTO_GO_IDLE);
    wr(o, GR_FE_GO_IDLE_TIMEOUT, GR_FE_GO_IDLE_TIMEOUT_PROD);
    wr(o, GR_FE_COMPUTE_GO_IDLE_TIMEOUT, GR_FE_COMPUTE_GO_IDLE_INIT);
    load_method_init(o, &method);
    return wait_idle(o, g);

restore:
    wr(o, GR_FE_GO_IDLE_TIMEOUT, GR_FE_GO_IDLE_TIMEOUT_PROD);
    wr(o, GR_FE_COMPUTE_GO_IDLE_TIMEOUT, GR_FE_COMPUTE_GO_IDLE_INIT);
    rmw(o, GR_DEBUG_2, 0u, GR_DEBUG_2_GFX_AUTO_GO_IDLE | GR_DEBUG_2_COMPUTE_AUTO_GO_IDLE);
    return rc;
}

int ga10b_grctx_golden_create(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g)
{
    if (!ops || !g || !g->netlist) return GA10B_GRCTX_ERR_ARG;
    if (g->golden_ready) return GA10B_GRCTX_OK;
    int rc = ga10b_grctx_alloc_global(ops, g);
    if (rc != 0) return rc;

    struct ga10b_vm vm;
    struct ga10b_dma inst = { 0 };
    struct ga10b_gr_chan_ctx c;
    bool fecs_bound = false;
    memset(&c, 0, sizeof(c));
    if (ga10b_vm_init(ops, &vm) != GA10B_CHAN_OK) return GA10B_GRCTX_ERR_NOMEM;

    /* nvgpu_gr_golden_ctx_prepare_inst_block: magic, PDB, subctx 0. No
     * RAMFC / method buffer: the golden image uses no host state. */
    if (ga10b_dma_alloc(ops, GA10B_INST_BLOCK_BYTES, &inst) != GA10B_CHAN_OK) {
        rc = GA10B_GRCTX_ERR_NOMEM;
        goto out;
    }
    uint32_t *iw = (uint32_t *)inst.cpu;
    ga10b_inst_set_magic(iw);
    ga10b_inst_init_pdb(iw, vm.pdb.phys);
    ga10b_inst_set_subctxs(iw, 1u, vm.pdb.phys, false);

    rc = ctx_prepare(ops, g, &vm, iw, &c);
    if (rc != 0) goto out;
    ga10b_dma_clean(ops, &inst);
    ga10b_dma_clean(ops, &c.header);
    ga10b_dma_clean(ops, &c.gr_ctx);
    ga10b_dma_clean(ops, &c.patch);
    if (ga10b_vm_tlb_invalidate(ops, &vm) != GA10B_CHAN_OK) {
        rc = GA10B_GRCTX_ERR_TIMEOUT;
        goto out;
    }

    /* From here FECS may hold inst/gr_ctx as its current context
     * (golden_hw issues BIND_POINTER early on). */
    fecs_bound = true;
    rc = golden_hw(ops, g, &c, inst.phys);
    if (rc != 0) goto out;

    /* nvgpu_gr_ctx_init_zcull (support_gfx): zcull mode no_ctxsw, ptr 0.
     * nvgpu writes these through its uncached mapping; SLM-OS writes
     * the cacheable view and cleans it before FECS reads the image. */
    ga10b_ctx_wr(c.gr_ctx.cpu, GA10B_CTXSW_ZCULL, GA10B_CTXSW_ZCULL_MODE_NO_CTXSW);
    ga10b_ctx_wr(c.gr_ctx.cpu, GA10B_CTXSW_ZCULL_PTR, 0u);
    ga10b_ctx_wr(c.gr_ctx.cpu, GA10B_CTXSW_ZCULL_PTR_HI, 0u);
    ops->cache_clean(ops->ctx, c.gr_ctx.cpu, GA10B_CTXSW_HDR_CLEAN_BYTES);

    /* nvgpu_gr_obj_ctx_save_golden_ctx. */
    struct fecs_op save = { .clr = 0x3u, .addr = GA10B_FECS_METHOD_WFI_GOLDEN_SAVE,
                            .data = ga10b_fecs_current_ctx_data(inst.phys),
                            .ok_cond = COND_AND, .ok = 0x1u,
                            .fail_cond = COND_AND, .fail = 0x2u };
    rc = fecs_submit(ops, g, &save, NULL);
    if (rc != 0) goto out;

    /* FECS wrote the image through a VOL (uncached in L2) mapping;
     * flush anyway, then drop the CPU's stale lines before copying. */
    (void)ga10b_gr_l2_flush(ops, true);
    if (ga10b_dma_alloc(ops, g->golden_size, &g->golden) != GA10B_CHAN_OK) {
        rc = GA10B_GRCTX_ERR_NOMEM;
        goto out;
    }
    ops->cache_inval(ops->ctx, c.gr_ctx.cpu, g->golden_size);
    memcpy(g->golden.cpu, c.gr_ctx.cpu, g->golden_size);
    g->golden_ready = true;

    /* capture_gfx_regs (graphics-class defaults) is read-only; skipped. */
    wr(ops, GR_FECS_CURRENT_CTX, 0u);   /* gm20b_gr_falcon_set_current_ctx_invalid */

out:
    if (rc != 0 && fecs_bound) {
        /* FECS may still reference these pages as its current context
         * or be mid-save into them: invalidate its context pointer and
         * leak the buffers rather than hand them back to the PMM, where
         * a late GPU write would land in reused memory. */
        wr(ops, GR_FECS_CURRENT_CTX, 0u);
        return rc;
    }
    ga10b_grctx_chan_free(ops, &c);
    ga10b_dma_free(ops, &inst);
    ga10b_vm_destroy(ops, &vm);
    return rc;
}

/* ==================================================================
 * Per-channel context
 * ================================================================== */

/* The image part of ALLOC_OBJ_CTX + SET_PREEMPTION_MODE, on buffers
 * ctx_prepare set up: golden image, MME timer, compute preemption mode.
 * c->patch_base is where the patch list stands after the global buffer
 * commits; everything after it is written again here, so a reset puts
 * the context back exactly as a fresh allocation leaves it. */
static int chan_load_golden(const struct ga10b_gpu_ops *ops, const struct ga10b_grctx *g,
                            enum ga10b_compute_preempt preempt, struct ga10b_gr_chan_ctx *c)
{
    /* nvgpu_gr_ctx_load_golden_ctx_image. */
    memcpy(c->gr_ctx.cpu, g->golden.cpu, g->golden_size);
    c->patch_count = c->patch_base;
    ga10b_ctx_image_after_golden_load(c->gr_ctx.cpu, c->global.va[GA10B_GR_BUF_PRIV_ACCESS_MAP],
                                      c->patch_count, c->patch_va);

    /* nvgpu_gr_obj_ctx_update_ctxsw_preemption_mode at ALLOC_OBJ_CTX
     * time: default compute mode is WFI, which writes nothing. */

    /* ga10b_gr_init_enable_mme_config_ptimer (NON_FUSA, first alloc). */
    struct ga10b_ctx_writer w = {
        .ops = ops, .patch = (uint32_t *)c->patch.cpu, .patch_words = c->patch.size / 4u,
        .count = ga10b_ctx_rd(c->gr_ctx.cpu, GA10B_CTXSW_PATCH_COUNT),
    };
    ga10b_ctx_write(&w, GR_PRI_MME_CONFIG, rd(ops, GR_PRI_MME_CONFIG) | GR_MME_PTIMER_ENABLE);
    ga10b_ctx_write(&w, GR_PRI_MME_FE1_CONFIG,
                    rd(ops, GR_PRI_MME_FE1_CONFIG) | GR_MME_PTIMER_ENABLE);

    /* SET_PREEMPTION_MODE (nvgpu_gr_setup_set_preemption_mode): write
     * the compute mode into the image and, as for every engine on a
     * non-MIG part, re-patch the CB manager. */
    if (preempt == GA10B_PREEMPT_CTA || preempt == GA10B_PREEMPT_CILP) {
        ga10b_ctx_image_set_compute_preempt(c->gr_ctx.cpu, preempt);
        ga10b_gr_commit_cb_manager(&w, &g->cfg);
    }
    if (w.overflow) return GA10B_GRCTX_ERR_STATE;
    c->patch_count = w.count;
    ga10b_ctx_wr(c->gr_ctx.cpu, GA10B_CTXSW_PATCH_COUNT, c->patch_count);

    ga10b_dma_clean(ops, &c->header);
    ga10b_dma_clean(ops, &c->gr_ctx);
    ga10b_dma_clean(ops, &c->patch);
    return GA10B_GRCTX_OK;
}

int ga10b_grctx_chan_alloc(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *g,
                           struct ga10b_vm *vm, uint32_t *inst_words, uint32_t veid,
                           enum ga10b_compute_preempt preempt,
                           struct ga10b_gr_chan_ctx *c)
{
    if (!ops || !g || !vm || !inst_words || !c || veid >= GA10B_MAX_SUBCTX) {
        return GA10B_GRCTX_ERR_ARG;
    }
    if (!g->golden_ready) return GA10B_GRCTX_ERR_STATE;

    int rc = ctx_prepare(ops, g, vm, inst_words, c);
    if (rc == 0) rc = chan_load_golden(ops, g, preempt, c);
    if (rc != 0) {
        ga10b_grctx_chan_free(ops, c);
        return rc;
    }
    return GA10B_GRCTX_OK;
}

int ga10b_grctx_chan_reset(const struct ga10b_gpu_ops *ops, const struct ga10b_grctx *g,
                           enum ga10b_compute_preempt preempt, struct ga10b_gr_chan_ctx *c)
{
    if (!ops || !g || !c || !c->gr_ctx.cpu || !c->patch.cpu || !c->header.cpu) {
        return GA10B_GRCTX_ERR_ARG;
    }
    if (!g->golden_ready || c->gr_ctx.size < g->golden_size) return GA10B_GRCTX_ERR_STATE;
    /* The subcontext header too: FECS writes it when it saves. */
    ga10b_ctx_header_load(c->header.cpu, c->global.va[GA10B_GR_BUF_PRIV_ACCESS_MAP],
                          c->patch_va, c->gr_ctx_va);
    return chan_load_golden(ops, g, preempt, c);
}
