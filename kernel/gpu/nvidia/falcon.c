/*
 * falcon.c — Falcon v4 primitives implementation (E3).
 *
 * See falcon.h for the interface. All register access goes through
 * the `gsp_platform` vtable so the same code runs under the Linux
 * VFIO harness and bare-metal x86-64 unchanged.
 *
 * Timing: polling loops use a simple spin. The `gsp_platform->mb`
 * barrier is issued between writes that must be observed in order
 * (e.g. DMATRFBASE / DMATRFMOFFS / DMATRFCMD), so register ordering
 * is correct on weakly-ordered memory even when the underlying
 * transport buffers writes.
 */

#include "falcon.h"
#include "gsp.h"

extern const struct gsp_platform_ops *gsp_platform;

/* ---- Register access helpers ---- */

static inline uint32_t flcn_r32(const struct falcon *f, uint32_t off)
{
    return gsp_platform->read32(f->base + off);
}

static inline void flcn_w32(const struct falcon *f, uint32_t off, uint32_t v)
{
    gsp_platform->write32(f->base + off, v);
}

/* RISC-V PRI is at a different base (NV_FALCON2_GSP_BASE = 0x111000)
 * than the Falcon PRI aperture — can't reuse flcn_r32 because that
 * addresses off the engine_base. Caller must check f->has_riscv
 * (only GSP Falcon has a RISC-V PRI aperture). */
static inline uint32_t riscv_r32(const struct falcon *f, uint32_t off)
{
    (void)f;   /* taken for API symmetry; RISC-V base is fixed */
    return gsp_platform->read32(NV_PGSP_RISCV_BASE + off);
}

/* ---- Time helper ----
 *
 * We don't have access to a high-resolution clock in the shared core.
 * Platforms provide one via gsp_platform->mb — use a simple iteration
 * counter as a proxy. Each iteration issues one MMIO read + one
 * barrier, which on x86-64 (uncached BAR0) is O(100ns). Scale the
 * caller-provided microsecond timeout into iteration budget assuming
 * 1 µs per 10 iterations — conservative, works even if MMIO is
 * slower than expected. */
static inline uint32_t us_to_iters(uint32_t us)
{
    /* 10 iters/µs; cap at 2^30 to avoid wrap on absurd inputs. */
    if (us > (1u << 30) / 10) return 1u << 30;
    return us * 10u;
}

/* ---- Public API ---- */

int falcon_probe(struct falcon *f, uint32_t engine_base)
{
    if (!f) return -1;
    f->base = engine_base;
    f->initialized = false;

    /* Sanity: reading CPUCTL on a live engine returns a finite value
     * in the low 16 bits. A firmware misconfiguration or off-die
     * engine reads 0xFFFFFFFF. */
    uint32_t cpuctl = flcn_r32(f, FALCON_CPUCTL);
    if (cpuctl == 0xFFFFFFFFu) return -1;

    uint32_t hwcfg  = flcn_r32(f, FALCON_HWCFG);
    uint32_t hwcfg2 = flcn_r32(f, FALCON_HWCFG2);
    if (hwcfg == 0xFFFFFFFFu || hwcfg2 == 0xFFFFFFFFu) return -1;

    /* IMEM/DMEM size in blocks of 256. */
    uint32_t imem_blocks = hwcfg & FALCON_HWCFG_IMEM_SIZE_MASK;
    uint32_t dmem_blocks =
        (hwcfg & FALCON_HWCFG_DMEM_SIZE_MASK) >> FALCON_HWCFG_DMEM_SIZE_SHIFT;

    if (imem_blocks == 0 || dmem_blocks == 0) return -1;

    f->imem_size = imem_blocks * FALCON_DMA_CHUNK;
    f->dmem_size = dmem_blocks * FALCON_DMA_CHUNK;
    f->has_riscv = (hwcfg2 & FALCON_HWCFG2_RISCV_ENABLE) != 0
                 && engine_base == NV_PGSP_BASE;
    f->initialized = true;
    return 0;
}

int falcon_reset(struct falcon *f)
{
    if (!f || !f->initialized) return -1;

    /* ENGINE.RESET is self-clearing but on Ampere the actual reset
     * takes effect after a short delay. The canonical sequence
     * (nouveau `ga102_flcn_enable`): write RESET, poll HWCFG2.MEM_SCRUBBING
     * until it clears. Scrubbing also clears after any cold boot, so
     * this is a no-op on first call after device reset. */
    flcn_w32(f, FALCON_ENGINE, FALCON_ENGINE_RESET);
    gsp_platform->mb();

    uint32_t budget = us_to_iters(FALCON_SCRUB_TIMEOUT_US);
    while (budget--) {
        uint32_t hwcfg2 = flcn_r32(f, FALCON_HWCFG2);
        if (hwcfg2 == 0xFFFFFFFFu) return -1;    /* engine went away */
        if ((hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) == 0) return 0;
    }
    return -1;
}

