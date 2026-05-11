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
| **BDF** | PCI Bus/Device/Function tuple identifying an endpoint in config space. On Pi 5 the Hailo-8L enumerates as `01:00.0` (bus 1, dev 0, fn 0). Used in this doc as `BDF 01:00.0 + 0xNN` to mean "config-space offset NN on the Hailo endpoint", reached via ECAM at `0x1F00000000 + (1<<20) + 0xNN` on BCM2712. |
| **VDMA** | Vendor DMA engine on the Hailo. Channels are per-direction (H2D vs D2H), each programmed via descriptor lists. |
| **CS / context-switch** | Hailo's network-group / context-switch protocol layered over FW_CONTROL RPCs. |
| **MIP1** | Pi 5 PCIe1 MSI peripheral at `0x1000131000`. Translates 8 MSI vectors to GIC SPIs `247..254`. |
| **PM cap / PMCSR** | PCI Power Management capability (cap_id `0x01`) in the endpoint's config-space cap chain. PMCSR (Power Management Control/Status Register) is a 16-bit register at `PM_cap + 0x04`. Per PCI spec §5.4.1, D3hot→D0 needs ≥10 ms before the device responds to config cycles. **PMCSR bit layout (PCI PM 1.2 §3.2.4):** `[1:0]` PowerState (RW; D0=00, D3hot=11), `[2]` Reserved (RO 0), `[3]` NoSoftReset (RO; hardware-defined), `[7:4]` Reserved (RO 0), `[8]` PME_En (RW), `[12:9]` Data_Select (RW), `[14:13]` Data_Scale (RO), `[15]` PME_Status (RW1C). SLM-OS only touches bits[1:0] via RMW; all other RW bits are preserved. |
| **ch=1 / ch=2 / ch=16** | The three VDMA channels the inference path uses: CCW upload (1, H2D), boundary input (2, H2D), boundary output (16, D2H). |

---

## 1. External interaction timeline

This section is what the boundary between SLM-OS and the outside world
sees. Every row is *one* MMIO transaction, IRQ, RPC round-trip,
doorbell, or DMA event — internal function calls are out of scope.

**Legend — mechanism column values:**

| Mechanism | Direction | Description |
|---|---|---|
| MMIO-W | host → device | 32-bit write to a BAR offset |
| MMIO-R | host → device | 32-bit read returning a value |
| MMIO-W RMW | host → device | Read-modify-write a sub-field |
| MMIO-R poll | host → device | Repeated reads until a condition is met |
| PCI-cfg-W | host → device | Config-space write (ECAM) |
| PCI-cfg-W RMW | host → device | Config-space read-modify-write of a sub-field |
| PCI-cfg-R | host → device | Config-space read (ECAM) |
| RPC | host → fw | Body in `BAR4+0x000` (ATR[0] → `0x60000000`), doorbell at fixed `BAR4+0x1684` |
| RPC-resp | fw → host | Body in `BAR4+0x640`, signaled by MSI bit 26 |
| IRQ | device → host | MSI to MIP1 → GIC SPI 247..254 |
| DMA-pull | device → host RAM | Endpoint reads through inbound aperture |
| DMA-push | device → host RAM | Endpoint writes through inbound aperture |
| busy-wait | (none) | `udelay` — host blocks for a fixed period; no transactions |

**Column schema — each row is one transaction:**

| Column | Content |
|---|---|
| # | Step number within the phase. Cross-reference as "Phase 3 step 10". |
| Purpose (lingo) | What we'd say in conversation: "Load firmware", "Set D3hot", "Submit input on ch=2". |
| Mechanism | One of the values in the legend table above. |
| Target | Concrete address. Formats: `BAR<n> + 0xOFF` (MMIO), `BDF 01:00.0 + 0xOFF` (PCI config), `dev 0xADDR` (device-side via ATR[0]), `SPI <n>` (IRQ), `RPC <opcode-name>` (FW_CONTROL RPC name). |
| Payload (lingo → bits) | Both: `"Set D3hot" → PMCSR bits[1:0] = 0b11 (0x0003)`. |
| Expected response | Both: `"PMCSR readback in D3hot" → bits[1:0] == 0b11`. |

**Timing markers** (appear as suffix on phase totals and individual steps):

- † = calibrated value (post-bisect, do not change without re-measuring)
- ‡ = observed bound (timeout ceiling or measured upper)
- ★ = best estimate, not characterized
- (no mark) = sequential / negligible / fast (µs-order)

