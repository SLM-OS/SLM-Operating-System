# Hailo Support Ticket Draft — Phase 8 #253

> Draft for support@hailo.ai or community.hailo.ai forum post.
> Edit the salutation and account/contact info before sending.

---

## Subject

Hailo-8L on Pi 5: CPU_ECC_ERROR on every host RPC from a custom (non-HailoRT) driver — what does memory_bitmap=0x00001000 refer to in fw v4.23?

## Summary

We're driving a Hailo-8L on Pi 5 (AI HAT+) from a custom bare-metal driver
(no Linux, no HailoRT userspace library). HailoRT runs MNIST inference on
this exact chip + HEF correctly at 17.8 FPS. Our driver issues byte-for-byte
identical FW_CONTROL request payloads and an equivalent MMIO sequence, but
firmware emits a `HEALTH_MONITOR_CPU_ECC_ERROR` (event_id=7,
priority=CRITICAL) with `memory_bitmap=0x00001000` on the very first
`CHANGE_CONTEXT_SWITCH_STATUS(RESET)` RPC, and again on
`SET_CONTEXT_INFO(ACTIVATION)` and `CCW DMA pull`. Subsequent inference
submits time out: `num_proc` on the boundary input channel never
advances, and reading the descriptor `RemainingPageSize_Status` field
shows the firmware never even attempts to fetch our descriptors
(status=0x00 across all entries).

We've validated everything we can think of from the host side. We're
asking for two specific things:

1. **What memory region does bit 12 of `memory_bitmap` correspond to in
   fw v4.23?** (i.e., what does `0x00001000` mean for the `D2H_EVENT_health_monitor_cpu_ecc_event_message_t.memory_bitmap` field on Hailo-8L?)
2. **What host-side initialization step does `hailo_pci` perform such
   that firmware can safely access that region?** Our driver does
   everything we could find in the open-source `hailo_pci` source, but
   evidently there's something that prevents the ECC trip when HailoRT
   drives the device.

## Hardware

- Raspberry Pi 5, BCM2712, ARM Cortex-A76, 4 GB RAM
- AI HAT+ daughterboard: Hailo-8L (vendor=0x1e60, device=0x2864, rev=0x01)
- BAR0 (config), BAR2 (vDMA), BAR4 (fw access) — 16K/4K/16K
- PCIe link: Gen2 x1 (downgraded by Pi 5; same on HailoRT working path)

## Firmware

- `hailort-pcie-driver 4.23.0` (Pi OS apt package)
- Firmware blob: 164560 bytes, identifies as version 4.23, rev=0x20000000
- HEF: `mnist.hef`, 240815 bytes, hw_arch=hailo8l, sdk_version=3.33.1
  (single network group, 1 input pad 28×28×1, 1 output pad 1×1×10,
  28 CCW actions)

## Smoking-gun event

After every host RPC that triggers the issue, draining the D2H
notification mailbox at `BAR4 + 0x640` returns:

```
header: version=0 sequence=N priority=1 module_id=22
        event_id=7 (HEALTH_MONITOR_CPU_ECC_ERROR_EVENT_ID) /
                  or event_id=8 (CPU_ECC_FATAL on some runs)
        param_count=1 payload_len=4
body[0..3]: 0x00001000 0x02000054 0xeafff6fb 0xb77fff7f
```

Decoded per `D2H_EVENT_health_monitor_cpu_ecc_event_message_t`:

```
memory_bitmap = 0x00001000   (bit 12 set)
```

The same bit is set on every triggering RPC across power cycles, fresh
or warm boots, with both our embedded firmware blob and Pi OS's shipped
blob.

## Reproducer

The chip is correctly driven by HailoRT under Pi OS:

```
$ sudo hailortcli run mnist.hef --frames-count 1
Network mnist/mnist: 100% | 1/1 | FPS: 17.81 | ETA: 00:00:00
```

`dmesg` is clean — zero ECC events.

The chip fails under our driver, on the same hardware, with the same
HEF. Power-cycling between sessions doesn't change the outcome. We've
confirmed:

- HailoRT clean shutdown → power off → SLM-OS boot: still fails
- Cold boot → HailoRT's exact firmware blob embedded in our driver:
  ECC pattern shifts (RESET clean) but boundary submit still fails
