/*
 * arm-smmu-noshutdown — Linux kernel module that prepares the L4T
 * kexec path for a clean handoff into SLM-OS.
 *
 * Two things happen on insmod, and both are necessary for SLM-OS's
 * xHCI driver to reach `NO_OP round-trip OK` after the handoff:
 *
 *   1. Neutralise arm-smmu's platform_driver.shutdown so
 *      arm_smmu_device_shutdown() stops firing from
 *      device_shutdown() on kexec. This keeps the SMMU's clocks
 *      alive and leaves Linux's IOVA-bound context banks in place
 *      across the handoff.
 *
 *   2. Ask Linux's iommu subsystem to add an identity mapping
 *      (IOVA == PA) for SLM-OS's 2 MB NC region on the xusb
 *      stream's active iommu_domain, so SLM-OS can program DCBAAP
 *      / CRCR with raw physical addresses and have them translate
 *      straight through.
 *
 * See docs/jetson-usb-networking-plan.md §10.3 and §10.8 for the
 * full investigation, including the struct-field bug the previous
 * template had (NULLing the wrong field on device_driver) and the
 * four alternative approaches that were tested and ruled out before
 * converging on the identity-mapping design.
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

#include "arm_smmu_noshutdown.h"

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
                if (unmapped == SLMOS_NC_SIZE) {
                    pr_info("arm-smmu-noshutdown: iommu_unmap "
                            "returned %zu bytes (expected %lu)\n",
                            unmapped, SLMOS_NC_SIZE);
                } else {
                    /* Partial unmap leaks IOMMU page-table entries
                     * across a subsequent reload — flag loudly so a
                     * reboot can be considered before kexec. */
                    pr_warn("arm-smmu-noshutdown: iommu_unmap "
                            "returned %zu bytes (expected %lu) — "
                            "partial unmap, IOMMU state may be "
                            "inconsistent\n",
                            unmapped, SLMOS_NC_SIZE);
                }
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
MODULE_DESCRIPTION("Suppress arm-smmu .shutdown and install identity "
                   "IOMMU mapping for the xusb stream across kexec "
                   "into SLM-OS. See #266, #285.");
