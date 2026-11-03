/*
 * test_ga10b_acr_blob.c - Host check of the GA10B netlist / ctxsw
 * surface / ACR LS blob builders against the real Jetson firmware
 * (#1088).
 *
 * Usage: test_ga10b_acr_blob [firmware_dir] [-o blob_out.bin]
 *   firmware_dir defaults to $HOME/jetson-ga10b-firmware.
 *
 * Parses NETA..NETD, builds the ctxsw ucode surface from the image
 * nvgpu uses on silicon (NETC prod), builds the non-WPR blob with the
 * jetson-nano-2 WPR1 registers, cross-checks the blob contents and
 * prints the layout. Exit status is non-zero on any failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ga10b_netlist.h"
#include "ga10b_ctxsw_ucode.h"
#include "ga10b_acr_blob.h"

/* NV_PFB_PRI_MMU_WPR1_ADDR_LO/HI read on jetson-nano-2. */
#define NANO2_WPR1_ADDR_LO  0x027d0000u
#define NANO2_WPR1_ADDR_HI  0x027d7e00u

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

struct file_buf {
    uint8_t *data;
    size_t size;
};

static int load(const char *dir, const char *name, struct file_buf *fb)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* malloc gives >= 8-byte alignment, which the typed views need. */
    fb->data = malloc(n > 0 ? (size_t)n : 1u);
    fb->size = (size_t)n;
    if (fb->data == NULL || fread(fb->data, 1, fb->size, f) != fb->size) {
        fclose(f);
        printf("cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    return 0;
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void dump_netlist(const char *name, const struct file_buf *fb)
{
    struct ga10b_netlist nl;
    int rc = ga10b_netlist_open(&nl, fb->data, fb->size);
    printf("\n== %s (%zu bytes) ==\n", name, fb->size);
    CHECK(rc == GA10B_NETLIST_OK, "open %s rc=%d", name, rc);
    if (rc != GA10B_NETLIST_OK)
        return;
    printf("  version=%u regions=%u\n", nl.version, nl.region_count);
    printf("  %-4s %-32s %10s %10s %10s\n", "id", "name", "offset", "size", "fnv1a");
    for (uint32_t i = 0; i < nl.region_count; i++) {
        struct ga10b_netlist_region r;
        ga10b_netlist_region_at(&nl, i, &r);
        printf("  %-4u %-32s 0x%08x 0x%08x 0x%08x\n", r.region_id,
               ga10b_netlist_region_name(r.region_id), r.data_offset, r.data_size,
               fnv1a(fb->data + r.data_offset, r.data_size));
    }
    uint32_t v;
    if (ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_MAJORV, &v) == 0)
        printf("  MAJORV=0x%08x", v);
    if (ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_NETLIST_NUM, &v) == 0)
        printf("  NETLIST_NUM=%u", v);
    if (ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_BUFFER_SIZE, &v) == 0)
        printf("  BUFFER_SIZE=0x%x", v);
    if (ga10b_netlist_get_u32(&nl, GA10B_NETLIST_REGIONID_CTXSW_REG_BASE_INDEX, &v) == 0)
        printf("  REG_BASE_INDEX=%u", v);
    printf("\n");

    struct ga10b_netlist_av_view av;
    struct ga10b_netlist_av64_view av64;
    struct ga10b_netlist_aiv_view aiv;
    if (ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SW_BUNDLE_INIT, &av) == 0)
        printf("  sw_bundle_init: %u av (first addr=0x%08x val=0x%08x)\n", av.count,
               av.count ? av.l[0].addr : 0u, av.count ? av.l[0].value : 0u);
    if (ga10b_netlist_get_av64_list(&nl, GA10B_NETLIST_REGIONID_SW_BUNDLE64_INIT, &av64) == 0)
        printf("  sw_bundle64_init: %u av64\n", av64.count);
    if (ga10b_netlist_get_aiv_list(&nl, GA10B_NETLIST_REGIONID_SW_CTX_LOAD, &aiv) == 0)
        printf("  sw_ctx_load: %u aiv (first addr=0x%08x idx=%u val=0x%08x)\n", aiv.count,
               aiv.count ? aiv.l[0].addr : 0u, aiv.count ? aiv.l[0].index : 0u,
               aiv.count ? aiv.l[0].value : 0u);
    if (ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SW_NON_CTX_LOAD, &av) == 0)
        printf("  sw_non_ctx_load: %u av\n", av.count);
    if (ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SW_METHOD_INIT, &av) == 0)
        printf("  sw_method_init: %u av\n", av.count);
    if (ga10b_netlist_get_av_list(&nl, GA10B_NETLIST_REGIONID_SWVEIDBUNDLEINIT, &av) == 0)
        printf("  sw_veid_bundle_init: %u av\n", av.count);
}

