# Tegra234 GPIO Controller — Driver Port Notes

Reference notes for the SLM-OS bare-metal Tegra234 GPIO driver, specifically
focused on releasing the IMX219 `cam_reset_gpio` (PH.06 on the main controller)
and switching the `cam_i2cmux` selector on the AON controller.

## Source and License

Upstream driver (cached locally):
- File: `../slmos-reference-cache/linux/linux-gpio-tegra186.c`
- URL: https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/gpio/gpio-tegra186.c
- Ref: Linux v6.12 (`torvalds/linux` tag `v6.12`)
- SPDX header: `// SPDX-License-Identifier: GPL-2.0-only`
- Copyright: `(c) 2016-2022, NVIDIA CORPORATION`

Companion DT bindings header (port enumeration):
- URL: https://raw.githubusercontent.com/torvalds/linux/v6.12/include/dt-bindings/gpio/tegra234-gpio.h
- SPDX: `GPL-2.0`

The Linux driver is GPL-2.0-only; SLM-OS does not copy code from it. These
notes describe the hardware register layout extracted by reading the driver
and the chip's port table — register offsets, bit fields, and address
arithmetic are hardware facts, not copyrightable expression.

## Hardware Overview

Tegra234 (Jetson Orin Nano / Orin family) exposes two GPIO controllers:

| Controller | Linux name      | DT compatible              | MMIO base    | Aperture |
|------------|-----------------|----------------------------|--------------|----------|
| Main       | `gpiochip0`     | `nvidia,tegra234-gpio`     | `0x02200000` | per-pin  |
| AON        | `gpiochip1`     | `nvidia,tegra234-gpio-aon` | `0x0C2F0000` | per-pin  |

Both controllers share the same per-pin register layout (the same `tegra186_*`
accessor functions in the upstream driver service both). They differ only in
their port table.

### Main controller port topology (25 ports, 164 pins total)

Listed `(name, bank, port, pins)` from `tegra234_main_ports[]`:

```
A  (0,0,8)  B  (0,3,1)  C  (5,1,8)  D  (5,2,4)  E  (5,3,8)
F  (5,4,6)  G  (4,0,8)  H  (4,1,8)  I  (4,2,7)  J  (5,0,6)
K  (3,0,8)  L  (3,1,4)  M  (2,0,8)  N  (2,1,8)  P  (2,2,8)
Q  (2,3,8)  R  (2,4,6)  X  (1,0,8)  Y  (1,1,8)  Z  (1,2,8)
AC (0,1,8)  AD (0,2,4)  AE (3,3,2)  AF (3,4,4)  AG (3,2,8)
```

`bank` and `port` are positional indexes into the MMIO grid (see address
arithmetic below); `pins` is the count of pins implemented by that port
(usually 8, sometimes fewer).

### AON controller port topology (6 ports, 32 pins total)

```
AA (0,4,8)  BB (0,5,4)  CC (0,2,8)  DD (0,3,3)  EE (0,0,8)  GG (0,1,1)
```

All AON ports live in bank 0; only `port` varies.

## Per-Pin Register Window

Each pin gets a 32-byte (`0x20`) window. Within that window the registers
SLM-OS cares about are:

| Offset | R/W | Name                  | Purpose                                           |
|-------:|-----|-----------------------|---------------------------------------------------|
| 0x00   | RW  | `GPIO_ENABLE_CONFIG`  | Master enable, direction, IRQ trigger config      |
| 0x04   | RW  | `GPIO_DEBOUNCE_CTRL`  | Debounce threshold (irrelevant for camera reset)  |
| 0x08   | R   | `GPIO_INPUT`          | Sampled input level (bit 0)                       |
| 0x0C   | RW  | `GPIO_OUTPUT_CONTROL` | Float vs drive (bit 0 = `FLOATED`, 1 = tri-state) |
| 0x10   | RW  | `GPIO_OUTPUT_VALUE`   | Driven output level (bit 0)                       |
| 0x14   | W1C | `GPIO_INTERRUPT_CLR`  | Clear pending IRQ for this pin                    |

### `GPIO_ENABLE_CONFIG` bit fields

| Bit   | Name              | Meaning                                              |
|-------|-------------------|------------------------------------------------------|
| 0     | `ENABLE`          | **Must be 1** for the pin to obey CPU R/W            |
| 1     | `OUT`             | 1 = output direction, 0 = input direction            |
| 2..3  | `TRIGGER_TYPE`    | NONE / LEVEL / SINGLE_EDGE / DOUBLE_EDGE             |
| 4     | `TRIGGER_LEVEL`   | High vs low (level) / rising vs falling (edge)       |
| 5     | `DEBOUNCE`        | Enable debounce (uses `GPIO_DEBOUNCE_CTRL`)          |
| 6     | `INTERRUPT`       | Enable IRQ delivery for this pin                     |
| 7     | `TIMESTAMP_FUNC`  | Hardware timestamp capture (GTE)                     |

### `GPIO_OUTPUT_CONTROL` bit fields

| Bit | Name      | Meaning                                          |
|-----|-----------|--------------------------------------------------|
| 0   | `FLOATED` | 1 = output buffer disabled (tri-state), 0 = drive |

