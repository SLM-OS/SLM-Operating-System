# Lab Operations Guide

This document describes how to interact with the Jetson Orin Nano lab hardware remotely. It is primarily intended for Claude Code to reference when performing lab operations.

---

## Quick Reference

| Operation | Command |
|-----------|---------|
| SSH to Jetson | `ssh -p 4243 root@gradient-nano.onthewifi.com` |
| Power cycle | `lab-tools/jetson-power.py cycle` |
| Serial (40-pin) | See "Serial Console Access" below |
| Reboot (preferred) | `reboot` via SSH or SLM-OS shell |

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

```bash
# From Claude Code (using Windows Python):
/c/Windows/py.exe "H:/My Drive/Capstone/CS-496-SLM-Operating-System/lab-tools/jetson-power.py" cycle
```

The Kasa smart plug is at `192.168.4.112`. The `cycle` command turns power off, waits 3 seconds, then turns it back on.

---

## Serial Console Access

### Important: Cygwin Environment Setup

**Claude Code runs in Git Bash**, which has different mount points than Cygwin. When running Cygwin bash from Claude Code, the inherited Git Bash environment causes `/usr/bin` to point to Git Bash's binaries instead of Cygwin's. This makes Cygwin-installed programs like `picocom` invisible.

**Solution:** Always use `env -i` to clear the inherited environment before running Cygwin:

```bash
# CORRECT: Clean environment - picocom will be found
C:/cygwin64/bin/env.exe -i HOME=/tmp PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe --login -c "picocom -b 115200 /dev/ttyS4"

# WRONG: Inherits Git Bash mounts - picocom not found
C:/cygwin64/bin/bash.exe --login -c "picocom -b 115200 /dev/ttyS4"
```

**Verification:** Check which `/usr/bin` is active:
```bash
C:/cygwin64/bin/env.exe -i PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe -c "mount | grep usr"
# Should show: C:/cygwin64/bin on /usr/bin
# NOT: C:/Program Files/Git/usr/bin on /usr/bin
```

### Serial Port Mapping

| Cygwin Device | Windows Port | Connection |
|---------------|--------------|------------|
| `/dev/ttyS4` | COM5 | USB-C debug (TCU) — **does not work for bare-metal** |
| TBD | TBD | USB-serial adapter on 40-pin header (pending) |

### Connecting to Serial Console

**40-pin Header UART (for SLM-OS bare-metal):**
```bash
C:/cygwin64/bin/env.exe -i HOME=/tmp PATH=/usr/bin:/bin C:/cygwin64/bin/bash.exe --login -c "picocom -b 115200 /dev/ttyS<N> --noreset"
```

Replace `<N>` with the correct serial port number once the USB-serial adapter is connected.

**Picocom options:**
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

```bash
# Copy kernel to Jetson
scp -P 4243 /c/temp/slmos-build/kernel/slmos.elf root@gradient-nano.onthewifi.com:/boot/

# Or copy binary format
scp -P 4243 /c/temp/slmos-build/kernel/slmos.bin root@gradient-nano.onthewifi.com:/boot/
```

### Booting SLM-OS via kexec

```bash
ssh -p 4243 root@gradient-nano.onthewifi.com
kexec -l /boot/slmos.elf --reuse-cmdline
kexec -e
```

Note: After `kexec -e`, SSH connection will drop. Monitor via serial console.

---

## Lab Tools Reference

All scripts are in `lab-tools/`:

### jetson-power.py

Controls Kasa smart plug (192.168.4.112):

```bash
/c/Windows/py.exe "H:/My Drive/Capstone/CS-496-SLM-Operating-System/lab-tools/jetson-power.py" <command>
```

| Command | Description |
|---------|-------------|
| `status` | Show current power state |
| `on` | Turn power on |
| `off` | Turn power off |
| `cycle` | Power off, wait 3s, power on |

### jetson-debug.sh / jetson-uart.sh

Shell scripts for serial access. These may need updating once USB-serial adapter is configured.

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
*For: Claude Code reference during lab operations*
