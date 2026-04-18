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
#include "gsp_wpr_meta.h"
#include "falcon.h"
#include "nvfw.h"
#include "nv_endian.h"
#include "nvidia_vbios.h"

#include <string.h>

extern const struct gsp_platform_ops *gsp_platform;

/* ---- WPR2 / FRTS region placement (Ampere, GA107 6 GB) ----
 *
 * Nouveau (`tu102_gsp_oneinit` + `tu102_gsp_vga_workspace_addr`)
 * computes the FRTS placement bottom-up from the VBIOS workspace:
 *
 *   vga_workspace.addr = (display ? bios_reg_625f04 : fb_size - 0x100000)
 *   wpr2.frts.size     = 0x100000              (1 MB)
 *   wpr2.frts.addr     = ALIGN_DOWN(vga_workspace.addr, 0x20000)
 *                        - wpr2.frts.size
 *
 * Without a fully-initialized display, vga_workspace.addr collapses
 * to `fb_size - 0x100000`; that's already 1 MB-aligned so the
 * ALIGN_DOWN is a no-op, leaving:
 *
 *   wpr2.frts.addr = fb_size - 0x100000 - 0x100000 = fb_size - 0x200000
 *
 * For GA107 6 GB that's 0x17FE00000 — verified against nouveau on
 * real hardware. An earlier draft of this file (and the reference
 * doc that mirrored it) used 0x120000 as the offset which placed
 * WPR2 at 0x17FEE0000, mid-VGA-workspace; FWSEC rejected the
 * region and busy-looped instead of halting, producing the exact
 * "ucode runs but never halts" symptom that motivated the audit.
 *
 * Reading the real FB size requires programming a handful of PFB
 * registers; for E3.4 we hardcode 6 GB for this card and leave
 * dynamic discovery as a follow-up (the bare-metal kernel's
 * `nvidia_gpu_init` will fill this in eventually — tracked below). */
#define GA107_FB_SIZE_BYTES         0x180000000ull   /* 6 GB */
#define WPR2_FRTS_SIZE              0x100000ull      /* 1 MB */
#define VGA_WORKSPACE_SIZE          0x100000ull      /* 1 MB, no-display fallback */
#define WPR2_FRTS_BASE_FROM_TOP     (VGA_WORKSPACE_SIZE + WPR2_FRTS_SIZE) /* 0x200000 */

/* Pin the GA107 FB+WPR2 layout as overflow-free. The Stage A
 * `gspFwWprEnd = wpr2_addr + wpr2_size` arithmetic in
 * `gsp_wpr_meta_populate_minimum` would wrap on a contrived input
 * but is fine here — fail the build instead of silently relying on
 * the bound when somebody adjusts the constants for a future GPU. */
_Static_assert(GA107_FB_SIZE_BYTES > WPR2_FRTS_BASE_FROM_TOP,
               "GA107_FB_SIZE_BYTES must accommodate WPR2 placement");
_Static_assert((GA107_FB_SIZE_BYTES - WPR2_FRTS_BASE_FROM_TOP) +
               WPR2_FRTS_SIZE > (GA107_FB_SIZE_BYTES - WPR2_FRTS_BASE_FROM_TOP),
               "WPR2 addr + size must not overflow uint64_t");

/* Ampere BAR0 offsets observable after FWSEC-FRTS run — definitions
 * in bringup.h so the harness diagnostic dump uses the same names. */

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
#define DMEMMAPPER_INIT_CMD_SB         0x19u
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

/* Shared LE helpers — bringup.c writes raw ucode payloads via DMA
 * buffers that get memcpy'd into Falcon MMIO; see nv_endian.h for the
 * file-wide LE contract and the _Static_assert that enforces it. */
#define wr32le nv_wr32le
#define rd32le nv_rd32le

/* Generic DMEMMAPPER patcher — takes @init_cmd so callers can probe
 * SB (0x19) or other lifecycle commands alongside the default FRTS
 * (0x15). Declared in bringup.h. Writes frts_region only when
 * init_cmd == FRTS (matches nouveau nvkm_gsp_fwsec_patch). */
int gsp_bringup_patch_dmemmapper(uint8_t *dmem, uint32_t dmem_size,
                                 uint32_t interface_off,
                                 uint32_t init_cmd,
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
        wr32le(dmem + dmem_base + DMEMMAPPER_INIT_CMD_OFFSET, init_cmd);

        /* cmd_in_buffer_offset is at +0x08; FWSEC reads its entire
         * command block from there. Block layout (nouveau/openrm):
         *   [+0 .. +24)   read_vbios sub-struct — FWSEC ALWAYS
         *                 reads this first; skipping it causes the
         *                 ucode to bail silently before FRTS runs.
         *   [+24 .. +44)  frts_region sub-struct (FRTS only)
         */
        uint32_t cmdbuf = rd32le(dmem + dmem_base + DMEMMAPPER_CMD_IN_BUF_OFFSET);
        uint32_t needed = READ_VBIOS_STRUCT_SIZE;
        if (init_cmd == DMEMMAPPER_INIT_CMD_FRTS) needed += 20;
        if (cmdbuf + needed > dmem_size) return -1;

        /* read_vbios: { u32 ver=1; u32 hdr=24; u64 addr=0; u32 size=0; u32 flags=2 } */
        uint8_t *rv = dmem + cmdbuf;
        wr32le(rv +  0, 1);
        wr32le(rv +  4, READ_VBIOS_STRUCT_SIZE);
        wr32le(rv +  8, 0);                          /* addr lo */
        wr32le(rv + 12, 0);                          /* addr hi */
        wr32le(rv + 16, 0);                          /* size */
        wr32le(rv + 20, READ_VBIOS_FLAGS_DEFAULT);

        if (init_cmd == DMEMMAPPER_INIT_CMD_FRTS) {
            /* frts_region at cmdbuf + 24: { u32 ver, hdr, addr, size, type }. */
            uint8_t *frts = dmem + cmdbuf + FRTS_REGION_OFFSET_FROM_CMDBUF;
            wr32le(frts +  0, 1);                                /* ver */
            wr32le(frts +  4, 20);                               /* hdr */
            wr32le(frts +  8, (uint32_t)(wpr_addr >> 12));       /* addr */
            wr32le(frts + 12, (uint32_t)(wpr_size >> 12));       /* size */
            wr32le(frts + 16, FRTS_REGION_TYPE_FB);              /* type */
        }
        return 0;
    }
    return -1;    /* DMEMMAPPER entry not found */
}

