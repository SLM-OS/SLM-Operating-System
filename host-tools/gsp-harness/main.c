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
#include <time.h>

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
"  --fwsec-sb       Probe variant: run FWSEC with init_cmd=SB (0x19)\n"
"                   instead of FRTS (0x15). SB is a subsequent-boot\n"
"                   lifecycle no-op that halts without writing WPR2.\n"
"                   Use to bisect whether a hang is FRTS-specific or\n"
"                   upstream of init_cmd dispatch. See handoff §4.1.\n"
"  --fwsec-trace    Kick FWSEC-FRTS and sample DEBUGINFO / MAILBOX0 /\n"
"                   CPUCTL / OS at 1/10/50/200/500/1000/2000 ms to\n"
"                   disambiguate 'stuck at one instruction' from\n"
"                   'slow progress'. See handoff §4.2.\n"
"  --check-devinit  Read the NV_PGC6_AON_SECURE_SCRATCH_GROUP_05\n"
"                   registers (BAR0+0x118128 / +0x118234) to decide\n"
"                   whether the GPU's VBIOS DEVINIT has completed.\n"
"                   Matches nouveau's tu102_devinit_wait check. If\n"
"                   DEVINIT hasn't run, FWSEC-FRTS is expected to\n"
"                   hang/abort without progress. See handoff §0.4.\n"
"  --booter-load    Run Booter Load on SEC2 (E3.4.d). Requires that\n"
"                   --fwsec-frts succeeded; reuses the WPR2 setup.\n"
"  --riscv-start    Flip GSP into RISC-V mode and start the core\n"
"                   (E3.4.e). Requires Booter Load completed.\n"
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
    /* Disable stdout buffering — when run via SSH, the pipe makes
     * stdout block-buffered and progress prints disappear until
     * buffer flush, which hides hang locations in diagnostics. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *pci_path = "/sys/bus/pci/devices/0000:01:00.0";
    const char *chip     = "ga107";
    bool trace           = false;
    enum { ACT_NONE, ACT_PROBE, ACT_VBIOS, ACT_FALCONS, ACT_DMA_TEST,
           ACT_FWSEC_FRTS, ACT_FWSEC_SB, ACT_FWSEC_TRACE,
           ACT_CHECK_DEVINIT, ACT_BOOTER_LOAD, ACT_RISCV_START,
           ACT_PHASE, ACT_BRINGUP } action = ACT_NONE;
    int phase = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--probe") == 0) { action = ACT_PROBE; }
        else if (strcmp(a, "--vbios") == 0) { action = ACT_VBIOS; }
        else if (strcmp(a, "--falcons") == 0) { action = ACT_FALCONS; }
        else if (strcmp(a, "--dma-test") == 0) { action = ACT_DMA_TEST; }
        else if (strcmp(a, "--fwsec-frts") == 0)  { action = ACT_FWSEC_FRTS; }
        else if (strcmp(a, "--fwsec-sb") == 0)    { action = ACT_FWSEC_SB; }
        else if (strcmp(a, "--fwsec-trace") == 0) { action = ACT_FWSEC_TRACE; }
        else if (strcmp(a, "--check-devinit") == 0) { action = ACT_CHECK_DEVINIT; }
        else if (strcmp(a, "--booter-load") == 0) { action = ACT_BOOTER_LOAD; }
        else if (strcmp(a, "--riscv-start") == 0) { action = ACT_RISCV_START; }
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

    /* vfio-pci does an FLR when userspace opens /dev/vfio/GROUP.
     * FLR resets the GPU and clears DEVINIT state. The on-chip BSI
     * (Bootstrap Sequencer Instruction) re-runs DEVINIT from the
     * VBIOS after FLR, but takes ~500 ms to complete. Poll the
     * NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0] byte-0 == 0xff "DEVINIT
     * done" marker (nouveau's tu102_devinit_wait) before letting any
     * action proceed. Actions that don't touch bringup (--probe,
     * --vbios, --falcons, --dma-test, --check-devinit) are exempt. */
    bool needs_devinit = (action == ACT_FALCONS ||
                          action == ACT_FWSEC_FRTS ||
                          action == ACT_FWSEC_SB ||
                          action == ACT_FWSEC_TRACE ||
                          action == ACT_BOOTER_LOAD ||
                          action == ACT_RISCV_START ||
                          action == ACT_BRINGUP ||
                          action == ACT_PHASE);
    if (needs_devinit) {
        /* Poll NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0] byte 0 for
         * 0xff (nouveau tu102_devinit_wait). BSI recovery after FLR
         * takes ~500 ms on GA107 test-pc; 2 s cap is generous. */
        const uint32_t budget_ms = 2000;
        uint32_t waited_ms = 0;
        uint32_t s = 0;
        while (waited_ms <= budget_ms) {
            s = gsp_platform->read32(0x00118234);
            if ((s & 0xffu) == 0xffu) break;
            struct timespec st = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L };
            nanosleep(&st, NULL);
            waited_ms += 50;
        }
        if ((s & 0xffu) == 0xffu) {
            fprintf(stderr,
                    "[GSP-HARNESS] BSI DEVINIT recovered in %u ms\n", waited_ms);
        } else {
            fprintf(stderr,
                    "[GSP-HARNESS] WARNING: BSI DEVINIT not complete in %u ms "
                    "(scratch=0x%08x); continuing anyway\n",
                    budget_ms, s);
        }
    }

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
    case ACT_CHECK_DEVINIT: {
        /* Matches nouveau tu102_devinit_wait (drivers/gpu/drm/nouveau/
         * nvkm/subdev/devinit/tu102.c). On GA10x, 0x118128 is
         * NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK and
         * 0x118234 is NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0]. Byte-0
         * is written to 0xff by the VBIOS init-script engine at the
         * end of POST/DEVINIT on Turing+.
         *
         * vfio-pci does a PCI FLR when /dev/vfio/GROUP is opened,
         * which clears this scratch. GA107's on-chip BSI (Bootstrap
         * Sequencer Instruction) re-runs DEVINIT from VBIOS after
         * FLR but takes ~500 ms. Poll for up to 2 s to accommodate
         * that recovery. */
        uint32_t plm = 0, scr0 = 0;
        uint32_t waited_ms = 0;
        const uint32_t budget_ms = 2000;
        while (waited_ms <= budget_ms) {
            plm  = gsp_platform->read32(0x00118128);
            scr0 = gsp_platform->read32(0x00118234);
            if ((plm & 0x1u) && (scr0 & 0xffu) == 0xffu) break;
            struct timespec st = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000L };
            nanosleep(&st, NULL);
            waited_ms += 50;
        }
        bool plm_ok   = (plm & 0x1u) != 0;
        bool scr0_ok  = (scr0 & 0xffu) == 0xffu;
        printf("[GSP-HARNESS] DEVINIT check (after waiting %u ms for BSI):\n", waited_ms);
        printf("                0x118128 (GR5 PLM)           = 0x%08x  (bit 0 %s)\n",
               plm,  plm_ok  ? "SET  — scratch readable" : "CLR  — blocked by priv-level");
        printf("                0x118234 (GR5_SCRATCH[0])    = 0x%08x  (byte 0 = 0x%02x %s)\n",
               scr0, scr0 & 0xffu,
               scr0_ok ? "— DEVINIT done" : "— DEVINIT NOT done / not run");
        if (plm_ok && scr0_ok) {
            printf("[GSP-HARNESS] Verdict: VBIOS DEVINIT has completed on this GPU.\n");
            return 0;
        }
        printf("[GSP-HARNESS] Verdict: VBIOS DEVINIT has NOT completed within %u ms.\n"
               "                Either UEFI never ran DEVINIT (GPU isn't the primary\n"
               "                display) or the BSI re-run after FLR didn't finish.\n",
               budget_ms);
        return 2;
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
    case ACT_FWSEC_FRTS:
    case ACT_FWSEC_SB: {
        const bool is_sb = (action == ACT_FWSEC_SB);
        const char *label = is_sb ? "FWSEC-SB" : "FWSEC-FRTS";
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            printf("[GSP-HARNESS] bringup prepare FAILED\n");
            return 1;
        }
        if (is_sb) b.init_cmd = GSP_DMEMMAPPER_CMD_SB;
        printf("[GSP-HARNESS] FWSEC ucode: imem=%u bytes dmem=%u bytes\n"
               "                engine_id=0x%x ucode_id=%u pkc_data_off=0x%x\n"
               "                imem_virt_base=0x%x interface_off=0x%x\n",
               b.fwsec_imem_size, b.fwsec_dmem_size,
               b.fwsec_engine_id, b.fwsec_ucode_id, b.fwsec_pkc_data_off,
               b.fwsec_imem_virt_base, b.fwsec_interface_offset);
        printf("[GSP-HARNESS] init_cmd: 0x%02x (%s)%s\n",
               b.init_cmd, label,
               is_sb ? "  — probe, WPR2 not expected to populate" : "");
        if (!is_sb) {
            printf("[GSP-HARNESS] WPR2 target: addr=0x%llx size=0x%llx\n",
                   (unsigned long long)b.wpr2_addr,
                   (unsigned long long)b.wpr2_size);
        }

        int rc = gsp_bringup_fwsec_frts(&b);
        if (b.diag_sig_count) {
            printf("[GSP-HARNESS] sig selection: fuse_reg[0x%x]=0x%x sig_count=%u\n"
                   "                sig_versions=0x%x → sig_index=%u\n",
                   b.diag_fuse_reg_off, b.diag_fuse_reg_val,
                   b.diag_sig_count, b.diag_sig_versions, b.diag_sig_index);
        }
        if (rc < 0) {
            printf("[GSP-HARNESS] %s FAILED at phase %u (rc=%d)\n",
                   label, b.last_error_phase, rc);
            uint32_t err    = gsp_platform->read32(NV_FWSEC_FRTS_ERR_REG);
            uint32_t wpr_lo = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
            uint32_t wpr_hi = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
            printf("                FWSEC err reg = 0x%08x (err_code=%u)\n",
                   err, err >> 16);
            printf("                WPR2 lo = 0x%08x  hi = 0x%08x\n", wpr_lo, wpr_hi);

            /* Dump GSP Falcon state to show whether BROM rejected
             * the signature, the ucode is looping, or DMA/TRFCFG
             * didn't fire. All offsets resolve via the named falcon
             * + RISC-V PRI base + register constants — no magic. */
            uint32_t cpuctl  = gsp_platform->read32(NV_PGSP_BASE       + FALCON_CPUCTL);
            uint32_t mbox0   = gsp_platform->read32(NV_PGSP_BASE       + FALCON_MAILBOX0);
            uint32_t mbox1   = gsp_platform->read32(NV_PGSP_BASE       + FALCON_MAILBOX1);
            uint32_t irqstat = gsp_platform->read32(NV_PGSP_BASE       + FALCON_IRQSTAT);
            uint32_t hwcfg2  = gsp_platform->read32(NV_PGSP_BASE       + FALCON_HWCFG2);
            uint32_t bcrctl  = gsp_platform->read32(NV_PGSP_RISCV_BASE + FALCON_RISCV_BCR_CTRL);
            uint32_t modsel  = gsp_platform->read32(NV_PGSP_RISCV_BASE + FALCON_BROM_MOD_SEL);
            uint32_t paraaddr= gsp_platform->read32(NV_PGSP_RISCV_BASE + FALCON_BROM_PARAADDR0);
            uint32_t os_reg  = gsp_platform->read32(NV_PGSP_BASE       + FALCON_OS);
            uint32_t dbginfo = gsp_platform->read32(NV_PGSP_BASE       + FALCON_DEBUGINFO);
            uint32_t engr    = gsp_platform->read32(NV_PGSP_BASE       + FALCON_ENGINE);
            uint32_t dmactl  = gsp_platform->read32(NV_PGSP_BASE       + FALCON_DMACTL);
            uint32_t trfcmd  = gsp_platform->read32(NV_PGSP_BASE       + FALCON_DMATRFCMD);
            /* Sample CPUCTL twice to detect "stuck running" vs "halted-
             * in-the-clear" — if the engine is alive its CPUCTL low bits
             * may not be perfectly stable, but the HALTED bit (4) and
             * the IINVAL bit (0) latch. */
            uint32_t cpuctl2 = gsp_platform->read32(NV_PGSP_BASE       + FALCON_CPUCTL);
            printf("                GSP Falcon state:\n");
            printf("                  CPUCTL=0x%08x → 0x%08x (halted=%d, alias_en=%d, iinval=%d)\n",
                   cpuctl, cpuctl2,
                   !!(cpuctl2 & FALCON_CPUCTL_HALTED),
                   !!(cpuctl2 & FALCON_CPUCTL_ALIAS_EN),
                   !!(cpuctl2 & 1));
            printf("                  MAILBOX0=0x%08x MAILBOX1=0x%08x  OS=0x%08x DEBUGINFO=0x%08x\n",
                   mbox0, mbox1, os_reg, dbginfo);
            printf("                  IRQSTAT=0x%08x (halt=%d, swgen0=%d)\n",
                   irqstat, !!(irqstat & 0x10), !!(irqstat & 0x40));
            printf("                  HWCFG2=0x%08x ENGINE=0x%08x DMACTL=0x%08x DMATRFCMD=0x%08x\n",
                   hwcfg2, engr, dmactl, trfcmd);
            printf("                  BCR_CTRL=0x%08x MOD_SEL=0x%08x PARAADDR0=0x%08x\n",
                   bcrctl, modsel, paraaddr);
            /* Post-timeout DEBUGINFO time series. If the value is
             * frozen across multiple samples → FWSEC stopped
             * executing. If it evolves → FWSEC is still making slow
             * progress past the 2 s timeout and may just need more
             * time. Doesn't touch Falcon state; purely reads. */
            printf("                post-timeout samples (100 ms apart):\n");
            for (int k = 0; k < 10; k++) {
                struct timespec st = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000L };
                nanosleep(&st, NULL);
                uint32_t s_dbg = gsp_platform->read32(NV_PGSP_BASE + FALCON_DEBUGINFO);
                uint32_t s_cpu = gsp_platform->read32(NV_PGSP_BASE + FALCON_CPUCTL);
                uint32_t s_mbx = gsp_platform->read32(NV_PGSP_BASE + FALCON_MAILBOX0);
                uint32_t s_os  = gsp_platform->read32(NV_PGSP_BASE + FALCON_OS);
                printf("                  t+%dms  DEBUGINFO=0x%08x  CPUCTL=0x%08x  MBX0=0x%08x  OS=0x%08x\n",
                       (k + 1) * 100, s_dbg, s_cpu, s_mbx, s_os);
            }
            return 1;
        }
        if (is_sb) {
            uint32_t mbox0   = gsp_platform->read32(NV_PGSP_BASE + FALCON_MAILBOX0);
            uint32_t os_reg  = gsp_platform->read32(NV_PGSP_BASE + FALCON_OS);
            uint32_t dbginfo = gsp_platform->read32(NV_PGSP_BASE + FALCON_DEBUGINFO);
            printf("[GSP-HARNESS] %s ok — Falcon halted cleanly\n"
                   "                MAILBOX0=0x%08x  OS=0x%08x  DEBUGINFO=0x%08x\n",
                   label, mbox0, os_reg, dbginfo);
            return 0;
        }
        printf("[GSP-HARNESS] %s ok — Falcon halted cleanly\n", label);
        uint32_t wpr_lo  = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        uint32_t wpr_hi  = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        uint32_t err_reg = gsp_platform->read32(NV_FWSEC_FRTS_ERR_REG);
        uint32_t mbox0   = gsp_platform->read32(NV_PGSP_BASE + FALCON_MAILBOX0);
        uint32_t os_reg  = gsp_platform->read32(NV_PGSP_BASE + FALCON_OS);
        uint32_t dbginfo = gsp_platform->read32(NV_PGSP_BASE + FALCON_DEBUGINFO);
        printf("                WPR2_LO   = 0x%08x\n", wpr_lo);
        printf("                WPR2_HI   = 0x%08x\n", wpr_hi);
        printf("                ERR_REG   = 0x%08x  (code=%u)\n", err_reg, err_reg >> 16);
        printf("                MAILBOX0  = 0x%08x  OS = 0x%08x  DEBUGINFO = 0x%08x\n",
               mbox0, os_reg, dbginfo);
        return 0;
    }
    case ACT_FWSEC_TRACE: {
        /* DEBUGINFO time-series probe. Kicks FWSEC-FRTS up through
         * STARTCPU then samples (DEBUGINFO, MAILBOX0, CPUCTL, OS) at
         * increasing intervals. Static values across samples → FWSEC
         * stuck at one instruction. Changing values → slow progress. */
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            printf("[GSP-HARNESS] bringup prepare FAILED\n");
            return 1;
        }
        b.trace_mode = true;
        if (gsp_bringup_fwsec_frts(&b) < 0) {
            uint32_t cpuctl  = gsp_platform->read32(NV_PGSP_BASE + FALCON_CPUCTL);
            uint32_t hwcfg2  = gsp_platform->read32(NV_PGSP_BASE + FALCON_HWCFG2);
            uint32_t dmactl  = gsp_platform->read32(NV_PGSP_BASE + FALCON_DMACTL);
            uint32_t trfcmd  = gsp_platform->read32(NV_PGSP_BASE + FALCON_DMATRFCMD);
            printf("[GSP-HARNESS] FWSEC-TRACE launch FAILED at phase %u\n"
                   "                CPUCTL=0x%08x (halted=%d) HWCFG2=0x%08x\n"
                   "                DMACTL=0x%08x DMATRFCMD=0x%08x\n",
                   b.last_error_phase, cpuctl,
                   !!(cpuctl & FALCON_CPUCTL_HALTED), hwcfg2, dmactl, trfcmd);
            gsp_bringup_free(&b);
            return 1;
        }
        printf("[GSP-HARNESS] FWSEC-TRACE: kicked STARTCPU, sampling…\n");
        printf("                init_cmd=0x%02x  MAILBOX0 sentinel=0xCAFEBEEF\n",
               b.init_cmd);
        printf("  %8s  %10s  %10s  %10s  %10s  %s\n",
               "t_ms", "DEBUGINFO", "MBOX0", "CPUCTL", "OS", "HALTED");
        static const uint32_t samples_us[] = {
            1000, 10000, 50000, 200000, 500000, 1000000, 2000000
        };
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint32_t last_us = 0;
        for (size_t i = 0; i < sizeof(samples_us)/sizeof(samples_us[0]); i++) {
            uint32_t delta = samples_us[i] - last_us;
            last_us = samples_us[i];
            struct timespec d = { .tv_sec = delta / 1000000u,
                                  .tv_nsec = (long)(delta % 1000000u) * 1000L };
            nanosleep(&d, NULL);

            uint32_t cpuctl  = gsp_platform->read32(NV_PGSP_BASE + FALCON_CPUCTL);
            uint32_t mbox0   = gsp_platform->read32(NV_PGSP_BASE + FALCON_MAILBOX0);
            uint32_t os_reg  = gsp_platform->read32(NV_PGSP_BASE + FALCON_OS);
            uint32_t dbginfo = gsp_platform->read32(NV_PGSP_BASE + FALCON_DEBUGINFO);
            bool halted = !!(cpuctl & FALCON_CPUCTL_HALTED);
            printf("  %8.1f  0x%08x  0x%08x  0x%08x  0x%08x  %s\n",
                   samples_us[i] / 1000.0, dbginfo, mbox0, cpuctl, os_reg,
                   halted ? "YES" : "no");
            if (halted) {
                printf("[GSP-HARNESS] Falcon halted at t=%.1f ms — stopping samples\n",
                       samples_us[i] / 1000.0);
                break;
            }
        }
        uint32_t final_err = gsp_platform->read32(NV_FWSEC_FRTS_ERR_REG);
        uint32_t final_wpr_lo = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        uint32_t final_wpr_hi = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        printf("[GSP-HARNESS] Post-samples: ERR=0x%08x (code=%u) "
               "WPR2_LO=0x%08x WPR2_HI=0x%08x\n",
               final_err, final_err >> 16, final_wpr_lo, final_wpr_hi);
        gsp_bringup_free(&b);
        return 0;
    }
    case ACT_BOOTER_LOAD: {
        /* Runs the full FWSEC-FRTS → Booter Load chain. The booter
         * is gated on FWSEC-FRTS having set up WPR2; running it
         * standalone would fail with an obscure SEC2 hang because
         * SEC2's first action is to dereference WPR2. */
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            printf("[GSP-HARNESS] bringup prepare FAILED\n"); return 1;
        }
        if (gsp_bringup_fwsec_frts(&b) < 0) {
            printf("[GSP-HARNESS] FWSEC-FRTS FAILED at phase %u — "
                   "see --fwsec-frts for full diagnostics\n", b.last_error_phase);
            return 1;
        }
        printf("[GSP-HARNESS] FWSEC-FRTS ok (WPR2 set), running Booter Load…\n");
        int rc = gsp_bringup_booter_load(&b);
        if (rc < 0) {
            printf("[GSP-HARNESS] Booter Load FAILED at phase %u (rc=%d)\n",
                   b.last_error_phase, rc);
            uint32_t cpuctl = gsp_platform->read32(NV_PSEC2_BASE + FALCON_CPUCTL);
            uint32_t mbox0  = gsp_platform->read32(NV_PSEC2_BASE + FALCON_MAILBOX0);
            uint32_t mbox1  = gsp_platform->read32(NV_PSEC2_BASE + FALCON_MAILBOX1);
            printf("                SEC2 CPUCTL=0x%08x (halted=%d) "
                   "MBOX0=0x%08x MBOX1=0x%08x\n",
                   cpuctl, !!(cpuctl & FALCON_CPUCTL_HALTED), mbox0, mbox1);
            return 1;
        }
        printf("[GSP-HARNESS] Booter Load halted ok — MAILBOX0 post = 0x%08x\n",
               b.booter_mbox0_post);
        printf("                (0 == nominal completion; non-zero == "
               "booter halted with status — common when WprMeta is "
               "incomplete pending E4)\n");
        return 0;
    }
    case ACT_RISCV_START: {
        /* Full chain through E3.4.e. Will fail at riscv_start unless
         * the booter populated WPR2 with a usable GSP-RM image — which
         * itself depends on a fully-formed WprMeta (E4 work). */
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            printf("[GSP-HARNESS] bringup prepare FAILED\n"); return 1;
        }
        if (gsp_bringup_fwsec_frts(&b) < 0) {
            printf("[GSP-HARNESS] FWSEC-FRTS FAILED at phase %u\n",
                   b.last_error_phase); return 1;
        }
        if (gsp_bringup_booter_load(&b) < 0) {
            printf("[GSP-HARNESS] Booter Load FAILED at phase %u\n",
                   b.last_error_phase); return 1;
        }
        int rc = gsp_bringup_riscv_start(&b);
        if (rc < 0) {
            printf("[GSP-HARNESS] RISC-V start FAILED at phase %u (rc=%d)\n",
                   b.last_error_phase, rc);
            uint32_t bcr = gsp_platform->read32(NV_PGSP_RISCV_BASE
                                                + FALCON_RISCV_BCR_CTRL);
            uint32_t cc  = gsp_platform->read32(NV_PGSP_RISCV_BASE
                                                + FALCON_RISCV_CPUCTL);
            printf("                BCR_CTRL=0x%08x RISCV_CPUCTL=0x%08x\n",
                   bcr, cc);
            return 1;
        }
        printf("[GSP-HARNESS] RISC-V active ✓ — GSP_INIT_DONE wait owned by E4\n");
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
