# NVCSI driver notes (from L4T code-read)

Code-read of NVIDIA's Linux for Tegra (L4T) NVCSI sources, distilled to
what SLM-OS needs to bring up single-shot RAW10 capture from the IMX219
on Jetson Orin Nano (Tegra234). Companion to the camera bring-up plan in
`docs/jetson-camera-imx219-plan.md`.

**The headline finding is that on Tegra234, the L4T driver does no
direct NVCSI MMIO at all.** Every hardware programming step is
forwarded to the Camera RTCPU (Cortex-R5) over IVC. See
"RTCPU dependency" below — it is the single most important question
this document resolves.

## Source & license

All cached files come from the OE4T mirror of the L4T R35.6.1 kernel
tree at:

  `https://github.com/OE4T/linux-tegra-5.10` — branch
  `oe4t-patches-l4t-r35.6.1`, commit `4e110b9` (current as of fetch).

Files cached in `docs/reference/` (`l4t-` prefix):

| Cached file                       | Upstream path                                                                      |
|-----------------------------------|------------------------------------------------------------------------------------|
| `l4t-nvcsi.c` / `.h`              | `nvidia/drivers/video/tegra/host/nvcsi/nvcsi.{c,h}`                                |
| `l4t-nvcsi-t194.c` / `.h`         | `nvidia/drivers/video/tegra/host/nvcsi/nvcsi-t194.{c,h}`                           |
| `l4t-nvcsi-deskew.c` / `.h`       | `nvidia/drivers/video/tegra/host/nvcsi/deskew.{c,h}`                               |
| `l4t-csi-framework.c`             | `nvidia/drivers/media/platform/tegra/camera/csi/csi.c`                             |
| `l4t-csi4_fops.c`                 | `nvidia/drivers/media/platform/tegra/camera/csi/csi4_fops.c` (T194 direct-MMIO; **dead on R35**) |
| `l4t-csi5_fops.c` / `.h`          | `nvidia/drivers/media/platform/tegra/camera/nvcsi/csi5_fops.{c,h}` (T234 path)     |
| `l4t-csi4_registers.h`            | `nvidia/include/media/csi4_registers.h`                                            |
| `l4t-nvhost_nvcsi_ioctl.h`        | `nvidia/include/uapi/linux/nvhost_nvcsi_ioctl.h`                                   |
| `l4t-capture-vi.c` / `.h`         | `nvidia/drivers/media/platform/tegra/camera/fusa-capture/capture-vi.{c,h}`         |
| `l4t-capture-vi-channel.c` / `.h` | `nvidia/drivers/media/platform/tegra/camera/fusa-capture/capture-vi-channel.{c,h}` |
| `l4t-capture-common.c` / `.h`     | `nvidia/drivers/media/platform/tegra/camera/fusa-capture/capture-common.{c,h}`     |
| `l4t-camrtc-capture-messages.h`   | `nvidia/include/soc/tegra/camrtc-capture-messages.h`                               |
| `l4t-rtcpu-capture-ivc.c`         | `nvidia/drivers/platform/tegra/rtcpu/capture-ivc.c`                                |

License: every header carries `SPDX-License-Identifier: GPL-2.0-only`
or the equivalent prose, copyright NVIDIA Corporation. Same caveat as
for any GPL Linux driver re-implementation: register addresses,
sequences, bit definitions, and IVC message layouts are facts about
the hardware and IPC contract, not copyrightable expression. Mode
tables and verbatim code blocks are not lifted; SLM-OS code derives
the sequence from these notes and writes its own implementation.

## Hardware overview

NVCSI is the Tegra234 MIPI CSI-2 receiver block, sitting between the
external D-PHY pads and the VI (Video Input) DMA engine. From the L4T
R35 device tree (`tegra234-soc-host1x.dtsi`):

- **MMIO base: `0x15A00000`**, parent is `host1x@13E00000` with
  identity `ranges;` (the unit address is the absolute physical
  address). Confirms the plan's "approximately 0x15a00000".
- **Compatible string:** `nvidia,tegra194-nvcsi` (T234 reuses the
  T194 binding — the underlying hardware MMIO layout is unchanged).
