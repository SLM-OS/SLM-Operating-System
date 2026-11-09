/*
 * shell_clocks.c - `clocks`: read the clock rates BPMP reports (#475).
 *
 * Memory and GPU clocks bound inference speed, and after a UEFI-direct
 * boot nothing like Linux's governors has raised them, so the rates have
 * to be looked at rather than assumed. `clocks emc <MHz>` requests an EMC
 * floor from BPMP's bandwidth manager (emc_hold.h); BPMP rounds it to a
 * rate in its DVFS table and may run faster for other clients.
 */

#include "bpmp.h"
#include "emc_hold.h"
#include "platform.h"
#include "shell.h"
#include "shell_internal.h"
#include "string.h"
#include <stdint.h>

int cmd_clocks(int argc, char *argv[])
{
    (void)argc;     /* only the Jetson branch takes arguments */
    (void)argv;

    if (!bpmp_is_available()) {
        shell_puts("clocks: no BPMP on this platform\r\n");
        return 0;
    }
#if defined(PLATFORM_JETSON_ORIN_NANO)
    if (argc == 3 && strcmp(argv[1], "emc") == 0) {
        uint32_t mhz = 0;
        if (shell_parse_uint(argv[2], &mhz) != 0 || mhz == 0) {
            shell_puts("Usage: clocks [emc <MHz>]\r\n");
            return -1;
        }
        if ((uint64_t)mhz * 1000000u > TEGRA234_EMC_MAX_HZ) {
            shell_printf("clocks: emc %u MHz is above the %llu MHz top rate\r\n",
                         (unsigned)mhz,
                         (unsigned long long)(TEGRA234_EMC_MAX_HZ / 1000000u));
            return -1;
        }
        /* A floor through the bandwidth manager, which the boot-time and
         * runtime checks then hold; an MRQ_CLK rate would not last
         * (#1161). */
        uint64_t got = 0;
        int rc = emc_hold_set_target((uint64_t)mhz * 1000000u, &got);
        if (rc != 0) {
            shell_printf("clocks: emc floor rc=%d\r\n", rc);
            return -1;
        }
        shell_printf("  emc floor %u MHz, BPMP chose %llu MHz\r\n", (unsigned)mhz,
                     (unsigned long long)(got / 1000000u));
    }
    static const struct {
        uint32_t id;
        const char *name;
    } clocks[] = {
        { TEGRA234_CLK_EMC,     "emc" },
        { TEGRA234_CLK_GPC0CLK, "gpc0clk" },
        { TEGRA234_CLK_GPC1CLK, "gpc1clk" },
        { TEGRA234_CLK_GPUSYS,  "gpusysclk" },
    };
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        uint64_t hz = 0;
        int rc = bpmp_clk_get_rate(clocks[i].id, &hz);
        if (rc != 0) {
            shell_printf("  %-10s rc=%d\r\n", clocks[i].name, rc);
        } else {
            shell_printf("  %-10s %llu MHz (%llu Hz)\r\n", clocks[i].name,
                         (unsigned long long)(hz / 1000000u),
                         (unsigned long long)hz);
        }
    }
    struct emc_hold_state h;
    emc_hold_get_status(&h);
    if (h.target_hz != 0) {
        shell_printf("  emc hold   floor %llu MHz: %u checks, %u drops (first after %s,"
                     " last after %s), %u restored\r\n",
                     (unsigned long long)(h.target_hz / 1000000u), (unsigned)h.checks,
                     (unsigned)h.drops, h.first_drop[0] ? h.first_drop : "-",
                     h.last_drop[0] ? h.last_drop : "-", (unsigned)h.reasserts);
    }
#endif
    return 0;
}
