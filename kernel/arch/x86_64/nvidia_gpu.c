/*
 * nvidia_gpu.c - NVIDIA GPU probe and identification for x86-64
 *
 * Reads GPU identification registers via PCI BAR0 to determine the
 * GPU model, architecture, and revision. Maps VRAM via BAR1 for
 * basic memory access verification.
 *
 * Register reference:
 *   NV_PMC_BOOT_0  (0x000) — Legacy GPU identification (split arch field)
 *   NV_PMC_BOOT_42 (0xA00) — Preferred GPU identification (clean layout)
 *   NV_PMC_ENABLE  (0x200) — Engine master enable
 *
 * BAR layout:
 *   BAR0: 16 MB MMIO registers (non-prefetchable, 32-bit)
 *   BAR1: VRAM aperture (prefetchable, 64-bit)
 *   BAR2: RAMIN / control structures (64-bit)
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "uart.h"
#include "shell.h"

#include "pci.h"
#include "../../gpu/nvidia/gsp.h"
#include "../../gpu/nvidia/bringup.h"

/* ---- NVIDIA Register Offsets (BAR0) ---- */

#define NV_PMC_BOOT_0          0x000   /* GPU identification (legacy) */
#define NV_PMC_ENDIAN          0x004   /* Endian control */
#define NV_PMC_BOOT_1          0x004   /* vGPU detection */
#define NV_PMC_INTR_HOST       0x100   /* Host interrupt status */
#define NV_PMC_INTR_EN_HOST    0x140   /* Host interrupt enable */
#define NV_PMC_ENABLE          0x200   /* Engine master enable */
#define NV_PMC_BOOT_42         0xA00   /* GPU identification (preferred) */

/* PTIMER */
#define NV_PTIMER_TIME_0       0x9400  /* Timer low 32 bits */
#define NV_PTIMER_TIME_1       0x9410  /* Timer high 32 bits */

/* ---- NVIDIA Vendor ID ---- */
#define NVIDIA_VENDOR_ID       0x10DE

/* ---- GPU State ---- */

static struct {
    bool     found;
    uint8_t  bus, dev, func;
    uint16_t device_id;

    /* BAR0 MMIO (GPU registers) */
    volatile uint32_t *bar0;
    uint64_t bar0_addr;
    uint32_t bar0_size;

    /* BAR1 VRAM aperture */
    volatile uint8_t *bar1;
    uint64_t bar1_addr;
    uint64_t bar1_size;

    /* Identification from BOOT_42 */
    uint32_t boot0;
    uint32_t boot42;
    uint8_t  architecture;
    uint8_t  implementation;
    uint16_t chip_id;
    uint8_t  major_rev;
    uint8_t  minor_rev;
} nvidia_gpu;

/* ---- Architecture Names ---- */

static const char *arch_name(uint8_t arch)
{
    switch (arch) {
    case 0x11: return "Maxwell";
    case 0x12: return "Maxwell 2nd gen";
    case 0x13: return "Pascal";
    case 0x14: return "Volta";
    case 0x16: return "Turing";
    case 0x17: return "Ampere";
    case 0x18: return "Hopper";
    case 0x19: return "Ada Lovelace";
    case 0x1A: return "Blackwell";
    default:   return "Unknown";
    }
}

static const char *impl_name(uint8_t arch, uint8_t impl)
{
    if (arch == 0x17) {  /* Ampere */
        switch (impl) {
        case 0x00: return "GA100";
        case 0x02: return "GA102";
        case 0x03: return "GA103";
        case 0x04: return "GA104";
        case 0x06: return "GA106";
        case 0x07: return "GA107";
        default:   return "GA1xx";
        }
    }
    return "?";
}

/* ---- BAR Size Probing ---- */

/*
 * Probe the size of a PCI BAR by writing all-ones and reading back.
 * The number of low bits that remain zero indicates the BAR size.
 * Restores the original BAR value after probing.
 */