- **Block name in the Orin TRM:** "NVCSI" / "MIPI CSI-2 receiver".
- **Topology:** 4 PHY bricks, 2 CIL partitions per brick (CIL_A and
  CIL_B), giving up to 6 logical CSI ports (NVCSI_PORT_A through
  NVCSI_PORT_F) and a 7th E/F pair on Orin. Each CIL partition
  supports up to 2 D-PHY data lanes plus a clock lane, so a single
  port + CIL_A is the 2-lane configuration the IMX219 needs.
- **Datatypes:** all standard MIPI CSI-2 short and long packet
  datatypes. RAW10 (`0x2B`) is the IMX219 primary format.
- **Interrupts:** none directly on the NVCSI DT node; per-stream
  status is mirrored into VI's interrupt path via the `ERROR_STATUS2VI_*`
  registers (see register table). Frame completion is reported
  through VI, not NVCSI.

## Register map (relevant subset)

All offsets are relative to NVCSI MMIO base (`0x15A00000` on T234).
The L4T `csi4_registers.h` header is the canonical source. Per-stream
register windows live at `0x010000`, `0x020000`, `0x030000`
(streams 0/2/4) with each stream's CIL_B partition `+0x800` from
CIL_A. PHY bricks live at `0x18000` with `0x10000` stride.

Subset SLM-OS will need for single-port, 2-lane, RAW10 single-shot
capture (CIL_A, port 0, brick 0):

| Offset (within block)                             | Symbol                            | Purpose                                                                             |
|---------------------------------------------------|-----------------------------------|-------------------------------------------------------------------------------------|
| **PHY brick 0 — base `0x18000`**                  |                                   |                                                                                     |
| `+0x00`                                           | `NVCSI_CIL_PHY_CTRL`              | PHY mode select: `0` = D-PHY, `1` = C-PHY. Write `0` for IMX219.                    |
| `+0x04`                                           | `NVCSI_CIL_CONFIG`                | Lane count: bits[2:0] for CIL_A lanes, bits[10:8] for CIL_B. Write `2` for 2-lane CIL_A. |
| `+0x0C`                                           | `NVCSI_CIL_PAD_CONFIG`            | De-serializer power. Write `0` to power on; `PDVCLAMP (1<<9)` to power down.         |
| `+0x10`                                           | `NVCSI_CIL_LANE_SWIZZLE_CTRL`     | Lane swap. `0x0` for default IMX219 mapping.                                         |
| `+0x18`                                           | `NVCSI_CIL_A_SW_RESET`            | Soft reset bits `SW_RESET0_EN | SW_RESET1_EN = 0x3` to assert; `0` to release.       |
| `+0x20`                                           | `NVCSI_CIL_A_PAD_CONFIG`          | LP enables: `E_INPUT_LP_CLK | E_INPUT_LP_IO0 | E_INPUT_LP_IO1` (bits 20/21/22).      |
| `+0x58`                                           | `NVCSI_CIL_A_POLARITY_SWIZZLE_CTRL` | Per-lane polarity inversion. `0x0` if lane polarity matches DT default.            |
| `+0x5C`                                           | `NVCSI_CIL_A_CONTROL`             | Settle times. Pack: `DEFAULT_DESKEW_COMPARE | DEFAULT_DESKEW_SETTLE | T18X_BYPASS_LP_SEQ | (clk_settle<<8) | (ths_settle<<0)`. See "Init sequence" for value derivation. |
| `+0x7C`                                           | `NVCSI_CIL_B_SW_RESET`            | Same as CIL_A; assert + release for symmetry even if CIL_B unused.                   |
| `+0x84`                                           | `NVCSI_CIL_B_PAD_CONFIG`          | Power down CIL_B if unused: `PD_CLK | PD_IO0 | PD_IO1 | SPARE_IO0 | SPARE_IO1`.       |
| **Per-stream — base `0x10000` (stream 0)**        |                                   |                                                                                     |
| `+0x08`                                           | `PP_EN_CTRL`                      | Pixel parser enable. Write `CFG_PP_EN = 0x1` to start receiving.                     |
| `+0x20`                                           | `VC0_DT_OVERRIDE`                 | Force a datatype on VC0. Leave `0` to auto-detect from packet headers.               |
| `+0x6C`                                           | `PPFSM_TIMEOUT_CTRL`              | Pixel parser FSM timeout. `0` to disable.                                            |
| `+0x70`                                           | `PH_CHK_CTRL`                     | Packet header CRC + ECC check enable. `CFG_PH_CRC_CHK_EN | CFG_PH_ECC_CHK_EN = 0x3`. |
| `+0x90`                                           | `ERROR_STATUS2VI_MASK`            | Which CSI errors propagate to VI. For VC0 only: `CFG_ERR_STATUS2VI_MASK_VC0 = 0x1`.  |
| `+0x94`                                           | `ERROR_STATUS2VI_VC0`             | Per-VC0 error status latch. Read after capture for diagnostics.                      |
| `+0xA4`                                           | `INTR_STATUS`                     | Stream interrupt status. Write-1-to-clear.                                           |
| `+0xA8`                                           | `INTR_MASK`                       | Stream interrupt enable mask.                                                        |
| `+0xAC`                                           | `ERR_INTR_STATUS`                 | Error interrupt status. Write-1-to-clear.                                            |
| `+0xB0`                                           | `ERR_INTR_MASK`                   | Error interrupt enable mask.                                                         |

