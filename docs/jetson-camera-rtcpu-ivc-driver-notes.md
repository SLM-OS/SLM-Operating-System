# Jetson Camera RTCPU (RCE) IVC Driver Notes

Code-read of the Linux-for-Tegra (L4T) `tegra-camera-rtcpu` platform driver and
its HSP+IVC transport layer, captured to scope the SLM-OS port. Companion to
`docs/jetson-camera-imx219-plan.md` (§"VI driver", §"Pre-Hardware Tasks") and
`docs/jetson-camera-vi-driver-notes.md`. The capture-protocol message wire
format is already documented via `docs/reference/l4t-camrtc-capture-messages.h`
and `l4t-camrtc-capture.h`; this document covers the **transport layer**
underneath them.

---

## Source and license

All cached files come from the OE4T mirror of L4T 5.10 at branch
`oe4t-patches-l4t-r35.6.4`
(commit tree `050b88cc9d0ffc4ace5f54ea0bc3f2b46f163486`). Headers carry
SPDX `GPL-2.0-only`; driver `.c` files carry the equivalent GPL v2 boilerplate
(no SPDX header). Per L4T convention, anything under `nvidia/` is a downstream
patch series on top of upstream `linux-tegra`.

| Cache file | Upstream path |
|---|---|
| `docs/reference/l4t-tegra-camera-rtcpu.c` | `nvidia/drivers/platform/tegra/tegra-camera-rtcpu.c` |
| `docs/reference/l4t-rtcpu-hsp-combo.c` / `.h` | `nvidia/drivers/platform/tegra/rtcpu/hsp-combo.{c,h}` |
| `docs/reference/l4t-rtcpu-hsp-mailbox-client.c` | `nvidia/drivers/platform/tegra/rtcpu/hsp-mailbox-client.c` |
| `docs/reference/l4t-rtcpu-ivc-bus.c` | `nvidia/drivers/platform/tegra/rtcpu/ivc-bus.c` |
| `docs/reference/l4t-rtcpu-tegra-rtcpu-trace.c` | `nvidia/drivers/platform/tegra/rtcpu/tegra-rtcpu-trace.c` |
| `docs/reference/l4t-rtcpu-rtcpu-monitor.c` | `nvidia/drivers/platform/tegra/rtcpu/rtcpu-monitor.c` |
| `docs/reference/l4t-rtcpu-capture-ivc-priv.h` | `nvidia/drivers/platform/tegra/rtcpu/capture-ivc-priv.h` |
| `docs/reference/l4t-rtcpu-clk-group.c` | `nvidia/drivers/platform/tegra/rtcpu/clk-group.c` |
| `docs/reference/l4t-rtcpu-reset-group.c` | `nvidia/drivers/platform/tegra/rtcpu/reset-group.c` |
| `docs/reference/l4t-rtcpu-device-group.c` | `nvidia/drivers/platform/tegra/rtcpu/device-group.c` |
| `docs/reference/l4t-rtcpu-camera-diagnostics.c` | `nvidia/drivers/platform/tegra/rtcpu/camera-diagnostics.c` |
| `docs/reference/l4t-camrtc-channels.h` | `nvidia/include/soc/tegra/camrtc-channels.h` |
| `docs/reference/l4t-camrtc-commands.h` | `nvidia/include/soc/tegra/camrtc-commands.h` |
| `docs/reference/l4t-camrtc-common.h` | `nvidia/include/soc/tegra/camrtc-common.h` |
| `docs/reference/l4t-camrtc-trace.h` | `nvidia/include/soc/tegra/camrtc-trace.h` |
| `docs/reference/l4t-camrtc-dbg-messages.h` | `nvidia/include/soc/tegra/camrtc-dbg-messages.h` |
| `docs/reference/l4t-binding-nvidia-tegra194-rce.txt` | `nvidia/Documentation/devicetree/bindings/platform/tegra/nvidia,tegra194-rce.txt` |
| `docs/reference/l4t-binding-tegra-ivc-channel.txt` | `nvidia/Documentation/devicetree/bindings/platform/tegra/tegra-ivc-channel.txt` |
| `docs/reference/l4t-binding-nvidia-tegra186-hsp.txt` | `nvidia/Documentation/devicetree/bindings/platform/tegra/nvidia,tegra186-hsp.txt` |
| `docs/reference/l4t-tegra234-camera.dtsi` | `nvidia/soc/t23x/kernel-dts/tegra234-soc/tegra234-camera.dtsi` |
| `docs/reference/l4t-tegra234-soc-base.dtsi` | `nvidia/soc/t23x/kernel-dts/tegra234-soc/tegra234-soc-base.dtsi` |

---

## Hardware overview

