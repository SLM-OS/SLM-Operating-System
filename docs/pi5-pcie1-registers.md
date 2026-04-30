# Pi 5 / BCM2712 `pcie1` and MIP1 Register Reference

Phase 0 research notes for the AI HAT+ support plan
(`docs/pi5-ai-hat-plan.md`). Scope: what SLM-OS needs to read, program, or
explicitly leave alone on the external PCIe root complex (`pcie1`) and its
MSI-X peripheral (MIP1).

Primary sources — all line references are to the
`raspberrypi/linux` `rpi-6.6.y` branch; local copies live in
`../slmos-reference-cache/` as `rpi-linux-*`:

- `arch/arm64/boot/dts/broadcom/bcm2712.dtsi` —
  `../slmos-reference-cache/rpi/rpi-linux-bcm2712.dtsi`
- `arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts` —
  `../slmos-reference-cache/rpi/rpi-linux-bcm2712-rpi-5-b.dts`
- `drivers/pci/controller/pcie-brcmstb.c` —
  `../slmos-reference-cache/rpi/rpi-linux-pcie-brcmstb.c`
- `drivers/irqchip/irq-bcm2712-mip.c` —
  `../slmos-reference-cache/rpi/rpi-linux-irq-bcm2712-mip.c`
- `arch/arm/boot/dts/overlays/pciex1-compat-pi5-overlay.dts` —
  `../slmos-reference-cache/rpi/rpi-linux-pciex1-compat-pi5-overlay.dts`

---

## 1. Three BCM2712 PCIe root complexes

`bcm2712.dtsi` defines three root complexes (lines 973, 1021, 1077). Only
`pcie1` is relevant for the AI HAT+.

| Node   | Base addr        | Usage                              | Lanes | Outbound window       | MSI target  |
|--------|------------------|------------------------------------|------:|-----------------------|-------------|
| pcie0  | `0x10_00100000`  | Reserved (dev kit, typically unused)|    1 | `0x14000000-0x17ffffff` | in-RC       |
| pcie1  | `0x10_00110000`  | **External connector (AI HAT+, NVMe)**| 1 | `0x18000000-0x1bffffff` | **MIP1**    |
| pcie2  | `0x10_00120000`  | Fixed to RP1 I/O controller         |    4 | `0x1c000000-0x1fffffff` | MIP0        |

Each root complex occupies a `0x9310`-byte register region
(`reg = <0x10 0x00110000  0x0 0x9310>` at `bcm2712.dtsi:1023`).

---

## 2. `pcie1` host register layout

Driver: `drivers/pci/controller/pcie-brcmstb.c` (generic `brcm,bcm2712-pcie`
compatible, lines 1892-1898). All offsets below are **inside the `pcie1`
register block** at physical base `0x10_00110000`.

### 2.1. Control / MISC registers

| Offset   | Name                              | Width | Source (`pcie-brcmstb.c`) |
|---------:|-----------------------------------|------:|---------------------------|
| `0x0188` | `PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1` | 32 | line 42 |
| `0x043C` | `PCIE_RC_CFG_PRIV1_ID_VAL3` (class code) | 32 | line 46 |
| `0x04DC` | `PCIE_RC_CFG_PRIV1_LINK_CAPABILITY` | 32 | line 49 |
| `0x1100` | `PCIE_RC_DL_MDIO_ADDR`            | 32    | line 61 |
| `0x1104` | `PCIE_RC_DL_MDIO_WR_DATA`         | 32    | line 62 |
| `0x1108` | `PCIE_RC_DL_MDIO_RD_DATA`         | 32    | line 63 |
| `0x4008` | `PCIE_MISC_MISC_CTRL`             | 32    | line 69 |
| `0x4064` | `PCIE_MISC_PCIE_CTRL`             | 32    | line 110 |
| `0x4068` | `PCIE_MISC_PCIE_STATUS`           | 32    | line 114 — **link status, read-only** |
| `0x406C` | `PCIE_MISC_REVISION`              | 32    | line 121 |

### 2.2. Link status decode

`PCIE_MISC_PCIE_STATUS @ 0x4068` bitfields (`pcie-brcmstb.c:114-119`):