- Cold boot → our originally-shipped firmware blob: original ECC
  pattern, boundary submit fails

## What our driver does (verified equivalent to `hailo_pci` source)

Boot path (mirrors `hailo_pcie_write_firmware_batch` +
`hailo_trigger_firmware_boot`):

1. PCI enable, BAR0/2/4 mapping
2. Disable ASPM L0s on RC and endpoint (matches Linux's
   `hailo_pcie_disable_aspm`)
3. Arm interrupts (`BSC_IMASK_HOST` |= mask, W1C `BCS_ISTATUS_HOST`,
   write `0xFFFFFFFF` to `BCS_SOURCE_INTERRUPT_PER_CHANNEL` and
   `BCS_DESTINATION_INTERRUPT_PER_CHANNEL`) — done before fw trigger
4. Allocate MSI vector, register handler — done before fw trigger
5. Validate fw header, decode app/cert/core blocks
6. Write `boot_fw_header` to `0xE0030`
7. Write `app_fw_code` to `0x60000` (chunked via 4 KB ATR window)
8. Write `boot_key_cert` to `0xE0048`
9. Write `boot_cont_cert` to `0xE0390`
10. Write `core_code` to `0xC0000`
11. Write `core_fw_header` to `0xA0000`
12. Write `1` to `trigger_address = 0xE0980`
13. Wait for `ATR[1].trsl_addr_lo` to read `PCIE_CONTROL_SECTION_ADDRESS_H8`
    (5 s budget, 50 ms interval) — equivalent to
    `hailo_pcie_is_firmware_loaded`

Load path (mirrors `hailo_activate_board` post-fw-boot + first inference):

1. `IDENTIFY` (opcode 0x00, APP_CPU)
2. `GET_DEVICE_INFORMATION` (0x33, APP_CPU) ×2
3. `CHANGE_CONTEXT_SWITCH_STATUS(state=RESET, app=0xff, batch_size=0, batch_count=0)`
   (0x25, CORE_CPU) — **this is where the first ECC event fires**
4. `CONTEXT_SWITCH_CLEAR_CONFIGURED_APPS` (0x47, CORE_CPU)
5. `GET_HW_CONSTS` (0x48, CORE_CPU)
6. `SET_NETWORK_GROUP_HEADER` (0x20, CORE_CPU)
7. `SET_CONTEXT_INFO(ACTIVATION)` (0x21, CORE_CPU) — **second ECC event fires**
8. `SET_CONTEXT_INFO(BATCH_SWITCHING)`
9. `SET_CONTEXT_INFO(PRELIMINARY)`
10. `SET_CONTEXT_INFO(DYNAMIC)`
11. `CHANGE_CONTEXT_SWITCH_STATUS(state=ENABLED, app=0, batch_size=0, batch_count=0)`
12. CCW VDMA pull (write `num_avail` to bulk cfg channel, wait for
    `num_proc` to catch up) — **on some runs, third ECC event fires here**

We then submit one MNIST inference: pre-prime 8 OUTPUT descriptors on
ch=16, then write `num_avail=2` to ch=2 (boundary input). HailoRT's
trace shows ch=2 SRC_IRQ within 7 µs; on our driver, `num_proc` on
ch=2 stays 0 indefinitely. Reading back the descriptor list (after a
host cache invalidate) shows status=0x00 on every descriptor — fw
never tried to fetch them.

## Ordering of ECC events vs host RPCs

To pre-empt the "is the ECC caused by your RPC or already in flight?"
question: the `D2H_EVENT` mailbox at `BAR4 + 0x640` is drained and
confirmed empty immediately before each RPC. The ECC notification
appears only *after* the FW_CONTROL response has been read back and
decoded. FW_CONTROL responses themselves report status=0 (success) —
firmware acknowledges the RPC, then posts the ECC event as a separate
D2H notification. This ordering has been consistent across ~50 runs.

## What we ruled out (confirmed identical to HailoRT)

