# Jetson VI5 Driver Notes (L4T code-read)

Notes extracted from the L4T `vi5` code path during the Pre-Hardware
Tasks for the Jetson IMX219 camera plan
(`docs/jetson-camera-imx219-plan.md` §"VI driver", §"Risk 3 — SMMU
translations for VI DMA"). Cached source files are in
`../slmos-reference-cache/tegra-l4t/l4t-*.c` / `../slmos-reference-cache/tegra-l4t/l4t-*.h`.

---

## Status — wire-format port complete (2026-04-27)

The plan in this doc has now been executed end-to-end on
jetson-nano-1 across PRs **#469 / #472 / #473 / #474 / #499**:

| Layer                                    | SLM-OS code                                                            | PR    |
|------------------------------------------|------------------------------------------------------------------------|-------|
| Bind 2nd IVC channel ("capture")         | `kernel/drivers/camrtc/camrtc.c` (extended TLV array)                  | #469  |
| `CAPTURE_CHANNEL_SETUP_REQ` (msg 0x1E)   | `camrtc_capture_channel_setup` in `camrtc_capture.c`                   | #472  |
| VI request region (0xBDFD0000, 64 KB)    | `camrtc_vi_req_*` accessors in `camrtc.c`; csidiag wires real IOVAs    | #473  |
| `CAPTURE_REQUEST_REQ` / `STATUS_IND`     | `camrtc_capture_request` over the *capture* IVC channel                | #474  |
| Review fixes + tests + plan/IVC docs     | typed `capture_descriptor_header` overlay; 4 new runtime tests         | #499  |

Verified `csidiag` chain on jetson-nano-1:

```
capture_init:    rc=0
PHY_STREAM_OPEN: rc=0 result=0x0    (stream 0 / port A / D-PHY)
CSI_SET_CONFIG:  rc=0 result=0x0    (2 lanes × 456 MHz)
CHANNEL_SETUP:   rc=0 result=0x0 channel_id=0x0 vi_mask=0x800000000
CAPTURE_REQUEST: send buffer_index=0 → RCE consumes + emits scheduler
                                       errors via TCU (vi5.c:4063,
                                       capture-scheduler.c:2179)
```

`vi_mask=0x800000000` (bit 35) is RCE's allocation of physical VI
hardware channel #35 to our request ring. The CAPTURE_REQUEST
round-trips at the wire level; `STATUS_IND` doesn't arrive because
the descriptor's `vi_channel_config` is zero-init (no real frame
format), so RCE bails before the IND-emission path.

**What's done** (first-light bundle 2026-04-27):

1. ✅ `struct camrtc_vi_channel_config` — 160 B, defined in
   `kernel/include/camrtc_capture.h`, populated by csidiag with
   IMX219 binning-mode RAW10 values.
2. ✅ `imx219_streaming_enable()` — wraps the I²C write to
   `MODE_SELECT = 0x01`.
3. ✅ Frame buffer carveout — 4 MB at `0xA1000000`, carved out
   of PMM region 1. Inside RCE's VM1 IOVA aperture.
4. ✅ Atomp surface IOVA wired via the **memoryinfo ring** (not
   `vi_channel_config.atomp.surface`).
5. ✅ `struct camrtc_capture_status` decode — csidiag overlays
   it on descriptor+272 and prints status code + decoded
   notify_bits.

**Hardware result on jetson-nano-1:** CAPTURE_REQUEST round-trips
end-to-end. STATUS_IND arrives with `status=14 (FALCON_ERROR)`
and `notify_bits=FRAME_START_TIMEOUT`. RCE programmed the VI
hardware and waited for SOF; the IMX219 sensor never produced
one.

**Remaining blocker** (separate PR): the IMX219 sensor needs the
full mode-table register init (binning resolution, format, PLL
config, AGC defaults — ~70 registers) before `MODE_SELECT=0x01`.
Linux's IMX219 driver does this in `imx219_set_mode`; SLM-OS
currently only writes MODE_SELECT.

Detailed wire-format reference (struct sizes / offsets / region
layouts / verified outputs / test coverage list) lives in
`docs/jetson-camera-rtcpu-ivc-driver-notes.md` §"Hardware Task 4 —
VI capture wire-format port".

---

## TL;DR — load-bearing answers up front

