# Lab Operations Guide

This document describes how to interact with the Jetson Orin Nano lab hardware remotely. It is primarily intended for Claude Code to reference when performing lab operations.

---

## Quick Reference

| Operation | Ubuntu | Windows (Cygwin) |
|-----------|--------|------------------|
| SSH to Jetson | `ssh -p 4243 root@gradient-nano.onthewifi.com` | Same |
| Reboot (preferred) | `reboot` via SSH or SLM-OS shell | Same |
| Power control | `./lab-tools/jetson-power.py <cmd>` | `py lab-tools/jetson-power.py <cmd>` |
| Serial console | `./lab-tools/jetson-uart.sh` | See Cygwin section below |

**Lab configuration:** `lab-tools/lab-settings.cfg` contains environment-specific settings.

---

## Restarting the Jetson

### Preferred: Software Reboot

Always prefer software reboot over power cycling when possible:

1. **From Linux (via SSH):**
   ```bash
   ssh -p 4243 root@gradient-nano.onthewifi.com
   reboot
   ```

2. **From SLM-OS shell:**
   ```
   slmos> reboot
   ```
   (Uses PSCI system reset)

### Last Resort: Power Cycle

Use power cycling only when:
- SSH is not responding
- SLM-OS shell is not responding
- System is hung or crashed
- No serial output and no network connectivity

**Ubuntu:**
```bash
./lab-tools/jetson-power.py cycle
```

**Windows:**
```bash
py lab-tools/jetson-power.py cycle
```

The Kasa smart plug IP is configured in `lab-tools/lab-settings.cfg`. The `cycle` command turns power off, waits 3 seconds, then turns it back on.

---

## Powering Off the Jetson

### Use Smart Plug Only

**⚠️ IMPORTANT:** Never use software power-off commands (`poweroff`, `shutdown -h`, `halt`) via SSH unless explicitly instructed. The Jetson requires **manual physical intervention** to restart after a software power-off — there is no remote way to turn it back on.

**Correct method — Smart plug:**

**Ubuntu:**
```bash
./lab-tools/jetson-power.py off   # Power off
./lab-tools/jetson-power.py on    # Power on
```

**Windows:**
```bash
py lab-tools/jetson-power.py off  # Power off
py lab-tools/jetson-power.py on   # Power on
```

The Jetson is configured to auto-power-on when AC power is applied, so turning the smart plug on will boot the system.

### Why Software Power-Off Doesn't Work Remotely

When Linux executes `poweroff`, the Jetson enters a halted state but the smart plug remains on. In this state:
- The Jetson will not respond to SSH
- The Jetson will not auto-restart when power is cycled
- Physical button press is required to restart

The smart plug `status` command only shows whether the **plug** is providing power, not whether the Jetson itself is running.

---

## Serial Console Access

### Ubuntu

**Using the lab-tools script (recommended):**
```bash
./lab-tools/jetson-uart.sh
```

**Or directly with picocom:**
```bash
sudo picocom -b 115200 /dev/ttyUSB0
```

The serial port is configured in `lab-tools/lab-settings.cfg` (default: `/dev/ttyUSB0`).

**Testing serial from Jetson side:**
```bash
ssh -p 4243 root@gradient-nano.onthewifi.com
echo "test message" > /dev/ttyTHS1
```

### Windows (Cygwin)

**Important: Cygwin Environment Setup**

Claude Code runs in Git Bash, which has different mount points than Cygwin. When running Cygwin bash from Claude Code, the inherited Git Bash environment causes `/usr/bin` to point to Git Bash's binaries instead of Cygwin's.

**Solution:** Always use `env -i` to clear the inherited environment:

```bash
# CORRECT: Clean environment - picocom will be found
C:/cygwin64/bin/env.exe -i HOME=/tmp PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe --login -c "picocom -b 115200 /dev/ttyS8"

# WRONG: Inherits Git Bash mounts - picocom not found
C:/cygwin64/bin/bash.exe --login -c "picocom -b 115200 /dev/ttyS8"
```

### Serial Port Mapping

| Ubuntu | Windows/Cygwin | Jetson Device | Connection |
|--------|----------------|---------------|------------|
| `/dev/ttyUSB0` | `/dev/ttyS8` (COM9) | `/dev/ttyTHS1` | 40-pin header UART ✓ |
| `/dev/ttyACM*` | `/dev/ttyS4` (COM5) | TCU | USB-C debug — **bare-metal: no** |

### Picocom Options