- **Wire bytes**: byte-for-byte identical for `CHANGE_STATUS(RESET)`
  request (42 B) and `SET_CONTEXT_INFO(ACTIVATION)` request (102 B
  total: 39 B prefix + 63 B body). The only diffs in ACTIVATION are
  the two `OPEN_BOUNDARY_{INPUT,OUTPUT}` `dma_address` fields, which
  are expected to differ (different DMA allocators), and both are
  inside the configured BAR2 inbound translation window.
- **Periph values** (`periph_bytes_per_buffer=784`,
  `periph_buffers_per_frame=1`): match HailoRT's wire capture exactly.
- **`initial_credit_size=0x10000`**: matches.
- **HEF parsing**: the HEF's intermediate decoded fields match
  `libhailort`'s view (action types, packed_vdma channel ids, page
  sizes, desc counts).
- **CCW upload**: cfg channel `num_proc` reaches the expected count
  (109 for cfg_channel[0], 1 for cfg_channel[1]) — the bulk weight
  upload completes successfully.
- **Cache flush to DRAM**: verified via `dc civac` probe — descriptor
  contents are visible in DRAM after `dc cvac` flush.
- **Firmware blob**: tested with both our originally-shipped blob and
  Pi OS's `/lib/firmware/hailo/hailo8_fw.bin` (which differ in
  content despite both reporting v4.23 — separate question worth
  investigating).
- **MMIO sequence**: trace_mmio capture from `hailo_pci` (1557
  events) shows boot trigger and runtime doorbell writes match what
  our driver does.
- **HailoRT-style settle pings**: added 2× `GET_DEVICE_INFO` +
  IDENTIFY before RESET, 4× more after ENABLED, plus 5/3/2 ms wall
  delays at the same phase boundaries HailoRT shows in its capture.
  No effect.
- **MSI registered before fw trigger**: yes, host MSI capability is
  programmed before we write to `0xE0980`.
- **Per-channel IRQ masks armed before fw trigger**: yes,
  `BCS_SRC/DST_INTERRUPT_PER_CHANNEL = 0xFFFFFFFF` set pre-trigger.
- **Boundary descriptor page size**: we use 512 B (input) / 64 B
  (output) / 512 B (CCW) — all well under `hailo_pci`'s Pi 5
  `max_desc_page_size=4096` cap (and under the recommended 16384),
  matching HailoRT's observed values byte-for-byte from the wire
  capture. 4-KB-page and 64-KB-alignment concerns ruled out.
- **Thread #6601 context**: we saw Hailo engineer Nadav's forum
  comment that `memory_bitmap` bit 12 can also fire under thermal
  stress. Pi 5 + AI HAT+ is actively cooled (official case fan),
  chip temp is steady under load, and the event fires on the *first*
  RPC from cold boot before any sustained compute — so unless bit 12
  is multiplexed across very different failure causes, thermal
  doesn't fit our repro.

## Specific questions

1. **What is bit 12 of `memory_bitmap` in
   `D2H_EVENT_health_monitor_cpu_ecc_event_message_t`?** Is it a
   physical memory region, an L2 cache way, an SRAM bank? The
   consistent value across runs (always exactly `0x00001000`)
   suggests a single named region rather than uninitialized error
   bits. If you can share the full bit → region mapping for fw
   v4.23 on Hailo-8L, that would let us cross-reference other
   `memory_bitmap` values we see (e.g. 0x02000054 in `body[1]`
   of the notification — if that's also a region mask, it's a
   much wider spread than bit 12 alone).

2. **What host-side action is required for firmware to safely access
   that region?** Our `hailo_pci`-equivalent does the same probe-time
   register writes, the same fw upload sequence, the same trigger
   write, and signals the same MSI infrastructure. What's missing?

3. **Does `libhailort` issue a `DISABLE_NOTIFICATION` / health-monitor
   mask RPC during init?** We see in `hailo-pcie.c` that the IOCTL
   surface exposes `HAILO_DISABLE_NOTIFICATION`, but we haven't been
   able to confirm whether HailoRT actually *uses* it during normal
   inference init (vs. reserving it for diagnostics). If userspace
   masks CPU_ECC notifications early in the load sequence, a bare-
   metal driver that never issues that mask will see notifications
   that HailoRT users never do — even if the underlying ECC
   condition is present in both cases. Our MMIO-layer wire capture
   can't decode FW_CONTROL payloads, so this is invisible to our
   diff.

