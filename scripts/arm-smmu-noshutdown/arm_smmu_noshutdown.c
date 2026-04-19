/*
 * arm-smmu-noshutdown — Linux kernel module that neutralises the
 * arm-smmu platform driver's .shutdown callback on L4T so Linux's
 * kexec path leaves the SMMU running across the handoff into SLM-OS.
 *
 * Purpose: SLM-OS #266 Phase 3A, Path 1 (see
 * docs/jetson-usb-networking-plan.md §10.3). This module fixes a
 * struct-field bug in the previous `.shutdown = NULL` template, so
 * on this L4T kernel arm_smmu_device_shutdown() now actually does
 * stop firing from device_shutdown(). See
 * docs/jetson-usb-networking-plan.md §10.8 for the hardware
 * investigation trail that led to the field fix, the four other
 * variants that were tested but do not solve the end-to-end blocker,
 * and the open follow-on work Phase 3A still needs.
 *
 * What this module fixes
 * ----------------------
 *
 *   The previous template stored NULL into `device_driver.shutdown`
 *   (the base struct). The platform bus dispatches shutdown through
 *   `platform_driver.shutdown` via platform_drv_shutdown, which is a
 *   SEPARATE field on the outer platform_driver struct. The base
 *   `device_driver.shutdown` is unused for platform drivers;
 *   NULLing it is a no-op. That explained why the previous template
 *   always reported "already NULL" on L4T even while
 *   `arm-smmu 8000000.iommu: disabling translation` still fired at
 *   kexec. The correct field is reached via to_platform_driver(drv).
 *
 *   A runtime diagnostic module verified this on hardware:
 *
 *     smmu-probe:   .shutdown = arm_smmu_device_shutdown+0x0/0x40
 *     smmu-probe:   .driver.bus->shutdown = platform_shutdown+0x0/0x60
 *
 *   — i.e. platform_driver.shutdown is what platform_drv_shutdown
 *   reads, and it's non-NULL (pointing at arm_smmu_device_shutdown).
 *   After this module loads:
 *
 *     smmu-probe:   .shutdown = (null)
 *
 *   and on the next kexec the Linux dmesg no longer contains
 *   `arm-smmu ... disabling translation`.
 *
 * What this module does NOT fix
 * -----------------------------
 *
 *   Suppressing arm_smmu_device_shutdown prevents Linux from
 *   tearing down the SMMU clocks or writing sCR0 = CLIENTPD. The
 *   SMMU therefore arrives in SLM-OS still enforcing Linux's
 *   IOVA-to-PA context banks. SLM-OS's xHCI driver programs DCBAAP
 *   / CRCR with raw physical addresses of its own NC-memory
 *   buffers, which the SMMU has no translations for, so the first
 *   HC DMA after `USBCMD.RUN=1` fails silently or — more often —
 *   propagates a fabric-level error response that wedges the xHCI
 *   aperture to 0xFFFFFFFF.
 *
 *   Four alternative fixes were tested in this same module's
 *   history and all either regressed or behaved no better than
 *   this plain-NULL version. See the plan for the full write-up;
 *   briefly:
 *
 *     - Replacement .shutdown that only writes sCR0 = CLIENTPD
 *       (skipping clock teardown) wedges the aperture because
 *       tegra-xusb has no .shutdown and is still actively DMA-ing
 *       when the bypass flips, reinterpreting in-flight IOVAs as
 *       raw PAs.
 *     - Replacement .shutdown that targets only SMMU0 (the xusb
 *       instance) wedges identically.
 *     - SLM-OS-side sCR0 write after xhci_halt() wedges identically,
 *       suggesting the Tegra234 SMMU needs a per-instance bypass
 *       sequence this code doesn't reproduce.
 *     - reboot_notifier firing from kernel_restart_prepare()
 *       produced "tegra-mc: EMEM address decode error" messages
 *       during Linux's remaining shutdown steps, confirming the
 *       bypass raced with in-flight Linux DMA. syscore_shutdown()
 *       is NOT called on the kexec path in 5.15, so there is no
 *       cross-subsystem hook that fires AFTER device_shutdown().
 *
 *   Bringing NO_OP round-trip to success therefore needs either a
 *   minimum-subset port of arm-smmu-v2 into SLM-OS (Path 2 in the
 *   plan, ~350-500 lines) so SLM-OS can program a context bank
 *   that maps its own physical addresses, or a much more careful
 *   pre-kexec sequence that halts every live bus master (not just
 *   xusb) before flipping bypass. Both are out of scope for the
 *   session that produced this module.
 *
 * Usage:
 *   insmod arm_smmu_noshutdown.ko
 *   # ... then kexec ...
 *
 * Licence: GPL v2 (kernel module requirement).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>

/*
 * SLM-OS's non-cacheable region on Jetson Orin Nano — the 2 MB block
 * at the top of RAM region 1, just below the OP-TEE carveout. Every
 * buffer SLM-OS's xHCI driver hands to the controller (DCBAA,
 * scratchpads, command/event ring TRBs, ERST) comes out of this
 * range via `ncmem_alloc`. See kernel/mm/vmm.c and kernel/CLAUDE.md
 * "Non-Cacheable Shared Memory" for why the region is where it is.
 *
 * Adding an identity mapping covering these 2 MB to the xusb stream's
 * existing Linux-programmed context bank lets SLM-OS program DCBAAP
 * / CRCR with raw physical addresses and have them translate through
 * the SMMU unchanged (IOVA == PA). No bypass, no context-bank
 * reprogramming, no risk of racing in-flight DMA.
 */