1. **VI5 is not register-programmable from the AP CPU.** There is no
   `vi5_registers.h`. The driver does not request a GIC IRQ, does not
   poke any VI MMIO from the CPU side, and there is no documented
   register layout for the T234 VI block. All capture programming
   reaches the hardware through the **Camera RTCPU (RCE)** via IVC
   mailbox messages.
2. **RTCPU is mandatory, not optional.** Channel setup, capture
   request submission, and frame-done notification are all IVC round
   trips. There is no "bypass-RTCPU" path in the L4T tree for VI5.
3. **DMA target IOVA is the RTCPU's IOVA, not the AP CPU's.** Surface
   buffer addresses are placed in `capture_descriptor_memoryinfo`
   after `dma_buf_attach(buf, rtcpu_dev)`. RCE walks those addresses
   through the SMMU stream-id of the **camera-rtcpu** node, not VI's
   own stream-id.
4. **Single-shot is "set up channel + submit one descriptor + wait one
   IVC reply,"** not a special hardware mode. queue_depth can be 1.
5. **Frame completion is an IVC `CAPTURE_STATUS_IND` message,** which
   the camera-rtcpu IVC subsystem demultiplexes via HSP shared-mailbox
   doorbell to `vi_capture_ivc_status_callback`, which fires a
   `complete()` on the channel's `capture_resp` waitqueue. The plan's
   "frame-done IRQ on the GIC" assumption is wrong for VI5 — the
   doorbell fires on a SPI, but it is the HSP-shared-mailbox SPI, not
   a VI peripheral SPI, and the VI MMIO base is never touched by the
   AP.

This invalidates the plan's §"VI driver" scope as written. The actual
SLM-OS dependency chain for VI5 is:

  SLM-OS → IVC → RCE firmware (VI Falcon ucode) → VI hardware → DRAM
                              ↓ (CAPTURE_STATUS_IND)
                           HSP doorbell SPI → SLM-OS

No SLM-OS code touches the 0x15c00000 window. Risk 1 (CBB firewall on
VI MMIO) is therefore **not the right risk to track for VI5** — the
load-bearing reachability question is whether NS EL2 can drive the
camera-RTCPU IVC mailbox channel at HSP base 0x03C00000 region.

---

## Source & license

Upstream:
- Repo: `OE4T/linux-tegra-5.10`
- Branch: `oe4t-patches-l4t-r35.6.4` (L4T r35.6.4 is the BSP that
  matches Jetson Orin Nano Super Developer Kit JetPack 5.1.4)
- Tree: `nvidia/drivers/media/platform/tegra/camera/{vi,fusa-capture}/`
- Headers: `nvidia/include/media/{vi.h,fusa-capture/*.h}` and
  `nvidia/include/soc/tegra/camrtc-capture*.h`

License: SPDX-License-Identifier: GPL-2.0-only on every file.
Copyright NVIDIA Corporation 2016-2024. Same downstream-only caveats
as all L4T code: NVIDIA does not maintain the bindings or the IVC wire
format as a stable ABI; an L4T release upgrade can rev the
`CAPTURE_REQUEST_REQ` payload or reorder fields silently.

Cached files (under `../slmos-reference-cache/`):

| File | Source path |
|------|-------------|
| `l4t-vi5_fops.c` | `nvidia/drivers/media/platform/tegra/camera/vi/vi5_fops.c` |
| `l4t-vi5_fops.h` | `nvidia/drivers/media/platform/tegra/camera/vi/vi5_fops.h` |
| `l4t-vi5_formats.h` | `nvidia/drivers/media/platform/tegra/camera/vi/vi5_formats.h` |
| `l4t-vi-channel.c` | `.../vi/channel.c` (V4L2 channel mgmt) |
| `l4t-vi-core.c` | `.../vi/core.c` (legacy core, not VI5-specific) |
| `l4t-vi-mc_common.c` | `.../vi/mc_common.c` |
| `l4t-vi-mc_common.h` | `nvidia/include/media/mc_common.h` |
| `l4t-vi.h` | `nvidia/include/media/vi.h` (VI2/VI4 reg defs only — not VI5) |
| `l4t-capture-vi.c` | `.../fusa-capture/capture-vi.c` (RTCPU IVC ops) |
| `l4t-capture-vi.h` | `nvidia/include/media/fusa-capture/capture-vi.h` |
| `l4t-capture-vi-channel.c` | `.../fusa-capture/capture-vi-channel.c` (chardev) |
| `l4t-capture-vi-channel.h` | `nvidia/include/media/fusa-capture/capture-vi-channel.h` |
| `l4t-capture-common.c` | `.../fusa-capture/capture-common.c` (DMA pinning) |
| `l4t-capture-common.h` | `nvidia/include/media/fusa-capture/capture-common.h` |
| `l4t-tegra_camera_core.h` | `nvidia/include/media/tegra_camera_core.h` |
| `l4t-camrtc-capture.h` | `nvidia/include/soc/tegra/camrtc-capture.h` (descriptor layout) |
| `l4t-camrtc-capture-messages.h` | `.../camrtc-capture-messages.h` (IVC wire format, cached by parallel agent) |

