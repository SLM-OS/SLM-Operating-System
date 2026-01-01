# Jetson Serial Debugging Session - December 2025

This document records a comprehensive debugging session attempting to get SLM-OS serial output working on the Jetson Orin Nano via kexec boot.

**Date:** December 28-29, 2025
**Hardware:** Jetson Orin Nano Super Developer Kit
**Host:** Ubuntu Linux (remote lab machine)
**Goal:** Verify SLM-OS shell I/O via serial console after kexec boot

---

## Table of Contents

1. [Initial State](#initial-state)
2. [Serial Communication Setup](#serial-communication-setup)
3. [Baud Rate Discovery](#baud-rate-discovery)
4. [UART Clock Issue After Kexec](#uart-clock-issue-after-kexec)
5. [BPMP Communication Attempts](#bpmp-communication-attempts)
6. [SD Card Boot Attempt](#sd-card-boot-attempt)
7. [Boot Lockout and Recovery](#boot-lockout-and-recovery)
8. [Current State and Next Steps](#current-state-and-next-steps)
9. [Key Findings Summary](#key-findings-summary)

---

## Initial State

### Environment
- Jetson Orin Nano connected to Ubuntu lab machine via:
  - SSH tunnel (port 4243) through router
  - USB-serial adapter on 40-pin header (pins 8/10 = UARTA)
  - Smart plug for remote power control (`lab-tools/jetson-power.py`)
- Serial device: `/dev/ttyUSB0` on Ubuntu host
- Jetson boots from SD card (238GB), not eMMC

### Previous Work
- `docs/jetson-kexec-debugging.md` documented earlier fixes:
  - Spinlock bypass for single-core operation
  - Watchdog disable before kexec
  - UART_INIT_MODE=2 (skip BPMP, assume UART ready)
- Linux-to-Linux serial was untested
- SLM-OS serial output after kexec was untested

---

## Serial Communication Setup

### Hardware Connection
```
Ubuntu Host                    Jetson Orin Nano
┌─────────────┐               ┌─────────────────┐
│ /dev/ttyUSB0│───USB-Serial──│ 40-pin header   │
│             │   adapter     │ Pin 8: UART TX  │
│             │               │ Pin 10: UART RX │
│             │               │ Pin 6: GND      │
└─────────────┘               └─────────────────┘
```

### Serial Configuration
- **Baud rate:** 115200
- **Data bits:** 8
- **Parity:** None
- **Stop bits:** 1
- **Flow control:** None

### Configuration Commands
```bash
# On Ubuntu host
sudo stty -F /dev/ttyUSB0 115200 raw -echo

# On Jetson (via SSH)
sudo stty -F /dev/ttyTHS1 115200 raw -echo
```

---

## Baud Rate Discovery

### Problem
Initial serial tests showed no communication between Ubuntu and Jetson.

### Investigation
Checked baud rate on both ends:
```bash
# Ubuntu host - was already at 115200
stty -F /dev/ttyUSB0 | grep speed
# Output: speed 115200 baud

# Jetson - was at 9600!
stty -F /dev/ttyTHS1 | grep speed
# Output: speed 9600 baud
```

### Root Cause
The Jetson's `/dev/ttyTHS1` defaulted to 9600 baud, while the Ubuntu host was at 115200.

### Fix
```bash
# On Jetson
sudo stty -F /dev/ttyTHS1 115200 raw -echo
```

### Verification
After fixing baud rate, Linux-to-Linux serial worked perfectly:
```bash
# Jetson → Ubuntu
echo "Hello from Jetson" > /dev/ttyTHS1
# Received on Ubuntu: "Hello from Jetson"

# Ubuntu → Jetson
echo "Hello from Ubuntu" > /dev/ttyUSB0
# Received on Jetson via: cat /dev/ttyTHS1
```

**Status:** Linux-to-Linux serial communication verified working at 115200 baud.

---

## UART Clock Issue After Kexec

### Problem
After kexec boot into SLM-OS, no serial output was received despite verified hardware connection.

### Investigation Steps

#### Step 1: Verify kernel_main() is reached
Added PSCI reboot checkpoint to `kernel/src/main.c`:
```c
void kernel_main(void) {
    // PSCI reboot to prove we reached kernel_main
    asm volatile(
        "mov x0, #0x84000009\n"  // PSCI SYSTEM_RESET
        "hvc #0\n"
    );
    // ... rest of kernel
}
```

**Result:** Jetson rebooted immediately after kexec, proving kernel_main() IS reached.

#### Step 2: Add early UART debug output
Added direct UART writes before any initialization:
```c
void kernel_main(void) {
    // Write directly to UART without waiting for THRE
    volatile uint32_t *uart_thr = (volatile uint32_t *)0x03100000;
    *uart_thr = 'S';
    *uart_thr = 'L';
    *uart_thr = 'M';
    // ...
}
```

**Result:** No output received. UART hardware not responding.

#### Step 3: Check UART clock status
The UART requires a clock signal from the BPMP (Boot and Power Management Processor). Linux typically enables this clock during boot but may disable it before kexec.

Examined the kexec flow:
1. Linux calls `kexec_file_load()` or `kexec -e`
2. Linux disables non-essential peripherals
3. Linux jumps to new kernel
4. SLM-OS starts with UART clock potentially disabled

### Root Cause
**Linux disables the UART clock before kexec.** The BPMP must be asked to re-enable it, but BPMP communication after kexec is problematic.

### Evidence
- `docs/jetson-kexec-debugging.md` assumed UART clock stays enabled (incorrect)
- Early UART writes produced no output
- BPMP IVC channels may be in corrupted state after kexec

---

## BPMP Communication Attempts

### Background
The BPMP (Boot and Power Management Processor) controls clocks and power on Tegra SoCs. Communication uses IVC (Inter-VM Communication) channels via shared memory and HSP (Hardware Synchronization Primitives) doorbells.

### Attempt 1: Change UART_INIT_MODE

Changed `kernel/drivers/uart_tegra.c`:
```c
// Changed from:
#define UART_INIT_MODE 2  // Skip BPMP, assume UART ready

// Changed to:
#define UART_INIT_MODE 1  // Use BPMP to enable clock
```

**Result:** No improvement. BPMP not responding.

### Attempt 2: IVC Channel Reset

Added to `kernel/drivers/bpmp.c` in `bpmp_init()`:
```c
/*
 * After kexec, the IVC channels may be in a corrupted state.
 * Reset the channel counters to force a clean state.
 */
volatile struct ivc_channel_header *tx_h = tx_header();
volatile struct ivc_channel_header *rx_h = rx_header();

/* Sync TX channel: set r_count = w_count */
mmio_write32(&tx_h->r_count, mmio_read32(&tx_h->w_count));

/* Sync RX channel: set r_count = w_count */
mmio_write32(&rx_h->r_count, mmio_read32(&rx_h->w_count));

/* Clear any pending doorbells */
hsp_ccplex_clear();
```

**Result:** No improvement. BPMP still not responding.

### Attempt 3: Force Kexec

Tried using force flag to skip some Linux cleanup:
```bash
kexec -f /boot/slmos.bin
```

**Result:** No improvement.

### Attempt 4: Keep UART Active

Attempted to keep UART active before kexec by writing to it:
```bash
echo "keeping uart active" > /dev/ttyTHS1 &
kexec -e
```

**Result:** No improvement. Linux still disables clock.

### Analysis

The BPMP firmware expects a specific initialization sequence:
1. Establish IVC channel handshake
2. Exchange capability information
3. Then process requests

After kexec, this state is lost. The BPMP may:
- Ignore requests from unknown/uninitialized clients
- Require a full system reset to re-establish communication
- Have security restrictions preventing post-boot initialization

**Conclusion:** BPMP communication after kexec appears to require significant additional work or may not be feasible without BPMP firmware modifications.

---

## SD Card Boot Attempt

### Rationale
Since kexec doesn't preserve UART clock state, try booting SLM-OS directly from bootloader (not via kexec). This would test whether the UART works at all in SLM-OS.

### Discovery: Existing SLM-OS Boot Entry
Found existing SLM-OS entry in `/boot/extlinux/extlinux.conf`:
```
LABEL slmos
    MENU LABEL SLM-OS
    LINUX /boot/slmos.bin
    APPEND --
```

### Modification
Changed boot default:
```
# Changed from:
DEFAULT JetsonIO

# Changed to:
DEFAULT slmos
```

### Result: Lockout
- SLM-OS booted successfully (based on no Linux boot messages)
- Lost SSH access (SLM-OS has no network stack)
- TCU (USB-C debug console) doesn't work with SLM-OS (SPE firmware inactive)
- No way to interact with system remotely

---

## Boot Lockout and Recovery

### Recovery Attempt 1: TCU Console

Connected USB-C cable to Jetson debug port.

**Problem:** `/dev/ttyACM0` was already present before connecting USB-C - it was the STM32 Smarthub, not the Jetson.

**Finding:** TCU only enumerates as a USB device when Linux (and SPE firmware) is running. With SLM-OS, there's no USB device enumeration for the debug port.

### Recovery Attempt 2: SD Card Edit

1. Powered off Jetson via smart plug
2. User physically removed SD card
3. Inserted SD card into Ubuntu host (appeared as `/dev/sde`)
4. Mounted root partition: `sudo mount /dev/sde1 /mnt/jetson-root`
5. Restored original extlinux.conf:
```
TIMEOUT 30
DEFAULT primary

LABEL primary
      MENU LABEL primary kernel
      LINUX /boot/Image
      INITRD /boot/initrd
      APPEND ${cbootargs} root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4 ...
```
6. Unmounted and reinstalled SD card

### New Problem: Boot Failure

After restoring config:
- NVIDIA splash screen appears
- Then just flashing cursor
- UEFI shows "Boot recovery mode" when selecting SD device

### UEFI Shell Investigation

User accessed UEFI shell manually:

1. **Filesystem mapping:**
   - FS0, FS1: Small partitions, no boot files
   - FS3: EFI System Partition with `/EFI/BOOT/BOOTAA64.efi`

2. **Attempted to run bootloader:**
   ```
   FS3:
   cd EFI\BOOT
   BOOTAA64.efi
   ```
   **Result:** "Command status: not found"

3. **Attempted to load bootloader:**
   ```
   load BOOTAA64.efi
   ```
   **Result:** "not an image"

### Filesystem Verification

From Ubuntu with SD card mounted:
```bash
# Check EFI partition
sudo fsck.vfat -n /dev/sde10
# Result: /dev/sde10: 4 files, 220/129022 clusters (clean)

# Verify EFI file
file /mnt/jetson-efi/EFI/BOOT/BOOTAA64.efi
# Result: PE32+ executable (EFI application) Aarch64

ls -la /mnt/jetson-efi/EFI/BOOT/
# BOOTAA64.efi exists, reasonable size
```

The EFI file appears valid from Linux's perspective but UEFI won't execute it.

---

## Current State and Next Steps

### Current State
- **Jetson:** Not booting. Shows NVIDIA splash then fails.
- **UEFI:** Reports "Boot recovery mode", won't execute BOOTAA64.efi
- **Hardware:** Possibly damaged (user reported potential short earlier)
- **SD Card:** Filesystem checks clean, EFI file appears valid

### Recommended Recovery

1. **Download fresh JetPack 6.2.1 SD card image**
   - URL: https://developer.nvidia.com/embedded/jetpack-sdk-62
   - Use Balena Etcher to write to new SD card

2. **Boot with fresh image**
   - This establishes known-good baseline
   - Verifies hardware is functional

3. **If fresh image works:**
   - Copy SLM-OS files to new card
   - Continue debugging UART via USB-serial adapter
   - Consider alternative approaches to UART clock

4. **If fresh image fails:**
   - Hardware damage likely from reported short
   - May need to inspect board for damage
   - Consider NVIDIA SDK Manager for full reflash

### Alternative UART Approaches (for future)

1. **Use USB-serial adapter exclusively**
   - Bypass TCU entirely
   - Requires solving BPMP clock enable issue

2. **Pre-enable UART before kexec**
   - Modify Linux kexec code to preserve UART clock
   - Requires custom kernel build

3. **Implement SLM-OS BPMP driver**
   - Full IVC channel initialization
   - May require reverse engineering BPMP protocol

4. **Direct clock register manipulation**
   - Bypass BPMP entirely
   - Risk: May conflict with BPMP, cause instability
   - Requires detailed Tegra clock controller knowledge

---

## Key Findings Summary

### Verified Working
- Linux-to-Linux serial at 115200 baud via 40-pin header
- USB-serial adapter hardware connection
- SLM-OS kernel_main() is reached after kexec
- kexec successfully loads and jumps to SLM-OS

### Verified NOT Working
- UART output from SLM-OS after kexec (clock disabled)
- BPMP communication after kexec (IVC channels corrupted)
- TCU debug console with SLM-OS (requires SPE firmware)

### Key Technical Discoveries

1. **Baud rate mismatch:** Jetson defaults to 9600, must be set to 115200
2. **UART clock:** Linux disables UART clock before kexec
3. **BPMP state:** IVC channels are corrupted/invalid after kexec
4. **TCU dependency:** USB-C debug requires SPE firmware (Linux only)
5. **Boot order:** Jetson boots from SD card, not eMMC

### Files Modified During Session

| File | Changes |
|------|---------|
| `kernel/drivers/uart_tegra.c` | Changed UART_INIT_MODE from 2 to 1 |
| `kernel/drivers/bpmp.c` | Added IVC channel reset code |
| `kernel/src/main.c` | Added early debug output, PSCI checkpoint |

### Lessons Learned

1. **Always verify both ends of serial connection** - baud rate mismatch wasted significant time
2. **TCU is not available for bare-metal debugging** - must use 40-pin header UART
3. **Changing boot defaults without console access is dangerous** - always have recovery plan
4. **kexec does not preserve peripheral state** - clocks, DMAs, etc. may be disabled

---

## Appendix: Serial Port Reference

### Jetson Orin Nano Serial Options

| Port | Address | Type | Access | Post-kexec Status |
|------|---------|------|--------|-------------------|
| UARTA | 0x03100000 | NS16550 | 40-pin pins 8/10 | Clock disabled |
| TCU | HSP mailbox | Combined | USB-C debug | Not available |

### 40-Pin Header UART Pinout

| Pin | Function | Direction |
|-----|----------|-----------|
| 6 | GND | - |
| 8 | UART TX (Jetson → Host) | Output |
| 10 | UART RX (Host → Jetson) | Input |

### USB-Serial Adapter Wiring

```
USB-Serial Adapter    Jetson 40-Pin Header
┌──────────────┐     ┌──────────────────┐
│ GND (Black)  │─────│ Pin 6 (GND)      │
│ RX (White)   │─────│ Pin 8 (TX)       │
│ TX (Green)   │─────│ Pin 10 (RX)      │
└──────────────┘     └──────────────────┘
```

---

## Recovery Session - December 29, 2025

After the initial boot failure, a comprehensive recovery session was undertaken.

### Fresh SD Card Flash

1. **Downloaded JetPack image**: `sd-blob.img` (23GB)
2. **Flashed to new 60GB SD card**: `/dev/sde`
   ```bash
   sudo dd if=~/Downloads/sd-blob.img of=/dev/sde bs=4M status=progress conv=fsync
   ```
3. **Result**: 15 partitions created, image wrote successfully

### Initial Boot Failure

Even with fresh image, system booted to **recovery shell** instead of full Linux:
- UEFI showed: "L4TLauncher: Attempting Recovery Boot"
- Kernel booted but dropped to `bash-5.1#` initramfs shell
- Kernel cmdline showed `root=/dev/initrd` instead of `root=/dev/mmcblk0p1`

### Debug UART Discovery

Found second USB-TTL adapter on `/dev/ttyUSB1` connected to Jetson's debug UART:
- `/dev/ttyUSB0` = 40-pin header UART (for SLM-OS debugging)
- `/dev/ttyUSB1` = Debug UART (MB1/UEFI/early boot messages)

This allowed capturing full boot logs including MB1 bootloader and UEFI messages.

### Recovery Shell Network Access

From initramfs recovery shell, established network:
```bash
mount /dev/mmcblk0p1 /mnt
mount --bind /dev /mnt/dev
mount --bind /proc /mnt/proc
mount --bind /sys /mnt/sys
ip link set eth0 up
dhclient eth0
# Got IP: 192.168.4.88
```

Started SSH server from chroot:
```bash
mkdir -p /mnt/run/sshd
ssh-keygen -A  # Generate host keys
echo "ssh-ed25519 AAAA... john@tarrasque" > /mnt/root/.ssh/authorized_keys
chroot /mnt /usr/sbin/sshd
```

### Firmware Analysis

Discovered version mismatch:
- **QSPI firmware**: 36.4.7
- **SD card rootfs**: R36.4.3

### Capsule Firmware Update

Used NVIDIA capsule update mechanism:
```bash
mount -t efivarfs efivarfs /sys/firmware/efi/efivars
/usr/sbin/nv_bootloader_capsule_updater.sh -q /opt/ota_package/t23x/TEGRA_BL_3767.Cap
```

After reboot, firmware updated to **36.4.3** (matching rootfs), but recovery boot persisted.

### Root Cause: L4TLauncher Recovery Path

The L4TLauncher (BOOTAA64.efi) was choosing recovery boot path regardless of:
- Firmware version match
- Valid extlinux.conf
- Normal boot slot configuration

Attempts to fix:
1. Disabled nv-oem-config service - no effect
2. Set nvbootctrl active slot - no effect
3. Checked EFI boot variables - all appeared correct

### Solution: Direct Kernel Boot Entry

Bypassed L4TLauncher entirely by creating direct EFI boot entry:
```bash
efibootmgr -c -d /dev/mmcblk0 -p 1 -L 'Linux Direct' \
  -l '\\boot\\Image' \
  -u 'root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4 console=ttyTCU0,115200'
```

**Result**: System booted into full Linux with systemd! Boot log showed:
- systemd services starting
- "OEM installation" setup running
- Network not yet configured (awaiting first-boot setup)

### Final State

- **Boot**: Working via direct kernel boot entry (bypassing L4TLauncher)
- **Network**: Not configured (first-boot OEM setup pending)
- **SSH via tunnel**: Not working (WiFi not configured)
- **Direct SSH**: Not working (DHCP not acquired)

The Jetson is bootable but requires physical access to complete first-boot setup (user creation, network configuration) or further remote configuration via serial console.

### Lessons Learned

1. **Debug UART invaluable**: `/dev/ttyUSB1` provided MB1/UEFI logs essential for diagnosis
2. **L4TLauncher behavior**: May force recovery boot on fresh images for first-boot setup
3. **Direct EFI boot**: Can bypass L4TLauncher issues by booting kernel directly
4. **Capsule updates**: Work but require proper environment (efivarfs mounted)
5. **Recovery shell**: Full Linux tools available via chroot to rootfs

### Recommendation

User is obtaining a new Jetson device. This unit may have intermittent issues from the earlier reported short circuit. Development will continue on Raspberry Pi 5 while awaiting new hardware.

---

*Document updated: December 29, 2025*