---

### Phase 1 — PCIe link-up + BAR mapping
*Total ~50-150 ms. SLM-OS owns this — Pi 5 firmware does not train `pcie1`.*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Reset the PCIe1 controller (clean slate) | MMIO-W ×2 | `RESCAL_BASE+0x00` (bridge ID 43) | "Assert SW reset" → set bit 0; then "Deassert" → clear bit 0 | None (writes); status visible at step 2 |
| 2 | Wait for reset to complete | MMIO-R poll | `RESCAL_BASE+0x08` | "Poll status bit 0" | Bit 0 → 1 within ~1 ms★ |
| 3 | Program SerDes PHY PLL for 54 MHz xosc refclk | MMIO-W ×7 | `PCIE1_MDIO_*` (0x1100-0x1108) | "MDIO PLL tune table" → 7 `(regad, val)` pairs from `mdio_pll_tune[]` | None; PLL locked before downstream writes |
| 4 | Tell RC bridge it's a host (vendor endian, PCI bridge class, CLKREQ, VDM QoS — hyp-T) | MMIO-W batch | `PCIE1_BASE` config regs | Vendor endian, class=PCI bridge, RC_BAR2 size=`0x15` (64 GB inbound aperture), HARD_DEBUG CLKREQ bits, VDM credit programming | None (config writes) |
| 5 | Wait for PCIe link train | MMIO-R poll | `PCIE1_MISC_STATUS = 0x4068` | "Poll PHY_LINKUP + DL_ACTIVE" → bits 4,5 | Both bits set within ≤100 ms‡ |
| 6 | Read endpoint vendor/device ID (enumeration + smoke test) | PCI-cfg-R | `BDF 01:00.0 + 0x00` | "Read 32-bit ID register" | `0x2864_1E60` (Hailo-8 device/vendor) |
| 7 | Enable bus mastering + memory space on endpoint | PCI-cfg-W RMW | `BDF 01:00.0 + 0x04` (Command register) | "Set BUS_MASTER + MEM_SPACE, preserve other bits" → `new = old \| 0x0006` (bit 1 = MEM_SPACE, bit 2 = BUS_MASTER) | Endpoint may now DMA + respond to memory cycles |
| 8 | Disable ASPM L0s (avoid link-recovery storms under VDMA) | PCI-cfg-W RMW | `BDF 01:00.0 + PCIe Express cap + 0x10` (LNKCTL) | "Clear ASPM L0s, preserve other LNKCTL bits" → `new = old & ~0x0001` (bit 0 = ASPM_Control L0s enable) | LNKCTL readback bit 0 == 0 |
| 9 | Aim endpoint MSIs at MIP1 | PCI-cfg-W ×3 | `BDF 01:00.0 + MSI cap + 0x04/0x08/0x0C` | MSI_ADDR_LO, MSI_ADDR_HI, MSI_DATA → MIP1 doorbell address | Endpoint sends MSI writes to MIP1 from here on |
| 10 | Unmask MIP1 host vectors, mask VPU, edge-triggered, clear pending | MMIO-W ×4 | `MIP1 (0x1000131000)` | `MASKL_HOST=0x00000000`, `MASKL_VPU=0xFFFFFFFF`, `CFGL=0xFFFFFFFF` (edge), `CLEAR=0xFF` | MIP1 forwards future MSIs to GIC SPIs 247..254 |
| 11 | Map outbound CPU-addr → PCIe-addr windows for BAR0/2/4 | MMIO-W ×3 | BCM2712 outbound window programming regs | Per-BAR translation (PCIe `0x00_80000000` → CPU `0x1b_80000000` non-prefetch; PCIe `0x04_00000000` → CPU `0x18_00000000` prefetch) | Host CPU may now reach endpoint BARs |

---