int falcon_wait_halted(struct falcon *f, uint32_t timeout_us)
{
    if (!f || !f->initialized) return -1;
    uint32_t budget = us_to_iters(timeout_us);
    while (budget--) {
        uint32_t cpuctl = flcn_r32(f, FALCON_CPUCTL);
        if (cpuctl == 0xFFFFFFFFu) return -1;
        /* 0xbadfXXXX poison = PRI arbiter denied the read. Bail
         * fast rather than spinning the full timeout: we can't
         * observe HALTED from our privilege level. */
        if ((cpuctl & 0xffff0000u) == 0xbadf0000u) return -1;
        if (cpuctl & FALCON_CPUCTL_HALTED) return 0;
    }
    return -1;
}

void falcon_start(struct falcon *f, uint32_t boot_pc)
{
    if (!f || !f->initialized) return;
    flcn_w32(f, FALCON_BOOTVEC, boot_pc);
    gsp_platform->mb();

    /* When BROM has handed control to the engine, CPUCTL.ALIAS_EN
     * (bit 6) is set and STARTCPU writes via 0x100 are gated. The
     * release path is CPUCTL_ALIAS (0x130). nova-core does this
     * check on every start; nouveau gets away with always writing
     * 0x100 because BROM-gated cases happen to clear quickly on the
     * cards it tests, but the contract is "honour ALIAS_EN". */
    uint32_t cpuctl = flcn_r32(f, FALCON_CPUCTL);
    if (cpuctl & FALCON_CPUCTL_ALIAS_EN)
        flcn_w32(f, FALCON_CPUCTL_ALIAS, FALCON_CPUCTL_STARTCPU);
    else
        flcn_w32(f, FALCON_CPUCTL, FALCON_CPUCTL_STARTCPU);
    gsp_platform->mb();
}

int falcon_dma_upload(struct falcon *f, uint64_t src_dma,
                      uint32_t falcon_off, uint32_t len, bool to_imem)
{
    if (!f || !f->initialized) return -1;

    /* Ampere Falcon DMA is fixed 256-byte chunks. Reject misaligned
     * work — the BROM has strict alignment requirements and partial
     * transfers are silently dropped. */
    if ((falcon_off & (FALCON_DMA_CHUNK - 1)) != 0) return -1;
    if ((len & (FALCON_DMA_CHUNK - 1)) != 0) return -1;
    if (len == 0) return 0;

    uint32_t limit = to_imem ? f->imem_size : f->dmem_size;
    if (falcon_off + len > limit) return -1;

    /* DMATRFBASE is the source IOVA shifted right by 8 (granularity
     * is 256 bytes). DMATRFBASE1 holds bits 39:32 (nouveau sets this
     * to 0 for sysmem under 32-bit IOVA; for 40-bit IOVAs it carries
     * the upper 9 bits). */
    uint32_t base_lo = (uint32_t)(src_dma >> 8);
    uint32_t base_hi = (uint32_t)(src_dma >> 40);

    flcn_w32(f, FALCON_DMATRFBASE,  base_lo);
    flcn_w32(f, FALCON_DMATRFBASE1, base_hi);
    gsp_platform->mb();

    /* Transfer loop — one 256-byte chunk per DMATRFCMD write. */
    uint32_t base_cmd = FALCON_DMATRFCMD_WRITE
                      | FALCON_DMATRFCMD_SIZE(FALCON_DMATRFCMD_SIZE_256B)
                      | (to_imem ? FALCON_DMATRFCMD_IMEM : 0);

    for (uint32_t off = 0; off < len; off += FALCON_DMA_CHUNK) {
        flcn_w32(f, FALCON_DMATRFMOFFS, falcon_off + off);
        flcn_w32(f, FALCON_DMATRFFBOFFS, off);
        gsp_platform->mb();
        flcn_w32(f, FALCON_DMATRFCMD, base_cmd);
        gsp_platform->mb();

        /* Poll DMATRFCMD.IDLE to detect completion. The chunk is
         * small (256 bytes over PCIe) — completes in microseconds
         * on uncontended hardware. */
        uint32_t budget = us_to_iters(FALCON_DMA_TIMEOUT_US);
        for (;;) {
            uint32_t cmd = flcn_r32(f, FALCON_DMATRFCMD);
            if (cmd == 0xFFFFFFFFu) return -1;
            if (cmd & FALCON_DMATRFCMD_IDLE) break;
            if (budget-- == 0) return -1;
        }
    }
    return 0;
}