/* Legacy wrapper — always requests FRTS. Kept so test_bringup.c can
 * keep exercising the patcher without knowing about the init_cmd
 * parameter. */
int gsp_bringup_patch_dmemmapper_frts(uint8_t *dmem, uint32_t dmem_size,
                                      uint32_t interface_off,
                                      uint64_t wpr_addr, uint64_t wpr_size)
{
    return gsp_bringup_patch_dmemmapper(dmem, dmem_size, interface_off,
                                        DMEMMAPPER_INIT_CMD_FRTS,
                                        wpr_addr, wpr_size);
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

    b->init_cmd  = DMEMMAPPER_INIT_CMD_FRTS;

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

    b->dma_imem_va = gsp_dma_alloc_checked(imem_aligned, 256, &b->dma_imem_iova);
    if (!b->dma_imem_va) return -1;
    b->dma_imem_size = imem_aligned;

    b->dma_dmem_va = gsp_dma_alloc_checked(dmem_aligned, 256, &b->dma_dmem_iova);
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

    /* Patch DMEMMAPPER in the DMA'd DMEM with the requested command.
     * Defaults to FRTS (0x15). Harness can override via b->init_cmd
     * (e.g. --fwsec-sb → 0x19) for bisecting FRTS-specific hangs. */
    b->last_error_phase = 2;
    if (gsp_bringup_patch_dmemmapper(b->dma_dmem_va, b->dma_dmem_size,
                                     b->fwsec_interface_offset,
                                     b->init_cmd,
                                     b->wpr2_addr, b->wpr2_size) < 0) {
        goto fail_free;
    }

    /* Cross-domain coherency: GSP Falcon DMA reads from these buffers
     * via PCIe (x86) or the SoC bus (Jetson). On ARM64 the memcpy
     * + sig patch + DMEMMAPPER patch above only touched CPU cache
     * lines — flush IMEM and DMEM staging buffers to PoC before the
     * Falcon DMA starts. No-op on x86-64 (PCIe DMA is coherent). */
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean(b->dma_imem_va, b->dma_imem_size);
        gsp_platform->cache_clean(b->dma_dmem_va, b->dma_dmem_size);
        gsp_platform->mb();
    }

    /* Reset GSP Falcon — kills whatever was running pre-bringup
     * (usually nothing — but SEC2/GSP state from the prior OS is
     * possible, and reset also clears IMEM/DMEM). */
    /* Reset the Falcon — but only if it isn't already idle. On VFIO
     * hosts, the PCI FLR triggered when userspace opens /dev/vfio/GROUP
     * has already reset the Falcon, and the on-chip BSI (Bootstrap
     * Sequencer) has re-run VBIOS DEVINIT. Writing FALCON_ENGINE.RESET
     * on a post-BSI Falcon causes a PRI bus hang on GA107 (observed
     * 2026-04-15 on test-pc: subsequent BAR0 reads never return). Skip
     * the reset when falcon_is_idle() already reports clean state —
     * bare-metal / non-VFIO callers without an FLR path will still
     * exercise falcon_reset below. */
    b->last_error_phase = 3;
    if (!falcon_is_idle(&b->gsp_flcn)) {
        if (falcon_reset(&b->gsp_flcn) < 0) goto fail_free;
    }

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
     * PRI aperture doubles as the BROM aperture on the dual-mode core).
     *
     * BOOTVEC for FWSEC v3 is 0 — both nouveau (`nvkm_gsp_fwsec_v3`
     * sets `fw->boot_addr = 0`) and nova-core
     * (`FwsecFirmware::boot_addr() -> 0`) hard-code this. The
     * descriptor's IMEMVirtBase field is metadata about where the
     * ucode was BUILT to run; the actual entry PC after BROM verify
     * is the start of IMEM (offset 0). Earlier scaffolding passed
     * IMEMVirtBase here, which started the Falcon at a non-zero PC
     * and produced "ucode runs but never halts" on real hardware. */
    b->last_error_phase = 6;
    if (b->trace_mode) {
        /* Harness sampling mode: kick the BROM + STARTCPU but return
         * immediately so the caller can poll DEBUGINFO / MAILBOX over
         * time. Caller is responsible for gsp_bringup_free(). */
        if (falcon_hs_kick(&b->gsp_flcn,
                           NV_PGSP_RISCV_BASE,
                           b->fwsec_pkc_data_off,
                           b->fwsec_engine_id,
                           b->fwsec_ucode_id,
                           0) < 0) {
            goto fail_free;
        }
        b->last_error_phase = 0;
        return 0;
    }
    if (falcon_hs_boot(&b->gsp_flcn,
                       NV_PGSP_RISCV_BASE,
                       b->fwsec_pkc_data_off,
                       b->fwsec_engine_id,
                       b->fwsec_ucode_id,
                       0,
                       FALCON_HALT_TIMEOUT_US) < 0) {
        goto fail_free;
    }

    /* Observable outcome: FRTS err reg clean, and — for CMD_FRTS —
     * WPR2 registers populated. SB (0x19) halts cleanly but never
     * writes WPR2; skip that post-check in the SB probe. */
    b->last_error_phase = 7;
    uint32_t err = gsp_platform->read32(NV_FWSEC_FRTS_ERR_REG);
    uint16_t err_code = (uint16_t)(err >> 16);
    if (err_code != 0) goto fail_free;

    if (b->init_cmd == DMEMMAPPER_INIT_CMD_FRTS) {
        uint32_t wpr_lo = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        uint32_t wpr_hi = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        if (wpr_lo == 0 && wpr_hi == 0) goto fail_free;
    }

    b->state = GSP_BRINGUP_FWSEC_FRTS_DONE;
    b->last_error_phase = 0;
    return 0;

