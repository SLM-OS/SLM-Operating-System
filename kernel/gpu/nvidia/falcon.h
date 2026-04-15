/*
 * falcon.h — NVIDIA Falcon v4 primitives for GSP-RM bringup (E3).
 *
 * Falcon is NVIDIA's tiny security / control processor (RV32-ish).
 * Ampere GA10x uses two instances E3 needs to talk to:
 *
 *   - **GSP Falcon** at BAR0 + 0x00110000. Dual-mode core that also
 *     houses the RISC-V RV64 boot control. GSP-RM ucode runs here
 *     after FWSEC-FRTS has set up the Write Protected Region.
 *   - **SEC2 Falcon** at BAR0 + 0x00840000. Runs the "Booter Load"
 *     signed ucode that validates and loads GSP-RM into the WPR.
 *
 * Both engines share the same Falcon v4 register layout — only the
 * base address differs. The shared core driver (this file +
 * falcon.c) speaks to them identically.
 *
 * All addresses and field semantics verified against
 *   - nouveau `drivers/gpu/drm/nouveau/nvkm/falcon/{base,ga102}.c`
 *   - NVIDIA open-gpu-kernel-modules `src/common/inc/swref/published/ampere/ga102/dev_falcon_v4.h`
 *   - `docs/reference/nvidia-gsp-bringup-sequence.md` (captured 2026-04-14)
 *
 * Not used on Jetson Orin Nano — its GSP ucode is pre-loaded by
 * the platform firmware before Linux comes up, so Jetson-specific
 * bringup doesn't route through Falcon DMA from host.
 */

#ifndef GPU_NVIDIA_FALCON_H
#define GPU_NVIDIA_FALCON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Engine bases (absolute, BAR0-relative) ---- */

#define NV_PGSP_BASE                  0x00110000u   /* GSP Falcon */
#define NV_PGSP_RISCV_BASE            0x00111000u   /* GSP RISC-V PRI aperture */
#define NV_PSEC2_BASE                 0x00840000u   /* SEC2 Falcon */
#define NV_PSEC2_BROM_BASE            0x00841000u   /* SEC2 RISC-V/BROM PRI aperture */

/* ---- Falcon v4 register offsets (relative to engine base) ---- */

#define FALCON_IRQSCLR                0x004u
#define FALCON_IRQSTAT                0x008u
#define FALCON_IRQMCLR                0x014u
#define FALCON_IRQMSET                0x010u
#define FALCON_IRQMASK                0x018u
#define FALCON_IRQDEST                0x01cu
#define FALCON_MAILBOX0               0x040u
#define FALCON_MAILBOX1               0x044u
#define FALCON_OS                     0x080u
#define FALCON_DEBUGINFO              0x094u
#define FALCON_CPUCTL                 0x100u
#define FALCON_BOOTVEC                0x104u
#define FALCON_HWCFG                  0x108u
#define FALCON_DMACTL                 0x10cu
#define FALCON_DMATRFBASE             0x110u
#define FALCON_DMATRFMOFFS            0x114u
#define FALCON_DMATRFCMD              0x118u
#define FALCON_DMATRFFBOFFS           0x11cu
#define FALCON_DMATRFBASE1            0x128u
#define FALCON_CPUCTL_ALIAS           0x130u
#define FALCON_HWCFG2                 0x0f4u
#define FALCON_ENGINE                 0x3c0u

/* PIO IMEM/DMEM — pre-Ampere path, kept for debugging. Ampere uses DMA. */
#define FALCON_IMEMC(i)               (0x180u + 0x10u * (i))
#define FALCON_IMEMD(i)               (0x184u + 0x10u * (i))
#define FALCON_IMEMT(i)               (0x188u + 0x10u * (i))
#define FALCON_DMEMC(i)               (0x1c0u + 0x08u * (i))
#define FALCON_DMEMD(i)               (0x1c4u + 0x08u * (i))

/* ---- CPUCTL bits ---- */

#define FALCON_CPUCTL_STARTCPU        (1u << 1)   /* WO — release halt */
#define FALCON_CPUCTL_HALTED          (1u << 4)   /* RO — core is halted */
#define FALCON_CPUCTL_ALIAS_EN        (1u << 6)   /* RO — ALIAS register armed */

/* ---- DMACTL bits ---- */