### Phase 2 — Firmware boot
*Total ~150-1000 ms (dominated by fw upload).*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Aim BAR4 window at the boot-status region | MMIO-W | `BAR0 + 0x0700` (ATR[0].trsl_addr_lo, `HAILO_ATR_BASE`) | "Retarget ATR[0] to device 0xE0000" → `trsl_addr = 0x000E0000` | None (write-only) |
| 2 | Read boot-status register (smoke test fw is alive in ROM) | MMIO-R | `BAR4 + 0x000` (= dev `0xE0000` via ATR[0]) | "Read boot status word" | `HAILO_BOOT_STATUS_UNINIT=0x1` or `HAILO_BOOT_STATUS_IN_BOOTLOADER=0x2`; NOT `0xFFFFFFFF` (poison) |
| 3 | Read endpoint vendor ID (second smoke test) | MMIO-R | `BAR0 + 0x0098` (`HAILO_REG_VENDOR`) | "Read 32-bit vendor reg" | Non-zero, non-poison |
| 4 | Upload **app firmware** to device `0x60000` | MMIO-W ×N | `BAR4 + ATR[0]` window (retargeted per 4 KB page across dev `0x60000..`) | "App fw bytes" → chunked dwords (~50-200 ms★ total) | None |
| 5 | Upload **boot key cert** to device `0xE0048` | MMIO-W ×N | `BAR4 + ATR[0]` window (target `0xE0048`) | "Key cert bytes" (~10 ms★) | None |
| 6 | Upload **boot content cert** to device `0xE0390` | MMIO-W ×N | `BAR4 + ATR[0]` window (target `0xE0390`) | "Content cert bytes" (~10 ms★) | None |
| 7 | Upload **core firmware** to device `0xC0000` | MMIO-W ×N | `BAR4 + ATR[0]` window (target `0xC0000..`) | "Core fw bytes" → chunked dwords (~50-200 ms★) | None |
| 8 | Tell the boot ROM the upload is done — go run fw | MMIO-W | dev `0xE0980` (= `BAR4 + 0x980` after ATR[0] retarget to `0xE0000`) | "Boot trigger" → `0x00000001` (`HAILO_FW_TRIGGER_VALUE`) | None on this register; chip branches to loaded fw |
| 9 | Wait for fw "boot complete" magic | MMIO-R poll | `BAR0 + 0x0700 + ATR1 stride` (ATR[1].trsl_addr_lo) | "Poll for fw-written magic" | `0x00200000` (`HAILO_ATR1_FW_LOADED_MAGIC`) within ≤1000 ms‡ |
| 10 | Receive BOOT_IRQ (fw signals "running") | IRQ | MIP1 → `SPI 247..254` (one) | n/a (passive wait) | One MSI; ISR ACKs MIP1 latch + reads ISTATUS |

> **Two doorbells to keep separate.** Step 8's boot trigger writes to
> *device address* `0xE0980` — reached by retargeting ATR[0] so device
> page `0xE0000` lands at BAR4's low 4 KB, then writing at
> `BAR4 + 0x980`. The FW_CONTROL doorbell used in Phase 3+ lives at
> *fixed* `BAR4 + 0x1684` (`HAILO_FW_TRIGGER_OFFSET`), outside the ATR
> window — that register does not move when ATR[0] is retargeted.

---

