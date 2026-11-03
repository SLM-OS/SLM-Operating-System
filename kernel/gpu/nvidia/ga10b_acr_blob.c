/*
 * ga10b_acr_blob.c - GA10B non-WPR LS ucode blob + flcn2_acr_desc
 * (#1088).
 *
 * Line-by-line port of the nvgpu path described in ga10b_acr_blob.h
 * (common/acr/acr_blob_construct.c, L4T R36.4.x). Each step names the
 * nvgpu function it mirrors.
 */

#include "ga10b_acr_blob.h"

#include "string.h"

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "ga10b_acr_blob: interface structs are written in host order (LE only)"
#endif

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static uint32_t align32(uint32_t v, uint32_t a)
{
    return (uint32_t)align_up(v, a);
}

bool ga10b_acr_wpr_info_from_regs(uint32_t wpr1_addr_lo, uint32_t wpr1_addr_hi,
                                  struct ga10b_wpr_info *out)
{
    if (out == nullptr)
        return false;
    out->wpr_base = 0;
    out->size = 0;

    /* fb_mmu_wpr1_addr_{lo,hi}_val_v: bits 31:4 (hw_fb_ga10b.h:306,309). */
    uint32_t lo_val = (wpr1_addr_lo >> 4) & 0x0fffffffu;
    uint32_t hi_val = (wpr1_addr_hi >> 4) & 0x0fffffffu;
    /* alignment_v() = 12 (hw_fb_ga10b.h:307,310). nvgpu forms
     * hi32 = val >> (32 - 12), lo32 = (u32)(val << 12), i.e. val << 12
     * as a 64-bit value (fb_ga10b_fusa.c:190-200, ALIGN_HI32 in
     * fb_ga10b.h:29). */
    uint64_t start = ((uint64_t)(lo_val >> 20) << 32) | (uint64_t)(uint32_t)(lo_val << 12);
    uint64_t end   = ((uint64_t)(hi_val >> 20) << 32) | (uint64_t)(uint32_t)(hi_val << 12);
    if (end < start)
        return false;
    out->wpr_base = start;
    out->size = end - start;  /* fb_ga10b_fusa.c:203 */
    return true;
}

/* nvgpu_acr_lsf_fecs_ucode_details (acr_blob_construct.c:239-267). */
static void fecs_ucode_details(const struct ga10b_ctxsw_ucode_segments *s,
                               struct ga10b_acr_blob_falcon *f)
{
    struct ga10b_ls_falcon_ucode_desc *d = &f->desc;
    const uint32_t A = GA10B_LSF_DATA_SIZE_ALIGNMENT;

    d->bootloader_start_offset = s->boot.offset;
    d->bootloader_size = align32(s->boot.size, A);
    d->bootloader_imem_offset = s->boot_imem_offset;
    d->bootloader_entry_point = s->boot_entry;
    d->image_size = align32(s->boot.size, A) + align32(s->code.size, A) +
                    align32(s->data.size, A);
    d->app_size = align32(s->code.size, A) + align32(s->data.size, A);
    d->app_start_offset = s->code.offset;
    d->app_imem_offset = GA10B_APP_IMEM_OFFSET;
    d->app_imem_entry = GA10B_APP_IMEM_ENTRY;
    d->app_dmem_offset = GA10B_APP_DMEM_OFFSET;
    d->app_resident_code_offset = GA10B_APP_RESIDENT_CODE_OFFSET;
    d->app_resident_code_size = s->code.size;           /* not aligned for FECS */
    d->app_resident_data_offset = s->data.offset - s->code.offset;
    d->app_resident_data_size = s->data.size;           /* not aligned for FECS */
    /* p_img->data = surface CPU VA (offset 0), data_size = image_size
     * (acr_blob_construct.c:264-265). */
    f->src_offset = 0;
    f->data_size = d->image_size;
}