static uint64_t probe_bar_size(uint8_t bus, uint8_t dev, uint8_t func,
                               uint8_t bar_reg, bool is_64bit)
{
    /* Save original */
    uint32_t orig_lo = pci_config_read32(bus, dev, func, bar_reg);
    uint32_t orig_hi = 0;
    if (is_64bit)
        orig_hi = pci_config_read32(bus, dev, func, bar_reg + 4);

    /* Write all-ones */
    pci_config_write32(bus, dev, func, bar_reg, 0xFFFFFFFF);
    uint32_t mask_lo = pci_config_read32(bus, dev, func, bar_reg);

    uint64_t mask = mask_lo;
    if (is_64bit) {
        pci_config_write32(bus, dev, func, bar_reg + 4, 0xFFFFFFFF);
        uint32_t mask_hi = pci_config_read32(bus, dev, func, bar_reg + 4);
        mask |= (uint64_t)mask_hi << 32;
    }

    /* Restore original */
    pci_config_write32(bus, dev, func, bar_reg, orig_lo);
    if (is_64bit)
        pci_config_write32(bus, dev, func, bar_reg + 4, orig_hi);

    /* MMIO BAR: clear type bits (low 4 bits) */
    bool is_mmio = !(orig_lo & 1);
    if (is_mmio)
        mask &= ~0xFULL;
    else
        mask &= ~0x3ULL;

    if (mask == 0) return 0;

    /* Size = ~mask + 1 (two's complement of the mask) */
    return (~mask) + 1;
}

/* ---- Public Interface ---- */

/*
 * Probe for NVIDIA GPU on the PCI bus.
 * Reads BAR0 and BAR1, maps them, reads identification registers.
 * Safe to call even if no GPU is present.
 */