4. **Are these CPU_ECC events ever harmless** (e.g., HailoRT triggers
   them too but the kernel driver silently ACKs them and inference
   still works)? Our reading of the open-source driver suggests not
   — the events are critical-priority and `hailo_pcie_handle_d2h_irq`
   surfaces them — but we'd like to confirm. Thread #6601 suggests
   bit 12 can be set by thermal stress (see note in "What we ruled
   out" above), which would imply at least one scenario where the
   same bit means "non-fatal" vs "fatal" depending on context.

5. **Is there any documentation for the post-fw-boot, pre-load-network
   handshake** beyond what's visible in the open-source `hailo_pci`
   driver (`hailo-pcie.c`, `hailo-pcie-common.c`, `hailo-vdma-common.c`)?
   We've line-by-line audited those and replicated the visible logic;
   if there's a step that lives only in `libhailort` userspace
   (closed source) and matters at the kernel-equivalent layer, that's
   probably where our gap is.

6. **Firmware logger access**: is there a way to turn on verbose
   firmware logging (beyond the `FW_LOGGER` RPC surface visible in
   the driver) that would surface *why* the ECC event fires — e.g.,
   which fw task, which access address, which source instruction
   pointer? That would likely short-circuit the whole investigation.

## Update 2026-04-24 (status: still investigating; safe to send)

The "root cause identified — LCU under-emission" header that lived
here briefly was a false alarm caused by comparing HailoRT's wire
sizes (which include the 39-byte SET_CONTEXT_INFO framing prefix)
against our body sizes. After byte-by-byte comparison, our
production `hailo_backend_run` emits CS bodies that match HailoRT's
byte-for-byte modulo IOVA fields. So the "we under-emit" theory
is dead.

The `host-tools/hailo-ushim` bisect findings below remain accurate.
The remaining concrete asymmetries between HailoRT and our drive
are:

1. HailoRT calls `GET_HW_CONSTS` (opcode 0x48) four times per
   session before `SET_NETWORK_GROUP_HEADER`. We call it once.
2. HailoRT interleaves APP_CPU settle pings (`IDENTIFY` 0x00,
   `GET_DEVICE_INFO` 0x33) between CS steps — specifically
   between the four `SET_CONTEXT_INFO` calls and
   `CHANGE_STATUS(ENABLED)`. We send the four CS calls
   back-to-back, then ENABLED immediately.
3. Our drive triggers `CPU_ECC_FATAL` (event_id=8) and
   `CPU_ECC_ERROR` (event_id=7) D2H events with
   `memory_bitmap=0x00001000` on every CORE-CPU RPC starting
   from `CHANGE_STATUS(RESET)`. HailoRT-on-Pi-OS doesn't.

Sending this ticket as-is is appropriate. The questions in the
"Specific questions" section are still the right asks. We are
trying the GET_HW_CONSTS-×4 change ourselves in parallel — if
that fixes the ECC events and the boundary submit hang, we'll
update the ticket; if it doesn't, the ticket is even more
relevant.

---

## Userspace-shim bisect (2026-04-24)

To narrow the problem space, we built a minimal Linux userspace
tool (`host-tools/hailo-ushim`, ~600 lines of C) that drives your
official `hailo_pci` kernel driver directly via its ioctl surface.
The tool allocates buffers via `HAILO_VDMA_BUFFER_MAP`, descriptor
lists via `HAILO_DESC_LIST_CREATE`, and sends `HAILO_FW_CONTROL`
payloads byte-for-byte from our bare-metal driver. Running it on
a HailoRT-booted Pi 5 + AI HAT+ exercises the exact kernel path
your supported tooling uses, with our exact byte sequences.

**Decisive results from four hardware iterations:**

1. SLM-OS's **descriptor geometry** (desc_count=64, page_size=512,
   non-circular, ch=2) is accepted by every `hailo_pci` validation
   path without modification.

2. SLM-OS's **CS RPC wire format** (parameter_count framing,
   length-prefixed fields, LE/BE conventions) is byte-for-byte
   accepted by fw. RESET, CLEAR_CONFIGURED_APPS, GET_HW_CONSTS,
   SET_NETWORK_GROUP_HEADER, all 4 SET_CONTEXT_INFO contexts, and
   ENABLED each return `major_status=0x00000000`.

