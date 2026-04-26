# IMX219 Sensor Driver Notes (Linux Reference Read)

Notes from a code-read of the Linux mainline IMX219 driver, distilled
into what SLM-OS needs to write a minimal bring-up. Companion to
`docs/jetson-camera-imx219-plan.md` (see "IMX219 sensor driver",
lines 174-192). Pure I2C-side state machine; no CSI lane manipulation
lives in this driver.

---

## Source and license

- Upstream URL:
  `https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/media/i2c/imx219.c`
- Ref used: `v6.12` (tag, fetched 2026-04-25). 1258 lines. The
  `master`-branch fallback was not needed.
- Local cache:
  `docs/reference/linux-imx219.c`
- License header (verbatim from the file):
  `// SPDX-License-Identifier: GPL-2.0`
- Copyright: 2019 Raspberry Pi (Trading) Ltd, with derivation notices
  for imx258 (Intel), imx214 (Qtechnology), and imx319 (Intel).

### Re-use posture

Sony's IMX219 register-write sequences are hardware facts derived from
the datasheet and are not copyrightable by Linux's authors. The
`imx219_common_regs[]` table can be re-stated in SLM-OS as a register
sequence without needing to inherit GPL-2.0. However, **verbatim
copies of the C source (including comments, control-flow, and the
exact symbol-naming layout)** carry GPL-2.0 obligations. SLM-OS code
should:

1. Re-state the register tables in its own struct layout (this doc
   already does so for the 1640x1232 mode, see "Mode tables" below).
2. Re-implement bring-up control flow from the spec, not copy
   `imx219_start_streaming` / `imx219_power_on` line-for-line.
3. Cite Linux as the cross-check reference in source-file comments.

The repository has no top-level `LICENSE` / `COPYING` file at the time
of this read; `README.md` says "See repository root" but no such file
is present. Confirming the canonical SLM-OS license text is an open
question (see "Open questions").

---

## Tegra HSI2C controller (cached for context)

- Upstream URL:
  `https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/i2c/busses/i2c-tegra.c`