/* nvgpu_acr_lsf_gpccs_ucode_details (acr_blob_construct.c:395-432). */
static void gpccs_ucode_details(const struct ga10b_ctxsw_ucode_segments *s,
                                struct ga10b_acr_blob_falcon *f)
{
    struct ga10b_ls_falcon_ucode_desc *d = &f->desc;
    const uint32_t A = GA10B_LSF_DATA_SIZE_ALIGNMENT;

    d->bootloader_start_offset = GA10B_BL_START_OFFSET;
    d->bootloader_size = align32(s->boot.size, A);
    d->bootloader_imem_offset = s->boot_imem_offset;
    d->bootloader_entry_point = s->boot_entry;
    d->image_size = align32(s->boot.size, A) + align32(s->code.size, A) +
                    align32(s->data.size, A);
    d->app_size = align32(s->code.size, A) + align32(s->data.size, A);
    d->app_start_offset = d->bootloader_size;
    d->app_imem_offset = GA10B_APP_IMEM_OFFSET;
    d->app_imem_entry = GA10B_APP_IMEM_ENTRY;
    d->app_dmem_offset = GA10B_APP_DMEM_OFFSET;
    d->app_resident_code_offset = GA10B_APP_RESIDENT_CODE_OFFSET;
    d->app_resident_code_size = align32(s->code.size, A);
    d->app_resident_data_offset = align32(s->data.offset, A) - align32(s->code.offset, A);
    d->app_resident_data_size = align32(s->data.size, A);
    /* p_img->data = surface + gpccs->boot.offset, data_size =
     * ALIGN(image_size, 256) (acr_blob_construct.c:426-430). */
    f->src_offset = s->boot.offset;
    f->data_size = align32(d->image_size, A);
}

/* bin_version source: lsf_ucode_desc_v2.ls_ucode_version of the
 * wrapper nvgpu built from the sig file (min(size, 2248) bytes into a
 * zeroed buffer). */