3. **Real MNIST CCW microcode** (from `mnist.hef`, file offset
   0x1f623, 256 bytes = 1 × 512 B descriptor) uploaded via
   `HAILO_VDMA_LAUNCH_TRANSFER` on ch=1 completes cleanly —
   `num_proc` on ch=1 advances, confirming fw processes VDMA
   traffic on the config channel.

4. Subsequent **`LAUNCH_TRANSFER` on ch=2 (boundary input) times
   out with `num_proc=0`** — byte-identical symptom to what our
   bare-metal driver exhibits.

**Interpretation:** given the same byte sequences produce the
same hang through two completely independent software stacks
(our bare-metal OS + your `hailo_pci`), the issue is not in our
low-level MMIO/cache/IRQ path, not in our descriptor geometry,
and not in our CS RPC wire format. It's in the relationship
between the CS handshake bodies and what fw needs to unblock the
boundary-input data path.

## Observed differences vs HailoRT's MNIST trace

The bisect surfaced three concrete asymmetries between our
handshake and what we see HailoRT do in the `trace_mmio` /
`trace_ioctl` capture from a successful MNIST run:

1. **GET_HW_CONSTS call count.** HailoRT invokes opcode 0x48
   four times in succession on the CORE CPU before issuing
   SET_NETWORK_GROUP_HEADER. We invoke it once. Is there a
   state machine requirement that reads stale data on the first
   1-3 calls, or is this benign retry logic?

2. **SET_CONTEXT_INFO body sizes.** From the fwctl wire trace,
   HailoRT's four SET_CONTEXT_INFO bodies for MNIST are
   102 / 153 / 528 / 161 bytes (before the 16-byte common
   header + 4-byte parameter_count). Our ctxsmoke-derived
   bodies for the same HEF are 102 / 16 / 37 / 103 bytes — a
   very different distribution, especially the 528-vs-37 gap on
   what we believe is PRELIMINARY / DYNAMIC. This suggests our
   context translator is under-emitting actions (burst credits,
   LCU sequencer, fetch_data_from_vdma, etc.) that a real MNIST
   inference setup requires. fw accepts our bodies with
   `major_status=0`, but perhaps those bodies don't fully
   configure the NN-core state machine for real inference to
   flow.

3. **Settle pings between RPCs.** HailoRT interleaves APP_CPU
   opcodes 0x00 (IDENTIFY) and 0x33 (GET_DEVICE_INFORMATION)
   between CS steps — specifically between the 4 SET_CONTEXT_INFO
   calls and CHANGE_STATUS(ENABLED). Our probe sends the 4
   SET_CONTEXT_INFO calls back-to-back then ENABLED immediately.
   Is fw expected to process SET_CONTEXT_INFO bodies
   asynchronously, with APP_CPU RPCs acting as a sync barrier?

The ticket's original questions still stand, but these three
asymmetries are the most concrete handles we have for a fix.
We can share the full fwctl capture + our probe source if the
ticket process allows it.

## Artifacts

We can share (private channel preferred):

- Boot-time MMIO trace from `hailo_pci` with `trace_mmio=Y` (1646
  lines): full fw upload + boot trigger + first inference RPC sequence
- Inference-time MMIO trace (1631 lines): all FWCTL TX bodies +
  doorbell writes + ISTATUS reads
- Side-by-side wire diff for `RESET` (byte-identical) and
  `ACTIVATION` (byte-identical except IOVAs)
- Our complete driver source (~5K LOC C in a public repo)
- Serial captures of failing inference attempts including the full
  D2H notification dump

## Environment context

This is a university capstone project building a small bare-metal
operating system that targets AI accelerators directly without a
host OS. We're not redistributing any Hailo IP — we link against the
hailort firmware blob you ship via `apt install hailort-pcie-driver`
(`modinfo hailo_pci` reports version 4.23.0) and our driver is
published under MIT license. Happy to discuss further or provide
whatever traces would help.

Best regards,
[Your name]
[Your email]
[Capstone affiliation]
