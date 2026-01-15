# Jetson Orin Nano Kexec Debugging - Session Summary

This document captures the debugging work done to get SLM-OS booting on the Jetson Orin Nano via kexec. Use this to continue development on another machine.

---

## Problem Statement

After implementing kexec boot support, SLM-OS would:
1. Hang immediately after kexec (spinlock issue)
2. Reboot back to Linux after ~2 minutes (watchdog issue)
3. Produce no serial output (UART configuration issue)

---

## Fixes Implemented

### 1. Spinlock Bypass for Jetson (Completed)

**Problem:** After kexec, the ARM64 exclusive monitor is in a corrupted state. LDAXR/STXR instructions (used for spinlocks) hang indefinitely.

**Solution:** Centralized spinlock bypass in `kernel/include/spinlock.h`. All spinlock operations become no-ops on Jetson (just memory barriers).

**Key changes in `kernel/include/spinlock.h`:**
```c
static inline void spin_lock(spinlock_t *lock)
{
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Jetson: skip locking, just barrier for memory ordering */
    (void)lock;
    dmb(ish);
#else
    /* Other platforms: use WFE for low-power spinning */
    // ... LDAXR/STXR implementation
#endif
}
```

Functions modified:
- `spin_lock()` - No-op with DMB barrier
- `spin_unlock()` - No-op with DMB barrier
- `spin_trylock()` - Always returns 1 (success)
- `spin_lock_irqsave()` - Still disables IRQs, skips actual lock
- `spin_unlock_irqrestore()` - Still restores IRQs, skips actual unlock

**Note:** This is safe because after kexec we're running single-core. The IRQ disable/enable still provides protection against interrupt handlers.

---

### 2. Watchdog Timer Disable (Completed)

**Problem:** Linux starts a hardware watchdog with 120-second timeout. After kexec, SLM-OS doesn't feed the watchdog, causing automatic reboot.

**Solution:** Disable the Tegra watchdog early in `kernel_main()` before any time-consuming initialization.

**Register definitions added to `kernel/include/platform.h`:**
```c
/* Watchdog Timer (WDT) */
#define WDT_BASE            0x02190000UL
#define WDT_CFG             0x0
#define WDT_STS             0x4
#define WDT_CMD             0x8
#define WDT_UNLOCK          0xC
#define WDT_UNLOCK_PATTERN  0xC45A
#define WDT_CMD_DISABLE     0x2
```

**Disable code added to `kernel/src/main.c` (in `kernel_main()`):**
```c
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Clear exclusive monitor (existing code) */
    __asm__ volatile(
        "clrex\n"
        "sevl\n"
        "wfe\n"
        ::: "memory"
    );

    /* Disable hardware watchdog timer */
    {
        volatile uint32_t *wdt_unlock = (volatile uint32_t *)(WDT_BASE + WDT_UNLOCK);
        volatile uint32_t *wdt_cmd = (volatile uint32_t *)(WDT_BASE + WDT_CMD);

        /* Unlock the watchdog registers */
        *wdt_unlock = WDT_UNLOCK_PATTERN;
        __asm__ volatile("dsb sy" ::: "memory");

        /* Disable the watchdog counter */
        *wdt_cmd = WDT_CMD_DISABLE;
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif
```

**Verification:** SLM-OS now runs for 5+ minutes without automatic reboot (previously rebooted at ~2 minutes).

---

### 3. UART Direct Mode (Completed, Untested)

**Problem:** The UART driver was set to silent mode (UART_INIT_MODE=0), producing no output.

**Solution:** Changed to "direct mode" which assumes UART clock is already enabled from Linux.

**Changes in `kernel/drivers/uart_tegra.c`:**
```c
/*
 * UART initialization modes:
 * 0 = Skip UART entirely (silent mode)
 * 1 = Try BPMP to enable clock (full initialization)
 * 2 = Direct mode - assume clock is already enabled (after kexec)
 */
#define UART_INIT_MODE 2

#if UART_INIT_MODE == 2
    /*
     * Direct mode - assume UART clock is already enabled.
     * After kexec from Linux, the UART hardware should still be
     * configured and clocked. Just reinitialize the UART settings.
     */
    g_uart_available = true;
#endif
```

**Why this should work:** Linux dmesg shows UARTA is active:
```
[3.768838] 3100000.serial: ttyTHS1 at MMIO 0x3100000 (irq = 112, base_baud = 0) is a TEGRA_UART
```
The kernel command line includes `console=ttyTHS1,115200`.

---

## Current State

### What's Working
- Kernel boots via kexec without hanging
- Watchdog doesn't cause automatic reboot
- Kernel runs for extended periods (5+ minutes confirmed)
- All kernel initialization appears to complete (verified via checkpoint testing)

### What's Untested
- SLM-OS serial output after kexec boot
- Scheduler actually running tasks (no visibility without UART)
- Shell interactivity

### Serial Hardware Status

**Resolved (December 28, 2025):** Serial hardware is working. Lab setup moved from Windows PC to Ubuntu machine.

