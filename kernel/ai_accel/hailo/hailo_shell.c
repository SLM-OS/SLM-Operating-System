/*
 * hailo_shell.c — `hailo` shell command.
 *
 * Usage:
 *   hailo          — dump driver state, IDs, BAR map.
 *   hailo probe    — re-run hailo_probe and print the result.
 *   hailo boot     — upload embedded firmware and bring the NPU to RUNNING.
 *   hailo fw       — report firmware version (post-boot only).
 *   hailo cfgdump  — (Pi 5 only) raw 64-byte bus 1 config dump.
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

/*
 * Firmware blob linked in at build time via the CMake HAILO_FW_BLOB
 * option (default: no blob; `hailo boot` reports "firmware not
 * embedded"). When set, CMakeLists wraps the file with
 * `.incbin` + `hailo_fw_start`/`hailo_fw_end` symbols. Declared weak
 * here so default builds link cleanly without a strong definition.
 */
extern const uint8_t hailo_fw_start[] __attribute__((weak));
extern const uint8_t hailo_fw_end[]   __attribute__((weak));

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
        /* PCIe1 EXT_CFG_INDEX at RC_base + 0x9000, EXT_CFG_DATA at
         * RC_base + 0x8000 (NOT 0x9004 — see pcie_bcm2712.c comment
         * on PCIE1_EXT_CFG_DATA for why the variant table's 0x9004
         * is vestigial and only 0x8000 returns real data past
         * config offset 0x07). */
        volatile uint32_t *idx  = (volatile uint32_t *)0x1000119000UL;
        volatile uint8_t  *data = (volatile uint8_t  *)0x1000118000UL;
        uart_puts("Dumping bus 1 dev 0 func 0 config (INDEX pinned):\n");
        *idx = 0x00100000u;
        __asm__ volatile("dsb sy" ::: "memory");
        for (uint32_t off = 0; off < 0x40; off += 4) {
            uint32_t v = *(volatile uint32_t *)(data + off);
            uart_printf("  [0x%02x]=0x%08lx\n", off, (unsigned long)v);
        }
#else
        uart_puts("hailo: cfgdump is Pi 5-only (requires BCM2712 pcie1 RC)\n");
#endif
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "boot") == 0) {
        /* Cast to (const void *) before comparing — treating extern
         * char[] symbols as arrays trips -Warray-compare. The weak
         * symbols resolve to NULL when no blob is linked. */
        const void *fw_lo = (const void *)hailo_fw_start;
        const void *fw_hi = (const void *)hailo_fw_end;
        if (!fw_lo || !fw_hi || fw_hi <= fw_lo) {
            shell_puts("hailo: firmware not embedded — rebuild with "
                       "-DHAILO_FW_BLOB=path/to/hailo8_fw.bin\n");
            return 0;
        }
        size_t fw_size = (size_t)((const uint8_t *)fw_hi
                                - (const uint8_t *)fw_lo);
        shell_printf("hailo: booting from embedded firmware (%lu bytes)...\n",
                     (unsigned long)fw_size);
        int rc = hailo_boot(fw_lo, fw_size);
        if (rc == HAILO_OK) {
            shell_printf("hailo: boot OK, state=%s\n",
                         hailo_state_str(hailo_get_state()));
        } else {
            shell_printf("hailo: boot failed (%d), state=%s\n", rc,
                         hailo_state_str(hailo_get_state()));
        }
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
    .help    = "Hailo NPU control (hailo, probe, boot, fw, cfgdump)",
    .mutates = true,   /* probe/fw mutate driver state; status is a whole-command tag */
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
