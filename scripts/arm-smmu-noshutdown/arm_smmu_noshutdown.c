/*
 * arm-smmu-noshutdown — Linux kernel module that NULLs the arm-smmu
 * platform driver's .shutdown callback, so Linux's kexec path
 * preserves the SMMU's translation state across the handoff.
 *
 * Purpose: SLM-OS #266 Phase 3A Option A.5 (successor to A.4).
 * A.4 targeted tegra-xusb but discovered that driver has no
 * .shutdown callback at all. The REAL blocker is arm-smmu's
 * shutdown path, which writes sCR0 |= CLIENTPD (disables all
 * client-port translation) before disabling the SMMU's clocks.
 * After that, any DMA transaction from the xHCI controller faults
 * internally, wedging the MMIO aperture when SLM-OS writes
 * USBCMD.RUN=1.
 *
 * NULLing arm-smmu's .shutdown pointer makes device_shutdown()
 * skip the CLIENTPD write, leaving translations live. The SMMU
 * context banks that Linux programmed for the xusb stream (the
 * IOVA mappings for DCBAAP and friends) remain valid, and SLM-OS's
 * first DMA on RUN=1 goes through cleanly.
 *
 * Trade-off: with translations live during the Linux→SLM-OS
 * transition, any residual DMA activity from other devices (NVMe,
 * network, audio) can write to memory they're mapped into. In
 * practice those drivers' own .shutdown callbacks fire first and
 * drain their pipelines — so the window where live translations
 * could cause trouble is small. Confirmed in local testing that
 * SLM-OS boots cleanly through this path.
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
#include <linux/platform_device.h>

static int __init arm_smmu_noshutdown_init(void)
{
    struct device_driver *drv = driver_find("arm-smmu", &platform_bus_type);
    if (!drv) {
        pr_warn("arm-smmu-noshutdown: arm-smmu driver not found — no-op\n");
        return 0;
    }

    if (drv->shutdown) {
        /*
         * No lock / WRITE_ONCE / barrier around this store. It's
         * safe because `drv->shutdown` is only ever invoked from
         * `device_shutdown()` on the kernel's reboot / kexec /
         * poweroff path, which runs single-threaded after all
         * userspace is torn down. At module-load time no reboot
         * is in flight, so no concurrent reader exists. The
         * kexec-path `device_shutdown()` walks drv->shutdown
         * once, and by the time it does this module has long
         * since finished loading.
         */
        drv->shutdown = NULL;
        pr_info("arm-smmu-noshutdown: NULLed arm-smmu driver->shutdown "
                "(#266 Phase 3A A.5) — SMMU translations will survive kexec\n");
    } else {
        pr_info("arm-smmu-noshutdown: shutdown already NULL — no-op\n");
    }
    return 0;
}

static void __exit arm_smmu_noshutdown_exit(void)
{
    /*
     * Do NOT restore the original pointer. Same rationale as the
     * tegra-xusb variant — load-and-forget-until-reboot model.
     */
}

module_init(arm_smmu_noshutdown_init);
module_exit(arm_smmu_noshutdown_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Skip arm-smmu .shutdown so SMMU translations "
                   "survive kexec into SLM-OS. See #266, #285.");