void nvidia_gpu_init(void)
{
    nvidia_gpu.found = false;

    /* Find NVIDIA display device (class 03:00 = VGA, 03:02 = 3D) */
    const struct pci_device *pci_dev = NULL;

    /* Scan all PCI devices for NVIDIA vendor */
    for (uint32_t i = 0; i < pci_get_device_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == NVIDIA_VENDOR_ID && d->class_code == 0x03) {
            pci_dev = d;
            break;
        }
    }

    if (!pci_dev) {
        uart_printf("[GPU] No NVIDIA GPU found on PCI bus\n");
        return;
    }

    nvidia_gpu.bus = pci_dev->bus;
    nvidia_gpu.dev = pci_dev->dev;
    nvidia_gpu.func = pci_dev->func;
    nvidia_gpu.device_id = pci_dev->device_id;

    uart_printf("[GPU] NVIDIA device at %02x:%02x.%x (device 0x%04x)\n",
                pci_dev->bus, pci_dev->dev, pci_dev->func, pci_dev->device_id);

    /* Ensure bus mastering and memory space are enabled */
    uint16_t cmd = pci_config_read16(pci_dev->bus, pci_dev->dev, pci_dev->func, 0x04);
    if (!(cmd & 0x06)) {
        cmd |= 0x06;  /* Memory Space + Bus Master */
        pci_config_write32(pci_dev->bus, pci_dev->dev, pci_dev->func, 0x04, cmd);
        uart_printf("[GPU] Enabled memory space + bus master\n");
    }

    /* ---- BAR0: GPU MMIO Registers (32-bit, non-prefetchable) ---- */
    uint32_t bar0_raw = pci_dev->bar[0];
    if (bar0_raw == 0 || (bar0_raw & 1)) {
        uart_printf("[GPU] BAR0 invalid (0x%x)\n", bar0_raw);
        return;
    }

    nvidia_gpu.bar0_addr = bar0_raw & ~0xFUL;
    nvidia_gpu.bar0_size = (uint32_t)probe_bar_size(
        pci_dev->bus, pci_dev->dev, pci_dev->func, 0x10, false);
    nvidia_gpu.bar0 = (volatile uint32_t *)(uintptr_t)nvidia_gpu.bar0_addr;

    uart_printf("[GPU] BAR0: 0x%lx (%u MB MMIO registers)\n",
                nvidia_gpu.bar0_addr, nvidia_gpu.bar0_size / (1024 * 1024));

    /* ---- BAR1: VRAM Aperture (64-bit, prefetchable) ---- */
    uint32_t bar1_lo = pci_dev->bar[1];
    uint32_t bar1_hi = pci_dev->bar[2];
    bool bar1_is_64bit = (bar1_lo & 1) == 0 && ((bar1_lo >> 1) & 3) == 2;

    nvidia_gpu.bar1_addr = bar1_lo & ~0xFUL;
    if (bar1_is_64bit)
        nvidia_gpu.bar1_addr |= (uint64_t)bar1_hi << 32;

    nvidia_gpu.bar1_size = probe_bar_size(
        pci_dev->bus, pci_dev->dev, pci_dev->func, 0x14, bar1_is_64bit);
    nvidia_gpu.bar1 = (volatile uint8_t *)(uintptr_t)nvidia_gpu.bar1_addr;

    uart_printf("[GPU] BAR1: 0x%lx (%lu MB VRAM aperture)\n",
                nvidia_gpu.bar1_addr,
                (unsigned long)(nvidia_gpu.bar1_size / (1024 * 1024)));

    /* ---- Read GPU Identification Registers ---- */
    nvidia_gpu.boot0 = nvidia_gpu.bar0[NV_PMC_BOOT_0 / 4];
    nvidia_gpu.boot42 = nvidia_gpu.bar0[NV_PMC_BOOT_42 / 4];

    /* Decode BOOT_42 (preferred, clean bit layout) */
    nvidia_gpu.architecture = (nvidia_gpu.boot42 >> 24) & 0x3F;
    nvidia_gpu.implementation = (nvidia_gpu.boot42 >> 20) & 0x0F;
    nvidia_gpu.chip_id = (nvidia_gpu.boot42 >> 20) & 0x3FF;
    nvidia_gpu.major_rev = (nvidia_gpu.boot42 >> 16) & 0x0F;
    nvidia_gpu.minor_rev = (nvidia_gpu.boot42 >> 12) & 0x0F;

    nvidia_gpu.found = true;

    uart_printf("[GPU] BOOT_0:  0x%08x\n", nvidia_gpu.boot0);
    uart_printf("[GPU] BOOT_42: 0x%08x\n", nvidia_gpu.boot42);
    uart_printf("[GPU] Chip: %s (0x%03x) — %s architecture\n",
                impl_name(nvidia_gpu.architecture, nvidia_gpu.implementation),
                nvidia_gpu.chip_id,
                arch_name(nvidia_gpu.architecture));
    uart_printf("[GPU] Revision: %u.%u\n",
                nvidia_gpu.major_rev, nvidia_gpu.minor_rev);

    /* Read engine enable status */
    uint32_t pmc_enable = nvidia_gpu.bar0[NV_PMC_ENABLE / 4];
    uart_printf("[GPU] PMC_ENABLE: 0x%08x\n", pmc_enable);

    /* Install the x86-64 platform shim for the shared GSP-RM code
     * (kernel/gpu/nvidia/gsp.c). Does NOT yet kick off gsp_init();
     * that happens once the shell-level `gpu init` command is wired
     * up (E3), so a developer can explicitly trigger the multi-
     * second bringup when ready rather than at every boot. */
    extern void x86_gsp_platform_install(void);
    x86_gsp_platform_install();
}

/*
 * Accessor for the probed NVIDIA GPU's PCI bus/dev/func.
 * Used by nvidia_gsp_platform.c:nvidia_vbios_platform_load() to
 * toggle the Expansion ROM BAR. Returns 0 on success, -1 if no
 * NVIDIA GPU was found.
 */