### Phase 3 — Post-boot setup
*Total ~510-550 ms boot path + ~20 ms PMCSR cycle (gated by `HAILO_D3HOT_AT_BOOT`).*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Clear boot-time host IRQ mask | MMIO-W | `BAR0 + 0x0190` (IMASK_HOST) | "Disarm IMASK" → `0x00000000` | Future ISTATUS reads return only intentional events |
| 2 | Clear per-channel SRC enables set during boot arm | MMIO-W | `BAR0 + 0x0400` (per-ch SRC enable) | "Disarm SRC" → `0x00000000` for boot-set bits | Steady-state SRC IRQ flow |
| 3 | Clear per-channel DEST enables set during boot arm | MMIO-W | `BAR0 + 0x0500` (per-ch DEST enable) | "Disarm DEST" → `0x00000000` for boot-set bits | Steady-state DEST IRQ flow |
| 4 | W1C any pending ISTATUS bits | MMIO-W | `BAR0 + 0x018C` (ISTATUS_HOST) | "Clear all pending" → write-1-to-clear | ISTATUS readback == 0 |
| 5 | **500 ms settle (hyp-O†)** — let fw finish SAGE1_ISP zero-init | busy-wait | (no MMIO) | "udelay 500000 µs" | Suppresses ~60% CPU_ECC rate on first RPC. Bisect-anchored: 375 ms fails 1/10, 450 ms passes, 500 ms = floor+margin. |
| 6 | Reset CS state machine | RPC | `CORE CPU` (mask `0x2` → `BAR4+0x1684`); body @ `BAR4+0x000` (ATR[0]→`0x60000000`) | `CHANGE_STATUS(RESET)` op `0x25`; params: state=0, app=0xFF, batch_size=0, batch_count=0 | Response @ `BAR4+0x640`; MSI bit 26; status major=0 minor=0 |
| 7 | Wipe stale network-group state from prior runs | RPC | `CORE CPU` | `CLEAR_CONFIGURED_APPS` op `0x47`; no params | Status OK; MSI bit 26 |
| 8 | Read fw-side hw constants (channel counts, page sizes, arch) | RPC | `CORE CPU` | `GET_HW_CONSTS` op `0x48`; no params | ~35 B response body; MSI bit 26 |
| 9 | Liveness ping (cache warm) | RPC ×N | `APP CPU` (mask `0x1` → `BAR4+0x1684`) | `IDENTIFY` op `0x00`; no params | 174 B response: "Hailo-8" name, fw version, hw arch |
| 10 | Bounce device to D3hot (mirror Linux probe→idle window) | PCI-cfg-W RMW | `BDF 01:00.0 + PM_cap + 0x04` (PMCSR; `PM_cap` from `pcie_find_capability(0x01)`) | "Set D3hot, preserve other bits" → `new = (old & ~0x0003) \| 0x0003`. Only PowerState bits[1:0] change; PME_En/Data_Select (RW) preserved; NoSoftReset/Data_Scale (RO) unaffected; PME_Status (RW1C) written-back-as-read. | PMCSR readback bits[1:0] == `0b11`; chip stays config-responsive |
| 11 | Let D-state transition settle | busy-wait | (no MMIO) | "udelay 10000 µs" (PCI spec §5.4.1) | Quiet bus |
| 12 | Return device to D0 for normal operation | PCI-cfg-W RMW | `BDF 01:00.0 + PM_cap + 0x04` (PMCSR) | "Set D0, preserve other bits" → `new = (old & ~0x0003) \| 0x0000`. Same RMW pattern as step 10. | PMCSR readback bits[1:0] == `0b00` |
| 13 | Wait for D0 to settle (D3hot→D0 needs ≥10 ms per PCI spec) | busy-wait | (no MMIO) | "udelay 10000 µs" | Device fully responsive to MMIO |

> **Phase 3 steps 10-13 are gated by `HAILO_D3HOT_AT_BOOT` (default
> ON).** Disabling skips them — fw stays in D0 throughout boot.
> Empirically the cycle shifts the CPU_ECC trigger out of the load
> and pre-submit drain paths into the boundary-submit attempt
> (#253 testing). Position must be END of boot: #682 hyp-N2
> (cycling BEFORE IDENTIFY) was disconfirmed — it escalates
> SAGE1_ISP from correctable CPU_ECC_ERROR (event_id=7) to fatal
> CPU_ECC_FATAL (event_id=8). #682 hyp-S (1 s D3hot dwell instead
> of 10 ms) was disconfirmed too — wedge persists, ECCs aggravate.

---

### Phase 4 — Model load (CS handshake)
*Total ~250-500 ms (CCW upload dominates + 200 ms post-ENABLED wait).*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Tell fw the I/O stream's channel + descriptor list + page geometry (one per input/output stream) | RPC ×N | `APP CPU` | `CONFIG_STREAM` op `0x03`; body: channel index, desc-list IOVA, page count, page size | Response carries `dataflow_manager_id` — stash for later RPC references |
| 2 | Upload CCW microcode parsed from HEF (1 KB chunks) | RPC ×many | `APP CPU` | `WRITE_MEMORY` op `0x01`; body: dest addr, length, payload | Status OK each (~ms each round-trip) |
| 3 | Send network-group header | RPC | `CORE CPU` | `SET_NETWORK_GROUP_HEADER` op `0x20`; 32 B action-stream body (context count, batching params, latency mode) | Status OK |
| 4 | Send context info (ACTIVATION) — bnd_in + bnd_out IOVAs patched at known offsets | RPC | `CORE CPU` | `SET_CONTEXT_INFO(ACTIVATION)` op `0x21`, type=3; 63 B action-stream body | Status OK |
| 5 | 50 ms inter-RPC floor (hyp-Q†) before next SET_CONTEXT_INFO | busy-wait | (no MMIO) | "udelay 50000 µs" | Fw absorbs context info without backpressure |
| 6 | Send context info (BATCH_SWITCHING) | RPC | `CORE CPU` | `SET_CONTEXT_INFO(BATCH_SWITCHING)` op `0x21`, type=2; 16 B body | Status OK |
| 7 | 50 ms inter-RPC floor (hyp-Q†) | busy-wait | (no MMIO) | "udelay 50000 µs" | — |
| 8 | Send context info (PRELIMINARY) — CCW desc-list IOVA patched at known offset | RPC | `CORE CPU` | `SET_CONTEXT_INFO(PRELIMINARY)` op `0x21`, type=0; 37 B body | Status OK |
| 9 | 50 ms inter-RPC floor (hyp-Q†) | busy-wait | (no MMIO) | "udelay 50000 µs" | — |
| 10 | Send context info (DYNAMIC) — bnd_in + bnd_out IOVAs patched at known offsets | RPC | `CORE CPU` | `SET_CONTEXT_INFO(DYNAMIC)` op `0x21`, type=1; 103 B body | Status OK |
| 11 | Flip fw into the CS state machine — fw asynchronously arms ch=1, ch=2, ch=16 by walking the action streams | RPC | `CORE CPU` | `CHANGE_STATUS(ENABLED)` op `0x25`; param: state=1 (ENABLED) | Status OK; fw walks action streams in background |
| 12 | Wait for channel arming to complete (fragile fixed delay) | busy-wait | (no MMIO) | "udelay 200000 µs ★" | Channels armed device-side. Correct predicate would poll ch=2 device sub-block `dev_base[31:16] != 0 && dev_proc != 0` — see #682 follow-up. |

