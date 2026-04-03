# ARM64 MMU Architecture

This document describes the Memory Management Unit (MMU) architecture for SLM-OS on ARM64 (AArch64).

---

## Overview

The MMU translates virtual addresses (VA) to physical addresses (PA), enabling:

- **Memory protection**: Prevent tasks from accessing each other's memory
- **Virtual address spaces**: Each task sees its own address space
- **Memory attributes**: Control caching, access permissions, device vs normal memory
- **Demand paging**: Map memory only when needed (future)

SLM-OS runs at EL1 (kernel mode) and uses the ARMv8-A virtual memory system (VMSAv8-64).

---

## Key System Registers

| Register | Purpose |
|----------|---------|
| **SCTLR_EL1** | System control - bit 0 (M) enables MMU |
| **TCR_EL1** | Translation control - granule size, address space size, cacheability |
| **TTBR0_EL1** | Translation table base for lower VA range (user space) |
| **TTBR1_EL1** | Translation table base for upper VA range (kernel space) |
| **MAIR_EL1** | Memory attribute indirection register - defines 8 memory type slots |

---

## Address Space Split (TTBR0 vs TTBR1)

ARM64 provides two translation table base registers, allowing a clean split between user and kernel address spaces:

```
┌─────────────────────────────────────────────────────────────────────┐
│  Virtual Address Space (48-bit example)                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  0xFFFF_FFFF_FFFF_FFFF  ┐                                           │
│          ...            │  TTBR1_EL1 (Kernel)                       │
│  0xFFFF_0000_0000_0000  ┘  VA[63:48] = all 1s                       │
│                                                                     │
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─     (Fault zone - mixed bits)               │
│                                                                     │
│  0x0000_FFFF_FFFF_FFFF  ┐                                           │
│          ...            │  TTBR0_EL1 (User)                         │
│  0x0000_0000_0000_0000  ┘  VA[63:48] = all 0s                       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Selection logic:**
- If VA[63:48] are all zeros → use TTBR0_EL1
- If VA[63:48] are all ones → use TTBR1_EL1
- Mixed bits → translation fault

**Benefits:**
- Kernel mappings exist in TTBR1, shared by all processes
- User mappings in TTBR0 are per-process
- Context switch only needs to update TTBR0 (fast)

**SLM-OS approach:**
- Phase 2: Use TTBR1 only (kernel-only, no user space yet)
- Future: Add TTBR0 for user space when needed

---

## Translation Table Format

### Granule Size Options

ARM64 supports three page sizes (granules):

| Granule | Page Size | Entries per Table | Table Size |
|---------|-----------|-------------------|------------|
| 4KB | 4KB | 512 (9 bits) | 4KB |
| 16KB | 16KB | 2048 (11 bits) | 16KB |
| 64KB | 64KB | 8192 (13 bits) | 64KB |

**SLM-OS choice: 4KB granule**
- Most common, well-documented
- Allows 2MB block mappings at L2 (good for model memory)
- Compatible with Linux conventions

### Translation Levels (4KB Granule)

With 4KB granule and 48-bit virtual addresses, translation uses 4 levels:

```
48-bit Virtual Address:
┌────────┬────────┬────────┬────────┬────────────┐
│ [47:39]│ [38:30]│ [29:21]│ [20:12]│   [11:0]   │
│ L0 idx │ L1 idx │ L2 idx │ L3 idx │   Offset   │
│ 9 bits │ 9 bits │ 9 bits │ 9 bits │  12 bits   │
└────────┴────────┴────────┴────────┴────────────┘
    │        │        │        │         │
    │        │        │        │         └─► Byte within 4KB page
    │        │        │        └───────────► 512 entries × 4KB = 2MB
    │        │        └────────────────────► 512 entries × 2MB = 1GB
    │        └─────────────────────────────► 512 entries × 1GB = 512GB
    └──────────────────────────────────────► 512 entries × 512GB = 256TB