- Local cache: `docs/reference/linux-i2c-tegra.c` (1979 lines, GPL-2.0)
- Why cached: the SLM-OS Tegra HSI2C controller driver (Pre-Hardware
  Task #1 in `jetson-camera-imx219-plan.md`) needs the `I2C_CNFG`,
  `I2C_STATUS`, `I2C_TX_FIFO`, `I2C_RX_FIFO`, packet-mode header
  format, and FIFO-word layout. This file is the only practical
  programmer's reference for the Tegra234 packet-mode HSI2C block;
  the Orin TRM is partial. Future sessions writing the SLM-OS HSI2C
  driver should read from this cache instead of re-fetching.

---

## I2C contract

| Property | Value | Notes |
|----------|-------|-------|
| Default 7-bit slave address | `0x10` | Standard IMX219 boot address |
| Alternate 7-bit slave address | `0x36` | Used when ADDR_SEL pin is strapped high; the Raspberry Pi Camera v2 board uses 0x10. The Jetson camera connector follows the same wiring. |
| Register addressing width | 16 bits, big-endian on the wire | Most writes are 8-bit data; some are 16-bit data MSB-first. Linux uses `devm_cci_regmap_init_i2c(client, 16)` (16-bit reg width). |
| Bus speed | 100 kHz works (standard mode); 400 kHz nominally supported | Linux gets the rate from DT; the sensor's CCI block tolerates either. SLM-OS bring-up should use 100 kHz to keep the Tegra HSI2C state machine simple. |
| Max single-burst length | ~6-7 bytes per logical write (1 reg = 2 addr + 1-2 data) | The "common register" table is 33 entries. Linux uses `cci_multi_reg_write()` which issues one I2C transaction per entry, **not** a true burst. SLM-OS can do the same: 33 separate write transactions is fine. |
| Longest contiguous write needed | None — every entry in every table is a discrete (reg, val) pair | No mode table requires a true block write. SLM-OS can implement only `i2c_write_reg16(slave, reg, val8)` and `i2c_write_reg16_16(slave, reg, val16)` and never need `i2c_write_burst`. |

A `i2c_write_burst(...)` API is still useful for general I2C device
support, but the IMX219 itself does not exercise it.

---

## Key registers

Extracted from `imx219.c` `#define`s (lines 33-130). All addresses are
16-bit. "Width" is the register's data payload width.

| Offset  | Width | Name                  | Purpose |
|---------|-------|-----------------------|---------|
| `0x0000` | 16    | CHIP_ID               | Returns `0x0219`. Used to verify the part responds. |
| `0x0100` | 8     | MODE_SELECT           | `0x00` = software standby, `0x01` = streaming. The "go" register. |
| `0x0114` | 8     | CSI_LANE_MODE         | `0x01` = 2-lane, `0x03` = 4-lane. Set once before streaming. |
| `0x0128` | 8     | DPHY_CTRL             | `0x00` = auto D-PHY timing (use this), `0x01` = manual. |
| `0x012a` | 16    | EXCK_FREQ             | External clock freq, format `MHz << 8` (e.g. 24 MHz = `0x1800`). |
| `0x0157` | 8     | ANALOG_GAIN           | Analog gain, 0..232. `gain = 256 / (256 - val)`. |
| `0x0158` | 16    | DIGITAL_GAIN          | Digital gain, `0x0100`..`0x0fff`. `0x0100` = unity. |
| `0x015a` | 16    | EXPOSURE              | Coarse exposure in lines. Min 4, max 65535. **No separate fine-exposure register on this part.** |
| `0x0160` | 16    | FRAME_LENGTH_LINES (VTS) | Sets vertical period (height + vblank). |
| `0x0162` | 16    | LINE_LENGTH_PCK (HTS) | Pixel-clock cycles per line. Always 3448 in Linux. |
| `0x0164` | 16    | X_ADD_STA_A           | Crop window start X (relative to pixel array). |
| `0x0166` | 16    | X_ADD_END_A           | Crop window end X (inclusive). |
| `0x0168` | 16    | Y_ADD_STA_A           | Crop window start Y. |
| `0x016a` | 16    | Y_ADD_END_A           | Crop window end Y (inclusive). |
| `0x016c` | 16    | X_OUTPUT_SIZE         | Width of frame after binning, in pixels. |
| `0x016e` | 16    | Y_OUTPUT_SIZE         | Height of frame after binning, in lines. |
| `0x0170` | 8     | X_ODD_INC_A           | Pixel skip (1 = no skip). |
| `0x0171` | 8     | Y_ODD_INC_A           | Line skip (1 = no skip). |
| `0x0172` | 8     | ORIENTATION           | `bit0=hflip`, `bit1=vflip`. Affects Bayer order. |
| `0x0174` | 8     | BINNING_MODE_H        | `0x00` none, `0x01` x2, `0x03` x2 analog (8bpp only). |
| `0x0175` | 8     | BINNING_MODE_V        | Same encoding as `_H`. |
| `0x018c` | 16    | CSI_DATA_FORMAT_A     | `(bpp << 8) \| bpp`. RAW10 = `0x0a0a`, RAW8 = `0x0808`. |
| `0x0301` | 8     | VTPXCK_DIV            | PLL: video-timing pixel-clock divider. |
| `0x0303` | 8     | VTSYCK_DIV            | PLL: video-timing system-clock divider. |
| `0x0304` | 8     | PREPLLCK_VT_DIV       | PLL: pre-PLL VT divider. |
| `0x0305` | 8     | PREPLLCK_OP_DIV       | PLL: pre-PLL OP divider. |
| `0x0306` | 16    | PLL_VT_MPY            | PLL: VT multiplier. |
| `0x0309` | 8     | OPPXCK_DIV            | Output pixel-clock divider, equals bpp. |
| `0x030b` | 8     | OPSYCK_DIV            | Output system-clock divider. |
| `0x030c` | 16    | PLL_OP_MPY            | PLL: OP multiplier. |
| `0x0600` | 16    | TEST_PATTERN          | 0=disabled, 1=solid color, 2=color bars, 3=grey bars, 4=PN9. Useful for first-light sanity. |
| `0x0624` | 16    | TP_WINDOW_WIDTH       | Test-pattern window width. Set equal to X_OUTPUT_SIZE. |
| `0x0626` | 16    | TP_WINDOW_HEIGHT      | Test-pattern window height. Set equal to Y_OUTPUT_SIZE. |

Plus a set of "extended access" magic writes (`0x30eb`, `0x300a`,
`0x300b`) that unlock writes to the `0x3000`-`0x5fff` range. These
appear at the head of `imx219_common_regs[]` and are mandatory.

---

## Bring-up sequence

Power supplies (VANA 2.8V, VDIG 1.8V, VDDL 1.2V) are assumed already
on, since SLM-OS inherits Linux's powered state across kexec. SLM-OS
must NOT toggle reset GPIO without first verifying supplies are on, or
the part will brown-out. Steps below are what SLM-OS must do.

1. **GPIOs released.** XCLR (reset) GPIO is high (de-asserted) —
   either left high by Linux, or driven high by SLM-OS GPIO code.
   *Shape:* GPIO write (no I2C).

2. **Wait `t4 + t5` = 6200 us.** The IMX219 datasheet calls out
   this minimum delay between XCLR rising edge and the first I2C
   write. (Linux uses `usleep_range(6200, 7200)`.) If SLM-OS
   inherits an already-running sensor from Linux, this delay is
   already satisfied — but a defensive wait is cheap.
   *Shape:* `udelay(6200)`.

3. **Read CHIP_ID at `0x0000` and verify == `0x0219`.**
   *Shape:* `i2c_read_reg16(bus, 0x10, 0x0000, &val16)` — returns
   `0x02` and `0x19` as two consecutive bytes, MSB first.

4. **Apply common register table.** This is the 33-entry list
   reproduced below as `imx219_common_regs[]`. Includes:
   - MODE_SELECT = 0 (force standby first)
   - 6 magic writes to unlock the `0x3000-0x5fff` region
   - PLL configuration (7 writes, hard-coded for 24 MHz xclk)
   - 12 "undocumented" register writes (tracked in the Linux
     comment as such — see "What's left out" / "Surprises")
   - LINE_LENGTH_A = 3448
   - X_ODD_INC_A = 1, Y_ODD_INC_A = 1
   - DPHY_CTRL = 0 (auto)
   - EXCK_FREQ = 24 << 8 = `0x1800`

   *Shape:* a loop of `i2c_write_reg16(...)` calls — 33 separate
   transactions. Linux's `cci_multi_reg_write` is just a wrapper
   around exactly this loop.

5. **Configure CSI lane count.** Write `0x0114` = `0x01` (2-lane).
   *Shape:* `i2c_write_reg16(bus, 0x10, 0x0114, 0x01)`.

6. **Write the per-mode register set** (see "Mode tables" below
   for the 1640x1232 binned-RAW10 mode, 13 writes).
   *Shape:* same as step 4 but shorter.

7. **Set initial controls.** At minimum:
   - EXPOSURE (`0x015a`) = `0x0640` (Linux default; 1600 lines)
   - ANALOG_GAIN (`0x0157`) = `0x00`
   - DIGITAL_GAIN (`0x0158`) = `0x0100` (unity)
   - VTS (`0x0160`) = `0x06E3` (1763 = vts_def for the chosen mode)

   *Shape:* `i2c_write_reg16` per control.

8. **Quirk: streaming/standby kick.** Linux writes
   `MODE_SELECT = 1` then 100 us later writes `MODE_SELECT = 0`
   (then later writes `1` again at real stream-start). The comment
   reads: "Sensor doesn't enter LP-11 state upon power up until and
   unless streaming is started, so upon power up switch the modes
   to: streaming -> standby". SLM-OS must do the same kick; the
   downstream NVCSI receiver requires LP-11 to negotiate D-PHY
   bring-up.
   *Shape:* `i2c_write_reg16(bus, 0x10, 0x0100, 0x01); udelay(110);
   i2c_write_reg16(bus, 0x10, 0x0100, 0x00); udelay(110);`

9. **MODE_SELECT = 1 -> streaming.** This is the actual go signal.
   First MIPI-CSI2 frame begins after a few line periods.
   *Shape:* `i2c_write_reg16(bus, 0x10, 0x0100, 0x01)`.

To stop: `MODE_SELECT = 0` (`0x0100` <- `0x00`). The sensor returns
to software standby; supplies and clocks remain on.

---

## Mode tables

### Common register table (apply once at probe, before mode-specific writes)

Translated from `imx219_common_regs[]` at `linux-imx219.c` lines
161-203. Shown as a C array literal SLM-OS can drop into a header.

```c
/*
 * IMX219 common bring-up register sequence (apply once after CHIP_ID
 * read, before any per-mode register writes). Hard-coded for 24 MHz
 * external clock, which is what Sony specs and what the Jetson Orin
 * Nano dev kit drives the camera connector at.
 *
 * Order matters: the 0x30eb / 0x300a / 0x300b sequence at the head
 * unlocks writes to the 0x3000-0x5fff register range.
 */
static const struct imx219_reg imx219_common_regs[] = {
    /* Force standby (in case the sensor was streaming) */
    { 0x0100, 0x00 },

    /* Unlock 0x3000-0x5fff register access */
    { 0x30eb, 0x05 },
    { 0x30eb, 0x0c },
    { 0x300a, 0xff },
    { 0x300b, 0xff },
    { 0x30eb, 0x05 },
    { 0x30eb, 0x09 },

    /* PLL configuration (24 MHz xclk -> 456 MHz link freq, 2-lane) */
    { 0x0301, 0x05 },  /* VTPXCK_DIV   = 5  */
    { 0x0303, 0x01 },  /* VTSYCK_DIV   = 1  */
    { 0x0304, 0x03 },  /* PREPLLCK_VT_DIV = 3 (auto) */
    { 0x0305, 0x03 },  /* PREPLLCK_OP_DIV = 3 (auto) */
    { 0x0306, 0x00 },  /* PLL_VT_MPY high byte  */
    { 0x0307, 0x39 },  /* PLL_VT_MPY low byte  = 57 */
    { 0x030b, 0x01 },  /* OPSYCK_DIV   = 1  */
    { 0x030c, 0x00 },  /* PLL_OP_MPY high byte  */
    { 0x030d, 0x72 },  /* PLL_OP_MPY low byte  = 114 */

    /* Undocumented Sony-magic registers (DO NOT REMOVE; see notes) */
    { 0x455e, 0x00 },
    { 0x471e, 0x4b },
    { 0x4767, 0x0f },
    { 0x4750, 0x14 },
    { 0x4540, 0x00 },
    { 0x47b4, 0x14 },
    { 0x4713, 0x30 },
    { 0x478b, 0x10 },
    { 0x478f, 0x10 },
    { 0x4793, 0x10 },
    { 0x4797, 0x0e },
    { 0x479b, 0x0e },

    /* Frame Bank "A" common settings */
    { 0x0162, 0x0d },  /* LINE_LENGTH_A high = 3448 (0x0d78) */
    { 0x0163, 0x78 },  /* LINE_LENGTH_A low  */
    { 0x0170, 0x01 },  /* X_ODD_INC_A = 1 */
    { 0x0171, 0x01 },  /* Y_ODD_INC_A = 1 */

    /* D-PHY auto timing, advertise 24 MHz external clock */
    { 0x0128, 0x00 },  /* DPHY_CTRL = AUTO */
    { 0x012a, 0x18 },  /* EXCK_FREQ high (24 MHz << 8) */
    { 0x012b, 0x00 },  /* EXCK_FREQ low  */
};

/*
 * NOTE on translation: Linux uses CCI macros (CCI_REG8 / CCI_REG16)
 * that hide whether a value is one or two I2C bytes. The list above
 * splits every 16-bit Sony register into two 8-bit (addr, val) pairs
 * in MSB-first order, which is the format the IMX219 expects on the
 * wire and makes the SLM-OS i2c_write_reg16(reg, val8) shape direct.
 * If SLM-OS prefers a 16-bit-data write helper, collapse the two
 * halves back together (e.g. 0x0162 <- 0x0d78).
 */
```

### 1640 x 1232 binned RAW10 @ 30 fps (target mode)

The Linux driver does not store this as a literal table. It computes
the registers from `supported_modes[2]` plus the V4L2 selection
("crop") rectangle, in `imx219_set_framefmt()` and
`imx219_set_pad_format()`. The values below were derived by walking
that code with `width=1640, height=1232, vts_def=1763,
bpp=10, bin_h=2, bin_v=2`. Centred crop computation:
- `crop.width = format.width * bin_h = 3280`
- `crop.height = format.height * bin_v = 2464`
- `crop.left = (NATIVE_WIDTH - crop.width) / 2 = (3296 - 3280) / 2 = 8`
- `crop.top  = (NATIVE_HEIGHT - crop.height) / 2 = (2480 - 2464) / 2 = 8`
- `X_ADD_STA = crop.left - PIXEL_ARRAY_LEFT = 0`
- `X_ADD_END = X_ADD_STA + crop.width  - 1 = 3279 = 0x0CCF`
- `Y_ADD_STA = crop.top  - PIXEL_ARRAY_TOP  = 0`
- `Y_ADD_END = Y_ADD_STA + crop.height - 1 = 2463 = 0x099F`

```c
/*
 * IMX219 mode: 1640 x 1232 RAW10, 2x2 binned, full FoV, 30 fps,
 * 2-lane CSI-2 at 456 MHz link rate.
 *
 * Apply AFTER imx219_common_regs[] and AFTER writing CSI_LANE_MODE.
 */
static const struct imx219_reg imx219_mode_1640x1232_raw10[] = {
    /* Crop rectangle = full pixel array (3280 x 2464) */
    { 0x0164, 0x00 }, { 0x0165, 0x00 },  /* X_ADD_STA = 0      */
    { 0x0166, 0x0c }, { 0x0167, 0xcf },  /* X_ADD_END = 3279   */
    { 0x0168, 0x00 }, { 0x0169, 0x00 },  /* Y_ADD_STA = 0      */
    { 0x016a, 0x09 }, { 0x016b, 0x9f },  /* Y_ADD_END = 2463   */

    /* Output frame size (post-binning) */
    { 0x016c, 0x06 }, { 0x016d, 0x68 },  /* X_OUTPUT_SIZE = 1640 */
    { 0x016e, 0x04 }, { 0x016f, 0xd0 },  /* Y_OUTPUT_SIZE = 1232 */

    /* 2x2 digital binning (RAW10 -> use BINNING_X2, not X2_ANALOG) */
    { 0x0174, 0x01 },                    /* BINNING_MODE_H = X2 */
    { 0x0175, 0x01 },                    /* BINNING_MODE_V = X2 */

    /* No flip (Bayer: SRGGB10) */
    { 0x0172, 0x00 },                    /* ORIENTATION = 0 */

    /* CSI data format = RAW10 in / RAW10 out */
    { 0x018c, 0x0a }, { 0x018d, 0x0a },  /* CSI_DATA_FORMAT_A = 0x0A0A */
    { 0x0309, 0x0a },                    /* OPPXCK_DIV = 10 */

    /* Frame timing: VTS = 1763 -> 30 fps with HTS=3448 */
    { 0x0160, 0x06 }, { 0x0161, 0xe3 },  /* FRAME_LENGTH_LINES = 1763 */

    /* Test-pattern window matches output (cosmetic, but Linux writes it) */
    { 0x0624, 0x06 }, { 0x0625, 0x68 },  /* TP_WINDOW_WIDTH  = 1640 */
    { 0x0626, 0x04 }, { 0x0627, 0xd0 },  /* TP_WINDOW_HEIGHT = 1232 */
};

/*
 * The 1640x1232 mode produces ~2.5 MB per frame
 * (1640 * 1232 * 10 / 8 = 2,525,200 bytes), which fits comfortably
 * in any of the three usable DRAM regions on Orin Nano post-kexec.
 *
 * For first-light bring-up, also write TEST_PATTERN = 2 (color bars)
 * before MODE_SELECT = 1, then read it back as 0x0002 -> if the I2C
 * round-trip works, color bars confirm sensor->NVCSI->VI is alive
 * without depending on optics or ambient light.
 */
```

---

## What's left out

The SLM-OS port deliberately omits most of the upstream driver:

- **V4L2 control framework** (`v4l2_ctrl_handler`, `s_ctrl`, all of
  the `v4l2_ctrl_new_std` plumbing). SLM-OS exposes exposure and gain
  as direct function calls or a tiny RPC, not as a generic control
  graph.
- **Media controller pads / subdev / async-register**. SLM-OS has
  one IMX219, hard-wired to one NVCSI port, hard-wired to one VI
  channel. Pipeline is static.
- **Runtime PM** (`pm_runtime_*`). SLM-OS keeps the sensor powered
  for the lifetime of the camera task; no idle/resume cycle.
- **Regulator framework** (`regulator_bulk_enable`). Supplies are
  brought up by Linux pre-kexec and left on. SLM-OS must NOT
  toggle XCLR low without first verifying the supplies are still
  energised, but does not own the regulator state machine.
- **Multi-format / flip / multi-mode selection logic**
  (`imx219_get_format_code`, `imx219_set_pad_format`, the four-deep
  `imx219_mbus_formats[]` array). SLM-OS hard-codes
  SRGGB10 + 1640x1232. Adding 640x480 later is a second mode table.
- **Test-pattern colour controls** (4 `V4L2_CID_TEST_PATTERN_*`
  entries). SLM-OS can keep the bare `TEST_PATTERN` register write
  for first-light, drop the per-channel colour controls.
- **DT/fwnode parsing** (`imx219_check_hwcfg`,
  `v4l2_fwnode_endpoint_alloc_parse`). SLM-OS hard-codes lane
  count, link frequency, and I2C address.

What SLM-OS does need to recreate from scratch:

- **The 6200 us power-on delay** between XCLR rising edge and the
  first I2C transaction. Even with supplies already on, if SLM-OS
  ever toggles XCLR (e.g. for sensor reset on probe failure), the
  delay applies. Linux's `usleep_range` becomes a busy-wait
  `udelay(6200)` in SLM-OS unless a sleep primitive is available.
- **The streaming/standby kick** (write `0x0100`=`0x01` then
  `0x00`) at the end of probe — this is what enables LP-11 on the
  D-PHY lanes so NVCSI can train against them.
- **Centred-crop computation**, if SLM-OS ever wants to support
  more than the one hard-coded mode. The math is in
  `imx219_set_pad_format()` lines 821-842.
- **Power-supply ordering invariant**. Linux comments
  (`imx219_supply_name[]` line 231) say "Supplies can be enabled
  in any order" — but Sony's datasheet specifies VDIG before VANA
  before VDDL is safest. Since SLM-OS inherits Linux's already-on
  state this doesn't matter at boot, but if SLM-OS ever
  power-cycles the camera the order must be respected.

---

## Surprises / quirks worth remembering

- **Twelve "undocumented registers"** at `0x4540`-`0x479b` are
  required and Linux's only comment is `/* Undocumented registers */`.
  These are Sony-internal and there is no public datasheet entry
  for them. Removing any of them risks silently breaking image
  quality or stream stability. Treat them as opaque magic.
- **The streaming/standby kick at probe** is not in the datasheet
  excerpts visible in the driver — it is a workaround for the D-PHY
  refusing to enter LP-11 until the sensor has been told to stream
  at least once. Without this, the downstream NVCSI receiver will
  never see valid lane state and will time out on D-PHY calibration.
- **Pixel rate and link frequency are hard-coded in the driver**
  (182.4 Mpix/s, 456 MHz link freq for 2-lane). These are not
  programmed into the sensor — they are advertised to Linux's
  V4L2 layer for downstream rate negotiation. SLM-OS must encode
  them in the NVCSI driver's link-config, not in the IMX219 driver.
- **No separate fine-exposure register on this part** despite the
  plan doc's note about "coarse + fine if separate". `EXPOSURE`
  at `0x015a` is the only exposure register (16 bits, in lines).
  Sub-line exposure is not supported by IMX219.
- **`PPL` (pixel-per-line) is fixed at 3448** for all modes. The
  driver comment at line 864 says "Currently PPL is fixed to
  IMX219_PPL_DEFAULT, so hblank depends on mode->width only". So
  HTS is a constant the SLM-OS driver can hard-code, not derive.
- **Bayer order shifts with flip**: the `imx219_mbus_formats[]`
  array is structured 4-deep specifically to encode the Bayer
  permutations under H/V flip. SLM-OS hard-codes ORIENTATION=0
  and gets SRGGB10, but if anyone ever turns on flip the
  consumer (MNIST input pipeline) needs to know the Bayer order
  changed.
- **Pixel array vs native size mismatch**: the sensor pixel array
  is 3280x2464, but the "native" size advertised to userspace is
  3296x2480 — there are 8-pixel-wide black borders on every side
  used for OB (optical black) clamp. The crop registers are
  measured **relative to the pixel array (8,8)**, not the native
  origin (0,0). The bring-up math in this doc assumes that
  convention.

### Tegra HSI2C — T194/T234 register set vs T210 (April 2026)

Live debugging of the CHIP_ID readback turned up a non-obvious
register-set difference between the Tegra210-era and Tegra194/234
HSI2C controllers. The original SLM-OS port mirrored Tegra210 — and
that's why CHIP_ID reads completed with `PACKET_XFER_COMPLETE` set
but `RX_FIFO` empty (rc=-5). Pinned regression is in
`kernel/tests/test_camera.c` (`I2C_T194_*` constants); cross-
checked against `docs/reference/linux-i2c-tegra.c` `tegra194_i2c_hw`.

What changed on T194/T234:

- **Master FIFO interface moved.** Legacy `FIFO_CONTROL`/`FIFO_STATUS`
  at `0x05C`/`0x060` still exist but are unused — the controller now
  reads `MST_FIFO_CONTROL`/`MST_FIFO_STATUS` at `0x0B4`/`0x0B8`.
  Polling RX count from the legacy view returns 0 even when the
  slave responded.
- **Mandatory CONFIG_LOAD.** Writes to `I2C_CNFG`, `I2C_CLK_DIVISOR`,
  and `I2C_INTERFACE_TIMING_*` land in staging registers and don't
  take effect until you write `MSTR_CONFIG_LOAD` (bit 0) to
  `I2C_CONFIG_LOAD` at `0x08C` and poll until it self-clears.
  Skipping this leaves the bus-timing FSM running on whatever
  Linux (or the chip default) had loaded — packets complete but
  the RX path captures nothing.
- **Different std-mode timing.** Tegra210 uses `clk_divisor_std_mode = 0x19`
  with `tlow=4`, `thigh=2`. Tegra194/234 needs `0x4F` with
  `tlow=8`, `thigh=7`, programmed into `I2C_INTERFACE_TIMING_0`
  (`0x094`). The interface-timing register *must* be written
  explicitly — chip default is 0 which is an invalid bus-timing
  config.

Symptom signature when this is wrong (preserve for future Tegra
bring-up): `imx219` shell command logs `chip_id read rc=-5`, the
diagnostic dump shows `INT_STATUS` with bit 1 (TX_FIFO_DATA_REQ)
stuck on, `MST_FIFO_STATUS = 0x00800080` (RX count 0, TX 8 free),
and `RX_FIFO` reads back 0x02 (residual or bus capacitance, not
real data). All four signals together = "controller is alive but
running on wrong timing config."

---

## Open questions

1. Does the Jetson Orin Nano dev kit's CSI carrier auto-enable VANA
   (2.8V), VDIG (1.8V), and VDDL (1.2V) regulators at board power-on,
   or do they require an explicit Linux regulator-fixed enable that
   SLM-OS must mirror? Linux uses `regulator_bulk_enable` here, but
   if the regulators are board-strapped "always on" then the inherit-
   from-Linux model is safe even after kexec.

2. Which Tegra234 I2C controller instance is wired to the camera
   connector on the Orin Nano dev kit (CAM_I2C, GEN1_I2C, etc.), and
   what is its base MMIO address? The plan doc notes "n depends on
   which bus the camera connector uses; identify from DT" — but the
   carrier-board schematic page or the `tegra234-p3768-0000+p3767-*`
   device tree should make this concrete before SLM-OS writes the
   HSI2C base into a header.

3. Is the IMX219's XCLK (24 MHz) sourced from a Tegra234 PLL output
   that BPMP can enable/disable, or from a fixed crystal on the camera
   module itself? If BPMP-controlled, SLM-OS must hold the clock on;
   if module-local, no action is needed.

4. Does the canonical SLM-OS license live in `LICENSE` / `COPYING` at
   the repo root (currently absent), or is it implicit-by-context?
   This needs to be resolved before SLM-OS can ship a derived register
   table that cites Linux as the cross-check reference.

5. The Linux driver's `imx219_check_hwcfg` insists on link-frequency
   == 456 MHz for 2-lane. Is that a hard sensor PLL constraint (i.e.
   no other 2-lane link rate is achievable with a 24 MHz xclk and the
   PLL_VT_MPY=57 / PLL_OP_MPY=114 values), or is it an arbitrary
   policy choice? SLM-OS needs the answer to pick downstream NVCSI
   D-PHY timing.

6. After Linux kexec hands off to SLM-OS with the camera *not*
   currently streaming, is the sensor in software standby (mode 0) or
   fully off? If Linux runs `imx219_power_off` as part of teardown,
   XCLR is low and SLM-OS must drive it high then re-run the full
   bring-up. If Linux leaves it in software standby, SLM-OS skips
   steps 1-3 above and starts at the common-regs table. Determining
   which case applies requires either a kexec-time GPIO snapshot or
   an unconditional XCLR drive on the SLM-OS side.