**Verified working:**
- Bidirectional serial at 115200 baud between Ubuntu host and Jetson
- Local `/dev/ttyUSB0` ↔ Jetson `/dev/ttyTHS1`
- Test: `echo "TEST" > /dev/ttyTHS1` from Jetson is received on Ubuntu

**40-pin Header UART Pinout (UARTA):**
| Pin | Function | Connect to |
|-----|----------|------------|
| 6   | GND      | Adapter GND |
| 8   | UART1_TX | Adapter RX |
| 10  | UART1_RX | Adapter TX |

**Serial Settings:** 115200 baud, 8N1, no flow control

---

## Files Modified

| File | Changes |
|------|---------|
| `kernel/include/spinlock.h` | Spinlock bypass for Jetson (all lock functions) |
| `kernel/include/platform.h` | Added WDT register definitions |
| `kernel/src/main.c` | Added watchdog disable code after exclusive monitor clear |
| `kernel/drivers/uart_tegra.c` | Changed UART_INIT_MODE from 0 to 2 |
| `kernel/mm/pmm.c` | Reverted per-file spinlock bypass (now uses centralized) |
| `kernel/src/kprintf.c` | Reverted per-file spinlock bypass (now uses centralized) |

---

## Build Commands

```bash
# Build for Jetson (from project root or with -C flag)
make BUILD_DIR=/path/to/build PLATFORM=JETSON_ORIN_NANO kernel-clean kernel

# The kernel ELF is at: $BUILD_DIR/kernel/slmos.elf
```

---

## Deployment Commands

```bash
# Copy kernel to Jetson
scp -P 4243 /path/to/slmos.elf root@gradient-nano.onthewifi.com:/root/

# SSH to Jetson and run kexec
ssh -p 4243 root@gradient-nano.onthewifi.com
kexec -l /root/slmos.elf --reuse-cmdline
kexec -e
# (connection will drop - this is expected)

# Power control (if needed)
python3 lab-tools/jetson-power.py cycle  # Power cycle
python3 lab-tools/jetson-power.py status # Check power state
```

---

## Next Steps

1. ~~**Fix serial hardware**~~ - ✅ Resolved - serial working on Ubuntu lab machine
2. **Test UART output** - Boot SLM-OS via kexec, should see banner and boot messages
3. **Verify scheduler** - With serial working, can confirm tasks are running
4. **Test shell** - Interactive shell via UART should work once serial is connected

---

## Technical References

### Tegra Watchdog
- Base address: `0x02190000`
- Unlock pattern: `0xC45A` (write to offset 0xC)
- Disable command: `0x2` (write to offset 0x8)
- Default timeout: 120 seconds
- Linux driver: `drivers/watchdog/tegra_wdt.c`

### UART (UARTA)
- Base address: `0x03100000`
- Type: NS16550-compatible with 32-bit access (reg-shift=2)
- Clock: 408 MHz (UART_CLOCK in platform.h)
- Linux device: `/dev/ttyTHS1`

### ARM64 Exclusive Monitor Issue
After kexec, the exclusive monitor state is corrupted. The CLREX instruction doesn't fully reset it. LDAXR/STXR sequences hang even after CLREX+SEVL+WFE sequence. Workaround: bypass spinlocks entirely on Jetson (safe for single-core operation).

---

## Debugging Tips

### Checkpoint Testing
To verify kernel reaches a certain point, add a PSCI reboot:
```c
/* Add this anywhere in kernel_main() to create a checkpoint */
{
    register uint64_t x0 __asm__("x0") = 0x84000009;  /* SYSTEM_RESET */
    __asm__ volatile("smc #0" : "+r"(x0) :: "memory");
    while(1) __asm__ volatile("wfi");
}
```
If the Jetson reboots back to Linux after kexec, the kernel reached that point.

### Serial Monitoring

**Ubuntu:**
```bash
# Use the lab-tools script
./lab-tools/jetson-uart.sh

# Or directly with picocom
sudo picocom -b 115200 /dev/ttyUSB0
```

**Windows (Cygwin):**
```bash
# Use env -i to get proper Cygwin mounts (not Git Bash)
C:/cygwin64/bin/env.exe -i HOME=/tmp PATH=/usr/bin:/bin:/usr/local/bin \
  C:/cygwin64/bin/bash.exe --login -c \
  "picocom -b 115200 /dev/ttyS4 --noreset"
```

### Linux UART Test
From Linux on Jetson, test serial output:
```bash
echo "TEST MESSAGE" > /dev/ttyTHS1
```

---

## See Also

**`docs/jetson-nvidia-support.md`** — Comprehensive documentation of all Jetson blockers, NVIDIA forum research, and potential solutions. This document consolidates findings from all debugging sessions.

**Key finding:** kexec is NOT supported by NVIDIA, and the CBB firewall blocks all bare-metal peripheral access. See the consolidated document for potential paths forward.

---

*Document created: December 28, 2025*
*Last update: January 15, 2026 - Added cross-reference to jetson-nvidia-support.md*
