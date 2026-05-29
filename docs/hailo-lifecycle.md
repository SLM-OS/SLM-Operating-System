# Hailo-8L Lifecycle Reference (Pi 5 / SLM-OS)

End-to-end reference for everything SLM-OS does to get a Hailo-8L NPU
on the Raspberry Pi 5 AI HAT+ from cold-boot to running inference.
Every section cites the source files / line ranges that implement
the step, so this can be used as a starting point for debugging or
as the authoritative reference for fw / ABI changes.

This document is descriptive, not aspirational — it documents the
code as it currently stands, including known wedges and quirks.

> **How this doc drifts.** Line numbers reflect the tree state at
> the most recent update (see footer). Section headings and symbol
> names are stable; line ranges may drift as files are edited.
> `git grep <symbol>` is authoritative when in doubt.

---

## 0. Hardware context

**Board:** Raspberry Pi 5 + Raspberry Pi AI HAT+ (Hailo-8 / Hailo-8L
silicon). The HAT+ sits on the Pi 5's external PCIe Gen2 x1 connector
(`pcie1`). The Pi 5's other PCIe controller (`pcie0`) is wired to the
internal RP1 chip and is not used by the Hailo path.

**SoC:** Broadcom BCM2712 (Pi 5). The PCIe1 root complex is documented
under `kernel/drivers/pcie/pcie_bcm2712.c`.

**Topology:**

```
┌──────────────────────────────┐
│  Cortex-A76 cluster (EL1)    │
│  ┌──────────────────────────┐│
│  │ SLM-OS kernel            ││
│  └────────┬─────────────────┘│
│           │ MMIO (Device-nGnRnE)
│  ┌────────▼─────────────────┐│
│  │ BCM2712 PCIe1 RC         ││
│  │   - link-training        ││
│  │   - MIP1 MSI controller  ││
│  │   - ATR (in/outbound)    ││
│  └────────┬─────────────────┘│
└───────────┼──────────────────┘
            │ PCIe Gen2 x1 (5 GT/s)
   ┌────────▼─────────────────┐
   │ Hailo-8L endpoint        │
   │   BAR0  config + ATR     │
   │   BAR2  VDMA channels    │
   │   BAR4  FW SRAM window   │
   └──────────────────────────┘
```

**Key terms used throughout this doc:**

| Term | Meaning |
|---|---|
| **ATR** | Address Translation Region — a window in BAR4 that the host moves around to access different parts of the Hailo's internal SRAM. |
| **BAR0/2/4** | PCIe Base Address Registers. The Hailo-8 exposes three: BAR0 = device config + ATR programming, BAR2 = per-VDMA-channel registers, BAR4 = ATR-mapped SRAM access. |
| **VDMA** | Vendor DMA engine on the Hailo. Channels are per-direction (H2D vs D2H), each programmed via descriptor lists. |
| **CS / context-switch** | Hailo's network-group / context-switch protocol layered over FW_CONTROL RPCs. |
| **MIP1** | Pi 5 PCIe1 MSI peripheral at `0x1000131000`. Translates 8 MSI vectors to GIC SPIs `247..254`. |
| **ch=1 / ch=2 / ch=16** | The three VDMA channels the inference path uses: CCW upload (1, H2D), boundary input (2, H2D), boundary output (16, D2H). |

---

## 1. External interaction timeline

This section is what the boundary between SLM-OS and the outside world
sees. Every row is *one* MMIO transaction, IRQ, RPC round-trip,
doorbell, or DMA event — internal function calls are out of scope.

**Legend:**

| Type | Direction | Mechanism |
|---|---|---|
| MMIO-W | host → device | 32-bit write to a BAR offset |
| MMIO-R | host → device | 32-bit read returning a value |
| Doorbell | host → device | MMIO-W to a trigger register |
| RPC | host → fw | Body in BAR4+0x000, doorbell at BAR4+0x1684 |
| RPC-resp | fw → host | Body in BAR4+0x640, signaled by MSI bit 26 |
| IRQ | device → host | MSI to MIP1 → GIC SPI 247..254 |
| DMA-pull | device → host RAM | Endpoint reads through inbound aperture |
| DMA-push | device → host RAM | Endpoint writes through inbound aperture |

**Timing markers:**

- † = calibrated value (post-bisect, do not change without re-measuring)
- ‡ = observed bound (timeout ceiling or measured upper)
- ★ = best estimate, not characterized
- (no mark) = sequential / negligible / fast (µs-order)

### Phase 1 — Power-up
*T = 0 at first PCIe controller MMIO. Total ~50-150 ms.*

| T+ | Type | Target | Operation |
|---|---|---|---|
| 0 | MMIO-W ×2 | RESCAL_BASE+0x00 | **Toggle PCIe1 controller SW reset** (bridge ID 43, bit 0 = 1 then 0) |
| ~1 ms★ | MMIO-R poll | RESCAL_BASE+0x08 | **Wait for RESCAL done** (poll bit 0) |
| ~ms | MMIO-W ×7 | PCIE1_MDIO_* (0x1100-0x1108) | **Program SerDes PHY PLL** for 54 MHz xosc refclk (7-entry `mdio_pll_tune[]` table) |
| ~10 ms | MMIO-W batch | PCIE1_BASE+config | **Configure RC bridge:** RC_BAR2 size=0x15 (64 GB inbound aperture), class=PCI bridge, vendor endian, HARD_DEBUG CLKREQ control, VDM QoS forwarding (hyp-T) |
| ≤100 ms‡ | MMIO-R poll | PCIE1_MISC_STATUS (0x4068) | **Wait for PCIe link up** (PHY_LINKUP+DL_ACTIVE, bits 4,5) |
| | MMIO-R | EP cfg space (bus 1) | **Read endpoint vendor/device** — expect 0x1E60/0x2864 (Hailo-8); enumeration |
| | MMIO-W | EP cfg cmd reg | **Enable BUS_MASTER + MEM_SPACE** — endpoint can now issue DMA |
| | MMIO-W | EP PCIe Express cap+0x10 | **Disable ASPM L0s** (clear LNKCTL bit 0) — avoids link-recovery storms under heavy VDMA |
| | MMIO-W ×3 | EP MSI cap | **Program endpoint MSI cap** (MSI_ADDR_LO/HI/DATA → MIP1 doorbell) |
| | MMIO-W ×4 | MIP1 (0x1000131000) | **Configure MSI peripheral:** unmask host vectors (MASKL_HOST=0), mask VPU (=0xFFFFFFFF), edge-triggered (CFGL=0xFFFFFFFF), clear pending |
| | MMIO-W ×3 | BCM2712 outbound windows | **Map outbound translations** (BAR0/2/4 PCIe addr → CPU addr) — host can now reach endpoint BARs |

