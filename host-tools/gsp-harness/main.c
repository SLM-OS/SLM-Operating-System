/*
 * main.c — CLI entry point for the Linux gsp-harness.
 *
 * Thin wrapper around the shared GSP-RM core (kernel/gpu/nvidia/).
 * Parses flags, calls into linux_gsp_platform_init, then dispatches
 * to the requested action.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../kernel/gpu/nvidia/nvidia_vbios.h"

extern int linux_gsp_platform_init(const char *pci_path, const char *chip,
                                   bool trace);

/* ---- Ampere BOOT_42 decode — standalone reproducing what
 * kernel/arch/x86_64/nvidia_gpu.c does, so the harness prints
 * identical information to the bare-metal boot log. ---- */
#define NV_PMC_BOOT_42  0xA00

static void print_boot42(uint32_t boot42)
{
    uint32_t chip_id       = (boot42 >> 20) & 0x3FF;
    uint32_t architecture  = (boot42 >> 24) & 0x3F;
    uint32_t implementation= (boot42 >> 20) & 0x0F;
    uint32_t major_rev     = (boot42 >> 16) & 0x0F;
    uint32_t minor_rev     = (boot42 >> 12) & 0x0F;
    (void)architecture; (void)implementation;
    printf("[GSP-HARNESS] BOOT_42: 0x%08x → arch=0x%02x chip=0x%03x rev %u.%u\n",
           boot42,
           (boot42 >> 24) & 0x3F,
           chip_id,
           major_rev, minor_rev);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"Actions (pick one):\n"
"  --probe          Map BARs, load firmware, read BOOT_42. Baseline check.\n"
"  --vbios          Read + parse VBIOS via /sys/.../rom. Reports BIT entries\n"
"                   and FWSEC presence (Turing+) without touching GSP.\n"
"  --phase N        Attempt GSP bringup phase N only (0..7).\n"
"  --bringup        Run full gsp_init() — phases 0 through 7.\n"
"\n"
"Options:\n"
"  --pci=PATH       PCI device sysfs path\n"
"                   (default: /sys/bus/pci/devices/0000:01:00.0)\n"
"  --chip=NAME      Chip codename for firmware lookup (default: ga107)\n"
"  --trace          Dump every BAR0 read/write to stderr\n"
"  --help           This help\n"
"\n"
"Typical use:\n"
"  sudo %s --probe            # baseline, should succeed\n"
"  sudo %s --bringup --trace  # full bringup with register trace\n",
    argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *pci_path = "/sys/bus/pci/devices/0000:01:00.0";
    const char *chip     = "ga107";
    bool trace           = false;
    enum { ACT_NONE, ACT_PROBE, ACT_VBIOS, ACT_PHASE, ACT_BRINGUP } action = ACT_NONE;
    int phase = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--probe") == 0) { action = ACT_PROBE; }
        else if (strcmp(a, "--vbios") == 0) { action = ACT_VBIOS; }
        else if (strcmp(a, "--bringup") == 0) { action = ACT_BRINGUP; }
        else if (strcmp(a, "--trace") == 0) { trace = true; }
        else if (strcmp(a, "--phase") == 0 && i + 1 < argc) {
            action = ACT_PHASE;
            phase = atoi(argv[++i]);
        }
        else if (strncmp(a, "--pci=", 6) == 0) { pci_path = a + 6; }
        else if (strncmp(a, "--chip=", 7) == 0) { chip = a + 7; }
        else {
            fprintf(stderr, "unknown argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (action == ACT_NONE) {
        fprintf(stderr, "no action specified\n");
        usage(argv[0]);
        return 2;
    }

    if (linux_gsp_platform_init(pci_path, chip, trace) < 0) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }

    extern const struct gsp_platform_ops *gsp_platform;

    switch (action) {
    case ACT_PROBE: {
        uint32_t boot42 = gsp_platform->read32(NV_PMC_BOOT_42);
        print_boot42(boot42);
        /* Engine register probe (PGRAPH at 0x400000). Pre-GSP, this
         * returns the "locked" poison pattern. */
        uint32_t eng = gsp_platform->read32(0x400000);
        printf("[GSP-HARNESS] engine register @0x400000 = 0x%08x%s\n",
               eng,
               eng == 0xBADF5040 ? " (GSP not loaded — expected)" : "");
        return 0;
    }
    case ACT_VBIOS: {
        const uint8_t *raw = NULL;
        size_t raw_size = 0;
        if (nvidia_vbios_platform_load(&raw, &raw_size) < 0) {
            fprintf(stderr, "[GSP-HARNESS] VBIOS load failed\n");
            return 1;
        }
        printf("[GSP-HARNESS] VBIOS image: %zu bytes @ %p\n",
               raw_size, (const void *)raw);
        /* Re-parse the externally-visible struct so we can print the
         * BIT-table summary alongside any FWSEC payload. The raw bytes
         * loaded above are owned by linux_platform.c and stay alive. */
        struct nvidia_vbios vb;
        if (nvidia_vbios_parse(raw, raw_size, &vb) < 0) {
            fprintf(stderr, "[GSP-HARNESS] VBIOS reparse failed\n");
            return 1;
        }
        printf("[GSP-HARNESS] BIT @0x%x: hdr_size=%u entry_size=%u entries=%u\n",
               vb.bit_offset, vb.hdr_size, vb.entry_size, vb.num_entries);

        /* Sub-image map — useful for E2.5 debugging. */
        printf("[GSP-HARNESS] sub-images (%u):\n", vb.num_subimages);
        for (uint8_t i = 0; i < vb.num_subimages; i++) {
            const struct nvidia_vbios_subimage *si = &vb.subimages[i];
            const char *n =
                (si->code_type == VBIOS_CODE_TYPE_X86)       ? "PciAt (x86 legacy)" :
                (si->code_type == VBIOS_CODE_TYPE_EFI)       ? "EFI" :
                (si->code_type == VBIOS_CODE_TYPE_VBIOS_EXT) ? "FwSec (VBIOS_EXT)" :
                "other";
            printf("              [%u] @0x%06x  code_type=0x%02x (%-20s)  len=%u\n",
                   i, si->offset, si->code_type, n, si->length);
        }

        const void *fw = NULL; size_t fw_size = 0;
        if (gsp_platform->vbios_get_fwsec(&fw, &fw_size) == 0) {
            printf("[GSP-HARNESS] FWSEC: %zu bytes @ %p\n", fw_size, fw);
        } else {
            /* FWSEC discovery now implements the full nova-core path
             * (BIT 'p' → FalconUcodeTablePtr → PciAt|FwSec1|FwSec2
             * concatenated offset → PMU table → FWSEC_PROD entry →
             * FalconUCodeDescV3 header → payload size). Real failure
             * cause on Ampere dumped via /sys/.../rom or /dev/mem:
             * the ROM BAR caps at 512 KB but the VBIOS declares a
             * larger image, so the pointer lands past our truncated
             * dump. ACPI _ROM or VFIO ROM ioctl gives the full image
             * — follow-up issue tracks that. */
            printf("[GSP-HARNESS] FWSEC: not extractable from this image\n"
                   "                  (possible causes: Pascal-era card with no\n"
                   "                   FwSec entries, missing BIT 'p' entry, or\n"
                   "                   truncated ROM dump — on GA10x, the ROM BAR\n"
                   "                   often caps at 512 KB even when the VBIOS\n"
                   "                   declares more. Use ACPI _ROM or a full VFIO\n"
                   "                   ROM read to get the rest.)\n");
        }
        return 0;
    }
    case ACT_PHASE: {
        /* Phase-by-phase stepping — the implementation in gsp.c
         * will expose a per-phase entry once E2+ land. For now the
         * only valid phase is 0 (firmware manifest check), and
         * that's implicit in gsp_init's first step. */
        if (phase != 0) {
            fprintf(stderr, "phase %d not yet available (E2+ adds 1..7)\n",
                    phase);
            return 2;
        }
        int rc = gsp_init();
        printf("[GSP-HARNESS] gsp_init() returned %d, state=%d, last_err_phase=%d\n",
               rc, (int)gsp_get_state(), gsp_last_error_phase());
        return rc == 0 ? 0 : 1;
    }
    case ACT_BRINGUP: {
        int rc = gsp_init();
        if (rc == 0) {
            printf("[GSP-HARNESS] GSP-RM running ✓ (state=%d)\n",
                   (int)gsp_get_state());
            return 0;
        }
        printf("[GSP-HARNESS] bringup failed at phase %d (state=%d)\n",
               gsp_last_error_phase(), (int)gsp_get_state());
        return 1;
    }
    default:
        return 2;
    }
}
