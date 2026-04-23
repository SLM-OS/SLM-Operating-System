# HailoRT `hailo_pci` vs SLM-OS source comparison — Phase 8 boundary submit

Side-by-side audit of Linux's `hailo_pci` driver (the working reference at
21,610 FPS MNIST on `pi-5-1`) against SLM-OS's `kernel/ai_accel/hailo/` for
the data path that's stuck.

**Source pulled live from `/usr/src/hailort-pcie-driver/` on Pi OS** (the
exact version matching the installed `hailo_pci.ko`, byte-identical to
`docs/reference/hailo-{vdma,pcie}-common.{c,h}` modulo ad-hoc pr_info
hooks I added during instrumentation experiments — confirmed via md5).

## launch_transfer flow comparison

**Linux** `hailo_vdma_launch_transfer` (vdma_common.c:438-521):
1. `validate_channel_state(channel)` — sanity check
2. Sanity-check `channel->state.num_avail == starting_desc`
3. Init `ongoing_transfer` bookkeeping
4. For each buffer: `program_descriptors_list(should_bind=false)` →
   `program_last_desc()` updates **only the last desc's `PageSize_DescControl`**
   with the residue size + IRQ bits in one write
5. `desc_list[first_desc].PageSize_DescControl |= first_desc_irq_bits`
   (no-op when `first_domain=NONE`, which is what HailoRT uses for MNIST)
6. Push ongoing_transfer (host-side bookkeeping for completion handling)
7. Compute `new_num_avail = (last_desc + 1) % desc_count`
8. Update software shadow: `channel->state.num_avail = new_num_avail`
9. `hailo_vdma_set_num_avail(host_regs, new_num_avail)` → MMIO write

**SLM-OS** `hailo_backend_run` (inference_device_hailo.c:1595-1705) +
`hailo_vdma_program_buffer` + `hailo_vdma_submit_and_wait`:
1. memcpy + cache_clean input tensor
2. `program_buffer` for IN — re-programs **all desc fields** (page_size, addr_l,
   addr_h, **zeros remaining_page_size_status**, etc.) on every desc
3. `program_buffer` for OUT — same full program
4. Pre-fill OUT descs 1..7 with same buffer (defensive)
5. `write_num_avail(out_channel, 1)` — pre-arm output
6. `submit_and_wait(in_channel, 2, timeout)` → MMIO write + poll for `num_proc`

## Equivalences confirmed (no gap)