bool falcon_is_priv_locked(const struct falcon *f)
{
    if (!f || !f->initialized) return false;
    uint32_t cpuctl = flcn_r32(f, FALCON_CPUCTL);
    /* NVIDIA's PRI arbiter returns 0xbadfXXXX (always upper 16 bits
     * = 0xbadf) when a read is denied by the register's priv-level
     * mask. The low 16 bits encode the offending PRI address. */
    return (cpuctl & 0xffff0000u) == 0xbadf0000u;
}

bool falcon_is_idle(const struct falcon *f)
{
    if (!f || !f->initialized) return false;

    uint32_t cpuctl  = flcn_r32(f, FALCON_CPUCTL);
    uint32_t dmactl  = flcn_r32(f, FALCON_DMACTL);
    uint32_t trfcmd  = flcn_r32(f, FALCON_DMATRFCMD);
    uint32_t hwcfg2  = flcn_r32(f, FALCON_HWCFG2);

    if (cpuctl == 0xFFFFFFFFu) return false;
    if ((cpuctl & FALCON_CPUCTL_HALTED) == 0) return false;
    if ((trfcmd & FALCON_DMATRFCMD_IDLE) == 0) return false;
    if (hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) return false;
    if (dmactl & (FALCON_DMACTL_IMEM_SCRUBBING | FALCON_DMACTL_DMEM_SCRUBBING))
        return false;
    return true;
}

int falcon_select_falcon_mode(struct falcon *f)
{
    if (!f || !f->initialized) return -1;
    if (!f->has_riscv) return 0;     /* SEC2 etc — no-op */

    /* BCR_CTRL bit 4 = CORE_SELECT. 0 = Falcon, 1 = RISC-V.
     * If RISC-V mode is currently selected, clear the register to
     * force Falcon mode and poll for VALID bit. */
    uint32_t bcr = riscv_r32(f, FALCON_RISCV_BCR_CTRL);
    if ((bcr & FALCON_RISCV_BCR_CORE_SELECT) == 0) return 0;

    gsp_platform->write32(NV_PGSP_RISCV_BASE + FALCON_RISCV_BCR_CTRL, 0);
    gsp_platform->mb();

    uint32_t budget = us_to_iters(10 * 1000);
    while (budget--) {
        uint32_t v = riscv_r32(f, FALCON_RISCV_BCR_CTRL);
        if (v == 0xFFFFFFFFu) return -1;
        if (v & FALCON_RISCV_BCR_VALID) return 0;
    }
    return -1;
}

void falcon_pre_dma_setup(struct falcon *f)
{
    if (!f || !f->initialized) return;

    /* falcon[0x624] |= 0x80 */
    uint32_t v = flcn_r32(f, FALCON_PRE_DMA_624);
    flcn_w32(f, FALCON_PRE_DMA_624, v | FALCON_PRE_DMA_624_BIT);

    /* Clear DMACTL. */
    flcn_w32(f, FALCON_DMACTL, 0);

    /* TRANSCFG mask/value. */
    v = flcn_r32(f, FALCON_PRE_DMA_600);
    v = (v & ~FALCON_PRE_DMA_600_MASK) | FALCON_PRE_DMA_600_VAL;
    flcn_w32(f, FALCON_PRE_DMA_600, v);
    gsp_platform->mb();
}

int falcon_hs_kick(struct falcon *f,
                   uint32_t brom_base,
                   uint32_t dmem_sign_off,
                   uint32_t engine_id,
                   uint32_t ucode_id,
                   uint32_t boot_vec)
{
    if (!f || !f->initialized) return -1;
    if (!falcon_is_idle(f))    return -1;

    /* Program BROM in the exact order nouveau uses. MOD_SEL last —
     * writing it triggers the signature verify, so everything the
     * verify consumes (PARAADDR, UCODE_ID, ENGIDMASK) must already
     * be in place. */
    gsp_platform->write32(brom_base + FALCON_BROM_PARAADDR0, dmem_sign_off);
    gsp_platform->write32(brom_base + FALCON_BROM_ENGIDMASK, engine_id);
    gsp_platform->write32(brom_base + FALCON_BROM_UCODE_ID,  ucode_id);
    gsp_platform->mb();
    gsp_platform->write32(brom_base + FALCON_BROM_MOD_SEL,
                          FALCON_BROM_MOD_SEL_RSA3K);
    gsp_platform->mb();

    falcon_start(f, boot_vec);
    return 0;
}