The default settle times the L4T driver uses (when DT does not
override) are `DEFAULT_DPHY_CLK_SETTLE = 0x21` and `cil_settletime`
calculated from the MIPI clock rate — see `tegra_csi_ths_settling_time`
in `l4t-csi-framework.c` for the formula.

Registers SLM-OS deliberately skips: TPG (test pattern generator) at
`0x0B8` and `0x194`–`0x1C0`, DPCM compression at `0x74`, deskew
calibration (`l4t-nvcsi-deskew.c`, only required above ~1.5 Gbps/lane
which is well above IMX219's 912 Mbps), MIPI cal (handled internally
by the PHY block on T234, see comment in `csi5_mipi_cal`).

## Init sequence (single-shot capture)

Numbered steps from "BPMP clocks enabled, MMIO mapped" to "first frame
arrives in the VI buffer". CIL_A on PHY brick 0, single port, 2 D-PHY
data lanes, RAW10, no virtual channel switching.

The L4T driver's sequence below is reconstructed from `csi4_phy_config()`
and `csi4_stream_init()` in `l4t-csi4_fops.c`. **Important:** that file
is the historical T194 direct-MMIO path. **On R35, no driver actually
links it in** (see "RTCPU dependency"). It is included here because it
is the only public reference for what the underlying hardware sequence
looks like — SLM-OS will need to either drive these registers directly
(if the CBB firewall permits and RTCPU is not required) or send the
equivalent IVC messages via RTCPU.

1. **Pre-conditions:** BPMP has enabled `TEGRA234_CLK_NVCSI` (typical
    rate 400 MHz, set in `t19_nvcsi_info.clocks`); NVCSI MMIO window
    at `0x15A00000` is mapped non-cached at EL2; the IMX219 sensor is
    out of reset and the I²C-side `MODE_SELECT` is still 0 (not
    streaming). No reset symbol is exposed in the T234 DT for NVCSI;
    soft reset is per-CIL via the `NVCSI_CIL_*_SW_RESET` registers.

2. **PHY mode select.** Write `DPHY = 0x0` to `NVCSI_CIL_PHY_CTRL`
    on brick 0 (offset `0x18000 + 0x00`).

3. **Clear CIL_A lane count** before reconfiguring. Read
    `NVCSI_CIL_CONFIG` (offset `+0x04`), mask off `DATA_LANE_A` bits,
    write back.

4. **Assert CIL_A soft reset.** Write `0x3`
    (`SW_RESET0_EN | SW_RESET1_EN`) to `NVCSI_CIL_A_SW_RESET`
    (`+0x18`).

5. **Park CIL_A pads.** Write
    `PD_CLK | PD_IO0 | PD_IO1 | SPARE_IO0 | SPARE_IO1` to
    `NVCSI_CIL_A_PAD_CONFIG` (`+0x20`). This powers down the lane
    receivers while reconfiguring.

6. **Park unused CIL_B.** Same `PD_*` pattern to
    `NVCSI_CIL_B_PAD_CONFIG` (`+0x84`); also assert
    `NVCSI_CIL_B_SW_RESET` (`+0x7C`).