#define FALCON_DMACTL_REQUIRE_CTX     (1u << 0)
#define FALCON_DMACTL_DMEM_SCRUBBING  (1u << 1)   /* RO — 1 = scrub in progress */
#define FALCON_DMACTL_IMEM_SCRUBBING  (1u << 2)   /* RO — 1 = scrub in progress */

/* ---- DMATRFCMD bits ---- */

#define FALCON_DMATRFCMD_FULL         (1u << 0)
#define FALCON_DMATRFCMD_IDLE         (1u << 1)   /* RO — 1 when no DMA in flight */
#define FALCON_DMATRFCMD_SEC(v)       (((v) & 3u) << 2)
#define FALCON_DMATRFCMD_IMEM         (1u << 4)   /* 1 = IMEM, 0 = DMEM */
#define FALCON_DMATRFCMD_WRITE        (1u << 5)   /* 1 = FB->Falcon, 0 = Falcon->FB */
#define FALCON_DMATRFCMD_SIZE(v)      (((v) & 7u) << 8)
#define FALCON_DMATRFCMD_CTXDMA(v)    (((v) & 7u) << 12)
#define FALCON_DMATRFCMD_SET_DMTAG    (1u << 16)

/* DMATRFCMD SIZE encoding — log2(bytes) - 2.
 * Ampere DMA transfers are always 256-byte chunks. */
#define FALCON_DMATRFCMD_SIZE_256B    6u   /* 2^(6+2) = 256 */
#define FALCON_DMA_CHUNK              256u

/* ---- HWCFG / HWCFG2 bits ---- */

#define FALCON_HWCFG_IMEM_SIZE_MASK   0x1ffu    /* bits 8:0 — IMEM in blocks of 256 */
#define FALCON_HWCFG_DMEM_SIZE_MASK   0x1ff0000u /* bits 24:16 — in blocks of 256 */
#define FALCON_HWCFG_DMEM_SIZE_SHIFT  16u
#define FALCON_HWCFG2_RISCV_ENABLE    (1u << 10)
#define FALCON_HWCFG2_MEM_SCRUBBING   (1u << 12)  /* 0 = scrub done */

/* ---- ENGINE register ---- */

#define FALCON_ENGINE_RESET           (1u << 0)   /* self-clearing */

/* Pre-DMA setup registers (Ampere GA10x — from nouveau ga102_flcn_fw_load).
 *
 * These aren't used on pre-Ampere. Their exact semantics are
 * undocumented publicly, but the sequence below is what the
 * proprietary driver + nouveau both issue before DMA works on a
 * dual-mode (Falcon + RISC-V) engine:
 *
 *   falcon[0x624] |= 0x80       // enable some transfer gating
 *   falcon[0x10c] = 0           // clear DMACTL
 *   falcon[0x600] mask 0x00010007, (0<<16) | (1<<2) | 1
 *                               // TRANSCFG: mem_type=1, target=1
 */
#define FALCON_PRE_DMA_624            0x624u
#define FALCON_PRE_DMA_624_BIT        0x80u
#define FALCON_PRE_DMA_600            0x600u
#define FALCON_PRE_DMA_600_MASK       0x00010007u
#define FALCON_PRE_DMA_600_VAL        ((0u << 16) | (1u << 2) | 1u)

/* ---- GSP RISC-V PRI (only valid when engine_base == NV_PGSP_BASE) ---- */

#define FALCON_RISCV_CPUCTL           0x388u      /* relative to NV_PGSP_RISCV_BASE */
#define FALCON_RISCV_BCR_CTRL         0x668u
#define FALCON_RISCV_CPUCTL_HALTED    (1u << 4)
#define FALCON_RISCV_CPUCTL_ACTIVE    (1u << 7)   /* 1 = RISC-V running */
#define FALCON_RISCV_BCR_VALID        (1u << 0)
#define FALCON_RISCV_BCR_CORE_SELECT  (1u << 4)   /* 0 = Falcon, 1 = RISC-V */
#define FALCON_RISCV_BCR_BRFETCH      (1u << 8)

/* ---- Falcon v4 BROM (Boot ROM) registers ----
 *
 * The BROM lives at a separate PRI aperture: 0x111000 for GSP,
 * 0x841000 for SEC2. After IMEM+DMEM are uploaded but before
 * CPUCTL.STARTCPU, the driver programs these four registers so the
 * BROM knows which signature in the DMEM to validate against, the
 * engine the ucode targets, and the signing algorithm. Order
 * matters: MOD_SEL must be last (it triggers the actual verify).
 *
 * Offsets are relative to the BROM base (0x111000 / 0x841000),
 * not the Falcon base. See `docs/reference/nouveau-falcon-hs-boot.md`.
 */