static void print_segments(const char *name, const struct ga10b_ctxsw_ucode_segments *s)
{
    printf("  %-5s boot_entry=0x%05x boot_imem_offset=0x%05x boot_signature=0x%08x\n",
           name, s->boot_entry, s->boot_imem_offset, s->boot_signature);
    printf("        boot off=0x%06x size=0x%06x | code off=0x%06x size=0x%06x | "
           "data off=0x%06x size=0x%06x\n",
           s->boot.offset, s->boot.size, s->code.offset, s->code.size,
           s->data.offset, s->data.size);
}

static void print_falcon(const struct ga10b_acr_blob_falcon *f)
{
    const struct ga10b_ls_falcon_ucode_desc *d = &f->desc;
    const struct ga10b_lsf_lsb_fields *l = &f->lsb;
    const struct ga10b_flcn_bl_dmem_desc *b = &f->bl_desc;
    printf("  falcon %u (%s)\n", f->falcon_id,
           f->falcon_id == GA10B_FALCON_ID_FECS ? "FECS" : "GPCCS");
    printf("    wpr_header: lsb_offset=0x%x bootstrap_owner=%u lazy=%u bin_version=%u status=%u\n",
           f->wpr_header.lsb_offset, f->wpr_header.bootstrap_owner,
           f->wpr_header.lazy_bootstrap, f->wpr_header.bin_version, f->wpr_header.status);
    printf("    desc: bl_start=0x%x bl_size=0x%x bl_imem=0x%x bl_entry=0x%x image_size=0x%x\n"
           "          app_start=0x%x app_size=0x%x res_code_size=0x%x res_data_off=0x%x "
           "res_data_size=0x%x\n",
           d->bootloader_start_offset, d->bootloader_size, d->bootloader_imem_offset,
           d->bootloader_entry_point, d->image_size, d->app_start_offset, d->app_size,
           d->app_resident_code_size, d->app_resident_data_offset, d->app_resident_data_size);
    printf("    image: surface[0x%x .. 0x%x) -> blob 0x%x\n", f->src_offset,
           f->src_offset + f->data_size, l->ucode_off);
    printf("    lsb: ucode_off=0x%x ucode_size=0x%x data_size=0x%x bl_code_size=0x%x "
           "bl_imem_off=0x%x\n         bl_data_off=0x%x bl_data_size=0x%x app_code_off=0x%x "
           "app_code_size=0x%x app_data_off=0x%x app_data_size=0x%x flags=0x%x\n",
           l->ucode_off, l->ucode_size, l->data_size, l->bl_code_size, l->bl_imem_off,
           l->bl_data_off, l->bl_data_size, l->app_code_off, l->app_code_size,
           l->app_data_off, l->app_data_size, l->flags);
    printf("    bl_dmem_desc: ctx_dma=%u code_dma=0x%08x_%08x non_sec_code off=0x%x size=0x%x "
           "entry=0x%x\n                  data_dma=0x%08x_%08x data_size=0x%x argc=%u argv=%u\n",
           b->ctx_dma, b->code_dma_base.hi, b->code_dma_base.lo, b->non_sec_code_off,
           b->non_sec_code_size, b->code_entry_point, b->data_dma_base.hi,
           b->data_dma_base.lo, b->data_size, b->argc, b->argv);
    printf("    full_ucode_size=0x%x\n", f->full_ucode_size);
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    const char *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else
            dir = argv[i];
    }
    char defdir[1024];
    if (dir == NULL) {
        const char *home = getenv("HOME");
        snprintf(defdir, sizeof(defdir), "%s/jetson-ga10b-firmware", home ? home : ".");
        dir = defdir;
    }
    printf("firmware dir: %s\n", dir);

    static const char *nets[] = {
        "NETA_img_prod_encrypted.bin", "NETB_img_prod_encrypted.bin",
        "NETC_img_prod_encrypted.bin", "NETD_img_prod_encrypted.bin",
    };
    struct file_buf net[4];
    for (int i = 0; i < 4; i++) {
        if (load(dir, nets[i], &net[i]) != 0)
            return 2;
        dump_netlist(nets[i], &net[i]);
    }

    const char *chosen = ga10b_netlist_silicon_image_name(false);
    printf("\nnvgpu silicon choice (prod fuse): %s (NETLIST_FINAL, no MAJORV check: %s)\n",
           chosen, ga10b_netlist_is_valid(GA10B_NETLIST_FINAL, 1u, 2u) ? "valid" : "invalid");
    CHECK(strcmp(chosen, nets[2]) == 0, "expected NETC");

    struct file_buf fecs_fw, gpccs_fw, fecs_sig, gpccs_sig;
    if (load(dir, "fecs_encrypt_prod.bin", &fecs_fw) || load(dir, "gpccs_encrypt_prod.bin", &gpccs_fw) ||
        load(dir, "fecs_pkc_sig_encrypt.bin", &fecs_sig) ||
        load(dir, "gpccs_pkc_sig_encrypt.bin", &gpccs_sig))
        return 2;

    struct ga10b_netlist nl;
    if (ga10b_netlist_open(&nl, net[2].data, net[2].size) != 0)
        return 2;
    struct ga10b_ctxsw_ucode_inputs ui = {0};
    ui.fecs_fw = fecs_fw.data;   ui.fecs_fw_size = fecs_fw.size;
    ui.gpccs_fw = gpccs_fw.data; ui.gpccs_fw_size = gpccs_fw.size;
    CHECK(ga10b_ctxsw_ucode_inputs_from_netlist(&nl, &ui) == 0, "netlist ucode regions");

    struct ga10b_ctxsw_ucode_info info;
    CHECK(ga10b_ctxsw_ucode_layout(&ui, &info) == 0, "layout");
    uint8_t *surface = malloc(info.surface_size);
    CHECK(ga10b_ctxsw_ucode_build(&ui, surface, info.surface_size, &info) == 0, "surface build");
    printf("\n== ctxsw ucode surface (from NETC) ==\n  surface_size=0x%x (%u bytes)\n",
           info.surface_size, info.surface_size);
    print_segments("FECS", &info.fecs);
    print_segments("GPCCS", &info.gpccs);
    CHECK(memcmp(surface + info.fecs.boot.offset, fecs_fw.data + 16, info.fecs.boot.size) == 0,
          "fecs boot bytes");
    CHECK(memcmp(surface + info.gpccs.code.offset, ui.gpccs_inst, ui.gpccs_inst_size) == 0,
          "gpccs code bytes");

    struct ga10b_wpr_info wpr;
    CHECK(ga10b_acr_wpr_info_from_regs(NANO2_WPR1_ADDR_LO, NANO2_WPR1_ADDR_HI, &wpr),
          "wpr regs");
    printf("\n== WPR1 (lo=0x%08x hi=0x%08x) ==\n  wpr_base=0x%llx size=0x%llx (%llu KiB)\n",
           NANO2_WPR1_ADDR_LO, NANO2_WPR1_ADDR_HI, (unsigned long long)wpr.wpr_base,
           (unsigned long long)wpr.size, (unsigned long long)(wpr.size / 1024u));
    CHECK(wpr.wpr_base == 0x27d000000ull && wpr.size == 0x7e0000ull, "wpr conversion");

    struct ga10b_acr_blob_inputs bi = {
        .surface = surface, .surface_size = info.surface_size, .ucode = &info,
        .fecs_sig = fecs_sig.data, .fecs_sig_size = fecs_sig.size,
        .gpccs_sig = gpccs_sig.data, .gpccs_sig_size = gpccs_sig.size,
        .wpr_base = wpr.wpr_base,
    };
    struct ga10b_acr_blob_layout lay;
    CHECK(ga10b_acr_blob_layout(&bi, &lay) == 0, "blob layout");
    uint8_t *blob = malloc(lay.wpr_size);
    CHECK(ga10b_acr_blob_build(&bi, blob, lay.wpr_size, &lay) == 0, "blob build");
    CHECK(ga10b_acr_blob_build(&bi, blob, lay.wpr_size - 1u, &lay) == GA10B_ACR_ERR_BUF_SMALL,
          "short buffer");
    ga10b_acr_blob_build(&bi, blob, lay.wpr_size, &lay);

    printf("\n== ACR non-WPR blob ==\n  managed falcons=%u wpr_size=0x%x (%u bytes) "
           "terminator at 0x%x\n", lay.managed_flcn_cnt, lay.wpr_size, lay.wpr_size,
           lay.terminator_offset);
    for (uint32_t i = 0; i < lay.managed_flcn_cnt; i++)
        print_falcon(&lay.flcn[i]);
    CHECK(lay.wpr_size <= wpr.size, "blob fits WPR1");

    /* Cross-check the bytes actually written. */
    for (uint32_t i = 0; i <= lay.managed_flcn_cnt; i++) {
        struct ga10b_lsf_wpr_header h;
        memcpy(&h, blob + i * sizeof(h), sizeof(h));
        printf("  wpr_header[%u]: falcon_id=0x%x lsb_offset=0x%x owner=%u lazy=%u bin=%u status=%u\n",
               i, h.falcon_id, h.lsb_offset, h.bootstrap_owner, h.lazy_bootstrap,
               h.bin_version, h.status);
    }
    struct ga10b_lsf_wpr_header term;
    memcpy(&term, blob + lay.terminator_offset, sizeof(term));
    CHECK(term.falcon_id == GA10B_FALCON_ID_INVALID && term.lsb_offset == 0, "terminator");
    CHECK(lay.flcn[0].falcon_id == GA10B_FALCON_ID_GPCCS &&
          lay.flcn[1].falcon_id == GA10B_FALCON_ID_FECS, "nvgpu list order GPCCS, FECS");
    for (uint32_t i = 0; i < lay.managed_flcn_cnt; i++) {
        const struct ga10b_acr_blob_falcon *f = &lay.flcn[i];
        const struct file_buf *sig = f->falcon_id == GA10B_FALCON_ID_FECS ? &fecs_sig : &gpccs_sig;
        struct ga10b_lsf_lsb_header_v1 lsb;
        memcpy(&lsb, blob + f->wpr_header.lsb_offset, sizeof(lsb));
        CHECK(lsb.signature.lsf_ucode_desc_v2.falcon_id == f->falcon_id, "lsb falcon_id");
        CHECK(memcmp(&lsb.signature, sig->data, 8) == 0, "lsb generic header");
        CHECK(memcmp((uint8_t *)&lsb.signature + 12, sig->data + 12, sig->size - 12) == 0,
              "lsb signature body");
        CHECK(lsb.ucode_off == f->lsb.ucode_off && lsb.flags == f->lsb.flags, "lsb fields");
        CHECK(memcmp(blob + f->lsb.ucode_off, surface + f->src_offset, f->data_size) == 0,
              "ucode image bytes");
        CHECK((f->lsb.ucode_off & 0xfffu) == 0 && (f->wpr_header.lsb_offset & 0xffu) == 0 &&
              (f->lsb.bl_data_off & 0xffu) == 0, "alignments");
        printf("  %s: signature b_prd=%u b_dbg=%u sig_size=%u encrypted=%u ls_ucode_id=%u\n",
               f->falcon_id == GA10B_FALCON_ID_FECS ? "FECS" : "GPCCS",
               lsb.signature.lsf_ucode_desc_v2.b_prd_present,
               lsb.signature.lsf_ucode_desc_v2.b_dbg_present,
               lsb.signature.lsf_ucode_desc_v2.sig_size,
               lsb.signature.lsf_ucode_desc_v2.b_ucode_ls_encrypted,
               lsb.signature.lsf_ucode_desc_v2.ls_ucode_id);
    }

    /* Example flcn2_acr_desc for a blob placed at a made-up address. */
    struct ga10b_flcn2_acr_desc d;
    ga10b_acr_build_flcn2_desc(&d, 0x0000000100000000ull, lay.wpr_size);
    printf("\n== flcn2_acr_desc (blob @ 0x100000000 example) ==\n"
           "  wpr_region_id=%u wpr_offset=%u no_regions=%u blob_size=0x%x blob_start=0x%llx "
           "ls_pmu_desc=0x%llx gpu_mode=0x%x sizeof=%zu\n",
           d.wpr_region_id, d.wpr_offset, d.regions.no_regions, d.nonwpr_ucode_blob_size,
           (unsigned long long)d.nonwpr_ucode_blob_start, (unsigned long long)d.ls_pmu_desc,
           d.gpu_mode, sizeof(d));

    if (out_path != NULL) {
        FILE *f = fopen(out_path, "wb");
        if (f != NULL) {
            fwrite(blob, 1, lay.wpr_size, f);
            fclose(f);
            printf("wrote blob to %s\n", out_path);
        }
    }

    free(blob);
    free(surface);
    for (int i = 0; i < 4; i++)
        free(net[i].data);
    free(fecs_fw.data);
    free(gpccs_fw.data);
    free(fecs_sig.data);
    free(gpccs_sig.data);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
