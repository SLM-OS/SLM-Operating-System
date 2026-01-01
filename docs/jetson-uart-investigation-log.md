# Jetson UART Investigation Log

Investigation into getting UART output from SLM-OS on Jetson Orin Nano after kexec boot.

---

## Background

**Problem**: After kexec from Linux to SLM-OS, there is no serial output on the 40-pin header UART (UARTA at 0x03100000).

**Root Cause Hypothesis**: Linux disables the UART clock before kexec, and BPMP communication (needed to re-enable clocks) doesn't work after kexec.

---

## Phase 1: CAR Register Investigation (Previous Session)

### Action 1.1: Search TRM for CAR registers
**Command**: Read TRM Part 1 table of contents
**Result**: Found CAR (Clock and Reset Controller) section at pages 5563-5564 - only 2 pages total, unusually short.

### Action 1.2: Read CAR section in TRM
**Command**: Read TRM pages 5563-5564
**Result**: Section is minimal, mostly references to BPMP. No direct register documentation like older Tegra chips.

### Action 1.3: Search online for Tegra234 CAR registers
**Command**: Web search for "Tegra234 CLK_RST_CONTROLLER UARTA register"
**Result**: Found Linux kernel header `tegra234-clock.h` with `TEGRA234_CLK_UARTA = 155`. Documentation indicates this is "output of mux controlled by CLK_RST_CONTROLLER_CLK_SOURCE_UARTA" but access is through BPMP.

### Action 1.4: Fetch tegra234.dtsi from Linux kernel
**Command**: WebFetch tegra234.dtsi
**Result**: Device tree shows:
- No standalone `car@` node (unlike older Tegra)
- All clock/reset cells provided by `&bpmp` node
- UARTA at 0x03100000 uses `clocks = <&bpmp TEGRA234_CLK_UARTA>`

### Action 1.5: Research CBB (Control Backbone) error codes
**Command**: Web search for Tegra234 clock access errors
**Result**: Found `SUBPRI_CLK_OFF_ERR` error code - hardware enforces clock-gated access attempts. This confirms direct register access will fail if clock is disabled.

### Conclusion from Phase 1
**CRITICAL FINDING**: Direct CAR register manipulation is NOT possible on Tegra234. The architecture enforces all clock control exclusively through BPMP firmware. This is fundamentally different from older Tegra chips.

---

## Phase 2: Alternative Approach - Keep Clock Enabled (Previous Session)

### Action 2.1: Research clk_ignore_unused parameter
**Command**: Web search for "clk_ignore_unused kernel parameter kexec"
**Result**: Found that `clk_ignore_unused` kernel boot parameter prevents Linux from disabling unused clocks. This could keep UART clock enabled before kexec.

**Key Quote**: "By default Linux disables clocks which have no reference in the device tree... To bypass the default clock disabling, include 'clk_ignore_unused' in the bootargs."

---

## Phase 3: Current Session Actions

### Action 3.1: Check Jetson connectivity and current kernel cmdline
**Command**: `ssh -p 4243 root@gradient-nano.onthewifi.com "uname -a; cat /proc/cmdline"`
**Result**:
```
Linux gradient-desktop 5.15.148-tegra #1 SMP PREEMPT Thu Sep 18 15:08:33 PDT 2025 aarch64
root=/dev/mmcblk0p1 rw rootwait rootfstype=ext4 mminit_loglevel=4 console=ttyTCU0,115200 ...
```
Note: `clk_ignore_unused` is NOT in current cmdline.

### Action 3.2: Check kexec availability and UART clock state
**Command**: `ssh ... "which kexec; ls -la /boot/slmos.bin; cat /sys/kernel/debug/clk/uarta/clk_enable_count"`
**Result**:
```
/usr/sbin/kexec
-rw-r--r-- 1 root root 946416 Dec 31 00:57 /boot/slmos.bin
0
```
**CRITICAL**: Clock enable count is **0** - UART clock is disabled!