fail_free:
    gsp_bringup_free(b);
    b->state = GSP_BRINGUP_FAILED;
    return -1;
}

void gsp_bringup_free(struct gsp_bringup *b)
{
    if (!b || !gsp_platform || !gsp_platform->dma_free) return;
    if (b->dma_imem_va) {
        gsp_platform->dma_free(b->dma_imem_va, b->dma_imem_size);
        b->dma_imem_va = NULL;
    }
    if (b->dma_dmem_va) {
        gsp_platform->dma_free(b->dma_dmem_va, b->dma_dmem_size);
        b->dma_dmem_va = NULL;
    }
}

/* ---- E3.4.d: Booter Load on SEC2 Falcon ----
 *
 * Walks booter_load-535.113.01.bin → DMA-mapped mutable copy of the
 * data section → patch signature → PIO upload IMEM/DMEM → BROM
 * program → STARTCPU → halt poll.
 *
 * Reference: docs/reference/nouveau-gsp-tu102.c `tu102_gsp_booter_ctor`
 * + nouveau-falcon-fw.c `nvkm_falcon_fw_boot` + nouveau-falcon-gm200.c
 * `gm200_flcn_fw_load` + `gm200_flcn_fw_boot`. We use PIO (matching
 * nouveau's `gm200_flcn_fw` func table for booter on Ampere) rather
 * than DMA — the booter blob is small (< 100 KB) and PIO avoids the
 * FBIF_TRANSCFG dance the DMA path requires. */

/* WprMeta is a >200-byte struct; we allocate one full page so the
 * address is well-aligned and we have room to populate fields in the
 * E4 RPC step without re-allocating. */
#define WPR_META_BUFFER_SIZE   4096u

/* ---- Stage A radix3 chain constants ----
 *
 * GSP-RM's bootloader uses a 3-level radix tree to map sysmem ELF
 * pages — every level is exactly one 4 KB page of u64 entries. For
 * Stage A we use the single-entry-per-level shape: L0[0] points at
 * L1, L1[0] points at L2, L2[0] points at a dummy 4 KB ELF page.
 * That's enough for booter to walk without faulting; the actual ELF
 * content doesn't matter until E4 plumbs in real GSP-RM. */
#define RADIX3_PAGE_SIZE              4096u
#define RADIX3_DUMMY_ELF_SIZE         4096u

/* Pure fill: write the single L0→L1, L1→L2, L2→ELF entries each
 * page needs. Other entries are left untouched (caller zeros pages
 * before calling). NULL in any page argument is a no-op for the
 * entire call — the chain is only useful end-to-end.
 *
 * Pinned in tests so a future "skip the L2 step" optimization can't
 * break the SEC2 booter handshake silently. */
void gsp_radix3_fill_dummy_chain(uint64_t *l0_page, uint64_t l1_iova,
                                 uint64_t *l1_page, uint64_t l2_iova,
                                 uint64_t *l2_page, uint64_t elf_iova)
{
    if (!l0_page || !l1_page || !l2_page) return;
    l0_page[0] = l1_iova;
    l1_page[0] = l2_iova;
    l2_page[0] = elf_iova;
}

/* Pure fill: populate the bare-minimum WprMeta fields needed for
 * SEC2 booter to perform validation without NULL-deref'ing. Caller
 * provides the radix3 L0 IOVA + WPR2 boundaries from prior bringup
 * state. Other fields are deliberately left zero — Stage A's goal
 * is to surface a *different* failure mode (debuggable MAILBOX0
 * status code) than the current "infinite hang" baseline. */
void gsp_wpr_meta_populate_minimum(GspFwWprMeta *meta,
                                   uint64_t radix3_l0_iova,
                                   uint64_t radix3_elf_size,
                                   uint64_t wpr2_addr,
                                   uint64_t wpr2_size,
                                   uint64_t fb_size)
{
    if (!meta) return;
    memset(meta, 0, sizeof(*meta));
    meta->magic                  = GSP_FW_WPR_META_MAGIC;
    meta->revision               = GSP_FW_WPR_META_REVISION;
    meta->sysmemAddrOfRadix3Elf  = radix3_l0_iova;
    meta->sizeOfRadix3Elf        = radix3_elf_size;
    meta->gspFwWprStart          = wpr2_addr;
    meta->gspFwWprEnd            = wpr2_addr + wpr2_size;
    meta->fbSize                 = fb_size;
}

