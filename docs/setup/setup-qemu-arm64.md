# Setting Up QEMU for ARM64 Emulation

This guide covers installing and configuring QEMU for ARM64 bare-metal development on SLM-OS using the `virt` machine type.

---

## What is QEMU virt?

The `virt` machine is a generic ARM64 virtual platform designed for:

- Bare-metal and OS development
- No specific hardware to emulate (simpler than real board emulation)
- Fast iteration without physical hardware
- Easy debugging with GDB integration

It provides:

- Configurable CPU cores (Cortex-A53, A57, A72, A76, etc.)
- PL011 UART for serial output
- GICv2/GICv3 interrupt controller
- Configurable RAM
- Virtio devices (block, network, etc.)

---

## Prerequisites

- Windows 10/11 (or Linux/macOS)
- Administrator access (for initial install)

---

## Step 1: Install QEMU

### Option A: Using winget (Windows - Recommended)

```powershell
winget install SoftwareFreedomConservancy.QEMU
```

### Option B: Download Installer (Windows)

1. Go to https://qemu.weilnetz.de/w64/
2. Download the latest `qemu-w64-setup-*.exe`
3. Run the installer
4. Select components (at minimum, select ARM system emulation)

### Option C: Using MSYS2 (Windows)

```bash
pacman -S mingw-w64-x86_64-qemu
```

### Option D: Linux

```bash
# Ubuntu/Debian
sudo apt install qemu-system-arm

# Fedora
sudo dnf install qemu-system-aarch64

# Arch
sudo pacman -S qemu-system-aarch64
```

### Option E: macOS

```bash
brew install qemu
```

---

## Step 2: Add to PATH (Windows)

If not automatically added:

1. Open **System Properties → Environment Variables**
2. Edit **Path** under User or System variables
3. Add: `C:\Program Files\qemu` (or your installation path)

---

## Step 3: Restart Terminal/CLion

Close and reopen your terminal to pick up PATH changes.

---

## Step 4: Verify Installation

```powershell
qemu-system-aarch64 --version
```

Expected output (version number will vary):

```
QEMU emulator version 10.1.0 (or similar)
Copyright (c) 2003-2024 Fabrice Bellard and the QEMU Project developers
```

Any version 6.0 or newer will work for SLM-OS development.

List available machines:

```powershell
qemu-system-aarch64 -machine help
```

You should see `virt` in the list.

List available CPUs:

```powershell
qemu-system-aarch64 -machine virt -cpu help
```

---

## Running SLM-OS in QEMU

### Basic Command

```powershell
qemu-system-aarch64 `
    -machine virt `
    -cpu cortex-a72 `
    -m 512M `
    -nographic `
    -kernel kernel.elf
```

### Command Breakdown

| Option | Description |
|--------|-------------|
| `-machine virt` | Use the generic ARM64 virtual machine |
| `-cpu cortex-a72` | Emulate Cortex-A72 CPU (similar to A76/A78) |
| `-m 512M` | 512 MB of RAM |
| `-nographic` | No GUI, serial output to terminal |
| `-kernel kernel.elf` | Load kernel ELF directly (no bootloader) |

### Multi-Core Configuration

```powershell
qemu-system-aarch64 `
    -machine virt `
    -cpu cortex-a72 `
    -smp cores=4 `
    -m 1G `
    -nographic `
    -kernel kernel.elf
```

| Option | Description |
|--------|-------------|
| `-smp cores=4` | Emulate 4 CPU cores |
| `-m 1G` | 1 GB of RAM |

### With Serial Output to File

```powershell
qemu-system-aarch64 `
    -machine virt `
    -cpu cortex-a72 `
    -m 512M `
    -nographic `
    -serial file:serial.log `
    -kernel kernel.elf
```

---

## UART Output

The virt machine has a PL011 UART at address `0x09000000`. This is where your kernel's debug output will appear.

### UART Register Map (PL011)

| Register | Offset | Description |
|----------|--------|-------------|
| DR | 0x000 | Data Register (read/write) |
| FR | 0x018 | Flag Register |
| IBRD | 0x024 | Integer Baud Rate |
| FBRD | 0x028 | Fractional Baud Rate |
| LCR_H | 0x02C | Line Control Register |
| CR | 0x030 | Control Register |

For basic output, you only need to write to DR (0x09000000).

### Minimal UART Output (C)

```c
#define UART_BASE 0x09000000
#define UART_DR   (*(volatile unsigned int *)(UART_BASE))