The Real-time Camera Engine (RCE) is a Cortex-R5F cluster inside Tegra234,
hosted under the camera complex at the same MMIO neighbourhood as VI/NVCSI/ISP.
RCE owns the VI Falcon microcode, the NVCSI stream-config flow on T234, and
the capture/notification pipeline; the AP (CCPLEX) talks to it exclusively
through HSP shared mailboxes plus shared semaphores plus IVC ring buffers in
DRAM. There is no AP-programmable VI register window on T234 — every capture
flows through RCE (see `docs/jetson-camera-vi-driver-notes.md` §"VI5 is
RTCPU-only on T234").

RCE register map per `tegra234-camera.dtsi:33-39`:

```
reg = <0 0xbc00000 0 0x1000>,    /* RCE EVP (R5 reset/exception vectors) */
      <0 0xb9f0000 0 0x40000>,   /* RCE PM (R5 power, FWLOADDONE) */
      <0 0xb840000 0 0x10000>,   /* AST-CPU (R5 address-translation) */
      <0 0xb850000 0 0x10000>;   /* AST-DMA (R5 DMA address-translation) */
```

The driver only touches `rce-pm` (`pm_base`) at runtime — to set
`TEGRA_PM_FWLOADDONE` (bit 0x2 in offset 0x40, `TEGRA_PM_R5_CTRL_0`) when
firmware load is complete and to poll `TEGRA_PM_WFIPIPESTOPPED` (bit 0x200000
in offset 0x20, `TEGRA_PM_PWR_STATUS_0`) when waiting for idle. EVP / AST are
configured by the bootloader and never written by the AP-side driver.

**RCE firmware blob.** The L4T platform driver does **not** call
`request_firmware()` — see `l4t-tegra-camera-rtcpu.c:1` (no `linux/firmware.h`
include). The RCE firmware ELF is loaded by the Tegra bootloader (CBoot →
TF-A → MB1) before Linux runs, and the AST regions are programmed at the
same time. The DT binding confirms this explicitly:
> *"When the RCE FW starts, it expects AST regions 0/1/2 are already set up
> for the RCE to access FW in DRAM, SYSRAM if applicable, and the special
> carveout used by camera hardware. By default, the memory areas and AST
> regions are set up by the Tegra bootloader."*
> (`l4t-binding-nvidia-tegra194-rce.txt:7-11`)

In a stock JetPack 6 / L4T r35 install the on-disk RCE image lives at
`/lib/firmware/nvidia/tegra234/camera-rtcpu-rce.img` — used by the bootloader,
not Linux. SLM-OS does not need to ship or load this file as long as kexec
inherits a live RCE.

---

## HSP region

The camera-rtcpu HSP controller is **separate from the BPMP HSP block**:

| Block | DT label | Base | Size | Owner |
|---|---|---|---|---|
| BPMP HSP (top1) | `hsp_top1` | `0x03d00000` | 0x000a0000 | BPMP MRQ traffic |
| Top HSP (top0) | `hsp_top` | `0x03c00000` | 0x000a0000 | CCPLEX-side doorbells (used by SLM-OS for BPMP today) |
| AON HSP | `aon_hsp` | `0x0c150000` | 0x00090000 | SPE / AON IVC |
| SCE HSP | `sce_hsp` | `0x0b150000` | 0x00090000 | SCE (safety RTCPU) |
| **RCE HSP** | **`hsp_rce`** | **`0x0b950000`** | **0x00090000** | **Camera RTCPU IVC + HSP-VM** |

Source: `l4t-tegra234-soc-base.dtsi:256-267` —

```
hsp_rce: tegra-hsp@b950000 {
    compatible = "nvidia,tegra186-hsp";
    reg = <0x0 0x0b950000 0x0 0x00090000>;
    interrupts = <0 TEGRA234_IRQ_RCE_HSP_SHARED_1 0x4>,
                 <0 TEGRA234_IRQ_RCE_HSP_SHARED_2 0x4>,
                 <0 TEGRA234_IRQ_RCE_HSP_SHARED_3 0x4>,
                 <0 TEGRA234_IRQ_RCE_HSP_SHARED_4 0x4>;
    nvidia,mbox-ie;
    #mbox-cells = <2>;
    interrupt-names = "shared1", "shared2", "shared3", "shared4";
};
```

The block follows the same `nvidia,tegra186-hsp` layout SLM-OS already drives
for BPMP — common registers + shared mailboxes + shared semaphores +
arbitrated semaphores + per-master doorbell blocks, with sizes discovered
from `HSP_DIMENSIONING` at `HSP_BASE + 0x380`. Importantly, RCE *does not*
use a doorbell block on this controller; the HSP-VM protocol uses **shared
mailbox pairs** (TX/RX) plus a **shared semaphore** for IVC group bits.
(See `l4t-tegra234-camera.dtsi:70-107` — every `hsp-vmN` node references
`SM TX(n)`, `SM RX(n+1)`, `SS n` against `hsp_rce`.)

This is the **Phase 0 recon target**. SLM-OS has not yet touched anything in
the 0x0B0_0000–0x0BF_FFFF neighborhood; verifying that NS EL2 can read
`hsp_rce` MMIO is the gate on whether the RCE-mediated VI / NVCSI Option B
path is reachable at all.

---

## IVC channel topology

RCE on Tegra234 supports up to **4 VM clients** (`hsp-vm1`..`hsp-vm4` in DT,
only `hsp-vm1` enabled by default). Each VM gets:

- 1× HSP shared-mailbox **TX** (CCPLEX → RCE, e.g. `SM TX(0)`)
- 1× HSP shared-mailbox **RX** (RCE → CCPLEX, e.g. `SM RX(1)`)
- 1× HSP shared **semaphore** (`SS 0`) — IVC group-pending bits

Both mailboxes live in the `hsp_rce` block at `0x0b950000`.

Within each VM there are multiple IVC channels in DRAM, all multiplexed into
one HSP-mailbox pair via 8-bit "channel group" bits in the shared semaphore
(`CAMRTC_HSP_SS_IVC_MASK = 0xFF`, `l4t-camrtc-commands.h:89`). The default
T234 layout (`l4t-tegra234-camera.dtsi:116-172`) has 6 IVC channels under one
group:

| DT node | Service | Group | Frames | Frame size | Bytes (rx+tx) |
|---|---|---|---|---|---|
| `echo@0` | echo | 1 | 16 | 64 | ~3 KB |
| `dbg@1` | debug (raw) | 1 | 1 | 448 | ~1.5 KB |
| `dbg@2` | debug (debugfs) | 1 | 1 | 8192 | ~17 KB |
| **`ivccontrol@3`** | **capture-control** | 1 | 64 | 320 | ~42 KB |
| **`ivccapture@4`** | **capture** | 1 | 512 | 64 | ~67 KB |
| `diag@5` | diag | 1 | 1 | 64 | ~1 KB |

The two **load-bearing channels for IMX219 capture** are `ivccontrol@3`
("capture-control", carries `CAPTURE_CHANNEL_SETUP_REQ`,
`CAPTURE_CHANNEL_RESET_REQ`, etc.) and `ivccapture@4` ("capture", carries
`CAPTURE_REQUEST_REQ` and `CAPTURE_STATUS_IND`). Wire format already
documented in `docs/reference/l4t-camrtc-capture-messages.h`. Trace and
diagnostics channels can be skipped for a minimal port.

**Region layout** (`l4t-rtcpu-ivc-bus.c:467-533`, region descriptor in
`l4t-tegra234-camera.dtsi:66`):

```
nvidia,ivc-channels = <&{/camera-ivc-channels} 2 0x90000000 0x10000>;
                                                 ^         ^
                                                 region#   reserved IOVA
```

- One DRAM region per `ivc-channels` entry (default: 1 region for all 6
  channels).
- Region buffer is `dma_alloc_coherent`-allocated by the AP, IOVA-mapped
  through RCE's SMMU stream (TEGRA_SID_NISO0_RCE in T234, see
  `tegra234-camera.dtsi:58`).
