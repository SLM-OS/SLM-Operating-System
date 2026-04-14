/*
 * gsp.c — Shared NVIDIA GSP-RM bringup core.
 *
 * Platform-independent pieces of the 7-phase GSP boot sequence that
 * docs/nvidia-gsp.md traces from nouveau. The actual phase
 * implementations will land alongside (gsp_falcon.c, gsp_rpc.c,
 * gsp_compute.c) as E3–E5 land.
 *
 * This file is compiled on every Ampere-capable platform (x86-64
 * discrete, Jetson Orin Nano integrated). The per-platform file
 * — kernel/arch/<arch>/nvidia_gsp_platform.c — is responsible for
 * populating `gsp_platform` with its vtable before gsp_init() is
 * called.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gsp.h"
#include "uart.h"

const struct gsp_platform_ops *gsp_platform;

static enum gsp_state g_state = GSP_STATE_UNINIT;
static int            g_last_error_phase = -1;

enum gsp_state gsp_get_state(void)      { return g_state; }
int            gsp_last_error_phase(void) { return g_last_error_phase; }

/* ---- Helpers ---- */

static bool phase0_firmware_load(void)
{
    if (!gsp_platform || !gsp_platform->firmware_get) {
        uart_puts("[GSP] no platform ops installed\n");
        return false;
    }

    struct gsp_firmware_blob b[GSP_FW_KIND_COUNT];
    static const char *NAMES[GSP_FW_KIND_COUNT] = {
        [GSP_FW_GSP]           = "gsp",
        [GSP_FW_BOOTLOADER]    = "bootloader",
        [GSP_FW_BOOTER_LOAD]   = "booter_load",
        [GSP_FW_BOOTER_UNLOAD] = "booter_unload",
    };

    bool ok = true;
    for (int k = 0; k < GSP_FW_KIND_COUNT; k++) {
        gsp_platform->firmware_get((enum gsp_firmware_kind)k, &b[k]);
        if (!b[k].data || b[k].size == 0) {
            uart_printf("[GSP] firmware missing: %s\n", NAMES[k]);
            ok = false;
        }
    }
    if (!ok) return false;

    /* Sanity — the GSP-RM blob is the big one (~38 MB). If it's
     * under 1 MB something extracted wrong. */
    if (b[GSP_FW_GSP].size < 1024 * 1024) {
        uart_printf("[GSP] gsp.bin suspiciously small: %lu bytes\n",
                    (unsigned long)b[GSP_FW_GSP].size);
        return false;
    }

    uart_printf("[GSP] firmware loaded (version %s):\n",
                b[GSP_FW_GSP].version ? b[GSP_FW_GSP].version : "unknown");
    for (int k = 0; k < GSP_FW_KIND_COUNT; k++) {
        uart_printf("[GSP]   %s: %lu bytes\n",
                    NAMES[k], (unsigned long)b[k].size);
    }
    return true;
}

/* ---- Public entry ---- */

int gsp_init(void)
{
    if (g_state != GSP_STATE_UNINIT) {
        uart_printf("[GSP] already initialized (state=%d)\n", (int)g_state);
        return 0;
    }

    uart_puts("[GSP] starting Ampere GSP-RM bringup\n");

    /* Phase 0 — firmware load + sanity check. */
    if (!phase0_firmware_load()) {
        g_state = GSP_STATE_FAILED;
        g_last_error_phase = 0;
        return -1;
    }
    g_state = GSP_STATE_FW_LOADED;

    /* Phases 1–7 land in E2–E5. For now bail out with a clear
     * message so the kernel boots cleanly and test-pc doesn't hang
     * waiting for a non-existent GSP RPC channel. */
    uart_puts("[GSP] phase 1+ not yet implemented — see gsp.h\n");
    return -1;
}

/* ---- Stubs for the compute submission API (E5) ---- */

int gsp_compute_matmul_submit(uint64_t a_off, uint64_t b_off,
                              uint64_t c_off,
                              uint32_t m, uint32_t k, uint32_t n)
{
    (void)a_off; (void)b_off; (void)c_off;
    (void)m;     (void)k;     (void)n;
    return -1;    /* GSP not running — caller falls back to CPU */
}

int gsp_compute_sync(void) { return -1; }
