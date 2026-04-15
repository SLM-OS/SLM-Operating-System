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
#include "../../kernel/gpu/nvidia/falcon.h"
#include "../../kernel/gpu/nvidia/nvfw.h"
#include "../../kernel/gpu/nvidia/bringup.h"

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
"  --vbios          Read + parse VBIOS via BAR0 PROM window. Reports BIT\n"
"                   entries and FWSEC presence (Turing+) without touching GSP.\n"
"  --falcons        Probe GSP + SEC2 Falcon engines: IMEM/DMEM sizes, halt\n"
"                   state, RISC-V capability. Hardware smoke test for E3.\n"
"  --dma-test       Allocate + IOMMU-map + free a DMA buffer via VFIO.\n"
"                   Confirms E3.2 DMA plumbing works end-to-end.\n"
"  --fwsec-frts     Run FWSEC-FRTS on GSP Falcon. First real GSP-RM\n"
"                   bringup step — sets up the WPR2 region in FB.\n"
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
    enum { ACT_NONE, ACT_PROBE, ACT_VBIOS, ACT_FALCONS, ACT_DMA_TEST,
           ACT_FWSEC_FRTS, ACT_PHASE, ACT_BRINGUP } action = ACT_NONE;
    int phase = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--probe") == 0) { action = ACT_PROBE; }
        else if (strcmp(a, "--vbios") == 0) { action = ACT_VBIOS; }
        else if (strcmp(a, "--falcons") == 0) { action = ACT_FALCONS; }
        else if (strcmp(a, "--dma-test") == 0) { action = ACT_DMA_TEST; }
        else if (strcmp(a, "--fwsec-frts") == 0) { action = ACT_FWSEC_FRTS; }
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
            /* FWSEC discovery implements the full nova-core path
             * (BIT 'p' → FalconUcodeTablePtr → PciAt|FwSec1|FwSec2
             * concatenated offset → PMU table → FWSEC_PROD entry →
             * FalconUCodeDescV3 header → payload size). Common failure
             * cause on GA107: NPDS declares FwSec2 length larger than
             * what fits in the 512 KB ROM BAR, so the pointer resolves
             * past the bytes the BAR exposes. This isn't a Linux
             * truncation (we read via /dev/mem to bypass kernel-side
             * caps); it's that the GPU literally only exposes 512 KB
             * of its ~568 KB SPI-flash VBIOS through the ROM BAR. The
             * remaining 56 KB lives in flash regions reachable only
             * via chip-specific paths — see #150. */
            printf("[GSP-HARNESS] FWSEC: not extractable from this image\n"
                   "                  (possible causes: Pascal-era card with no\n"
                   "                   FwSec entries, missing BIT 'p' entry, or — most\n"
                   "                   common on GA107 — the GPU's ROM BAR exposes only\n"
                   "                   the first ~512 KB of an >568 KB VBIOS, and FWSEC\n"
                   "                   lives in the missing tail. See issue #150 for the\n"
                   "                   work to read the rest via PRAMIN / VRAM shadow.)\n");
        }
        return 0;
    }
    case ACT_FALCONS: {
        /* Hardware smoke test for the Falcon v4 register map in
         * kernel/gpu/nvidia/falcon.c — probes both engines and
         * reports what they look like on real Ampere silicon.
         * Read-only; safe to run without affecting GSP state. */
        struct falcon gsp_flcn, sec2_flcn;

        if (falcon_probe(&gsp_flcn, NV_PGSP_BASE) < 0) {
            printf("[GSP-HARNESS] GSP Falcon probe failed\n");
        } else {
            printf("[GSP-HARNESS] GSP Falcon @0x%08x  IMEM=%u KB  DMEM=%u KB  RISC-V=%s  idle=%s\n",
                   gsp_flcn.base,
                   gsp_flcn.imem_size / 1024,
                   gsp_flcn.dmem_size / 1024,
                   gsp_flcn.has_riscv ? "yes" : "no",
                   falcon_is_idle(&gsp_flcn) ? "yes" : "no");
        }

        if (falcon_probe(&sec2_flcn, NV_PSEC2_BASE) < 0) {
            printf("[GSP-HARNESS] SEC2 Falcon probe failed\n");
        } else {
            printf("[GSP-HARNESS] SEC2 Falcon @0x%08x  IMEM=%u KB  DMEM=%u KB  RISC-V=%s  idle=%s\n",
                   sec2_flcn.base,
                   sec2_flcn.imem_size / 1024,
                   sec2_flcn.dmem_size / 1024,
                   sec2_flcn.has_riscv ? "yes" : "no",
                   falcon_is_idle(&sec2_flcn) ? "yes" : "no");
        }
        return 0;
    }
    case ACT_DMA_TEST: {
        /* E3.2 DMA plumbing smoke test: allocate + IOMMU-map + free
         * three buffers of different sizes, write a recognizable
         * pattern into each, and confirm the DMA address we get is
         * reasonable (IOMMU-space, not a raw VA).
         *
         * We can't directly confirm the GPU can read our buffer
         * without actually programming Falcon DMA (that's E3.4 work).
         * What this test CAN confirm: the VFIO session opens, the
         * Type1 IOMMU accepts our maps, and the IOVAs come back in
         * the expected 0x10000000+ range. */
        struct {
            size_t size;
            size_t align;
        } cases[] = {
            { 4096,       0 },
            { 64 * 1024,  256 },
            { 1024 * 1024, 4096 },
        };
        int ncases = (int)(sizeof(cases) / sizeof(cases[0]));

        int failed = 0;
        for (int i = 0; i < ncases; i++) {
            uint64_t iova = 0;
            void *va = gsp_platform->dma_alloc(cases[i].size, cases[i].align, &iova);
            if (!va) {
                printf("[GSP-HARNESS] dma_alloc(%zu) FAILED\n", cases[i].size);
                failed++;
                continue;
            }
            if (iova < 0x10000000ull) {
                printf("[GSP-HARNESS] dma_alloc(%zu): iova 0x%lx looks like a raw VA\n"
                       "                             (VFIO probably unavailable — "
                       "see earlier init line)\n",
                       cases[i].size, (unsigned long)iova);
                failed++;
            } else {
                printf("[GSP-HARNESS] dma_alloc(%zu, align=%zu): va=%p iova=0x%lx\n",
                       cases[i].size, cases[i].align, va, (unsigned long)iova);
            }
            /* Light write+read check that the buffer is writable. */
            uint8_t *bytes = (uint8_t *)va;
            bytes[0] = 0xA5;
            bytes[cases[i].size - 1] = 0x5A;
            if (bytes[0] != 0xA5 || bytes[cases[i].size - 1] != 0x5A) {
                printf("[GSP-HARNESS] buffer not host-writable!\n");
                failed++;
            }
            gsp_platform->dma_free(va, cases[i].size);
        }

        if (failed) {
            printf("[GSP-HARNESS] --dma-test: %d/%d FAILED\n", failed, ncases);
            return 1;
        }
        printf("[GSP-HARNESS] --dma-test: all %d cases PASS\n", ncases);
        return 0;
    }
    case ACT_FWSEC_FRTS: {
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            printf("[GSP-HARNESS] bringup prepare FAILED\n");
            return 1;
        }
        printf("[GSP-HARNESS] FWSEC ucode: imem=%u bytes dmem=%u bytes\n"
               "                engine_id=0x%x ucode_id=%u pkc_data_off=0x%x\n"
               "                imem_virt_base=0x%x interface_off=0x%x\n",
               b.fwsec_imem_size, b.fwsec_dmem_size,
               b.fwsec_engine_id, b.fwsec_ucode_id, b.fwsec_pkc_data_off,
               b.fwsec_imem_virt_base, b.fwsec_interface_offset);
        printf("[GSP-HARNESS] WPR2 target: addr=0x%llx size=0x%llx\n",
               (unsigned long long)b.wpr2_addr,
               (unsigned long long)b.wpr2_size);

        int rc = gsp_bringup_fwsec_frts(&b);
        if (b.diag_sig_count) {
            printf("[GSP-HARNESS] sig selection: fuse_reg[0x%x]=0x%x sig_count=%u\n"
                   "                sig_versions=0x%x → sig_index=%u\n",
                   b.diag_fuse_reg_off, b.diag_fuse_reg_val,
                   b.diag_sig_count, b.diag_sig_versions, b.diag_sig_index);
        }
        if (rc < 0) {
            printf("[GSP-HARNESS] FWSEC-FRTS FAILED at phase %u\n",
                   b.last_error_phase);
            uint32_t err    = gsp_platform->read32(0x00001438);
            uint32_t wpr_lo = gsp_platform->read32(0x001fa824);
            uint32_t wpr_hi = gsp_platform->read32(0x001fa828);
            printf("                FWSEC err reg = 0x%08x (err_code=%u)\n",
                   err, err >> 16);
            printf("                WPR2 lo = 0x%08x  hi = 0x%08x\n", wpr_lo, wpr_hi);

            /* Dump GSP Falcon state to show whether BROM rejected
             * the signature, the ucode is looping, or DMA/TRFCFG
             * didn't fire. */
            uint32_t cpuctl  = gsp_platform->read32(0x00110100);
            uint32_t mbox0   = gsp_platform->read32(0x00110040);
            uint32_t mbox1   = gsp_platform->read32(0x00110044);
            uint32_t irqstat = gsp_platform->read32(0x00110008);
            uint32_t hwcfg2  = gsp_platform->read32(0x001100f4);
            uint32_t bcrctl  = gsp_platform->read32(0x00111668);
            uint32_t modsel  = gsp_platform->read32(0x00111180);
            uint32_t paraaddr= gsp_platform->read32(0x00111210);
            printf("                GSP Falcon state:\n");
            printf("                  CPUCTL=0x%08x (halted=%d, started=%d)\n",
                   cpuctl, !!(cpuctl & 0x10), !(cpuctl & 0x10));
            printf("                  MAILBOX0=0x%08x MAILBOX1=0x%08x\n", mbox0, mbox1);
            printf("                  IRQSTAT=0x%08x (halt=%d, swgen0=%d)\n",
                   irqstat, !!(irqstat & 0x10), !!(irqstat & 0x40));
            printf("                  HWCFG2=0x%08x BCR_CTRL=0x%08x\n", hwcfg2, bcrctl);
            printf("                  MOD_SEL=0x%08x PARAADDR0=0x%08x\n", modsel, paraaddr);
            return 1;
        }
        printf("[GSP-HARNESS] FWSEC-FRTS ok — WPR2 registers:\n");
        uint32_t wpr_lo = gsp_platform->read32(0x001fa824);
        uint32_t wpr_hi = gsp_platform->read32(0x001fa828);
        printf("                WPR2_LO = 0x%08x\n                WPR2_HI = 0x%08x\n",
               wpr_lo, wpr_hi);
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