- Layout inside region:
  - First **4096 bytes** (`CAMRTC_IVC_CONFIG_SIZE`): array of
    `struct camrtc_tlv_ivc_setup` entries, one per channel, terminated
    with a zero-tag entry. RCE reads this at `CH_SETUP` time to learn
    each channel's `(rx_iova, rx_frame_size, rx_nframes, tx_iova, …,
    channel_group, ivc_service)` — defined in
    `l4t-camrtc-channels.h:51-74`.
  - Remainder: per-channel pairs of IVC ring buffers (TX + RX), each
    sized by `tegra_ivc_total_queue_size(nframes * frame_size)`.

So a minimal IMX219 deployment needs ONE region containing two channels
(`capture-control` + `capture`), totalling roughly 4 KB config + ~110 KB IVC
rings, plus rounding — comfortably under one 1 MB carveout.

The IVC ring layout itself is the same `tegra-ivc.c` ring used by BPMP MRQ
(`docs/reference/linux-tegra-ivc.c`) — head + tail + counts + payload, with
DMB / DSB ordering already implemented in SLM-OS's `kernel/drivers/bpmp/ivc.c`.

---

## Bring-up sequence

L4T's flow when Linux is the AP. Steps marked **inherited** can be skipped
by SLM-OS if it kexecs from a Linux that already had RCE fully online (see
§"Post-kexec considerations").

1. **Bootloader** (CBoot/TF-A/MB1, before Linux):
   - Load `camera-rtcpu-rce.img` into the RCE carveout in DRAM.
   - Program RCE AST regions 0/1/2 to map FW DRAM, SYSRAM, and the camera
     carveout into RCE's address space.
   - (Optionally start RCE — the driver re-asserts reset and re-deasserts
     it during probe regardless.)
2. **Linux probe entry** — `tegra_cam_rtcpu_probe`
   (`l4t-tegra-camera-rtcpu.c:1188`):
   - Match `compatible = "nvidia,tegra194-rce"` → `rce_pdata`.
   - Read `nvidia,cpu-name`, `nvidia,cmd-timeout` (default 2000 ms),
     `nvidia,max-reboot` (default 3) from DT.
3. **Get resources** — `tegra_camrtc_get_resources`
   (`l4t-tegra-camera-rtcpu.c:272`):
   - Acquire BPMP-managed clocks (`TEGRA234_CLK_RCE_CPU_NIC`,
     `TEGRA234_CLK_RCE_NIC`, `TEGRA234_CLK_RCE_CPU`) and reset
     (`TEGRA234_RESET_RCE_ALL`) via the BPMP MRQ path.
   - `ioremap` rce-pm (and historically rce-cfg/ape-amisc on other
     variants).
4. **Trace + coverage allocation** — `tegra_rtcpu_trace_create`
   / `tegra_rtcpu_coverage_create`. Allocates DMA-coherent buffers; can be
   no-ops in a minimal port (skip the trace channel and don't include
   `nvidia,trace`).
5. **HSP driver init** — `tegra_camrtc_hsp_init`
   (`l4t-tegra-camera-rtcpu.c:1094`):
   - Walk DT child `hsp` → look up the `nvidia,tegra-camrtc-hsp-vm` node.
   - Acquire `vm-tx`, `vm-rx`, `vm-ss` HSP resources from `hsp_rce`.
   - Register RX-full and TX-empty notify callbacks
     (`camrtc_hsp_rx_full_notify`, `camrtc_hsp_tx_empty_notify` in
     `l4t-rtcpu-hsp-combo.c:128-169`).
6. **Power on** — `tegra_camrtc_poweron`
   (`l4t-tegra-camera-rtcpu.c:856`):
   - Enable clocks (`tegra_camrtc_enable_clks`), bump to fast rate.
   - Deassert reset via BPMP (`tegra_rce_cam_deassert_resets`,
     `l4t-tegra-camera-rtcpu.c:736`).
   - Set `TEGRA_PM_FWLOADDONE` bit in `rce-pm + 0x40` to release the R5
     from `nCPUHALT` — this is what actually starts RCE executing.
7. **Boot sync** — `tegra_camrtc_boot_sync` → `camrtc_hsp_sync`
   (`l4t-rtcpu-hsp-combo.c:298-308`):
   - **HELLO handshake.** AP sends `CAMRTC_HSP_MSG(CAMRTC_HSP_HELLO,
     cookie)` via `tegra_hsp_sm_tx_write` on the VM-TX mailbox. Cookie is
     `sched_clock() >> 5` masked to 24 bits. Wait for an RX-full IRQ
     carrying the same `(HELLO, cookie)` payload back. (Loop discards
     stale messages until cookie matches.)
   - **PROTOCOL exchange.** AP sends `CAMRTC_HSP_MSG(CAMRTC_HSP_PROTOCOL,
     RTCPU_DRIVER_SM6_VERSION)` (`= 6`). RCE responds with its FW
     protocol version (= 6 on current FW) or `RTCPU_FW_INVALID_VERSION`
     (`0xFFFFFF`) if mismatched.
   - **RESUME.** AP sends `CAMRTC_HSP_MSG(CAMRTC_HSP_RESUME, cookie)`,
     RCE responds with the same opcode + status. This activates the FW
     side and gates camera-HW power.
8. **Optional FW hash readback** — `camrtc_hsp_get_fw_hash` issues 20
   `(FW_HASH, index)` requests, one per byte, and concatenates the
   responses into a SHA1.
9. **IVC bus build** — `tegra_ivc_bus_create`
   (`l4t-rtcpu-ivc-bus.c:546`):
   - Walk `nvidia,ivc-channels` for each region: count children, sum
     queue sizes, `dma_alloc_coherent` the whole region.
   - For each child node: parse `(service, version, group, frame-count,
     frame-size)`, slice rx/tx ring buffers out of the region after the
     4 KB config block, init `struct ivc` via `tegra_ivc_init_with_dma_handle`
     pointing at the rx/tx slices, populate a
     `camrtc_tlv_ivc_setup` TLV in the region's config block.
10. **Channel SETUP per region** — `tegra_ivc_bus_boot_sync` →
    `camrtc_hsp_ch_setup` → `CAMRTC_HSP_MSG(CAMRTC_HSP_CH_SETUP, iova >> 8)`:
    - Sends each region's config-block IOVA to RCE.
    - RCE reads the TLV config block out of DRAM (via its SMMU stream),
      walks the entries, and binds each `(group, service)` to the
      provided rx/tx ring IOVAs.
    - Returns 0 on success, or `RTCPU_CH_ERR_*` (128–132).
11. **Mark online** — `tegra_ivc_bus_ready(true)`. Each `tegra_ivc_channel`
    receives a "ready" notification; downstream protocol drivers (capture,
    capture-control) can now `tegra_ivc_write` / `tegra_ivc_read` frames.
12. **Steady-state notification flow.** AP-to-RCE: AP writes a frame into
    its TX ring, sets the IVC group bit in `vm-ss`
    (`tegra_hsp_ss_set(group << 16)`), then writes a one-shot IRQ message
    `CAMRTC_HSP_MSG(CAMRTC_HSP_IRQ, 1)` into the TX mailbox — all of which
    happens inside `camrtc_hsp_vm_group_ring`
    (`l4t-rtcpu-hsp-combo.c:252-277`). RCE-to-AP: RCE writes to the RX
    mailbox, AP's RX-full IRQ fires, the handler reads `vm-ss`, masks
    the FW group bits (`CAMRTC_HSP_SS_FW_MASK = 0xFFFF`), and dispatches
    each set bit to the matching IVC channel(s).

For a minimal SLM-OS port targeting IMX219, steps 1–3 are inherited from
kexec, steps 4 and 8 can be omitted, and steps 5–7 + 9–11 must be ported.

---

## Diff vs BPMP IVC

The BPMP transport SLM-OS already ships in `kernel/drivers/bpmp/`
(`bpmp.c`, `hsp.c`, `ivc.c`, `mrq.c`) is a **single channel-pair** between
CCPLEX and BPMP using HSP doorbells at `HSP_TOP_BASE = 0x03C00000`
(`kernel/include/platform.h:163`). The camera-rtcpu transport is a
**multi-channel** HSP-VM protocol on a different controller. Per piece:

| Piece | BPMP path (today) | Camera-RTCPU path (to add) | Verdict |
|---|---|---|---|
| HSP region | `0x03C00000` (`hsp_top`) | `0x0B950000` (`hsp_rce`) | **Different base — same `nvidia,tegra186-hsp` layout. SLM-OS's `hsp_init(uintptr_t hsp_base)` (kernel/drivers/bpmp/hsp.h:66) already accepts an arbitrary base, so the dimensioning probe and offset math is reusable verbatim.** |
| HSP signaling primitive | **Doorbell** block (CCPLEX→BPMP). Single bit toggle in TRIGGER, polled / IRQ via PENDING. | **Shared-mailbox pair** (CCPLEX TX → RCE RX; RCE TX → CCPLEX RX) plus a **shared semaphore** for per-channel group bits. Each side polls TX-empty / RX-full state. | **Different. New code needed: SM TX/RX register accessors and SS get/set/clear. Linux's reference is `nvidia/drivers/platform/tegra/hsp/`; the shared-mailbox register set is documented in `docs/reference/linux-tegra-hsp.c` (already cached for the BPMP work).** |
| Wire-message format on the mailbox | 32-bit BPMP MRQ index + IVC ring head/tail tracked separately (the doorbell only signals "look at the ring"). | 32-bit `CAMRTC_HSP_MSG(id, 24-bit param)` packed into the mailbox itself; sub-protocol of HELLO/PROTOCOL/RESUME/SUSPEND/BYE/CH_SETUP/PING/FW_HASH/IRQ. The mailbox carries control traffic; IVC frames carry capture data. | **Different. New code needed: wire encoding macros (`CAMRTC_HSP_MSG`, `CAMRTC_HSP_MSG_ID`, `CAMRTC_HSP_MSG_PARAM`) plus a small request/response state machine. ~150 lines following `l4t-rtcpu-hsp-combo.c:200-396`.** |
| IVC ring layout + ordering | `linux-tegra-ivc.c` ring: head/tail counters, frame buffer, `smp_mb()` ordering. SLM-OS's `kernel/drivers/bpmp/ivc.c` is a port of this. | **Same** ring shape. `tegra_ivc_init_with_dma_handle` is the same call site Linux uses for both. Per-channel rings live inside a shared region instead of standalone allocations, but the per-ring code path is identical. | **Same. SLM-OS's existing `ivc.c` can be lifted with no changes; only the surrounding region-allocation / TLV-config layer is new.** |
| Channel multiplexing | Single BPMP channel — no multiplexing. | Up to 8 groups per VM, multiple channels per group; group bits in shared semaphore identify which channel(s) to drain on RX-full. | **Different. SLM-OS needs a small dispatch table mapping group bits to channel-handler callbacks. Trivial — ~30 lines.** |
| Boot handshake | Pi 5 / Jetson BPMP path inherits a live BPMP from kexec; SLM-OS does not re-handshake. | RCE requires HELLO + PROTOCOL + RESUME exchange before any IVC channel is usable. | **Different — but only on first bring-up. After kexec from Linux, RCE is already past handshake, and SLM-OS can either re-issue HELLO (RCE is documented to handle re-sync) or trust the existing state.** |
| Channel setup ("teach RCE the rings") | n/a — BPMP knows its single channel via static convention. | Per-region `CH_SETUP(iova >> 8)` HSP message, RCE reads a `camrtc_tlv_ivc_setup[]` from the region's first 4 KB. | **Different — new but small. ~80 lines of TLV-builder + one HSP send/recv.** |
| SMMU / IOVA | BPMP IVC ring lives in cacheable carveout; BPMP accesses it directly with no IOMMU translation. | RCE goes through SMMU stream `TEGRA_SID_NISO0_RCE`. SLM-OS today has no SMMU programming and would need to either inherit Linux's existing SMMU mappings (kexec advantage — see Open Questions) or program a passthrough mapping. | **Different — and the **biggest new risk**. Discussed under §"Post-kexec considerations" and `docs/jetson-camera-imx219-plan.md` Risk 3.** |

**SLM-OS port size estimate.** ~600–900 lines of new code:

- `kernel/drivers/camrtc/hsp_sm.c` (~250 lines): SM TX/RX + SS accessors,
  modeled on `linux-tegra-hsp.c`.
- `kernel/drivers/camrtc/camrtc_hsp.c` (~250 lines): the HSP-VM protocol
  state machine (HELLO, PROTOCOL, RESUME, CH_SETUP, send/recv, group_ring).
- `kernel/drivers/camrtc/ivc_bus.c` (~200 lines): region allocator, TLV
  builder, channel dispatch.
- ~100 lines of headers/glue.

Plus reuse of `kernel/drivers/bpmp/ivc.c` and `kernel/drivers/bpmp/hsp.c`'s
dimensioning logic (split into a shared helper).

---

## Post-kexec considerations

**Does Linux leave RCE running after `slmos-kexec`?** With high probability,
**yes** — and SLM-OS would inherit a live RCE just like it inherits a live
BPMP today. Evidence:

- The RCE PM-runtime model uses a 5-second autosuspend
  (`l4t-tegra234-camera.dtsi:68`, `nvidia,autosuspend-delay-ms = <5000>`)
  and only suspends on user-space `media-ctl close + idle timeout`. If
  no camera workload is running at kexec time, the autosuspend has likely
  fired and RCE is in `tegra_camrtc_fw_suspend` state. If a camera was
  active, RCE is fully online.
- `tegra_cam_rtcpu_shutdown` (`l4t-tegra-camera-rtcpu.c:1441`) runs on a
  reboot/shutdown path but **not on kexec** unless the kexec sequence
  explicitly calls it. The current `slmos-kexec` script only suspends GPU
  (per `scripts/jetson-kexec-slmos.sh:1-30` — same pattern docs reference)
  and does not touch the camera complex.
- RCE state is in DRAM (firmware ELF, IVC rings, SMMU mappings). Provided
  SLM-OS does not zero those carveouts, the state survives kexec.

**Two scenarios:**

| Pre-kexec RCE state | Post-kexec required action |
|---|---|
| Active (camera was running) | (a) Re-issue `HELLO` with a fresh cookie to flush stale traffic — `camrtc_hsp_vm_hello` loops until cookie matches, so any in-flight RX is drained. (b) Re-issue `CH_SETUP` per region only if SLM-OS re-allocates IVC region IOVA; if SLM-OS reuses the existing region, no `CH_SETUP` needed. |
| Suspended (idle 5+ s before kexec) | (a) Set `TEGRA_PM_FWLOADDONE` bit again on `rce-pm + 0x40` if the autosuspend cleared it — defensive; the L4T `tegra_camrtc_fw_suspend` only sends `CAMRTC_HSP_SUSPEND`, it does not clear FWLOADDONE. (b) Send `CAMRTC_HSP_RESUME` to wake camera HW. (c) Re-issue `CH_SETUP` if needed. |

**GPU-suspend pattern as reference.** `scripts/jetson-kexec-slmos.sh` already
runtime-PM-suspends the GPU before kexec to drain stale DMA (see
`CLAUDE.md` §"nvgpu RAS Error"). The analogous defensive step for RCE
would be `echo suspend > /sys/devices/.../tegra-camera-rtcpu/power/control`
before kexec. **Recommended for Phase 0:** do NOT add this yet. Test the
"naive kexec, RCE just works" path first; add a suspend step only if
post-kexec HSP probes show stale traffic. The L4T HELLO loop (discard until
cookie matches) is specifically designed to recover from stale state, so
the "do nothing" path is plausible.

---

## What to peek at Phase 0

**Recommended Phase 0 peek address: `0x0B950380` — `HSP_DIMENSIONING` on
`hsp_rce`.**

Rationale:
- This is the lowest-cost, highest-information probe. `HSP_DIMENSIONING`
  is a single read-only register at `HSP_BASE + 0x380` that reports the
  shared-mailbox / shared-semaphore / arbitrated-semaphore counts for
  the controller. SLM-OS already reads the BPMP-side equivalent at
  `0x03C00380` in `hsp_init()`. A successful read with a non-zero,
  non-`0xFFFFFFFF` value confirms (a) NS EL2 can reach the
  `hsp_rce` MMIO window through CBB without a fault, and (b) the
  controller is powered and clocked.
- Expected value: the same encoded format BPMP returns (low bits =
  num_sm/2, next bits = num_ss, etc., per `kernel/drivers/bpmp/hsp.c`).
  `hsp_rce` reg span is 0x90000, identical to `aon_hsp` / `sce_hsp`,
  so the fields will be in the same place. A typical Tegra234 HSP
  block reports something like `0x00010404` (4 SMs / 2 = 2 mailbox
  pairs, 4 SSes, 0 ASes) but the **exact** value matters less than
  "not 0, not 0xFFFFFFFF, not a synchronous-external-abort".
- Failure modes to watch for at Phase 0:
  - **Synchronous external abort** → CBB is firewalling the
    0x0B95_0000 region from NS EL2 (similar to UARTA at 0x03100000).
    Decision: NVCSI Option B and VI-via-RTCPU are both blocked; the
    plan must fall back to the §"Fallback Paths" branch.
  - **Read returns 0xFFFFFFFF** → BPMP has clock-gated `hsp_rce`
    (PCIe pattern, see `CLAUDE.md` "Jetson PCIe clock teardown on
    kexec"). Decision: file an MRQ_CLK_ENABLE for the RCE HSP clock
    and re-probe; if BPMP rejects (#190 pattern), fall back.
  - **Read returns 0x00000000** → register exists but controller is
    in reset. Probably benign — try setting BPMP reset deassert and
    re-probe.

**Secondary peek targets** (only if the primary probe succeeds):
- `0x0B9F0040` — `rce-pm + TEGRA_PM_R5_CTRL_0`. Bit 0x2
  (`TEGRA_PM_FWLOADDONE`) reports "is RCE running?". Reading 0x2 set
  means RCE was running at kexec time and SLM-OS inherits a live R5;
  reading 0 means RCE is halted (suspended or never started).
- `0x0B9F0020` — `rce-pm + TEGRA_PM_PWR_STATUS_0`. Bit 0x200000
  (`TEGRA_PM_WFIPIPESTOPPED`) reports "is R5 in WFI?". Useful to
  distinguish "halted at reset" from "running but idle in WFI".

These three addresses together fully characterize the RCE state SLM-OS
inherits.

---

## Hardware Task 3 Option B — RESOLVED (2026-04-26, issue #438)

The HELLO block documented in the section below is **resolved** by
mirroring L4T's `tegra_camrtc_poweron` from `camrtc_init` before
sending HELLO. Specifically: `bpmp_clk_enable(RCE_CPU_NIC)` +
`bpmp_clk_enable(RCE_NIC)` + `bpmp_clk_enable(RCE_CPU)` +
`bpmp_reset_deassert(RESET_RCE_ALL)` + a 10 ms wait. The firmware
restarts in place from its unzeroed DRAM carveout (no FW reload
needed) and HELLO + PROTOCOL + RESUME complete cleanly.

Verified live on jetson-nano-1 (kexec from Linux):

```
=== RCE HSP-VM diag + handshake ===
[INFO] camrtc: hsp_rce DIMENSIONING=0x00080048 (SM=8 SS=4)
[INFO] camrtc: rce-pm R5_CTRL_0=0x00000002 (FWLOADDONE=1)
[INFO] camrtc: rce-pm PWR_STATUS_0=0x04600000 (WFIPIPESTOPPED=1)
[Camera-FW on t234-rce-safe started]                  <- firmware restart
[Camera-FW on t234-rce-safe ready SHA1=e2238...]      <- firmware ready
[INFO] camrtc: HELLO echo matched (cookie=0xda5c29)
[INFO] camrtc: RCE FW protocol version=6 (SM6 expected)
[INFO] camrtc: RESUME ack (status=0x0)
  camrtc_init:          rc=0
  *** RCE HSP-VM session established ***
