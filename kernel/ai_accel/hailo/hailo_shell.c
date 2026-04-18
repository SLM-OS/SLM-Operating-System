/*
 * hailo_shell.c — `hailo` shell command.
 *
 * Usage:
 *   hailo          — dump driver state, IDs, BAR map.
 *   hailo probe    — re-run hailo_probe and print the result.
 *   hailo fw       — report firmware version (post-boot only).
 *
 * Safe to run on any platform. On non-RASPI5 builds the driver is
 * never installed (stub returns -ENODEV) so `hailo` just reports
 * "driver not available".
 */

#include "hailo.h"
#include "shell.h"
#include "uart.h"
#include <stdint.h>
#include <string.h>

static int cmd_hailo(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        uint16_t v = 0, d = 0;
        int rc = hailo_probe(&v, &d);
        if (rc == HAILO_OK) {
            uart_printf("hailo: probe OK, vendor=0x%04x device=0x%04x, "
                        "state=%s\n", v, d,
                        hailo_state_str(hailo_get_state()));
        } else if (rc == HAILO_ERR_NODEV) {
            uart_puts("hailo: no device (probe returned NODEV)\n");
        } else {
            uart_printf("hailo: probe failed (%d)\n", rc);
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "fw") == 0) {
        uint32_t maj = 0, min = 0, rev = 0;
        int rc = hailo_get_firmware_version(&maj, &min, &rev);
        if (rc == HAILO_OK) {
            uart_printf("hailo: firmware %u.%u.%u\n", maj, min, rev);
        } else {
            uart_printf("hailo: firmware version unavailable (%d — "
                        "device must be booted first)\n", rc);
        }
        return 0;
    }

    /* Default: one-line status. */
    uart_printf("hailo: state=%s\n", hailo_state_str(hailo_get_state()));
    return 0;
}

static const shell_cmd_t hailo_cmd = {
    .name    = "hailo",
    .handler = cmd_hailo,
    .help    = "Hailo NPU status (hailo, hailo probe, hailo fw)",
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