- `-b 115200` — Baud rate
- `--noreset` — Don't reset DTR/RTS on connect (avoids resetting some devices)
- `--exit-after <ms>` — Exit after timeout (useful for scripted captures)

**To exit picocom:** Press `Ctrl-A` then `Ctrl-X`

### USB-C Debug Port (TCU) Limitations

The USB-C debug port (`/dev/ttyS4`) uses Tegra Combined UART (TCU), which:
- Works when Linux is running (SPE firmware handles routing)
- Does NOT work after kexec or for bare-metal code
- Produces no output when SLM-OS is running

For SLM-OS debugging, use the 40-pin header UART (UARTA at 0x03100000).

See `docs/jetson-tcu.md` for technical details on why TCU doesn't work.

---

## Deploying Code to Jetson

### Via SSH (when Linux is running)

**Ubuntu:**
```bash
# Copy kernel to Jetson
scp -P 4243 build/kernel/slmos.elf root@gradient-nano.onthewifi.com:/root/
```

**Windows:**
```bash
scp -P 4243 C:/temp/slmos-build/kernel/slmos.elf root@gradient-nano.onthewifi.com:/root/
```

### Booting SLM-OS via kexec

```bash
ssh -p 4243 root@gradient-nano.onthewifi.com
kexec -l /root/slmos.elf --reuse-cmdline
kexec -e
```

Note: After `kexec -e`, SSH connection will drop. Monitor via serial console.

---

## Lab Tools Reference

All scripts are in `lab-tools/`. Configuration is in `lab-tools/lab-settings.cfg`.

### lab-settings.cfg

Environment-specific settings:
```bash
KASA_PLUG_IP="192.168.4.112"      # Smart plug IP address
JETSON_UART_PORT="/dev/ttyUSB0"   # 40-pin header serial port
JETSON_DEBUG_PORT="/dev/ttyACM1"  # USB-C debug port (Linux only)
SERIAL_BAUD=115200                # Baud rate
POWER_CYCLE_DELAY=3               # Seconds between off/on
```

### jetson-power.py

Controls Kasa smart plug for Jetson power:

**Ubuntu:** `./lab-tools/jetson-power.py <command>`
**Windows:** `py lab-tools/jetson-power.py <command>`

| Command | Description |
|---------|-------------|
| `status` | Show current power state and plug info |
| `on` | Turn power on |
| `off` | Turn power off |
| `cycle` | Power off, wait, power on |

### jetson-uart.sh

Connect to 40-pin header UART (UARTA) for SLM-OS debugging:
```bash
./lab-tools/jetson-uart.sh              # Use port from config
./lab-tools/jetson-uart.sh /dev/ttyUSB1 # Use specific port
```

### jetson-debug.sh

Connect to USB-C debug port (TCU). Only works when Linux is running:
```bash
./lab-tools/jetson-debug.sh
```

---

## Troubleshooting

### "picocom not found" in Cygwin

Cause: Git Bash environment inherited, `/usr/bin` points to wrong location.

Fix: Use `env -i` wrapper as shown above.

### No serial output after kexec

Cause: TCU requires SPE firmware which stops after kexec.

Fix: Use 40-pin header UART instead of USB-C debug port.

### SSH connection refused

Possible causes:
1. Jetson is running SLM-OS (no network stack)
2. Jetson is hung/crashed
3. Network issue

Fix: Power cycle via `jetson-power.py cycle`, wait for Linux to boot.

### Power cycle doesn't bring system back

1. Wait longer (Jetson boot takes ~30 seconds)
2. Check physical connections
3. Try multiple power cycles
4. May need physical access to check hardware

---

## Hardware Reference

### 40-Pin Header Serial (J14)

| Pin | Function | Connect to USB-Serial |
|-----|----------|----------------------|
| 6 | GND | GND |
| 8 | UART1_TX | RX |
| 10 | UART1_RX | TX |

**Important:** Use 3.3V USB-serial adapter. 5V will damage the Jetson.

### Button Header (J14)

| Pins | Function |
|------|----------|
| 5-6 | Jumper for power button mode |
| 7-8 | Reset (momentary short) |
| 9-10 | Force Recovery |
| 11-12 | Power button |

---

*Created: 25 December 2025*
*Updated: 28 December 2025 - Added Ubuntu support, verified serial working*
*For: Claude Code reference during lab operations*


## (Re-)Programming MicroSD Card

In order to access and (re-)flash the microSD card in a device, follow the procedure here: docs/sd-wire-usage.md

D
## Flashing