int nvidia_gpu_get_pci_address(uint8_t *out_bus, uint8_t *out_dev,
                               uint8_t *out_func)
{
    if (!nvidia_gpu.found) return -1;
    if (out_bus)  *out_bus  = nvidia_gpu.bus;
    if (out_dev)  *out_dev  = nvidia_gpu.dev;
    if (out_func) *out_func = nvidia_gpu.func;
    return 0;
}

/*
 * Test VRAM access by writing and reading a pattern.
 * Returns 0 on success, -1 on failure.
 */
int nvidia_gpu_vram_test(void)
{
    if (!nvidia_gpu.found || !nvidia_gpu.bar1 || nvidia_gpu.bar1_size == 0) {
        uart_printf("[GPU] VRAM test: no GPU or BAR1 not mapped\n");
        return -1;
    }

    uart_printf("[GPU] VRAM test: writing pattern to BAR1 at 0x%lx...\n",
                nvidia_gpu.bar1_addr);

    /* Write test pattern (first 256 bytes) */
    volatile uint32_t *vram = (volatile uint32_t *)nvidia_gpu.bar1;
    uint32_t pattern = 0xDEADBEEF;
    int errors = 0;

    for (int i = 0; i < 64; i++) {
        vram[i] = pattern ^ (uint32_t)i;
    }

    /* Memory fence */
    __asm__ volatile("mfence" ::: "memory");

    /* Read back and verify */
    for (int i = 0; i < 64; i++) {
        uint32_t expected = pattern ^ (uint32_t)i;
        uint32_t actual = vram[i];
        if (actual != expected) {
            if (errors < 5)
                uart_printf("[GPU]   VRAM[%d]: expected 0x%08x, got 0x%08x\n",
                            i, expected, actual);
            errors++;
        }
    }

    if (errors == 0) {
        uart_printf("[GPU] VRAM test: PASSED (64 words verified)\n");
        return 0;
    } else {
        uart_printf("[GPU] VRAM test: FAILED (%d errors)\n", errors);
        return -1;
    }
}

/* ---- Shell Command ---- */

/*
 * Extended VRAM test — tests multiple offsets across the aperture.
 * Returns 0 on success, -1 on failure.
 */
int nvidia_gpu_vram_test_extended(void)
{
    if (!nvidia_gpu.found || !nvidia_gpu.bar1 || nvidia_gpu.bar1_size == 0) {
        uart_printf("[GPU] VRAM test: no GPU or BAR1 not mapped\n");
        return -1;
    }

    volatile uint32_t *vram = (volatile uint32_t *)nvidia_gpu.bar1;
    uint64_t aperture_words = nvidia_gpu.bar1_size / 4;
    int total_errors = 0;

    /* Test at multiple offsets: 0, 1MB, 16MB, 64MB, 128MB */
    uint64_t offsets[] = {0, 256*1024, 4*1024*1024, 16*1024*1024, 32*1024*1024};
    int num_offsets = 5;

    for (int t = 0; t < num_offsets; t++) {
        uint64_t word_offset = offsets[t];
        if (word_offset + 64 > aperture_words) {
            uart_printf("[GPU]   Offset 0x%lx: beyond aperture, skipped\n",
                        word_offset * 4);
            continue;
        }

        uint32_t pattern = 0xA5A5A5A5 ^ (uint32_t)t;
        int errors = 0;

        /* Write pattern */
        for (int i = 0; i < 64; i++)
            vram[word_offset + i] = pattern ^ (uint32_t)i;

        __asm__ volatile("mfence" ::: "memory");

        /* Read back */
        for (int i = 0; i < 64; i++) {
            uint32_t expected = pattern ^ (uint32_t)i;
            uint32_t actual = vram[word_offset + i];
            if (actual != expected) {
                if (errors == 0)
                    uart_printf("[GPU]   Offset 0x%lx+%d: expected 0x%08x, got 0x%08x\n",
                                word_offset * 4, i * 4, expected, actual);
                errors++;
            }
        }

        if (errors == 0) {
            uart_printf("[GPU]   Offset 0x%08lx: PASS (64 words)\n", word_offset * 4);
        } else {
            uart_printf("[GPU]   Offset 0x%08lx: FAIL (%d errors)\n", word_offset * 4, errors);
            total_errors += errors;
        }
    }

    return total_errors == 0 ? 0 : -1;
}