7. **Power down brick de-serializer** (since CIL_B is unused):
    write `PDVCLAMP = (1 << 9)` to `NVCSI_CIL_PAD_CONFIG` (`+0x0C`).

8. **Power on de-serializer** (now that pad reset is settled): write
    `0` to `NVCSI_CIL_PAD_CONFIG` (`+0x0C`).

9. **Compute settle times.** From `tegra_csi_ths_settling_time` in
    `l4t-csi-framework.c`:
    `cil_settletime = (T_HS_PREPARE_min_ns + T_HS_ZERO_min_ns)
    * mipi_clk_mhz / 1000 - 1` clamped to a 6-bit field. For IMX219 at
    456 MHz × 2 lanes = 912 Mbps/lane the typical computed value is
    `0x14` (matches `DEFAULT_THS_SETTLE`). `csi_settletime` for the
    CIL clock domain is `tegra_csi_clk_settling_time(csi, cil_clk_mhz)`,
    typically `0x21` (`DEFAULT_DPHY_CLK_SETTLE`).

10. **Set CIL_A lane count.** Write
    `(cil_config & ~DATA_LANE_A) | (2 << DATA_LANE_A_OFFSET)` to
    `NVCSI_CIL_CONFIG` (`+0x04`). Field width is 3 bits at offset 0.

11. **Enable LP receivers on CIL_A pads.** Write
    `E_INPUT_LP_CLK | E_INPUT_LP_IO0 | E_INPUT_LP_IO1` (bits 20/21/22)
    to `NVCSI_CIL_A_PAD_CONFIG` (`+0x20`).

12. **Program CIL_A control / settle times.** Write
    `DEFAULT_DESKEW_COMPARE | DEFAULT_DESKEW_SETTLE
    | (csi_settletime << CLK_SETTLE_SHIFT)
    | T18X_BYPASS_LP_SEQ
    | (cil_settletime << THS_SETTLE_SHIFT)` to
    `NVCSI_CIL_A_CONTROL` (`+0x5C`). For a 912 Mbps/lane IMX219 mode
    this packs to roughly `0x00462114`.

13. **Release CIL_A soft reset.** Write `0x0` to
    `NVCSI_CIL_A_SW_RESET` (`+0x18`).

14. **Initialise stream 0 status registers.** All of these are
    write-1-to-clear; writing `0xFFFFFFFF` clears any latched bits
    from a previous probe. Block base: `0x010000`.
    - `CILA_INTR_STATUS` (`+0x400`) ← `0xFFFFFFFF`
    - `CILA_ERR_INTR_STATUS` (`+0x408`) ← `0xFFFFFFFF`
    - `CILA_INTR_MASK` (`+0x404`) ← `0xFFFFFFFF` (mask all CIL
      interrupts during single-shot — frame-done comes from VI)
    - `CILA_ERR_INTR_MASK` (`+0x40C`) ← `0xFFFFFFFF`
    - `INTR_STATUS` (`+0xA4`) ← `0x3FFFF`
    - `ERR_INTR_STATUS` (`+0xAC`) ← `0x7FFFF`
    - `ERROR_STATUS2VI_MASK` (`+0x90`) ← `0x0` (do not propagate to
      VI for the smoke-test path; flip to `0x1` once CSI is known
      good and errors should abort the capture)
    - `INTR_MASK` (`+0xA8`) ← `0x0`
    - `ERR_INTR_MASK` (`+0xB0`) ← `0x0`

15. **Stream config.** Same block base `0x010000`.
    - `PPFSM_TIMEOUT_CTRL` (`+0x6C`) ← `0`
    - `PH_CHK_CTRL` (`+0x70`) ← `CFG_PH_CRC_CHK_EN | CFG_PH_ECC_CHK_EN`
      (`0x3`)
    - `VC0_DPCM_CTRL` (`+0x74`) ← `0` (no DPCM)
    - `VC0_DT_OVERRIDE` (`+0x20`) ← `0` (auto-detect RAW10 from
      packet header)

16. **Enable pixel parser.** Write `CFG_PP_EN = 0x1` to `PP_EN_CTRL`
    (`+0x08`). NVCSI is now armed and waits for valid packets on the
    D-PHY lanes.

