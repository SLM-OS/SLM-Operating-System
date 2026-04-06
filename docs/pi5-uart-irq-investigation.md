# Pi 5 UART Interrupt Investigation Status

**Date:** April 6, 2026
**Status:** BAR3 FIX IMPLEMENTED — MSI-X TLPs reach MIP0 (IRQ storm test confirms). Handler debugging needed.

## Summary

Interrupt-driven UART RX on Pi 5 requires the RP1 PL011 interrupt to traverse: PL011 → RP1 MSIX_CFG → PCIe MSI-X TLP → RC BAR1 inbound window → MIP0 → GIC SPI 153 (IRQ 185) → el1_irq_handler → ring buffer.

**Everything works except the PCIe BAR1 inbound match.** The RP1 generates MSI-X TLPs (proven by the TEST bit and no-IACK_EN flood tests), but the RC BAR1 doesn't capture them, causing PCIe completion timeouts.

## What Works (Verified)

| Component | Status | Evidence |
|-----------|--------|----------|
| GIC SPI delivery for IRQ 185 | ✅ | Software ISPENDR → HPPIR=185 |
| RP1 MSI-X engine fires TLPs | ✅ | MSIX_CFG TEST bit causes RP1 stall (completion timeout) |
| MSI-X table programmed | ✅ | Readback: addr=0x0F_FFFFF000, data=0x19, ctrl=0 |
| MSI-X Enable in config space | ✅ | Cap=0x803C0011 (Enable=1, FuncMask=0) |
| MSIX_CFG vector 25 | ✅ | MSIX_CFG[25]=0x9 (ENABLE + IACK_EN) |
| PL011 IMSC + interrupt active | ✅ | MIS=0x40 (RTIM), RP1 INTSTAT bit 25 set |
| RC BAR1 registers written | ✅ | Readback: {0x0F, 0xFFFFF01C} remap={0x10, 0x00130001} |
| MIP0 configured | ✅ | Vec 25 unmasked, edge-triggered, VPU masked |
| GIC IRQ 185 configured | ✅ | target=0x1, pri=0x40, enabled=1 |
| IRQ handler + ring buffer | ✅ | Tested on QEMU, ready for Pi 5 |
| Polling fallback | ✅ | Shell works normally via polling |

## What Doesn't Work

**MIP0 status register stays 0x0** — the MSI-X TLP from RP1 never reaches MIP0.

When MSIX_CFG fires (via TEST bit or no-IACK_EN flood), the TLP goes out on PCIe but gets a completion timeout, stalling the RP1 engine and hanging all UART output.

## Register Values (Verified by Readback)

### PCIe RC (at 0x1000120000)
```
BAR1_CONFIG_LO  (+0x402C) = 0xFFFFF01C  (addr=0xFFFFF000, size=0x1C=4KB)
BAR1_CONFIG_HI  (+0x4030) = 0x0000000F  (addr hi, full: 0x0F_FFFFF000)
UBUS_BAR1_REMAP (+0x40AC) = 0x00130001  (phys=0x00130000, access_enable=1)
UBUS_BAR1_REMAP_HI (+0x40B0) = 0x00000010  (phys hi, full: 0x10_00130000)
PCIE_STATUS     (+0x4068) = 0x0003E0B0  (DL_ACTIVE=1, link up)
GICD_CTLR (non-secure)    = 0x1         (EnableGrp1)
```

### MSI-X Table (at 0x1F00410000, BAR0)
```
Entry 25: addr_lo=0xFFFFF000, addr_hi=0x0000000F, data=0x19, ctrl=0x0
```

### RP1 INTC (at 0x1F00108000)
```
MSIX_CFG[25] = 0x9 (ENABLE=1, IACK_EN=1)
INTSTATL = 0x02000000 (bit 25 = UART0 active)
```

### MIP0 (at 0x1000130000)
```
INT_MASKL_HOST = ~(1<<25) (UART0 unmasked, all others masked)
INT_MASKH_HOST = 0xFFFFFFFF (all masked)
INT_CFGL_HOST  = 0xFFFFFFFF (edge-triggered)
INT_STATUSL_HOST = 0x0 (nothing received)
```

