# Platform Abstraction Strategy

This document defines how SLM-OS handles platform-specific differences to support multiple hardware targets from a single codebase.

---

## Table of Contents

1. [Supported Platforms](#supported-platforms)
2. [Platform-Specific Components](#platform-specific-components)
3. [Abstraction Strategies](#abstraction-strategies)
4. [Implementation Approach](#implementation-approach)
5. [Directory Structure](#directory-structure)
6. [Adding a New Platform](#adding-a-new-platform)

---

## Supported Platforms

| Platform | Status | SoC | CPU | RAM | GPU |
|----------|--------|-----|-----|-----|-----|
| QEMU virt | Primary dev | Virtual | Cortex-A76 (emulated) | Configurable | None |
| Jetson Orin Nano | Target | Tegra234 | Cortex-A78AE | 4-8 GB | Ampere |
| Raspberry Pi 5 | Stretch goal | BCM2712 | Cortex-A76 | 4-8 GB | VideoCore VII |

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
| Type | PL011 | Tegra186 (NS16550) | PL011 |
| Base address | 0x09000000 | 0x03100000 | 0x107D001000 |
| Register layout | ARM standard | 8250-compatible | ARM standard |
| Driver | uart_pl011.c | uart_tegra.c | uart_pl011.c |

#### Interrupt Controller

| Aspect | QEMU virt | Jetson Orin Nano | Raspberry Pi 5 |
|--------|-----------|------------------|----------------|
| Type | GICv2 | GICv2 | GIC-400 (v2) |
| Distributor | 0x08000000 | 0x03881000 | 0x107FFF9000 |
| CPU interface | 0x08010000 | 0x03882000 | 0x107FFFA000 |

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
│   │   └── gic_v2.c        # GICv2 (all platforms)
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

*Last updated: December 2025*