| Bit | Name (mask)                     | Meaning                       |
|----:|---------------------------------|-------------------------------|
| 4   | `PCIE_PHYLINKUP_MASK     = 0x10`| PHY link up                   |
| 5   | `PCIE_DL_ACTIVE_MASK     = 0x20`| Data link layer active        |
| 6   | `PCIE_LINK_IN_L23_MASK   = 0x40`| Link in L2/L3 low-power state |
| 6   | `PORT_MASK_2712          = 0x40`| RC port-online (note: aliases with L23 — `pcie-brcmstb.c:916-921`) |
| 7   | `PORT_MASK               = 0x80`| Generic-variant port-online   |

`brcm_pcie_link_up()` (`pcie-brcmstb.c:923-928`) reports up only when both
`PHY` and `DL_ACTIVE` are set. For SLM-OS, the read sequence is:

```c
u32 status = readl(pcie1_base + 0x4068);
bool phy_up = !!(status & 0x10);
bool dl_up  = !!(status & 0x20);
bool link_ok = phy_up && dl_up;
```

**SLM-OS inherits a trained link** — VideoCore firmware performs reset,
retrain, and link-up before Linux/SLM-OS boots. Reading the status reg is
fine; writing to it is not part of the inherited model.

### 2.3. Config-space access window (EXT_CFG)

Key pair of registers for reading PCIe config space
(`pcie-brcmstb.c:219-220, 1885-1890`):

| Offset   | Name              | Purpose                                     |
|---------:|-------------------|---------------------------------------------|
| `0x9000` | `PCIE_EXT_CFG_INDEX` | Write bus/devfn/offset selector          |
| `0x9004` | `PCIE_EXT_CFG_DATA`  | 4-byte config-space read/write window    |

Note: on the BCM2712 variant, `EXT_CFG_DATA` is at `0x9004`
(`pcie_offsets_bcm2712[]` at `pcie-brcmstb.c:1886-1887`), **not** `0x8000`
as the generic default. The driver uses a variant table to pick the right
offset.

The access protocol (`brcm_pcie_map_bus()`, `pcie-brcmstb.c:932-951`):

```c
static void __iomem *brcm_pcie_map_bus(struct pci_bus *bus,
                                       unsigned int devfn, int where)
{
    void __iomem *base = pcie->base;         // pcie1 reg base
    int idx;

    if (pci_is_root_bus(bus))
        return devfn ? NULL : base + PCIE_ECAM_REG(where);  // RC self

    if (!brcm_pcie_link_up(pcie))
        return NULL;        // early-out; config access to a dead link
                            // faults the CPU

    idx = PCIE_ECAM_OFFSET(bus->number, devfn, 0);
    writel(idx, pcie->base + PCIE_EXT_CFG_INDEX);      // 0x9000
    return base + PCIE_EXT_CFG_DATA + PCIE_ECAM_REG(where);  // 0x9004 + (where & 0xFFF)
}
```

`PCIE_ECAM_OFFSET(bus, devfn, 0)` is the standard
`(bus<<20) | (devfn<<12)` ECAM encoding. The `where` field (register
offset within the function) becomes the low 12 bits of the BAR-side
access. Width is the native MMIO access size (u8/u16/u32) — the generic
PCI layer uses `pci_generic_config_read/write` on top of this map
(`pcie-brcmstb.c:1914-1915`).

**Caveat:** the `writel(idx)` to `EXT_CFG_INDEX` is stateful. If SLM-OS
ever has two threads doing config reads concurrently, one thread's index
write will leak into the other's data read. The brcmstb driver relies on
the kernel's per-bus lock. SLM-OS needs its own spinlock around the
`(write_index, read_data)` pair.

**Second caveat:** `brcm_pcie_map_bus()` early-returns `NULL` when the
link is down — the comment at line 943 is explicit: *"An access to our
HW w/o link-up will cause a CPU Abort."* SLM-OS must check link status
before any config access, or swallow the fault.

### 2.4. BAR outbound windows

From `bcm2712.dtsi:1053-1060` (pcie1 `ranges` property):

```dts
ranges = <0x02000000 0x00 0x80000000   // 2 GB, 32-bit, non-prefetchable
          0x1b 0x80000000              // at CPU phys 0x1b_80000000
          0x00 0x80000000>,
         <0x43000000 0x04 0x00000000   // 14 GB, 64-bit, prefetchable
          0x18 0x00000000              // at CPU phys 0x18_00000000
          0x03 0x80000000>;
```

