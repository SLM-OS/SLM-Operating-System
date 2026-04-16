/*
 * ga10b_bringup.c — GA10B (Jetson integrated Ampere) nvgpu-native bringup.
 *
 * Skeleton with phase stubs. See ga10b_bringup.h for the public API and
 * docs/jetson-nvgpu-bringup-research.md for the architectural overview.
 *
 * Each phase is a separate function so the shell can invoke them
 * independently while we iterate against hardware — same pattern
 * as the discrete-Ampere bringup.c.
 */

#include "ga10b_bringup.h"
#include "gsp.h"
#include "falcon.h"
#include "../../include/uart.h"

#include <stddef.h>
#include <string.h>

extern const struct gsp_platform_ops *gsp_platform;

/* ---- Firmware symbols from nvidia_ga10b_firmware.S ----
 *
 * Guarded by ENABLE_GA10B_FIRMWARE: the build sets this when
 * -DGA10B_FIRMWARE_DIR=<path> is provided and all required blobs
 * exist. Without it, ga10b_firmware_get() returns -1 and any
 * phase that needs firmware aborts cleanly. */
#if defined(ENABLE_GA10B_FIRMWARE)
extern const uint8_t ga10b_fw_acr_text_start[];
extern const uint8_t ga10b_fw_acr_text_end[];
extern const uint8_t ga10b_fw_acr_data_start[];
extern const uint8_t ga10b_fw_acr_data_end[];
extern const uint8_t ga10b_fw_acr_manifest_start[];
extern const uint8_t ga10b_fw_acr_manifest_end[];
extern const uint8_t ga10b_fw_fecs_start[];
extern const uint8_t ga10b_fw_fecs_end[];
extern const uint8_t ga10b_fw_fecs_sig_start[];
extern const uint8_t ga10b_fw_fecs_sig_end[];
extern const uint8_t ga10b_fw_gpccs_start[];
extern const uint8_t ga10b_fw_gpccs_end[];
extern const uint8_t ga10b_fw_gpccs_sig_start[];
extern const uint8_t ga10b_fw_gpccs_sig_end[];
extern const uint8_t ga10b_fw_pmu_image_start[];
extern const uint8_t ga10b_fw_pmu_image_end[];
extern const uint8_t ga10b_fw_pmu_desc_start[];
extern const uint8_t ga10b_fw_pmu_desc_end[];
extern const uint8_t ga10b_fw_pmu_sig_start[];
extern const uint8_t ga10b_fw_pmu_sig_end[];
extern const uint8_t ga10b_fw_net_a_start[];
extern const uint8_t ga10b_fw_net_a_end[];
extern const uint8_t ga10b_fw_net_b_start[];
extern const uint8_t ga10b_fw_net_b_end[];
extern const uint8_t ga10b_fw_net_c_start[];
extern const uint8_t ga10b_fw_net_c_end[];
extern const uint8_t ga10b_fw_net_d_start[];
extern const uint8_t ga10b_fw_net_d_end[];
extern const uint8_t ga10b_fw_safety_text_start[];
extern const uint8_t ga10b_fw_safety_text_end[];
extern const uint8_t ga10b_fw_safety_data_start[];
extern const uint8_t ga10b_fw_safety_data_end[];
extern const uint8_t ga10b_fw_safety_manifest_start[];
extern const uint8_t ga10b_fw_safety_manifest_end[];
#endif

