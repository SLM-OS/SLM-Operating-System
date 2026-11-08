/*
 * shell_clocks.c - `clocks`: read the clock rates BPMP reports (#475).
 *
 * Memory and GPU clocks bound inference speed, and after a UEFI-direct
 * boot nothing like Linux's governors has raised them, so the rates have
 * to be looked at rather than assumed. `clocks emc <MHz>` requests an EMC
 * rate; BPMP rounds it to a rate in its DVFS table.
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
        uint64_t got = 0;
        int rc = bpmp_clk_set_rate(TEGRA234_CLK_EMC,
                                   (uint64_t)mhz * 1000000u, &got);
        if (rc != 0) {
            shell_printf("clocks: emc set_rate rc=%d\r\n", rc);
            return -1;
        }
        shell_printf("  emc set, BPMP chose %llu MHz\r\n",
                     (unsigned long long)(got / 1000000u));
    }
    /* #1161: a memory clock floor through the bandwidth manager
     * (MRQ_BWMGR_INT) instead of MRQ_CLK, for a client id (default 2,
     * CPU cluster 0). */
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "emc-floor") == 0) {
        uint32_t mhz = 0, client = 2;
        if (shell_parse_uint(argv[2], &mhz) != 0 ||
            (argc == 4 && shell_parse_uint(argv[3], &client) != 0)) {
            shell_puts("Usage: clocks emc-floor <MHz> [bwmgr client]\r\n");
            return -1;
        }
        uint64_t got = 0;
        int rc = bpmp_emc_floor_khz(client, mhz * 1000u, &got);
        shell_printf("  bwmgr floor %u MHz for client %u: rc=%d, BPMP reports %llu MHz\r\n",
                     (unsigned)mhz, (unsigned)client, rc,
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
        shell_printf("  emc hold   target %llu MHz: %u checks, %u drops (first after %s,"
                     " last after %s), %u restored\r\n",
                     (unsigned long long)(h.target_hz / 1000000u), (unsigned)h.checks,
                     (unsigned)h.drops, h.first_drop[0] ? h.first_drop : "-",
                     h.last_drop[0] ? h.last_drop : "-", (unsigned)h.reasserts);
    }
#endif
    return 0;
}
