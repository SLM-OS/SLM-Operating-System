/*
 * tegra-xusb-noshutdown — Linux kernel module that NULLs the
 * tegra-xusb platform driver's .shutdown callback.
 *
 * Purpose: SLM-OS #266 Phase 3A Option A.4. Linux's kexec path calls
 * `device_shutdown()` which invokes tegra-xusb's `.shutdown()`, which
 * halts the XUSB Falcon microcontroller. The halted Falcon leaves
 * the xHCI controller in a state where USBCMD.RUN=1 from SLM-OS
 * wedges the MMIO aperture (issue #285). NULLing the .shutdown
 * pointer makes `device_shutdown()` skip tegra-xusb, leaving the
 * Falcon alive and the controller usable post-kexec.
 *
 * Usage:
 *   insmod tegra_xusb_noshutdown.ko
 *   # ... then kexec ...
 *
 * The module's only side effect is the one pointer write. Unloading
 * does not restore the original pointer (kept NULL until reboot),
 * but the module can be unloaded without issue — the driver just
 * continues to have no shutdown hook.
 *
 * Licence: GPL v2, because the kernel API requires it.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>

static int __init tegra_xusb_noshutdown_init(void)
{
    struct device_driver *drv = driver_find("tegra-xusb", &platform_bus_type);
    if (!drv) {
        pr_warn("tegra-xusb-noshutdown: tegra-xusb driver not found — no-op\n");
        return 0;
    }

    if (drv->shutdown) {
        drv->shutdown = NULL;
        pr_info("tegra-xusb-noshutdown: NULLed tegra-xusb driver->shutdown "
                "(#266 Phase 3A A.4) — Falcon will survive kexec\n");
    } else {
        pr_info("tegra-xusb-noshutdown: shutdown already NULL — no-op\n");
    }
    return 0;
}

static void __exit tegra_xusb_noshutdown_exit(void)
{
    /*
     * Deliberately do NOT restore the original pointer. If we unload
     * this module before kexec, someone probably wants to undo us —
     * but restoring requires remembering the old pointer across load
     * boundaries, and the module is supposed to be load-and-forget
     * until reboot anyway.
     */
}

module_init(tegra_xusb_noshutdown_init);
module_exit(tegra_xusb_noshutdown_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS (John Jezl)");
MODULE_DESCRIPTION("Skip tegra-xusb .shutdown so SLM-OS can take over "
                   "the XHCI controller post-kexec. See #266, #285.");
