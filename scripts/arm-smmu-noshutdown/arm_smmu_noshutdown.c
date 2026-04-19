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
#include <linux/platform_device.h>

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
    return 0;
}

static void __exit arm_smmu_noshutdown_exit(void)
{
    /*
     * Do NOT restore the original pointer. The module is designed
     * for a load-and-forget-until-reboot model: once the platform
     * driver's shutdown pointer has been cleared, the next kexec
     * will skip SMMU teardown and the Linux half of the system is
     * about to hand off to SLM-OS anyway.
     */
}

module_init(arm_smmu_noshutdown_init);
module_exit(arm_smmu_noshutdown_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Field-fix for arm-smmu .shutdown suppression "
                   "across kexec into SLM-OS. See #266, #285.");