```

### Reduced Address Space (39-bit VA)

For simpler implementation, SLM-OS uses 39-bit virtual addresses:
- **3 levels** instead of 4 (skip L0)
- **512GB** address space (plenty for embedded)
- Set TCR_EL1.T1SZ = 25 (64 - 39 = 25)

```
39-bit Virtual Address:
┌────────┬────────┬────────┬────────────┐
│ [38:30]│ [29:21]│ [20:12]│   [11:0]   │
│ L1 idx │ L2 idx │ L3 idx │   Offset   │
│ 9 bits │ 9 bits │ 9 bits │  12 bits   │
└────────┴────────┴────────┴────────────┘
```

### Descriptor Formats

Each translation table entry is 64 bits. The format depends on the level and type:

#### Table Descriptor (L1/L2 pointing to next level)

```
┌────────────────────────────────────────────────────────────────────┐
│ 63    59│58  55│54  52│51  48│47                    12│11   2│1  0│
├─────────┼──────┼──────┼──────┼────────────────────────┼──────┼────┤
│  Attrs  │ Res0 │ UXN  │ Res0 │   Next Table Address   │ Ign  │ 11 │
│         │      │ PXN  │      │   (bits [47:12])       │      │    │
└────────────────────────────────────────────────────────────────────┘
Bits [1:0] = 0b11 indicates "table descriptor"
```

#### Block Descriptor (L1 = 1GB, L2 = 2MB)

```
┌────────────────────────────────────────────────────────────────────┐
│ 63    59│58  55│54  52│51  48│47                    n│n-1  12│1  0│
├─────────┼──────┼──────┼──────┼────────────────────────┼───────┼────┤
│  Upper  │ Res0 │ Cont │ Res0 │   Output Address       │ Lower │ 01 │
│  Attrs  │      │ nG   │      │   (aligned to block)   │ Attrs │    │
└────────────────────────────────────────────────────────────────────┘
Bits [1:0] = 0b01 indicates "block descriptor"
n = 30 for L1 (1GB), n = 21 for L2 (2MB)
```

#### Page Descriptor (L3 = 4KB)

```
┌────────────────────────────────────────────────────────────────────┐
│ 63    59│58  55│54  52│51  48│47                    12│11   2│1  0│
├─────────┼──────┼──────┼──────┼────────────────────────┼──────┼────┤
│  Upper  │ Res0 │ Cont │ Res0 │   Output Address       │ Lower│ 11 │
│  Attrs  │      │ nG   │      │   (4KB aligned)        │ Attrs│    │
└────────────────────────────────────────────────────────────────────┘
Bits [1:0] = 0b11 indicates "page descriptor" at L3
```

#### Invalid Descriptor

```
Bits [1:0] = 0b00 indicates "invalid" (causes translation fault)
```

### Descriptor Attributes

**Lower attributes (bits [11:2]):**

| Bits | Field | Description |
|------|-------|-------------|
| [11:10] | Reserved | |
| [9:8] | SH | Shareability (00=Non, 10=Outer, 11=Inner) |
| [7:6] | AP | Access Permission (see below) |
| [5] | NS | Non-secure (ignored at EL1) |
| [4:2] | AttrIndx | Index into MAIR_EL1 (0-7) |

**Access Permissions (AP[7:6]):**

| AP | EL1 Access | EL0 Access |
|----|------------|------------|
| 00 | Read/Write | None |
| 01 | Read/Write | Read/Write |
| 10 | Read-only | None |
| 11 | Read-only | Read-only |

**Upper attributes (bits [63:52]):**

| Bits | Field | Description |
|------|-------|-------------|
| [54] | UXN | Unprivileged Execute Never |
| [53] | PXN | Privileged Execute Never |
| [52] | Contiguous | Hint for TLB (group of entries) |
| [58:55] | Reserved | |
| [63:59] | Software | Available for OS use |

---

## Memory Attributes (MAIR_EL1)

MAIR_EL1 defines up to 8 memory attribute configurations. Page table entries reference these by index (AttrIndx field).

### Attribute Encoding

Each MAIR slot is 8 bits with format depending on memory type:

**Device Memory (bits [7:4] = 0b0000):**

| Encoding | Type | Description |
|----------|------|-------------|
| 0x00 | nGnRnE | No Gathering, no Reordering, no Early write ack |
| 0x04 | nGnRE | No Gathering, no Reordering, Early write ack |
| 0x08 | nGRE | No Gathering, Reordering, Early write ack |
| 0x0C | GRE | Gathering, Reordering, Early write ack |

**Normal Memory (bits [7:4] != 0b0000):**

Format: `0bOOOOIIII` where OOOO = outer, IIII = inner cache policy

| Value | Meaning |
|-------|---------|
| 0b0100 | Non-cacheable |
| 0b1111 | Write-back, read/write allocate |

### SLM-OS MAIR Configuration

```c
/*
 * MAIR_EL1 configuration for SLM-OS
 *
 * Index 0: Device-nGnRnE (UART, GIC - strictest ordering)
 * Index 1: Device-nGnRE  (Most MMIO - slightly relaxed)
 * Index 2: Normal Non-cacheable (DMA buffers, GPU shared memory)
 * Index 3: Normal Write-back (RAM - code, data, stacks)
 */