#define FALCON_BROM_PARAADDR0         0x210u      /* DMEM byte offset of signature */
#define FALCON_BROM_UCODE_ID          0x198u      /* from descriptor's UcodeId */
#define FALCON_BROM_ENGIDMASK         0x19Cu      /* from descriptor's EngineIdMask */
#define FALCON_BROM_MOD_SEL           0x180u      /* signing algo; triggers verify */
#define FALCON_BROM_MOD_SEL_RSA3K     1u


/* ---- Public API ----
 *
 * Every function operates on a `struct falcon` that records the
 * engine base and caches HWCFG values read during init. All
 * register access goes through the shared `struct gsp_platform_ops`
 * vtable — the Falcon driver is platform-agnostic.
 */

struct falcon {
    uint32_t base;            /* engine base, BAR0-relative */
    uint32_t imem_size;       /* bytes, cached from HWCFG */
    uint32_t dmem_size;       /* bytes, cached from HWCFG */
    bool     has_riscv;       /* true if this engine has RISC-V PRI (GSP only) */
    bool     initialized;
};

/* ---- Timeouts (microseconds) ----
 *
 * Conservative — real hardware typically completes these in < 1 ms.
 * Large margins guard against boot-time bus contention. */
#define FALCON_SCRUB_TIMEOUT_US       (500u * 1000u)  /* 500 ms */
#define FALCON_RESET_TIMEOUT_US       (100u * 1000u)  /* 100 ms */
#define FALCON_DMA_TIMEOUT_US         (100u * 1000u)
#define FALCON_HALT_TIMEOUT_US        (2u * 1000u * 1000u)  /* 2 s for Booter / FWSEC */

/*
 * Probe a Falcon engine. Reads HWCFG / HWCFG2 to fill in imem/dmem
 * sizes and the has_riscv flag. Does NOT reset the engine.
 *
 * Returns 0 on success. -1 if the engine base reads back 0xFFFFFFFF
 * (off-die / not mapped) or HWCFG is obviously invalid.
 */
int falcon_probe(struct falcon *f, uint32_t engine_base);

/*
 * Reset an engine via the self-clearing ENGINE.RESET bit, then
 * wait until HWCFG2.MEM_SCRUBBING reports 0 (DMEM/IMEM scrub done).
 * On return, the Falcon is halted with scratch cleared — safe for
 * ucode upload.
 */
int falcon_reset(struct falcon *f);

/*
 * Wait until the Falcon CPU is halted (CPUCTL.HALTED = 1). Used
 * after running signed ucode to detect completion. Returns 0 on
 * halt, -1 on timeout.
 */
int falcon_wait_halted(struct falcon *f, uint32_t timeout_us);

/*
 * Set the Falcon boot vector (PC at STARTCPU) and start the CPU.
 * Convenience wrapper — equivalent to write(BOOTVEC, pc) + write
 * CPUCTL.STARTCPU. No wait; caller polls or calls falcon_wait_halted.
 */
void falcon_start(struct falcon *f, uint32_t boot_pc);

/*
 * DMA-upload bytes from system memory into Falcon IMEM or DMEM.
 *
 * @src_dma   40-bit IOVA of the source buffer in sys RAM. Caller
 *            must have already bound the buffer via VFIO_IOMMU_MAP_DMA
 *            (Linux harness) or the platform IOMMU (bare metal).
 * @falcon_off  Byte offset within IMEM (or DMEM) to upload to.
 *              Must be a multiple of 256.
 * @len       Bytes to transfer. Must be a multiple of 256 — Ampere
 *            Falcon DMA only accepts 256-byte chunks.
 * @to_imem   true = IMEM target, false = DMEM.
 *
 * Returns 0 on success, -1 on DMATRFCMD timeout or alignment error.
 */
int falcon_dma_upload(struct falcon *f, uint64_t src_dma,
                      uint32_t falcon_off, uint32_t len,
                      bool to_imem);

/*
 * Convenience: check whether the engine is in a clean state for
 * ucode upload. Returns true if CPU is halted AND no DMA in flight
 * AND no memory scrub in progress.
 */
bool falcon_is_idle(const struct falcon *f);