Translation:

| Region          | PCIe address        | CPU physical address      | Size  |
|-----------------|---------------------|---------------------------|------:|
| 32-bit non-pref | `0x00_80000000`     | `0x1b_80000000`           | 2 GB  |
| 64-bit pref     | `0x04_00000000`     | `0x18_00000000`           |14 GB  |

The **programmable windows** that implement this mapping are written by
Linux via `PCIE_MISC_CPU_2_PCIE_MEM_WIN0_*` at offsets `0x400c+`
(`pcie-brcmstb.c:81-138`). SLM-OS does **not** need to program these —
VideoCore firmware leaves them programmed, and
overwriting them would break the inherited state.

For SLM-OS driver work: a Hailo-8 endpoint will advertise ~MiB-sized BARs
(BAR0, BAR2, BAR4 per `hailo-driver-notes.md` §2). The endpoint's
BAR values, read from config space, will be **PCIe-side addresses** in
the `0x00_80000000+` range; SLM-OS maps them into its own MMU by adding
the PCIe-to-CPU-phys offset `(0x1b_80000000 - 0x00_80000000)` for
non-prefetchable BARs.

### 2.5. DMA inbound window

`bcm2712.dtsi:1062-1064` `dma-ranges`:

```dts
dma-ranges = <0x03000000 0x10 0x00000000
              0x00 0x00000000
              0x10 0x00000000>;
```

Translation: PCIe address `0x10_00000000` maps to CPU phys `0x00_00000000`
(start of DRAM), 64 GB span. Endpoint DMA into host RAM goes through this
window — the endpoint writes `0x10_xxxxxxxx` and the RC translates to
`0x00_xxxxxxxx` (= physical DRAM).

SLM-OS's PMM-allocated DMA buffers live in the low 4 GB of DRAM
(phys `0x00_00000000 - 0x00_FFFFFFFF`). The endpoint-side DMA address to
use for a buffer at CPU phys `P` is `P + 0x10_00000000`. This offset is
the **DMA offset** SLM-OS's PCIe driver must return from
`pcie_virt_to_dma_addr()`.

---

## 3. Interrupts: GIC SPI routing

From `bcm2712.dtsi:1036-1047`, `pcie1` itself has 6 SPIs allocated:

| SPI | Purpose                         |
|----:|---------------------------------|
| 218 | AER (advanced error reporting) — marked unused |
| 219 | INTx legacy A                   |
| 220 | INTx legacy B                   |
| 221 | INTx legacy C                   |
| 222 | INTx legacy D                   |
| 223 | `pcie` — RC's own IRQ (bus errors, hotplug) |
| 224 | `msi` — RC-internal MSI (unused when `msi-parent = <&mip1>`) |
| 225 | NMI — unused                    |
| 226 | PME — unused                    |