#define MAIR_ATTR_DEVICE_nGnRnE     0x00
#define MAIR_ATTR_DEVICE_nGnRE      0x04
#define MAIR_ATTR_NORMAL_NC         0x44
#define MAIR_ATTR_NORMAL_WB         0xFF

#define MAIR_INDEX_DEVICE_nGnRnE    0
#define MAIR_INDEX_DEVICE_nGnRE     1
#define MAIR_INDEX_NORMAL_NC        2
#define MAIR_INDEX_NORMAL_WB        3

#define MAIR_EL1_VALUE  ((MAIR_ATTR_DEVICE_nGnRnE << (8 * 0)) | \
                         (MAIR_ATTR_DEVICE_nGnRE  << (8 * 1)) | \
                         (MAIR_ATTR_NORMAL_NC     << (8 * 2)) | \
                         (MAIR_ATTR_NORMAL_WB     << (8 * 3)))
```

### When to Use Each Attribute

| Attribute | Use Case |
|-----------|----------|
| Device-nGnRnE | UART (side effects on read), GIC (interrupt ack) |
| Device-nGnRE | General MMIO where ordering matters |
| Normal NC | Buffers shared with GPU/DMA, must be cache-coherent |
| Normal WB | All regular RAM (kernel, stacks, model weights) |

---

## TCR_EL1 Configuration

The Translation Control Register configures the translation system.

### Key Fields

| Field | Bits | Description |
|-------|------|-------------|
| T0SZ | [5:0] | Size of TTBR0 region: 2^(64-T0SZ) bytes |
| T1SZ | [21:16] | Size of TTBR1 region: 2^(64-T1SZ) bytes |
| TG0 | [15:14] | TTBR0 granule: 00=4KB, 01=64KB, 10=16KB |
| TG1 | [31:30] | TTBR1 granule: 01=16KB, 10=4KB, 11=64KB |
| IRGN0/1 | Various | Inner cacheability for table walks |
| ORGN0/1 | Various | Outer cacheability for table walks |
| SH0/1 | Various | Shareability for table walks |
| EPD0 | [7] | Disable TTBR0 walks (1 = fault on TTBR0 access) |
| EPD1 | [23] | Disable TTBR1 walks (1 = fault on TTBR1 access) |

**Note:** TG0 and TG1 use different encodings for the same granule sizes.

### SLM-OS TCR Configuration

For 39-bit VA with 4KB granule:

```c
/*
 * TCR_EL1 configuration for SLM-OS
 *
 * - 39-bit VA (512GB address space)
 * - 4KB granule
 * - Inner shareable, write-back cacheable table walks
 * - TTBR0 disabled initially (kernel only)
 */
