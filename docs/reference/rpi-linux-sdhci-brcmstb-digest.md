# sdhci-brcmstb.c digest (Pi 5 EMMC2 driver)

Source: https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/mmc/host/sdhci-brcmstb.c
Fetched: 2026-04-24

Pi 5 binds EMMC2 to this driver via DT compatible `brcm,bcm2712-sdhci`
(see arch/arm64/boot/dts/broadcom/bcm2712.dtsi node `sdio1@fff000`,
secondary block name `brcm,sdhci-brcmstb`). The historic name
"emmc2" / `brcm,bcm2711-emmc2` from sdhci-iproc.c is not used on Pi 5.

## of_match_table (lines 563-570)

    static const struct of_device_id __maybe_unused sdhci_brcm_of_match[] = {
        { .compatible = "brcm,bcm2712-sdhci", .data = &match_priv_2712 },
        ...
    };

## match_priv_2712 (lines ~544)

    static const struct brcmstb_match_priv match_priv_2712 = {
        .flags    = BRCMSTB_MATCH_FLAGS_USE_CARD_BUSY,
        .hs400es  = sdhci_brcmstb_hs400es,
        .cfginit  = sdhci_brcmstb_cfginit_2712,
        .ops      = &sdhci_brcmstb_ops_2712,
    };

## sdhci_brcmstb_ops_2712 (.reset = brcmstb_reset)

    static struct sdhci_ops sdhci_brcmstb_ops_2712 = {
        .set_clock         = sdhci_bcm2712_set_clock,
        .set_power         = sdhci_brcmstb_set_power,
        .set_bus_width     = sdhci_set_bus_width,
        .reset             = brcmstb_reset,
        .set_uhs_signaling = sdhci_set_uhs_signaling,
        .init_sd_express   = bcm2712_init_sd_express,
    };

    static void brcmstb_reset(struct sdhci_host *host, u8 mask)
    {
        sdhci_and_cqhci_reset(host, mask);
        /* Reset will clear this, so re-enable it */
        enable_clock_gating(host);
    }

## Probe — clock acquisition (lines 596-599)

    clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
    if (IS_ERR(clk))
        return dev_err_probe(&pdev->dev, PTR_ERR(clk),
            "Failed to get and enable clock from Device Tree\n");

So the driver DOES call clk_prepare_enable on the EMMC2 gate clock
itself (via devm_clk_get_optional_enabled). It does not trust VC
firmware to have left the clock enabled.

Optional secondary clock "sdio_freq" set if `clock-frequency` DT
property exists (lines 716-731): clk_set_rate then clk_get_rate.

## No SDHCI_QUIRK_NO_CARD_NO_RESET set anywhere

Only quirks set on bcm2712 are derived from match_priv flags (none of
the flag bits map to NO_CARD_NO_RESET), and a comment-free
`mmc_of_parse()` consumption of DT props.

## sdhci_brcmstb_add_host (lines 754-)

For non-CQE path (most cases): direct `sdhci_add_host(host)`.
For CQE path: `sdhci_setup_host()` then `__sdhci_add_host()`.
Both __sdhci_add_host paths reach the generic sdhci_init() ->
sdhci_reset_for_all() path.

## Generic sdhci.c flow (rpi-6.12.y)

drivers/mmc/host/sdhci.c sdhci_init():

    static void sdhci_init(struct sdhci_host *host, int soft)
    {
        ...
        if (soft)
            sdhci_reset_for(host, INIT);
        else
            sdhci_reset_for_all(host);   /* SDHCI_RESET_ALL */
        ...
    }

`sdhci_reset_for_all` -> `sdhci_do_reset(host, SDHCI_RESET_ALL)` ->
host->ops->reset = brcmstb_reset -> sdhci_and_cqhci_reset (full
SDHCI_RESET_ALL).

Call sites of sdhci_init(host, ...) in sdhci.c:
  line 2079 (sdhci_reinit, soft=0)
  line 2100 (sdhci_set_ios)
  line 3572 (sdhci_resume_host, soft=0)
  line 3577 (sdhci_resume_host, soft=mmc->pm_flags&MMC_PM_KEEP_POWER)
  Plus __sdhci_add_host -> sdhci_init(host, 0) (RESET_ALL on probe).

## MMC core CMD0 path

drivers/mmc/core/mmc_ops.c:154-189  mmc_go_idle()
    cmd.opcode = MMC_GO_IDLE_STATE;   /* CMD0 */
    cmd.arg    = 0;
    cmd.flags  = MMC_RSP_SPI_R1 | MMC_RSP_NONE | MMC_CMD_BC;
    err = mmc_wait_for_cmd(host, &cmd, 0);

drivers/mmc/core/core.c:2641-2690  mmc_rescan_try_freq()
    mmc_power_up(host, host->ocr_avail);
    mmc_hw_reset_for_init(host);          /* eMMC HW reset if available */
    if (!(caps2 & MMC_CAP2_NO_SDIO)) sdio_reset(host);
    mmc_go_idle(host);                    /* CMD0 always issued */
    /* then mmc_attach_sdio / mmc_attach_sd / mmc_attach_mmc */

drivers/mmc/core/mmc.c:2876-  mmc_attach_mmc()
    mmc_set_bus_mode(host, MMC_BUSMODE_OPENDRAIN);
    mmc_send_op_cond(host, 0, &ocr);  /* CMD1 */

## Verdict

Linux performs a full cold init of EMMC2 on Pi 5 — it does NOT trust
VideoCore firmware state:

  1. Probe: devm_clk_get_optional_enabled() acquires + enables the
     EMMC2 gate clock (sdhci-brcmstb.c:596).
  2. Probe -> __sdhci_add_host -> sdhci_init(host, 0) ->
     sdhci_reset_for_all -> SDHCI_RESET_ALL via brcmstb_reset.
  3. cfginit_2712 reprograms SDIO_CFG_MAX_50MHZ_MODE,
     SDIO_CFG_CTRL (force CD), SDIO_CFG_CQ_CAPABILITY.
  4. MMC core mmc_rescan_try_freq: mmc_power_up,
     mmc_hw_reset_for_init, sdio_reset, mmc_go_idle (CMD0),
     then attach SDIO/SD/MMC (which issues CMD1/CMD8 etc.).

No `SDHCI_QUIRK_NO_CARD_NO_RESET` is set for bcm2712. No comments
mention VideoCore. No code paths skip reset under "firmware already
initialized" assumptions.