### `GPIO_OUTPUT_VALUE` / `GPIO_INPUT` bit fields

| Bit | Name   | Meaning                       |
|-----|--------|-------------------------------|
| 0   | `HIGH` | Driven (or sampled) level     |

The two enables interact: to **drive** a pin, the driver must clear
`OUTPUT_CONTROL.FLOATED`, then set `ENABLE_CONFIG.ENABLE | ENABLE_CONFIG.OUT`.
Setting `ENABLE` alone leaves the pin as an input; clearing `FLOATED` without
`ENABLE` does nothing.

### Security / VM gate (read-only checks)

There is also a per-pin **secure** aperture (separate MMIO base; the Linux
driver gets it from a second `reg` cell). Each pin has an 8-byte secure window
holding `GPIO_VM` (+0x00, R/W permission to non-secure world) and `GPIO_SCR`
(+0x04, security write/read enable + grant bits). Linux uses these only for a
"is this pin accessible from the non-secure world?" sanity check during
`init_valid_mask`. SLM-OS can skip the check on first bring-up; if writes to
the data aperture are silently dropped, suspect the firewall and revisit the
secure aperture. The secure aperture stride is `port * 0x40 + pin * 0x08`
inside `bank * 0x1000`.

## Address Arithmetic

The Linux driver computes the per-pin base from `tegra186_gpio_get_base`:

```c
offset = port->bank * 0x1000 + port->port * 0x200 + pin_in_port * 0x20;
abs    = controller_base + offset;
```

Then individual registers are at `abs + reg_off` (e.g. `+0x10` for OUTPUT_VALUE).

Concrete formula:

```
pin_register(controller_base, bank, port, pin_in_port, reg_offset) =
    controller_base
  + (bank        * 0x1000)
  + (port        * 0x200)
  + (pin_in_port * 0x20)
  + reg_offset
```

Note: `bank` and `port` here are the values from the port table
(`tegra234_main_ports[]`), **not** the linear port index used in the
dt-bindings macro `TEGRA234_MAIN_GPIO(port, offset)`. The dt-bindings macro
just gives a flat line number per controller; the per-pin MMIO offset comes
from the bank/port pair encoded in the SoC table.

### Worked example — PH.06 (`cam_reset_gpio`)

Port H, line 1167: `TEGRA234_MAIN_GPIO_PORT( H, 4, 1, 8)` → `bank=4`,
`port=1`, `pins=8`. Pin index within port = 6.

```
offset = 4 * 0x1000 + 1 * 0x200 + 6 * 0x20
       = 0x4000  +  0x200  +  0xC0
       = 0x42C0
abs    = 0x02200000 + 0x42C0
       = 0x022042C0      (= ENABLE_CONFIG)
```

| PH.06 register     | Address       |
|--------------------|---------------|
| `ENABLE_CONFIG`    | `0x022042C0`  |
| `DEBOUNCE_CTRL`    | `0x022042C4`  |
| `INPUT`            | `0x022042C8`  |
| `OUTPUT_CONTROL`   | `0x022042CC`  |
| `OUTPUT_VALUE`     | `0x022042D0`  |
| `INTERRUPT_CLR`    | `0x022042D4`  |

### Worked example — AON `cam_i2cmux` selector

Linux exposes the AON controller as `gpiochip1`. The `cam_i2cmux` selector is
documented as line 19. AON ports use the same dt-bindings linear mapping
(`port_index * 8 + offset`), so line 19 = port index 2, offset 3, which is
**port CC, pin 3**.

Port CC, line 1207: `TEGRA234_AON_GPIO_PORT(CC, 0, 2, 8)` → `bank=0`,
`port=2`, `pins=8`. Pin index within port = 3.

```
offset = 0 * 0x1000 + 2 * 0x200 + 3 * 0x20
       = 0x400  +  0x60
       = 0x460
abs    = 0x0C2F0000 + 0x460
       = 0x0C2F0460      (= ENABLE_CONFIG)
```

| CC.3 register      | Address       |
|--------------------|---------------|
| `ENABLE_CONFIG`    | `0x0C2F0460`  |
| `OUTPUT_CONTROL`   | `0x0C2F046C`  |
| `OUTPUT_VALUE`     | `0x0C2F0470`  |

## Init Sequence for Output Drive

To drive `cam_reset_gpio` (PH.06) low, then later release it high, the
sequence is the same one Linux's `gpio_direction_output` uses:

1. **Set the desired output level first**
   - `OUTPUT_VALUE` = 0 (assert reset, low) or 1 (release, high)
2. **Clear FLOATED to engage the output buffer**
   - Read-modify-write `OUTPUT_CONTROL`: clear bit 0
3. **Enable + select output direction in one shot**
   - Read-modify-write `ENABLE_CONFIG`: set bit 0 (`ENABLE`) and bit 1 (`OUT`)

For the IMX219 bring-up flow:

