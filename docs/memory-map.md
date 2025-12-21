# Memory Map Documentation

This document describes the physical memory layout for each supported SLM-OS platform.

---

## Table of Contents

1. [Overview](#overview)
2. [QEMU virt Machine](#qemu-virt-machine)
3. [Jetson Orin Nano](#jetson-orin-nano)
4. [Raspberry Pi 5](#raspberry-pi-5)
5. [Memory Map Abstraction](#memory-map-abstraction)
6. [Physical Memory Allocator Design](#physical-memory-allocator-design)

---

## Overview

### Key Differences Summary

| Aspect | QEMU virt | Jetson Orin Nano | Raspberry Pi 5 |
|--------|-----------|------------------|----------------|
| DRAM base | 0x40000000 | 0x80000000 | 0x00000000 |
| DRAM size | Configurable | 4-8 GB | 4-8 GB |
| Peripheral base | 0x08000000 | 0x02000000 | 0x107C000000 |
| Layout complexity | Simple | Complex | Moderate |
| Reserved regions | None | Many | Some |
| GPU memory | None | Large carveout | Shared with CPU |
| Memory discovery | Hardcoded/DTB | Device tree | Device tree |

---

## QEMU virt Machine

### Memory Map

```
0x00000000 ┌─────────────────────────────────────┐
           │  Flash (128 MB)                     │
           │  Not used by SLM-OS                 │
0x08000000 ├─────────────────────────────────────┤
           │  GIC Distributor (64 KB)            │
0x08010000 ├─────────────────────────────────────┤
           │  GIC CPU Interface (64 KB)          │
0x08020000 ├─────────────────────────────────────┤
           │  GIC reserved (to 0x09000000)       │
0x09000000 ├─────────────────────────────────────┤
           │  PL011 UART (4 KB)                  │
0x09001000 ├─────────────────────────────────────┤
           │  Other peripherals (RTC, etc.)      │
0x0A000000 ├─────────────────────────────────────┤
           │  Virtio MMIO devices (16 MB)        │
0x10000000 ├─────────────────────────────────────┤
           │  Reserved / Unmapped                │
0x40000000 ├─────────────────────────────────────┤
           │  RAM (starts here)                  │
           │  ┌─────────────────────────────────┐│
           │  │ Kernel .text                    ││
           │  ├─────────────────────────────────┤│
           │  │ Kernel .rodata                  ││
           │  ├─────────────────────────────────┤│
           │  │ Kernel .data                    ││
           │  ├─────────────────────────────────┤│
           │  │ Kernel .bss                     ││
           │  ├─────────────────────────────────┤│
           │  │ Kernel stack                    ││
           │  ├─────────────────────────────────┤│
           │  │ Free memory (managed by PMM)    ││
           │  │                                 ││
           │  │                                 ││
           │  └─────────────────────────────────┘│
0x40000000 │                                     │
   + size  └─────────────────────────────────────┘
```

### Address Table

| Resource | Base Address | Size | Description |
|----------|--------------|------|-------------|
| Flash | 0x00000000 | 128 MB | Unused |
| GIC Distributor | 0x08000000 | 64 KB | Interrupt controller |
| GIC CPU | 0x08010000 | 64 KB | Per-CPU interface |
| UART0 | 0x09000000 | 4 KB | PL011 serial |
| RTC | 0x09010000 | 4 KB | Real-time clock |
| Virtio | 0x0A000000 | 16 MB | Virtual I/O |
| RAM | 0x40000000 | Configurable | Main memory |

### Reserved Regions

None. All RAM from 0x40000000 to end is usable after kernel image.

---

## Jetson Orin Nano

### Memory Map

```
0x00000000 ┌─────────────────────────────────────┐
           │  Boot ROM / Reserved                │
0x02000000 ├─────────────────────────────────────┤
           │  Peripheral region                  │
           │  - GPIO, I2C, SPI, PWM, etc.        │
0x03000000 ├─────────────────────────────────────┤
           │  Additional peripherals             │
           │  - UART at 0x03100000               │
0x03881000 ├─────────────────────────────────────┤
           │  GIC Distributor                    │
0x03882000 ├─────────────────────────────────────┤
           │  GIC CPU Interface                  │
0x04000000 ├─────────────────────────────────────┤
           │  More peripherals / reserved        │
           │                                     │
0x80000000 ├─────────────────────────────────────┤
           │  DRAM starts here                   │
           │  ┌─────────────────────────────────┐│
           │  │ Bootloader reserved (~16 MB)    ││
           │  ├─────────────────────────────────┤│
           │  │ TrustZone secure memory         ││
           │  ├─────────────────────────────────┤│
           │  │ Kernel image                    ││
           │  ├─────────────────────────────────┤│
           │  │ Usable RAM                      ││
           │  │                                 ││
           │  ├─────────────────────────────────┤│
           │  │ GPU carveout (1-2 GB)           ││
           │  │ - Model weights                 ││
           │  │ - Inference tensors             ││
           │  │ - CUDA memory                   ││
           │  ├─────────────────────────────────┤│
           │  │ Video encoder/decoder buffers   ││
           │  ├─────────────────────────────────┤│
           │  │ Camera ISP buffers              ││
           │  └─────────────────────────────────┘│
0x80000000 │                                     │
 + 4/8 GB  └─────────────────────────────────────┘
```

### Address Table

| Resource | Base Address | Size | Description |
|----------|--------------|------|-------------|
| UARTA | 0x03100000 | 4 KB | Debug console |
| UARTB | 0x03110000 | 4 KB | Secondary UART |
| GIC Distributor | 0x03881000 | 4 KB | Interrupt controller |
| GIC CPU | 0x03882000 | 4 KB | Per-CPU interface |
| DRAM | 0x80000000 | 4-8 GB | Main memory |

### Reserved Regions

| Region | Approximate Size | Purpose |
|--------|------------------|---------|
| Bootloader | ~16 MB | MB1, MB2, UEFI data |
| TrustZone | ~32 MB | Secure world memory |
| GPU carveout | 1-2 GB | CUDA, model inference |
| Video codec | ~128 MB | Encoder/decoder buffers |
| Camera ISP | ~64 MB | Image signal processor |
| Display | ~64 MB | Framebuffer, overlays |

**Note:** Exact sizes and locations come from the device tree. These are estimates based on typical Jetson configurations.

### GPU Memory Considerations

For SLM workloads, the GPU carveout is critical:

1. **Model weights** — Large (100s of MB to GB)
2. **Activation tensors** — Proportional to batch size
3. **CUDA runtime** — Fixed overhead (~100 MB)
4. **Workspace memory** — For intermediate computations

The CPU cannot directly access GPU carveout memory. Data must be:
- Allocated via NVIDIA's memory APIs
- Transferred between CPU and GPU explicitly
- Or use unified memory (with performance implications)

---

## Raspberry Pi 5

### Memory Map

```
0x00000000 ┌─────────────────────────────────────┐
           │  DRAM (starts at 0)                 │
           │  ┌─────────────────────────────────┐│
           │  │ VideoCore firmware (first 1MB)  ││
           │  ├─────────────────────────────────┤│
           │  │ ARM code / Kernel               ││
           │  ├─────────────────────────────────┤│
           │  │ Usable RAM                      ││
           │  │                                 ││
           │  ├─────────────────────────────────┤│
           │  │ VideoCore GPU memory            ││
           │  │ (configurable via config.txt)   ││
           │  └─────────────────────────────────┘│
0x00000000 │                                     │
 + 4/8 GB  ├─────────────────────────────────────┤
           │  (High memory, if >4GB model)       │
           │                                     │
0x107C000000├─────────────────────────────────────┤
           │  Peripheral base (BCM2712)          │
           │  - GPIO, I2C, SPI, etc.             │
0x107D001000├─────────────────────────────────────┤
           │  PL011 UART0                        │
0x107FFF9000├─────────────────────────────────────┤
           │  GIC-400 Distributor                │
0x107FFFA000├─────────────────────────────────────┤
           │  GIC-400 CPU Interface              │
           └─────────────────────────────────────┘
```

### Address Table

| Resource | Base Address | Size | Description |
|----------|--------------|------|-------------|
| DRAM | 0x00000000 | 4-8 GB | Main memory |
| Peripheral base | 0x107C000000 | — | BCM2712 peripherals |
| UART0 | 0x107D001000 | 4 KB | PL011 serial |
| GIC Distributor | 0x107FFF9000 | 4 KB | Interrupt controller |
| GIC CPU | 0x107FFFA000 | 4 KB | Per-CPU interface |

### Reserved Regions

| Region | Typical Size | Purpose |
|--------|--------------|---------|
| VideoCore firmware | 1 MB | GPU firmware at low memory |
| GPU memory | 64-512 MB | Configurable via config.txt |

### GPU Memory Sharing

Unlike Jetson, the Pi 5's VideoCore VII shares memory with the CPU:
- GPU memory size configured in `config.txt`
- Memory is CMA (Contiguous Memory Allocator) based
- CPU can access GPU buffers (with cache coherency considerations)

---

## Memory Map Abstraction

### Platform Descriptor Structure

```c
/* Memory region descriptor */
struct mem_region {
    uintptr_t base;
    size_t    size;
    uint32_t  flags;      /* See MEM_* flags below */
    const char *name;
};

/* Region flags */
#define MEM_USABLE      (1 << 0)    /* Available for allocation */
#define MEM_RESERVED    (1 << 1)    /* Firmware/bootloader reserved */
#define MEM_MMIO        (1 << 2)    /* Memory-mapped I/O */
#define MEM_GPU         (1 << 3)    /* GPU carveout */
#define MEM_DMA         (1 << 4)    /* DMA-capable region */
#define MEM_KERNEL      (1 << 5)    /* Kernel image location */

/* Platform memory configuration */
struct platform_memory {
    /* Main DRAM */
    uintptr_t dram_base;
    size_t    dram_size;

    /* Memory regions (from DTB or hardcoded) */
    struct mem_region regions[32];
    int num_regions;

    /* Convenience pointers */
    uintptr_t kernel_start;
    uintptr_t kernel_end;
    uintptr_t heap_start;       /* First free page after kernel */
};

extern struct platform_memory mem_info;
```

### Initialization Flow

```
1. Early boot (assembly)
   └── Set up minimal stack

2. Platform init (C)
   ├── Parse DTB (if available)
   │   └── Extract memory regions
   └── Or use hardcoded values

3. Memory manager init
   ├── Find usable regions
   ├── Mark kernel pages as used
   └── Initialize page allocator
```

---

## Physical Memory Allocator Design

### Considerations for Multi-Platform

1. **Variable DRAM base** — Can't assume 0x40000000
2. **Non-contiguous usable regions** — Holes for reserved areas
3. **Large address space** — 64-bit addresses, potential >4GB RAM
4. **Alignment requirements** — GPU buffers may need specific alignment

### Proposed Interface

```c
/* Initialize PMM from platform memory info */
void pmm_init(const struct platform_memory *mem);

/* Allocate physical pages */
void *pmm_alloc_page(void);                     /* Single 4KB page */
void *pmm_alloc_pages(size_t count);            /* Contiguous pages */
void *pmm_alloc_aligned(size_t count, size_t align);  /* Aligned allocation */

/* Free physical pages */
void pmm_free_page(void *page);
void pmm_free_pages(void *page, size_t count);

/* Query */
size_t pmm_get_free_pages(void);
size_t pmm_get_total_pages(void);
void pmm_dump_stats(void);
```

### Implementation Strategy

For Month 1 (QEMU):
- Simple bitmap allocator
- Single contiguous region
- Hardcoded memory size

For Month 2+ (Jetson/Pi):
- Multiple region support
- Parse device tree for memory map
- Skip reserved regions in bitmap

---

## References

- [QEMU virt machine source](https://github.com/qemu/qemu/blob/master/hw/arm/virt.c)
- [Jetson Orin Technical Reference Manual](https://developer.nvidia.com/embedded/downloads)
- [BCM2712 Peripherals](https://datasheets.raspberrypi.com/)
- [ARM Memory Model](https://developer.arm.com/documentation/den0024/latest/)

---

*Last updated: December 2025*
