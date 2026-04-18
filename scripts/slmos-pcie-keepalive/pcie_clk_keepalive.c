/*
 * pcie_clk_keepalive.c — Hold PCIe C8 clocks through kexec for SLM-OS
 *
 * Experiment for SLM-OS issue #25. When SLM-OS is kexec'd from Linux
 * on a Jetson Orin Nano Super Developer Kit, something in the kexec
 * shutdown path gates the `pex2_c8_core` BPMP clock. SLM-OS then
 * sees PCIe C8 APPL/DBI MMIO as 0xFFFFFFFF.
 *
 * User-space refcount bumps via /sys/kernel/debug/bpmp/debug/clk/...
 * don't survive kexec. This module tests whether grabbing the clock
 * via `clk_prepare_enable` at the *kernel* level — and NOT releasing
 * it on module exit — holds the clock through the transition.
 *
 * Usage:
 *   make -C $(pwd) modules
 *   insmod pcie_clk_keepalive.ko
 *   # Then: kexec -e /path/to/slmos.elf
 *   # In SLM-OS: rtldiag should show APPL != 0xFFFFFFFF if it worked
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/err.h>

static struct clk *g_clk_core;
static struct clk *g_clk_core_m;

static int __init pcie_clk_keepalive_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	int ret;

	np = of_find_node_by_path("/bus@0/pcie@140a0000");
	if (!np) {
		pr_err("pcie-clk-keepalive: no DT node pcie@140a0000\n");
		return -ENODEV;
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		pr_err("pcie-clk-keepalive: no platform_device for pcie@140a0000\n");
		return -ENODEV;
	}

	g_clk_core = clk_get(&pdev->dev, "core");
	if (IS_ERR(g_clk_core)) {
		pr_err("pcie-clk-keepalive: clk_get(core) failed: %ld\n",
		       PTR_ERR(g_clk_core));
		g_clk_core = NULL;
		return -ENODEV;
	}

	ret = clk_prepare_enable(g_clk_core);
	if (ret) {
		pr_err("pcie-clk-keepalive: clk_prepare_enable(core) = %d\n", ret);
		clk_put(g_clk_core);
		g_clk_core = NULL;
		return ret;
	}

	/* core_m is optional per pcie-tegra194 */
	g_clk_core_m = clk_get(&pdev->dev, "core_m");
	if (!IS_ERR(g_clk_core_m)) {
		clk_prepare_enable(g_clk_core_m);
	} else {
		g_clk_core_m = NULL;
	}

	pr_info("pcie-clk-keepalive: holding core clock at %lu Hz (refcount pinned)\n",
	        clk_get_rate(g_clk_core));
	return 0;
}

static void __exit pcie_clk_keepalive_exit(void)
{
	/*
	 * DELIBERATELY do NOT release the clock. The whole point of this
	 * module is to hold a kernel-level reference through kexec.
	 * Releasing on rmmod would defeat the experiment.
	 *
	 * Side effect: module can't be cleanly unloaded on a non-kexec
	 * reboot without warnings. That's acceptable — this is a lab
	 * experiment, not production.
	 */
	pr_info("pcie-clk-keepalive: exiting; clock reference intentionally retained\n");
}

module_init(pcie_clk_keepalive_init);
module_exit(pcie_clk_keepalive_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("SLM-OS Capstone");
MODULE_DESCRIPTION("Hold PCIe C8 clocks through kexec for SLM-OS bare-metal access (#25)");