/* Field-copy from parsed nvfw_image into the booter_* fields of
 * struct gsp_bringup. Extracted so host-side regression tests
 * (test_bringup.c) can pin the assignment logic — particularly the
 * BOOTVEC = apps[0].offset rule, which used to be wrongly set to
 * os_code_offset and was the root cause of the Phase 2 STOPPED
 * failure observed in PR #287's hardware experiment.
 *
 * Caller must have validated that:
 *   - img.os_code_offset + os_code_size  <= img.data_size
 *   - img.os_data_offset + os_data_size  <= img.data_size
 *   - img.apps[0].offset + apps[0].size  <= img.data_size
 *   - img.patch_loc lies inside [os_data_offset, os_data_offset+os_data_size]
 *
 * Field semantics — see falcon.h + the Phase 2 layout comment in
 * gsp_bringup_booter_load for the full v2-vs-v1 layout discussion. */
void gsp_bringup_set_booter_layout(struct gsp_bringup *b,
                                   const struct nvfw_image *img)
{
    /* Defensive — the one in-tree caller
     * (`gsp_bringup_booter_load`) always passes valid pointers, but
     * the helper is extern-visible for tests so guard against a
     * future caller forgetting. */
    if (!b || !img) return;
    b->booter_imem_ns_off   = img->os_code_offset;
    b->booter_imem_ns_size  = img->os_code_size;
    b->booter_imem_sec_off  = img->apps[0].offset;
    b->booter_imem_sec_size = img->apps[0].size;
    b->booter_dmem_offset   = img->os_data_offset;
    b->booter_dmem_size     = img->os_data_size;
    b->booter_dmem_sign     = img->patch_loc - img->os_data_offset;
    b->booter_engine_id     = img->engine_id;
    b->booter_ucode_id      = img->ucode_id;
    /* BOOTVEC is the secure entry point — where the HS-bootrom jumps
     * AFTER signature verification succeeds. For HS-only booter that
     * means apps[0].offset, NOT os_code_offset. The earlier value
     * (os_code_offset = 0 on R535 booter_load) sent the Falcon to
     * IMEM[0] which is the non-secure preamble, not the actual booter
     * entry. Reference: OGKM `kflcnRegWrite(NV_PFALCON_FALCON_BOOTVEC,
     * pUcode->imemVa)` where `imemVa = header.appCodeOffset`
     * (`docs/reference/ogkm-kernel_gsp_falcon_ga102.c:278`,
     * `docs/reference/ogkm-kernel_gsp_booter.c:322`). Same in nouveau
     * v2 `nvkm_falcon_fw_ctor_hs_v2:351`: `fw->boot_addr =
     * lhdr->app[0].offset`. */
    b->booter_boot_addr     = img->apps[0].offset;
}

/* Booter expects MAILBOX0 == 0 on completion in nominal flow. With
 * an incomplete WprMeta (E3.4 milestone), the booter may halt with a
 * non-zero status. We accept "halt within timeout" as the success
 * criterion at this step and surface the mailbox value via diagnostics. */