17. **Hand off to VI.** VI must already have a single-frame DMA
    descriptor configured against the target buffer; VI's
    frame-done interrupt fires when the buffer is full.

18. **Start the sensor.** Write `1` to IMX219's `MODE_SELECT` (`0x0100`)
    via I²C. The sensor immediately begins clocking out frames on the
    D-PHY lanes and NVCSI forwards them to VI.

19. **Wait for VI frame-done IRQ** — see the VI driver notes
    (separate document). NVCSI's own `INTR_STATUS` should remain
    clean for a successful capture; a non-zero value indicates a
    packet-header error or PP_FSM timeout.

20. **Stop:** clear `PP_EN_CTRL` to `0`, then write `MODE_SELECT = 0`
    on the sensor. Optionally re-assert CIL_A soft reset for a clean
    teardown.

Steps 2–16 above are pure NVCSI MMIO. **`[via RTCPU]` would replace
all of them with two IVC messages** — `CAPTURE_PHY_STREAM_OPEN_REQ`
plus `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` — see next section.

## RTCPU dependency

**Resolved answer: on Tegra234, the L4T driver routes all NVCSI
configuration through the Camera RTCPU IVC. There is no longer a
direct-MMIO bypass path in mainline L4T R35.**

Evidence from the code:

1. `l4t-csi5_fops.c` (the only `tegra_csi_fops` actually used on T234
   and on T194 in R35) does zero MMIO writes to NVCSI. Every
   operation — `csi5_stream_open`, `csi5_stream_close`,
   `csi5_stream_set_config`, TPG start/stop, MIPI cal — is wrapped in
   a `CAPTURE_CONTROL_MSG` and submitted with
   `vi_capture_control_message()` (which lands in
   `tegra_capture_ivc_control_submit()` in `l4t-rtcpu-capture-ivc.c`).
2. `csi5_mipi_cal()` returns 0 with the comment
   `/* Camera RTCPU handles MIPI calibration */` — explicit RTCPU
   ownership.
3. `nvcsi-t194.c::t194_nvcsi_late_probe()` at line 171 sets
   `nvcsi->csi.fops = &csi5_fops;` — even the T194 driver uses the
   IVC fops on R35. The `csi4_fops` symbol in `l4t-csi4_fops.c`
   (the historical direct-MMIO path) is **defined but never assigned
   anywhere in the R35 tree**. Confirmed by grepping the entire repo
   for `&csi4_fops` — zero hits. It is dead/legacy code retained for
   reference.
4. The only direct MMIO `nvcsi.c` itself touches is
   `nvcsi_cil_sw_reset()`, exported for the deskew path and called
   from `l4t-nvcsi-deskew.c` for high-speed CPHY links — not relevant
   to a 912 Mbps/lane DPHY IMX219.

**Implication for SLM-OS:** the question of whether SLM-OS can talk
directly to NVCSI MMIO from EL2 is now an empirical CBB firewall
question, not a Linux-API question. The L4T driver does not
demonstrate the direct-MMIO path on T234, so SLM-OS has two real
options:

- **Option A — Direct MMIO (what `csi4_fops.c` does on T194).** Use
  the 20-step sequence in "Init sequence" above. Requires the CBB
  firewall to permit reads and writes to `0x15A00000`–`0x15A3FFFF`
  from NS EL2-VHE. The hardware sequence itself is publicly
  documented (in dead L4T code), and the underlying NVCSI MMIO
  layout has not changed between T194 and T234 — `csi4_registers.h`
  offsets apply on Orin. Phase-0 CBB recon (per the plan) must
  confirm.
- **Option B — RTCPU IVC (what `csi5_fops.c` does on R35).** Stand
  up an IVC channel pair to the Camera RTCPU over the HSP doorbell,
  send `CAPTURE_PHY_STREAM_OPEN_REQ` (msg id `0x36`) followed by
  `CAPTURE_CSI_STREAM_SET_CONFIG_REQ`, both wrapped in
  `CAPTURE_MSG_HEADER` (8 bytes, see `l4t-camrtc-capture-messages.h`).
  Ports the existing BPMP IPC pattern (`docs/archive/plans/jetson-bpmp-ipc-plan.md`)
  to the camera-control IVC channel.