#define TCR_T0SZ(n)         (((64) - (n)) << 0)     /* TTBR0 VA size */
#define TCR_T1SZ(n)         (((64) - (n)) << 16)    /* TTBR1 VA size */
#define TCR_TG0_4KB         (0b00UL << 14)
#define TCR_TG1_4KB         (0b10UL << 30)
#define TCR_SH0_INNER       (0b11UL << 12)
#define TCR_SH1_INNER       (0b11UL << 28)
#define TCR_ORGN0_WB_WA     (0b01UL << 10)
#define TCR_ORGN1_WB_WA     (0b01UL << 26)
#define TCR_IRGN0_WB_WA     (0b01UL << 8)
#define TCR_IRGN1_WB_WA     (0b01UL << 24)
#define TCR_EPD0_DISABLE    (1UL << 7)              /* Disable TTBR0 */
#define TCR_IPS_40BIT       (0b010UL << 32)         /* 40-bit PA */

#define TCR_EL1_VALUE   (TCR_T0SZ(39)      | \
                         TCR_T1SZ(39)      | \
                         TCR_TG0_4KB       | \
                         TCR_TG1_4KB       | \
                         TCR_SH0_INNER     | \
                         TCR_SH1_INNER     | \
                         TCR_ORGN0_WB_WA   | \
                         TCR_ORGN1_WB_WA   | \
                         TCR_IRGN0_WB_WA   | \
                         TCR_IRGN1_WB_WA   | \
                         TCR_EPD0_DISABLE  | \
                         TCR_IPS_40BIT)
```

---

## MMU Enable Sequence

Enabling the MMU requires careful sequencing to avoid faults:

### Step-by-Step

```
1. Ensure MMU is disabled (SCTLR_EL1.M = 0)

2. Set up MAIR_EL1
   - Configure memory attribute slots

3. Set up TCR_EL1
   - Configure granule sizes, VA sizes, cacheability

4. Create translation tables
   - Allocate page-aligned memory for tables
   - Populate with valid descriptors
   - CRITICAL: Include identity mapping for current code

5. Set TTBR0_EL1 and/or TTBR1_EL1
   - Write physical address of L1 table
   - Ensure ASID is set if using TTBR0

6. Barrier sequence
   DSB ISH      ; Ensure all table writes complete
   ISB          ; Synchronize context

7. Enable MMU
   - Set SCTLR_EL1.M = 1
   - Optionally enable caches (C, I bits)

8. Barrier after enable
   ISB          ; Ensure MMU is active for next instruction
```

### Identity Mapping Requirement

**Critical:** The code that enables the MMU must be identity-mapped (VA = PA).

After `MSR SCTLR_EL1, Xn`, the next instruction is fetched from the address in the link register - which is still a physical address. Without identity mapping, this causes an immediate translation fault.

```
Physical memory:       Virtual memory (after MMU enable):
┌──────────────────┐   ┌──────────────────┐
│ 0x4000_0000      │   │ 0xFFFF_0000_4000_0000 (kernel VA)     │
│   mmu_enable:    │◄──│   (high mapping)                      │
│   msr sctlr...   │   │                                       │
│   isb            │   ├──────────────────┤
│   ret            │   │ 0x4000_0000                           │
└──────────────────┘   │   (identity mapping - same as PA)     │◄─┐
                       └──────────────────┘                      │
                                                                 │
                       After ret, LR still contains 0x4000_xxxx ─┘
```

### Post-Enable Transition

After MMU is enabled with identity mapping:
1. Jump to high kernel address (e.g., `0xFFFF_0000_4000_0000`)
2. Remove identity mapping (optional, for cleaner address space)
3. Continue execution from virtual addresses only

---

## SLM-OS Virtual Address Layout

### Phase 2 Layout (Kernel Only)

Using 39-bit VA with TTBR1 (kernel space starts at `0xFFFF_FF80_0000_0000`):

```
┌─────────────────────────────────────────────────────────────────────┐
│  SLM-OS Kernel Virtual Address Space (39-bit, TTBR1)                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  0xFFFF_FFFF_FFFF_FFFF  ┐                                           │
│          ...            │  Reserved                                 │
│  0xFFFF_FFC0_0000_0000  ┘                                           │
│                                                                     │
│  0xFFFF_FFBF_FFFF_FFFF  ┐                                           │
│          ...            │  MMIO Mappings (GIC, UART, etc.)          │
│  0xFFFF_FF80_0800_0000  ┘  ~128GB for device memory                 │
│                                                                     │
│  0xFFFF_FF80_07FF_FFFF  ┐                                           │
│          ...            │  Kernel heap / dynamic allocations        │
│  0xFFFF_FF80_0200_0000  ┘                                           │
│                                                                     │
│  0xFFFF_FF80_01FF_FFFF  ┐                                           │
│          ...            │  Kernel image (code + data + bss)         │
│  0xFFFF_FF80_0000_0000  ┘  Maps to PA 0x4000_0000                   │
│                                                                     │
│  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─     (Below this: TTBR0 / user space)        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Initial Mappings Required