static uint32_t sig_ls_ucode_version(const uint8_t *sig, size_t sig_size)
{
    const size_t off = offsetof(struct ga10b_lsf_ucode_desc_wrapper, lsf_ucode_desc_v2) +
                       offsetof(struct ga10b_lsf_ucode_desc_v2, ls_ucode_version);
    uint8_t b[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 4; i++)
        if (off + i < sig_size)
            b[i] = sig[off + i];
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

/* nvgpu_acr_lsf_pmu_ncore_ucode_details (acr_blob_construct.c:91-148,
 * PKC branch): p_img->ndesc = the desc file, p_img->data = the whole
 * image file, data_size = fw_image->size, is_next_core_img = true. */
static void pmu_ncore_ucode_details(const uint8_t *desc_file, size_t image_size,
                                    struct ga10b_acr_blob_falcon *f)
{
    memcpy(&f->ndesc, desc_file, sizeof(f->ndesc));
    f->is_next_core = true;
    f->src_offset = 0;
    f->data_size = (uint32_t)image_size;
}

/* lsfm_add_ucode_img + lsfm_fill_static_lsb_hdr_info_pkc
 * (acr_blob_construct.c:697-733, 607-647). */
static int fill_static_info(struct ga10b_acr_blob_falcon *f, uint32_t falcon_id,
                            bool is_priv_load, bool is_lazy_bootstrap,
                            const uint8_t *sig, size_t sig_size)
{
    const struct ga10b_ls_falcon_ucode_desc *d = &f->desc;
    struct ga10b_lsf_lsb_fields *lsb = &f->lsb;

    f->falcon_id = falcon_id;
    f->wpr_header.falcon_id = falcon_id;
    f->wpr_header.bootstrap_owner = GA10B_FALCON_ID_GSPLITE;  /* acr_sw_ga10b.c:526 */
    f->wpr_header.status = GA10B_LSF_IMAGE_STATUS_COPY;
    /* acr->lsf[id].is_lazy_bootstrap: support_ls_pmu for FECS/GPCCS
     * (acr_sw_ga10b.c:417, 443), false for the PMU (:391). */
    f->wpr_header.lazy_bootstrap = is_lazy_bootstrap ? 1u : 0u;
    f->wpr_header.bin_version = sig_ls_ucode_version(sig, sig_size);

    if (f->is_next_core) {
        /* lsfm_fill_static_lsb_hdr_info, PKC + next-core branch
         * (acr_blob_construct.c:672-689). flags stay 0 (the node is
         * kzalloc'd and this branch never sets them); bl_data_size /
         * bl_data_off are recomputed by lsf_calc_wpr_size_pkc. */
        const struct ga10b_falcon_next_core_ucode_desc *n = &f->ndesc;
        uint64_t ucode_size = (uint64_t)n->bootloader_offset + n->bootloader_size +
                              n->bootloader_param_size;
        uint64_t base_size = ucode_size + n->next_core_elf_size;
        uint64_t pad = align_up(base_size, GA10B_LSF_UCODE_DATA_ALIGNMENT) - base_size;
        uint64_t data_size = (uint64_t)n->next_core_elf_size + pad;
        if (ucode_size > 0xFFFFFFFFull || data_size > 0xFFFFFFFFull)
            return GA10B_ACR_ERR_OVERFLOW;  /* nvgpu_safe_add_u32 would BUG */
        lsb->ucode_size = (uint32_t)ucode_size;
        lsb->data_size = (uint32_t)data_size;
        lsb->bl_code_size = 0;
        lsb->bl_imem_off = 0;
        lsb->flags = 0;
        (void)is_priv_load;  /* is_priv_load is false for the PMU (:392) */
        return GA10B_ACR_OK;
    }

    lsb->bl_code_size = align32(d->bootloader_size, GA10B_LSF_BL_CODE_SIZE_ALIGNMENT);
    uint64_t full_app_size = align_up(d->app_size, GA10B_LSF_BL_CODE_SIZE_ALIGNMENT) +
                             lsb->bl_code_size;
    uint64_t ucode_size = align_up(d->app_resident_data_offset,
                                   GA10B_LSF_BL_CODE_SIZE_ALIGNMENT) + lsb->bl_code_size;
    if (full_app_size < ucode_size || full_app_size > 0xFFFFFFFFull)
        return GA10B_ACR_ERR_OVERFLOW;  /* nvgpu_safe_sub_u32 would BUG */
    lsb->ucode_size = (uint32_t)ucode_size;
    lsb->data_size = (uint32_t)(full_app_size - ucode_size);
    lsb->bl_imem_off = d->bootloader_imem_offset;
    lsb->flags = GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_FALSE;
    if (is_priv_load)
        lsb->flags |= GA10B_NV_FLCN_ACR_LSF_FLAG_FORCE_PRIV_LOAD_TRUE;
    return GA10B_ACR_OK;
}

/* lsf_calc_wpr_size_pkc (acr_blob_construct.c:919-962). */
static void calc_wpr_size_pkc(struct ga10b_acr_blob_falcon *f, uint64_t *wpr_off)
{
    uint64_t off = *wpr_off;
    struct ga10b_lsf_lsb_fields *lsb = &f->lsb;

    off = align_up(off, GA10B_LSF_LSB_HEADER_ALIGNMENT);
    f->wpr_header.lsb_offset = (uint32_t)off;
    off += sizeof(struct ga10b_lsf_lsb_header_v1);

    off = align_up(off, GA10B_LSF_UCODE_DATA_ALIGNMENT);
    lsb->ucode_off = (uint32_t)off;
    off += f->data_size;

    lsb->bl_data_size = align32((uint32_t)sizeof(struct ga10b_flcn_bl_dmem_desc),
                                GA10B_LSF_BL_DATA_SIZE_ALIGNMENT);
    off = align_up(off, GA10B_LSF_BL_DATA_ALIGNMENT);
    lsb->bl_data_off = (uint32_t)off;
    off += lsb->bl_data_size;

    f->full_ucode_size = (uint32_t)(off - lsb->ucode_off);
    /* Non-PMU falcons only (acr_blob_construct.c:951-960); the
     * PMU_NEXT_CORE app_* fields stay 0. */
    if (!f->is_next_core) {
        lsb->app_code_off = lsb->bl_code_size;
        lsb->app_code_size = lsb->ucode_size - lsb->bl_code_size;
        lsb->app_data_off = lsb->ucode_size;
        lsb->app_data_size = lsb->data_size;
    } else {
        /* lsf_gen_wpr_requirements (acr_blob_construct.c:1014-1023):
         * NVRISCV image has no code/data split, reserve WPR space. */
        off += GA10B_PMU_NVRISCV_WPR_RSVD_BYTES;
    }

    *wpr_off = off;
}

/* lsfm_populate_flcn_bl_dmem_desc (acr_blob_construct.c:1051-1126). */
static void populate_bl_dmem_desc(struct ga10b_acr_blob_falcon *f, uint64_t wpr_base)
{
    const struct ga10b_ls_falcon_ucode_desc *d = &f->desc;
    struct ga10b_flcn_bl_dmem_desc *bl = &f->bl_desc;

    uint64_t addr_base = (uint64_t)f->lsb.ucode_off + wpr_base;
    uint64_t addr_code = addr_base + d->app_start_offset;
    uint64_t addr_data = addr_code + d->app_resident_data_offset;

    memset(bl, 0, sizeof(*bl));
    bl->ctx_dma = 0;  /* falcon_dma_idx: FECS GK20A_PMU_DMAIDX_UCODE=0, GPCCS unset=0 */
    bl->code_dma_base.lo = (uint32_t)addr_code;
    bl->code_dma_base.hi = (uint32_t)(addr_code >> 32);
    bl->non_sec_code_off = d->app_resident_code_offset;
    bl->non_sec_code_size = d->app_resident_code_size;
    bl->data_dma_base.lo = (uint32_t)addr_data;
    bl->data_dma_base.hi = (uint32_t)(addr_data >> 32);
    bl->data_size = d->app_resident_data_size;
    bl->code_entry_point = d->app_imem_entry;
    bl->argc = GA10B_UCODE_PARAMS;
    bl->argv = 0;  /* no get_cmd_line_args_offset for FECS/GPCCS */
}

int ga10b_acr_blob_layout(const struct ga10b_acr_blob_inputs *in,
                          struct ga10b_acr_blob_layout *out)
{
    if (out == nullptr)
        return GA10B_ACR_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (in == nullptr || in->surface == nullptr || in->ucode == nullptr ||
        in->fecs_sig == nullptr || in->gpccs_sig == nullptr ||
        in->fecs_sig_size == 0u || in->gpccs_sig_size == 0u)
        return GA10B_ACR_ERR_ARG;
    if (in->include_pmu &&
        (in->pmu_image == nullptr || in->pmu_image_size == 0u ||
         in->pmu_image_size > 0xFFFFFFFFu ||
         in->pmu_desc == nullptr || in->pmu_desc_size < GA10B_NEXT_CORE_UCODE_DESC_SIZE ||
         in->pmu_sig == nullptr || in->pmu_sig_size == 0u))
        return GA10B_ACR_ERR_ARG;

    /* Discovery walks falcon ids upward (FECS=2, GPCCS=3,
     * PMU_NEXT_CORE=13) and prepends, so the managed list is
     * [PMU_NEXT_CORE ->] GPCCS -> FECS. */
    const uint32_t base = in->include_pmu ? 1u : 0u;
    struct ga10b_acr_blob_falcon *pmu = in->include_pmu ? &out->flcn[0] : nullptr;
    struct ga10b_acr_blob_falcon *gpccs = &out->flcn[base];
    struct ga10b_acr_blob_falcon *fecs = &out->flcn[base + 1u];
    const bool lazy = in->include_pmu;  /* g->support_ls_pmu */

    fecs_ucode_details(&in->ucode->fecs, fecs);
    gpccs_ucode_details(&in->ucode->gpccs, gpccs);

    /* nvgpu reads data_size bytes from the surface at src_offset. */
    for (uint32_t i = base; i < base + 2u; i++) {
        const struct ga10b_acr_blob_falcon *f = &out->flcn[i];
        if ((uint64_t)f->src_offset + f->data_size > (uint64_t)in->surface_size) {
            memset(out, 0, sizeof(*out));
            return GA10B_ACR_ERR_SURFACE;
        }
    }

    int rc = fill_static_info(fecs, GA10B_FALCON_ID_FECS, false /* MIG off */, lazy,
                              in->fecs_sig, in->fecs_sig_size);
    if (rc == GA10B_ACR_OK)
        rc = fill_static_info(gpccs, GA10B_FALCON_ID_GPCCS, true, lazy,
                              in->gpccs_sig, in->gpccs_sig_size);
    if (rc == GA10B_ACR_OK && pmu != nullptr) {
        pmu_ncore_ucode_details(in->pmu_desc, in->pmu_image_size, pmu);
        rc = fill_static_info(pmu, GA10B_FALCON_ID_PMU_NEXT_CORE, false, false,
                              in->pmu_sig, in->pmu_sig_size);
    }
    if (rc != GA10B_ACR_OK) {
        memset(out, 0, sizeof(*out));
        return rc;
    }
    out->managed_flcn_cnt = base + 2u;

    /* lsf_gen_wpr_requirements (acr_blob_construct.c:964-1049): WPR
     * header array of (cnt + 1) entries, the extra one the terminator. */
    uint64_t wpr_off = (uint64_t)sizeof(struct ga10b_lsf_wpr_header) *
                       (out->managed_flcn_cnt + 1u);
    for (uint32_t i = 0; i < out->managed_flcn_cnt; i++)
        calc_wpr_size_pkc(&out->flcn[i], &wpr_off);
    if (wpr_off > 0xFFFFFFFFull) {
        memset(out, 0, sizeof(*out));
        return GA10B_ACR_ERR_OVERFLOW;
    }
    out->wpr_size = (uint32_t)wpr_off;
    out->terminator_offset = out->managed_flcn_cnt *
                             (uint32_t)sizeof(struct ga10b_lsf_wpr_header);
    out->wpr_base = in->wpr_base;

    for (uint32_t i = 0; i < out->managed_flcn_cnt; i++)
        if (!out->flcn[i].is_next_core)
            populate_bl_dmem_desc(&out->flcn[i], in->wpr_base);
    return GA10B_ACR_OK;
}

/* lsfm_init_wpr_contents (acr_blob_construct.c:1172-1331). */
int ga10b_acr_blob_build(const struct ga10b_acr_blob_inputs *in,
                         void *buf, size_t buf_size,
                         struct ga10b_acr_blob_layout *out)
{
    int rc = ga10b_acr_blob_layout(in, out);
    if (rc != GA10B_ACR_OK)
        return rc;
    if (buf == nullptr || buf_size < out->wpr_size)
        return GA10B_ACR_ERR_BUF_SMALL;

    uint8_t *b = buf;
    memset(b, 0, out->wpr_size);

    const size_t sig_field = offsetof(struct ga10b_lsf_lsb_header_v1, signature);
    const size_t fid_off = sig_field +
        offsetof(struct ga10b_lsf_ucode_desc_wrapper, lsf_ucode_desc_v2) +
        offsetof(struct ga10b_lsf_ucode_desc_v2, falcon_id);
    const size_t tail_off = offsetof(struct ga10b_lsf_lsb_header_v1, ucode_off);

    for (uint32_t i = 0; i < out->managed_flcn_cnt; i++) {
        const struct ga10b_acr_blob_falcon *f = &out->flcn[i];
        const uint8_t *sig = in->gpccs_sig;
        size_t sig_size = in->gpccs_sig_size;
        const uint8_t *image = in->surface;
        if (f->falcon_id == GA10B_FALCON_ID_FECS) {
            sig = in->fecs_sig;
            sig_size = in->fecs_sig_size;
        } else if (f->falcon_id == GA10B_FALCON_ID_PMU_NEXT_CORE) {
            sig = in->pmu_sig;
            sig_size = in->pmu_sig_size;
            image = in->pmu_image;
        }

        /* WPR header i. */
        memcpy(b + (size_t)i * sizeof(struct ga10b_lsf_wpr_header), &f->wpr_header,
               sizeof(f->wpr_header));

        /* LSB header: signature = wrapper (min(sig_size, 2248) bytes,
         * rest zero) with falcon_id overwritten, then the fields. */
        uint8_t *lsb = b + f->wpr_header.lsb_offset;
        size_t n = sig_size < sizeof(struct ga10b_lsf_ucode_desc_wrapper)
                       ? sig_size : sizeof(struct ga10b_lsf_ucode_desc_wrapper);
        memcpy(lsb + sig_field, sig, n);
        memcpy(lsb + fid_off, &f->falcon_id, sizeof(uint32_t));
        memcpy(lsb + tail_off, &f->lsb, sizeof(f->lsb));

        /* BL gen desc: bl_gen_desc_size = sizeof(flcn_bl_dmem_desc).
         * Not written for a next-core image (acr_blob_construct.c:1270). */
        if (!f->is_next_core)
            memcpy(b + f->lsb.bl_data_off, &f->bl_desc, sizeof(f->bl_desc));

        /* Ucode image. */
        memcpy(b + f->lsb.ucode_off, image + f->src_offset, f->data_size);
    }

    /* Terminator: zeroed header with falcon_id = FALCON_ID_INVALID. */
    struct ga10b_lsf_wpr_header last;
    memset(&last, 0, sizeof(last));
    last.falcon_id = GA10B_FALCON_ID_INVALID;
    memcpy(b + out->terminator_offset, &last, sizeof(last));
    return GA10B_ACR_OK;
}

void ga10b_acr_build_flcn2_desc(struct ga10b_flcn2_acr_desc *d,
                                uint64_t blob_addr, uint32_t blob_size)
{
    if (d == nullptr)
        return;
    memset(d, 0, sizeof(*d));
    d->nonwpr_ucode_blob_start = blob_addr;          /* acr_sw_ga10b.c:307-308 */
    d->nonwpr_ucode_blob_size = blob_size;           /* acr_sw_ga10b.c:312-313 */
    d->regions.no_regions = 1u;                      /* acr_sw_ga10b.c:318 */
    d->wpr_offset = GA10B_ACR_WPR_OFFSET;            /* acr_sw_ga10b.c:322 */
    d->gpu_mode &= ~GA10B_ACR_EMULATE_MODE_MASK;     /* acr_sw_ga10b.c:324-330 (not emulate) */
    d->gpu_mode &= ~GA10B_ACR_MIG_MODE;              /* acr_sw_ga10b.c:332-336 (MIG off) */
    d->gpu_mode &= ~GA10B_ACR_SIMULATION_MODE;       /* acr_sw_ga10b.c:338-342 (not sim) */
    d->ls_pmu_desc = 0;                              /* support_ls_pmu == false */
}

void ga10b_acr_build_flcn2_desc_ls_pmu(struct ga10b_flcn2_acr_desc *d,
                                       uint64_t blob_addr, uint32_t blob_size,
                                       uint64_t ls_pmu_desc_addr)
{
    if (d == nullptr)
        return;
    ga10b_acr_build_flcn2_desc(d, blob_addr, blob_size);
    d->ls_pmu_desc = ls_pmu_desc_addr;               /* acr_sw_ga10b.c:300-301 */
}

int ga10b_acr_copy_ls_pmu_desc(void *dst, size_t dst_size,
                               const uint8_t *desc_file, size_t desc_file_size)
{
    if (dst == nullptr || desc_file == nullptr ||
        dst_size < GA10B_NEXT_CORE_UCODE_DESC_SIZE ||
        desc_file_size < GA10B_NEXT_CORE_UCODE_DESC_SIZE)
        return GA10B_ACR_ERR_ARG;
    memcpy(dst, desc_file, GA10B_NEXT_CORE_UCODE_DESC_SIZE);
    return GA10B_ACR_OK;
}