Practical IVC shape if SLM-OS goes Option B:

- Two channels, both `CAPTURE_IVC_ALIGN`-padded:
  - `nvidia,tegra186-camera-ivc-protocol-capture-control` —
    bidirectional, used for setup messages (stream open, stream
    config, sync-gen enable).
  - `nvidia,tegra186-camera-ivc-protocol-capture` — used for
    per-frame request and status; SLM-OS may not need this if VI's
    frame-done IRQ is sufficient.
- IVC framing is the standard `tegra-ivc` ring, same primitives
  SLM-OS already uses for BPMP. Doorbell is HSP shared mailbox.
- The minimum two-message setup for single-shot RAW10 capture:
  1. `CAPTURE_PHY_STREAM_OPEN_REQ` — `{stream_id, csi_port,
     phy_type=DPHY=0, pad32}` → returns `result=CAPTURE_OK` (0).
  2. `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` — embeds
     `nvcsi_brick_config` (DPHY mode, lane polarities) +
     `nvcsi_cil_config` (num_lanes=2, lp_bypass_mode=1,
     t_hs_settle, mipi_clock_rate_kHz) + `nvcsi_error_config`. RTCPU
     internally executes the equivalent of steps 2–16 above.

**Recommendation:** target Option A first because it eliminates an
entire IPC dependency (Camera RTCPU firmware loaded into the SoC
must already be running and SLM-OS must have IVC + HSP plumbing for
the camera channels, neither of which exists today). Phase 0 CBB
recon (one read at `0x15A00000`) is the gate; if blocked, fall back
to Option B.

**Update — 2026-04-26: Option A is blocked.** Hardware verification
on jetson-nano-1 shows that direct NVCSI MMIO is not accessible
from any AP context, regardless of clock / power-domain / reset
state:

- From SLM-OS at NS EL2-VHE: `peek 0x15a00000` returns `0xFFFFFFFF`.
  After enabling `TEGRA234_POWER_DOMAIN_VI` via `bpmp_pg_set_state`
  AND `TEGRA234_CLK_NVCSI` via `bpmp_clk_enable`, the readback
  remains `0xFFFFFFFF`. Writes silently fail —
  `poke 0x15a18004 0xCAFEBABE` followed by `peek` returns
  `0xFFFFFFFF`, the canonical "MMIO has no responder" pattern.