| Virtual Address | Physical Address | Size | Attributes |
|-----------------|------------------|------|------------|
| Kernel code/data | 0x4000_0000 | ~2MB | Normal WB, RX/RW |
| Kernel stack | 0x4000_0000 + offset | 64KB | Normal WB, RW |
| UART | 0x0900_0000 | 4KB | Device nGnRnE |
| GIC Dist | 0x0800_0000 | 64KB | Device nGnRnE |
| GIC CPU | 0x0801_0000 | 64KB | Device nGnRnE |
| Identity (temp) | 0x4000_0000 | ~2MB | Normal WB, RX |

---

## Page Fault Handling

When translation fails, the CPU generates a synchronous exception with:

- **ESR_EL1**: Exception syndrome (includes fault type, access type)
- **FAR_EL1**: Faulting virtual address
- **ELR_EL1**: Address of the faulting instruction

### Exception Classes (ESR_EL1.EC)

| EC | Exception Type |
|----|----------------|
| 0x20 | Instruction Abort (lower EL) |
| 0x21 | Instruction Abort (same EL) |
| 0x24 | Data Abort (lower EL) |
| 0x25 | Data Abort (same EL) |

### Fault Status Codes (ESR_EL1.ISS[5:0])

| FSC | Fault Type |
|-----|------------|
| 0x04-0x07 | Translation fault (level 0-3) — page not mapped |
| 0x08-0x0B | Access flag fault (level 0-3) |
| 0x0C-0x0F | Permission fault (level 0-3) — access not allowed |
| 0x10-0x14 | Synchronous external abort |
| 0x21 | Alignment fault |

### Data Abort Fields

For data aborts (EC = 0x24/0x25), ESR_EL1 also contains:
- **WnR (bit 6)**: 1 = write access, 0 = read access
- **CM (bit 8)**: 1 = fault during cache maintenance operation

### SLM-OS Page Fault Handler

The handler in `kernel/arch/arm64/exceptions.c` provides detailed diagnostics:

```
*********************************
***       PAGE FAULT          ***
*********************************

Type:    Data Abort (WRITE)
Address: 0xdeadbeef
Reason:  Translation fault, level 3

Task Context:
  Task ID:   5
  Task Name: worker
  CPU:       1

Fault Location:
  ELR (PC):  0xffff000000080abc

Raw Exception State:
  ESR_EL1:   0x96000047
  EC:        0x25 (Data Abort (same EL))
  FSC:       0x7
  WnR:       1 (write)
  SPSR_EL1:  0x60000005

System halted.
```

### Implementation Files

| File | Purpose |
|------|---------|
| `kernel/arch/arm64/exceptions.c` | Page fault handler with `decode_fault_status()` |
| `kernel/src/panic.c` | General panic with task context and register dump |

### Future: Demand Paging

Phase 5+ may implement demand paging for model memory, where translation faults trigger lazy allocation rather than panic.

---

## TLB Shootdown API

The Translation Lookaside Buffer (TLB) caches page table entries for fast address translation. When page tables are modified, the TLB must be invalidated to ensure the new mappings take effect.

### API Functions