int gsp_bringup_booter_load(struct gsp_bringup *b)
{
    if (!b) return GSP_ERR_INVAL;
    if (!gsp_platform || !gsp_platform->dma_alloc || !gsp_platform->dma_free
        || !gsp_platform->firmware_get)
        return GSP_ERR_INVAL;
    if (b->state != GSP_BRINGUP_FWSEC_FRTS_DONE) {
        /* Allow re-entry on a partially-failed bringup, but the WPR2
         * registers must be set — booter dereferences WprMeta to find
         * the WPR boundaries SEC2 will fill. */
        return GSP_ERR_INVAL;
    }

    b->last_error_phase = 100;

    /* ---- Phase 1: load + parse booter_load.bin ---- */
    struct gsp_firmware_blob blob;
    gsp_platform->firmware_get(GSP_FW_BOOTER_LOAD, &blob);
    if (!blob.data || blob.size == 0) return GSP_ERR_FAULT;

    struct nvfw_image img;
    if (nvfw_parse(blob.data, blob.size, &img) < 0) return GSP_ERR_FAULT;
    if (img.num_apps == 0) return GSP_ERR_FAULT;

    /* On 0x10DE-magic blobs (R535 mainline) num_sig is itself a file
     * offset to the count — same indirection as patch_loc/patch_sig.
     * On older 0x3B1D14F0 blobs num_sig is the count directly. We
     * treat the value as authoritative when small (< 64); otherwise
     * dereference. */
    uint32_t sig_count = img.num_sig;
    if (img.bin_magic == NVFW_BIN_MAGIC_STD && sig_count >= 64) {
        if (sig_count + 4 > img.size) return GSP_ERR_FAULT;
        sig_count = (uint32_t)img.bytes[sig_count]
                  | ((uint32_t)img.bytes[sig_count + 1] <<  8)
                  | ((uint32_t)img.bytes[sig_count + 2] << 16)
                  | ((uint32_t)img.bytes[sig_count + 3] << 24);
    }
    if (sig_count == 0 || sig_count > 64) return GSP_ERR_FAULT;
    if (img.sig_prod_size == 0) return GSP_ERR_FAULT;
    uint32_t sig_size = img.sig_prod_size / sig_count;
    if (sig_size == 0 || sig_size > 4096) return GSP_ERR_FAULT;

    /* ---- Phase 2: layout the booter image ----
     *
     * R535 booter blobs are HS v2 — each section identifies its own
     * source byte offset within the data section explicitly:
     *
     *   non-secure IMEM source = data_offset + os_code_offset (typically 0;
     *                                                   for HS-only booter
     *                                                   os_code_size == 0
     *                                                   and the upload is
     *                                                   a no-op)
     *   secure IMEM source     = data_offset + apps[0].offset
     *                            (the secure app's source AND target IMEM
     *                            offset — both are app[0].offset by spec)
     *   DMEM source            = data_offset + os_data_offset
     *                            (target DMEM offset = 0)
     *   dmem_sign              = patch_loc - os_data_offset (DMEM byte
     *                            offset where signature gets patched in)
     *
     * Reference: OGKM `kgspExecuteHsFalcon_GA102` (
     * `docs/reference/ogkm-kernel_gsp_falcon_ga102.c:213-275`) and
     * nouveau v2 `nvkm_falcon_fw_ctor_hs_v2` (
     * `docs/reference/nouveau-falcon-fw.c:340-356`).
     *
     * The previous SLM-OS code mirrored the v1 nouveau layout
     * (`fw->nmem_base_img = 0; fw->imem_base_img = lhdr->apps[0]`) which
     * assumes secure code lives immediately after non-secure in the data
     * section. R535 v2 doesn't make that assumption — secure code is
     * wherever apps[0].offset says it is, often NOT contiguous with the
     * non-secure section. Reading from `+ os_code_size` instead of
     * `+ apps[0].offset` loaded HEADER BYTES into IMEM, the HS-bootrom
     * signature check rejected silently, and SEC2 STOPPED at first
     * instruction (CPUCTL=0x20, no MAILBOX0 response — the symptom
     * captured in PR #287's hardware experiment). */
    if (img.os_code_offset + img.os_code_size > img.data_size) return GSP_ERR_FAULT;
    if (img.os_data_offset + img.os_data_size > img.data_size) return GSP_ERR_FAULT;
    if (img.apps[0].offset + img.apps[0].size > img.data_size) return GSP_ERR_FAULT;
    if (img.patch_loc < img.os_data_offset) return GSP_ERR_FAULT;
    if (img.patch_loc + sig_size > img.os_data_offset + img.os_data_size) return GSP_ERR_FAULT;
    if (img.sig_prod_offset + img.sig_prod_size > img.size) return GSP_ERR_FAULT;

    gsp_bringup_set_booter_layout(b, &img);

    /* Track the most-recent failure code through the goto-fail path
     * so callers see _why_ booter setup gave up, not just _that_ it
     * did. last_error_phase still narrows the location. */
    int rc = GSP_OK;

    /* ---- Phase 3: allocate DMA-mapped mutable copy of data section ---- */
    b->last_error_phase = 101;
    b->dma_booter_va = gsp_dma_alloc_checked(img.data_size, 256,
                                              &b->dma_booter_iova);
    if (!b->dma_booter_va) return GSP_ERR_NOMEM;
    b->dma_booter_size = img.data_size;
    memcpy(b->dma_booter_va, img.bytes + img.data_offset, img.data_size);

    /* Patch signature 0 into DMEM at sig location. gm200_flcn_fw_signature
     * always picks idx=0 for production sigs (debug-mode flips to a
     * different sig but we never run in debug). The signature lives at
     * sig_prod_offset + patch_sig within the file — patch_sig is the
     * offset INTO the sig table (not the file). */
    uint32_t sig_prod_at = img.sig_prod_offset + img.patch_sig;
    if (sig_prod_at + sig_size > img.size) { rc = GSP_ERR_FAULT; goto fail; }
    /* Write signature into DMEM portion of our DMA buffer. */
    memcpy((uint8_t *)b->dma_booter_va + img.os_data_offset + b->booter_dmem_sign,
           img.bytes + sig_prod_at, sig_size);

    /* Cross-domain coherency: SEC2 will DMA from this buffer (and
     * Falcon PIO upload reads it back into IMEM/DMEM via writes
     * through the host-side mapping). On ARM64 (Jetson) the memcpy
     * above only updates CPU cache lines — flush to PoC so the GPU
     * reads the patched ucode, not stale DRAM. The mb() pairs the
     * flush with a `dsb sy` so any subsequent MMIO that kicks DMA
     * is ordered after the cache flush completes. No-op on x86-64. */
    gsp_platform->cache_clean(b->dma_booter_va, b->dma_booter_size);
    gsp_platform->mb();

    /* ---- Phase 4: allocate WprMeta DMA buffer ----
     *
     * Booter reads MAILBOX0/1 as a phys addr to GspFwWprMeta. The
     * struct is populated in Phase 4b below — this phase only owns
     * the allocation so a NOMEM failure here is reported separately
     * from a NOMEM in the radix3 alloc. */
    b->last_error_phase = 102;
    b->dma_wpr_meta_va = gsp_dma_alloc_checked(WPR_META_BUFFER_SIZE,
                                                4096,
                                                &b->dma_wpr_meta_iova);
    if (!b->dma_wpr_meta_va) { rc = GSP_ERR_NOMEM; goto fail; }
    b->dma_wpr_meta_size = WPR_META_BUFFER_SIZE;

    /* ---- Phase 4b (Stage A): allocate dummy radix3 chain ----
     *
     * Four DMA-mapped 4 KB pages: L0 → L1 → L2 → dummy ELF. SEC2's
     * HS booter walks `sysmemAddrOfRadix3Elf` as soon as it passes
     * magic/revision validation; without a non-NULL chain it
     * NULL-derefs and hangs forever. The four pages stay around
     * until bringup completes (or fails); fail-path frees them. */
    b->last_error_phase = 103;
    b->dma_radix3_l0_va = gsp_dma_alloc_checked(RADIX3_PAGE_SIZE,
                                                 RADIX3_PAGE_SIZE,
                                                 &b->dma_radix3_l0_iova);
    if (!b->dma_radix3_l0_va) { rc = GSP_ERR_NOMEM; goto fail; }
    b->dma_radix3_l1_va = gsp_dma_alloc_checked(RADIX3_PAGE_SIZE,
                                                 RADIX3_PAGE_SIZE,
                                                 &b->dma_radix3_l1_iova);
    if (!b->dma_radix3_l1_va) { rc = GSP_ERR_NOMEM; goto fail; }
    b->dma_radix3_l2_va = gsp_dma_alloc_checked(RADIX3_PAGE_SIZE,
                                                 RADIX3_PAGE_SIZE,
                                                 &b->dma_radix3_l2_iova);
    if (!b->dma_radix3_l2_va) { rc = GSP_ERR_NOMEM; goto fail; }
    b->dma_radix3_elf_va = gsp_dma_alloc_checked(RADIX3_DUMMY_ELF_SIZE,
                                                  RADIX3_PAGE_SIZE,
                                                  &b->dma_radix3_elf_iova);
    if (!b->dma_radix3_elf_va) { rc = GSP_ERR_NOMEM; goto fail; }
    b->dma_radix3_elf_size = RADIX3_DUMMY_ELF_SIZE;

    /* Zero each DMA-mapped page first so unused entries don't carry
     * stale heap content into SEC2's view. The WprMeta buffer also
     * gets the full-page zero (not just the 256-byte struct's worth
     * inside `gsp_wpr_meta_populate_minimum`) — booter reads exactly
     * the struct today, but a defense-in-depth zero of the rest of
     * the DMA page costs nothing and prevents heap leakage to the
     * GPU's view if a future booter or Stage B change reads more. */
    memset(b->dma_wpr_meta_va,   0, b->dma_wpr_meta_size);
    memset(b->dma_radix3_l0_va,  0, RADIX3_PAGE_SIZE);
    memset(b->dma_radix3_l1_va,  0, RADIX3_PAGE_SIZE);
    memset(b->dma_radix3_l2_va,  0, RADIX3_PAGE_SIZE);
    memset(b->dma_radix3_elf_va, 0, RADIX3_DUMMY_ELF_SIZE);
    gsp_radix3_fill_dummy_chain(b->dma_radix3_l0_va,
                                b->dma_radix3_l1_iova,
                                b->dma_radix3_l1_va,
                                b->dma_radix3_l2_iova,
                                b->dma_radix3_l2_va,
                                b->dma_radix3_elf_iova);

    /* Populate WprMeta with the bare-minimum fields. Booter will
     * accept magic/revision, walk a non-NULL radix3 chain, and
     * (expected) reject the missing bootloader / signature with a
     * specific MAILBOX0 status — the Stage A diagnostic goal. */
    gsp_wpr_meta_populate_minimum(b->dma_wpr_meta_va,
                                  b->dma_radix3_l0_iova,
                                  b->dma_radix3_elf_size,
                                  b->wpr2_addr,
                                  b->wpr2_size,
                                  GA107_FB_SIZE_BYTES);

    /* Cross-domain flush: SEC2 reads all five buffers via DMA. The
     * memcpy/memset/populate above only touched CPU caches; flush
     * everything to PoC before MAILBOX0/1 is set (which kicks SEC2
     * to start consuming). The mb() pairs the flushes with a
     * `dsb sy` so the BAR0 write ordering is preserved. No-op
     * cache_clean on x86-64; mb() is mfence. */
    gsp_platform->cache_clean(b->dma_wpr_meta_va,   b->dma_wpr_meta_size);
    gsp_platform->cache_clean(b->dma_radix3_l0_va,  RADIX3_PAGE_SIZE);
    gsp_platform->cache_clean(b->dma_radix3_l1_va,  RADIX3_PAGE_SIZE);
    gsp_platform->cache_clean(b->dma_radix3_l2_va,  RADIX3_PAGE_SIZE);
    gsp_platform->cache_clean(b->dma_radix3_elf_va, b->dma_radix3_elf_size);
    gsp_platform->mb();

    /* ---- Phase 5: reset SEC2, pre-PIO setup ----
     * Skip reset if already idle — same reasoning as
     * gsp_bringup_fwsec_frts phase 3: on VFIO hosts, vfio-pci's FLR
     * + on-chip BSI DEVINIT recovery leave the Falcon in an
     * idle-but-live state, and writing FALCON_ENGINE.RESET on that
     * state hangs the PRI bus.
     *
     * `last_error_phase` IDs renumbered when Phase 4b was inserted
     * (Stage A): pre-Stage-A used 100..106 sequentially for Phases
     * 1..9; post-Stage-A uses 100..107 with Phase 4b at 103 and
     * everything from Phase 5 onward shifted by +1 to stay
     * monotonic with execution order. */
    b->last_error_phase = 104;
    /* Skip falcon_reset when the Falcon is already idle (post-FLR
     * path on VFIO) or when its control registers are priv-locked
     * (0xbadfXXXX) — writing FALCON_ENGINE.RESET on a priv-locked
     * engine stalls the PRI bus indefinitely on GA107 SEC2. On
     * bare-metal / non-VFIO callers neither flag will be set and
     * falcon_reset runs normally. */
    bool sec2_idle   = falcon_is_idle(&b->sec2_flcn);
    bool sec2_locked = falcon_is_priv_locked(&b->sec2_flcn);
    if (!sec2_idle && !sec2_locked) {
        if (falcon_reset(&b->sec2_flcn) < 0) { rc = GSP_ERR_IO; goto fail; }
    }
    falcon_pre_pio_setup(&b->sec2_flcn);

    /* ---- Phase 6: PIO upload non-secure IMEM, secure IMEM, DMEM ---- */
    b->last_error_phase = 105;
    /* Round all PIO sizes up to 4-byte boundaries (the upload helper
     * requires u32 alignment). The Falcon's IMEM/DMEM is byte-addressed
     * but PIO writes through u32 ports. Trailing bytes past the actual
     * ucode end are treated as scratch by the running ucode. */
    uint32_t ns_round  = (img.os_code_size + 3u) & ~3u;
    uint32_t sec_round = (img.apps[0].size + 3u) & ~3u;
    uint32_t dmem_round = (img.os_data_size + 3u) & ~3u;

    /* Source pointer for each section uses the EXPLICIT field offset
     * from the v2 load header, not an assumed contiguous v1 layout.
     * For HS-only booter, ns_round will be 0 (no non-secure section)
     * and the first call is a no-op; the secure call is the load-bearing
     * one. See the §"Phase 2: layout the booter image" comment above
     * for why this matters. */
    rc = falcon_pio_upload_imem(&b->sec2_flcn,
                                (uint8_t *)b->dma_booter_va
                                    + img.os_code_offset,
                                ns_round,
                                img.os_code_offset,
                                false);
    if (rc < 0) goto fail;

    rc = falcon_pio_upload_imem(&b->sec2_flcn,
                                (uint8_t *)b->dma_booter_va
                                    + img.apps[0].offset,
                                sec_round,
                                img.apps[0].offset,
                                true);
    if (rc < 0) goto fail;

    rc = falcon_pio_upload_dmem(&b->sec2_flcn,
                                (uint8_t *)b->dma_booter_va + img.os_data_offset,
                                dmem_round,
                                0);
    if (rc < 0) goto fail;

    /* ---- Phase 7: program SEC2 BROM ----
     * Order matters: PARAADDR, ENGIDMASK, UCODE_ID, then MOD_SEL last
     * (writing MOD_SEL kicks the BROM to verify everything queued). */
    b->last_error_phase = 106;
    gsp_platform->write32(NV_PSEC2_BROM_BASE + FALCON_BROM_PARAADDR0,
                          b->booter_dmem_sign);
    gsp_platform->write32(NV_PSEC2_BROM_BASE + FALCON_BROM_ENGIDMASK,
                          b->booter_engine_id);
    gsp_platform->write32(NV_PSEC2_BROM_BASE + FALCON_BROM_UCODE_ID,
                          b->booter_ucode_id);
    gsp_platform->mb();
    gsp_platform->write32(NV_PSEC2_BROM_BASE + FALCON_BROM_MOD_SEL,
                          FALCON_BROM_MOD_SEL_RSA3K);
    gsp_platform->mb();

    /* ---- Phase 8: hand booter the WprMeta phys addr via MAILBOX0/1 ---- */
    gsp_platform->write32(NV_PSEC2_BASE + FALCON_MAILBOX0,
                          (uint32_t)(b->dma_wpr_meta_iova & 0xFFFFFFFFu));
    gsp_platform->write32(NV_PSEC2_BASE + FALCON_MAILBOX1,
                          (uint32_t)(b->dma_wpr_meta_iova >> 32));

    /* ---- Phase 9: STARTCPU + halt poll ---- */
    b->last_error_phase = 107;
    falcon_start(&b->sec2_flcn, b->booter_boot_addr);
    if (falcon_wait_halted(&b->sec2_flcn, FALCON_HALT_TIMEOUT_US) < 0) {
        rc = GSP_ERR_TIMEOUT;
        goto fail;
    }

    /* Booter halted. Read MAILBOX0 — caller interprets. */
    b->booter_mbox0_post = gsp_platform->read32(NV_PSEC2_BASE + FALCON_MAILBOX0);

    b->state = GSP_BRINGUP_BOOTER_LOAD_DONE;
    b->last_error_phase = 0;
    return GSP_OK;

fail:
    if (b->dma_booter_va) {
        gsp_platform->dma_free(b->dma_booter_va, b->dma_booter_size);
        b->dma_booter_va = NULL;
    }
    if (b->dma_wpr_meta_va) {
        gsp_platform->dma_free(b->dma_wpr_meta_va, b->dma_wpr_meta_size);
        b->dma_wpr_meta_va = NULL;
    }
    /* Stage A radix3 chain — same fail-path-only free pattern as
     * the booter image and WprMeta buffer. On success path the
     * chain stays live for SEC2 to keep walking. */
    if (b->dma_radix3_l0_va) {
        gsp_platform->dma_free(b->dma_radix3_l0_va, RADIX3_PAGE_SIZE);
        b->dma_radix3_l0_va = NULL;
    }
    if (b->dma_radix3_l1_va) {
        gsp_platform->dma_free(b->dma_radix3_l1_va, RADIX3_PAGE_SIZE);
        b->dma_radix3_l1_va = NULL;
    }
    if (b->dma_radix3_l2_va) {
        gsp_platform->dma_free(b->dma_radix3_l2_va, RADIX3_PAGE_SIZE);
        b->dma_radix3_l2_va = NULL;
    }
    if (b->dma_radix3_elf_va) {
        gsp_platform->dma_free(b->dma_radix3_elf_va, b->dma_radix3_elf_size);
        b->dma_radix3_elf_va = NULL;
    }
    b->state = GSP_BRINGUP_FAILED;
    return rc ? rc : GSP_ERR_IO;
}

