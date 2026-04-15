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
#define READ_VBIOS_STRUCT_SIZE         24u   /* ver + hdr + u64 addr + size + flags */
#define READ_VBIOS_FLAGS_DEFAULT       2u
#define FRTS_REGION_OFFSET_FROM_CMDBUF 24u
#define FRTS_REGION_TYPE_FB            2u

/* Per-ucode fuse-version registers. GA10x layout:
 *   0x008241C0 + (ucode_id - 1) * 4. We read the register for our
 *   ucode_id=9, then pick the right signature:
 *     idx = sig_versions - fls(reg)  (when reg != 0)
 *     idx = sig_count - 1            (when reg == 0 — no fuse)
 *
 * Nouveau's `ga100_flcn_fw_signature` does exactly this. Without
 * the right index the BROM's RSA3K verify fails silently and
 * CPUCTL stays 0 forever — exactly what we're observing. */
#define NV_FUSE_VERSION_REG_BASE       0x008241C0u

/* FalconUCodeDescV3 fields we need for the sig index calc. */
#define DESC_V3_SIG_COUNT_OFF          39
#define DESC_V3_SIG_VERSIONS_OFF       40

/* MAILBOX0 sentinel — nouveau writes 0xCAFEBEEF when no caller
 * value is provided. Lets post-boot state distinguish "FWSEC ran
 * and wrote the result" from "ucode never executed". */
#define FWSEC_MBOX0_SENTINEL           0xCAFEBEEFu

