/*
 * bringup.c — GSP-RM bringup state machine (E3.4).
 *
 * One-shot sequence: probe Falcons → run FWSEC-FRTS on GSP Falcon →
 * run Booter Load on SEC2 → start GSP RISC-V → wait for GSP_INIT_DONE.
 * Each step is a separate function so the harness can drive them
 * individually for debugging.
 */

#include "bringup.h"
#include "gsp.h"
#include "falcon.h"
#include "nvidia_vbios.h"

#include <string.h>

extern const struct gsp_platform_ops *gsp_platform;

/* ---- WPR2 / FRTS region placement (Ampere, GA107 6 GB) ----
 *
 * Nouveau computes WPR2 FRTS from the VBIOS workspace register
 * (0x625F04). Without a fully-initialized display engine we use
 * nouveau's fallback path: `wpr2.frts.addr = fb_size - 0x120000`,
 * size = 1 MB. For GA107 6 GB that's 0x17FE00000 — confirmed against
 * the reference doc's worked example.
 *
 * Reading the real FB size requires programming a handful of PFB
 * registers; for E3.4 we hardcode 6 GB for this card and leave
 * dynamic discovery as a follow-up (the bare-metal kernel's
 * `nvidia_gpu_init` will fill this in eventually — tracked below). */
#define GA107_FB_SIZE_BYTES         0x180000000ull   /* 6 GB */
#define WPR2_FRTS_SIZE              0x100000ull      /* 1 MB */
#define WPR2_FRTS_BASE_FROM_TOP     0x120000ull      /* offset below FB end */

/* Ampere BAR0 offsets — observable results of FWSEC-FRTS. */
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO 0x001fa824u
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI 0x001fa828u
#define NV_FWSEC_FRTS_ERR           0x00001438u      /* top 16 bits = err code */

/* DMEMMAPPER FWSEC application interface layout (see reference:
 * docs/reference/nouveau-falcon-hs-boot.md).
 *
 * Interface table header at dmem + interface_off (4 bytes):
 *   u8 ver (=1), u8 hdr, u8 len, u8 cnt
 * Then cnt entries, 8 bytes each:
 *   u32 id, u32 dmem_base
 *
 * DMEMMAPPER app data at dmem + app.dmem_base:
 *   u32 signature
 *   u32 cmd_in_buffer_offset     (offset 0x08)
 *   ... (22 bytes reserved)
 *   u32 init_cmd                 (offset 0x2C)
 *
 * FRTS region struct at dmem + cmd_in_buffer_offset + 24:
 *   u32 ver (=1), u32 hdr (=20), u32 addr (>>12), u32 size (>>12), u32 type (=2)
 */
#define DMEMMAPPER_APP_ID              0x00000004u
#define DMEMMAPPER_INIT_CMD_FRTS       0x15u
#define DMEMMAPPER_CMD_IN_BUF_OFFSET   0x08u
#define DMEMMAPPER_INIT_CMD_OFFSET     0x2Cu
#define FRTS_REGION_OFFSET_FROM_CMDBUF 24u
#define FRTS_REGION_TYPE_FB            2u

static inline void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xffu;
    p[1] = (v >> 8) & 0xffu;
    p[2] = (v >> 16) & 0xffu;
    p[3] = (v >> 24) & 0xffu;
}

static inline uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/*
 * Walk the DMEMMAPPER app-interface table in @dmem looking for the
 * entry with id = 0x04. Writes init_cmd = FRTS and the frts_region
 * struct (type=FB, addr/size shifted >>12) at the locations the
 * ucode expects. Returns 0 on success, -1 if the interface table
 * looks malformed or DMEMMAPPER isn't present.
 */
static int patch_dmemmapper_frts(uint8_t *dmem, uint32_t dmem_size,
                                 uint32_t interface_off,
                                 uint64_t wpr_addr, uint64_t wpr_size)
{
    if (interface_off + 4 > dmem_size) return -1;
    uint8_t *itab = dmem + interface_off;
    uint8_t ver  = itab[0];
    uint8_t hdr  = itab[1];
    uint8_t len  = itab[2];
    uint8_t cnt  = itab[3];
    (void)hdr; (void)len;
    if (ver != 1) return -1;

    if (interface_off + 4u + (uint32_t)cnt * 8u > dmem_size) return -1;
    uint8_t *entries = itab + 4;

    for (uint8_t i = 0; i < cnt; i++) {
        uint8_t *e = entries + i * 8u;
        uint32_t id        = rd32le(e);
        uint32_t dmem_base = rd32le(e + 4);
        if (id != DMEMMAPPER_APP_ID) continue;

        /* Found DMEMMAPPER. Check room for the struct we're writing. */
        if (dmem_base + DMEMMAPPER_INIT_CMD_OFFSET + 4 > dmem_size) return -1;

        /* init_cmd first. */
        wr32le(dmem + dmem_base + DMEMMAPPER_INIT_CMD_OFFSET,
               DMEMMAPPER_INIT_CMD_FRTS);

        /* cmd_in_buffer_offset is at +0x08; the buffer itself is
         * at dmem + cmd_in_buffer_offset, and the FRTS region
         * struct goes 24 bytes in (past a read_vbios sub-struct
         * that FWSEC always consults first). */
        uint32_t cmdbuf = rd32le(dmem + dmem_base + DMEMMAPPER_CMD_IN_BUF_OFFSET);
        uint32_t frts   = cmdbuf + FRTS_REGION_OFFSET_FROM_CMDBUF;
        if (frts + 20 > dmem_size) return -1;

        /* struct { u32 ver, hdr, addr, size, type } — 20 bytes. */
        wr32le(dmem + frts +  0, 1);                                /* ver */
        wr32le(dmem + frts +  4, 20);                               /* hdr */
        wr32le(dmem + frts +  8, (uint32_t)(wpr_addr >> 12));       /* addr */
        wr32le(dmem + frts + 12, (uint32_t)(wpr_size >> 12));       /* size */
        wr32le(dmem + frts + 16, FRTS_REGION_TYPE_FB);              /* type */
        return 0;
    }
    return -1;    /* DMEMMAPPER entry not found */
}