### GIC (at 0x107FFF9000)
```
IRQ 185: ISENABLER=1, IPRIORITYR=0x40, ITARGETSR=0x01
GICC_CTLR=0x1, PMR=0xF0, BPR=0x3, HPPIR=30 (timer)
PIDR2=0x2B (GIC-400, ArchRev=2)
```

## Key Findings

### GIC SPI Security Boundary (BCM2712-specific)
- SPIs 32-223: non-secure writable (priority writes take effect)
- SPIs 224+: secure-only (priority writes ignored, software trigger hangs)
- PCIe INTA at SPI 229 (IRQ 261) is in the **secure range** — unusable from EL1
- MIP0 range SPI 128-191 (IRQs 160-223) is in the **non-secure range** — works

### GIC Distributor Init
- Must NOT disable distributor (`GICD_CTLR=0`) during init — this resets TF-A's SPI config
- Use minimal init: only set ITARGETSR + IPRIORITYR without distributor disable

### PCIe Config Space Access
- EXT_CFG_DATA at RC+0x8000 (not 0x9004): reads RP1 config from EL1, writes hang
- EXT_CFG at RC+0x9004 with bus=1: reads hang (PCIe completion timeout for downstream access)
- RP1 appears at bus 0 via 0x8000 (vendor=0x1de4, device=0x0001)
- MSI-X capability at config offset 0xB0: 61 vectors, table in BAR0
- All RC register WRITES require EL2 (done in boot.S before EL2→EL1 drop)

### MSI-X Address
- Circle uses `0x0F_FFFFF000` = `{0x0F, 0xFFFFF000}` (36-bit address) — **WRONG for Pi 5**
- DT says `<0xff 0xfffff000>` = `{0xFF, 0xFFFFF000}` (40-bit address) — **CORRECT (confirmed by Linux register dump)**
- Linux BAR3_CONFIG_HI = 0xFF, matching the device tree value

### PCI SMC Service
- `SMC_PCI_VERSION` (0x84000130) returns -1 (SMC_UNK) — service not compiled into TF-A
- Pi 5 platform.mk has "PCI support via SMC calls disabled by default"

## Linux Register Dump (April 6, 2026)

Booted Raspberry Pi OS (2025-05-13, kernel 6.12.25) on the same Pi 5 and dumped PCIe RC registers via `/dev/mem` over SSH. **Root cause found: wrong BAR and wrong PCI address.**

### Linux PCIe RC Configuration

| Register | Linux | SLM-OS | Notes |
|----------|-------|--------|-------|
| BAR1_CONFIG_LO (+0x402C) | 0x00000007 | 0xFFFFF01C | Linux: 512KB window for RP1 BAR space |
| BAR1_CONFIG_HI (+0x4030) | 0x00000000 | 0x0000000F | Linux: BAR1 NOT used for MSI-X |
| UBUS_BAR1_REMAP (+0x40AC) | 0x00000001 | 0x00130001 | Linux: remaps to 0x1F_00000000 (RP1) |
| UBUS_BAR1_REMAP_HI (+0x40B0) | 0x0000001F | 0x00000010 | |
| **BAR3_CONFIG_LO (+0x403C)** | **0xFFFFF01C** | (not configured) | **Linux uses BAR3 for MSI-X!** |
| **BAR3_CONFIG_HI (+0x4040)** | **0x000000FF** | (not configured) | **PCI addr = 0xFF_FFFFF000** |
| **UBUS_BAR3_REMAP (+0x40BC)** | **0x00130001** | (not configured) | **→ MIP0 at 0x10_00130000** |
| **UBUS_BAR3_REMAP_HI (+0x40C0)** | **0x00000010** | (not configured) | |
| MISC_CTRL (+0x4008) | 0x00263480 | (not read) | SCB_ACCESS_EN=1, RCB_MPS=1 |
| PCIE_STATUS (+0x4068) | 0x0005E0B0 | 0x0003E0B0 | Different flags |

### Root Cause: Two Bugs

**Bug 1: Wrong BAR.** Linux routes MSI-X through **BAR3** (registers at +0x403C/4040/40BC/40C0), NOT BAR1. BAR1 is used for RP1 peripheral BAR space mapping. SLM-OS configured BAR1, which conflicted with the firmware's RP1 MMIO mapping and broke peripheral access.