### Phase 2 — Firmware boot
*T = 0 at first probe MMIO. Total ~150-1000 ms (mostly fw upload time).*

| T+ | Type | Target | Operation |
|---|---|---|---|
| 0 | MMIO-W | BAR0+ATR[0] | **Retarget BAR4 ATR window** to device 0xE0000 (boot status region) |
| | MMIO-R | BAR4+(0xE0000 & 0xFFF) | **Read boot status** word — sanity check that PCIe + ATR work |
| | MMIO-R | BAR0+0x0098 | **Read vendor ID** (HAILO_REG_VENDOR) — second smoke test |
| ~50-200 ms★ | MMIO-W ×N | BAR4 ATR window (low 4 KB) | **Upload app firmware** to device 0x60000; chunked across 4 KB ATR pages, ATR retargeted per page |
| ~10 ms★ | MMIO-W ×N | BAR4 ATR window | **Upload boot key cert** to device 0xE0048 |
| ~10 ms★ | MMIO-W ×N | BAR4 ATR window | **Upload boot content cert** to device 0xE0390 |
| ~50-200 ms★ | MMIO-W ×N | BAR4 ATR window | **Upload core firmware** to device 0xC0000 |
| | MMIO-W | dev 0xE0980 (BAR4+0x980 after ATR[0] retarget to 0xE0000) | **Trigger fw boot** — write 0x00000001 to the boot ROM trigger register; chip branches to loaded firmware |
| ≤1000 ms‡ | MMIO-R poll | BAR0+ATR[1].trsl_lo | **Poll for 0x00200000 magic** — fw signals "boot complete" by writing this register |
| (varies) | IRQ | MIP1 → GIC SPI | **BOOT_IRQ** — fw raises MSI to host once running |

> **Two separate "doorbells" — don't confuse them.** The boot trigger
> (above) is a write to *device address* `0xE0980`, reached via
> ATR[0] retargeting BAR4's low 4 KB to device page `0xE0000`, then
> writing at BAR4+`0x980`. The FW_CONTROL doorbell used in later
> phases lives at *fixed* `BAR4+0x1684`, outside the ATR window —
> a register that doesn't move when ATR[0] is retargeted.

### Phase 3 — Post-boot setup
*T = 0 at boot magic / BOOT_IRQ. Total ~510-550 ms (dominated by 500 ms settle).*

| T+ | Type | Direction | Operation |
|---|---|---|---|
| 0 | (busy-wait) | — | **500 ms settle** (hyp-O†) — empirically suppresses boot-time `SAGE1_ISP` ECC during the first RPC. Mechanism is not fully characterized — fw-internal init / SRAM scrub / task scheduling all plausible; runtime ECC is *not* fixed by this delay. Bisect-anchored at 500 ms (375 ms still fails 1/10, 450 ms passes; 500 ms is the floor + margin). |
| 500 ms | MMIO-W | host → BAR0 | **Disarm boot-time IRQ masks** (hyp-N) — clear `IMASK_HOST` + per-channel SRC/DEST enables before steady-state IRQ flow |
| 500 ms | RPC | host → CORE CPU | **`CHANGE_STATUS(RESET)`** opcode `0x25`; params: state=0, app=0xff, batch_size=0, batch_count=0 — reset context-switch state machine |
| | RPC-resp | CORE CPU → host | major=0 minor=0; signaled by MSI bit 26 |
| | RPC | host → CORE CPU | **`CLEAR_CONFIGURED_APPS`** opcode `0x47` — wipe leftover network-group state from prior runs |
| | RPC | host → CORE CPU | **`GET_HW_CONSTS`** opcode `0x48` — fw returns ~35 B (channel counts, page sizes, hw architecture) |
| | RPC ×N | host → APP CPU | **`IDENTIFY`** opcode `0x00` — settle pings; 174 B response with "Hailo-8" name + fw version |

For each RPC: host writes `[md5(16) + buffer_len(4) + payload]` to
the ATR[0]-mapped control section (BAR4+0x000 with ATR[0] retargeted
to `0x60000000`), writes `APP_CPU_MASK` (=0x1) or `CORE_CPU_MASK` to
the *fixed* FW_CONTROL doorbell at `BAR4+0x1684` (not affected by
ATR[0]), fw raises MSI bit 26 when done, host reads response from
BAR4+0x640 (still in ATR[0] window). ~ms each round-trip.

### Phase 4 — Model load (CS handshake)
*T = 0 at first CONFIG_STREAM. Total ~250-500 ms (CCW upload + 200 ms enable wait).*

| T+ | Type | Direction | Operation |
|---|---|---|---|
| 0 | RPC ×N | host → APP CPU | **`CONFIG_STREAM`** opcode `0x03` (per input/output stream); fw returns `dataflow_manager_id` for later reference |
| ~ms each | RPC ×many | host → APP CPU | **`WRITE_MEMORY`** opcode `0x01` (1 KB chunks) — upload CCW microcode parsed from HEF |
| | RPC | host → CORE CPU | **`SET_NETWORK_GROUP_HEADER`** opcode `0x20`, 32 B action-stream body (context count, batching params, latency mode) |
| | RPC | host → CORE CPU | **`SET_CONTEXT_INFO(ACTIVATION)`** opcode `0x21`, type=3, 63 B action-stream body (descriptor-list IOVAs for bnd_in + bnd_out patched at known offsets) |
| (50 ms gap†) | RPC | host → CORE CPU | **`SET_CONTEXT_INFO(BATCH_SWITCHING)`** type=2, 16 B action-stream body (hyp-Q† inter-RPC floor) |
| (50 ms gap†) | RPC | host → CORE CPU | **`SET_CONTEXT_INFO(PRELIMINARY)`** type=0, 37 B action-stream body (CCW descriptor-list IOVA patched at known offset) |
| (50 ms gap†) | RPC | host → CORE CPU | **`SET_CONTEXT_INFO(DYNAMIC)`** type=1, 103 B action-stream body (bnd_in + bnd_out descriptor-list IOVAs patched at known offsets) |
| | RPC | host → CORE CPU | **`CHANGE_STATUS(ENABLED)`** opcode `0x25`, state=1 — fw enters CS state machine and *asynchronously* arms ALL VDMA channels (ch=1, ch=2, ch=16) by walking the action streams above |
| | (busy-wait) | — | **~200 ms★** fixed delay — fragile proxy for "ch arming complete". Correct predicate would be polling ch=2 device-side `dev_base[31:16] != 0 && dev_proc != 0` — see #682 follow-up. |