```c
#include "vmm.h"

/* Invalidate a single virtual address */
void vmm_invalidate_tlb(uint64_t virt);

/* Invalidate all TLB entries */
void vmm_invalidate_tlb_all(void);

/* Invalidate a range of virtual addresses */
void vmm_invalidate_tlb_range(uint64_t start, uint64_t end);

/* Invalidate by ASID (Address Space ID) - for future user space */
void vmm_invalidate_tlb_asid(uint64_t virt, uint8_t asid);
void vmm_invalidate_tlb_asid_all(uint8_t asid);
```

### Inner Shareable Domain

All TLB invalidation uses the "IS" (Inner Shareable) suffix, which means the invalidation is broadcast to all CPUs in the inner shareable domain:

```c
/* Single address invalidation */
__asm__ volatile(
    "dsb ishst\n"           /* Ensure PTE write complete */
    "tlbi vaae1is, %0\n"    /* Invalidate by VA, all ASIDs, Inner Shareable */
    "dsb ish\n"             /* Wait for TLB invalidate to complete */
    "isb"                   /* Synchronize instruction stream */
    : : "r"(virt >> 12) : "memory"
);
```

**TLBI Instructions Used:**

| Instruction | Description |
|-------------|-------------|
| `TLBI VAAE1IS, Xt` | Invalidate by VA, all ASIDs, EL1, Inner Shareable |
| `TLBI VAE1IS, Xt` | Invalidate by VA and ASID, EL1, Inner Shareable |
| `TLBI ASIDE1IS, Xt` | Invalidate by ASID, EL1, Inner Shareable |
| `TLBI VMALLE1IS` | Invalidate all EL1 entries, Inner Shareable |

The "IS" suffix ensures that on SMP systems, all CPUs see the TLB invalidation without requiring explicit inter-processor interrupts (IPIs).

### When to Invalidate

TLB invalidation is required after:
1. Changing a page table entry's physical address
2. Changing a page table entry's permissions
3. Unmapping a virtual address
4. Context switch to a different ASID (future)

**Common pattern:**
```c
/* 1. Modify page table */
l2_table[index] = new_pte;

/* 2. Barrier to ensure write is visible */
__asm__ volatile("dsb ishst" ::: "memory");

/* 3. Invalidate TLB */
vmm_invalidate_tlb(virt_addr);
```

### Test Coverage

The VMM tests in `kernel/tests/test_vmm.c` include functional TLB tests that verify invalidation correctness:

1. **test_remap_requires_invalidation** — Remaps a VA to a different PA, verifies the new PA is accessed
2. **test_remap_with_full_flush** — Same test using `vmm_invalidate_tlb_all()`
3. **test_remap_with_range_invalidation** — Same test using `vmm_invalidate_tlb_range()`
4. **test_sequential_remaps** — Remaps VA through PA1→PA2→PA3→PA1
5. **test_rapid_remap_stress** — 50 rapid remap cycles with verification

These tests use helper functions to manipulate page tables without automatic TLB invalidation, proving that explicit invalidation is necessary and working.

---

## References

### Official ARM Documentation
- [Armv8-A Address Translation](https://documentation-service.arm.com/static/5efa1d23dbdee951c1ccdec5)
- [Learn the Architecture: AArch64 Memory Model](https://documentation-service.arm.com/static/6298a839b334256d9ea8af70)
- [Learn the Architecture: AArch64 Memory Attributes](https://documentation-service.arm.com/static/63a43e333f28e5456434e18b)

### Tutorials
- [AArch64 MMU Programming (Lowenware)](https://lowenware.com/blog/aarch64-mmu-programming/)
- [Quick and Dirty AArch64 MMU Setup](https://dannasman.github.io/aarch64-mmu.html)
- [ARM Paging - OSDev Wiki](https://wiki.osdev.org/ARM_Paging)

### Linux Kernel Reference
- [Memory Layout on AArch64 Linux](https://docs.kernel.org/arch/arm64/memory.html)
- [AArch64 Kernel Page Tables](https://wenboshen.org/posts/2018-09-09-page-table.html)

### Tools
- [arm64-pgtable-tool](https://github.com/ashwio/arm64-pgtable-tool) - Generates page table setup code

---

*Last updated: December 2025*