```

The investigation in the section below is **kept verbatim** for
future reference — it documents the hypotheses that were tried and
ruled out (SS-bit drain, IRQ-wake-then-HELLO, runtime-PM pin), so a
maintainer who hits a similar HSP-VM block on a different RTCPU
(SCE/APE/DCE) doesn't re-walk the wrong trails. The fix in
`kernel/drivers/camrtc/camrtc.c::camrtc_init` is documented
inline so the comment-vs-code-vs-doc story stays consistent.

The next investigation step (filed issue #438 has now been closed
by this PR) was originally to write a Linux-side
`rtcpu_noshutdown.ko` inhibitor module, mirroring the
`arm_smmu_noshutdown.ko` workaround for tegra-xusb (#285). Turned
out the AP-side BPMP MRQs were sufficient — no kernel-module work
needed. Lesson: try the "re-engage from the AP side via BPMP"
path before reaching for kernel-module workarounds for similar
RTCPU blocks in the future.

---

## Hardware Task 3 Option B — Phase 0 + HELLO findings (2026-04-26)

Live verification on jetson-nano-1 confirmed the **MMIO layer of the
HSP-RCE IVC path is reachable from NS EL2** but the **HSP-VM HELLO
handshake doesn't get a response** from the inherited RCE firmware.

What works (Phase 0 GREEN):
- `hsp_rce` MMIO at `0x0B950000` is fully readable from EL2.
  `HSP_DIMENSIONING` reads `0x00080048` (8 SMs, 4 SSes, 0 ASes).
- `rce-pm` MMIO at `0x0B9F0000` is readable.
  `R5_CTRL_0 = 0x00000002` → `FWLOADDONE` set: bootloader released
  the R5 from `nCPUHALT`.
  `PWR_STATUS_0 = 0x04600000` → `WFIPIPESTOPPED` set: R5 is in WFI,
  idle waiting for an interrupt.
- `SM[0]` (VM-TX) and `SM[1]` (VM-RX) at `0x0B960000` and
  `0x0B968000` peek cleanly. `FULL_INT_IE = 0x1` and
  `EMPTY_INT_IE = 0x1` on both — left configured by Linux pre-kexec.
- `SS[0]` at `0x0B9A0000` peeks cleanly with `0x00000001` (a
  leftover FW→VM group bit from Linux's last activity).

What doesn't work (HELLO BLOCKED):
- `kernel/drivers/camrtc/camrtc.c::camrtc_init()` writes
  `CAMRTC_HSP_MSG(HELLO, cookie)` to `SM[0]` with the FULL bit set.
- 2 ms after the write, `SM[0]` still reads back with FULL=1 — RCE
  never drained the mailbox. The HELLO times out at 100 ms.
- Clearing `SS[0]` to flush stale FW-side group bits before sending
  HELLO did not help (same TX-FULL-stuck symptom).
- Cookie value, message encoding, and SM TX-write sequence all
  match L4T's `tegra_hsp_sm_tx_write` /
  `camrtc_hsp_vm_send_irqmsg` paths verbatim.

**ORIGINAL working theory** (now ruled out — see "Additional
verification" + "Path A tried" below): RCE's HSP IRQ routing was
reconfigured by Linux's pre-kexec runtime suspend (camera
autosuspend after 5 s idle, see
`tegra234-camera.dtsi:68 nvidia,autosuspend-delay-ms = <5000>`),
leaving the SM[0] FULL → R5 IRQ path masked at the GIC even though
the SM-side `FULL_INT_IE` bit is set.

**Refined working theory** (current best, captured in issue #438):
the camera firmware's HSP-VM ISR is torn down by Linux's kexec
`device_shutdown()` callback for `tegra-camera-rtcpu` (analogous to
the tegra-xusb teardown documented in #285), or by a TF-A / SPE-side
state change firing before SLM-OS's CPU sees the kexec. Holding
runtime PM on doesn't prevent either of those, which matches the
Path A null result below.

Additional verification (2026-04-26, same session): tried sending
`CAMRTC_HSP_MSG(IRQ=0x00, 1)` to SM[0] BEFORE the HELLO request, on
the theory that RCE's HSP-VM ISR might need an IRQ wake-up before
it processes higher-level opcodes. The IRQ wake message also stayed
stuck with TX FULL=1 — RCE doesn't drain SM[0] at all, regardless
of message ID. **This rules out hypotheses that depend on the
specific message content** (HELLO session-state, opcode-specific
firmware paths) and strongly points at the wake-up path itself
being broken.

Also tried (Path A from the avenue list) — pre-kexec
`echo on > /sys/devices/platform/bc00000.rtcpu/power/control` so
Linux holds tegra-camera-rtcpu fully active across the kexec
boundary, preventing the autosuspend transition from firing.
Boot log confirms `rtcpu power/control=on runtime_status=active`
at kexec time. **Same failure mode** — RCE still doesn't drain
SM[0]. The autosuspend hypothesis is now **disproven**: even with
RCE held actively running, post-kexec SLM-OS can't get its mailbox
serviced. Whatever teardown disables the HSP-VM path runs even
when runtime PM is pinned on. Likely candidates: Linux's kexec
`device_shutdown()` callback for `tegra-camera-rtcpu` (analogous
to the tegra-xusb teardown documented in #285), or a TF-A/SPE-side
state change that fires before SLM-OS's CPU sees the kexec.

Avenues for the next investigation cycle (each a 1-day deploy
loop on jetson-nano-1):

1. ~~**Pre-kexec RCE keep-alive.**~~ **TRIED 2026-04-26 — DOES NOT
   FIX THE HELLO BLOCK.** `scripts/jetson-kexec-slmos.sh` now writes
   `power/control = on` to `tegra-camera-rtcpu` before kexec (boot
   log: `rtcpu power/control=on runtime_status=active`). RCE still
   doesn't drain SM[0]. Hypothesis disproven; the script change is
   kept because it's harmless and rules out the autosuspend axis
   for any future investigation.
2. **Force pre-kexec camera activity.** Run a quick
   `gst-launch-1.0 nvarguscamerasrc num-buffers=1 ! fakesink` <30 s
   before kexec so the autosuspend timer hasn't fired. If HELLO
   succeeds in this case but not after autosuspend, same
   diagnosis as (1).
3. **BPMP-side RCE re-enable.** Send `MRQ_PG` for camera RTCPU's
   power domain, `MRQ_CLK_ENABLE` for the rce clocks, then
   `MRQ_RESET_DEASSERT` for `TEGRA234_RESET_RCE_ALL` from SLM-OS
   itself before the HELLO. Mirrors L4T's
   `tegra_camrtc_poweron` (cached at
   `docs/reference/l4t-tegra-camera-rtcpu.c:856`).
4. **HSP common-region INT_STATUS readback.** Peek the HSP common
   region's INT_STATUS register (per-shared-IRQ-output pending
   mask, `linux-tegra-hsp.c:HSP_INT_STATUS`). If the shared-IRQ
   output for SM[0] FULL is set in INT_STATUS but RCE doesn't
   process, the IRQ is firing but RCE isn't running its HSP ISR
   — confirms the firmware-state hypothesis.
5. **Read TF-A's HSP IRQ routing config** via `peek` of the GIC
   distributor IROUTER for the RCE_HSP_SHARED IRQs to confirm
   they're targeted at RCE's MPIDR.

The structurally-complete `kernel/drivers/camrtc/camrtc.c`
HSP-VM transport (HELLO + PROTOCOL + RESUME state machine,
mailbox accessors, SS read/clear) is kept as the diagnostic
ground for this investigation. The `rcediag` shell command
exposes the failure unambiguously and reports all the
intermediate state needed to pick up the trail.

---

## Open questions

1. **RCE firmware ownership.** The L4T binding says the bootloader loads
   the FW. **Open:** does CBoot on the Jetson Orin Nano dev kit actually
   load `camera-rtcpu-rce.img`, or does the JetPack Linux kernel side-load
   it after early-init? `request_firmware` is not in the platform driver,
   but this could happen via a separate `tegra_fw_load` shim or via
   user-space `rmmod`/`modprobe nvidia-rce` (no such module exists in
   r35, but worth confirming on hardware). If userspace loads it, the
   SLM-OS post-kexec story changes: SLM-OS would need to load the FW
   itself, or kexec would have to happen *after* user-space had brought
   the camera stack up at least once.

2. **RCE SMMU stream-id post-kexec.** The DT pins
   `TEGRA_SID_NISO0_RCE` for RCE's SMMU stream, but SLM-OS does not
   currently program the SMMU. Two sub-questions:
   - Does Linux's SMMU configuration for the RCE stream survive kexec?
     (BPMP's MC SID programming is sticky across kexec on T234 — needs
     verifying for RCE.)
   - If yes, can SLM-OS reuse Linux's IOVA range
     `0xA000_0000–0xC000_0000` (the gap between the two
     `iommu-resv-regions` entries) for fresh `CH_SETUP` calls? If no,
     SLM-OS needs at minimum to read back the existing IVC region IOVA
     from the live RCE state — and there is no public way to do that.
     Most likely path: re-allocate from the same IOVA range and
     re-issue `CH_SETUP`; the IVC ring contents don't matter at re-init
     because `tegra_ivc_channel_reset` zeros the head/tail counters.

3. **Post-kexec liveness validation.** What's the cheapest "is RCE
   responding?" probe that doesn't require the full HSP-VM init?
   `CAMRTC_HSP_PING` is the natural choice (`l4t-rtcpu-hsp-combo.c:382`),
   but it requires the RX-IRQ wiring to be set up first. A
   read-only check (no message exchange) of "is the RX mailbox empty?"
   on `hsp_rce` would be a faster pre-init probe.

4. **`hsp_top` vs `hsp_rce` port reuse.** SLM-OS's BPMP HSP code in
   `hsp.c` is hard-coded for the doorbell signaling primitive. Refactoring
   the dimensioning probe out into a shared helper before adding the SM
   TX/RX layer is the cleanest split — but adds churn to a working BPMP
   path. **Open:** is a refactor preferred, or should the camera-rtcpu
   port duplicate the dimensioning logic? Recommendation: refactor —
   but file as a separate PR before the camera-rtcpu work to keep the
   blast radius small.

5. **Trace and diagnostics IVC channels.** L4T allocates IVC ring buffers
   for `echo`, `dbg@1`, `dbg@2`, `diag@5` even when nothing uses them,
   because the per-region config block is shared and RCE expects a
   complete TLV array. ~~**Open:** does RCE accept a config block with
   only `capture-control` + `capture` channels declared, or does it
   require all six?~~ **RESOLVED 2026-04-26 (PR #456):** zero-terminated
   single-channel config block works — `CAMRTC_HSP_CH_SETUP` with one
   `camrtc_tlv_ivc_setup` for capture-control plus the zero terminator
   returned `RTCPU_CH_SUCCESS`. No dummy rings needed for the unused
   channels.

---

## Hardware Task 3 Option B — IVC ring transport (2026-04-27, PRs #460/#461)

After `CH_SETUP` binds the rings (PR #456), the next layer is the
tegra-IVC ring transport itself: send/recv frame-sized messages,
advance counters, notify the peer. Two non-obvious gotchas were
discovered live on jetson-nano-1 between writing the first cut and
getting RCE to actually consume frames.

### Gotcha 1 — half-zero the queue headers, never full-zero

Each 128-byte queue header is split into two 64-byte halves so AP↔RCE
cache traffic doesn't false-share. Each side OWNS one half:

| Queue              | AP half (writes)                            | RCE half (writes)                              |
|--------------------|---------------------------------------------|------------------------------------------------|
| TX (AP→RCE)        | bytes 0..63 — AP's `tx_count` + `tx_state`  | bytes 64..127 — RCE's `rx_count`               |
| RX (RCE→AP)        | bytes 64..127 — AP's `rx_count`             | bytes 0..63 — RCE's `tx_count` + `tx_state`    |

`CAMRTC_HSP_CH_SETUP` zero-init'd the entire region for SLM-OS, but
RCE appears to write its own `tx_state` during CH_SETUP processing.
The first cut of `camrtc_ivc_init` re-zeroed the full 128-byte header
on both queues — clobbering RCE's `tx_state` writes — which left RCE
silently ignoring frames (TX `tx_count` advanced to 1; RCE never
consumed; `recv_wait` timed out at 1 s). The fix in
`kernel/drivers/camrtc/camrtc_ivc.c:camrtc_ivc_init` is to zero only
AP's halves: `tx_iova[0..63]` and `rx_iova[64..127]`. RCE's halves are
left intact for the SYNC handshake to observe.

### Gotcha 2 — rate-limit the SYNC handshake; RCE has a heartbeat watchdog

L4T's `tegra_ivc_reset` + `tegra_ivc_notified`
(`docs/reference/linux-tegra-ivc.c:398`) drives the IVC state machine
on actual SS-bit interrupts — one notify per state transition. SLM-OS
polls instead, and a tight no-spacing poll loop trips RCE's heartbeat
watchdog (`BUG: core/watchdog/heartbeat-task.c:73 *** RCE WATCHDOG
FAILURE: HALTING ***`) within milliseconds: each `notify_rce` writes
the SS_SET bit AND sends a `CAMRTC_HSP_IRQ` mailbox message, and a
hundred of those in a row floods RCE's mailbox-FULL ISR.

The shape that works (`camrtc_ivc.c:camrtc_ivc_init`):

1. **Kickoff:** write `tx_state = SYNC` once + `notify_rce` once.
2. **Poll loop:** every 1 ms, read both `tx_state`s. Apply the L4T
   transition table only when state has changed since the previous
   poll. Notify only on actual transitions, never on a no-change
   re-poll.
3. **100 ms ceiling:** if EST/EST hasn't been observed by then,
   return `-2`.

In practice the handshake completes in 1 iteration on jetson-nano-1
(RCE's response races our first poll), but the spacing is
load-bearing for RCE's safety.

### Region placement — NC mapping at 0xBDFE0000

`CAMRTC_CTRL_REGION_PHYS = 0xBDFE0000` lives inside the existing 2 MB
NC mapping at 0xBDE00000–0xBDFFFFFF (set up by `vmm_init`, MAIR index
2 = Normal Non-Cacheable, Inner Shareable). The address is inside
RCE's VM1 IOVA aperture (0xA0000000..0xC0000000) and bypasses AP's
L1/L2 cache, so AP↔RCE memory ordering is just `dsb sy` — no
`DC CVAC`, `DC CIVAC`, or `dma-coherent` SMMU programming needed. An
earlier attempt with the region at 0xA0000000 cacheable + `DC CVAC`
did NOT work (tracking issue #458, closed by PR #460).

### Notify path

`notify_rce(group)` in `camrtc_ivc.c` mirrors L4T
`camrtc_hsp_vm_group_ring`
(`docs/reference/l4t-rtcpu-hsp-combo.c:252`):

```c
mmio_write32(SS0 + SHRD_SEM_SET, (group & 0xFF) << 16);
camrtc_send_irq(CAMRTC_HSP_IRQ, 1u, 1000u);
```

For `group=1` (capture-control) that's bit 16 of SS[0]. RCE clears
the bit in its mailbox-FULL ISR, processes the IVC group, and (when
it has data to send back) sets bit 0 of SS[0] (FW→VM, group 1) plus
optionally sends a `CAMRTC_HSP_IRQ` of its own — which the
`camrtc_send_msg` drain loop in `camrtc.c` handles transparently
(opcodes < 0x40 are unidirectional notifications, drained while
waiting for the matching response).

### Capture-control message wrappers (PR #461)

`kernel/include/camrtc_capture.h` defines the typed wire-format
structs ported from `docs/reference/l4t-camrtc-capture-messages.h`,
with sizes pinned by `_Static_assert`s in
`kernel/tests/test_camera.c`:

| Struct                                  | Size  | Reference                                  |
|-----------------------------------------|-------|--------------------------------------------|
| `capture_msg_header`                    | 8 B   | `l4t-camrtc-capture-messages.h:27`         |
| `capture_phy_stream_open_req`           | 16 B  | `l4t-camrtc-capture-messages.h:337`        |
| `capture_phy_stream_open_resp`          | 8 B   | `l4t-camrtc-capture-messages.h:352`        |
| `nvcsi_brick_config`                    | 16 B  | `l4t-camrtc-capture.h:1531`                |
| `nvcsi_cil_config`                      | 16 B  | `l4t-camrtc-capture.h:1551`                |
| `vi_hsm_csimux_error_mask_config`       | 8 B   | `l4t-camrtc-capture.h:1585`                |
| `nvcsi_error_config`                    | 56 B  | `l4t-camrtc-capture.h:1715`                |
| `capture_csi_stream_set_config_req`     | 104 B | `l4t-camrtc-capture-messages.h:407`        |
| `capture_csi_stream_set_config_resp`    | 8 B   | `l4t-camrtc-capture-messages.h:427`        |

Wrappers in `kernel/drivers/camrtc/camrtc_capture.c`:

- `camrtc_capture_init` — runs `camrtc_init` + `CH_SETUP` +
  `camrtc_ivc_init` for the capture-control channel. Idempotent.
- `camrtc_capture_phy_stream_open(stream_id, csi_port, phy_type)` —
  sends `CAPTURE_PHY_STREAM_OPEN_REQ` (msg 0x36), waits for
  `RESP` (0x37), validates the transaction id round-trip.
- `camrtc_capture_csi_stream_set_config(stream_id, csi_port,
  num_lanes, mipi_clock_rate)` — sends
  `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` (0x40), bulk-zeroes the 104-B
  body so unused error masks land at 0 (= no error reporting), then
  sets only stream/port/num_lanes/mipi_clock_rate.

### Verified end-to-end

`csidiag` shell command on jetson-nano-1:

```
=== NVCSI port A open via RCE IVC ===
[INFO] camrtc_ivc: handshake EST/EST after kickoff
[INFO] camrtc_ivc: channel up (group=1, rx=0xbdfe1000, tx=0xbdfe6080,
                                nframes=64, frame_size=320)