int gsp_bringup_prepare(struct gsp_bringup *b)
{
    if (!b) return -1;
    memset(b, 0, sizeof(*b));

    if (falcon_probe(&b->gsp_flcn,  NV_PGSP_BASE)  < 0) return -1;
    if (falcon_probe(&b->sec2_flcn, NV_PSEC2_BASE) < 0) return -1;

    struct nvidia_vbios vb;
    const uint8_t *img = NULL; size_t img_size = 0;
    if (nvidia_vbios_platform_load(&img, &img_size) < 0) return -1;
    if (nvidia_vbios_parse(img, img_size, &vb) < 0) return -1;

    struct nvidia_vbios_fwsec_parts p;
    if (nvidia_vbios_get_fwsec_parts(&vb, &p) < 0) return -1;

    b->fwsec_desc             = p.desc;
    b->fwsec_sigs             = p.sigs;
    b->fwsec_sigs_size        = p.sigs_size;
    b->fwsec_imem             = p.imem;
    b->fwsec_imem_size        = p.imem_size;
    b->fwsec_dmem             = p.dmem;
    b->fwsec_dmem_size        = p.dmem_size;
    b->fwsec_interface_offset = p.interface_off;
    b->fwsec_engine_id        = p.engine_id;
    b->fwsec_ucode_id         = p.ucode_id;
    b->fwsec_pkc_data_off     = p.pkc_data_off;
    b->fwsec_imem_virt_base   = p.imem_virt_base;

    b->wpr2_size = WPR2_FRTS_SIZE;
    b->wpr2_addr = GA107_FB_SIZE_BYTES - WPR2_FRTS_BASE_FROM_TOP;

    b->state = GSP_BRINGUP_INIT;
    return 0;
}

/*
 * Round @n up to a multiple of @align. Align is assumed to be a
 * power of two. */
static inline size_t round_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