> **Strict order enforced by fw v4.23:** ACTIVATION → BATCH_SWITCHING
> → PRELIMINARY → DYNAMIC. Skipping any returns
> `0x4013006e UNEXPECTED_CONTEXT_ORDER`.

> **Body sizes are action-stream payload bytes.** The full FW_CONTROL
> request on the wire is larger: `[md5(16) + buffer_len(4) +
> parameter_count(4) + length-prefixes + body]`. **IOVA-patch
> offsets are HEF-specific + ABI-specific** — extracted from a
> captured HailoRT trace for the MNIST HEF on fw v4.23. A different
> HEF or fw version may move them.

> **No host-side channel-start MMIO.** SLM-OS's production path
> (`inference_device_hailo.c:2218`) does NOT call
> `hailo_vdma_channel_start()` for any inference channel. fw arms
> ch=1, ch=2, ch=16 itself via the action streams above. The older
> `hailo_infer.c` path does call channel_start but is not on the
> production code path.

---

### Phase 5 — Inference dispatch
*Total = inference latency (model-dependent: ms to 100s of ms).*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Bump `num_avail=1` on ch=1 (CCW upload, H2D) | MMIO-W RMW | `BAR2 + (1×0x20) + 0x00 = BAR2+0x20` (ch=1 HOST sub-block BASE_DWORD) | "One descriptor ready" → set bits[31:16] (num_avail) to 1 | Fw fetches CCW descriptor in background |
| 2 | Device pulls CCW buffer via descriptor IOVA | DMA-pull | (host RAM @ ch=1 desc IOVA) | (256 B - few KB depending on model) | Data arrives at fw side; ch=1 SRC IRQ pending |
| 3 | Receive ch=1 SRC completion IRQ | IRQ | MIP1 → `SPI 247..254` | n/a (passive); ISR reads ISTATUS aggregate then per-ch SRC at `BAR0+0x0400` | bit 0..7 in ISTATUS asserted; ack via W1C |
| 4 | Pre-arm output ch=16 (D2H) | MMIO-W RMW | `BAR2 + (16×0x20) + 0x10 = BAR2+0x210` (ch=16 D2H sub-block; D2H HOST sub-block is at `+0x10`) | "Descriptor staged for output" → set bits[31:16] (num_avail) | Fw stages but does not yet fire transfer |
| 5 | **Submit input on ch=2 (boundary IN, H2D) — #682 wedge fires here** | MMIO-W RMW | `BAR2 + (2×0x20) + 0x00 = BAR2+0x40` (ch=2 HOST sub-block BASE_DWORD) | "Input descriptor ready" → set bits[31:16] (num_avail) to 1 | **Healthy:** fw fetches input, advances `dev_proc`. **Wedged:** write returns success, `dev_base[31:16]` stays `0x0000` (HailoRT trace shows `0x001f`), `dev_proc` stays `0x00000000` (HailoRT: `0x00020002`), no IRQ fires. |
| 6 | Device pulls input tensor via descriptor IOVA (healthy path only) | DMA-pull | (host RAM @ ch=2 desc IOVA) | "Input tensor" (784 B for MNIST) | Data at fw side; NN core begins inference |
| 7 | Model inference (matmul, activations, etc.) | (internal NN) | — | (no host visibility) | Output produced; ch=16 DMA fires |
| 8 | Device pushes output tensor to ch=16's bound buffer | DMA-push | (host RAM @ ch=16 desc IOVA) | "Output tensor" | Buffer written; ch=2 SRC + ch=16 DEST IRQs pending |
| 9 | Receive ch=2 SRC + ch=16 DEST completion IRQs | IRQ | MIP1 → `SPI 247..254` | n/a; ISR reads ISTATUS then per-ch SRC + DEST registers | bits 2 + 24 (8+16) in ISTATUS; ack via W1C |
| 10 | Read incremented `num_proc` to confirm completion | MMIO-R | `BAR2 + 0x44` (ch=2 NUM_PROC_DWORD) and `BAR2 + 0x214` (ch=16 NUM_PROC_DWORD) | "Read num_proc bits[15:0]" | Both incremented; host invalidates cache lines over ch=16 buffer before reading output |