static inline uint32_t fls32(uint32_t v)
{
    /* find-last-set: bit position of the highest set bit, 1-indexed.
     * 0 → 0, 1 → 1, 2 → 2, 4 → 3, 8 → 4, ... */
    if (v == 0) return 0;
    uint32_t n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

int gsp_bringup_select_sig_index(uint32_t fuse_reg, uint16_t sig_versions,
                                 uint8_t sig_count)
{
    if (sig_count == 0) return -1;

    /* Dev-kit / no-fuse case — match nova-core's fallback to the
     * last available signature. Nouveau errors here; we accept it
     * because retail cards always have non-zero fuse_reg and a
     * dev-kit failing on signature picks the most recent sig. */
    if (fuse_reg == 0)
        return (int)sig_count - 1;

    /* Nouveau ga102 algorithm:
     *   reg_bit = 1 << fls(fuse_reg)
     *   if !(reg_bit & sig_versions): no matching sig → error
     *   idx = 0
     *   while !(reg_bit & sig_versions & 1):
     *       idx += sig_versions & 1
     *       reg_bit >>= 1
     *       sig_versions >>= 1
     *   return idx
     */
    uint32_t reg_bit  = 1u << fls32(fuse_reg);
    uint32_t working  = sig_versions;
    if (!(reg_bit & working)) return -1;

    uint32_t idx = 0;
    while (!(reg_bit & working & 1u)) {
        idx     += working & 1u;
        reg_bit >>= 1;
        working >>= 1;
    }
    if (idx >= sig_count) idx = sig_count - 1;
    return (int)idx;
}

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

/* Public entry point — declared in bringup.h. Logic body below
 * is shared with the static __ test wrapper. */
int gsp_bringup_patch_dmemmapper_frts(uint8_t *dmem, uint32_t dmem_size,
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

        /* cmd_in_buffer_offset is at +0x08; FWSEC reads its entire
         * command block from there. Block layout (nouveau/openrm):
         *   [+0 .. +24)   read_vbios sub-struct — FWSEC ALWAYS
         *                 reads this first; skipping it causes the
         *                 ucode to bail silently before FRTS runs.
         *   [+24 .. +44)  frts_region sub-struct
         */
        uint32_t cmdbuf = rd32le(dmem + dmem_base + DMEMMAPPER_CMD_IN_BUF_OFFSET);
        if (cmdbuf + READ_VBIOS_STRUCT_SIZE + 20 > dmem_size) return -1;

        /* read_vbios: { u32 ver=1; u32 hdr=24; u64 addr=0; u32 size=0; u32 flags=2 } */
        uint8_t *rv = dmem + cmdbuf;
        wr32le(rv +  0, 1);
        wr32le(rv +  4, READ_VBIOS_STRUCT_SIZE);
        wr32le(rv +  8, 0);                          /* addr lo */
        wr32le(rv + 12, 0);                          /* addr hi */
        wr32le(rv + 16, 0);                          /* size */
        wr32le(rv + 20, READ_VBIOS_FLAGS_DEFAULT);

        /* frts_region at cmdbuf + 24: { u32 ver, hdr, addr, size, type }. */
        uint8_t *frts = dmem + cmdbuf + FRTS_REGION_OFFSET_FROM_CMDBUF;
        wr32le(frts +  0, 1);                                /* ver */
        wr32le(frts +  4, 20);                               /* hdr */
        wr32le(frts +  8, (uint32_t)(wpr_addr >> 12));       /* addr */
        wr32le(frts + 12, (uint32_t)(wpr_size >> 12));       /* size */
        wr32le(frts + 16, FRTS_REGION_TYPE_FB);              /* type */
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
     * Signature index selection (nouveau ga100_flcn_fw_signature):
     *   reg = BAR0[0x8241C0 + (ucode_id - 1) * 4]
     *   if reg != 0:  idx = sig_versions - fls(reg)
     *   else:         idx = sig_count - 1     (no fuse burned — default)
     *
     * sig_versions comes from the V3 descriptor's SignatureVersions
     * field; sig_count from SignatureCount. Without the right index,
     * BROM's RSA3K verify fails silently and CPUCTL stays 0 forever
     * — which exactly matches what we observe on the retail GA107. */
    b->last_error_phase = 201;
    const uint32_t sig_size = 384u;

    uint8_t  sig_count    = b->fwsec_desc[DESC_V3_SIG_COUNT_OFF];
    uint16_t sig_versions =
        (uint16_t)b->fwsec_desc[DESC_V3_SIG_VERSIONS_OFF] |
        ((uint16_t)b->fwsec_desc[DESC_V3_SIG_VERSIONS_OFF + 1] << 8);
    uint32_t fuse_reg_off = NV_FUSE_VERSION_REG_BASE
                          + (uint32_t)(b->fwsec_ucode_id - 1) * 4u;
    uint32_t fuse_reg     = gsp_platform->read32(fuse_reg_off);

    b->diag_fuse_reg_off   = fuse_reg_off;
    b->diag_fuse_reg_val   = fuse_reg;
    b->diag_sig_count      = sig_count;
    b->diag_sig_versions   = sig_versions;

    /* Exact algorithm from nouveau ga102_gsp_fwsec_signature:
     *
     *   reg_bit = 1 << (fls(fuse_reg))    // highest burned fuse
     *   if (!(reg_bit & sig_versions))    // no matching signature
     *       fail
     *   idx = 0
     *   while (!(reg_bit & sig_versions & 1)):
     *       idx += sig_versions & 1
     *       reg_bit >>= 1
     *       sig_versions >>= 1
     *   return idx
     *
     * For RTX 3050: fuse_reg=0x3, fls=2, reg_bit=0x4, sig_versions=0xF
     * produces idx=2. The ucode fuse versions it claims to support
     * (bits set in sig_versions) are indexed in order of the
     * register's position — counting how many SUPPORTED fuse
     * versions come BEFORE the currently-burned one. */
    int isig = gsp_bringup_select_sig_index(fuse_reg, sig_versions, sig_count);
    if (isig < 0) goto fail_free;    /* no matching sig — fail closed */
    uint32_t sig_index = (uint32_t)isig;
    b->diag_sig_index = sig_index;

    if (b->fwsec_sigs_size < sig_size * (sig_index + 1)) goto fail_free;
    if (b->fwsec_pkc_data_off + sig_size > b->dma_dmem_size) goto fail_free;

    memcpy((uint8_t *)b->dma_dmem_va + b->fwsec_pkc_data_off,
           b->fwsec_sigs + sig_index * sig_size,
           sig_size);

    /* Patch DMEMMAPPER in the DMA'd DMEM with our FRTS request. */
    b->last_error_phase = 2;
    if (gsp_bringup_patch_dmemmapper_frts(b->dma_dmem_va, b->dma_dmem_size,
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

    /* MAILBOX0 sentinel — nouveau writes 0xCAFEBEEF when caller
     * passes no value. Post-boot state is then diagnosable: 0 =
     * FWSEC ran and cleared the mailbox; 0xCAFEBEEF = ucode never
     * executed. MAILBOX1 stays 0. */
    gsp_platform->write32(NV_PGSP_BASE + FALCON_MAILBOX0, FWSEC_MBOX0_SENTINEL);
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
