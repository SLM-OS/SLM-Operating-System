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
#define NV_PGSP_BASE        0x00110000u   /* GSP Falcon block (runs ACR) */

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

int ga10b_bringup_acr(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_INIT) return -1;
    uart_puts("[GA10B] phase 1 (ACR) not yet implemented — see #13\n");
    b->last_error_phase = 1;
    return -1;
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