static int cmd_gpu(int argc, char *argv[])
{
    if (!nvidia_gpu.found) {
        uart_printf("No NVIDIA GPU detected.\n");
        return 0;
    }

    /* Subcommand: "gpu init" runs the GSP-RM bringup sequence */
    if (argc >= 2 && argv[1][0] == 'i') {
        extern int gsp_init(void);
        extern enum gsp_state gsp_get_state(void);

        uart_printf("[GPU] Starting GSP-RM bringup on %s...\n",
                    impl_name(nvidia_gpu.architecture, nvidia_gpu.implementation));

        /* SEC2 Falcon unlock — work-in-progress (see #185 notes).
         *
         * Empirical findings on test-pc (GA107):
         *   - NV_PMC_DEVICE_ENABLE (0x600) is already 0xffffffff in
         *     SLM-OS after UEFI POST (same as nouveau post-init). So
         *     SEC2's device-enable bit is not what's missing.
         *   - SEC2 CPUCTL reads 0xbadf5620 from the moment UEFI hands
         *     off. HWCFG2 reads 0x000067f7 (bit 13 set = not-yet-reset).
         *   - Under nouveau, CPUCTL reads 0x00000020 and HWCFG2 reads
         *     0x000047f7 (bit 13 cleared). That bit is the "RESET_READY"
         *     indicator per nova-core (nouveau-falcon-hal-ga102.rs).
         *   - Brute-force toggling each DEVICE_ENABLE bit hangs the PRI
         *     bus because bit 0 (likely HOST) being cleared kills MMIO.
         *
         * The missing step is likely:
         *   1. Replay the VBIOS DEVINIT sequence that nouveau triggers
         *      via its devinit subdev (nvkm/subdev/devinit/tu102.c), OR
         *   2. Find the specific SEC2 device-enable bit index via a
         *      PTOP (PRI TOP) table walk (BAR0 + 0x022400 on Ampere).
         *
         * For now, just report current state and proceed with Booter
         * Load attempt — it will fail predictably, leaving SEC2 state
         * observable via `gpu sec2` for iteration.
         */
        volatile uint32_t *dev_en = &nvidia_gpu.bar0[0x600 / 4];
        volatile uint32_t *sec2_hwcfg2 = &nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x0f4) / 4];
        volatile uint32_t *sec2_cpuctl = &nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x100) / 4];
        uart_printf("[GPU] NV_PMC_DEVICE_ENABLE(0x600) = 0x%08x\n", *dev_en);
        uart_printf("[GPU] SEC2 CPUCTL = 0x%08x  HWCFG2 = 0x%08x\n",
                    *sec2_cpuctl, *sec2_hwcfg2);

        /* Phase 0: firmware load + sanity check */
        int rc = gsp_init();
        if (rc < 0 && gsp_get_state() == GSP_STATE_FAILED) {
            uart_printf("[GPU] GSP Phase 0 (firmware load) FAILED\n");
            return -1;
        }

        /* Phase 1: FWSEC-FRTS on GSP Falcon */
        uart_printf("[GPU] Phase 1: FWSEC-FRTS — preparing bringup...\n");
        struct gsp_bringup b;
        if (gsp_bringup_prepare(&b) < 0) {
            uart_printf("[GPU] bringup prepare FAILED\n");
            return -1;
        }
        uart_printf("[GPU] FWSEC: imem=%u dmem=%u engine=0x%x ucode=%u\n",
                    b.fwsec_imem_size, b.fwsec_dmem_size,
                    b.fwsec_engine_id, b.fwsec_ucode_id);
        uart_printf("[GPU] WPR2 target: addr=0x%lx size=0x%lx\n",
                    (unsigned long)b.wpr2_addr,
                    (unsigned long)b.wpr2_size);

        rc = gsp_bringup_fwsec_frts(&b);
        if (b.diag_sig_count) {
            uart_printf("[GPU] sig: fuse[0x%x]=0x%x count=%u ver=0x%x idx=%u\n",
                        b.diag_fuse_reg_off, b.diag_fuse_reg_val,
                        b.diag_sig_count, b.diag_sig_versions, b.diag_sig_index);
        }
        if (rc < 0) {
            uart_printf("[GPU] FWSEC-FRTS FAILED at phase %u\n", b.last_error_phase);
            gsp_bringup_free(&b);
            return -1;
        }

        /* Read WPR2 registers to confirm FWSEC populated them */
        extern const struct gsp_platform_ops *gsp_platform;
        uint32_t wpr_lo = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
        uint32_t wpr_hi = gsp_platform->read32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
        uart_printf("[GPU] FWSEC-FRTS SUCCESS — WPR2: lo=0x%08x hi=0x%08x\n",
                    wpr_lo, wpr_hi);

        /* Phase 2: Booter Load on SEC2 */
        uart_printf("[GPU] Phase 2: Booter Load on SEC2...\n");
        rc = gsp_bringup_booter_load(&b);
        if (rc < 0) {
            uart_printf("[GPU] Booter Load FAILED at phase %u\n", b.last_error_phase);
            /* Dump SEC2 state for diagnosis — matches what the VFIO
             * harness prints on FWSEC failures. */
            uint32_t s_cpuctl  = gsp_platform->read32(NV_PSEC2_BASE + 0x100);
            uint32_t s_mbox0   = gsp_platform->read32(NV_PSEC2_BASE + 0x040);
            uint32_t s_mbox1   = gsp_platform->read32(NV_PSEC2_BASE + 0x044);
            uint32_t s_irqstat = gsp_platform->read32(NV_PSEC2_BASE + 0x008);
            uint32_t s_os      = gsp_platform->read32(NV_PSEC2_BASE + 0x080);
            uint32_t s_dbginfo = gsp_platform->read32(NV_PSEC2_BASE + 0x094);
            uint32_t s_modsel  = gsp_platform->read32(NV_PSEC2_BROM_BASE + 0x010);
            uint32_t s_paraddr = gsp_platform->read32(NV_PSEC2_BROM_BASE + 0x004);
            uart_printf("[GPU]   SEC2 CPUCTL=0x%08x (halted=%u)\n",
                        s_cpuctl, (s_cpuctl >> 4) & 1);
            uart_printf("[GPU]   SEC2 MBOX0=0x%08x MBOX1=0x%08x OS=0x%08x\n",
                        s_mbox0, s_mbox1, s_os);
            uart_printf("[GPU]   SEC2 IRQSTAT=0x%08x DEBUGINFO=0x%08x\n",
                        s_irqstat, s_dbginfo);
            uart_printf("[GPU]   SEC2 BROM MOD_SEL=0x%08x PARAADDR=0x%08x\n",
                        s_modsel, s_paraddr);
            gsp_bringup_free(&b);
            return -1;
        }
        uart_printf("[GPU] Booter Load complete — MAILBOX0=0x%08x\n",
                    b.booter_mbox0_post);

        /* Phase 3: GSP RISC-V startup */
        uart_printf("[GPU] Phase 3: GSP RISC-V start...\n");
        rc = gsp_bringup_riscv_start(&b);
        if (rc < 0) {
            uart_printf("[GPU] RISC-V start FAILED\n");
            gsp_bringup_free(&b);
            return -1;
        }
        uart_printf("[GPU] GSP RISC-V RUNNING — bringup complete\n");

        gsp_bringup_free(&b);
        return 0;
    }

    /* Subcommand: "gpu sec2" dumps SEC2 Falcon state (priv-lock diagnosis) */
    if (argc >= 2 && argv[1][0] == 's') {
        uint32_t cpuctl  = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x100) / 4];
        uint32_t hwcfg2  = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x0f4) / 4];
        uint32_t mbox0   = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x040) / 4];
        uint32_t mbox1   = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x044) / 4];
        uint32_t irqstat = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x008) / 4];
        uint32_t os_reg  = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x080) / 4];
        uint32_t dbginfo = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x094) / 4];
        uint32_t engctl  = nvidia_gpu.bar0[(NV_PSEC2_BASE + 0x0bc) / 4];
        uint32_t modsel  = nvidia_gpu.bar0[(NV_PSEC2_BROM_BASE + 0x010) / 4];
        uint32_t paraddr = nvidia_gpu.bar0[(NV_PSEC2_BROM_BASE + 0x004) / 4];
        /* Also probe GSP Falcon for comparison */
        uint32_t g_cpuctl = nvidia_gpu.bar0[(NV_PGSP_BASE + 0x100) / 4];
        uint32_t g_hwcfg2 = nvidia_gpu.bar0[(NV_PGSP_BASE + 0x0f4) / 4];

        uart_printf("SEC2 Falcon state (PSEC2_BASE=0x%x):\n", NV_PSEC2_BASE);
        uart_printf("  CPUCTL    = 0x%08x  (priv-lock=%s)\n",
                    cpuctl, (cpuctl & 0xffff0000) == 0xbadf0000 ? "yes" : "no");
        uart_printf("  HWCFG2    = 0x%08x  (priv-lock=%s)\n",
                    hwcfg2, (hwcfg2 & 0xffff0000) == 0xbadf0000 ? "yes" : "no");
        uart_printf("  IRQSTAT   = 0x%08x\n", irqstat);
        uart_printf("  MAILBOX0  = 0x%08x  MAILBOX1 = 0x%08x\n", mbox0, mbox1);
        uart_printf("  OS        = 0x%08x  DEBUGINFO= 0x%08x\n", os_reg, dbginfo);
        uart_printf("  ENGCTL    = 0x%08x\n", engctl);
        uart_printf("  BROM MOD_SEL  = 0x%08x\n", modsel);
        uart_printf("  BROM PARAADDR = 0x%08x\n", paraddr);
        uart_printf("GSP Falcon state (PGSP_BASE=0x%x):\n", NV_PGSP_BASE);
        uart_printf("  CPUCTL    = 0x%08x  (priv-lock=%s)\n",
                    g_cpuctl, (g_cpuctl & 0xffff0000) == 0xbadf0000 ? "yes" : "no");
        uart_printf("  HWCFG2    = 0x%08x  (priv-lock=%s)\n",
                    g_hwcfg2, (g_hwcfg2 & 0xffff0000) == 0xbadf0000 ? "yes" : "no");
        return 0;
    }

    /* Subcommand: "gpu vram" runs VRAM test */
    if (argc >= 2 && argv[1][0] == 'v') {
        uart_printf("VRAM Test (BAR1 at 0x%lx, %lu MB):\n",
                    nvidia_gpu.bar1_addr,
                    (unsigned long)(nvidia_gpu.bar1_size / (1024 * 1024)));
        int result = nvidia_gpu_vram_test_extended();
        uart_printf("Result: %s\n", result == 0 ? "PASSED" : "FAILED");
        return result;
    }

    /* Subcommand: "gpu regs" reads additional registers */
    if (argc >= 2 && argv[1][0] == 'r') {
        uart_printf("GPU Registers (BAR0 at 0x%lx):\n", nvidia_gpu.bar0_addr);
        uart_printf("  PMC_BOOT_0:    0x%08x\n", nvidia_gpu.bar0[0x000 / 4]);
        uart_printf("  PMC_BOOT_42:   0x%08x\n", nvidia_gpu.bar0[0xA00 / 4]);
        uart_printf("  PMC_ENABLE:    0x%08x\n", nvidia_gpu.bar0[0x200 / 4]);
        uart_printf("  PMC_INTR_HOST: 0x%08x\n", nvidia_gpu.bar0[0x100 / 4]);
        uart_printf("  PMC_INTR_EN:   0x%08x\n", nvidia_gpu.bar0[0x140 / 4]);
        uart_printf("  PTIMER_TIME_0: 0x%08x\n", nvidia_gpu.bar0[0x9400 / 4]);
        uart_printf("  PTIMER_TIME_1: 0x%08x\n", nvidia_gpu.bar0[0x9410 / 4]);
        uart_printf("  PBUS[0x1000]:  0x%08x\n", nvidia_gpu.bar0[0x1000 / 4]);
        uart_printf("  PSTRAPS:       0x%08x\n", nvidia_gpu.bar0[0x101000 / 4]);
        return 0;
    }

    /* Default: show GPU info */
    uart_printf("NVIDIA GPU: %s (%s)\n",
                impl_name(nvidia_gpu.architecture, nvidia_gpu.implementation),
                arch_name(nvidia_gpu.architecture));
    uart_printf("  PCI:      %02x:%02x.%x (device 0x%04x)\n",
                nvidia_gpu.bus, nvidia_gpu.dev, nvidia_gpu.func,
                nvidia_gpu.device_id);
    uart_printf("  Chip ID:  0x%03x (rev %u.%u)\n",
                nvidia_gpu.chip_id, nvidia_gpu.major_rev, nvidia_gpu.minor_rev);
    uart_printf("  BOOT_0:   0x%08x\n", nvidia_gpu.boot0);
    uart_printf("  BOOT_42:  0x%08x\n", nvidia_gpu.boot42);
    uart_printf("  BAR0:     0x%lx (%u MB MMIO)\n",
                nvidia_gpu.bar0_addr, nvidia_gpu.bar0_size / (1024 * 1024));
    uart_printf("  BAR1:     0x%lx (%lu MB VRAM)\n",
                nvidia_gpu.bar1_addr,
                (unsigned long)(nvidia_gpu.bar1_size / (1024 * 1024)));

    uint32_t pmc_enable = nvidia_gpu.bar0[NV_PMC_ENABLE / 4];
    uint32_t pmc_intr = nvidia_gpu.bar0[NV_PMC_INTR_HOST / 4];
    uart_printf("  PMC_ENABLE:    0x%08x\n", pmc_enable);
    uart_printf("  PMC_INTR_HOST: 0x%08x\n", pmc_intr);

    uart_printf("\nSubcommands: gpu init, gpu sec2, gpu vram, gpu regs\n");

    return 0;
}

