# Pi OS instrumented HailoRT trace findings — VDMA layer (2026-04-23)

Second-pass capture: instrumented `vdma_common.c` to also wrap direct `iowrite32` / `ioread32` (which bypass `hailo_resource_*`). The first trace was MISSING all VDMA channel-register writes (BAR2). This trace fills the gap.

Files:
- Trace: `docs/reference/hailort-v4.23.0-full-trace-yolov6n-with-vdma-pi5.txt` (5173 lines, 266 VDMA events)
- Patch update: appended VDMA-side wrapper to the existing `hailort-v4.23.0-trace-instrumentation.patch`

## Headline finding #2 (after #1 was disconfirmed)

For the **input boundary submit** (ch=2), the entire MMIO sequence between userspace `LAUNCH_TRANSFER` ioctl and the firmware's SRC_IRQ response is exactly **two reads + one write**:

```
VDMA RD32 ch=2+0x40 val=0x00007801   ← snapshot
VDMA RD32 ch=2+0x40 val=0x00007801   ← second read (consistency)
VDMA WR32 ch=2+0x40 val=0x09607801   ← num_avail=2400 RMW
                                       ↓ (microseconds later)
ISTATUS_HOST=0x00800001              ← fw fires SRC_IRQ
SOURCE_INTERRUPT_PER_CHANNEL=0x4    ← bit 2 = ch=2 fired
VDMA RD32 ch=2+0x44 val=0x09600960   ← num_proc advanced to 2400!
```

That is the **complete** wire protocol for an input submit. There's no doorbell to BAR4, no per-channel IRQ enable RMW, no second-stage trigger. SLM-OS already does the same single RMW. The problem is *not* a missing register write.

**Conclusion: the bug is below the MMIO layer.**

## Where it must be

If MMIO is identical and fw doesn't respond on SLM-OS, fw either:
- Cannot DMA-read the descriptor list at the IOVA we put in the channel reg
- Reads garbage from it (cache-coherency)
- Is in some internal state that ignores the boundary submit despite identical channel reg state

## Strongest evidence for the address-range sub-hypothesis

Looking at SLM-OS chan dumps from a failed run:

| Channel | addr_l_dword | addr_h | Reconstructed IOVA | Translated host phys (via PCIe inbound) |
|---|---|---|---|---|
| CCW (ch=1) | `0x00aa0040` | `0x10` | `0x10_00aa_0000` | `0x00aa_0000` (~11 MB) |
| IN boundary (ch=2) | `0xffc50000` | `0x10` | `0x10_ffc5_0000` | `0xffc5_0000` (~4 GB − 4 MB) |

CCW load works (fw advances `num_proc` from 0 to 109 in 0 µs). Boundary submit fails (fw never moves `num_proc`). The only systematic difference is the IOVA address range: CCW is at ~11 MB, IN is at ~4 GB − 4 MB.

The BCM2712 inbound window is configured (in `pcie_bcm2712.c:732-744`) to span 64 GB on the PCIe side (`SIZE=0x15`) mapped to host phys 0..(SCB0=0x11=4 GB). On paper that means `0xffc50000` should translate fine. **But empirically only the low address works.**

## What to test next (highest leverage)

1. **Force boundary allocations from low PMM.** Modify `inference_device_hailo.c` to either pre-reserve a low-PMM region for boundary tensors+desc-lists, or scan PMM for low-addressed allocations. If fw starts responding on the boundary submit, the address-range hypothesis is proven.

2. **Audit TF-A's top-of-RAM reservations on Pi 5.** TF-A may carve out the top of physical RAM in a way that's reachable via CPU but not via PCIe inbound. Linux's `pcie-brcmstb.c` likely declares `dma-ranges` that constrain DMA-allocator output to the safe window — SLM-OS doesn't model this.

3. **Read the DT `dma-ranges` for pcie1.** The Linux device tree's pcie1 node has `dma-ranges` describing what physical-address windows are reachable. If it excludes 0xFC000000+ (or whatever high range), that's the constraint we're missing.

4. **Capture the actual bytes at the boundary IN desc list IOVA via fw mailbox** — if HailoRT exposes a "device-side memory peek" mechanism for debug, we could ask fw to read its view of `0x10_FFC5_0000` and report what it sees. Probably not exposed; falls back to (1).

## Status of the negative-result branch

`phase-8-incremental-num-avail` (uncommitted) has:
- The incremental num_avail loop (HailoRT-mirroring; doesn't fix the bug but no harm)
- Two findings docs (`hailort-trace-findings-2026-04-22.md`, this file)
- The trace files (`hailort-v4.23.0-full-trace-yolov6n*.txt`)
- Updated instrumentation patch (`hailort-v4.23.0-trace-instrumentation.patch`)

Either commit as a research-pass branch (preserves the trace + findings), or keep work local while testing hypothesis #2 (address range).