The IVC capture transport is in
`drivers/platform/tegra/rtcpu/capture-ivc.c` (not cached here — code
shared with the parallel NVCSI agent's `l4t-tegra-camera-rtcpu.c` /
`l4t-tegra-ivc.c`).

---

## Hardware overview

| Attribute | Value | Notes |
|-----------|-------|-------|
| SoC block | "VI5" — Video Input, fifth generation | Orin (T234) is the only T-series part on VI5; Xavier (T194) used VI4 |
| MMIO base (T234) | ~`0x15c00000` (per plan) | Not touched by AP CPU code; only the VI Falcon ucode running inside RCE accesses this |
| GIC IRQ to AP | **None requested** | Confirmed via grep on `request_irq` / `platform_get_irq` across `vi5_fops.c`, `capture-vi*.c`, `capture-common.c` — zero hits |
| Channel count | up to 64 default (`DEFAULT_VI_CHANNELS` in `capture-vi.c:70`); per-instance limit set by DT `nvidia,vi-max-channels` | Allocated lazily by RCE on `CAPTURE_CHANNEL_SETUP_REQ` |
| VI instances on T234 | up to 2 (`MAX_VI_UNITS = 2` in `capture-vi.c:75`) | DT property `nvidia,vi-devices` enumerates them; `nvidia,vi-mapping` maps NVCSI stream-id → VI instance |
| Block name in TRM | "VI" / "Video Input 5" | Public Orin TRM does not include programmer's reference for VI; VI Falcon microcode IAS ("VI Microcode IAS v0.5.13") is referenced in `camrtc-capture.h:923` but is NDA-only |
| VI5 vs prior VI | All channel programming moved into RCE firmware; the AP-side legacy CSI/VI register file (`vi.h` in cache) is **VI4-era and does not apply** | The presence of `vi.h` register defs in the L4T tree is a red herring — they're for T186/T194; T234 ignores them |

---

## Register map (relevant subset)

**There is no AP-side register map for VI5.** This section exists only
to record that absence definitively.

What `nvidia/include/media/vi.h` contains (cached as `l4t-vi.h`) — and
why it is **not** what SLM-OS needs:

```
VI_CFG_INTERRUPT_MASK_0             0x8c
VI_CFG_INTERRUPT_STATUS_0           0x98
VI_CSI_0_ERROR_STATUS               0x184
VI_CSI_*_WD_CTRL                    0x18c, 0x28c, ...
CSI_CSI_PIXEL_PARSER_A_*            0x850, 0x854, ...
```

These are T186/T194 (VI2/VI4) offsets within the CSI-VI legacy
register file. Tegra234's VI Falcon does not expose this layout to the
AP. The only "VI register access" SLM-OS has is the IVC channel
configuration descriptor (`struct vi_channel_config` in
`l4t-camrtc-capture.h:538`) which RCE translates into Falcon ucode
register writes on the AP's behalf.

If a future SLM-OS bring-up needed to bypass RCE entirely it would
need (a) the VI Falcon firmware blob, (b) a way to load it into
Falcon, and (c) the Falcon's MCU command interface — none of which are
in any L4T release.

---

## Init sequence (single-shot capture)

All of these run on the AP. RCE-side steps are flagged `[via RCE]`.

Prereqs (out of scope for this doc, covered elsewhere):
- BPMP clocks and resets (see "Clocks, resets, power domains" below)
- Camera RTCPU booted and IVC channels established (see RTCPU dep)
- NVCSI brick and CIL configured (parallel NVCSI driver notes)
- IMX219 streaming on the chosen NVCSI port