| Field | Linux | SLM-OS | Match |
|---|---|---|---|
| Descriptor `page_size_desc_control` byte layout | `(page_size << 8) + 0x02` | same | ✓ |
| `DESCRIPTOR_DESC_CONTROL` constant | 0x02 | 0x02 | ✓ |
| HOST IRQ bits OR'd into last desc | `0x20 \| 0x04 \| 0x08 = 0x2C` | same constants | ✓ |
| Last desc final ctrl | `0x02 \| 0x2C = 0x2E` | 0x2E | ✓ verified in trace |
| First desc IRQ when first_domain=NONE | none (no-op OR) | none | ✓ |
| `new_num_avail` formula | `(last_desc + 1) % desc_count` | same | ✓ |
| Channel register addr layout (ALIGNED_ADDR_L, ADDR_H, BASE_DWORD) | as documented | matches reference | ✓ |
| ATR0 save/restore around fw_access window | `read_atr_table` → write → restore | `atr0_save` / `atr0_restore` | ✓ |
| ATR base offset / field layout | 0x700 / +0/+4/+8/+C/+10 | same constants | ✓ |
| BSC IMASK / ISTATUS / per-channel IRQ enable values | 0xFFFFFFFF / 0xFF00FFFF | same | ✓ |
| Doorbell offsets (raise_ready) | 0x1684 | 0x1684 | ✓ |
| Doorbell values (APP=1, CORE=2) | 1, 2 | 1, 2 | ✓ |
| Channel indexing (IN=2, OUT=16 for MNIST) | 2, 16 | 2, 16 | ✓ |
| `masked_channel_id` for PCIe | 0 (no-op encoding) | n/a (we don't OR anything) | ✓ |

## Genuine differences (none confirmed as the bug)

### 1. Pre-binding model

**Linux**: `hailo_vdma_start_channel` is called for **every channel × both
sides (host_regs + device_regs)** at PCIe probe time (hailo-pcie.c:668-673).
Pre-binds a kernel-allocated descriptor list. Then user-space `vdma_buffer_map`
ioctl re-programs the desc entries to point at user pages. Then per-submit
`launch_transfer` runs with `should_bind=false`, only updating the last desc's
`PageSize_DescControl`.

**SLM-OS**: We *do* have `hailo_vdma_channel_start` (hailo_vdma.c:315) but
we deliberately do NOT call it from `hailo_backend_run` — comment at line
1603 cites a prior failed experiment where re-issuing channel_start clobbered
fw's CTRL byte (ABORT_PAUSE write) and num_proc never advanced. We rely on
fw's OpenBoundary action to program the channel registers.

**Verified via channel dump**: ch=2 host-side regs DO end up correctly
populated (ctrl=0x01 START, did=0 HOST_DMA, depth=5, addr=0x1000af0000
matching our IN list). So fw IS programming them correctly via OpenBoundary.
This difference exists but the end state matches Linux.

### 2. Per-submit re-programming

**Linux**: Only updates last desc `PageSize_DescControl`. Other desc fields
(addr_l, addr_h, remaining_page_size_status) survive across transfers.

**SLM-OS**: Re-programs every field of every desc on every submit, including
zeroing `remaining_page_size_status`.

**Why this isn't (likely) the bug**: On the FIRST submit there is no prior
status to erase, and the initial submit is the one that fails. End-state of
the descriptors matches Linux's after one launch.

### 3. ongoing_transfer bookkeeping

**Linux**: Maintains a circular buffer of in-flight transfers per channel,
matching IRQs to specific transfers for completion callbacks.

**SLM-OS**: No bookkeeping. We poll `num_proc` directly.

**Why this isn't the bug**: ongoing_transfer is host-side state. Fw doesn't
read it. Only affects how the host *interprets* completions, not whether fw
processes the submit.

## The actual gap

**Five sessions of structural comparison have converged on:** the bytes we
write to channel registers are identical to Linux's, the descriptor entries
in host memory are identical to Linux's, the IRQ ack flow matches Linux's,
the ATR window is handled like Linux's. Yet fw fires a completion IRQ on
ch=2 within 17 μs on Linux (per the captured MMIO trace) and never fires
one on SLM-OS (per the heartbeat poll diagnostic).

Whatever fw is reading or expecting that we don't satisfy, **it's not at the
hailo_pci-equivalent layer**. The remaining places it could live:

- **Below `hailo_pci`** — PCIe root complex programming on the BCM2712 side
  (`pcie_bcm2712.c`). Things like inbound window MPS/MRRS, AXI QoS bits,
  outbound window mapping, ATU policies that affect *fw's* ability to
  initiate DMA back to host memory. The fw needs to DMA-fetch our desc list
  on the IN side; if anything in the inbound translation path is wrong, fw's
  attempted read returns garbage / completes with an unrecoverable error and
  fw silently gives up on the channel.
- **Above `hailo_pci`** — a HailoRT userspace step we're not replicating.
  Linux's flow includes a `vdma_buffer_map` ioctl path that does
  `dma_map_sg` and `bind_and_program_descriptors_list(should_bind=true)`.
  We do the equivalent inline. But maybe HailoRT also issues a control RPC
  we don't see in the captured wire log (one not opcoded under SET_CONTEXT_INFO,
  CHANGE_STATUS, or the standard handshake — possibly something like
  CONFIG_STREAM or OPEN_STREAM that the v4.23 HailoRT wraps internally).
- **Subtle desc-list memory characteristics** — Linux uses `dma_alloc_coherent`
  for the descriptor list itself (kernel-allocated, uncached), then
  `dma_map_sg` for user buffers. SLM-OS uses cacheable PMM pages for both.
  We `cache_clean_range` the data tensor, but I haven't audited whether the
  *descriptor list* itself gets cleaned at the right moment for each submit.
  If fw reads stale desc bytes, it sees zeros / wrong IRQ bits / wrong page_size.

## Highest-leverage next probe (not started)

**Audit `hailo_tensor_prepare_for_device` and the descriptor-list cache flush
path on SLM-OS.** Specifically:

1. When we write `desc_list[i].page_size_desc_control` from CPU, that write
   sits in the L1/L2 cache.
2. Next we MMIO-write `num_avail` to the channel register.
3. Fw reads the desc list via DMA from `desc_list_iova`.
4. If our cache line for the desc list hasn't been flushed to DRAM, fw gets
   stale data from before our update.

The control-channel CCW DMA works because `hailo_cs_translator` calls
`hailo_platform->cache_clean` on the CCW buffer. The boundary tensor data
gets cleaned via `hailo_tensor_prepare_for_device`. **But does the boundary
descriptor list itself get cleaned after `program_buffer` updates its
contents?** Line-by-line audit of `program_buffer` and its callers is the
fastest remaining check before reaching for PCIe-RC-side speculation.

This wasn't done because by the time it surfaced as the most likely candidate,
the session had already burned five hypotheses.

## Files used in this comparison

- `/tmp/hailo-source-compare/vdma_common.c` (live from Pi OS — the working
  driver source, byte-identical to docs/reference/hailo-vdma-common.c)
- `/tmp/hailo-source-compare/pcie_common.c` — same
- `/tmp/hailo-source-compare/vdma.c` — Linux's vdma layer wrapper
- `kernel/ai_accel/hailo/hailo_vdma.c` — SLM-OS equivalent
- `kernel/inference/inference_device_hailo.c::hailo_backend_run` — SLM-OS
  per-submit logic
- `docs/reference/hailort-v4.23.0-mmio-trace-mnist-pi5.txt` — the ftrace
  reference capture from `hailo_resource_write32` during a working MNIST
  run, used as the "what bytes go where, when" baseline.