- From Linux EL1: `devmem 0x15a00000` also returns `0xFFFFFFFF`
  even while `gst-launch nvarguscamerasrc` is actively streaming
  frames through the camera. Linux's NVCSI driver does not access
  the MMIO directly either (csi5_fops on R35 is the only fops
  that's actually wired up; csi4_fops is dead code).
- `bpmp_reset_deassert(TEGRA234_RESET_NVCSI)` returns `-13`
  (EACCES) — BPMP refuses to deassert NVCSI's reset for AP-side
  callers, presumably because the camera RTCPU owns it.

**Phase 0's "NVCSI MMIO reachable" was a false positive.** The
recon probe almost certainly read a different address window or
caught NVCSI in a brief post-suspend race; the conclusion does not
hold under live verification. The architectural reality matches
the L4T R35 code-read: **NVCSI is RTCPU-exclusive on T234**.

Implication: SLM-OS must implement Option B (Camera RTCPU IVC) for
the camera bring-up to proceed past the NVCSI gate. The
existing `kernel/drivers/camera/nvcsi.c` direct-MMIO driver and
the `nvcsi` shell command are kept as a diagnostic that surfaces
the failure mode unambiguously (CIL_CONFIG readback mismatch with
all-ones, log message naming the underlying cause) so a future
maintainer doesn't re-walk this trail. The 20-step bring-up
sequence above remains the canonical reference for what the RTCPU
IVC will internally execute.

## Clocks and resets

From `nvidia/drivers/video/tegra/host/t194/t194.c::t19_nvcsi_info`:

```c
.clocks = { {"nvcsi", 400000000} },
```

Single clock, named `nvcsi`, target rate 400 MHz. The Tegra234 DT
node `nvcsi@15a00000` resolves the alias to:

- **`TEGRA234_CLK_NVCSI`** — sole clock for the block.

There is **no `TEGRA234_CLK_NVCSILP`** in the T234 binding (the LP
clock for low-power lane is now derived internally; on T194 there
were two clocks). The original plan in
`docs/jetson-camera-imx219-plan.md` line 207-208 lists both — that
should be updated to single-clock.

There is **no reset symbol** exposed in the T234 DT for NVCSI itself;
soft reset is per-CIL via the `NVCSI_CIL_A_SW_RESET` and
`NVCSI_CIL_B_SW_RESET` registers (programmed by the driver in steps
4 and 13 above). The companion plan to BPMP and the parallel
`tegra234_clocks.h` task should expect to define `TEGRA234_CLK_NVCSI`
but not `TEGRA234_RESET_NVCSI`.

VI has its own clock + reset list — covered in the VI driver notes.

## What's left out

SLM-OS deliberately skips:

- **V4L2 / media-controller framework.** `l4t-csi-framework.c`
  registers a `v4l2_subdev` and integrates with the Linux media
  controller graph. SLM-OS calls the equivalent steps directly from
  the camera component init.
- **Runtime PM** (`pm_runtime_get_sync` / `nvhost_module_busy`).
  SLM-OS keeps NVCSI clocked continuously while the camera component
  is loaded; no idle/resume cycle.
- **TPG (test pattern generator).** Useful for Linux to validate
  the CSI receiver without a sensor; SLM-OS exercises the real
  sensor instead.
- **Deskew calibration** (`l4t-nvcsi-deskew.c`). Required only for
  data rates above ~1.5 Gbps/lane (CPHY or 4-lane RAW12 sensors).
  IMX219 at 912 Mbps/lane does not need it.
- **Multi-stream** / virtual channel switching. SLM-OS uses VC0 only.
- **Error recovery** (`csi5_error_recover`). On a CSI error in
  single-shot mode, SLM-OS returns failure to the Lua caller; the
  next `cam:capture()` call performs a full re-init.
- **Power-domain management.** No `power-domains` property on the
  T234 NVCSI DT node; the block draws from the always-on host1x
  power domain.

What SLM-OS must recreate that Linux does pre-`probe()`:

- BPMP-mediated clock enable for `TEGRA234_CLK_NVCSI` (see
  `docs/archive/plans/jetson-bpmp-ipc-plan.md`). Already in scope.
- MMIO mapping of `0x15A00000` (256 KB, non-cached, EL2).
- If Option B (RTCPU IVC), allocation and registration of IVC
  channels with the camera RTCPU — substantial new code, not
  currently anywhere in SLM-OS.

## Open questions

- **CBB firewall on `0x15A00000`.** Phase 0 recon must answer: does
  a single read peek from NS EL2-VHE return data, or does it
  generate a RAS Uncorrectable Error and EL3 power-off? No way to
  tell from a code-read.
- **CIL clock source.** `t19_nvcsi_info` lists only `nvcsi` at 400
  MHz, but `csi4_phy_config()` references `TEGRA_CSICIL_CLK_MHZ` for
  settle-time math. Whether the CIL clock is gated separately or is
  derived inside the brick from the NVCSI clock is unclear from the
  R35 DT — needs a TRM cross-reference or a runtime probe.
- **Frame-done interrupt sourcing.** This document and the L4T
  driver both indicate frame-done arrives via VI, not NVCSI. The VI
  notes (separate doc) need to confirm which GIC SPI fires on a
  single-channel capture and whether NVCSI errors latch a parallel
  IRQ that SLM-OS should wire up.
- **Per-lane polarity / swizzle on the Orin Nano carrier.** The
  Orin Nano dev kit DT files specify `lane_polarity` and lane
  swizzle properties for the J17 / J20 connectors. Pulling those
  values from `tegra234-p3768-camera-imx219-*.dtsi` is a separate
  small fetch task before bring-up; this document assumes the
  default (no swizzle, polarity = 0).
- **Whether RTCPU firmware is already loaded post-kexec.** L4T
  loads the Camera RTCPU firmware via the `tegra_camrtc` driver
  during Linux boot. If SLM-OS pursues Option B, it inherits a
  running RTCPU only when kexec preserves it; otherwise SLM-OS
  would need its own firmware loader (significant scope).