Numbered init steps from "ready to capture one frame" to "DMA buffer
ready":

1. **Allocate descriptor queue** — single-shot needs queue_depth=1.
   `dma_alloc_coherent(rtcpu_dev, sizeof(struct capture_descriptor),
   &iova, GFP_KERNEL)`. The IOVA returned is the RTCPU's IOVA, not
   the AP's physical address. (`vi5_fops.c:338`)

2. **Allocate memoryinfo ring** — `dma_alloc_coherent(rtcpu_dev,
   sizeof(struct capture_descriptor_memoryinfo), ...)`. Holds the
   surface base addresses; separate from the descriptor itself
   because the descriptor is also exposed to userspace.
   (`capture-vi.c:716`)

3. **Build `CAPTURE_CHANNEL_SETUP_REQ` message** with
   `struct capture_channel_config` payload
   (`l4t-camrtc-capture.h:383`). For single-shot RAW10 from one CSI
   stream:
   - `channel_flags = CAPTURE_CHANNEL_FLAG_VIDEO |
     CAPTURE_CHANNEL_FLAG_RAW`
     (drop `CAPTURE_CHANNEL_FLAG_EMBDATA` and
     `CAPTURE_CHANNEL_FLAG_LINETIMER` from the L4T default in
     `vi5_fops.c:45`)
   - `vi_unit_id = 0` (Orin Nano has one VI instance)
   - `vi_channel_mask = ~0ULL` (let RCE pick any HW channel)
   - `csi_stream.stream_id = 0` (matches NVCSI PixelParser index)
   - `csi_stream.csi_port = 0..7` (NVCSI brick port)
   - `csi_stream.virtual_channel = 0`
   - `requests = <descriptor queue IOVA>`
   - `requests_memoryinfo = <memoryinfo IOVA>`
   - `queue_depth = 1`
   - `request_size = sizeof(struct capture_descriptor)`
   - `request_memoryinfo_size =
     sizeof(struct capture_descriptor_memoryinfo)`
   - syncpoint info: progress_sp filled, embdata_sp/linetimer_sp
     zeroed (see "What's left out")

4. **Send setup request** `[via IVC capture-control mailbox]`. RCE
   replies with a `CAPTURE_CHANNEL_SETUP_RESP` containing
   `channel_id` and `vi_channel_mask` (which HW channel was
   allocated). Wait for the IVC completion. Timeout is 1 second in
   L4T (`capture-vi.c:356`, `HZ`).

5. **Build the capture descriptor** in the queue slot. Single-shot
   RAW10 from IMX219 1640×1232:
   - `sequence = 0`
   - `capture_flags = CAPTURE_FLAG_STATUS_REPORT_ENABLE`
     (`CAPTURE_FLAG_ERROR_REPORT_ENABLE` optional)
   - `frame_completion_timeout = 0` (RCE default)
   - `ch_cfg.match.stream = 1u << <nvcsi_stream>` (one-hot)
   - `ch_cfg.match.stream_mask = 0x3f`
   - `ch_cfg.match.vc = 1u << 0`
   - `ch_cfg.match.vc_mask = 0xffff`
   - `ch_cfg.match.datatype = 0x2b` (CSI MIPI RAW10)
   - `ch_cfg.match.datatype_mask = 0x3f`
   - `ch_cfg.frame.frame_x = 1640`
   - `ch_cfg.frame.frame_y = 1232`
   - `ch_cfg.pixfmt_enable = 1`
   - `ch_cfg.pixfmt.format = T_R16` (RAW10 packed into 16-bit)
   - `ch_cfg.atomp.surface_stride[0] = 1640 * 2`

6. **Fill the memoryinfo slot** with the surface base IOVA:
   - `surface[0].base_address = <RTCPU-IOVA of capture buffer>`
   - `surface[0].size = stride * height` (≈4 MB for the IMX219
     1640×1232 RAW10 packed-to-16 case)
   - all other surfaces zero

7. **Send `CAPTURE_REQUEST_REQ` IVC message** on the capture
   mailbox (separate from the capture-control mailbox used in step
   4) `[via IVC capture mailbox]`. Payload is just `{ msg_id,
   channel_id, buffer_index = 0 }`. Non-blocking: the call returns
   immediately. (`capture-vi.c:1399`)