int ga10b_firmware_get(enum ga10b_firmware_kind kind,
                       struct ga10b_firmware_blob *out)
{
    if (!out) return -1;
    out->data = NULL;
    out->size = 0;

#if !defined(ENABLE_GA10B_FIRMWARE)
    (void)kind;
    return -1;
#else
    switch (kind) {
#   define _FW(K, SYM) \
        case K: \
            out->data = SYM##_start; \
            out->size = (size_t)(SYM##_end - SYM##_start); \
            return 0;
    _FW(GA10B_FW_ACR_TEXT,         ga10b_fw_acr_text)
    _FW(GA10B_FW_ACR_DATA,         ga10b_fw_acr_data)
    _FW(GA10B_FW_ACR_MANIFEST,     ga10b_fw_acr_manifest)
    _FW(GA10B_FW_FECS,             ga10b_fw_fecs)
    _FW(GA10B_FW_FECS_SIG,         ga10b_fw_fecs_sig)
    _FW(GA10B_FW_GPCCS,            ga10b_fw_gpccs)
    _FW(GA10B_FW_GPCCS_SIG,        ga10b_fw_gpccs_sig)
    _FW(GA10B_FW_PMU_IMAGE,        ga10b_fw_pmu_image)
    _FW(GA10B_FW_PMU_DESC,         ga10b_fw_pmu_desc)
    _FW(GA10B_FW_PMU_SIG,          ga10b_fw_pmu_sig)
    _FW(GA10B_FW_NET_A,            ga10b_fw_net_a)
    _FW(GA10B_FW_NET_B,            ga10b_fw_net_b)
    _FW(GA10B_FW_NET_C,            ga10b_fw_net_c)
    _FW(GA10B_FW_NET_D,            ga10b_fw_net_d)
    _FW(GA10B_FW_SAFETY_TEXT,      ga10b_fw_safety_text)
    _FW(GA10B_FW_SAFETY_DATA,      ga10b_fw_safety_data)
    _FW(GA10B_FW_SAFETY_MANIFEST,  ga10b_fw_safety_manifest)
#   undef _FW
    default: return -1;
    }
#endif
}

/* ---- BAR0 engine bases (GA10B layout) ----
 *
 * Verified via docs/jetson-nvgpu-acr-analysis.md against OE4T nvgpu
 * l4t-r36.5 sources. GSP Falcon block starts at 0x110000; the RISCV
 * subblock (used for ACR) is at 0x111000.
 *
 * FECS, GPCCS, PMU are bootstrapped by ACR itself on GA10B (since we
 * skip LSPMU) — we don't need to probe them from SLM-OS. Their bases
 * will matter for later phases (GR init, method submission) but not
 * for ACR load. */
/* NV_PGSP_BASE and NV_PGSP_RISCV_BASE come from falcon.h. Registered
 * here for grep-ability:
 *   NV_PGSP_BASE        0x00110000   (GSP Falcon block)
 *   NV_PGSP_RISCV_BASE  0x00111000   (GSP RISCV subblock)
 */

/* ---- GSP RISCV registers (offsets from NV_PGSP_RISCV_BASE) ----
 *
 * Used by phase 1 to start the HS ACR ucode in preloaded mode.
 * Register meanings per docs/reference/nvgpu-hw-ga10b-hw_priscv_ga10b.h. */

#define RISCV_BOOT_VECTOR_LO        0x380u
#define RISCV_BOOT_VECTOR_HI        0x384u
#define RISCV_CPUCTL                0x388u
#define RISCV_CPUCTL_STARTCPU       (1u << 0)
#define RISCV_BR_RETCODE            0x65cu
#define RISCV_BCR_CTRL              0x668u
#define RISCV_BCR_CTRL_VALID        (1u << 0)
#define RISCV_BCR_CTRL_BRFETCH      (1u << 8)
#define RISCV_BCR_CTRL_CORE_RISCV   (1u << 4)   /* 1 = select RISCV core */
/* BCR_CTRL = 0x11 = CORE_SELECT=RISCV | VALID   (preloaded IMEM/DMEM mode) */
#define RISCV_BCR_CTRL_PRELOADED \
    (RISCV_BCR_CTRL_CORE_RISCV | RISCV_BCR_CTRL_VALID)

/* RISCV CPUCTL status bits — need explicit masks (the aliased Falcon
 * ones from falcon.h use HALTED=bit4 which works for both cores). */
#define RISCV_CPUCTL_HALTED         (1u << 4)
#define RISCV_CPUCTL_ACTIVE         (1u << 7)

/* ACR HS completion marker — written to MAILBOX0 by the ucode when
 * it runs. nvgpu's acr_sw_ga10b checks for ACR_OK. Exact constant
 * verified in docs/reference/nvgpu-common-acr-acr_bootstrap.c. */
#define ACR_BOOT_OK                 0x000000ffu

/* ============================================================================
 * Phase entry points
 * ============================================================================
 *
 * For now, all phases are stubs that log and return -1. Each will be
 * filled in by tasks #13 (ACR), #14 (FECS/GPCCS/PMU), #15 (channel),
 * #16 (smoke test / compute). Returning -1 here lets the runner exit
 * cleanly and lets the harness walk phase-by-phase while we iterate.
 */

int ga10b_bringup_prepare(struct ga10b_bringup *b)
{
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    b->state = GA10B_BRINGUP_INIT;
    b->last_error_phase = -1;

    if (!gsp_platform) {
        uart_puts("[GA10B] no platform ops installed\n");
        return -1;
    }

    /* Sanity: firmware must be embedded. Checking one blob is enough —
     * either the build linked all of nvidia_ga10b_firmware.S or none. */
    struct ga10b_firmware_blob tmp;
    if (ga10b_firmware_get(GA10B_FW_ACR_TEXT, &tmp) < 0 || tmp.size == 0) {
        uart_puts("[GA10B] firmware not embedded "
                  "(build with -DGA10B_FIRMWARE_DIR=<path>)\n");
        return -1;
    }
    uart_printf("[GA10B] ACR text: %lu bytes\n", (unsigned long)tmp.size);

    /* Probe the GSP Falcon — this is the one ACR runs on. FECS,
     * GPCCS, and PMU are bootstrapped by ACR itself (since we skip
     * LSPMU), so SLM-OS doesn't touch their MMIO directly. */
    if (falcon_probe(&b->gsp_flcn, NV_PGSP_BASE) < 0) {
        uart_puts("[GA10B] GSP Falcon probe failed\n");
        return -1;
    }

    uart_puts("[GA10B] prepare OK — GSP Falcon probed\n");
    return 0;
}

/* ---- Phase 1: ACR on GSP RISCV ----
 *
 * Load the ACR HS ucode into the GSP Falcon's IMEM/DMEM via PIO, then
 * kick the RISCV core in "preloaded" mode. The manifest at the tail
 * of DMEM gets consumed by the Falcon BROM to verify the signed
 * ucode; if verification passes, BROM jumps to the entry point in
 * IMEM and ACR runs.
 *
 * This matches nvgpu_acr_bootstrap_hs_ucode_riscv in
 *   docs/reference/nvgpu-common-acr-acr_bootstrap.c:360–419
 * (preloaded BCR_CTRL=0x11 branch — we skip the 0x111 DMA path).
 *
 * On success the RISCV core runs, ACR authenticates FECS/GPCCS/(PMU),
 * and eventually halts with MAILBOX0 = ACR_BOOT_OK and BR_RETCODE =
 * success pattern. On failure BR_RETCODE reports the BROM error code
 * (signature mismatch, manifest bad, etc.).
 */

/* Thin wrappers over the platform vtable — saves repeating the
 * gsp_platform-> prefix at every register poke. */
static inline uint32_t bar0_r32(uint32_t off)
{
    return gsp_platform->read32(off);
}
static inline void bar0_w32(uint32_t off, uint32_t val)
{
    gsp_platform->write32(off, val);
}

/*
 * GA10B GSP engine reset — differs from the generic falcon_reset
 * (which only writes the self-clearing RESET bit and polls HWCFG2).
 * On GA10B the pattern is an explicit assert/deassert:
 *
 *   1. Write ENGINE.RESET = 1       (assert)
 *   2. Wait 10+ µs
 *   3. Write ENGINE.RESET = 0       (deassert — crucial!)
 *
 * Reference: docs/reference/nvgpu-hal-gsp-gsp_ga10b.c:54 ga10b_gsp_engine_reset.
 * Without the deassert write the engine stays in reset forever, and
 * HWCFG2/CPUCTL read back as PRI poison (0xbadfXXXX) — which is
 * exactly what we see on jetson-nano-2 after Linux's nvgpu detach.
 */
static int ga10b_gsp_engine_reset(void)
{
    bar0_w32(NV_PGSP_BASE + FALCON_ENGINE, FALCON_ENGINE_RESET);
    gsp_platform->mb();

    /* 10 µs delay. gsp_platform doesn't expose a delay primitive,
     * so spin a bounded loop. On Cortex-A78AE @ ~1.5 GHz, 15k NOPs
     * is ~10 µs. Generous. */
    for (volatile int i = 0; i < 15000; i++) { }

    bar0_w32(NV_PGSP_BASE + FALCON_ENGINE, 0u);
    gsp_platform->mb();

    /* Wait for HWCFG2.MEM_SCRUBBING to clear (engine done scrubbing
     * its own memory). Up to 500 ms. Note: for the initial boot path
     * where the engine has never been reset, this typically completes
     * in < 1 ms. */
    uint32_t loops = 500u * 1000u;
    while (loops-- > 0) {
        uint32_t hwcfg2 = bar0_r32(NV_PGSP_BASE + FALCON_HWCFG2);
        if (hwcfg2 == 0xFFFFFFFFu) return -1;
        if ((hwcfg2 & 0xbadf0000u) == 0xbadf0000u) {
            /* Still priv-locked; keep waiting. */
        } else if ((hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) == 0) {
            return 0;
        }
        for (volatile int i = 0; i < 1500; i++) { }  /* ~1 µs */
    }
    return -1;
}

static int wait_for_halt_us(uint32_t timeout_us)
{
    /* Poll RISCV CPUCTL for HALTED=1 or ACTIVE=0. 1 µs loops use a
     * cheap CNTPCT read — accurate enough for a coarse timeout. */
    uint32_t loops = timeout_us;
    while (loops-- > 0) {
        uint32_t ctl = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL);
        if ((ctl & RISCV_CPUCTL_HALTED) || !(ctl & RISCV_CPUCTL_ACTIVE)) {
            return 0;
        }
        /* Spin a few times to make one "loop" take ~1 µs on Cortex-A78AE
         * at 1.5 GHz. Not exact; fine for coarse ms-scale timeouts. */
        for (volatile int i = 0; i < 1500; i++) { }
    }
    return -1;
}

