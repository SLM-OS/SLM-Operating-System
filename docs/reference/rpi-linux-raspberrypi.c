// SPDX-License-Identifier: GPL-2.0
//
// Cached excerpt from raspberrypi/linux rpi-6.12.y branch:
//   drivers/firmware/raspberrypi.c
//
// Source URL:
//   https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/firmware/raspberrypi.c
//
// Saved per project policy (CLAUDE.md "Reference File Cache").
// Only the reboot-related portions are reproduced verbatim. Refer to the
// upstream file for the full driver.
//
// -----------------------------------------------------------------------------
// Lines 185-224  -- the LINUX_REBOOT_CMD_RESTART2 ("tryboot") handler
// -----------------------------------------------------------------------------

static int rpi_firmware_notify_reboot(struct notifier_block *nb,
				      unsigned long action,
				      void *data)
{
	struct rpi_firmware *fw;
	struct platform_device *pdev = g_pdev;
	u32 reboot_flags = 0;

	if (!pdev)
		return 0;

	fw = platform_get_drvdata(pdev);
	if (!fw)
		return 0;

	// The partition id is the first parameter followed by zero or
	// more flags separated by spaces indicating the reason for the reboot.
	//
	// 'tryboot': Sets a one-shot flag which is cleared upon reboot and
	//            causes the tryboot.txt to be loaded instead of config.txt
	//            by the bootloader and the start.elf firmware.
	//
	//            This is intended to allow automatic fallback to a known
	//            good image if an OS/FW upgrade fails.
	//
	// N.B. The firmware mechanism for storing reboot flags may vary
	// on different Raspberry Pi models.
	if (data && strstr(data, " tryboot"))
		reboot_flags |= 0x1;

	// The mailbox might have been called earlier, directly via vcmailbox
	// so only overwrite if reboot flags are passed to the reboot command.
	if (reboot_flags)
		(void)rpi_firmware_property(fw, RPI_FIRMWARE_SET_REBOOT_FLAGS,
				&reboot_flags, sizeof(reboot_flags));

	(void)rpi_firmware_property(fw, RPI_FIRMWARE_NOTIFY_REBOOT, NULL, 0);

	return 0;
}

// -----------------------------------------------------------------------------
// Lines 499-519  -- registration via legacy reboot notifier
// -----------------------------------------------------------------------------

static struct notifier_block rpi_firmware_reboot_notifier = {
	.notifier_call = rpi_firmware_notify_reboot,
};

static int __init rpi_firmware_init(void)
{
	int ret = register_reboot_notifier(&rpi_firmware_reboot_notifier);
	if (ret)
		goto out1;
	ret = platform_driver_register(&rpi_firmware_driver);
	if (ret)
		goto out2;

	return 0;

out2:
	unregister_reboot_notifier(&rpi_firmware_reboot_notifier);
out1:
	return ret;
}
core_initcall(rpi_firmware_init);

// -----------------------------------------------------------------------------
// Property tag enum values from include/soc/bcm2835/raspberrypi-firmware.h
// (rpi-6.12.y):
//
//   RPI_FIRMWARE_NOTIFY_REBOOT      = 0x00030048
//   RPI_FIRMWARE_GET_REBOOT_FLAGS   = 0x00030064
//   RPI_FIRMWARE_SET_REBOOT_FLAGS   = 0x00038064
//
// Mailbox property channel = 8 (MBOX_CHAN_PROPERTY in raspberrypi.c).
// -----------------------------------------------------------------------------