8. **Wait for `CAPTURE_STATUS_IND`** on the same capture mailbox
   `[via IVC capture mailbox]`. Default L4T timeout is 2500 ms
   (`vi5_fops.c:43`, `CAPTURE_TIMEOUT_MS`). The IVC subsystem
   delivers this via HSP shared-mailbox doorbell SPI → callback
   `vi_capture_ivc_status_callback` → `complete(&capture_resp)`.

9. **Read frame status** out of `descr->status` (the descriptor
   itself was written by RCE in place):
   - `status.status == CAPTURE_STATUS_SUCCESS` (1) → frame valid
   - `status.sof_timestamp`, `status.eof_timestamp` (ns, monotonic)
   - any non-success value → log `status.err_data` and check
     `status.notify_bits` against `CAPTURE_STATUS_NOTIFY_BIT_*`

10. **Cache-invalidate the capture buffer** before reading from the
    AP if it was allocated cacheable (see DMA target requirements
    below). Then process the RAW10 frame.

Teardown (omitted from the count above): send
`CAPTURE_CHANNEL_RELEASE_REQ` to free the RCE channel allocation;
free the descriptor queue and memoryinfo ring.

---

## DMA target requirements

| Requirement | Value | Source |
|-------------|-------|--------|
| Alignment | 64-byte minimum (atom packer); page-aligned in practice because L4T uses `dma_alloc_coherent` and `round_up(..., PAGE_SIZE)` | `vi5_fops.c:908` |
| Contiguity | **Required to be physically contiguous** | `capture_common_pin_memory` falls back to `sg_phys(sgt->sgl)` when there is no IOMMU (`capture-common.c:611-612`); a multi-element scatter-list silently uses only the first element |
| Size (single full-frame) | `stride × height` | For IMX219 1640×1232 RAW10-packed-to-16: `(1640*2) × 1232 = 4,040,960 bytes` ≈ 4 MB |
| Cacheability | L4T allocates **cache-coherent** via `dma_alloc_coherent(rtcpu_dev, ...)` for descriptors. For the surface buffers themselves, L4T uses `dma_buf_attach`/`dma_buf_map_attachment` which honors the producer's cacheability — typically dma-buf-heap-cma (uncached) or videobuf2-dma-contig (uncached) | `vi5_fops.c:338` (descriptor); `capture-common.c:599-624` (surface) |
| SLM-OS NC carveout fit | **Yes — VI fits the same NC-carveout pattern documented in `kernel/CLAUDE.md` §"Non-Cacheable Shared Memory"**. RCE writes to the buffer; the AP reads it after the IVC `CAPTURE_STATUS_IND`. As long as the carveout is configured non-cacheable on the AP side and the write happens before the AP reads, no explicit cache maintenance is needed. This is identical to how the BPMP IVC shared mailbox region is treated | (no in-tree L4T equivalent; SLM-OS pattern) |

Caution on cacheable surfaces: L4T's `vi5_fops.c:338` uses
`dma_alloc_coherent` for the **descriptor queue** specifically because
RCE writes the `status.*` fields back into the descriptor in place.
On ARM64 with a coherent DMA controller, `dma_alloc_coherent` returns
non-cacheable mappings by default. The same logic applies to the
surface buffer — easier to make it non-cacheable than to invalidate
4 MB of D-cache on every frame.

---

## SMMU expectations

This is the load-bearing question for SLM-OS.

**Stream-id used by VI for DMA writes:**
The VI hardware itself has its own SMMU stream-id, but **L4T never
uses it for capture buffer DMA**. L4T programs surface IOVAs that are
valid in the **camera-rtcpu SMMU domain**:

```c
// vi5_fops.c:338 — descriptor queue IOVA
chan->request[vi_port] = dma_alloc_coherent(
    chan->tegra_vi_channel[vi_port]->rtcpu_dev,   // <-- rtcpu_dev
    setup.queue_depth * setup.request_size,
    &setup.iova, GFP_KERNEL);

// capture-common.c:599 — surface buffer IOVA
attach = dma_buf_attach(buf, dev);   // dev == rtcpu_dev in caller
sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
unpin_data->iova = sg_dma_address(sgt->sgl);
```