> **Wedged path:** ch=2 NUM_AVAIL write returns success but fw never
> advances NUM_PROC and never fetches the input. No IRQ fires. Host
> times out after 5 s (ushim) or kernel timeout (production). See
> `docs/hailo-support-ticket-draft.md` for the full investigation.

---

### Phase 6 — Teardown
*Triggered by shell `reboot` (cmd_reboot → `hailo_control_signal_driver_shutdown`).*

| # | Purpose (lingo) | Mechanism | Target | Payload (lingo → bits) | Expected response |
|---|---|---|---|---|---|
| 1 | Tell fw the driver is going away — park gracefully | MMIO-W | `BAR4 + 0x1684` (`raise_ready_offset`) | "Driver shutdown signal" → `0x00000004` (`HAILO_FW_ACCESS_DRIVER_SHUTDOWN_MASK`) | No reply expected. Fw may assert ISTATUS bit 27 (driver-down ACK) — currently drained but not acted on. |

> SLM-OS has no in-tree "soft reset" path. If the chip wedges
> (state machine in `HAILO_STATE_FAILED` after a `hailo_boot`
> failure, or ATR[0] poisoned by a probe-then-reboot sequence),
> only a full SoC reboot recovers. See §1.7.

---

### 1.6 Driver state machine

The driver tracks one state variable (`state` in `hailo_core.c:65`,
type `enum hailo_state` in `hailo.h:277-284`):

| Value | Name | Meaning | How entered |
|---|---|---|---|
| `0` | `UNINIT` | Platform vtable not installed, or platform init failed | Default at boot; `hailo_init()` failure (`hailo_core.c:297`) |
| `1` | `PROBED` | IDs read, BARs mapped, ATR[0] usable | `hailo_probe()` success (`hailo_core.c:358`) |
| `2` | `FIRMWARE_ARMED` | FW image validated + uploaded, trigger doorbell not yet raised | `hailo_boot()` partway through (`hailo_core.c:732`) |
| `3` | `BOOTING` | Trigger raised, polling for ATR[1] magic | `hailo_boot()` (`hailo_core.c:788`) |
| `4` | `RUNNING` | FW alive, control channel live, RPCs OK | End of `hailo_boot()` (`hailo_core.c:919`) |
| `0xFF` | `FAILED` | Terminal error — `hailo_probe` and `hailo_boot` both refuse further work | Any `hailo_boot()` early-return error path (`hailo_core.c:689, 697, 703, 720, 728, 1037`) |

Transitions only ever advance — there is no `RUNNING → PROBED` or
`FAILED → anything`. The shell's `hailo probe` from a `RUNNING`
state does NOT reset the state; it short-circuits because the
device is already initialized.

### 1.7 Recovery from FAILED — only a full reboot

SLM-OS has no `hailo_reset` / `hailo_recover` entry point. Once
`state = HAILO_STATE_FAILED` is set, the only path back to RUNNING
is a full kernel boot. This matters in two practical scenarios:

1. **Boot failure mid-upload.** A `hailo_boot()` that fails at
   firmware validation, upload, trigger, or poll latches FAILED.
   Re-issuing `hailo boot` from the shell sees the FAILED guard
   at `hailo_core.c:680` and returns `HAILO_ERR_NODEV`.

