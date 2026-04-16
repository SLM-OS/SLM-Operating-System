# NVIDIA GSP Firmware — Research Findings

This document captures research into the NVIDIA GPU System Processor (GSP) firmware boot sequence, based on analysis of the NVIDIA open-gpu-kernel-modules source and the nouveau Linux kernel driver. These findings were gathered during Phase 4X (x86-64 port) to understand what is required for bare-metal GPU compute on Ampere architecture.

> **Phase E status (2026-04-16):** **FWSEC-FRTS (E3.4) succeeds on
> retail Ampere** (GA107 / RTX 3050 under VFIO on test-pc), 3/3
> runs, WPR2 populated. **Booter Load (E3.4.d) is blocked** on this
> platform by the SEC2 priv-lock raised by BSI after vfio-pci's
> mandatory FLR (issue #185). E3.4.e, E4, E5, E6 are transitively
> blocked. Code-shipped portions transfer cleanly to Jetson and
> bare-metal SLM-OS. **See `docs/x86-64-gpu-inference-status.md`
> for the live handoff, the root-cause explanation, and the
> ranked paths forward.** Execution plan archived at
> `docs/archive/plans/x86-64-capstone-gap-closure-plan.md` §E.

---

## Summary

**GSP is mandatory on Ampere (GA10x) and later.** There is no legacy register-programming mode. The GPU gates engine access behind GSP-RM initialization — uninitialized engine registers return `0xBADF5040`. Without GSP, SLM-OS can enumerate the GPU, read identification registers, and access VRAM via BAR1, but cannot perform compute, 3D, or display operations.

**Bringing GSP up is complex but tractable.** The boot chain involves two separate microcontrollers (SEC2 Falcon and GSP RISC-V), cryptographic verification, a 38 MB firmware blob, and a full RPC communication stack. SLM-OS now has the firmware embedded in the kernel image (E1), the BIT-table parser shared across platforms (E2), and a vtable-based platform shim (`struct gsp_platform_ops`) that lets x86-64 bare-metal, the Linux userspace harness, and future Jetson bare-metal all drive the same shared core.

---

## What is the GSP?

The GSP (GPU System Processor) is a **RISC-V microcontroller embedded on the GPU die**. Starting with Turing (TU10x) and becoming mandatory on Ampere (GA10x), NVIDIA offloaded the Resource Manager (RM) — previously kernel-mode CPU code — onto this core. The GSP runs an OS called **LibOS** which hosts **GSP-RM**, the full GPU resource manager.

On Ampere:
- All GPU engine initialization goes through GSP-RM
- Memory management uses GSP-controlled page tables
- Display, compute, and 3D engines require GSP to bring them out of reset
- Without GSP, BAR0 engine registers return `0xBADF5040` (hardware fault indicator)

---

## What SLM-OS Can Do Without GSP

Verified on real hardware (i7-6700 + RTX 3050, GA107):

| Capability | Status | Notes |
|------------|--------|-------|
| PCI enumeration | ✅ Works | 21 devices found via ECAM |
| GPU identification | ✅ Works | BOOT_0, BOOT_42 → GA107 (0x177), Ampere, Rev 10.1 |
| BAR0 MMIO register read | ✅ Partial | PMC registers work; engine registers return 0xBADF5040 |
| BAR1 VRAM read/write | ✅ Works | 5 offsets verified (0–128 MB) |
| PTIMER | ✅ Works | GPU timer readable (does not require GSP) |
| PSTRAPS | ✅ Works | Strap configuration readable |
| Engine registers | ❌ Blocked | Return 0xBADF5040 (engines in reset, GSP not loaded) |
| GPU compute | ❌ Blocked | Requires GSP-RM to initialize PGRAPH |
| Display output | ❌ Blocked | Requires GSP-RM to initialize PDISPLAY |

---

## GSP Firmware Files

For GA107 (RTX 3050), firmware lives at `/lib/firmware/nvidia/ga107/gsp/` (symlinks to `ga102/gsp/`):

| File | Size | Format | Purpose |
|------|------|--------|---------|
| `gsp-535.113.01.bin.zst` | 38 MB | RISC-V ELF | GSP-RM firmware (the full resource manager) |
| `bootloader-535.113.01.bin.zst` | 20 KB | Binary blob | GSP bootloader (loaded into WPR) |
| `booter_load-535.113.01.bin.zst` | 60 KB | HS Falcon ucode | Falcon microcode that loads GSP-RM into WPR |
| `booter_unload-535.113.01.bin.zst` | 40 KB | HS Falcon ucode | Falcon microcode for teardown |

A monolithic copy also exists at `/lib/firmware/nvidia/535.288.01/gsp_ga10x.bin`.

---

## GSP Boot Sequence

The complete boot chain, traced from nouveau's Linux kernel driver:

### Phase 0: Firmware Loading from Disk

```
nvkm_gsp_load_fw("gsp", "535.113.01")          → gsp-535.113.01.bin.zst
nvkm_gsp_load_fw("bootloader", "535.113.01")    → bootloader-535.113.01.bin.zst
nvkm_gsp_load_fw("booter_load", "535.113.01")   → booter_load-535.113.01.bin.zst
nvkm_gsp_load_fw("booter_unload", "535.113.01") → booter_unload-535.113.01.bin.zst
```

### Phase 1: One-Time Init

1. Extract `.fwimage` section from GSP ELF into DMA-accessible memory
2. Extract `.fwsignature_ga10x` section for signature verification
3. Build **radix3 page table** (3-level page table mapping ~38 MB ELF into GPU-addressable pages)
4. Parse bootloader binary header for `monitorCodeOffset`, `monitorDataOffset`, `manifestOffset`, `appVersion`
5. Construct **FWSEC-SB** ucode from VBIOS (Secure Boot validation code from GPU ROM)
6. Initialize **libos arguments** — 4 entries:
   - `LOGINIT` — init log buffer (DMA memory)
   - `LOGINTR` — interrupt log buffer
   - `LOGRM` — RM log buffer
   - `RMARGS` — physical address of RM arguments structure
7. Populate **GspSystemInfo** — PCIe config, physical addresses, chipset info

### Phase 2: FB Layout Calculation

Memory layout at the top of VRAM (all offsets relative to FB end):

```
FB Top (e.g., 6 GB for RTX 3050)
 ├── VGA Workspace (128KB–1MB, from VBIOS register 0x625F04)
 ├── FRTS Region (1 MB) — Firmware Runtime Services
 ├── Boot Binary (bootloader, ~20KB, 4K aligned)
 ├── GSP-RM ELF (38 MB, 64K aligned)
 ├── WPR2 Heap (~22–98 MB depending on VRAM size, 1MB aligned)
 ├── WPR2 Metadata (GspFwWprMeta, 256 bytes, 1MB aligned)
 └── Non-WPR Heap (1 MB)
```

### Phase 3: FWSEC-FRTS Execution

1. Parse FWSEC ucode from VBIOS — *empirically* the FWSEC ucode is
   NOT a top-level BIT entry with id 0x85 as originally documented
   here. On production Ampere (validated against an RTX 3050), FWSEC
   is reachable via the PMU ucode descriptor table pointed to by the
   `BIT_TOKEN_FALCON_DATA` entry (id 0x70). The NPDS sub-image format
   adds a further wrinkle not handled by openrm 535.113.01 or
   nova-core mainline. Full write-up in `docs/archive/investigations/x86-64-gsp-fwsec-investigation.md`
   and [issue #143](https://github.com/johnjezl/CS-496-Capstone-SLM-Operating-System/issues/143).
2. Load FWSEC into GSP Falcon's IMEM/DMEM via PIO
3. Boot the Falcon to run FWSEC, which establishes the **Write Protected Region** (WPR2)
4. Verify WPR2 via registers `0x1FA824` (WPR2_LO) and `0x1FA828` (WPR2_HI)

### Phase 4: GSP Reset into RISC-V Mode

1. Engine reset via `NV_PGSP_FALCON_ENGINE` at `0x1103C0` (bit 0)
2. Poll `NV_PFALCON_FALCON_HWCFG2` (offset `0xF4`) for RESET_READY
3. Program `NV_PRISCV_RISCV_BCR_CTRL` (offset `0x668`):
   - `CORE_SELECT = RISCV` (bit 4 = 1)
   - `VALID = TRUE` (bit 0 = 1)

### Phase 5: Program Mailboxes

```c
// Write libos arguments address to GSP mailboxes
nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));  // MAILBOX0
nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));  // MAILBOX1
```

Register addresses: `NV_PGSP_FALCON_MAILBOX0` = `0x110040`, `NV_PGSP_FALCON_MAILBOX1` = `0x110044`.

### Phase 6: Boot GSP via Booter Load

1. Set up shared memory for message queues (command queue + status queue, each 256 KB)
2. Populate `GSP_ARGUMENTS_CACHED` with shared memory addresses
3. Populate `GspFwWprMeta` with all FB layout addresses
4. Execute **Booter Load** on the **SEC2 Falcon** (a separate security processor):
   - Writes WPR metadata address to SEC2 mailboxes
   - Booter Load copies GSP-RM ELF into WPR, starts GSP RISC-V core
5. Wait for `NV_PRISCV_RISCV_CPUCTL` bit 7 (`ACTIVE_STAT`) to indicate RISC-V is running

### Phase 7: Wait for GSP-RM

1. Write `appVersion` to `NV_PFALCON_FALCON_OS` (offset `0x080`)
2. Poll message queue for `NV_VGPU_MSG_EVENT_GSP_INIT_DONE` RPC
3. Once received, GSP-RM is running — all GPU management goes through RPC

---

## Key Registers (BAR0 Offsets)

### GSP Falcon (base `0x110000`)

| Register | Offset | Purpose |
|----------|--------|---------|
| `NV_PGSP_FALCON_MAILBOX0` | `+0x040` | LibOS args address (low 32) |
| `NV_PGSP_FALCON_MAILBOX1` | `+0x044` | LibOS args address (high 32) |
| `NV_PFALCON_FALCON_OS` | `+0x080` | App version writeback |
| `NV_PFALCON_FALCON_HWCFG2` | `+0x0F4` | HW config (RISCV capability bit 10) |
| `NV_PFALCON_FALCON_CPUCTL` | `+0x100` | CPU control (STARTCPU bit 1, HALTED bit 4) |
| `NV_PFALCON_FALCON_BOOTVEC` | `+0x104` | Boot vector address |
| `NV_PRISCV_RISCV_CPUCTL` | `+0x388` | RISC-V CPU status (ACTIVE bit 7) |
| `NV_PRISCV_RISCV_BCR_CTRL` | `+0x668` | Boot control (CORE_SELECT, VALID) |
| `NV_PGSP_FALCON_ENGINE` | `+0x3C0` | Engine reset (bit 0) |
| `NV_PGSP_QUEUE_HEAD(i)` | `+0xC00+i*8` | Message queue heads (8 entries) |

### WPR (Write Protected Region)

| Register | Offset | Purpose |
|----------|--------|---------|
| WPR2_LO | `0x1FA824` | WPR2 region low address |
| WPR2_HI | `0x1FA828` | WPR2 region high address |

---

## Nouveau Source File Map

For anyone extending SLM-OS to implement GSP boot, these are the key source files in the Linux kernel nouveau driver:

| File | Purpose |
|------|---------|
| `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c` | GSP-RM RPC protocol, message queues, init sequence |
| `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c` | GSP one-time init: firmware load, radix3 page tables, libos args, FB layout, FWSEC, boot |
| `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c` | GA102-specific GSP reset (RISC-V mode switch) |
| `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/base.c` | GSP firmware loading from /lib/firmware |
| `drivers/gpu/drm/nouveau/nvkm/falcon/fw.c` | Falcon microcode execution (SEC2 Booter Load) |
| `drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c` | GA102 Falcon reset and RISC-V BCR programming |

### open-gpu-kernel-modules Reference Headers

| File | Purpose |
|------|---------|
| `src/common/inc/swref/published/nv_ref.h` | NV_PMC_BOOT_0, BOOT_42 bit fields |
| `src/common/inc/swref/published/nv_arch.h` | Architecture codes (Ampere=0x17) |
| `src/common/inc/swref/published/ampere/ga100/dev_boot.h` | BOOT register offsets |
| `src/common/inc/swref/published/ampere/ga100/dev_nv_xve.h` | PCIe config space registers |
| `src/nvidia/generated/g_rpc-structures.h` | RPC message structures for GSP-RM communication |

---

## Platform Shim Contract (`struct gsp_platform_ops`)

The shared GSP bringup code in `kernel/gpu/nvidia/` never touches hardware directly. All platform-specific behavior is isolated behind a vtable defined in `kernel/gpu/nvidia/gsp.h`. Each platform provides an implementation of `struct gsp_platform_ops` and installs it into the global `gsp_platform` pointer before calling `gsp_init()`.

### Existing Implementations

| Platform | File | Status |
|----------|------|--------|
| x86-64 bare-metal | `kernel/arch/x86_64/nvidia_gsp_platform.c` | Partial — `firmware_get`, `vbios_get_fwsec`, `mb` implemented; BAR0/BAR1/DMA stubbed |
| Linux userspace (VFIO) | `host-tools/gsp-harness/linux_platform.c` | Complete — used for hardware validation on test-pc |
| ARM64 (Jetson) | `kernel/arch/arm64/nvidia_gsp_platform_stub.c` | Linker stub only — full implementation needed |

### Vtable Functions

#### 1. BAR0 Register Access

```c
uint32_t (*read32)(uint32_t bar0_offset);
void     (*write32)(uint32_t bar0_offset, uint32_t value);
```

**Purpose:** 32-bit GPU register read/write. Offsets are relative to BAR0 base — identical between discrete PCIe (ECAM-mapped) and integrated SoC (fixed MMIO at `0x17000000` on Jetson).

**Constraints:**
- Reads MUST NOT be cached; each call reads fresh from hardware
- Writes MUST be ordered against subsequent reads. On x86-64 this is automatic (strong memory model). On ARM64, a `DSB SY` between successive MMIO writes to distinct registers is required

**Called from:** `falcon.c` (all register access via `flcn_r32`/`flcn_w32`), `bringup.c` (FWSEC mailboxes, WPR2 readback, Booter Load BROM registers, RISC-V BCR_CTRL)

**Platform notes:**
- x86-64: volatile dereference of ioremap'd BAR0 VA
- Jetson: volatile dereference of identity-mapped `0x17000000 + offset`
- VFIO: volatile dereference of mmap'd VFIO BAR0 region

#### 2. BAR1 (VRAM) Byte-Level Access

```c
void (*bar1_read)(uint32_t offset, void *dst, size_t n);
void (*bar1_write)(uint32_t offset, const void *src, size_t n);
```

**Purpose:** Read/write arbitrary-sized byte buffers into GPU VRAM (BAR1 aperture). Offsets are BAR1-relative.

**Constraints:**
- x86-64: uncached MMIO aperture; strongly ordered
- Jetson: window into unified memory; platform is responsible for cache maintenance before returning
- Not yet called in E1–E4 phases (needed for E5 compute submission — tensor data residency)

#### 3. DMA Memory Allocation

```c
void *(*dma_alloc)(size_t size, size_t align, uint64_t *out_dma_addr);
void  (*dma_free)(void *ptr, size_t size);
```

**Purpose:** Allocate memory accessible by both CPU and GPU. Returns a CPU-addressable pointer and writes the GPU-visible DMA address to `*out_dma_addr`.

**Constraints:**
- `align` is typically 256 (Falcon DMA) or 4096 (WprMeta)
- If `out_dma_addr` is non-NULL, the GPU-visible address must be written
- Shared code treats the DMA address as opaque — never assumes identity mapping

**Platform notes:**
- x86-64: DMA address == physical address (IOMMU passthrough); allocate from PMM
- Jetson: unified memory; may need SMMU mapping and return IOVA
- VFIO: `ioctl(VFIO_IOMMU_MAP_DMA)` to map pages into GPU address space

**Called from:** `bringup.c` (FWSEC IMEM/DMEM staging, Booter Load data section copy, GspFwWprMeta), `rpc.c` (shared memory ring — E4 skeleton)

#### 4. Cache Maintenance

```c
void (*cache_clean)(const void *addr, size_t size);
void (*cache_invalidate)(void *addr, size_t size);
```

**Purpose:**
- `cache_clean`: writeback dirty cachelines to Point of Coherency. Called BEFORE the GPU reads DMA buffers
- `cache_invalidate`: invalidate cachelines. Called BEFORE the CPU reads data written by the GPU

**Platform notes:**
- x86-64: no-ops (PCIe DMA is cache-coherent)
- Jetson: DC CVAC (clean) / DC IVAC (invalidate) via `cache.h` helpers; Jetson's per-core L2 is incoherent
- VFIO: no-ops (Linux manages coherency)

**Called from:** `bringup.c` (after memcpy/patching firmware buffers, before Falcon DMA reads), `rpc.c` (RPC mailbox words for cross-CPU coherency)

#### 5. Memory Barrier

```c
void (*mb)(void);
```

**Purpose:** Full memory barrier. Ensures register writes are complete and visible to the GPU before subsequent operations.

**Implementation:**
- x86-64: `mfence`
- Jetson: `dsb sy`
- VFIO: `__sync_synchronize()` (GCC built-in)

**Called from:** `falcon.c` — extensively after register write sequences (DMA transfer setup, BROM programming, BCR_CTRL writes)

#### 6. Firmware Accessor

```c
void (*firmware_get)(enum gsp_firmware_kind kind, struct gsp_firmware_blob *out);
```

**Firmware kinds:** `GSP_FW_GSP` (RISC-V ELF, ~38 MB), `GSP_FW_BOOTLOADER` (~20 KB), `GSP_FW_BOOTER_LOAD` (~60 KB), `GSP_FW_BOOTER_UNLOAD` (~40 KB).

**Constraints:**
- MUST NOT return NULL function pointer — if a blob is unavailable, set `out->{data=NULL, size=0, version=NULL}` and the bringup aborts cleanly in Phase 0
- Firmware version string (e.g. `"535.113.01"`) must accompany each blob

**Platform notes:**
- x86-64: embedded at build time via `.incbin` (in `nvidia_gsp_firmware.S`)
- Jetson: can embed the same way, or load from rootfs at runtime
- VFIO: loaded from `/lib/firmware/nvidia/` at harness startup

#### 7. VBIOS FWSEC Access

```c
int (*vbios_get_fwsec)(const void **out_data, size_t *out_size);
```

**Purpose:** Retrieve the FWSEC (Firmware Security Boot) ucode from the NVIDIA VBIOS. Returns 0 on success, -1 if unavailable.

**Platform notes:**
- x86-64: reads PCI Expansion ROM BAR, parses BIT table via `nvidia_vbios_parse()`, extracts FWSEC from PMU descriptor table
- Jetson: no VBIOS exists. Set `*out_data = NULL` and return 0 — the boot sequence has a Jetson-specific path that sources FWSEC-equivalent setup from the pre-boot firmware (QSPI). This path is not yet implemented; it will likely require reading FWSEC from the L4T BSP partition
- VFIO: same as x86-64, via sysfs ROM file

### Initialization Sequence

```
platform_gpu_init()
    └── Discover GPU (PCI scan or fixed MMIO probe)
    └── Map BAR0 / BAR1
    └── Populate platform ops vtable
    └── gsp_platform = &<platform>_gsp_ops;
    └── gsp_init()          ← shared 7-phase boot sequence begins
```

On x86-64, `nvidia_gpu_init()` calls `x86_gsp_platform_install()`. On Jetson, the equivalent function must be written in `kernel/arch/arm64/nvidia_gsp_platform.c`.

### Shared Code That Calls Through the Vtable

| Phase | Ops Used | File |
|-------|----------|------|
| Phase 0: Firmware Load | `firmware_get` | `gsp.c` |
| Phase 3: FWSEC-FRTS | `vbios_get_fwsec`, `dma_alloc`, `dma_free`, `read32`, `write32`, `cache_clean`, `mb` | `bringup.c` |
| Phase 6: Booter Load | `firmware_get`, `dma_alloc`, `dma_free`, `cache_clean`, `mb`, `read32`, `write32` | `bringup.c` |
| Phase 4: RISC-V Startup | `read32`, `write32`, `mb` | `bringup.c` |
| Falcon Driver | `read32`, `write32`, `mb` | `falcon.c` |
| RPC (E4) | `cache_clean`, `cache_invalidate`, `mb` | `rpc.c` |
| E5 Compute (future) | `bar1_read`, `bar1_write`, `dma_alloc`, `dma_free` | — |

### ARM64 (Jetson) Implementation Checklist

The full Jetson platform shim (`kernel/arch/arm64/nvidia_gsp_platform.c`) must implement all 11 vtable functions. Specific considerations:

| Function | Jetson Approach | Notes |
|----------|----------------|-------|
| `read32` / `write32` | Volatile read/write at `(0x17000000 + offset)` | GPU MMIO confirmed accessible from EL2+VHE |
| `bar1_read` / `bar1_write` | Unified memory — may be identity or need SMMU IOVA | Determine whether GA10B BAR1 is a separate aperture or aliases system RAM |
| `dma_alloc` / `dma_free` | PMM pages + optional SMMU mapping | If GA10B uses StreamID-based isolation, platform must install SMMU entries |
| `cache_clean` | `cache_clean_range()` from `kernel/include/cache.h` | DC CVAC, already used by SMP code |
| `cache_invalidate` | `cache_invalidate_range()` from `kernel/include/cache.h` | DC IVAC |
| `mb` | `__asm__ volatile("dsb sy" ::: "memory")` | |
| `firmware_get` | `.incbin` at build time (same as x86-64) or load from SD card | L4T BSP firmware at `/lib/firmware/nvidia/ga10b/gsp/` |
| `vbios_get_fwsec` | Return `{NULL, 0}` with rc=0 | No VBIOS on integrated GPU; FWSEC-equivalent from QSPI boot chain |

### Testing

Platform shim implementations are validated by the shared test suites:

- **Host tests (no GPU required):** `test-vbios` (30), `test-falcon` (37), `test-bringup` (25), `test-rpc` (17) — 123 total, all passing
- **Hardware validation:** FWSEC-FRTS execution (3/3 on x86-64 RTX 3050), WPR2 register readback
- **Kernel integration:** `make test` GPU subsystem tests (22 tests, stub driver)

A new platform shim should pass all host tests before attempting hardware validation. The host test harness (`host-tools/gsp-harness/`) uses mock platform ops, so the tests exercise the shared GSP code paths without real GPU hardware.

---

## Why This Matters for Jetson

The Jetson Orin Nano uses the same Ampere GPU architecture (GA10B, an integrated variant of GA10x). The GSP boot sequence is fundamentally the same:
- Same RISC-V core, same firmware format
- Same SEC2 Falcon Booter Load mechanism
- Same WPR memory layout
- Same RPC communication protocol

The key difference is that on Jetson, the GSP firmware may be pre-loaded by the bootloader (UEFI/CBoot), whereas on discrete PCIe GPUs it must be loaded by the OS driver. If the Jetson CBB firewall can be resolved, the GSP communication patterns documented here apply directly.

---

## Implementation notes (E3.4 audit)

Two bugs in the FWSEC-FRTS path were caught by a code-level audit
against nouveau and nova-core during E3.4 hardware bringup. Both
reproduce the symptom "FWSEC ucode starts (MAILBOX0 transitions
from 0xCAFEBEEF → 0) but Falcon never halts and WPR2 stays at
baseline".

1. **BOOTVEC must be 0 for FWSEC v3.** Both `nvkm_gsp_fwsec_v3`
   (`fw->boot_addr = 0`) and nova-core
   (`FwsecFirmware::boot_addr() -> 0`) hard-code this. The
   descriptor's `IMEMVirtBase` field is metadata about where the
   ucode was BUILT to run — it is **not** the entry PC after BROM
   verify. Passing `IMEMVirtBase` to BOOTVEC starts the Falcon at a
   non-zero PC inside garbage memory.

2. **CPUCTL.ALIAS_EN must be honoured at STARTCPU time.** After
   BROM verifies the signed ucode, it sets `CPUCTL.ALIAS_EN`
   (bit 6) and gates writes to `CPUCTL` (`0x100`). The release path
   is `CPUCTL_ALIAS` (`0x130`). nova-core checks this on every
   start; nouveau happens to dodge the bug because the cards it
   tests clear ALIAS_EN quickly. Both bugs were fixed in
   `kernel/gpu/nvidia/{bringup,falcon}.c` and have regression
   tests in `host-tools/gsp-harness/test_falcon.c`
   (`test_start_uses_cpuctl_alias_when_en_set`).

Hardware re-test on test-pc (RTX 3050) is required to confirm
WPR2 now populates and the ucode HALTs cleanly.

## Conclusion

SLM-OS demonstrates bare-metal GPU access on NVIDIA Ampere: PCI
enumeration, chip identification (GA107/RTX 3050), BAR0 register
reads, verified VRAM read/write via BAR1, and a cross-platform
shared GSP-RM bringup core (`kernel/gpu/nvidia/`) that drives
FWSEC-FRTS, Booter Load on SEC2 (E3.4.d), GSP RISC-V startup
(E3.4.e), and the RPC ring scaffolding (E4). Full GPU compute
remains gated on hardware completion of FWSEC, full WprMeta
population, and the RPC marshalling that lights up GSP_INIT_DONE.

---

*Research completed: April 2026*
*Hardware: i7-6700 + NVIDIA GeForce RTX 3050 (GA107, device 0x2584)*
*References: NVIDIA open-gpu-kernel-modules, Linux kernel nouveau driver, envytools*
