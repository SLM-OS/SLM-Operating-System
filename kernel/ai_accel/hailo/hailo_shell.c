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
            shell_printf("hailo: probe OK, vendor=0x%04x device=0x%04x, "
                        "state=%s\n", v, d,
                        hailo_state_str(hailo_get_state()));
        } else if (rc == HAILO_ERR_NODEV) {
            shell_puts("hailo: no device (probe returned NODEV)\n");
        } else {
            shell_printf("hailo: probe failed (%d)\n", rc);
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "cfgdump") == 0) {
#if defined(PLATFORM_RASPI5)
        /* PCIe1 EXT_CFG_INDEX/DATA are hardwired to BCM2712's pcie1 RC;
         * the addresses are only valid on Pi 5. */
        volatile uint32_t *idx  = (volatile uint32_t *)0x1000119000UL;
        volatile uint8_t  *data = (volatile uint8_t  *)0x1000119004UL;
        uart_puts("Re-program INDEX between EACH read (bus 1 dev 0 func 0):\n");
        for (uint32_t off = 0; off < 0x40; off += 4) {
            *idx = 0x00100000u;
            __asm__ volatile("dsb sy" ::: "memory");
            uint32_t v = *(volatile uint32_t *)(data + off);
            uart_printf("  [0x%02x]=0x%08lx\n", off, (unsigned long)v);
        }
        uart_puts("\nRead first 4 dwords WITHOUT re-programming INDEX (INDEX kept at 0x100000):\n");
        *idx = 0x00100000u;
        __asm__ volatile("dsb sy" ::: "memory");
        for (uint32_t off = 0; off < 0x20; off += 4) {
            uint32_t v = *(volatile uint32_t *)(data + off);
            uart_printf("  [0x%02x]=0x%08lx\n", off, (unsigned long)v);
        }
#else
        uart_puts("hailo: cfgdump is Pi 5-only (requires BCM2712 pcie1 RC)\n");
#endif
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "fw") == 0) {
        uint32_t maj = 0, min = 0, rev = 0;
        int rc = hailo_get_firmware_version(&maj, &min, &rev);
        if (rc == HAILO_OK) {
            shell_printf("hailo: firmware %u.%u.%u\n", maj, min, rev);
        } else {
            shell_printf("hailo: firmware version unavailable (%d — "
                        "device must be booted first)\n", rc);
        }
        return 0;
    }

    /* Default: one-line status. */
    shell_printf("hailo: state=%s\n", hailo_state_str(hailo_get_state()));
    return 0;
}

static const shell_cmd_t hailo_cmd = {
    .name    = "hailo",
    .handler = cmd_hailo,
    .help    = "Hailo NPU status (hailo, hailo probe, hailo fw)",
    .mutates = true,   /* probe/fw mutate driver state; status is a whole-command tag */
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