int gsp_bringup_fwsec_frts(struct gsp_bringup *b)
{
    if (!b) return -1;
    if (!gsp_platform || !gsp_platform->dma_alloc || !gsp_platform->dma_free)
        return -1;

    b->last_error_phase = 1;

    /* Ampere Falcon DMA is in 256-byte chunks. Round IMEM/DMEM up
     * to chunk boundaries so falcon_dma_upload accepts them. */
    size_t imem_aligned = round_up(b->fwsec_imem_size, FALCON_DMA_CHUNK);
    size_t dmem_aligned = round_up(b->fwsec_dmem_size, FALCON_DMA_CHUNK);

    b->dma_imem_va = gsp_platform->dma_alloc(imem_aligned, 256, &b->dma_imem_iova);
    if (!b->dma_imem_va) return -1;
    b->dma_imem_size = imem_aligned;

    b->dma_dmem_va = gsp_platform->dma_alloc(dmem_aligned, 256, &b->dma_dmem_iova);
    if (!b->dma_dmem_va) {
        gsp_platform->dma_free(b->dma_imem_va, b->dma_imem_size);
        b->dma_imem_va = NULL;
        return -1;
    }
    b->dma_dmem_size = dmem_aligned;

    /* Copy FWSEC IMEM and DMEM into DMA-mapped buffers. Zero-pad
     * the tail of the aligned buffers so the BROM sees zeros past
     * the ucode's end-of-image (not leftover DMA buffer noise). */
    memset(b->dma_imem_va, 0, imem_aligned);
    memcpy(b->dma_imem_va, b->fwsec_imem, b->fwsec_imem_size);

    memset(b->dma_dmem_va, 0, dmem_aligned);
    memcpy(b->dma_dmem_va, b->fwsec_dmem, b->fwsec_dmem_size);

    /* Patch the production signature into DMEM at PKCDataOffset.
     * The BROM reads 384 bytes from DMEM[pkc_data_off] and validates
     * them against the image. Without this the BROM keeps spinning /
     * the ucode never starts executing.
     *
     * Signature index selection — FWSEC ships multiple signatures
     * (one per allowed fuse version). Production cards typically use
     * index 0 (the "current" signature). Multi-fuse selection is a
     * follow-up; for now index 0 works on RTX 3050 retail boards. */
    b->last_error_phase = 201;
    const uint32_t sig_size = 384u;
    const uint32_t sig_index = 0;
    if (b->fwsec_sigs_size < sig_size * (sig_index + 1)) goto fail_free;
    if (b->fwsec_pkc_data_off + sig_size > b->dma_dmem_size) goto fail_free;
    memcpy((uint8_t *)b->dma_dmem_va + b->fwsec_pkc_data_off,
           b->fwsec_sigs + sig_index * sig_size,
           sig_size);

    /* Patch DMEMMAPPER in the DMA'd DMEM with our FRTS request. */
    b->last_error_phase = 2;
    if (patch_dmemmapper_frts(b->dma_dmem_va, b->dma_dmem_size,
                              b->fwsec_interface_offset,
                              b->wpr2_addr, b->wpr2_size) < 0) {
        goto fail_free;
    }

    /* Reset GSP Falcon — kills whatever was running pre-bringup
     * (usually nothing — but SEC2/GSP state from the prior OS is
     * possible, and reset also clears IMEM/DMEM). */
    b->last_error_phase = 3;
    if (falcon_reset(&b->gsp_flcn) < 0) goto fail_free;

    /* On dual-mode GSP Falcon, force Falcon (non-RISC-V) core
     * select. FWSEC is a Falcon ucode — if the engine was last
     * used in RISC-V mode, DMA and STARTCPU go to the wrong core.
     * No-op on SEC2. */
    b->last_error_phase = 301;
    if (falcon_select_falcon_mode(&b->gsp_flcn) < 0) goto fail_free;

    /* Ampere-specific pre-DMA config (nouveau ga102_flcn_fw_load). */
    falcon_pre_dma_setup(&b->gsp_flcn);

    /* DMA IMEM then DMEM into GSP Falcon. */
    b->last_error_phase = 4;
    if (falcon_dma_upload(&b->gsp_flcn, b->dma_imem_iova,
                          0, imem_aligned, true) < 0) goto fail_free;

    b->last_error_phase = 5;
    if (falcon_dma_upload(&b->gsp_flcn, b->dma_dmem_iova,
                          0, dmem_aligned, false) < 0) goto fail_free;

    /* Clear MAILBOX0/1 — nouveau passes mbox0=0 for FWSEC. */
    gsp_platform->write32(NV_PGSP_BASE + FALCON_MAILBOX0, 0);
    gsp_platform->write32(NV_PGSP_BASE + FALCON_MAILBOX1, 0);

    /* Heavy-signed boot. GSP Falcon BROM is at 0x111000 (the RISC-V
     * PRI aperture doubles as the BROM aperture on the dual-mode core). */
    b->last_error_phase = 6;
    if (falcon_hs_boot(&b->gsp_flcn,
                       NV_PGSP_RISCV_BASE,
                       b->fwsec_pkc_data_off,
                       b->fwsec_engine_id,
                       b->fwsec_ucode_id,
                       b->fwsec_imem_virt_base,
                       FALCON_HALT_TIMEOUT_US) < 0) {
        goto fail_free;
    }

    /* Observable outcome: WPR2 registers populated, FRTS err reg clean. */
    b->last_error_phase = 7;
    uint32_t err = gsp_platform->read32(NV_FWSEC_FRTS_ERR);
    uint16_t err_code = (uint16_t)(err >> 16);
    if (err_code != 0) goto fail_free;

    uint32_t wpr_lo = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t wpr_hi = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    if (wpr_lo == 0 && wpr_hi == 0) goto fail_free;

    b->state = GSP_BRINGUP_FWSEC_FRTS_DONE;
    b->last_error_phase = 0;
    return 0;

fail_free:
    if (b->dma_imem_va) gsp_platform->dma_free(b->dma_imem_va, b->dma_imem_size);
    if (b->dma_dmem_va) gsp_platform->dma_free(b->dma_dmem_va, b->dma_dmem_size);
    b->dma_imem_va = NULL;
    b->dma_dmem_va = NULL;
    b->state = GSP_BRINGUP_FAILED;
    return -1;
}

/* E3.4.d + E3.4.e stubs — will land in follow-up commits on this branch. */

int gsp_bringup_booter_load(struct gsp_bringup *b)
{
    if (!b) return -1;
    /* TODO(E3.4.d): DMA booter_load.bin (parsed by nvfw) into SEC2
     * Falcon, program BROM from its meta (engine_id=1, ucode_id=3),
     * set MAILBOX0/1 to WPR meta phys addr, start, poll halt.
     * See docs/reference/nvidia-gsp-bringup-sequence.md §3.1. */
    b->last_error_phase = 100;
    return -1;
}

int gsp_bringup_riscv_start(struct gsp_bringup *b)
{
    if (!b) return -1;
    /* TODO(E3.4.e): reset GSP Falcon, write BCR_CTRL = VALID|RISCV|BRFETCH
     * at 0x111668, set boot vector, release reset, poll RISCV_CPUCTL bit 7.
     * See docs/reference/nvidia-gsp-bringup-sequence.md §4. */
    b->last_error_phase = 200;
    return -1;
}