MSI-X interrupts for devices on `pcie1` **do not come through SPI 224** —
they route through **MIP1** (see §4). SPI 223 is the only pcie1-level SPI
SLM-OS needs to wire for RC errors (if AER is ever enabled). Legacy INTx
lines (219-222) are not used by Hailo (it's an MSI-only device), so SLM-OS
can leave them unbound for the AI HAT+.

---

## 4. MIP1 (the MSI-X peripheral for `pcie1`)

Source: `bcm2712.dtsi:1141-1153`, driver
`drivers/irqchip/irq-bcm2712-mip.c` (full file cached as
`rpi-linux-irq-bcm2712-mip.c`).

### 4.1. Base address — **correction to plan doc**

The AI HAT+ plan (`pi5-ai-hat-plan.md` §2.4) speculated MIP1 would be
parallel to MIP0's `0x1000130000`. **Actual base is
`0x1000131000`** — 4 KB *above* MIP0, not parallel to it:

```dts
mip0: msi-controller@130000 { reg = <0x10 0x00130000 0x0 0xc0>; ... };
mip1: msi-controller@131000 { reg = <0x10 0x00131000 0x0 0xc0>; ... };
```

Each MIP occupies a `0xC0`-byte register block. Register layout
(`irq-bcm2712-mip.c:15-26`):

| Offset | Name                    | Purpose                          |
|-------:|-------------------------|----------------------------------|
| `0x00` | `MIP_INT_RAISED`        | Latched raw interrupt status     |
| `0x10` | `MIP_INT_CLEARED`       | Write-to-clear pending bits      |
| `0x20` | `MIP_INT_CFGL_HOST`     | Host routing config, low 32       |
| `0x30` | `MIP_INT_CFGH_HOST`     | Host routing config, high 32      |
| `0x40` | `MIP_INT_MASKL_HOST`    | Host mask, low 32 (0 = unmasked) |
| `0x50` | `MIP_INT_MASKH_HOST`    | Host mask, high 32               |
| `0x60` | `MIP_INT_MASKL_VPU`     | VPU mask, low 32 (used to keep firmware from seeing MSIs) |
| `0x70` | `MIP_INT_MASKH_VPU`     | VPU mask, high 32                |
| `0x80` | `MIP_INT_STATUSL_HOST`  | Host pending, low 32             |
| `0x90` | `MIP_INT_STATUSH_HOST`  | Host pending, high 32            |
| `0xA0` | `MIP_INT_STATUSL_VPU`   | VPU pending, low 32              |
| `0xB0` | `MIP_INT_STATUSH_VPU`   | VPU pending, high 32             |

### 4.2. MIP1's SPI mapping — **narrow range**

From `bcm2712.dtsi:1147-1152`:

```dts
mip1: msi-controller@131000 {
    compatible = "brcm,bcm2712-mip-intc";
    reg = <0x10 0x00131000 0x0 0xc0>;
    msi-controller;
    interrupt-controller;
    #interrupt-cells = <2>;
    brcm,msi-base-spi = <247>;
    brcm,msi-num-spis = <8>;        // <- note: only 8, not 64
    brcm,msi-offset = <8>;
    brcm,msi-pci-addr = <0xff 0xffffe000>;
};
```

MIP1 is **much smaller** than MIP0 (which has 64 MSIs starting at SPI 128):

| Property          | MIP0 (internal/RP1) | MIP1 (external/AI HAT+) |
|-------------------|---------------------|-------------------------|
| Base phys addr    | `0x10_00130000`     | `0x10_00131000`         |
| `msi-base-spi`    | 128                 | **247**                 |
| `msi-num-spis`    | 64                  | **8**                   |
| `msi-offset`      | 0                   | **8**                   |
| `msi-pci-addr`    | `0xff_fffff000`     | `0xff_ffffe000`         |

**Implication for SLM-OS:** pcie1 endpoints get **at most 8 usable MSI-X
vectors**, occupying GIC SPIs **247..254**. A comment in the DTS notes
*"Actually 20 total, but the others are both sparse and non-consecutive"*
— meaning there are physically more wires, but downstream Linux only
exposes the contiguous 8. For the AI HAT+ that is not a problem — the
Hailo driver uses only a single MSI (`pci_enable_msi()` in
`hailort-drivers/linux/pcie/src/pcie.c:951` — it does not even use MSI-X).

### 4.3. MSI address/data composition

`mip_compose_msi_msg()` (`irq-bcm2712-mip.c:51-58`):

```c
msg->address_hi = upper_32_bits(priv->msg_addr);  // 0x000000FF
msg->address_lo = lower_32_bits(priv->msg_addr);  // 0xFFFFE000 for MIP1
msg->data       = d->hwirq;                        // 0..7 after subtract msi_base
```

`msg_addr` is taken from `brcm,msi-pci-addr`. For MIP1, the endpoint
writes to PCIe address **`0xFF_FFFFE000 + (data << ?)`** — actually the
data payload carries the specific MSI index; the address is a single-page
target (4 KB, per `brcm_pcie_encode_ibar_size(0x1000)` at
`pcie-brcmstb.c:2040`).

### 4.4. RC-side plumbing (done by firmware, documented for completeness)

Linux routes MIP writes into the RC by programming **RC_BAR1** on pcie1
to accept the MSI target address
(`pcie-brcmstb.c:2023-2049`):

```c
writel(lower_32_bits(msi_pci_addr) | brcm_pcie_encode_ibar_size(0x1000),
       pcie->base + PCIE_MISC_RC_BAR1_CONFIG_LO);
writel(upper_32_bits(msi_pci_addr),
       pcie->base + PCIE_MISC_RC_BAR1_CONFIG_HI);
writel(lower_32_bits(msi_phys_addr) |
       PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_ACCESS_ENABLE_MASK,
       pcie->base + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP);
writel(upper_32_bits(msi_phys_addr),
       pcie->base + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI);
```

Register offsets from `pcie-brcmstb.c`:

| Offset | Name                                   | Source |
|-------:|----------------------------------------|--------|
| `0x402C` | `PCIE_MISC_RC_BAR1_CONFIG_LO`        | line 89 |
| `0x4030` | `PCIE_MISC_RC_BAR1_CONFIG_HI`        | line 91 |
| `0x40AC` | `PCIE_MISC_UBUS_BAR1_CONFIG_REMAP`   | line 160 |
| `0x40B0` | `PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI`| line 162 |

**SLM-OS assumption:** VideoCore firmware programs these during
`pcie1` bring-up (when `dtparam=pciex1` is set). SLM-OS reads these
registers to confirm the MSI target is programmed; it does **not**
rewrite them.

### 4.5. MIP1 initialisation sequence (for SLM-OS Phase 3)

Mirrors `mip_of_msi_init()` (`irq-bcm2712-mip.c:232-322`), with unmasking
only for MIP1's 8-vector range:

```c
// Map MIP1's 0xC0-byte register block
void *mip1 = vmm_map_mmio(0x1000131000, 0xC0);

// Unmask all 8 host vectors (writes 0 = unmasked)
iowrite32(0, mip1 + MIP_INT_MASKL_HOST);   // 0x40
iowrite32(0, mip1 + MIP_INT_MASKH_HOST);   // 0x50 — upper is unused on MIP1
                                            //         but matches Linux driver

// Mask VPU side — VideoCore's not supposed to see MSIs on pcie1
iowrite32(~0, mip1 + MIP_INT_MASKL_VPU);   // 0x60
iowrite32(~0, mip1 + MIP_INT_MASKH_VPU);   // 0x70

// Edge-triggered config
iowrite32(~0, mip1 + MIP_INT_CFGL_HOST);   // 0x20
iowrite32(~0, mip1 + MIP_INT_CFGH_HOST);   // 0x30

// Wire SPIs 247..254 to GIC handlers in SLM-OS's IRQ subsystem.
```

When an endpoint's MSI-X table entry points at the MIP1 target address
(`0xFF_FFFFE000`) and data index N, the MIP latches bit N, fires GIC
SPI `247 + 8 + N` (`msi_base + msi_offset + hwirq`), and the handler
runs in SLM-OS's GIC path.

To acknowledge an MSI, write the bit into `MIP_INT_CLEARED` (0x10). This
is the pattern the upstream Linux MIP driver uses (implicitly, via the
GIC EOI).

---

## 5. Reset and training — DO NOT TOUCH

These registers **are** under firmware control and SLM-OS must leave them
alone. Listed only so the review path can flag unauthorised writes.

| Offset   | Name                                | Do-not-touch rationale |
|---------:|-------------------------------------|------------------------|
| `0x9210` | `PCIE_RGR1_SW_INIT_1` (RGR reset)   | VideoCore firmware asserts/deasserts this during link training. Rewriting it will drop the link — not recoverable from SLM-OS without TF-A help. |
| `0x4064` | `PCIE_MISC_PCIE_CTRL` `[2] PERSTB`  | PERST# to the endpoint. Toggling resets the AI HAT+ entirely; firmware owns PERST sequencing. |
| `0x4008` | `PCIE_MISC_MISC_CTRL`               | Endian, burst, RCB config — set by firmware at correct values per VideoCore's MPS/MRRS negotiation. |
| resets = `<&bcm_reset 7>, <&bcm_reset 43>, <&pcie_rescal>` (from `bcm2712.dtsi:1048`) — these reset domains are wired into the CPR / BCM reset controller at `0x10_00000000+`. SLM-OS has no reset-controller driver and must not try to drive these. |

The pcie_rescal (`bcm2712.dtsi:1069-1073`) is a shared block between all
three pcie RCs; even a "just toggling pcie1" reset will glitch pcie2/RP1
if done wrong. Leave it to firmware.

---

## 6. Enable check — Pi 5 firmware / `config.txt`

`bcm2712-rpi-5-b.dts:173-175`:

```dts
&pcie1 {
    brcm,vdm-qos-map = <0x33333333>;
};
```

That is the **only** mention of pcie1 in the Pi 5 board DTS. It does
**not** set `status = "okay"`. Setting `status = "okay"` happens via the
`dtparam=pciex1` parameter in `config.txt`, which the downstream DTB
preprocessor applies.

**Consequence:** if `config.txt` does not have `dtparam=pciex1`, VideoCore
skips pcie1 bring-up entirely:

- `pcie1` register block still readable (it's a static SoC block).
- Link status at `0x4068` reports `0x00` (no PHY, no DL).
- `EXT_CFG_INDEX` writes are ignored; reads return stale data.
- Config-space access to any endpoint aborts.

The Pi 5 AI HAT+ plan's Phase 0 smoke-test must confirm
`dtparam=pciex1` is present and a link trains. The overlay
`pciex1-compat-pi5-overlay.dts` provides three switches:

- `pciex1=l1ss` — enable L1 sub-state low-power
- `pciex1=no-l0s` — disable ASPM L0s
- `pciex1=no-mip` — force MSIs through RC-internal vector instead of
  MIP1 (SPI 224 instead of 247..254)

For SLM-OS, **the default (MIP1 routing, MSIs enabled, no ASPM overrides)
is what Phase 3+ expects**. If `pciex1=no-mip` is set, the MSI path in
Phase 3 would need to target SPI 224 instead. Worth probing at runtime
by reading pcie1's `msi-parent` equivalent from `PCIE_MISC_RC_BAR1_*` —
if RC_BAR1 is programmed to `0xFF_FFFFE000`, MIP1 routing is active.

---

## 7. SLM-OS Phase 1 / 3 call-outs

### Phase 1 (PCIe host controller driver)

Required register accesses on `pcie1`:

1. Map `0x10_00110000 .. +0x9310` as MMIO.
2. Verify link: read `0x4068`, check bits 4+5.
3. ECAM walk: for each bus/devfn, write index to `0x9000`, read data from
   `0x9004 + (offset & 0xFFC)`. Needs spinlock around the pair.
4. BAR walk: standard PCIe config (type 0 header at devfn 0), discover
   endpoint's BAR values; translate PCIe addr → CPU phys addr through
   the fixed `0x00_80000000 → 0x1b_80000000` mapping (for non-prefetchable)
   or `0x04_00000000 → 0x18_00000000` (for 64-bit prefetchable).
5. Map the chosen BAR region via VMM as Device-nGnRnE.

### Phase 3 (Hailo driver)

MSI allocation path:

1. Find the endpoint's MSI capability in its config-space capability
   list (not MSI-X — see `hailo-driver-notes.md` §9.6).
2. Program the MSI message address to `0xFF_FFFFE000` (MIP1 target).
3. Program the MSI data to `0` (first MIP1 vector — Hailo uses only one).
4. Register an ISR on GIC SPI `247 + 8 + 0 = 255` (the first MIP1 vector
   after offset).
5. Unmask MIP1's vector 0: clear bit 0 in `MIP_INT_MASKL_HOST` at MIP1
   offset `0x40`.

---

## 8. Cached reference files

Under `../slmos-reference-cache/`:

- `rpi-linux-bcm2712.dtsi` — master DTSI with pcie0/1/2 + mip0/1 nodes
- `rpi-linux-bcm2712-rpi-5-b.dts` — board file; confirms pcie1 not
  enabled by default
- `rpi-linux-pcie-brcmstb.c` — host controller driver; register offsets
  + `map_bus` routine
- `rpi-linux-irq-bcm2712-mip.c` — MIP MSI interrupt controller driver;
  register layout + init
- `rpi-linux-pciex1-compat-pi5-overlay.dts` — `dtparam=pciex1` override
  options (l1ss/no-l0s/no-mip)
- `rpi-linux-pcie-32bit-dma-pi5-overlay.dts` — forces 32-bit DMA for
  endpoints that cannot address above 4 GB (e.g. some SATA bridges;
  probably not needed for Hailo)