int ga10b_bringup_acr(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_INIT) return -1;

    struct ga10b_firmware_blob text, data, manifest;
    if (ga10b_firmware_get(GA10B_FW_ACR_TEXT, &text) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_DATA, &data) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_MANIFEST, &manifest) < 0) {
        uart_puts("[GA10B-ACR] firmware blobs missing\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_printf("[GA10B-ACR] text=%lu data=%lu manifest=%lu bytes\n",
                (unsigned long)text.size,
                (unsigned long)data.size,
                (unsigned long)manifest.size);

    /* Sanity: code must fit in IMEM, data+manifest must fit in DMEM. */
    if (text.size > b->gsp_flcn.imem_size) {
        uart_printf("[GA10B-ACR] text %lu > IMEM %u\n",
                    (unsigned long)text.size, b->gsp_flcn.imem_size);
        b->last_error_phase = 1;
        return -1;
    }
    if (data.size + manifest.size > b->gsp_flcn.dmem_size) {
        uart_printf("[GA10B-ACR] data+manifest %lu > DMEM %u\n",
                    (unsigned long)(data.size + manifest.size),
                    b->gsp_flcn.dmem_size);
        b->last_error_phase = 1;
        return -1;
    }

    /* Reset GSP Falcon via the GA10B-specific assert/deassert sequence
     * (generic falcon_reset only asserts — sufficient for discrete
     * Ampere where the engine comes out of BIOS in a usable state,
     * but not for Jetson where Linux nvgpu leaves it in reset). */
    if (ga10b_gsp_engine_reset() < 0) {
        uart_puts("[GA10B-ACR] GSP engine reset failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_puts("[GA10B-ACR] GSP engine reset complete\n");

    /* PIO-upload the three pieces:
     *   - text  → IMEM offset 0    (secure=true so IMEMC.SECURE is set
     *                                — matches nvgpu copy_to_imem path
     *                                for HS ucodes)
     *   - data  → DMEM offset 0
     *   - manifest → DMEM at offset (dmem_size - manifest_size). The
     *     Falcon BROM reads PKC parameters from the tail of DMEM.
     */
    if (falcon_pio_upload_imem(&b->gsp_flcn, text.data, (uint32_t)text.size,
                               0u, true) < 0) {
        uart_puts("[GA10B-ACR] IMEM upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    if (falcon_pio_upload_dmem(&b->gsp_flcn, data.data, (uint32_t)data.size,
                               0u) < 0) {
        uart_puts("[GA10B-ACR] DMEM data upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uint32_t manifest_off = b->gsp_flcn.dmem_size - (uint32_t)manifest.size;
    if (falcon_pio_upload_dmem(&b->gsp_flcn, manifest.data,
                               (uint32_t)manifest.size, manifest_off) < 0) {
        uart_puts("[GA10B-ACR] DMEM manifest upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_printf("[GA10B-ACR] ucode loaded "
                "(IMEM@0+%lu, DMEM@0+%lu, manifest@%u+%lu)\n",
                (unsigned long)text.size,
                (unsigned long)data.size,
                manifest_off,
                (unsigned long)manifest.size);

    /* Read back the first 16 bytes of each DMEM region to verify
     * PIO upload integrity. If these don't match the source bytes,
     * something is wrong with our PIO path and the BROM rejection
     * is moot until that's fixed.
     * DMEMC bits: bit 24=AINCW (auto-inc on write), bit 25=AINCR
     *             (auto-inc on read). Low 24 bits = byte offset. */
    {
#       define DMEMC_AINCR_BIT (1u << 25)
        /* Read data@0 */
        bar0_w32(NV_PGSP_BASE + FALCON_DMEMC(0), DMEMC_AINCR_BIT | 0u);
        gsp_platform->mb();
        uint32_t d0 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t d1 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t src_d0 = (uint32_t)data.data[0] |
                          ((uint32_t)data.data[1] << 8) |
                          ((uint32_t)data.data[2] << 16) |
                          ((uint32_t)data.data[3] << 24);
        uint32_t src_d1 = (uint32_t)data.data[4] |
                          ((uint32_t)data.data[5] << 8) |
                          ((uint32_t)data.data[6] << 16) |
                          ((uint32_t)data.data[7] << 24);
        uart_printf("[GA10B-ACR] dmem@0 readback: 0x%08lx 0x%08lx "
                    "(src 0x%08lx 0x%08lx) %s\n",
                    (unsigned long)d0, (unsigned long)d1,
                    (unsigned long)src_d0, (unsigned long)src_d1,
                    (d0 == src_d0 && d1 == src_d1) ? "OK" : "MISMATCH");

        /* Read manifest@manifest_off */
        bar0_w32(NV_PGSP_BASE + FALCON_DMEMC(0),
                 DMEMC_AINCR_BIT | manifest_off);
        gsp_platform->mb();
        uint32_t m0 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t m1 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t src_m0 = (uint32_t)manifest.data[0] |
                          ((uint32_t)manifest.data[1] << 8) |
                          ((uint32_t)manifest.data[2] << 16) |
                          ((uint32_t)manifest.data[3] << 24);
        uint32_t src_m1 = (uint32_t)manifest.data[4] |
                          ((uint32_t)manifest.data[5] << 8) |
                          ((uint32_t)manifest.data[6] << 16) |
                          ((uint32_t)manifest.data[7] << 24);
        uart_printf("[GA10B-ACR] dmem@manifest readback: 0x%08lx 0x%08lx "
                    "(src 0x%08lx 0x%08lx) %s\n",
                    (unsigned long)m0, (unsigned long)m1,
                    (unsigned long)src_m0, (unsigned long)src_m1,
                    (m0 == src_m0 && m1 == src_m1) ? "OK" : "MISMATCH");
    }

    /* Program RISCV boot: vector = 0, BCR_CTRL = CORE_RISCV|VALID. */
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BOOT_VECTOR_LO, 0u);
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BOOT_VECTOR_HI, 0u);
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BCR_CTRL, RISCV_BCR_CTRL_PRELOADED);

    /* Clear MAILBOX0 so we can detect the ucode writing to it. */
    gsp_platform->write32(NV_PGSP_BASE + 0x040u, 0u);
    gsp_platform->mb();

    /* STARTCPU — RISCV begins executing from BOOT_VECTOR (= 0, which
     * after BROM verification means entry point in IMEM). */
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL, RISCV_CPUCTL_STARTCPU);
    gsp_platform->mb();
    uart_puts("[GA10B-ACR] STARTCPU kicked — polling for halt\n");

    /* Wait up to 2 s for ACR to either halt or report status. */
    int wait_rc = wait_for_halt_us(2u * 1000u * 1000u);

    uint32_t mbox0    = bar0_r32(NV_PGSP_BASE + 0x040u);
    uint32_t mbox1    = bar0_r32(NV_PGSP_BASE + 0x044u);
    uint32_t cpuctl   = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL);
    uint32_t retcode  = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_BR_RETCODE);
    uint32_t hwcfg2   = bar0_r32(NV_PGSP_BASE + FALCON_HWCFG2);
    uint32_t bcr_ctrl = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_BCR_CTRL);

    uart_printf("[GA10B-ACR] post-kick: CPUCTL=0x%08lx MBOX0=0x%08lx "
                "MBOX1=0x%08lx BR_RETCODE=0x%08lx wait_rc=%d\n",
                (unsigned long)cpuctl,
                (unsigned long)mbox0,
                (unsigned long)mbox1,
                (unsigned long)retcode,
                wait_rc);
    uart_printf("[GA10B-ACR] hwcfg2=0x%08lx bcr_ctrl=0x%08lx "
                "br_retcode.result=%lu (0=run 1=dunno 2=FAIL 3=PASS)\n",
                (unsigned long)hwcfg2,
                (unsigned long)bcr_ctrl,
                (unsigned long)(retcode & 0x3u));

    if (wait_rc < 0) {
        uart_puts("[GA10B-ACR] timeout — RISCV neither halted nor stopped\n");
        b->last_error_phase = 1;
        return -1;
    }
    /* BR_RETCODE != 0 typically means BROM rejected the ucode.
     * MAILBOX0 = ACR_BOOT_OK (0xff) means ACR ran and succeeded.
     * Either absence of "OK" or presence of a BROM error = fail. */
    if (mbox0 != ACR_BOOT_OK) {
        uart_printf("[GA10B-ACR] ACR did NOT report BOOT_OK "
                    "(expected 0x%x, got 0x%lx)\n",
                    ACR_BOOT_OK, (unsigned long)mbox0);
        b->last_error_phase = 1;
        return -1;
    }

    uart_puts("[GA10B-ACR] ACR running — BOOT_OK received\n");
    b->state = GA10B_BRINGUP_ACR_RUNNING;
    return 0;
}