Strict order enforced by fw v4.23: ACTIVATION → BATCH_SWITCHING →
PRELIMINARY → DYNAMIC. Skipping any returns
`0x4013006e UNEXPECTED_CONTEXT_ORDER`.

> **Notes on body sizes and offsets.** "Body" sizes above are
> *action-stream payload* bytes. The full FW_CONTROL request on the
> wire is larger: `[md5(16) + buffer_len(4) + parameter_count(4) +
> length-prefixes + body]`. **The IOVA-patch offsets are HEF-specific
> + ABI-specific** — they were extracted from a captured HailoRT trace
> for the MNIST test HEF on fw v4.23. A different HEF or fw version
> can move them. Either re-derive from a trace or parse the action
> stream symbolically; do not hard-code without re-validating.

> **No host-side channel-start MMIO.** This is not a missing step —
> SLM-OS's production path (`inference_device_hailo.c:2218`) explicitly
> does NOT call `hailo_vdma_channel_start()` for any channel during
> inference. fw arms ch=1, ch=2, and ch=16 itself by interpreting the
> action streams in `SET_CONTEXT_INFO`. The older `hailo_infer.c` path
> calls channel_start, but it isn't on the production code path.

### Phase 5 — Inference dispatch
*T = 0 at first `num_avail` write. Total = inference latency (model dependent).*

| T+ | Type | Direction / Target | Operation |
|---|---|---|---|
| 0 | MMIO-W RMW | BAR2+0x20+0x00 (ch=1 host sub-block BASE_DWORD) | **Bump num_avail=1 on ch=1 (CCW upload, H2D)** — signal "one descriptor ready" by writing the high 16 bits of BASE_DWORD; fw should fetch CCW microcode |
| ~ms | DMA-pull | device → host RAM | Device fetches CCW buffer (256 B - few KB) via descriptor IOVA in ch=1's bound desc list |
| ~ms | IRQ | MIP1 → GIC SPI | **ch=1 SRC complete** (data=0x01 = num descriptors processed) |
| | MMIO-W RMW | BAR2+0x200+0x10 (ch=16 host sub-block BASE_DWORD) | **Pre-arm output ch=16 (D2H)** — descriptor staged; transfer fires when fw produces output |
| | MMIO-W RMW | BAR2+0x40+0x00 (ch=2 host sub-block BASE_DWORD) | **Submit input on ch=2 (boundary IN, H2D)** — fw should fetch input and feed NN core. **#682 wedge fires here** |
| ~ms★ | DMA-pull | device → host RAM | Device reads input tensor (784 B for MNIST) via descriptor IOVA |
| (model lat.) | (internal NN) | — | Model inference (matmul, activations, etc.) — ~ms-100s ms depending on model |
| (model lat.) | DMA-push | device → host RAM | Device writes output tensor to ch=16's bound buffer |
| | IRQ | MIP1 → GIC SPI | **ch=2 SRC + ch=16 DEST completions** |
| | MMIO-R | ch=2 + ch=16 host sub-block NUM_PROC_DWORD | **Read incremented num_proc counters** — confirm completion + ack IRQ |

**Wedged path:** ch=2 `NUM_AVAIL` write returns success but fw never
advances `NUM_PROC` and never fetches the input. No IRQ fires. Host
times out after 5 s (ushim) or kernel timeout (production).

---

## 2. System boot through PCIe link-up

### 1.1 BCM2712 controller bringup

Implemented in `kernel/drivers/pcie/pcie_bcm2712.c`.

The Pi 5 firmware (closed-source, in EEPROM) does NOT train `pcie1` —
SLM-OS has to do it. This is what the Linux kernel's
`drivers/pci/controller/dwc/pcie-brcmstb.c` handles on Pi OS, and
SLM-OS is a rough port of the same sequence.

Two functions are involved:

- `bcm2712_init()` (lines 939-967) is the top-level entry. It
  resolves MMIO regions, calls `bcm2712_train_link()`, and
  finishes with MIP1 host/VPU mask programming.
- `bcm2712_train_link()` (lines 739-933) is the heavy lifter that
  does the reset → rescal → MDIO → RC bridge → wait-link-up flow.

Sequence (inside `bcm2712_train_link()`):

1. **Reset toggle** — uses `reset_w32()` / `rescal_r32()` helpers
   (defined at lines 370-378). Asserts then deasserts bridge SW
   reset ID 43, then polls `RESCAL_STATUS` (`RESCAL_BASE = 0x1000119500`,
   defined at lines 215-219) for completion.

2. **MDIO PLL programming** — walks the 7-entry `mdio_pll_tune[]`
   table (defined at line 179) and writes each `(regad, val)` pair
   via the MDIO interface at `PCIE1_MDIO_*` (0x1100-0x1108). Loop
   at line 469.