int falcon_hs_boot(struct falcon *f,
                   uint32_t brom_base,
                   uint32_t dmem_sign_off,
                   uint32_t engine_id,
                   uint32_t ucode_id,
                   uint32_t boot_vec,
                   uint32_t timeout_us)
{
    int rc = falcon_hs_kick(f, brom_base, dmem_sign_off,
                            engine_id, ucode_id, boot_vec);
    if (rc < 0) return rc;
    return falcon_wait_halted(f, timeout_us);
}

/* Suppress "unused function" warnings from some compilers when
 * riscv_r32 isn't called in this TU (future E3 step wires it). */
static inline __attribute__((unused)) uint32_t
falcon_riscv_cpuctl(const struct falcon *f)
{
    if (!f->has_riscv) return 0xFFFFFFFFu;
    return riscv_r32(f, FALCON_RISCV_CPUCTL);
}

void falcon_pre_pio_setup(struct falcon *f)
{
    if (!f || !f->initialized) return;

    /* Mask-set 0x624 bit 7 (same gate the DMA path opens). */
    uint32_t v = flcn_r32(f, FALCON_PRE_DMA_624);
    flcn_w32(f, FALCON_PRE_DMA_624, v | FALCON_PRE_DMA_624_BIT);

    /* Clear DMACTL — REQUIRE_CTX off, scrub bits are RO. */
    flcn_w32(f, FALCON_DMACTL, 0);
    gsp_platform->mb();
}

/* IMEMC bits — see dev_falcon_v4.h. */
#define FALCON_IMEMC_AINCW        (1u << 24)
#define FALCON_IMEMC_SECURE       (1u << 28)
/* DMEMC bits. */
#define FALCON_DMEMC_AINCW        (1u << 24)

int falcon_pio_upload_imem(struct falcon *f, const uint8_t *src,
                           uint32_t len, uint32_t falcon_off, bool is_secure)
{
    if (!f || !f->initialized || !src) return GSP_ERR_INVAL;
    if ((falcon_off & 3u) != 0 || (len & 3u) != 0) return GSP_ERR_INVAL;
    if (falcon_off + len > f->imem_size) return GSP_ERR_INVAL;
    if (len == 0) return GSP_OK;

    /* IMEMC port 0: AINCW=1 (auto-increment write addr), set OFFS+BLK
     * to falcon_off. SECURE bit enters secure IMEM aperture. */
    uint32_t ctrl = FALCON_IMEMC_AINCW | (falcon_off & 0x00FFFFFFu);
    if (is_secure) ctrl |= FALCON_IMEMC_SECURE;
    flcn_w32(f, FALCON_IMEMC(0), ctrl);

    /* IMEMT — instruction-page tag (PC>>8). Falcon uses this to
     * map IMEM blocks to virtual instruction addresses; for PIO
     * upload of a flat ucode at falcon_off, the tag matches. */
    flcn_w32(f, FALCON_IMEMT(0), falcon_off >> 8);
    gsp_platform->mb();

    /* Stream u32 words. Source is little-endian on disk; assemble
     * explicitly so a misaligned host pointer works (the booter
     * blob's IMEM section is u32-aligned in practice but we don't
     * want to assume that on every platform). */
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = (uint32_t)src[i + 0]
                   | ((uint32_t)src[i + 1] <<  8)
                   | ((uint32_t)src[i + 2] << 16)
                   | ((uint32_t)src[i + 3] << 24);
        flcn_w32(f, FALCON_IMEMD(0), w);
    }
    gsp_platform->mb();
    return GSP_OK;
}

int falcon_pio_upload_dmem(struct falcon *f, const uint8_t *src,
                           uint32_t len, uint32_t falcon_off)
{
    if (!f || !f->initialized || !src) return GSP_ERR_INVAL;
    if ((falcon_off & 3u) != 0 || (len & 3u) != 0) return GSP_ERR_INVAL;
    if (falcon_off + len > f->dmem_size) return GSP_ERR_INVAL;
    if (len == 0) return GSP_OK;

    flcn_w32(f, FALCON_DMEMC(0), FALCON_DMEMC_AINCW | (falcon_off & 0x00FFFFFFu));
    gsp_platform->mb();

    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = (uint32_t)src[i + 0]
                   | ((uint32_t)src[i + 1] <<  8)
                   | ((uint32_t)src[i + 2] << 16)
                   | ((uint32_t)src[i + 3] << 24);
        flcn_w32(f, FALCON_DMEMD(0), w);
    }
    gsp_platform->mb();
    return GSP_OK;
}