#define SLMOS_NC_BASE   0xBDE00000UL
#define SLMOS_NC_SIZE   (2UL * 1024 * 1024)   /* 2 MB */

static bool iommu_mapping_added;

/*
 * Ask Linux's iommu subsystem to add an identity mapping for SLM-OS's
 * 2 MB NC region on the xusb stream's active domain. After this call,
 * HC DMAs the SLM-OS xHCI driver issues with IOVA == 0xBDE00000+
 * translate straight through to the same physical addresses — no
 * bypass, no context-bank replacement, no race with other live DMA
 * on this or any other SMMU instance.
 */
static int arm_smmu_noshutdown_map_slmos_region(void)
{
    struct device *xusb_dev;
    struct iommu_domain *domain;
    int rc;

    /* The tegra-xusb platform device the xHCI host controller is
     * bound to. Name is the DT unit-address form ("@3610000" →
     * "3610000.usb"). Found at boot time; the device is stable for
     * the life of the kernel. */
    xusb_dev = bus_find_device_by_name(&platform_bus_type, NULL,
                                        "3610000.usb");
    if (!xusb_dev) {
        pr_warn("arm-smmu-noshutdown: tegra-xusb 3610000.usb not "
                "found — cannot add SLM-OS IOMMU mapping\n");
        return -ENODEV;
    }

    domain = iommu_get_domain_for_dev(xusb_dev);
    if (!domain) {
        pr_warn("arm-smmu-noshutdown: xusb has no attached "
                "iommu_domain — cannot add SLM-OS IOMMU mapping\n");
        put_device(xusb_dev);
        return -ENODEV;
    }

    pr_info("arm-smmu-noshutdown: xusb iommu_domain type=%d "
            "(IOMMU_DOMAIN_DMA=%d, IDENTITY=%d, UNMANAGED=%d)\n",
            domain->type, IOMMU_DOMAIN_DMA, IOMMU_DOMAIN_IDENTITY,
            IOMMU_DOMAIN_UNMANAGED);

    rc = iommu_map(domain, SLMOS_NC_BASE, SLMOS_NC_BASE, SLMOS_NC_SIZE,
                   IOMMU_READ | IOMMU_WRITE);
    if (rc) {
        pr_warn("arm-smmu-noshutdown: iommu_map(IOVA=0x%lx PA=0x%lx "
                "size=0x%lx) failed: %d\n",
                SLMOS_NC_BASE, SLMOS_NC_BASE, SLMOS_NC_SIZE, rc);
    } else {
        iommu_mapping_added = true;
        pr_info("arm-smmu-noshutdown: added identity IOMMU mapping "
                "IOVA 0x%lx..0x%lx (PA identical) on xusb domain "
                "(#266 Phase 3A Path 1 option 1)\n",
                SLMOS_NC_BASE, SLMOS_NC_BASE + SLMOS_NC_SIZE);
    }

    put_device(xusb_dev);
    return rc;
}

static int __init arm_smmu_noshutdown_init(void)
{
    struct device_driver *drv = driver_find("arm-smmu", &platform_bus_type);
    struct platform_driver *pdrv;

    if (!drv) {
        pr_warn("arm-smmu-noshutdown: arm-smmu driver not found — no-op\n");
        return 0;
    }

    pdrv = to_platform_driver(drv);

    if (pdrv->shutdown) {
        /*
         * Plain pointer store. Safe because platform_drv_shutdown()
         * only reads this field under device_shutdown() on the
         * reboot / kexec / poweroff path, which runs single-threaded
         * after all userspace is gone. At module-load time no kexec
         * is in flight, so no concurrent reader exists. By the time
         * one appears the new value is already in place.
         */
        pdrv->shutdown = NULL;
        pr_info("arm-smmu-noshutdown: NULLed arm-smmu "
                "platform_driver->shutdown (#266 Phase 3A) — "
                "arm_smmu_device_shutdown() will no longer fire at "
                "kexec\n");
    } else {
        pr_info("arm-smmu-noshutdown: platform_driver->shutdown "
                "already NULL — no-op\n");
    }

    /* Independent of the shutdown suppression above — if this fails
     * we still want the shutdown-suppression half of the fix to take
     * effect, and the failure mode is observable in dmesg. */
    (void)arm_smmu_noshutdown_map_slmos_region();

    return 0;
}

static void __exit arm_smmu_noshutdown_exit(void)
{
    /*
     * Reverse the IOMMU mapping on rmmod so a subsequent reload of
     * a rebuilt module doesn't see `iommu_map` fail with -EEXIST.
     * `platform_driver.shutdown` is deliberately NOT restored —
     * load-and-forget model; restoring the pointer would reopen the
     * exact failure we came here to prevent.
     */
    if (iommu_mapping_added) {
        struct device *xusb_dev =
            bus_find_device_by_name(&platform_bus_type, NULL,
                                     "3610000.usb");
        if (xusb_dev) {
            struct iommu_domain *domain =
                iommu_get_domain_for_dev(xusb_dev);
            if (domain) {
                size_t unmapped = iommu_unmap(domain, SLMOS_NC_BASE,
                                               SLMOS_NC_SIZE);
                pr_info("arm-smmu-noshutdown: iommu_unmap returned "
                        "%zu bytes (expected %lu)\n",
                        unmapped, SLMOS_NC_SIZE);
            }
            put_device(xusb_dev);
        }
        iommu_mapping_added = false;
    }
}

module_init(arm_smmu_noshutdown_init);
module_exit(arm_smmu_noshutdown_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Field-fix for arm-smmu .shutdown suppression "
                   "across kexec into SLM-OS. See #266, #285.");