The IOVA placed in `capture_descriptor_memoryinfo.surface[].base_address`
is the IOVA in the camera-rtcpu's SMMU stream. RCE forwards this to
the VI Falcon, which programs it into the VI atom-packer. The VI
hardware then issues writes through the **VI stream-id**, which
points to the **same translation table** as the camera-rtcpu's
stream-id (or to a stream-id that aliases the same page table).

This is determined by L4T DT properties — the camera-rtcpu node and
the VI node must share an `iommu-map` or be assigned to the same
SMMU `pasid` group. The exact mapping is in the L4T r35.6.4 DT (not
fetched — see "Open questions").

**L4T does NOT use VI in stream-bypass.** The fall-through in
`capture-common.c:611-612` (`if sg_dma_address == 0, use sg_phys`) is
a defensive path for builds with `CONFIG_IOMMU_DMA=n`, not the normal
case. Production L4T runs both VI and camera-rtcpu through the SMMU.

**Failure mode if translation is missing when VI fires:**
SMMU translation faults on T234 raise an SMMU context-bank
fault interrupt to the AP CPU (separate SPI from VI), and the
hardware drops the offending transaction. The visible symptom is
**zeroed capture buffer + a `CAPTURE_STATUS_NOTIFY_BIT_*` payload
error in the IVC status indication**. There is no automatic retry.
The plan's "silent zero writes" prediction is correct — the AP only
sees zeros plus an error bit.

**For SLM-OS:**
The cleanest path is to **use a non-cacheable carveout below the
SMMU** — that is, configure the camera-rtcpu's SMMU stream-id either
in stream-bypass mode for the carveout's address range, or install a
1:1 (identity) translation covering the carveout. If Linux did the
latter, it survives kexec because the SMMU page tables are in DRAM
and the AP-side SMMU driver does not reset them on shutdown. The
post-kexec USB/SMMU work in #266 / #285 is directly relevant.

If neither bypass nor an inherited identity mapping can be relied on,
SLM-OS would need to program the SMMU's camera-rtcpu stream-id
itself — significant scope (an `arm-smmu-v3` driver subset). This is
the same scope decision that mothballed #266.

**Probe to run before committing:** at NS EL2 after kexec, check
the SMMU stream-id translation for the camera-rtcpu stream by
reading the SMMU's STE (Stream Table Entry) for that stream-id and
checking the V (valid) bit and the translation mode (S1+S2/S1
only/bypass). The SMMU base for T234 is at `0x12000000`. The
stream-id can be read out of the L4T DT once a copy is on hand.

---

## RTCPU dependency

**RTCPU is mandatory.** There is no code path in vi5_fops or
capture-vi that touches VI hardware without going through RCE first.
Cross-reference the parallel NVCSI agent's notes — the RTCPU IPC is
shared infrastructure across NVCSI + VI.

**Wire format for capture descriptors submitted to RTCPU:**
SLM-OS does not submit the descriptor itself over IVC — only a
`CAPTURE_REQUEST_REQ` message containing `{ msg_id, channel_id,
buffer_index }`. The descriptor lives in DRAM (the queue allocated in
step 1 above) and RCE reads it via DMA at the IOVA passed in
`capture_channel_config.requests` during channel setup.

The IVC message payloads SLM-OS needs to construct
(from `l4t-camrtc-capture-messages.h`):

| Message | Payload | Direction | Notes |
|---------|---------|-----------|-------|
| `CAPTURE_CHANNEL_SETUP_REQ = 0x1E` | `struct capture_channel_config` | AP → RCE | capture-control mailbox |
| `CAPTURE_CHANNEL_SETUP_RESP = 0x11` | `{ result, channel_id, vi_channel_mask }` | RCE → AP | capture-control mailbox |
| `CAPTURE_REQUEST_REQ = 0x01` | `{ buffer_index }` | AP → RCE | capture mailbox |
| `CAPTURE_STATUS_IND = 0x02` | `{ buffer_index }` | RCE → AP | capture mailbox; AP reads status from descriptor |
| `CAPTURE_CHANNEL_RELEASE_REQ = 0x14` | `{ reset_flags = 0 }` | AP → RCE | capture-control mailbox |
| `CAPTURE_CHANNEL_RELEASE_RESP = 0x15` | `{ result }` | RCE → AP | capture-control mailbox |

