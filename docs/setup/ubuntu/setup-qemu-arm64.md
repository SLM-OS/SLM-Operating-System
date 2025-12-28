# Setting Up QEMU for ARM64 Emulation on Ubuntu

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

- Ubuntu 22.04 LTS or newer
- `sudo` access

---

## Step 1: Install QEMU

Install the QEMU ARM system emulator:

```bash
sudo apt install -y qemu-system-arm
```

This installs:
- `qemu-system-aarch64` - ARM64 system emulator
- `qemu-system-arm` - ARM32 system emulator (also included)

---

## Step 2: Verify Installation

```bash
qemu-system-aarch64 --version
```

Expected output:

```
QEMU emulator version 6.2.0 (Debian 1:6.2+dfsg-2ubuntu6)
Copyright (c) 2003-2021 Fabrice Bellard and the QEMU Project developers
```

Any version 6.0 or newer will work for SLM-OS development.

List available machines:

```bash
qemu-system-aarch64 -machine help
```

The `virt` machine should appear in the list.

List available CPUs:

```bash
qemu-system-aarch64 -machine virt -cpu help
```

---

## Running SLM-OS in QEMU

### Using the Makefile (Recommended)

The project Makefile provides convenient targets:

```bash
make run      # Build and run in QEMU
make debug    # Build and start QEMU with GDB server
make gdb      # Connect GDB to running QEMU (run in 2nd terminal)
```

### Manual Command

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -m 512M \
    -nographic \
    -kernel build/kernel/slmos.elf
```

### Command Breakdown

| Option | Description |
|--------|-------------|
| `-machine virt` | Use the generic ARM64 virtual machine |
| `-cpu cortex-a76` | Emulate Cortex-A76 CPU (matches Raspberry Pi 5) |
| `-m 512M` | 512 MB of RAM |
| `-nographic` | No GUI, serial output to terminal |
| `-kernel build/kernel/slmos.elf` | Load kernel ELF directly (no bootloader) |

### Multi-Core Configuration

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -smp cores=4 \
    -m 1G \
    -nographic \
    -kernel build/kernel/slmos.elf
```

| Option | Description |
|--------|-------------|
| `-smp cores=4` | Emulate 4 CPU cores |
| `-m 1G` | 1 GB of RAM |

### With Serial Output to File

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -m 512M \
    -nographic \
    -serial file:serial.log \
    -kernel build/kernel/slmos.elf
```

---

## UART Output

The virt machine has a PL011 UART at address `0x09000000`. This is where kernel debug output appears.

### UART Register Map (PL011)

| Register | Offset | Description |
|----------|--------|-------------|
| DR | 0x000 | Data Register (read/write) |
| FR | 0x018 | Flag Register |
| IBRD | 0x024 | Integer Baud Rate |
| FBRD | 0x028 | Fractional Baud Rate |
| LCR_H | 0x02C | Line Control Register |
| CR | 0x030 | Control Register |

For basic output, only writing to DR (0x09000000) is required.

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

### Using the Makefile (Recommended)

**Terminal 1:**
```bash
make debug
```

**Terminal 2:**
```bash
make gdb
```

This automatically starts QEMU with GDB server and connects GDB with symbols loaded.

### Manual Commands

**Terminal 1 - Start QEMU with GDB Server:**

```bash
qemu-system-aarch64 \
    -machine virt \
    -cpu cortex-a76 \
    -m 512M \
    -nographic \
    -kernel build/kernel/slmos.elf \
    -S \
    -gdb tcp::1234
```

| Option | Description |
|--------|-------------|
| `-S` | Pause CPU at startup (wait for debugger) |
| `-gdb tcp::1234` | Listen for GDB on port 1234 |

**Terminal 2 - Connect with GDB:**

```bash
gdb-multiarch build/kernel/slmos.elf
```

Then in GDB:

```gdb
target remote localhost:1234
break kernel_main
continue
```

**Note:** Use `gdb-multiarch` on Ubuntu instead of `aarch64-none-elf-gdb` (though both work if the ARM toolchain is installed).

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

Note: When using `-kernel`, QEMU loads the kernel at `0x40000000` and sets the PC there.

---

## Exiting QEMU

When running with `-nographic`:

- Press `Ctrl+A` then `X` to quit
- Press `Ctrl+A` then `C` to enter QEMU monitor
- In monitor, type `quit` to exit

---

## Common Issues

### "qemu-system-aarch64: command not found"

QEMU is not installed:

```bash
sudo apt install -y qemu-system-arm
```

### Kernel doesn't boot / no output

1. Verify kernel is built for correct address (0x40000000)
2. Check linker script entry point
3. Ensure UART code writes to 0x09000000
4. Try adding `-d in_asm` to see executed instructions

### "Could not initialize SDL"

Use `-nographic` flag to disable graphical output.

### Kernel hangs immediately

Add `-d int` to see interrupts/exceptions:

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a76 -m 512M -nographic \
    -kernel build/kernel/slmos.elf -d int
```

### View QEMU debug output

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a76 -m 512M -nographic \
    -kernel build/kernel/slmos.elf -d in_asm,cpu -D qemu.log
```

This logs executed instructions and CPU state to `qemu.log`.

---

## Quick Reference

### Using Makefile (Recommended)

```bash
make run      # Build and run
make debug    # Start with GDB server
make gdb      # Connect GDB (2nd terminal)
```

### Manual Commands

```bash
# Run
qemu-system-aarch64 -machine virt -cpu cortex-a76 -m 512M -nographic \
    -kernel build/kernel/slmos.elf

# Debug
qemu-system-aarch64 -machine virt -cpu cortex-a76 -m 512M -nographic \
    -kernel build/kernel/slmos.elf -S -gdb tcp::1234
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