/* ---- E3.4.e: GSP RISC-V startup ---- */

/* GSP libos handoff — the RISC-V bootloader picks up its argument
 * struct from MAILBOX0/1. For E3.4.e milestone we hand it the WprMeta
 * address that booter just consumed (matching nouveau's pattern in
 * tu102_gsp_oneinit step 7); the libos arg struct itself is built
 * by E4 and the address re-handed before final RISC-V launch. */

int gsp_bringup_riscv_start(struct gsp_bringup *b)
{
    if (!b) return GSP_ERR_INVAL;
    if (b->state != GSP_BRINGUP_BOOTER_LOAD_DONE) return GSP_ERR_INVAL;
    if (!gsp_platform) return GSP_ERR_INVAL;

    b->last_error_phase = 200;

    /* Reset GSP Falcon — clears pre-existing state and scrubs IMEM/DMEM. */
    if (falcon_reset(&b->gsp_flcn) < 0) return GSP_ERR_IO;

    /* Flip the boot-control register into RISC-V mode.
     * Mask: 0x111 → set VALID | CORE_SELECT(=RISC-V, bit 4) | BRFETCH (bit 8).
     * Nouveau equivalent (ga102_gsp_reset / r535 init):
     *   nvkm_falcon_mask(&gsp->falcon, 0x1668, 0x111, 0x111)
     * which reads register at NV_PGSP_BASE+0x1000+0x668 = 0x111668. */
    b->last_error_phase = 201;
    uint32_t bcr = gsp_platform->read32(NV_PGSP_RISCV_BASE + FALCON_RISCV_BCR_CTRL);
    if (bcr == 0xFFFFFFFFu) return GSP_ERR_IO;
    bcr &= ~(uint32_t)(FALCON_RISCV_BCR_VALID
                       | FALCON_RISCV_BCR_CORE_SELECT
                       | FALCON_RISCV_BCR_BRFETCH);
    bcr |=  FALCON_RISCV_BCR_VALID
         |  FALCON_RISCV_BCR_CORE_SELECT
         |  FALCON_RISCV_BCR_BRFETCH;
    gsp_platform->write32(NV_PGSP_RISCV_BASE + FALCON_RISCV_BCR_CTRL, bcr);
    gsp_platform->mb();

    /* Hand the libos arg pointer to the RISC-V bootloader via the
     * GSP Falcon mailboxes. For E3.4.e we use the WprMeta address
     * we already DMA-allocated; the E4 RPC step replaces this with
     * a proper libos arg struct. */
    gsp_platform->write32(NV_PGSP_BASE + FALCON_MAILBOX0,
                          (uint32_t)(b->dma_wpr_meta_iova & 0xFFFFFFFFu));
    gsp_platform->write32(NV_PGSP_BASE + FALCON_MAILBOX1,
                          (uint32_t)(b->dma_wpr_meta_iova >> 32));

    /* Release reset by writing STARTCPU. The BCR programming above
     * tells the engine to fetch RISC-V boot code rather than the
     * Falcon entry. */
    b->last_error_phase = 202;
    falcon_start(&b->gsp_flcn, 0);

    /* Poll RISCV_CPUCTL.ACTIVE_STAT (bit 7). On success the GSP RISC-V
     * core is running and will eventually post GSP_INIT_DONE on the
     * sysmem msgq (E4 work). We allow up to 2 s — the bootloader
     * does considerable setup before flagging ACTIVE. */
    b->last_error_phase = 203;
    uint32_t budget = 2u * 1000u * 1000u;     /* 2 s in µs */
    /* Reuse falcon's iter heuristic: 10 iters/µs. */
    uint32_t iters = (budget > (1u << 30) / 10u) ? (1u << 30) : budget * 10u;
    while (iters--) {
        uint32_t v = gsp_platform->read32(NV_PGSP_RISCV_BASE + FALCON_RISCV_CPUCTL);
        if (v == 0xFFFFFFFFu) return GSP_ERR_IO;
        if (v & FALCON_RISCV_CPUCTL_ACTIVE) {
            b->gsp_riscv_active = true;
            b->state = GSP_BRINGUP_RISCV_RUNNING;
            b->last_error_phase = 0;
            return GSP_OK;
        }
    }
    return GSP_ERR_TIMEOUT;
}