[INFO] capture_init: ready
[INFO] phy_stream_open: send REQ tx=0x1 stream=0 port=0 phy=0
[INFO] phy_stream_open: RESP tx=0x1 result=0x0
[INFO] csi_stream_set_config: send REQ tx=0x2 stream=0 port=0
                              lanes=2 mipi_kHz=456000
[INFO] csi_stream_set_config: RESP tx=0x2 result=0x0
*** NVCSI configured for IMX219 (2-lane D-PHY 456 MHz). ***
```

Ring counters confirm full round-trip in both directions:
TX `tx_count=1, rx_count=1`; RX `tx_count=1`. Idempotent on repeat
invocation (transaction id increments).

### Test coverage

`kernel/tests/test_camera.c` covers:
- 11 `_Static_assert`s on the `camrtc_tlv_ivc_setup` wire format
  (PR #456).
- 11 pins on the `nvcsi_brick_config` / `nvcsi_cil_config` /
  `nvcsi_error_config` / `capture_csi_stream_set_config_req` struct
  sizes and field offsets (PR #461).
- 8 pins on opcode IDs (`CAMRTC_HSP_IRQ` / `PING` / `FW_HASH` /
  `CH_SETUP`, `CAPTURE_PHY_STREAM_OPEN_REQ` / `RESP`,
  `CAPTURE_CSI_STREAM_SET_CONFIG_REQ` / `RESP`).
- Cross-platform stub-path runtime tests (PR #463):
  `test_camrtc_send_irq_uninit_returns_negative`,
  `test_camrtc_ivc_init_arg_validation` (NULL ch, zero IOVAs,
  non-power-of-two `nframes`, non-64-aligned `frame_size` — all
  rejected), `test_camrtc_ivc_predicates_uninit_safe` (NULL/uninit
  → false), `test_camrtc_ivc_send_recv_uninit_returns_negative`
  (no NULL deref), `test_camrtc_capture_stubs_return_minus_one`
  (out-pointer cleared to 0xFFFFFFFF sentinel), and
  `test_camrtc_capture_null_out_result_safe`.

---

## Hardware Task 4 — VI capture wire-format port (2026-04-27, PRs #469/#472/#473/#474)

Built on Task 3's CH_SETUP + IVC ring + capture-control-message
infrastructure, this task adds the second IVC channel ("capture")
plus the four capture-control messages that drive a VI capture:

| Message                          | Opcode | Channel           | Body size  | Verifies                    |
|----------------------------------|--------|-------------------|-----------:|------------------------------|
| `CAPTURE_CHANNEL_SETUP_REQ`      | 0x1E   | capture-control   | 272 B      | RCE allocates a VI channel   |
| `CAPTURE_CHANNEL_SETUP_RESP`     | 0x11   | capture-control   | 16 B       | RCE returns channel_id + vi_mask |
| `CAPTURE_REQUEST_REQ`            | 0x01   | **capture**       | 8 B        | RCE picks up a request       |
| `CAPTURE_STATUS_IND`             | 0x02   | **capture**       | 8 B        | RCE signals completion       |

### Two-channel CH_SETUP region layout (PR #469)

Single `CAMRTC_HSP_CH_SETUP` message now binds both channels in
one TLV array. Region at `0xBDFE0000` (64 KB) layout:

| Offset   | Size      | Contents                                |
|---------:|----------:|------------------------------------------|
| 0x0000   | 88 B      | TLV[0] capture-control                  |
| 0x0058   | 88 B      | TLV[1] capture                          |
| 0x00B0   | (rest)    | zero terminator (TLV.tag = 0)            |
| 0x1000   | 20608 B   | capture-control rx (RCE→AP)              |
| 0x60C0   | 20608 B   | capture-control tx (AP→RCE)              |
| 0xB140   | 4224 B    | capture rx (RCE→AP)                      |
| 0xC1C0   | 4224 B    | capture tx (AP→RCE)                      |
| 0xD1E0   | (free)    | end of used bytes (53,760 / 65,536)      |

Both channels share `group=1` per the L4T DT (`tegra234-camera.dtsi`
ivccontrol@3 + ivccapture@4). Geometry constants live in
`kernel/include/camrtc_layout.h` so `camrtc.c` (CH_SETUP TLV
write) and `camrtc_capture.c` (`camrtc_ivc_init` for both rings)
can't drift independently.

### VI request region (PR #473)

Separate 64 KB carveout at `0xBDFD0000` (just below the CH_SETUP
region in the same NC mapping) for the per-request descriptors:

| Offset   | Size                               | Contents                  |
|---------:|------------------------------------:|----------------------------|
| 0x0000   | queue_depth × 1024 = 1024 B        | request_ring (descriptors) |
| 0x4000   | queue_depth × 128 = 128 B          | memoryinfo_ring            |

Today `queue_depth=1` (single-shot). `request_size=1024` is a
generous over-estimate of `sizeof(capture_descriptor)` (~448 B);
RCE uses it as the slot stride, not a struct-size assertion.
`memoryinfo_size=128` matches `sizeof(capture_descriptor_memoryinfo)`
exactly. 5 `_Static_assert`s pin the geometry: inside RCE VM1
aperture, inside NC mapping, no overlap with CH_SETUP region,
ring fits before memoryinfo, memoryinfo fits in carveout.

### CHANNEL_SETUP_REQ wrapper (PR #472)

`camrtc_capture_channel_setup` builds a 280-byte request frame
(8 B header + 272 B `capture_channel_config`), bulk-zeroes
everything, then sets only the load-bearing fields:

```
channel_flags     = VIDEO | RAW | CSI    (0x10003)
vi_unit_id        = VI_UNIT_VI           (0; T234 has no VI2)
vi_channel_mask   = ~0ULL                 (let RCE pick any channel)
csi_stream        = {stream_id, csi_port, vc=0}
requests          = camrtc_vi_req_ring_iova()
requests_memoryinfo = camrtc_vi_req_meminfo_iova()
queue_depth / request_size / memoryinfo_size = camrtc_vi_req_*()
slvsec_stream_*   = SLVSEC_STREAM_DISABLED (0xFF)
num_vi_gos_tables = 0; vi_gos_tables[] = 0  (no GOS for first-light)
progress_sp / embdata_sp / linetimer_sp = 0   (no Host1x syncpoints)
error_mask_*       = 0
stop_on_error_notify_bits = 0
```

Verified on jetson-nano-1 with the smoke-test (queue_depth=0)
returning `INVALID_PARAMETER` — proving the 272-byte body parses
cleanly. With real IOVAs (PR #473), RCE allocates VI channel and
returns `result=0 channel_id=0 vi_mask=0x800000000` (bit 35 = VI
hardware channel #35).

### CAPTURE_REQUEST flow (PR #474)

`camrtc_capture_request(buffer_index, *out_status_index, timeout_us)`:

1. Caller pre-populates `request_ring[buffer_index]` with a
   `capture_descriptor` (`camrtc_capture_descriptor_header` exposes
   the leading 12 B — sequence + capture_flags + timeouts — that
   RCE reads first).
2. Wrapper builds a 16-byte `CAPTURE_REQUEST_REQ` frame on the
   *capture* IVC channel (`g_cap_chan`).
3. RCE walks the descriptor, programs VI, captures, fills the
   per-frame `capture_status` substruct of the descriptor, sends
   `CAPTURE_STATUS_IND` back on the capture rx ring.
4. Wrapper polls `g_cap_chan` for `STATUS_IND`, validates msg_id +
   buffer_index round-trip.

A successful return only means "RCE responded"; the caller MUST
inspect the slot's `capture_status.status` field for the actual
per-frame outcome.

Verified on jetson-nano-1: with a zero-init `vi_channel_config`
(no real frame format set), RCE consumed the request and emitted
its own scheduler errors over TCU (`vi5.c:4063`,
`capture-scheduler.c:2179`/`2259`). Wire format is correct; the
absence of `STATUS_IND` is expected because RCE bails before the
emission path under the internal VI errors. A real
`vi_channel_config` port lands in subsequent PRs.

### Test coverage

`kernel/tests/test_camera.c` adds (PR #475):
- 6 new `_Static_assert`s on capture_request_req / status_ind
  struct sizes (8 B each), opcodes (0x01 / 0x02), and per-
  descriptor capture_flags bits.
- 5 new `_Static_assert`s on the `capture_descriptor_header`
  prefix struct (12 B total; offsets 0/4/8/10).
- 19 `_Static_assert`s on `capture_channel_config` + sub-structs
  (PR #472).
- 5 `_Static_assert`s on the layout-constant agreement between
  camrtc.c and camrtc_capture.c via `camrtc_layout.h` (PR #469).
- New cross-platform stub-path runtime tests:
  `test_camrtc_capture_stubs_return_minus_one` extended to cover
  channel_setup + capture_request,
  `test_camrtc_capture_null_out_result_safe` extended for the
  same, `test_camrtc_vi_req_accessors` (Jetson values vs QEMU
  zeros), and `test_camrtc_ch_setup_accessors_uninit_zero`.

---

*End of notes.*
