/*
 * hailo_shell.c — `hailo` shell command.
 *
 * Usage:
 *   hailo          — dump driver state, IDs, BAR map.
 *   hailo probe    — re-run hailo_probe and print the result.
 *   hailo boot     — upload embedded firmware and bring the NPU to RUNNING.
 *   hailo load P   — read a `.hef` model at VFS path P and dump metadata.
 *   hailo fw       — report firmware version (post-boot only).
 *   hailo cfgdump  — (Pi 5 only) raw 64-byte bus 1 config dump.
 *
 * Safe to run on any platform. On non-RASPI5 builds the driver is
 * never installed (stub returns -ENODEV) so `hailo` just reports
 * "driver not available".
 */

#include "hailo.h"
#include "hef_header.h"
#include "hef_parser.h"
#include "pmm.h"
#include "shell.h"
#include "uart.h"
#include "vfs.h"
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

/*
 * Upper bound for the proto body the `hailo load` shell path will
 * decode. hef_header.c's HEF_PROTO_MAX_SIZE stays at 256 MB as the
 * schema bound, but the shell refuses anything above this cap so a
 * corrupted-but-magic-matching .hef can't commandeer hundreds of MB
 * of RAM via a fake proto size. 16 MB covers every compiled Hailo
 * Model Zoo entry today (yolov5m is the largest at ~17 MB total
 * file; its proto body is ~2 MB).
 */
#define HAILO_LOAD_MAX_PROTO_MB  16u

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
            shell_puts("hailo: firmware not embedded -- rebuild with "
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

    if (argc >= 2 && strcmp(argv[1], "load") == 0) {
        if (argc < 3) {
            shell_puts("usage: hailo load <vfs-path>\n");
            return 0;
        }
        const char *path = argv[2];

        struct vfs_entry_info info = {0};
        if (vfs_stat_path(path, &info) != 0) {
            shell_printf("hailo: stat '%s' failed\n", path);
            return 0;
        }
        if (info.size < 12) {
            shell_printf("hailo: '%s' too small (%lu bytes)\n",
                         path, (unsigned long)info.size);
            return 0;
        }

        /* Two-pass read: the outer header (up to 32 bytes) tells us
         * the proto body size, which may be multi-MB. Stage 1 pulls
         * just the header into a stack buffer; stage 2 PMM-allocates
         * the body. */
        uint8_t hdr_buf[64];
        size_t hdr_read = info.size < sizeof(hdr_buf) ? info.size : sizeof(hdr_buf);
        int n = vfs_read_path(path, (char *)hdr_buf, hdr_read, 0);
        if (n < 0) {
            shell_printf("hailo: read '%s' failed\n", path);
            return 0;
        }

        struct hef_outer_header outer = {0};
        int rc = hef_parse_outer_header(hdr_buf, (size_t)n, &outer);
        if (rc != HEF_OK) {
            shell_printf("hailo: outer-header parse failed (%d)\n", rc);
            return 0;
        }
        shell_printf("hailo: hef v%u proto_size=%u (total %lu bytes)\n",
                     outer.version, outer.proto_size,
                     (unsigned long)info.size);

        if (outer.proto_size > HAILO_LOAD_MAX_PROTO_MB * 1024u * 1024u) {
            shell_printf("hailo: refusing to load proto body of %u bytes "
                         "(> %u MB shell cap; if this is legitimate, raise "
                         "HAILO_LOAD_MAX_PROTO_MB in hailo_shell.c)\n",
                         outer.proto_size, HAILO_LOAD_MAX_PROTO_MB);
            return 0;
        }

        /* Allocate a contiguous page-aligned buffer for the proto
         * body. PMM rounds up to the next power-of-2 page count. */
        size_t body_pages = (outer.proto_size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (body_pages == 0) body_pages = 1;
        void *body = pmm_alloc_pages(body_pages);
        if (!body) {
            shell_printf("hailo: pmm_alloc_pages(%lu) failed for proto body\n",
                         (unsigned long)body_pages);
            return 0;
        }

        n = vfs_read_path(path, (char *)body, outer.proto_size,
                          outer.proto_offset);
        if (n < 0 || (uint32_t)n < outer.proto_size) {
            shell_printf("hailo: read proto body failed (%d of %u)\n",
                         n, outer.proto_size);
            pmm_free_pages(body, body_pages);
            return 0;
        }

        struct hef_info meta;
        rc = hef_parse_body(body, outer.proto_size, &meta);
        pmm_free_pages(body, body_pages);

        if (rc != HEF_PARSER_OK) {
            shell_printf("hailo: proto decode failed (%d)\n", rc);
            return 0;
        }

        shell_printf("  hw_arch = %s (%u)\n",
                     meta.hw_arch_known ?
                         (meta.hw_arch == HEF_HW_ARCH_HAILO8   ? "hailo8"  :
                          meta.hw_arch == HEF_HW_ARCH_HAILO8L  ? "hailo8l" :
                          meta.hw_arch == HEF_HW_ARCH_HAILO15H ? "hailo15h":
                          meta.hw_arch == HEF_HW_ARCH_HAILO15M ? "hailo15m":
                          meta.hw_arch == HEF_HW_ARCH_HAILO10H ? "hailo10h":
                          "unknown") : "absent",
                     meta.hw_arch);
        if (meta.sdk_version[0]) {
            shell_printf("  sdk_version = %s\n", meta.sdk_version);
        }
        shell_printf("  network_groups = %u\n", meta.network_group_count);
        if (meta.first_network_group[0]) {
            shell_printf("  first network group = %s\n",
                         meta.first_network_group);
        }
        if (meta.string_truncated) {
            shell_printf("  (note: at least one string was truncated "
                         "at %u bytes)\n", (unsigned)HEF_PARSER_MAX_STR);
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
    .help    = "Hailo NPU control (hailo, probe, boot, load <path>, fw, cfgdump)",
    .mutates = true,   /* probe/fw mutate driver state; status is a whole-command tag */
};

void hailo_register_shell_commands(void)
{
    shell_register_command(&hailo_cmd);
}