int ga10b_bringup_fecs(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_ACR_RUNNING) return -1;
    uart_puts("[GA10B] phase 2 (FECS) not yet implemented — see #14\n");
    b->last_error_phase = 2;
    return -1;
}

int ga10b_bringup_gpccs(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_FECS_UP) return -1;
    uart_puts("[GA10B] phase 3 (GPCCS) not yet implemented — see #14\n");
    b->last_error_phase = 3;
    return -1;
}

int ga10b_bringup_pmu(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_GPCCS_UP) return -1;
    uart_puts("[GA10B] phase 4 (PMU) not yet implemented — see #14\n");
    b->last_error_phase = 4;
    return -1;
}

int ga10b_bringup_address_space(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_PMU_UP) return -1;
    uart_puts("[GA10B] phase 5 (GMMU/inst block) not yet implemented\n");
    b->last_error_phase = 5;
    return -1;
}

int ga10b_bringup_channel(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_ENGINES_READY) return -1;
    uart_puts("[GA10B] phase 6 (channel) not yet implemented — see #15\n");
    b->last_error_phase = 6;
    return -1;
}

int ga10b_bringup_smoke_test(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_CHANNEL_OPEN) return -1;
    uart_puts("[GA10B] phase 7 (smoke test) not yet implemented — see #16\n");
    b->last_error_phase = 7;
    return -1;
}

int ga10b_bringup_run(struct ga10b_bringup *b)
{
    int rc;

    rc = ga10b_bringup_prepare(b);
    if (rc < 0) return rc;

    rc = ga10b_bringup_acr(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_fecs(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_gpccs(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_pmu(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_address_space(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_channel(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_smoke_test(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    uart_puts("[GA10B] bringup complete — channel open, method accepted\n");
    return 0;
}