void uart_putc(char c) {
    UART_DR = c;
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}
```

---

## Debugging with GDB

### Start QEMU with GDB Server

```powershell
qemu-system-aarch64 `
    -machine virt `
    -cpu cortex-a72 `
    -m 512M `
    -nographic `
    -kernel kernel.elf `
    -S `
    -gdb tcp::1234
```

| Option | Description |
|--------|-------------|
| `-S` | Pause CPU at startup (wait for debugger) |
| `-gdb tcp::1234` | Listen for GDB on port 1234 |

### Connect with GDB

In another terminal:

```powershell
aarch64-none-elf-gdb kernel.elf
```

Then in GDB:

```gdb
target remote localhost:1234
break kernel_main
continue
```

### Useful GDB Commands

| Command | Description |
|---------|-------------|
| `target remote localhost:1234` | Connect to QEMU |
| `break kernel_main` | Set breakpoint |
| `continue` or `c` | Continue execution |
| `stepi` | Step one instruction |
| `info registers` | Show all registers |
| `x/10i $pc` | Disassemble 10 instructions at PC |
| `x/10x $sp` | Show 10 words at stack pointer |

---

## Memory Map (virt machine)

| Address Range | Size | Description |
|---------------|------|-------------|
| 0x00000000 - 0x07FFFFFF | 128 MB | Flash memory |
| 0x08000000 - 0x08FFFFFF | 16 MB | GIC (Interrupt Controller) |
| 0x09000000 - 0x09000FFF | 4 KB | PL011 UART |
| 0x09010000 - 0x09010FFF | 4 KB | RTC |
| 0x0A000000 - 0x0AFFFFFF | 16 MB | Virtio MMIO devices |
| 0x40000000 - ... | Configurable | RAM (starts here by default) |

Note: When using `-kernel`, QEMU loads your kernel at `0x40000000` and sets the PC there.

---

## CLion Integration

### Create Run Configuration

1. **Run → Edit Configurations**
2. Click **+** → **Shell Script**
3. Configure:
   - **Name:** `Run in QEMU`
   - **Script text:**
     ```
     qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic -kernel $PROJECT_DIR$/build/kernel.elf
     ```
   - **Working directory:** `$ProjectFileDir$`

### Create Debug Configuration

1. **Run → Edit Configurations**
2. Click **+** → **Shell Script**
3. Configure:
   - **Name:** `QEMU Debug Server`
   - **Script text:**
     ```
     qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic -kernel $PROJECT_DIR$/build/kernel.elf -S -gdb tcp::1234
     ```

Then create a **Remote Debug** configuration to connect GDB to port 1234.

---

## Exiting QEMU

When running with `-nographic`:

- Press `Ctrl+A` then `X` to quit
- Press `Ctrl+A` then `C` to enter QEMU monitor
- In monitor, type `quit` to exit

---

## Common Issues

### "qemu-system-aarch64 is not recognized"

QEMU is not in PATH. Add the installation directory to your PATH environment variable.

### Kernel doesn't boot / no output

1. Verify kernel is built for correct address (0x40000000)
2. Check linker script entry point
3. Ensure UART code writes to 0x09000000
4. Try adding `-d in_asm` to see executed instructions

### "Could not initialize SDL"

Use `-nographic` flag to disable graphical output, or install SDL2.

### Kernel hangs immediately

Add `-d int` to see interrupts/exceptions:

```powershell
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic -kernel kernel.elf -d int
```

### View QEMU debug output

```powershell
qemu-system-aarch64 ... -d in_asm,cpu -D qemu.log
```

This logs executed instructions and CPU state to `qemu.log`.

---

## Quick Reference

### Minimal Test Command

```powershell
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic -kernel kernel.elf
```

### Debug Command

```powershell
qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic -kernel kernel.elf -S -gdb tcp::1234
```

### Exit QEMU

`Ctrl+A` then `X`

---

## Additional Resources

- [QEMU ARM Documentation](https://www.qemu.org/docs/master/system/target-arm.html)
- [QEMU virt Machine](https://www.qemu.org/docs/master/system/arm/virt.html)
- [PL011 UART Reference](https://developer.arm.com/documentation/ddi0183/latest/)

---

*Last updated: December 2025*
