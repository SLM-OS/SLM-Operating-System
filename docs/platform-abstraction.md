# Platform Abstraction Strategy

This document defines how SLM-OS handles platform-specific differences to support multiple hardware targets from a single codebase.

---

## Table of Contents

1. [Supported Platforms](#supported-platforms)
2. [Platform-Specific Components](#platform-specific-components)
3. [Abstraction Strategies](#abstraction-strategies)
4. [Device Tree Support](#device-tree-support)
5. [Implementation Approach](#implementation-approach)
6. [Directory Structure](#directory-structure)
7. [Adding a New Platform](#adding-a-new-platform)

---

## Supported Platforms

| Platform | Status | SoC | CPU | RAM | GPU |
|----------|--------|-----|-----|-----|-----|
| QEMU virt | Primary dev | Virtual | Cortex-A76 (emulated) | Configurable | None |
| Jetson Orin Nano | Target | Tegra234 | Cortex-A78AE | 4-8 GB | Ampere |
| Raspberry Pi 5 | Working | BCM2712 | Cortex-A76 | 4-8 GB | VideoCore VII |

---

## Platform-Specific Components

### Component Classification

| Component | Variation Level | Abstraction Strategy |
|-----------|-----------------|----------------------|
| Memory map | High | Platform descriptor |
| UART | Medium | Compile-time driver selection |
| Interrupt controller | Low | Compile-time (GIC variants similar) |
| Timer | Low | Compile-time (ARM generic timer) |
| Boot sequence | Medium | Platform-specific boot code |
| GPU | Very High | Runtime HAL (future) |

### Detailed Differences

#### Memory Map

| Aspect | QEMU virt | Jetson Orin Nano | Raspberry Pi 5 |
|--------|-----------|------------------|----------------|
| DRAM base | 0x40000000 | 0x80000000 | 0x00000000 |
| DRAM size | Configurable | 4-8 GB | 4-8 GB |
| Peripheral base | 0x08000000 | 0x02000000 | 0x107C000000 |
| GPU carveout | None | 1-2 GB | Shared |
| Reserved regions | Minimal | Many (firmware, TZ) | Some |

#### UART

| Aspect | QEMU virt | Jetson Orin Nano | Raspberry Pi 5 |
|--------|-----------|------------------|----------------|
| Type | PL011 | Tegra186 (NS16550) | RP1 PL011 |
| Base address | 0x09000000 | 0x03100000 | 0x1F00030000 (via RP1) |
| Register layout | ARM standard | 8250-compatible | ARM standard (no flag reads) |
| Driver | uart_pl011.c | uart_tegra.c | uart_rp1_bitbang.c |
| Notes | - | BPMP clock enable | Requires firmware PCIe init |

#### Interrupt Controller

| Aspect | QEMU virt | Jetson Orin Nano | Raspberry Pi 5 |
|--------|-----------|------------------|----------------|
| Type | GICv2 | GICv3 | GIC-400 (v2) |
| Distributor | 0x08000000 | 0x0F400000 (GICD) | 0x107FFF9000 |
| CPU interface | 0x08010000 | 0x0F440000 (GICR) | 0x107FFFA000 |

---

## Abstraction Strategies

### Strategy 1: Compile-Time Selection

Best for: Components with few variants, hot code paths

```c
// platform.h
#if defined(PLATFORM_QEMU_VIRT) || defined(PLATFORM_RPI5)
    #define UART_TYPE_PL011
#elif defined(PLATFORM_JETSON)
    #define UART_TYPE_NS16550
#endif

// CMakeLists.txt selects appropriate source file
```

**Pros:**
- Zero runtime overhead
- Compiler can optimize fully
- Simple implementation

**Cons:**
- Separate binary per platform
- Can't switch at runtime

**Use for:** UART, timer, interrupt controller, boot code

### Strategy 2: Platform Descriptor

Best for: Configuration data, memory maps

```c
struct platform_info {
    const char *name;

    /* Memory layout */
    uintptr_t dram_base;
    size_t    dram_size;
    uintptr_t kernel_base;

    /* Peripherals */
    uintptr_t uart_base;
    uintptr_t gic_dist_base;
    uintptr_t gic_cpu_base;

    /* Reserved regions */
    struct mem_region reserved[16];
    int num_reserved;
};

extern const struct platform_info *platform;
```

**Pros:**
- Clean separation of data
- Easy to populate from device tree later
- Single binary can support multiple platforms (with DTB)

**Cons:**
- Slight runtime overhead for lookups
- Must be initialized early in boot

**Use for:** Memory map, peripheral addresses, platform identification

### Strategy 3: Hardware Abstraction Layer (HAL)

Best for: Complex subsystems with vastly different implementations

```c
struct gpu_ops {
    int  (*init)(void);
    void (*shutdown)(void);
    int  (*alloc_memory)(size_t size, void **ptr);
    int  (*submit_inference)(struct model *m, struct tensor *in, struct tensor *out);
};

extern const struct gpu_ops *gpu;
```

**Pros:**
- Maximum flexibility
- Can swap implementations at runtime
- Clean API boundaries

**Cons:**
- Function pointer overhead
- More complex to implement
- Harder to inline/optimize

**Use for:** GPU interface (future), possibly storage drivers

---

## Device Tree Support

SLM-OS includes a minimal Device Tree Blob (DTB) parser that enables runtime hardware discovery. This bridges the gap between compile-time platform selection and fully dynamic configuration.

### Overview

The DTB parser (`kernel/src/dtb.c`) extracts hardware configuration from a Flattened Device Tree passed by the bootloader:

| Property | Source Node | Usage |
|----------|-------------|-------|
| RAM base/size | `/memory@*` | PMM initialization |
| UART base | `/pl011@*`, `/uart@*`, `/serial@*` | Console output |
| UART IRQ | `interrupts` property | Interrupt routing |
| GIC distributor | `/intc@*`, `/gic@*` | Interrupt controller |
| GIC CPU interface | `reg` property (second entry) | Per-CPU interrupt handling |
| CPU count | `/cpus/cpu@*` node count | SMP initialization |
| Timer IRQ | `/timer` `interrupts` property | Scheduler tick |

### Boot Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│  Bootloader/QEMU                                                    │
│  └── Passes DTB address in x0 register                              │
├─────────────────────────────────────────────────────────────────────┤
│  boot.S                                                             │
│  └── Preserves x0 → x19, passes to kernel_main(dtb)                 │
├─────────────────────────────────────────────────────────────────────┤
│  kernel_main()                                                      │
│  └── Calls dtb_parse(dtb, &fdt_info)                                │
│      ├── Success: Uses parsed values                                │
│      └── Failure: Falls back to platform.h defaults                 │
└─────────────────────────────────────────────────────────────────────┘
```

### API

```c
/* Parse DTB and extract platform info */
int dtb_parse(const void *dtb, fdt_info_t *info);

/* Get globally parsed info (after boot) */
const fdt_info_t *dtb_get_info(void);

/* Validate DTB header only */
int dtb_validate(const void *dtb);

/* Print parsed info (debug) */
void dtb_print_info(const fdt_info_t *info);
```

### Fallback Behavior

If DTB parsing fails (NULL pointer, invalid magic, unsupported version), the system falls back to compile-time defaults from `platform.h`. This ensures the kernel boots reliably even without a valid DTB.

Current status:
- **QEMU ELF boot**: DTB pointer is NULL (QEMU behavior with `-kernel` and ELF files)
- **Real hardware**: Bootloaders (U-Boot, UEFI) pass valid DTB pointer
- **Fallback**: `platform.h` values used when parsing fails

### Shell Command

The `dtb` shell command displays the current platform configuration:

```
slmos> dtb
Device Tree Information:

  Status:       using defaults

  Memory:
    Base:       0x40000000
    Size:       128 MB

  UART:
    Base:       0x9000000
    IRQ:        33
  ...
```

### Future Work

Once hardware testing confirms DTB parsing works on real hardware:
1. Remove hardcoded values from `platform.h`
2. Make `fdt_info_t` the single source of truth for platform configuration
3. Add more property extraction (I2C, SPI, GPIO for Jetson peripherals)

---

## Implementation Approach

### Current (Month 1-2): Simple Compile-Time

```
kernel/
├── include/
│   └── platform.h          # Platform detection and basic defines
├── drivers/
│   ├── uart_pl011.c        # PL011 driver
│   └── uart_tegra.c        # Tegra driver (future)
└── platform/
    ├── qemu_virt.c         # QEMU platform descriptor
    ├── jetson.c            # Jetson platform descriptor (future)
    └── rpi5.c              # RPi5 platform descriptor (future)
```

Build system selects platform:
```bash
make PLATFORM=qemu_virt    # Default
make PLATFORM=jetson       # Jetson build
make PLATFORM=rpi5         # Raspberry Pi 5 build
```

### Future (Month 3+): Hybrid Approach

1. **Platform descriptor** for memory map and addresses
2. **Compile-time selection** for core drivers (UART, GIC, timer)
3. **HAL with function pointers** for GPU subsystem

---

## Directory Structure

```
kernel/
├── include/
│   ├── platform.h          # Platform detection, common defines
│   ├── platform_types.h    # struct platform_info, etc.
│   ├── uart.h              # UART interface
│   ├── gic.h               # GIC interface
│   └── gpu.h               # GPU HAL interface (future)
│
├── platform/
│   ├── platform.c          # Platform initialization
│   ├── qemu_virt.c         # QEMU virt platform descriptor
│   ├── jetson.c            # Jetson Orin Nano descriptor
│   └── rpi5.c              # Raspberry Pi 5 descriptor
│
├── drivers/
│   ├── uart/
│   │   ├── uart_pl011.c    # PL011 (QEMU, RPi5)
│   │   └── uart_tegra.c    # Tegra186 (Jetson)
│   ├── gic/
│   │   └── gic.c            # GIC driver (GICv2 + GICv3)
│   └── timer/
│       └── arm_timer.c     # ARM generic timer
│
└── gpu/                    # Future: GPU HAL
    ├── gpu_hal.c           # Common GPU interface
    ├── gpu_jetson.c        # Jetson GPU driver
    └── gpu_stub.c          # No-op for QEMU
```

---

## Adding a New Platform

### Step 1: Create Platform Descriptor

```c
// kernel/platform/new_platform.c
#include "platform_types.h"

const struct platform_info new_platform = {
    .name = "New Platform",
    .dram_base = 0x80000000,
    .dram_size = 4UL * 1024 * 1024 * 1024,  // 4 GB
    .uart_base = 0x12340000,
    // ... etc
};
```

### Step 2: Add Platform to platform.h

```c
#elif defined(PLATFORM_NEW)
    #define UART_TYPE_xxx
    #define GIC_VERSION 2
    // ...
```

### Step 3: Add Driver (if needed)

If the platform uses a new UART type, implement:
- `kernel/drivers/uart/uart_new.c`

### Step 4: Update Build System

```cmake
# CMakeLists.txt
if(PLATFORM STREQUAL "NEW")
    set(PLATFORM_SOURCES kernel/platform/new_platform.c)
    set(UART_DRIVER kernel/drivers/uart/uart_new.c)
endif()
```

### Step 5: Update Documentation

- Add to `docs/platform-abstraction.md` (this file)
- Create `docs/platforms/new_platform.md` with specifics

---

## Performance Considerations

### Hot Path Components

These are called frequently and should use compile-time selection:
- UART putc/getc (printf output)
- Interrupt handling
- Timer tick
- Memory allocation (page allocator internals)

### Cold Path Components

These are called rarely and can use indirection:
- Platform initialization
- GPU setup
- Memory map discovery
- Device enumeration

### Guidelines

1. **Default to compile-time selection** unless runtime flexibility is required
2. **Use platform descriptor** for configuration data that doesn't affect hot paths
3. **Reserve HAL/function pointers** for subsystems with fundamentally different implementations (GPU)
4. **Profile before optimizing** — function pointer overhead is often negligible

---

## Decision Record

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Primary abstraction | Compile-time | Zero overhead, platforms are fixed at flash time |
| Memory map storage | Platform descriptor struct | Clean, extensible, DTB-ready |
| UART abstraction | Compile-time driver selection | Only 2-3 variants, hot path |
| GPU abstraction | HAL with function pointers | Vastly different APIs, complex subsystem |
| Build selection | CMake + Make variable | Standard tooling, easy CI integration |

---

## QEMU vs Jetson: Practical Differences

This section documents runtime differences discovered during development. These affect how the kernel boots and operates on each platform.

### Boot Process

| Aspect | QEMU | Jetson Orin Nano |
|--------|------|------------------|
| Boot method | Direct `-kernel` load | kexec from Linux or UEFI |
| Kernel format | ELF (`.elf`) or Image (`.bin`) | Image format (PE/COFF header) |
| DTB source | QEMU-generated | Bootloader-provided |
| DTB in x0 | Only with `.bin` format | Yes (kexec/UEFI pass it) |
| Load address | ELF: fixed at 0x40000000<br>BIN: 2MB-aligned (e.g., 0x40200000) | Runtime-determined by bootloader |

**Key finding:** QEMU's ELF loader (`-kernel foo.elf`) does *not* pass DTB address in x0. Use binary format for DTB testing, or accept platform.h defaults for ELF testing.

### Boot Header

The kernel binary includes a PE/COFF header for UEFI compatibility:

```
Offset 0x00: "MZ" - PE/COFF magic (encoded as ARM64 ccmp instruction)
Offset 0x04: Branch to real_start
Offset 0x38: "ARM\x64" - ARM64 Image magic
Offset 0x3C: PE header offset
```

The `ccmp x18, #0, #0xd, pl` instruction encodes to bytes `4D 5A 40 FA`, providing both:
- Valid ARM64 instruction (harmless conditional compare)
- "MZ" signature at offset 0 for PE/COFF recognition

### Serial Console

| Aspect | QEMU | Jetson Orin Nano |
|--------|------|------------------|
| Primary UART | PL011 at 0x09000000 | UARTA (NS16550) at 0x03100000 |
| Debug port | `-serial stdio` | USB-C (TCU) or 40-pin header |
| Clock init | Not needed | BPMP clock enable required |
| IRQ | 33 (SPI 1) | Configured via BPMP |

**Jetson-specific:** UARTA requires BPMP IPC to enable clock before use. The USB-C debug port uses TCU (Tegra Combined UART), which requires SPE firmware cooperation and doesn't work after kexec. See `docs/jetson-tcu.md`.

### Memory Map

| Region | QEMU virt | Jetson Orin Nano |
|--------|-----------|------------------|
| RAM base | 0x40000000 | 0x80000000 |
| RAM size | Configurable (1GB default) | 4-8 GB (minus carveouts) |
| GIC distributor | 0x08000000 | 0x0F400000 (GICD) |
| GIC redistributor | 0x08010000 | 0x0F440000 (GICR) |
| UART | 0x09000000 (PL011) | 0x03100000 (UARTA) |

### Interrupt Controller

| Aspect | QEMU | Jetson |
|--------|------|--------|
| GIC version | GICv2 | GICv3 |
| Timer IRQ | 30 (virtual timer PPI) | 30 |
| UART IRQ | 33 (SPI 1) | Platform-specific |

QEMU uses GICv2 while Jetson uses GICv3. The GIC driver handles both versions via platform-specific initialization.

### Clock and Power Management

| Aspect | QEMU | Jetson |
|--------|------|--------|
| Clocks | Always on | BPMP-managed |
| Power domains | Not modeled | Must enable via BPMP |
| BPMP IPC | Not needed | Required for UART, GPIO |

Jetson peripherals are clock-gated by default. The BPMP (Boot and Power Management Processor) controls clocks via IPC messages. See `kernel/drivers/bpmp.c`.

### Development Implications

1. **Primary development:** Use QEMU with ELF format for fast iteration
2. **DTB testing:** Use binary format (`slmos.bin`) to get DTB from QEMU
3. **Hardware testing:** Always use binary format (required for kexec/UEFI)
4. **Platform defaults:** `platform.h` values match QEMU virt machine
5. **Jetson bring-up:** Requires serial console via 40-pin header UART

### Makefile Targets

```bash
make run          # Run ELF in QEMU (fast, no DTB)
make test         # Test ELF in QEMU (uses platform defaults)
make PLATFORM=JETSON_ORIN_NANO kernel  # Build for Jetson
```

---

## Raspberry Pi 5: Platform Notes

This section documents Pi 5-specific requirements discovered during bring-up.

### Boot Configuration

Pi 5 requires specific `config.txt` settings for bare-metal UART to work:

```
arm_64bit=1
kernel_address=0x80000
kernel=kernel_2712.img

# Critical: Firmware pre-initializes PCIe/RP1 for bare-metal
pciex4_reset=0
uart_2ndstage=1
os_check=0
```

Without `pciex4_reset=0` and `uart_2ndstage=1`, the firmware resets PCIe after loading the kernel, making RP1 peripherals inaccessible.

### RP1 Southbridge

The Pi 5's GPIO and UART are on the RP1 chip, connected via PCIe:

| Address Range | Contents |
|---------------|----------|
| 0x1F00000000+ | RP1 peripherals (maps to RP1's internal 0x40000000) |
| 0x1F000D0000 | GPIO IO (pin control) |
| 0x1F000E0000 | GPIO RIO (direct GPIO) |
| 0x1F000F0000 | GPIO PADS |
| 0x1F00030000 | UART0 (PL011) |

### Known Limitations

1. **UART flag register:** Reading UART_FR causes data abort. The driver uses blind writes with delay.

2. **Spinlocks before MMU enable:** ARM exclusive monitor operations (LDAXR/STXR) require cacheable memory, which is not available before MMU enable. A runtime flag `spinlock_hw_enabled` (declared in `spinlock.h`, set by `vmm_init()` after MMU enable) gates real atomic locking. Pre-MMU, spinlocks are barrier-only; post-MMU, they use full ldaxr/stxr sequences. SMP is live.

### Serial Console

- **Pins:** GPIO14 (TXD, pin 8), GPIO15 (RXD, pin 10), GND (pin 6)
- **Baud:** 115200, 8N1
- **GPIO FUNCSEL:** 4 (UART function)

### Driver

The driver `uart_rp1.c` uses hardware PL011:
- TX: PL011 flag register polling (TXFF bit)
- RX: PL011 flag register polling (RXFE bit) with GPIO pad config (OD=1, FUNCSEL 5→4)

See `docs/pi5-baremetal-status.md` for full details.

---

*Last updated: January 2026*