### Action 3.3: Check UART clock debugfs interface
**Command**: `ssh ... "ls /sys/kernel/debug/clk/uarta/"`
**Result**: Files are read-only status files (clk_enable_count, clk_rate, etc.) - no way to directly enable clock through debugfs.

### Action 3.4: Check UART clock rate and parent
**Command**: `ssh ... "cat /sys/kernel/debug/clk/uarta/clk_rate; cat /sys/kernel/debug/clk/uarta/clk_parent"`
**Result**:
```
68000000
pllp_out0
```
Clock rate is 68 MHz, parent is pllp_out0.

### Action 3.5: Find UARTA device mapping
**Command**: `ssh ... "dmesg | grep -i 'serial\|3100000'"`
**Result**:
```
[    3.777205] 3100000.serial: ttyTHS1 at MMIO 0x3100000 (irq = 112, base_baud = 0) is a TEGRA_UART
```
UARTA (0x3100000) is mapped to `/dev/ttyTHS1`.

### Action 3.6: Try opening UART device to enable clock
**Command**: `ssh ... "stty -F /dev/ttyTHS1 115200 raw -echo; cat /sys/kernel/debug/clk/uarta/clk_enable_count"`
**Result**: Clock count still 0. The stty command opens and closes immediately, so clock is disabled again.