**Bug 2: Wrong PCI address.** The MSI-X target address high byte should be `0xFF` (from device tree: `<0xff 0xfffff000>`), NOT `0x0F` (from Circle). Full MSI-X address: `0xFF_FFFFF000`. SLM-OS used Circle's `0x0F` value.

### Fix Required

In `boot.S` (EL2 PCIe RC setup):
1. Leave BAR1 alone (firmware uses it for RP1 MMIO)
2. Configure **BAR3** registers instead:
   - `RC_BAR3_CONFIG_LO` (+0x403C) = 0xFFFFF01C
   - `RC_BAR3_CONFIG_HI` (+0x4040) = 0x000000FF
   - `UBUS_BAR3_REMAP`   (+0x40BC) = 0x00130001
   - `UBUS_BAR3_REMAP_HI`(+0x40C0) = 0x00000010
3. Update MSI-X table entry 25 address to `{0xFF, 0xFFFFF000}`
4. Set MISC_CTRL to include SCB_ACCESS_EN and RCB_MPS (0x00263480)

### Also Note: MISC_CTRL

Linux MISC_CTRL = 0x00263480 has additional bits set compared to firmware default. Key bits:
- SCB_ACCESS_EN (bit 12) = 1 — enables SCB (system coherent bus) access
- CFG_READ_UR_MODE (bit 13) = 1 — unsupported request handling
- RCB_MPS (bit 17) = 1 — max payload size

These may also be needed for BAR3 inbound matching to work.

## Previous Hypotheses (Superseded)

## Next Step: Linux Register Dump (Option A)

Boot Linux on this Pi 5 and dump the following registers to compare with our bare-metal state:

```bash
# PCIe RC BAR1
devmem 0x100012402C  # BAR1_CONFIG_LO
devmem 0x1000124030  # BAR1_CONFIG_HI
devmem 0x10001240AC  # UBUS_BAR1_REMAP
devmem 0x10001240B0  # UBUS_BAR1_REMAP_HI

# PCIe RC MISC
devmem 0x1000124008  # MISC_CTRL
devmem 0x1000124068  # PCIE_STATUS

# MIP0
devmem 0x1000130040  # INT_MASKL_HOST
devmem 0x1000130020  # INT_CFGL_HOST
devmem 0x1000130080  # INT_STATUSL_HOST

# GIC for IRQ 185 (SPI 153)
devmem 0x107FFF9100 + (185/32)*4  # ISENABLER
devmem 0x107FFF9400 + (185/4)*4   # IPRIORITYR
devmem 0x107FFF9800 + (185/4)*4   # ITARGETSR
```

Also check if Linux has additional RC register writes by instrumenting the brcmstb driver or using ftrace on `writel` calls during RP1 probe.

## Files Modified for Interrupt Support

| File | Changes |
|------|---------|
| `kernel/drivers/uart_rp1.c` | Ring buffer, IRQ handler, `uart_irq_init`, MSIX_CFG, MSI-X table programming |
| `kernel/arch/arm64/boot.S` | EL2: MSI-X enable, RC BAR1→MIP routing, MIP0 init |
| `kernel/arch/arm64/exceptions.c` | `case UART_IRQ` in IRQ dispatch |
| `kernel/include/platform.h` | UART_IRQ=185, RP1_INTC, MIP0, PCIE_RC, MSIX defines |
| `kernel/include/uart.h` | `uart_irq_init`, `uart_irq_handler` declarations |
| `kernel/include/timer.h` | (unchanged — sleep functions from earlier) |
| `kernel/mm/vmm.c` | VMM mappings for PCIe RC, MIP0, RP1 INTC, RP1 BAR0 |
| `kernel/drivers/gic.c` | Minimal dist init (no disable), PIDR2 readback |
| `kernel/src/main.c` | `uart_irq_init()` call after SMP init |
| `kernel/src/shell_sys.c` | `cpu` command: UART mode, GIC/MIP/INTSTAT diagnostics |
| `CMakeLists.txt` | Renamed `uart_rp1_bitbang.c` → `uart_rp1.c` |

---

*Last updated: April 4, 2026*