```
# Hold sensor in reset
WRITE OUTPUT_VALUE   = 0
WRITE OUTPUT_CONTROL = (read & ~1)        # clear FLOATED
WRITE ENABLE_CONFIG  = (read | 0x3)       # ENABLE | OUT

# (start XCLK from BPMP-controlled extperiph1, wait t_LOW per IMX219 datasheet)

# Release reset, sensor begins boot
WRITE OUTPUT_VALUE   = 1                  # drive high
# (ENABLE_CONFIG / OUTPUT_CONTROL already set; just toggling the data bit)
```

Linux's `tegra-camera-platform` device tree typically sets PH.06 to "active
low" (`GPIO_ACTIVE_LOW`) and the IMX219 driver toggles it via
`gpiod_set_value_cansleep(reset_gpio, 1)` to assert reset and `0` to release.
SLM-OS works with raw levels, so the polarity is the bare wire: **0 = sensor
held in reset, 1 = sensor running**. The IMX219 datasheet requires the
XCLK to be stable for at least one cycle before deasserting XCLR (reset),
and ~1 ms of stabilization after.

The `cam_i2cmux` selector (CC.3 on AON) chooses between the two CSI camera
connectors on the dev kit. It is a static output: drive it once at probe
time to point at the connector with the sensor, leave it set.

## What's Left Out

These notes intentionally do **not** cover:

- **Interrupts.** No GIC routing, no `INTERRUPT_STATUS_*` registers (offsets
  `0x100 + bank*4`), no `INT_ROUTE_MAPPING`. The IMX219 reset pin is purely
  output; the i2cmux selector is purely output. Neither needs IRQ support.
- **Debouncing.** `DEBOUNCE_CTRL` and `ENABLE_CONFIG.DEBOUNCE` are unused for
  output pins. Debouncing only matters for noisy input lines (buttons, etc.).
- **Pinmux.** Pad function selection (alt 0 / alt 1 / GPIO) and pad electrical
  configuration (drive strength, pull-up / pull-down, schmitt) live in the
  PINMUX block at `0x02430000`, not the GPIO controller. Linux relies on
  `pinctrl-tegra` for this. SLM-OS inherits the pinmux state set up by the
  bootloader (cbootargs / MB1 BCT pinmux table); the camera pads are already
  in GPIO mode by the time SLM-OS starts.
- **Hardware timestamping (GTE).** The AON controller exposes
  `ENABLE_CONFIG.TIMESTAMP_FUNC` (bit 7) for capturing GPIO transitions with
  a hardware timer. Not needed for camera reset / mux selection.
- **Secure / VM access checks.** SLM-OS runs at EL2 and (on Jetson) starts
  from a kexec out of Linux, which has already validated access. The
  `GPIO_VM` / `GPIO_SCR` security aperture is described above for
  reference but not exercised by the camera path.

## Open Questions

These cannot be answered from a code-read alone and need either hardware
probing or DTS / TRM cross-reference:

1. **Post-kexec ENABLE / FLOATED state.** Linux's `gpio-tegra186` sets up
   `ENABLE_CONFIG.ENABLE` when a consumer claims a pin. After `kexec` from a
   running Linux that had IMX219 bound, PH.06 is probably already
   `ENABLE | OUT`, with `FLOATED=0` and `OUTPUT_VALUE=1` (sensor running).
   SLM-OS should still program all three registers explicitly rather than
   assume. (TODO: capture the pre-kexec values via `devmem2` and confirm.)

2. **Secure firewall on PH.06 and CC.3 from EL2.** The CBB firewall blocks
   some peripherals at EL2 (UARTA, GPU at `0x17000000`). The GPIO controllers
   at `0x02200000` and `0x0C2F0000` are believed accessible at EL2 because
   the EL2 shell already drives serial and the BPMP IPC successfully, but
   neither is on the same bus segment as GPIO. First write attempt will
   confirm — a synchronous external abort means it's blocked and we need to
   re-evaluate (e.g., go through BPMP or stay at EL1).

3. **`cam_i2cmux` polarity.** Line 19 on `gpiochip1` is documented as the
   selector, but which level (high vs low) selects which connector
   (`CAM0` vs `CAM1`) is board-specific and not in the GPIO driver. The
   Jetson Orin Nano carrier board schematic (page 17, "CSI MUX") needs to be
   consulted, or the Linux DTB's `nvidia,bus-width` /
   `nvidia,tegra-camera-platform` properties decoded.

4. **Debounce / drive-strength inheritance.** Neither matters for the camera
   path, but if any sensor signal turns out to be flaky after kexec, the
   pinmux block (not the GPIO controller) may need re-programming. Out of
   scope for the GPIO driver itself.

5. **Line-49 vs PH.06 mapping discrepancy.** The dt-bindings macro
   `TEGRA234_MAIN_GPIO(H, 6)` evaluates to `7 * 8 + 6 = 62`, not 49.
   Linux's `gpiochip0` line 49 corresponds to PH.06 only after the
   `init_valid_mask` callback filters out pins that fail the security check
   (`tegra186_gpio_is_accessible`), which compresses the line numbering. The
   raw hardware (port H, pin 6) is what the address arithmetic uses; the
   user-visible line number depends on how the driver enumerates valid pins.
   SLM-OS should always work in (port, pin-in-port) coordinates internally
   and never rely on Linux's compacted line numbers.