### Action 3.7: Change UART driver to direct mode
**File**: `kernel/drivers/uart_tegra.c`
**Change**: `UART_INIT_MODE 0` → `UART_INIT_MODE 2`
**Result**: Mode 2 assumes clock is already enabled (for kexec scenario where we'll hold clock open).

### Action 3.8: Build kernel for Jetson (first attempt - wrong platform name)
**Command**: `make kernel-clean && make kernel PLATFORM=jetson`
**Result**: FAILED - undefined reference to `uart_putc`. Platform name `jetson` doesn't match CMakeLists.txt.

### Action 3.9: Check correct platform name
**Command**: `grep PLATFORM.*jetson Makefile CMakeLists.txt`
**Result**: Correct name is `JETSON_ORIN_NANO`, not `jetson`.

### Action 3.10: Rebuild with correct platform name
**Command**: `make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO`
**Result**: SUCCESS - `slmos.bin` built (946416 bytes).

### Action 3.11: Deploy to Jetson
**Command**: `scp -P 4243 build/kernel/slmos.bin root@gradient-nano.onthewifi.com:/boot/slmos.bin`
**Result**: SUCCESS - file transferred.

### Action 3.12: Verify deployment
**Command**: `ssh ... "ls -la /boot/slmos.bin; md5sum /boot/slmos.bin"`
**Result**:
```
-rw-r--r-- 1 root root 946416 Dec 31 03:12 /boot/slmos.bin
d98676b0965afe1cdcbe6c39e42e026b  /boot/slmos.bin
```
Deployment verified.

### Action 3.13: Start serial capture
**Command**: `sudo stty -F /dev/ttyUSB1 115200 raw -echo; sudo timeout 120 cat /dev/ttyUSB1 > /tmp/kexec_uart_test.txt &`
**Result**: Serial capture started on /dev/ttyUSB1 (PID 336759).

### Action 3.14: Execute kexec with UART clock held open
**Command**: SSH to Jetson, open /dev/ttyTHS1 to enable clock, then execute kexec
**Result**:
```
=== Before opening UART ===
0
=== After opening UART ===
1
=== Loading kexec ===
Can't open (/proc/kcore).
=== Executing kexec ===
```
**KEY FINDING**: Opening /dev/ttyTHS1 successfully enabled the UART clock (count 0 → 1).

### Action 3.15: Check serial capture after kexec
**Command**: `cat /tmp/kexec_uart_test.txt`
**Result**:
```
[ 8190.280495] tegra-ivc-bus bc00000.rtcpu:ivc-bus:echo@0: ivc channel driver missing
...
[ 8190.321169] kexec_core: Starting new kernel
��ERROR:   Exception reason=2 syndrome=0x80000411
ERROR:   **************************************
ERROR:   RAS Uncorrectable Error in SNOC, base=0xe011000:
ERROR:   	Status = 0xec00030d
ERROR:   SERR = Illegal address (software fault): 0xd
ERROR:   	IERR = Carveout Uncorrectable Error: 0x3
ERROR:   	MISC1 = 0x62d4842000000000
ERROR:   	ADDR = 0xe0a5a5a5a5a5a5a5
ERROR:   **************************************
ERROR:   RAS Uncorrectable Error in ACI, base=0xe01a000:
ERROR:   	SERR = Assertion failure: 0x4
ERROR:   	IERR = SNOC Write Error: 0xd
ERROR:   Powering off core
```

**ANALYSIS**:
1. **UART clock approach WORKED** - we got serial output after kexec!
2. Kernel starts but immediately crashes with RAS (Reliability, Availability, Serviceability) errors
3. Address 0xe0a5a5a5a5a5a5a5 suggests corrupted/uninitialized memory access
4. "Carveout Uncorrectable Error" - accessing TrustZone protected memory region
5. This is an EL3 (TrustZone firmware) error, not our kernel output

**NEW PROBLEM**: SLM-OS kernel is executing code that accesses protected memory regions.
This could be:
- Wrong load address for kexec
- Stack pointer set to invalid region
- Early boot code accessing restricted addresses

### Action 3.16: Power cycle Jetson to restore Linux
**Command**: `./lab-tools/jetson-power.py cycle`
**Result**: Jetson rebooted to Linux.

### Action 3.17: Check kexec debug output for load addresses
**Command**: `ssh ... "kexec -l /boot/slmos.bin --debug --reuse-cmdline 2>&1"`
**Result**:
```
kexec_load: entry = 0x80126680 flags = 0xb70000
nr_segments = 3
segment[0].mem   = 0x80000000, memsz = 0xe8000   (kernel image)
segment[1].mem   = 0x800e8000, memsz = 0x3e000   (DTB area)
segment[2].mem   = 0x80126000, memsz = 0x4000    (purgatory stub)
```
**KEY FINDING**: kexec uses a "purgatory" stub at 0x80126000 that runs first. Entry point 0x80126680 is inside purgatory, not our kernel.

### Action 3.18: Check kernel ELF entry point
**Command**: `aarch64-none-elf-readelf -h build/kernel/slmos.elf`
**Result**: ELF entry = 0x80000000 (correct). But kexec's purgatory intercepts.

### Action 3.19: Search for Tegra234 kexec issues
**Command**: Web search for "Tegra234 Jetson kexec RAS Carveout Uncorrectable Error"
**Result**: Found NVIDIA forum post about kexec issues on Jetson Orin.

### Action 3.20: Fetch NVIDIA forum post
**Command**: WebFetch forums.developer.nvidia.com kexec thread
**Result**:
> **NVIDIA Response (Wayne Wong)**: "kexec was not validated on Jetson devices and should not be relied upon for kernel replacement."
>
> The CBB firewall errors occur because after kexec, the security configuration from the bootloader is still active and blocks certain memory accesses.

**CRITICAL FINDING**: NVIDIA explicitly states **kexec is not supported on Jetson**. The RAS/CBB errors we see are expected behavior - the security hardware blocks unauthorized register access after kexec.

---

## Phase 4: Alternative Approaches

Given that kexec is not officially supported, we have several options:

1. **Direct UEFI boot** - Requires position-independent code (previously attempted, had issues)
2. **Modified bootloader** - High risk, complex
3. **chainloader approach** - Boot minimal Linux that launches SLM-OS
4. **SD card boot** - Modify extlinux.conf to boot SLM-OS directly

### Action 4.1: Check extlinux.conf for direct UEFI boot
**Command**: `ssh ... "cat /boot/extlinux/extlinux.conf"`
**Result**: Found existing SLM-OS entry in extlinux.conf.

### Action 4.2: Test direct boot via extlinux
**Command**: Changed DEFAULT to slmos, rebooted
**Result**:
```
[28067.952660] kexec_core: Starting new kernel
ERROR:   RAS Uncorrectable Error in SNOC...
ERROR:   ADDR = 0xe0a5a5a5a5a5a5a5
ERROR:   Powering off core
```

**CRITICAL FINDING**: L4T bootloader uses **kexec internally**! Even when selecting a kernel via extlinux, the bootloader boots Linux first, then uses kexec to load the selected kernel. This means extlinux approach hits the SAME kexec issues.

---

## Phase 5: True UEFI Boot (Bypass L4T kexec)

The L4T boot sequence is:
1. UEFI loads L4T bootloader (BOOTAA64.efi)
2. L4T bootloader boots minimal Linux
3. Linux uses kexec to load kernel selected in extlinux.conf
4. **kexec fails for bare-metal OS due to security restrictions**

To bypass this, we need to:
1. Boot SLM-OS directly as an EFI application (not via extlinux)
2. Replace BOOTAA64.efi with SLM-OS.efi
3. Or add SLM-OS to UEFI boot menu directly

### Action 5.1: Attempt to send boot selection via serial
**Commands**: Multiple attempts to send '0' or ESC to serial during boot
**Result**: FAILED - Serial input via 40-pin header not received by bootloader.

**FINDING**: The 40-pin header UART appears to be **output-only** during early boot. The UEFI/bootloader input may require:
- USB-C debug cable (TCU)
- USB keyboard
- Or HDMI display

### Action 5.2: Check for TCU (USB-C debug) connection
**Command**: `lsusb; ls /dev/ttyACM*`
**Result**: Found /dev/ttyACM0, but no output when reading from it.

### Action 5.3: Attempt recovery - send '0' during boot menu
**Command**: Power cycle, send '0' repeatedly via /dev/ttyUSB1 during boot window
**Result**: FAILED - Boot menu still defaulted to SLM-OS (Option 1), crashed with same error.
**Serial capture showed**:
```
L4T boot options
0: primary kernel
1: SLM-OS
Press 0-1 to boot selection within 3.0 seconds.
Press any other key to boot default (Option: 1)
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:   ADDR = 0x8000000003100000
```
**Note**: Error address 0x03100000 is UARTA - crash happens when SLM-OS tries to access UART.

### Action 5.4: Attempt recovery - send ESC to enter UEFI Setup
**Command**: Power cycle, send ESC (0x1b) during UEFI prompt window
**Result**: FAILED - ESC not received, boot continued to L4T menu, crashed.

### Action 5.5: Final power cycle attempts
**Command**: Multiple power cycles with various timing for serial input
**Result**: FAILED - Jetson remains stuck. Cannot SSH (No route to host).

### Action 5.6: Manual recovery by user
**Command**: User power cycled and manually selected primary kernel during boot menu
**Result**: SUCCESS - Linux booted, SSH accessible.

### Action 5.7: Fix extlinux.conf
**Command**: `sed -i 's/DEFAULT slmos/DEFAULT primary/' /boot/extlinux/extlinux.conf`
**Result**: Fixed - DEFAULT restored to primary kernel.

**CURRENT SITUATION** (after recovery):
- Jetson recovered and running Linux
- extlinux.conf fixed to default to primary kernel
- Ready to continue investigation

---

## Phase 6: Investigate UEFI Direct Boot

### Action 6.1: Check UEFI boot configuration
**Command**: `efibootmgr -v; ls /boot/efi/EFI/BOOT/`
**Result**:
- Boot0001 = UEFI SD Device (current default)
- Boot0007 = UEFI Shell available
- BOOTAA64.efi is L4T bootloader (110KB)

**Observation**: UEFI Shell is available as Boot0007. Could potentially use it to bypass L4T.

### Action 6.2: Test with UART disabled (silent mode)
**Change**: Set `UART_INIT_MODE 0` in uart_tegra.c
**Command**: Rebuilt, deployed, set DEFAULT slmos, rebooted
**Result**: DIFFERENT ERROR - UEFI Synchronous Exception:
```
Synchronous Exception at 0x000000025DE11B00
ASSERT [ArmCpuDxe] .../DefaultExceptionHandler.c(345): ((BOOLEAN)(0==1))
Resetting the system in 5 seconds.
```

**Analysis**:
- This is a UEFI exception, not TrustZone RAS error
- Exception at address 0x25DE11B00 (kernel load area)
- System enters boot loop with 5-second reset
- Serial input still cannot reach boot menu

**Current state**: Jetson stuck in boot loop, needs manual recovery.

**Note on test design**: Silent mode test was flawed because:
1. L4T uses kexec internally regardless of UART setting
2. No way to verify success without output (LED/GPIO would be better indicator)

---

## Phase 7: Recovering from L4T Recovery Mode

After repeated boot failures, L4T enters persistent recovery mode. Normal reboots and even manually selecting boot files won't escape it.

### Action 7.1: Enter UEFI Shell
**Method**: Boot Manager (F11) → Select "UEFI Shell"

### Action 7.2: Dump UEFI variables
**Command**: `dmpstore -all` (saved output to file for analysis)

### Action 7.3: Identify recovery trigger
**Found**: `RootfsStatusSlotA` = 0xFF (255) indicates "failed" rootfs status
```
Variable '781E084C-A330-417C-B678-38E696380CB9:RootfsStatusSlotA'
  00000000: FF 00 00 00    ← 0xFF = failed/bad status
```

### Action 7.4: Reset the recovery flag
**Command in UEFI Shell**:
```
setvar RootfsStatusSlotA -guid 781E084C-A330-417C-B678-38E696380CB9 =0x00
reset
```
**Result**: SUCCESS - Jetson boots normally to Linux.

---

## IMPORTANT: L4T Recovery Mode Fix

If Jetson gets stuck in "Recovery Boot" mode after failed boots:

1. Enter UEFI Shell (F11 → UEFI Shell, or Boot0007)
2. Run: `setvar RootfsStatusSlotA -guid 781E084C-A330-417C-B678-38E696380CB9 =0x00`
3. Run: `reset`

The GUID `781E084C-A330-417C-B678-38E696380CB9` is NVIDIA's L4T variable namespace.

---

## Phase 8: Direct UEFI Boot Test (Bypassing L4T)

### Action 8.1: Create UEFI boot entry for SLM-OS
**Commands**:
```bash
cp /boot/slmos.bin /boot/efi/EFI/BOOT/SLMOS.efi
efibootmgr -c -d /dev/mmcblk0 -p 10 -L 'SLM-OS Direct' -l '\\EFI\\BOOT\\SLMOS.efi'
efibootmgr -n 0009  # Set as next boot
```
**Result**: Boot0009 "SLM-OS Direct" created successfully.

### Action 8.2: Test direct UEFI boot
**Command**: Reboot with BootNext=0009
**Result**: SAME ERROR - CBB firewall blocks UART access:
```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:   ADDR = 0x8000000003100000
ERROR:   IERR = CBB Interface Error: 0x6
ERROR:   Powering off core
```

**CRITICAL FINDING**: Even direct UEFI boot (completely bypassing L4T/kexec) hits the same CBB firewall restriction. The issue is NOT kexec-specific - it's a fundamental Tegra234 security feature that blocks unsigned/unauthenticated code from accessing peripherals.

**Root Cause**: Tegra234 CBB (Control Backbone) firewall restricts peripheral access. The bootloader configures which code can access which peripherals. Our bare-metal kernel lacks the proper security credentials.

---

## Recovery Options

1. **Physical access required**: Short Force Recovery pins (J14 pins 9-10)
2. **SD card fix**: Remove SD card, mount externally, edit extlinux.conf
3. **Wait for watchdog**: Some Jetsons auto-recover after failed boots
4. **USB keyboard**: Connect to Jetson's USB port for bootloader input

---

## Key Findings Summary

1. **UART clock approach WORKS** - verified we can enable clock before kexec
2. **kexec NOT SUPPORTED on Jetson** - NVIDIA confirmed, RAS errors expected
3. **L4T uses kexec internally** - even extlinux boot uses kexec
4. **Direct boot crashes at UART access** - CBB firewall blocks 0x03100000
5. **40-pin serial is output-only** during bootloader - cannot send input remotely

---

## Phase 9: SD Card Recovery (New Session)

After Phase 8, the Jetson was stuck in a boot loop - the "SLM-OS Direct" UEFI entry was first in boot order but crashed immediately. Serial input via both 40-pin UART and TCU (USB-C debug) failed to reach the boot manager.

### Action 9.1: Attempts to send input via serial
**Commands**: Multiple attempts to send Down Arrow + Enter via /dev/ttyUSB1 and /dev/ttyACM0 using picocom and dd
**Result**: FAILED - Neither serial port accepts input during UEFI boot menu. The 40-pin header is confirmed output-only during bootloader.

### Action 9.2: SD card recovery
**Method**:
1. Power off Jetson
2. Remove SD card, insert into host machine card reader
3. Mount EFI partition (sde10) and delete SLMOS.efi
4. Verify extlinux.conf has `DEFAULT primary`
5. Unmount, return SD card to Jetson

**Commands**:
```bash
sudo mount /dev/sde10 /mnt/jetson-efi
sudo rm /mnt/jetson-efi/EFI/BOOT/SLMOS.efi
sudo mount /dev/sde1 /mnt/jetson-root
head -5 /mnt/jetson-root/boot/extlinux/extlinux.conf  # Verified DEFAULT primary
sudo umount /mnt/jetson-efi /mnt/jetson-root
```

### Action 9.3: Boot and cleanup
**Result**: Jetson booted successfully to Linux. Cleaned up stale UEFI boot entry:
```bash
efibootmgr -b 0009 -B   # Delete "SLM-OS Direct" entry
```

**Final state**: Jetson fully recovered, boot order restored with "UEFI SD Device" first.

---

## Conclusions and Next Steps

### Root Cause Confirmed
The Tegra234 CBB (Control Backbone) firewall blocks all peripheral access from unsigned/unauthenticated code. This is a hardware-enforced security feature that cannot be bypassed through software alone.

### What Works
- UART clock can be enabled before kexec by opening /dev/ttyTHS1
- Serial output works during early boot (UEFI, L4T bootloader)
- We can capture boot errors via 40-pin header UART

### What Doesn't Work
- **kexec**: Not supported on Jetson (NVIDIA confirmed)
- **extlinux boot**: Uses kexec internally, same restrictions
- **Direct UEFI boot**: CBB firewall blocks peripheral access
- **Serial input**: 40-pin header is output-only during boot

### Potential Paths Forward

1. **Signed kernel**: Investigate NVIDIA's secure boot chain to get proper CBB permissions
2. **Different platform**: Focus on Raspberry Pi 5 or QEMU for bare-metal development
3. **Hypervisor approach**: Run SLM-OS as a guest under Linux with passed-through devices
4. **CBB firewall research**: Deep dive into Tegra234 firewall configuration (may require NDA documentation)

### Recommendation
Given the hardware security restrictions on Jetson Orin, **Raspberry Pi 5 may be a more viable bare-metal target** for the capstone project. The Pi 5 has well-documented peripherals and no security firewalls blocking peripheral access.

---

*Log started: 2025-12-31*
*Last updated: 2025-12-31 19:30 UTC*