Each `CAPTURE_*_MSG` is wrapped in a `struct CAPTURE_MSG_HEADER`
(`l4t-camrtc-capture-messages.h:27`) that supplies `msg_id` and
either `channel_id` (post-setup) or `transaction` (pre-setup, opaque
ID matched in the response).

**Two separate IVC channels** are required:
- "capture-control" — synchronous request/response for setup,
  release, reset (1 sec timeout)
- "capture" — async request submission and completion notification
  (2.5 sec frame timeout)

Both ride on the camera-rtcpu HSP shared-mailbox infrastructure, set
up by `tegra-capture-ivc.c` in L4T (not cached — out of scope for VI
notes; covered by the parallel NVCSI/RTCPU code-read).

---

## Clocks, resets, power domains

From `vi5_fops.c` and the broader L4T VI/camera bring-up
(`tegra_vi5_enable`, `tegra_camera_emc_clk_enable`):

| Resource | Symbol (L4T r35 / Tegra234) | Role |
|----------|------------------------------|------|
| Clock | `TEGRA234_CLK_VI` | VI host-clock |
| Clock | `TEGRA234_CLK_VI_CONST` | VI constant clock (some BPMP versions) |
| Clock | `TEGRA234_CLK_ISP` | Required if any ISP path is used; not for SLM-OS single-shot |
| Clock | `TEGRA234_CLK_NVCSI` | (NVCSI driver scope) |
| Clock | `TEGRA234_CLK_NVCSILP` | (NVCSI driver scope) |
| Clock (EMC) | `TEGRA234_CLK_EMC` | DRAM frequency floor; raised by `tegra_camera_emc_clk_enable` to guarantee bandwidth for capture |
| Reset | `TEGRA234_RESET_VI` | Released after clocks are gated up |
| Reset | `TEGRA234_RESET_NVCSI` | (NVCSI driver scope) |
| Power domain | `TEGRA234_POWER_DOMAIN_VIC` (per plan) — **but** L4T DT typically lists the VI power domain as `TEGRA234_POWER_DOMAIN_VI` (separate from VIC, which is the Video Image Compositor) | Plan's `VIC` reference looks like a typo for `VI`; verify against `dt-bindings/power/tegra234-powergate.h` once the DT is on hand |

Cross-reference: the parallel agent fetched
`../slmos-reference-cache/linux/linux-dt-bindings-tegra234-clock.h` and
`../slmos-reference-cache/linux/linux-dt-bindings-tegra234-powergate.h` (visible in
the cache). Concrete numeric IDs for `TEGRA234_CLK_VI` and
`TEGRA234_POWER_DOMAIN_VI` should be looked up there before writing
SLM-OS code.

L4T does not directly poke clock/reset/power-domain registers — it
calls the BPMP IPC (`bpmp_send`) which SLM-OS already implements (see
`../slmos-reference-cache/linux/linux-bpmp-tegra186.c` and the existing BPMP driver in
the SLM-OS tree).

---

## What's left out

The following L4T machinery is not relevant to a single-shot capture
in SLM-OS and was skimmed but not deeply analyzed:

- **V4L2 + vb2 (videobuf2)** — `channel.c`'s 71KB of buffer queue
  state machine, queue setup, dequeue threads
  (`tegra_channel_kthread_capture_*`). SLM-OS uses a simple
  blocking `vi5_capture()` call from a Lua-driven task.
- **Media controller** — `tegra_capture_vi_media_controller_init`
  and the `graph.c` topology code build the V4L2 pipeline graph from
  DT phandles. Not needed when SLM-OS hard-wires one IMX219 → one
  NVCSI → one VI channel.
- **Runtime PM** — `pm_runtime_get_sync` etc. SLM-OS keeps VI/NVCSI
  clocked for the whole capture window.
- **Syncpoints (host1x)** — `vi_capture_setup_syncpts` allocates
  `progress_sp`, `embdata_sp`, `linetimer_sp` from the host1x
  syncpoint pool. These are an optimization (lockless wait on a
  monotonic counter via host1x channel waits) and are not strictly
  required when the AP is willing to block on the IVC completion
  instead. The L4T descriptor `progress_sp` field can be set to the
  invalid syncpoint and RCE still sends `CAPTURE_STATUS_IND`. That
  said, syncpoints reduce IVC traffic (don't need a status message
  per frame) — for single-shot, IVC is fine.
