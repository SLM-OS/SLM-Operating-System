/*
 * smmu-probe — diagnostic kernel module that introspects the live
 * `arm-smmu` platform_driver and prints the runtime values of its
 * probe / remove / shutdown / pm pointers, plus every device bound
 * to it. Built alongside `arm_smmu_noshutdown.ko` so the field-fix
 * in #266 Phase 3A Path 1 can be verified before and after.
 *
 * This module is the functional test for `arm_smmu_noshutdown`:
 * there is no practical way to unit-test a Linux kernel module that
 * patches another built-in driver's struct, so verification runs on
 * hardware. Load smmu-probe, check dmesg, load arm_smmu_noshutdown,
 * reload smmu-probe, check dmesg again. See this directory's
 * README.md §Verification.
 *
 * Expected output BEFORE loading arm_smmu_noshutdown on L4T 5.15:
 *
 *   smmu-probe:   .shutdown = arm_smmu_device_shutdown+0x0/0x40
 *
 * Expected output AFTER loading arm_smmu_noshutdown (fixed field):
 *
 *   smmu-probe:   .shutdown = (null)
 *
 * Licence: GPL v2.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/platform_device.h>
#include <linux/kallsyms.h>

static const char *lookup_name(unsigned long addr, char *buf, size_t buflen)
{
    if (!addr) {
        snprintf(buf, buflen, "(null)");
        return buf;
    }
    sprint_symbol(buf, addr);
    if (buf[0] == '\0')
        snprintf(buf, buflen, "%pS", (void *)addr);
    return buf;
}

static int print_bound_dev(struct device *dev, void *data)
{
    pr_info("smmu-probe:   bound dev=%s\n", dev_name(dev));
    return 0;
}

static int __init smmu_probe_init(void)
{
    struct device_driver *drv;
    struct platform_driver *pdrv;
    char nbuf[KSYM_SYMBOL_LEN];

    pr_info("smmu-probe: starting\n");

    drv = driver_find("arm-smmu", &platform_bus_type);
    if (!drv) {
        pr_warn("smmu-probe: arm-smmu not found on platform_bus_type\n");
        return 0;
    }
    pdrv = to_platform_driver(drv);

    pr_info("smmu-probe: arm-smmu driver @ %px\n", pdrv);
    pr_info("smmu-probe:   .probe    = %s\n",
            lookup_name((unsigned long)pdrv->probe, nbuf, sizeof(nbuf)));
    pr_info("smmu-probe:   .remove   = %s\n",
            lookup_name((unsigned long)pdrv->remove, nbuf, sizeof(nbuf)));
    pr_info("smmu-probe:   .shutdown = %s\n",
            lookup_name((unsigned long)pdrv->shutdown, nbuf, sizeof(nbuf)));
    pr_info("smmu-probe:   .driver.pm= %s\n",
            lookup_name((unsigned long)pdrv->driver.pm, nbuf, sizeof(nbuf)));
    pr_info("smmu-probe:   .driver.bus->shutdown = %s\n",
            lookup_name((unsigned long)pdrv->driver.bus->shutdown,
                        nbuf, sizeof(nbuf)));

    (void)driver_for_each_device(drv, NULL, NULL, print_bound_dev);

    pr_info("smmu-probe: done\n");
    return 0;
}

static void __exit smmu_probe_exit(void)
{
    pr_info("smmu-probe: unloading\n");
}

module_init(smmu_probe_init);
module_exit(smmu_probe_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Diagnostic: report arm-smmu platform_driver "
                   "callbacks + bound devices. Companion to "
                   "arm_smmu_noshutdown.ko. See #266 Phase 3A.");