2. **Probe-after-running-then-failed.** A `hailo probe` against a
   chip whose fw is already running advances the state machine
   back to PROBED; a subsequent `hailo boot` then refuses because
   `boot_status` reads `0xFFFFFFFF` (the boot ROM is no longer
   the active code path — running fw owns the register). That
   refusal sets FAILED. Empirically this also poisons ATR[0]
   such that subsequent `hailo probe` reads return NODEV. Only
   reboot recovers.

The driver does program a `hailo_control_signal_driver_shutdown`
(`0x4` to `BAR4+0x1684`) from `cmd_reboot` (Phase 6) to give fw
a chance to park, but the chip side of that signal is not
characterized as a true reset — fw continues running, just with
an awareness that the host is going down.

### 1.8 Shell command surface

`hailo <subcmd>` — full list in `kernel/ai_accel/hailo/hailo_shell.c`.
Each line is one `strcmp(argv[1], …)` site.

| Subcommand | Purpose | Line |
|---|---|---|
| `probe` | Re-run `hailo_probe`; advances UNINIT → PROBED | 656 |
| `cfgdump` | Raw 64-byte bus-1 PCI config-space hex dump | 671 |
| `boot` | Upload embedded fw, trigger boot, poll for magic, settle, handshake | 693 |
| `load <path> [upload <B>\|sched]` | Read a HEF, dump header, optionally upload CCWs or arm AI scheduler | 726 |
| `fw` | Print fw version (post-boot only) | 1085 |
| `peek <addr> [N]` | READ_MEMORY N bytes (≤64) at device-side `addr`; hex-dump | 1097 |
| `poke <addr> <val>` | WRITE_MEMORY a single u32 at device-side `addr` (little-endian) | 1131 |
| `cfgstream <dir> <ch>` | Issue CONFIG_STREAM probe; report `dataflow_manager_id` | 1161 |
| `infer <B>` | End-to-end VDMA inference smoke test with synthetic B-byte tensor | 1210 |
| `ctxsmoke` | Replay ushim's context-switch handshake; partial pings | 1265 |
| `stage` | Stage descriptor lists for the wedge reproducer | 1273 |
| `d2h` | Drain device-to-host queue (notification consumer) | 1284 |
| `runmodel` | Full production-path inference run | 1302 |
| `edges` | List boundary channels (in/out, channel index) | 1405 |
| `edgeinfo <ch>` | Per-channel HOST + DEVICE sub-block register dump | 1462 |
| `fwlog` | Read fw log buffer as ASCII | 1483 |
| `fwloghex` | Read fw log buffer as hex | 1493 |
| `bist` | Built-in self-test (post-boot diagnostic) | 1515 |
| `last_err` | Print most recent driver error code + string | 1555 |

### 1.9 Out of scope

The following are **not** covered by this section:

- HailoRT userspace scheduling / multi-process / device-sharing
  protocols. SLM-OS owns the device single-process.
- Hailo-15 / Hailo-10 silicon. SLM-OS targets Hailo-8L only;
  some register addresses and fw RPC opcodes are shared with
  Hailo-8 (non-L) but the fw build is different.
- Multi-NPU / chained inference. SLM-OS enumerates one Hailo
  endpoint per boot.
- Hot-plug. The device is assumed present from kernel boot.
- The internal RP1 controller on `pcie0`. SLM-OS does not use
  RP1 in the Hailo path.

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
`0x00020002`). See `docs/hailo-support-ticket-draft.md` for the
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
| hyp-X-3 | Boundary IN ch=2 wedge investigation | **Open — see #682** |

The ch=2 wedge (hyp-X-3) is the current open issue. The host-side
hypotheses (missing ioctl, wrong descriptor field values, partial vs
full-ring bind, dynamic vs static bind, alternate interrupt-domain
on launch) have all been eliminated; the wedge is in fw-side ch=2
arm logic and currently sits with Hailo support.

---

*Last updated: 2026-05-10. Reflects the state of the codebase at
PR #768 (`682/ushim-bind-parity` merged into main). Doc-review fixes
in PR #771. §1 v2 rewrite (per-row schema with purpose/mechanism/
target/payload/expected; PMCSR cycle, state machine, recovery,
shell surface added) on branch `docs/hailo-lifecycle-v2`.*