/*
 * Boot a heavy-signed ucode on a Falcon.
 *
 * Preconditions:
 *   - IMEM and DMEM images already DMA'd into @f via falcon_dma_upload().
 *   - Signature was included in the DMEM image at offset @dmem_sign_off
 *     (relative to DMEM start).
 *   - @engine_id / @ucode_id / @boot_vec come from the ucode's
 *     V3 descriptor (EngineIdMask, UcodeId, InterfaceOffset).
 *   - @brom_base is NV_PGSP_RISCV_BASE (for GSP Falcon) or
 *     NV_PSEC2_BROM_BASE (for SEC2).
 *
 * Programs BROM PARAADDR/UCODE_ID/ENGIDMASK, sets MOD_SEL=RSA3K
 * (triggers signature verify), writes BOOTVEC, kicks STARTCPU,
 * polls CPUCTL.HALTED up to @timeout_us.
 *
 * Returns 0 if the Falcon halts within the timeout. -1 on timeout
 * or if the engine is not in a clean state to start from. Does NOT
 * verify the ucode-level success code — caller reads MAILBOX0 or
 * engine-specific error registers to decide "did the ucode do what
 * it was supposed to".
 */
int falcon_hs_boot(struct falcon *f,
                   uint32_t brom_base,
                   uint32_t dmem_sign_off,
                   uint32_t engine_id,
                   uint32_t ucode_id,
                   uint32_t boot_vec,
                   uint32_t timeout_us);

/*
 * On dual-mode engines (GSP Falcon), force Falcon (non-RISC-V)
 * mode. Reads BCR_CTRL (RISC-V PRI aperture + 0x668); if
 * CORE_SELECT is set to RISC-V, clears BCR_CTRL and polls for
 * VALID. No-op on SEC2 (has no RISC-V aperture). Must be called
 * after falcon_reset and before falcon_hs_boot / falcon_dma_upload
 * when targeting a dual-mode engine with a Falcon ucode.
 *
 * Returns 0 on success or if the engine is already in Falcon mode.
 * -1 on timeout waiting for BCR_CTRL.VALID.
 */
int falcon_select_falcon_mode(struct falcon *f);

/*
 * Ampere pre-DMA setup poke sequence. Must run once after reset
 * and before falcon_dma_upload on a fresh engine. Writes three
 * Ampere-specific config registers (0x624, 0x10c, 0x600) in the
 * exact sequence nouveau's ga102_flcn_fw_load uses. Safe to
 * call on any Ampere Falcon (GSP or SEC2).
 */
void falcon_pre_dma_setup(struct falcon *f);

/*
 * Pre-PIO setup: matches the `else` branch of nouveau's
 * `gm200_flcn_fw_load`. Two register writes before the booter
 * (PIO-loaded HS ucode) is uploaded to SEC2:
 *
 *   falcon[0x624] |= 0x80   // mask-set bit 7
 *   falcon[0x10c]  = 0      // clear DMACTL
 *
 * Notably no FBIF_TRANSCFG (0x600) — PIO doesn't go through FBIF.
 * Safe on any Ampere Falcon.
 */
void falcon_pre_pio_setup(struct falcon *f);

/*
 * PIO upload bytes from system memory into Falcon IMEM via the
 * IMEMC/IMEMT/IMEMD register triplet at port 0.
 *
 * @src         pointer to source bytes (host memory; no DMA needed).
 * @len         number of bytes to copy. Must be a multiple of 4.
 * @falcon_off  byte offset within IMEM to upload to. Must be 4-byte
 *              aligned. The "tag" written to IMEMT is `falcon_off >> 8`
 *              — same convention nouveau uses (instruction-page tag).
 * @is_secure   when true, sets IMEMC.SECURE (bit 28) so the upload
 *              targets the secure IMEM region. Booter has both a
 *              non-secure section (false) and a secure section (true).
 *
 * No completion polling — IMEMD writes complete synchronously on the
 * PRI bus. Returns 0 on success, -1 on alignment violation.
 */
int falcon_pio_upload_imem(struct falcon *f, const uint8_t *src,
                           uint32_t len, uint32_t falcon_off, bool is_secure);

/*
 * PIO upload bytes from system memory into Falcon DMEM via the
 * DMEMC/DMEMD register pair at port 0. Same constraints as
 * falcon_pio_upload_imem but DMEM has no secure-bit and no tag.
 */
int falcon_pio_upload_dmem(struct falcon *f, const uint8_t *src,
                           uint32_t len, uint32_t falcon_off);

#endif /* GPU_NVIDIA_FALCON_H */