- **Multi-channel ring** — the `queue_depth >= CAPTURE_MIN_BUFFERS`
  clamp in `vi5_channel_setup_queue` is a V4L2 minimum, not a
  hardware minimum. The IVC layer accepts queue_depth=1 (the
  descriptor queue is just a ring buffer of one element).
- **Embedded data + line timer** — `CAPTURE_CHANNEL_FLAG_EMBDATA`
  and `CAPTURE_CHANNEL_FLAG_LINETIMER` channel flags add separate
  surface allocations and IVC progress messages. Not needed for
  RAW10 → MNIST.
- **Compand, PDAF, SLVS-EC** — all skipped via flag bits.
- **ISP pipeline** — VI raw output goes to DRAM; ISP would consume it
  later. SLM-OS is doing CPU-side preprocessing instead.
- **Error recovery / channel reset** —
  `vi5_channel_error_recover` and `vi_capture_reset` rebuild the
  channel state machine after an unrecoverable error. SLM-OS can
  collapse this to "report error to caller; release channel; retry
  next time."
- **Gang mode** (`chan->valid_ports > NVCSI_STREAM_1`) — splits a
  wide sensor across two VI channels. Not relevant for IMX219.

---

## Open questions

1. **VI MMIO reachability from NS EL2 after kexec — moot.** The plan
   originally listed VI MMIO at ~0x15c00000 as a CBB recon target.
   Code-read shows the AP never touches this window for VI5. The
   actual reachability question is whether NS EL2 can drive the
   camera-RTCPU HSP shared-mailbox region (~0x03C00000). HSP is
   already in use by the BPMP driver; the camera-rtcpu HSP block is
   a different instance but the same hardware family.
2. **Camera RTCPU boot state after Linux kexec.** Linux brings up
   RCE during boot (`drivers/platform/tegra/rtcpu/`), and RCE keeps
   running across kexec because it's on a separate Cortex-R5 inside
   the SoC. SLM-OS inherits a running RCE. The IVC channels Linux
   established may or may not be in a clean state. The first IVC
   send after kexec might receive a stale completion or fail silently.
   This is a runtime probe item.
3. **"Single-shot" is not a hardware mode.** It is `queue_depth=1`,
   submit one descriptor, wait one IVC reply, release. The IMX219
   itself runs in continuous-streaming mode the whole time —
   "single-shot" is just "stop draining after the first frame." If
   the sensor has been streaming for some time before the first
   `CAPTURE_REQUEST_REQ` arrives, the first captured frame may be
   one already in flight and have a stale exposure. Document for
   the demo.
4. **host1x dependency.** L4T VI5 uses host1x for syncpoint
   allocation and channel arbitration. RCE is the only consumer of
   host1x channels for capture (the AP-side host1x driver is not on
   the capture critical path). Whether SLM-OS needs to touch host1x
   at all depends on whether RCE accepts an IVC channel-setup with
   `progress_sp.id = INVALID`. This is also a runtime probe item;
   if it does not, SLM-OS would need a small host1x syncpoint
   allocator.
5. **DT iommus property for camera-rtcpu and VI.** Need to extract
   from the L4T r35.6.4 DT for Orin Nano:
   - `iommus = <&smmu N>` on the `tegra-camera-rtcpu` node →
     stream-id used for capture buffer addressing
   - `iommus = <&smmu N>` on the VI node → stream-id used for VI's
     own DMA
   - whether `dma-coherent` or `iommu-addresses` are present on
     either node
   Action: fetch the JetPack 5.1.4 BSP source bundle (NVIDIA SDK
   Manager) once a hardware bring-up is greenlit, or do a runtime
   `mem peek` on the SMMU stream table from EL2 with the stream-ids
   supplied as arguments.
6. **Does the plan's "frame-done IRQ on the GIC" assumption need to
   change?** Yes — for VI5 there is no VI peripheral IRQ. The
   "IRQ" SLM-OS waits on is the HSP shared-mailbox doorbell SPI for
   the capture IVC channel. SLM-OS already handles HSP doorbells for
   BPMP IVC; the same code can serve camera IVC.