static const shell_cmd_t gpu_nvidia_cmd = {
    .name = "gpu",
    .handler = cmd_gpu,
    .help = "NVIDIA GPU info (gpu init | gpu vram | gpu regs)"
};

void nvidia_gpu_register_shell_commands(void)
{
    shell_register_command(&gpu_nvidia_cmd);
}

/* ---- Accessors for tests ---- */

bool nvidia_gpu_is_found(void) { return nvidia_gpu.found; }
uint16_t nvidia_gpu_get_chip_id(void) { return nvidia_gpu.chip_id; }
uint8_t nvidia_gpu_get_architecture(void) { return nvidia_gpu.architecture; }
uint64_t nvidia_gpu_get_bar0_addr(void) { return nvidia_gpu.bar0_addr; }
uint64_t nvidia_gpu_get_bar1_addr(void) { return nvidia_gpu.bar1_addr; }

/* ---- Accessors for the GSP platform shim ---- */

volatile uint32_t *nvidia_gpu_get_bar0(void)   { return nvidia_gpu.bar0; }
uint32_t           nvidia_gpu_get_bar0_size(void) { return nvidia_gpu.bar0_size; }
volatile uint8_t  *nvidia_gpu_get_bar1(void)   { return nvidia_gpu.bar1; }
uint64_t           nvidia_gpu_get_bar1_size(void) { return nvidia_gpu.bar1_size; }

#endif /* PLATFORM_X86_64 */