3. **Program RC bridge** (`bcm2712_program_rc_bridge()`, lines 702-722):
   - Vendor endian mode
   - Class code (PCI bridge)
   - `HARD_DEBUG` register CLKREQ bits
   - **RC_BAR1/2/3** — these define the *inbound* DMA window the
     endpoint can target. SCB0 (RC_BAR2) sizing matters: SLM-OS
     uses `log2(64 GB) - 15 = 0x15` (64 GB span).
   - QoS forwarding setup (VDM credit programming — hyp-T from the
     #682 investigation)

4. **Wait for link up** (`wait_link_up()`, lines 509-525) — polls
   `MISC_STATUS` (offset 0x4068) bits 4-5 for `PHY_LINKUP` +
   `DL_ACTIVE`. Times out at ~100 ms.

5. **Outbound windows** programmed via `bcm2712_map_bar()`
   (lines 1064-1135):
   - Non-prefetchable: PCIe `0x00_80000000` → CPU `0x1b_80000000`
     (2 GB, BAR0/4 land here)
   - Prefetchable 64-bit: PCIe `0x04_00000000` → CPU
     `0x18_00000000` (14 GB, larger BARs go here if any)

6. **Inbound window offset:** any host physical address used by
   the device for DMA gets `0x1000000000` added. Defined as
   `PCIE1_DMA_OFFSET` in `hailo_pi5.c:272-300`.

### 1.2 MSI controller setup (MIP1)

Same file, lines 1142-1340.

The Pi 5 has a separate "MSI Interrupt Peripheral" (MIP1) at
`0x1000131000` that aggregates incoming MSIs and translates them
to GIC SPIs.

- 8 MSI vectors total, mapped to GIC SPIs `247..254`
- `bcm2712_alloc_msi()` (lines 1184-1240) — allocates a contiguous
  group of vectors and programs the endpoint's MSI capability
  (`ADDR_LO/HI/DATA`) to point back at MIP1
- `bcm2712_bind_irq_handler()` (lines 1280-1340) — registers a
  per-vector trampoline with `gic_register_handler()`, then
  unmasks via `gic_enable_irq()`

This whole subsystem must be live before the Hailo can deliver any
interrupt to the host. MIP1 supports up to 8 vectors, but the Hailo
endpoint MSI capability is configured single-vector (`multi_en=0`,
`multi_cap=0` in the EP cap) — interrupt sources (per-channel SRC,
per-channel DEST, FW_CONTROL response, FW notification, etc.) are
demultiplexed by reading the chip's `ISTATUS_HOST` aggregate
register. See §8 for the demux logic.

### 1.3 PCIe device enumeration

`kernel/drivers/pcie/pcie_core.c:64-400+` walks the bus, decodes
config space, and builds a device table. `pcie_find_device()` is
the lookup the Hailo platform shim uses by `(vendor, device)` ID.

---

## 3. Hailo platform shim init

Implemented in `kernel/ai_accel/hailo/hailo_pi5.c`. This is the
glue between the generic Hailo driver core and the Pi 5 PCIe stack.

### 2.1 Discovery & enable (`pi5_init()`, lines 49-193)

1. **Find the device** (line 57-58):
   ```c
   pcie_find_device(HAILO_PCI_VENDOR_ID = 0x1E60,
                    HAILO_PCI_DEVICE_HAILO8 = 0x2864);
   ```

2. **Enable bus mastering** (line 70):
   `pcie_enable_bus_master(hailo_pcidev)` — sets memory-space and
   `BUS_MASTER` bits in the device's PCIe command register. Required
   before the endpoint can issue DMA reads/writes.

3. **Disable ASPM L0s** (lines 72-172) — walks the PCIe Express
   capability chain to clear `LNKCTL` bit 0. Empirically, leaving
   ASPM L0s on caused link recovery storms during heavy VDMA traffic.

4. **PCIe geometry audit** (lines 116-135, audit F-03 added 2026-04-24)
   — logs MPS (Max Payload Size), MRRS (Max Read Request Size), and
   negotiated link speed/width. Diagnostic only; helpful for "is the
   link actually Gen2 x1?" sanity checks.

5. **Map BARs** (lines 177-191):
   ```
   BAR0 (HAILO_BAR_CONFIG=0)    → device config + ATR[0..3]
   BAR2 (HAILO_BAR_VDMA=2)      → per-VDMA-channel registers
   BAR4 (HAILO_BAR_FW_ACCESS=4) → ATR[0]-mapped 4 KB SRAM window
   ```
   All three are mapped Device-nGnRnE (no caching, no gathering,
   strongly ordered).

### 2.2 Platform ops vtable

`hailo_pi5.c` registers itself with `hailo_core` by populating
the `hailo_platform_ops` struct defined in `hailo.h:193-268`. Key
ops:

| Op | Implementation | Purpose |
|---|---|---|
| `read32(bar, off)` | `pi5_read32` | MMIO read with bounds check |
| `write32(bar, off, val)` | `pi5_write32` | MMIO write |
| `bar4_write(off, src, len)` | `pi5_bar4_write` | Multi-word fw upload, DSB SY at end |
| `bar4_read(off, dst, len)` | `pi5_bar4_read` | Multi-word fw region read |
| `dma_alloc(size, align)` | PMM-backed | DMA buffer alloc, low-physical bias for inbound window |
| `dma_free(...)` | PMM-backed | Release |
| `cache_clean(va, len)` | `cache.h` | DC CIVAC + DSB before device read |
| `cache_invalidate(va, len)` | `cache.h` | DC CIVAC + DSB before host read |
| `mb()` | `dsb sy` | Full memory barrier |
| `udelay(us)` | per-platform | Polling delay |
| `register_irq(vec, handler)` | MIP1 path | Wire MSI → handler |

### 2.3 BAR4 multi-word access (lines 239-270)

The fw upload path needs to write potentially hundreds of KB of
firmware code through a 4 KB ATR window. The two helpers are:

- `pi5_bar4_write()` (239-254): dword-aligned writes, **DSB SY barrier
  at line 253** to flush write buffer before returning. Unaligned
  writes are rejected outright.
- `pi5_bar4_read()` (256-270): dword-aligned reads, alignment enforced.

These are the lowest-level primitive that the firmware-upload chunker
in `hailo_core.c` builds on top of.

---

## 4. Probe and firmware boot

Implemented in `kernel/ai_accel/hailo/hailo_core.c`. The state
machine is `UNINIT → PROBED → FIRMWARE_ARMED → BOOTING → RUNNING`
(lines 65-66, 283-297).

### 3.1 ATR[0] window management (lines 105-144) — the foundational primitive

The Hailo-8 has a 4 KB SRAM window mapped at BAR4. The window's
*device-side* target is set by ATR[0]'s `trsl_addr_lo/hi` registers
in BAR0. To read/write any Hailo-internal address, the host:

1. Programs ATR[0].trsl_addr to the target page
2. Reads/writes via BAR4 + (target_addr & 0xFFF)

```c
atr0_set_target(target_dev_addr);    // programs ATR[0]
val = read32(BAR4, target_dev_addr & 0xFFF);
```

**Concurrency:** `atr0_lock` (spinlock, line 76) protects ATR[0]
from concurrent retargeting. Acquired with IRQs disabled because the
fw-control IRQ handler also reads through ATR[0]. `atr0_save()` /
`atr0_restore()` (lines 127-144) wrap nested accesses.

### 3.2 Device-side helpers (lines 152-265)

All built on top of ATR[0]:

- `dev_read32(addr)` / `dev_write32(addr, val)` — single u32
- `dev_read(addr, dst, len)` / `dev_write(addr, src, len)` —
  multi-byte, automatically chunks across 4 KB boundaries
- **`dev_write_chunked()` (238-265)** — the firmware-upload primitive.
  Splits an arbitrarily-sized buffer across 4 KB ATR pages, handles
  head and tail misalignment, and is the sole producer of writes into
  the Hailo's code RAM during fw upload.

### 3.3 Probe (`hailo_probe()`, lines 313-425)

First contact with the device:

1. Read **boot status** at device address `0xE0000` via ATR[0]+BAR4.
   This is a smoke test — if the read returns `0xFFFFFFFF` or hangs,
   either PCIe link isn't really up, BARs aren't mapped, or the chip
   is in a bad reset state.
2. Read **vendor ID** from BAR0 offset `0x0098` (`HAILO_REG_VENDOR`).
3. State transitions `UNINIT → PROBED`.

### 3.4 Firmware validation (`hailo_validate_firmware()`, lines 454-658)

The fw blob is a multi-section image: app FW + boot key cert + boot
content cert + core FW. Each section has a header with:

- Magic (`0x1DD89DE0`)
- Header version
- Major / minor / revision
- Code size

Validation checks magic/version + bounds, parses out section
offsets, and stages everything for upload. Helper: `hailo_decode_cert()`
and `hailo_decode_core_fw()` (declarations in `hailo_internal.h`).

### 3.5 Boot sequence (`hailo_boot()`, lines 678-1000)

This is the biggest single function in the driver. Flow:

1. **Lines 702-800: Validate sections** — re-runs the validator
   pass and pulls section pointers/sizes into local state.

2. **Lines 808-850: Upload app firmware** to
   `app_fw_code_ram_base = 0x60000` via `dev_write_chunked()`.

3. **Lines 852-885: Upload boot key cert** to `boot_key_cert =
   0xE0048` and **boot content cert** to `boot_cont_cert = 0xE0390`.

4. **Lines 887-915: Upload core firmware** to `core_code_ram_base =
   0xC0000`.

5. **Line 933: Trigger doorbell.** Write `HAILO_FW_TRIGGER_VALUE = 0x1`
   to `raise_ready_offset = BAR4 + 0x1684`. This signals to the
   chip's boot ROM that the host is done uploading and the chip
   should branch to the loaded firmware.

6. **Lines 945-1000: Boot status polling.** Poll
   `ATR[1].trsl_addr_lo` for `HAILO_ATR1_FW_LOADED_MAGIC = 0x00200000`.
   ~1 second timeout.

7. **Post-BOOT_IRQ settle (hyp-O, 2026-05-09):** Once the BOOT_IRQ
   fires and the magic appears, **wait an additional 500 ms before
   the first FW_CONTROL RPC**. Empirically, this suppresses a
   `SAGE1_ISP` ECC that ~60% of boots otherwise hit during the first
   `CLEAR_CONFIGURED_APPS`. With the settle, ECC rate is 0/10 boots,
   matching Linux's hailo_pci behavior. The exact mechanism inside
   the chip is not characterized — fw-internal SRAM zero-init,
   ECC scrubber completion, fw task scheduling, and quiescing of
   in-flight fabric reads are all plausible. **Runtime ECC is not
   fixed by this delay** — even a 2 s settle leaves the inference-
   time wedge unchanged. Bisect-anchored at 500 ms (375 ms still
   fails 1/10, 450 ms passes; 500 ms is floor + margin).

   ```c
   // hailo_core.c:910 (rationale comments at 870-909)
   hailo_platform->udelay(500000u); /* 500 ms (bisect-anchored) */
   ```

State transitions `BOOTING → RUNNING` after successful boot poll.

---

## 5. Post-boot host setup

Once `hailo_boot()` returns success, fw is alive on the APP and CORE
CPUs and the chip is ready for RPCs.

### 4.1 IRQ disarm (hyp-N, hailo_control_disarm_irq_masks, lines 1038-1060)

During boot, the host arms a temporary set of IRQ masks (hyp-K) so
that the BOOT_IRQ delivers cleanly. Once boot is confirmed, those
masks are *disarmed* before the first real RPC — otherwise spurious
IRQs from the running fw can collide with the host's RPC poll loop.

The disarm sequence:
1. Clear `IMASK_HOST` (BAR0 + 0x0190)
2. Clear per-channel SRC (`BAR0 + 0x400`) and DEST (`BAR0 + 0x500`)
   IRQ enables that were set during arm
3. ISTATUS_HOST W1C of any pending bits

### 4.2 Initial RPC handshake

Three RPCs fire before any model load is attempted, in this exact
order. All implemented in `hailo_control.c`:

1. **CHANGE_STATUS(RESET)** (`hailo_control_change_context_switch_status`,
   lines 832-875) — opcode `0x25`, target = CORE CPU. Resets the
   context-switch state machine.

2. **CLEAR_CONFIGURED_APPS** (`hailo_control_context_switch_clear_configured_apps`,
   lines 878-898) — opcode `0x47`. Wipes any leftover network-group
   state from prior runs.

3. **GET_HW_CONSTS** (`hailo_control_get_hw_consts`, lines 901-950) —
   opcode `0x48`. Reads back fw-side constants (channel counts, page
   sizes, etc). Required as a pre-configure handshake (hailo_core_cpu).

### 4.3 Settle pings (hyp-M follow-on)

After GET_HW_CONSTS returns, the host fires a small number of
**APP-CPU IDENTIFY pings** (`hailo_control_identify`, lines 452-478,
opcode `0x00`) before the first model-configuration RPC. This is a
cheap cache-warm / liveness check; the production MNIST path issues
no-op pings while ctxsmoke issues partial pings. Memory of why it
matters: PR #682 (2026-05-06) wired it in as cheap insurance after
observing CORE-CPU response asymmetry on some boots.

### 4.4 IDENTIFY (post-boot self-check)

`hailo_control_identify()` (opcode `0x00`) returns a 174-byte
response containing the device name (`"Hailo-8"`), fw version, and
hw architecture. SLM-OS uses this as the final liveness check before
declaring the chip ready for inference.

---

## 6. Model load (CS handshake)

This is what the inference backend's `hailo_backend_load_model()`
(in `inference_device_hailo.c`, line 1902+) drives. Conceptually,
fw needs to know:

- The network-group's overall layout (IO streams, latency profile)
- The detailed per-context action list (what to do when entering each
  context)
- The compiled CCW microcode for the network

### 5.1 VDMA setup

For each input/output stream, allocate a descriptor list and a
contiguous DMA buffer:

1. **Allocate descriptor list** — `hailo_vdma_desc_list_alloc()`
   (`hailo_vdma.c:142-145`). Power-of-2 desc count, 64 KB-aligned
   backing buffer (`HAILO_VDMA_DESC_LIST_ALIGN`). Returned: host VA,
   device IOVA, and a desc-list handle.

2. **Allocate data buffer** — PMM-backed via `dma_alloc_low()` (low
   physical bias to fit the inbound aperture).

3. **Program descriptors** — `hailo_vdma_program_descriptor()`
   (`hailo_vdma.h:196-199`) populates each descriptor's `page_size`,
   `dma_address` (64-byte aligned), and `data_id` field. The list is
   bound to a specific VDMA channel.

4. **Cache clean** — `cache_clean_range()` over the descriptor list
   *before* arming the channel; otherwise the device reads stale
   descriptors.

### 5.2 CONFIG_STREAM RPC

`hailo_control_config_stream_pcie()` (lines 573-621), opcode `0x03`,
APP CPU. Issued once per stream. Hands fw the channel index +
descriptor-list IOVA + page geometry. Fw returns a
`dataflow_manager_id` that identifies this stream within the
network-group. Stash this — later RPCs reference it.

### 5.3 CCW upload

`hailo_control_upload_ccw()` (lines 624-701) — chains
`WRITE_MEMORY` (opcode `0x01`) RPCs at 1 KB granules to upload the
compiled NN microcode parsed out of the HEF. Memory range is
fw-managed; host just hands it bytes.

### 5.4 Network-group header

`hailo_control_set_network_group_header()` (lines 704-753), opcode
`0x20`, CORE CPU. Sends a 32-byte fixed-format header describing the
network-group: number of contexts, batching params, latency mode.
The exact byte layout for the test path is captured in
`host-tools/hailo-ushim/main.c` as `NG_HEADER_BODY`.

### 5.5 SET_CONTEXT_INFO ×4 (the strict order)

`hailo_control_set_context_info()` (lines 756-829), opcode `0x21`.
Fw v4.23 enforces a strict order: **ACTIVATION → BATCH_SWITCHING →
PRELIMINARY → DYNAMIC**. Skipping any returns
`0x4013006e UNEXPECTED_CONTEXT_ORDER`.

Each context's body is a packed action stream: byte-level format
defined under `hailo_control.c:1436-1447` (the
`hailo_cs_set_ctx_info_req_prefix_wire` struct + chunked body).
Action types include "configure stream", "execute CCW", "wait for
event", etc. Bodies are large enough to need chunking — the chunk
ceiling is `HAILO_CS_CONTEXT_CHUNK_MAX_BYTES = 1461` (line 590 of
`hailo_control.h`).

The body bytes contain **descriptor-list IOVAs** at known offsets,
patched in at runtime once the VDMA setup completes:

- ACTIVATION: bnd_out IOVA, bnd_in IOVA
- PRELIMINARY: CCW IOVA
- DYNAMIC: bnd_out IOVA, bnd_in IOVA

(See `host-tools/hailo-ushim/main.c` `ACTIVATION_OFFSET_*`,
`PRELIMINARY_OFFSET_*`, `DYNAMIC_OFFSET_*` for the offsets used in
the ctxsmoke reproducer.)

### 5.6 CHANGE_STATUS(ENABLED)

`hailo_control_change_context_switch_status()` again, opcode `0x25`,
CORE CPU, with `state = ENABLED (1)`. This signals fw to enter the
context-switch state machine and arm boundary channels for inference.

After this RPC, fw asynchronously walks the context-switch action
streams and writes per-channel CSRs. SLM-OS's host-side code waits
**~200 ms** for this to complete before issuing the first
`LAUNCH_TRANSFER`.

---

## 7. Inference dispatch

Driven by `inference_device_hailo.c::hailo_backend_run()`
(line 2093+). For each input tensor:

### 6.1 CCW upload kick (ch=1, H2D)

If the network needs CCW reload (typical for first inference on a
freshly-loaded model), program ch=1's descriptors to point at the
CCW buffer and bump `num_avail`:

```c
// per-channel registers in BAR2, channel base = chan_idx * 0x20
write32(BAR2, chan_base + NUM_AVAIL_OFFSET, 1);  // doorbell
```

Fw sees the `num_avail` change, fetches the CCW descriptor, DMAs the
microcode, and bumps `num_proc`. Host polls `num_proc` (or waits on
the SRC IRQ for ch=1) with a timeout.

### 6.2 Pre-arm output (ch=16, D2H)

Same dance for ch=16: program descriptors to point at the output
buffer, bump `num_avail`. Fw doesn't *complete* this transfer yet —
it stages the descriptor so it'll execute as soon as the model
produces output.

### 6.3 Submit input (ch=2, H2D) — **this is where #682 wedges**

Write `num_avail` on ch=2. In a healthy run, fw:

1. Reads ch=2's descriptors via DMA
2. Reads the input tensor at the descriptor's IOVA
3. Feeds it to the NN core
4. Completion signal propagates to ch=16 (output DMA out)
5. Both channels' `num_proc` advance, IRQs fire

In the wedged path, fw never advances `num_proc` on ch=2. The
device-side `dev_base[31:16]` stays `0x0000` (HailoRT's working
trace shows `0x001f`), and `dev_proc` stays `0x00000000` (HailoRT:
`0x00020002`). See `docs/archive/investigations/hailo-support-ticket-draft.md` for the
full investigation; the wedge is below the host/driver boundary.

### 6.4 Wait + read result

`hailo_vdma_submit_and_wait()` (`hailo_vdma.c:332+`) — polls
`num_proc` against a deadline, or sleeps on the channel's IRQ if MSI
is enabled.

After completion: `cache_invalidate_range()` over the output buffer
(device wrote it; host's L1/L2 may have stale contents) before
reading.

---

## 8. Interrupt flow

Hailo can deliver IRQs through three paths, all routed via MIP1 →
GIC:

> Note on names. This section uses `ISTATUS_HOST` / `IMASK_HOST` as
> shortened forms for readability. The actual macro names in
> `hailo_control.c` / `hailo_shell.c` are `HAILO_BCS_ISTATUS_HOST`
> and `HAILO_BSC_IMASK_HOST` (yes, the BCS/BSC swap is in the
> source, not a typo here).

| IRQ source | ISTATUS_HOST bit | Where handled |
|---|---|---|
| Per-channel SRC (H2D) | bits 0..7 | `control_msi_handler` lines 200-217, also boundary path's wait |
| Per-channel DEST (D2H) | bits 8..15 | same — bits 8-15, dispatched by channel |
| FW_CONTROL response | bit 26 (`0x04 << 24`) | `control_msi_handler` line 228 sets `control_msi_pending` |
| FW notification | bit 25 | Currently polled via D2H drain; not wired to a dedicated handler |
| Driver-down ACK | bit 27 | Drainage during teardown |

The shared MSI handler is `control_msi_handler` (`hailo_control.c:158-230`):

```c
1. Read HAILO_BCS_ISTATUS_HOST (BAR0 + 0x018C)
2. If aggregate channel bits set:
     read per-channel SRC at BAR0+0x400
     read per-channel DEST at BAR0+0x500
     W1C those (writes clear)
3. If FW_CONTROL bit set:
     set control_msi_pending = true
4. W1C the aggregate HAILO_BCS_ISTATUS_HOST
```

The FW_CONTROL response path is polled cooperatively in
`wait_for_response()` (`hailo_control.c:241-280`) — it watches
either the `control_msi_pending` flag or polls ISTATUS directly,
with ~100 µs intervals.

---

## 9. Cache & coherency

The Hailo accesses host memory through PCIe DMA. SLM-OS pages
allocated for DMA are mapped Inner Shareable, but the BCM2712's
PCIe RC is **not** I/O-coherent — host caches are NOT snooped on
DMA. So the driver does explicit cache maintenance:

| Operation | When | Why |
|---|---|---|
| `cache_clean_range(buf, len)` | Before submitting H2D transfer | Ensure CPU writes are visible to device |
| `cache_invalidate_range(buf, len)` | After D2H completion | Discard stale CPU cache; force re-fetch from DRAM |
| `dsb sy` | After MMIO config writes | Order MMIO vs. subsequent ops |
| `dsb ishst` | After page-table writes | Inner-shareable store barrier before TLB invalidate |

**Specific call sites:**

- `hailo_pi5.c:253` — DSB SY after every BAR4 multi-word fw upload
- `hailo_vdma.h:79-92` — clean after host writes descriptor fields,
  invalidate before reading device-written `status`
- `inference_device_hailo.c` — buffer prep before submit; post-
  completion invalidate before reading tensor output

The cache primitives themselves live in `kernel/include/cache.h`
(`cache_clean()`, `cache_clean_range()`, `cache_invalidate()`,
`cache_invalidate_range()`). ARM64 implementation: `DC CIVAC, <addr>`
per cache line + final DSB SY. Lines are 64 B on Cortex-A76;
`cache_clean_range` walks at that granule.

---

## 10. Cross-cutting: ATR window management

ATR (Address Translation Region) is the mechanism by which the
Hailo's BAR4 (a 4 KB host-visible window) maps to *any* address
within the chip's internal SRAM. There are 4 ATR slots:

| ATR | Use |
|---|---|
| 0 | Movable host-driven window (`atr0_set_target`); used for fw upload, control RPC, dev_read/write |
| 1 | After BOOT_IRQ, fw writes `0x00200000` magic to `trsl_addr_lo` to signal boot complete |
| 2-3 | Reserved / unused by SLM-OS path |

The ATR programming registers live in BAR0 starting at
`HAILO_ATR_BASE = 0x0700`. Each slot has its own
`trsl_addr_lo/trsl_addr_hi` registers.

**Critical invariant:** ATR[0] is shared between the IRQ handler
and the RPC path. All accesses go through `atr0_lock` (held with
IRQs disabled). Violating this gives sporadic boot-time corruption
that's very hard to reproduce.

For the FW_CONTROL RPC, ATR[0] is retargeted to
`HAILO_CONTROL_SECTION_ADDR_H8 = 0x60000000` for the duration of
the call (`hailo_control.c:314`), then optionally restored.

---

## 11. Cross-cutting: address spaces

Three address spaces exist simultaneously and need to stay
consistent:

```
Host VA          (kernel virtual, MMU-managed)
Host PA          (physical RAM, page-aligned)
PCIe IOVA        (what the device sees)

Host PA → IOVA:  add 0x1000000000 (PCIE1_DMA_OFFSET)
                 [BCM2712 PCIe1 inbound window]
IOVA → device:   PLDA bridge does no further translation;
                 the IOVA bits go straight to the endpoint
```

Every IOVA embedded in a SET_CONTEXT_INFO body must be the
**descriptor-list IOVA** (returned by `hailo_vdma_desc_list_alloc()`
or its userspace counterpart `HAILO_DESC_LIST_CREATE`), not a raw
buffer IOVA. Fw expects to walk a descriptor list at that address.

---

## 12. Tests

`kernel/tests/test_hailo.c` (~8200 lines) is the offline unit-test
harness. It mocks BAR0/2/4 memory regions and a fake fw state machine
that:

- Echoes RPC requests back as success responses
- Simulates BOOT_IRQ on demand
- Tracks per-channel mock register state
- Counts MMIO ops for observability

Coverage:
- FW header / cert / core validation (good and malformed inputs)
- Boot sequence (upload → trigger → poll → settle)
- Control transport (wire format, MD5 check, sequence number, MSI vs poll)
- Error paths (timeout, truncation, bad headers)

`kernel/tests/test_pcie.c` covers the BCM2712 enumeration / BAR
decode in isolation.

`kernel/tests/test_inference_device.c` covers the inference vtable
without a real Hailo backend (uses a stub backend).

---

## 13. Key file index

| File | Lines | Purpose |
|---|---|---|
| `kernel/drivers/pcie/pcie_bcm2712.h` | 34 | RC bridge diagnostic API decl |
| `kernel/drivers/pcie/pcie_bcm2712.c` | 1406 | PCIe1 controller bringup, link training, MSI, BAR mapping |
| `kernel/drivers/pcie/pcie_core.c` | ~700 | Generic PCIe enumeration, `pcie_find_device()` |
| `kernel/include/pcie.h` | — | Public PCIe API declarations |
| `kernel/ai_accel/hailo/hailo_pi5.c` | 760 | Pi 5 platform shim: discovery, BAR maps, DMA alloc, BAR4 chunker |
| `kernel/ai_accel/hailo/hailo_core.c` | 1059 | Probe, validate fw, boot, ATR mgmt, dev_read/write, state machine |
| `kernel/ai_accel/hailo/hailo_control.h` | 896 | RPC wire format, opcodes, struct layouts |
| `kernel/ai_accel/hailo/hailo_control.c` | 2272 | RPC transport, MSI handler, all CS RPC implementations |
| `kernel/ai_accel/hailo/hailo_vdma.h` | 426 | Descriptor format, channel API, cache contract |
| `kernel/ai_accel/hailo/hailo_vdma.c` | 892 | Descriptor list alloc, channel arm/start/stop |
| `kernel/ai_accel/hailo/hailo.h` | 349 | Public API, BAR indices, fw addresses, platform ops vtable |
| `kernel/ai_accel/hailo/hailo_internal.h` | 130 | Cert / core fw decode helpers |
| `kernel/inference/inference_device_hailo.c` | 2692 | Inference backend: load_model, run, channel programming |
| `kernel/include/cache.h` | — | DC CIVAC + DSB primitives shared across drivers |
| `kernel/tests/test_hailo.c` | 8199 | Unit tests with mock BAR regions + mock fw |
| `host-tools/hailo-ushim/` | — | Linux userspace harness that replays the same RPC + VDMA sequence against `/dev/hailo0` for byte-level A/B testing |

---

## 14. Selected device-side addresses (cheat sheet)

| Address | Purpose |
|---|---|
| `0x0098` (BAR0) | Vendor ID register (probe) |
| `0x018C` (BAR0) | `ISTATUS_HOST` — IRQ aggregate status |
| `0x0190` (BAR0) | `IMASK_HOST` |
| `0x0400` (BAR0) | Per-channel SRC IRQ enable/status |
| `0x0500` (BAR0) | Per-channel DEST IRQ enable/status |
| `0x0700+` (BAR0) | ATR[0..3] programming registers |
| `0x0640` (BAR4) | FW_CONTROL response landing zone |
| `0x1684` (BAR4) | `raise_ready_offset` — fw boot trigger doorbell |
| `0x60000` (device) | App fw code RAM base |
| `0xA0000` (device) | Core fw header |
| `0xC0000` (device) | Core fw code RAM base |
| `0xE0000` (device) | Boot status |
| `0xE0030` (device) | Boot fw header |
| `0xE0048` (device) | Boot key cert |
| `0xE0390` (device) | Boot content cert |
| `0xE0980` (device) | Trigger address |
| `0x60000000` (device) | `HAILO_CONTROL_SECTION_ADDR_H8` (RPC ATR[0] target) |

Per-channel VDMA register block (BAR2, channel_base = `chan_idx × 0x20`).
Each channel owns 32 bytes split into a HOST sub-block and a DEVICE
sub-block (16 B each). **The order swaps by direction** — see
`hailo_vdma.h:244-280`:

| Channel direction | HOST sub-block | DEVICE sub-block |
|---|---|---|
| H2D (chan 0..15)  | channel_base + `0x00..0x0F` | channel_base + `0x10..0x1F` |
| D2H (chan 16..31) | channel_base + `0x10..0x1F` | channel_base + `0x00..0x0F` |

Both sub-blocks have **identical layout** within their 16 B:

| Offset | Field | Bits | Description |
|---|---|---|---|
| `+0x00` | `BASE_DWORD` | `[31:16]` num_avail | host doorbell ("descriptors I'm handing fw") |
| | | `[14:11]` depth | log2(desc_count) |
| | | `[10:8]` data_id | per-channel id from `CONFIG_STREAM` |
| | | `[7:0]` CONTROL | 0=START, 1=STOP, 2=ABORT_PAUSE |
| `+0x04` | `NUM_PROC_DWORD` | `[15:0]` num_proc | completion counter (fw advances) |
| `+0x08` | `ALIGNED_ADDR_L` | `[31:16]` address_l | low 16 of `(iova >> 16)` |
| `+0x0C` | `ADDR_H` | `[31:0]` address_h | high 32 of iova |

**For ch=2 (H2D, boundary input — the #682 wedge channel):**

- HOST sub-block at `BAR2 + (2 × 0x20) + 0x00 = BAR2+0x40..0x4F`
- DEVICE sub-block at `BAR2 + (2 × 0x20) + 0x10 = BAR2+0x50..0x5F`

The `dev_base` and `dev_proc` mismatches that anchor the wedge
investigation refer to fields in **the DEVICE sub-block at
BAR2+0x50..0x5F** — fw mirrors the channel state there after arming
via the SET_CONTEXT_INFO action stream. HailoRT's working trace shows
non-zero values; SLM-OS's wedged trace shows zeros.

**Programming sequence used by `hailo_vdma_channel_start()` (called
on host-armed channels only):**

1. RMW `ALIGNED_ADDR_L` to write `address_l` into bits `[31:16]`
2. Write `ADDR_H` with `address_h`
3. Write `BASE_DWORD` with `(depth << 11) | (data_id << 8)` — implicitly
   clears CONTROL
4. RMW `BASE_DWORD` setting CONTROL to `START`

---

## 15. Glossary of investigation hypotheses (#682)

This section catalogs the hyp-N tags scattered through commits. Most
of these are now disconfirmed; recording them so future readers don't
chase the same ghosts.

| Tag | Hypothesis | Status |
|---|---|---|
| hyp-K | Pre-fw-upload IRQ arm + MSI setup needed for clean BOOT_IRQ | **Confirmed** — implemented |
| hyp-M | 6× GET_HW_CONSTS pre-load improves stability | Disconfirmed; replaced by single + ping (hyp-O follow-on) |
| hyp-N | Post-BOOT_IRQ disarm of arm-time IRQ masks | **Confirmed** — implemented |
| hyp-O | 500 ms post-BOOT_IRQ settle eliminates SAGE1_ISP ECC | **Confirmed** — implemented |
| hyp-Q | 50 ms inter-RPC floor between SET_CONTEXT_INFO chunks | **Confirmed** — implemented |
| hyp-S | D3hot dwell between RESET and re-probe | Disconfirmed |
| hyp-T | VDM QoS programming on PCIe1 RC | **Confirmed** — implemented |
| hyp-U/V/W | Bridge-error diagnostic + SCB0 widen | **Confirmed** — implemented |
| hyp-X-1 | Pre-IN-submit IRQ drain | Disconfirmed |
| hyp-X-3 | Boundary IN ch=2 wedge investigation | **Closed exhausted — see #682 (2026-05-12) and #1001 (2026-05-28)** |
| hyp-X (#1012) | Pre-program all 32 IN ring slots (HailoRT parity) | Disconfirmed on hardware; kept as defensive wire parity |
| hyp-Y (#1012) | Clear LIRQ bit on last descriptor (HailoRT parity) | Disconfirmed on hardware; kept as defensive wire parity. Side-finding: clearing LIRQ also suppresses the spurious `event_id=0 ETHERNET_RX_ERROR` d2h notification (LIRQ side-effect, not a wedge signal). |
| dma-content-diff | Host RAM that fw reads via DMA byte-faithful to HailoRT | Disconfirmed as cause — content matches, wedge persists (PR #1010, 2026-05-27) |

The ch=2 wedge (hyp-X-3) is closed exhausted across two passes. #682
closed 2026-05-12 after the Linux-side fw_control kprobe showed
HailoRT uses fw_control only for IDENTIFY and routes primary
configuration through undocumented BAR4 mmap writes. #1001 closed
2026-05-28 after a second-pass 10-hypothesis chain plus the
DMA-content byte-diff proved every host-observable byte — MMIO wire,
DMA descriptor list, DMA CCWS payload — matches HailoRT and the
wedge still reproduces. Remaining attack surfaces are external to
host-observable state (fw-memory BAR4 inspection, Hailo support
escalation, HailoRT runtime decompilation). See
`docs/hailo-protocol-architecture.md` §"Update 2026-05-28" for the
full disconfirmation surface.

---

*Last updated: 2026-05-28. Second-pass closure of the ch=2 wedge
(#1001) appended; §15 glossary expanded with hyp-X / hyp-Y / DMA
content-diff outcomes. Original lifecycle text last revised 2026-05-10
at PR #768 (`682/ushim-bind-parity`); doc-review fixes in PR #771.*
