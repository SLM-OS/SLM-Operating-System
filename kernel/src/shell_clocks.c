/*
 * shell_clocks.c - `clocks`: read the clock rates BPMP reports (#475).
 *
 * Memory and GPU clocks bound inference speed, and after a UEFI-direct
 * boot nothing like Linux's governors has raised them, so the rates have
 * to be looked at rather than assumed. `clocks emc <MHz>` requests an EMC
 * rate; BPMP rounds it to a rate in its DVFS table.
 */

#include "bpmp.h"
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
#endif
    return 0;
}
