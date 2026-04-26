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

## Update 2026-04-25 — all three asymmetries tested, all disconfirmed

The earlier draft (2026-04-24) listed three concrete asymmetries vs
HailoRT's MNIST trace. We've since tested each and ruled it out:

1. **GET_HW_CONSTS call count.** Implemented 4× call as a tight
   loop matching HailoRT's cadence. Result: CPU_ECC events shifted
   distribution slightly, boundary submit still hangs identically.
   Asymmetry is real but not load-bearing.

2. **SET_CONTEXT_INFO body sizes.** The earlier draft claimed our
   bodies were 102/16/37/103 vs HailoRT's 102/153/528/161 —
   **this was a measurement error.** Our ctxsmoke probe path emits
   minimal bodies (16/37/103 for the BSW/PRELIMINARY/DYNAMIC slots);
   our production load path (`hailo_backend_run`) emits 63/114/489/122.
   With the 39-byte SET_CONTEXT_INFO framing prefix added, that's
   102/153/528/161 — **byte-for-byte match to HailoRT, modulo the
   4 IOVA-bearing bytes per ACTIVATE_BOUNDARY action.** The
   "we under-emit actions" theory is dead.

3. **Settle pings between CS steps.** Tested at all three plausible
   positions: pre-RESET (3 pings), post-ENABLED (4 pings), and
   pre-ENABLED (4 pings, the position HailoRT actually uses per
   the trace timeline). Each ping returned `rc=0` from fw. Boundary
   submit still hangs in all three configurations. Asymmetry is real
   but not load-bearing.

### Additional structural probes done 2026-04-25

We continued investigating to narrow the gap further. Each probe
is small, falsifiable, and tested on hardware:

- **BIST (RUN_BIST_TEST opcode 0x3C).** Implemented to probe whether
  bit 12 of `memory_bitmap` matches the BIST `top_bypass_bitmap`
  enum. The BIST whitelist is hard-enforced to bits 2-5 (the L4
  SRAM banks); bit 12 (which `CONTROL_PROTOCOL__bist_top_mem_block_t`
  names `SAGE1_ISP_12`) is rejected with `major=0x400300b2`. We can
  confirm L4 SRAM is healthy (rc=0 with all-zero result) but cannot
  directly probe the SAGE1_ISP region. Also confirmed BIST itself
  does not trigger ECC events.

- **HailoRT SCB-style pre-trigger init sequence.** HailoRT's MMIO
  trace at boot (lines 1463-1480) shows an 8-write sequence to BAR0
  offsets 0x96c..0x988 — including `0x000005fa` written to 0x978
  (the ARM Cortex-M `SCB->AIRCR` vector key). We replicated all 8
  writes via dev_write32 before our trigger. Result: ECC distribution
  shifted (load itself stays clean) but boundary submit still hangs
  with same proc=0 / desc_status=0x00 signature.

- **D3hot transition.** `hailo_pcie:949` puts the device in
  `PCI_D3hot` after fw load; user open later transitions back to
  D0. SLM-OS now does the same round-trip via the standard PCI PM
  capability. Confirmed PMCSR transitions D0→D3→D0 (0x2008→0x200b
  →0x2008). Result: bit-12 ECC shifts entirely out of the load and
  pre-submit drain paths — but still fires the moment we attempt
  the boundary submit on ch=2. The submit hang is unchanged.

- **WRITE_MEMORY targeting audit.** Reviewed every host-side use of
  `WRITE_MEMORY` (opcode 0x01) in our driver and HailoRT's MMIO
  trace. Neither side issues `WRITE_MEMORY` during MNIST inference.
  Our CCW upload uses VDMA descriptor lists, not FW_CONTROL.
  Symmetric to HailoRT — not a host/device data-write divergence.

- **MSI binding before fw trigger.** Already implemented prior to
  this round. The MSI capability is programmed and handler bound
  before the `0xE0980` trigger write. No structural difference vs
  Linux's `hailo_pcie_enable_interrupts` flow.

- **Stage-2 firmware upload.** `hailo-pcie-common.c:308-315` shows
  Hailo-8 has only ONE upload stage — stage-2 exists only for
  `HAILO_BOARD_TYPE_HAILO10H`. Verified there's no stage-2 to
  replicate.

### Net effect on the bit-12 ECC trigger

Across the three structural changes that move state (D3hot, SCB
sequence, settle pings), the bit-12 CPU_ECC trigger MOVES — but
never disappears. Each change shifts which RPC or which timing
window first surfaces it. The boundary submit on channel 2 fails
identically in every configuration: `num_proc=0`, all
`RemainingPageSize_Status` bytes 0x00, fw never fetches our
descriptors.

Our updated reading: the bit-12 ECC may be a **symptom** of fw
state divergence, not the direct cause of the submit hang. Whatever
internal state HailoRT's flow leaves the chip in lets channel 2
proceed; ours doesn't, regardless of what host-observable bytes/
MMIO/IRQ/power-state we replicate.

### What we believe we've ruled out (host side)

- Wire-format bytes (CS RPC headers, parameter framing, length prefixes)
- Action body content (verified byte-for-byte vs HailoRT, IOVA-only diff)
- Descriptor geometry (page size, count, channel index, alignment)
- IOVA / inbound-window translation
- Cache flushing (DRAM-OK probe via `dc civac` before reads)
- Initial credit size, periph values, nn_stream_config
- IRQ mask ordering (armed before fw trigger)
- MSI capability programming (cap configured before trigger)
- D3hot/D0 round-trip (now in our driver)
- SCB/AIRCR pre-trigger init sequence
- PCIe state (link speed, MPS, MRRS, ASPM L0s disabled both ends)
- Firmware blob version (Pi OS blob and your distribution blob both tried)
- Multi-stage firmware upload (Hailo-8 has only stage-1)

We've published an end-to-end ushim probe that drives **your**
`hailo_pci` ioctls with our exact byte sequences. The probe
reproduces the boundary-submit hang identically — confirming the
issue is not in our bare-metal kernel's MMIO/cache/IRQ paths but
in something fw-side that our handshake fails to configure.

We're out of host-side hypotheses. The questions in the
"Specific questions" section are the right asks. Bit-12 decode is
the highest-leverage answer — once we know what region SAGE1_ISP
is, we can either probe it directly or stop chasing the symptom.

## Update 2026-04-25 (continued) — fw debug log capture

After concluding the structural-suspect sweep, we instrumented our
driver to dump the fw debug log buffers (`BAR4[0x2000]` for APP CPU,
`BAR4[0x3000]` for CORE CPU, 4 KB rings) as raw hex. The `host_offset`
/ `chip_offset` header advances cleanly, so the fw IS running and
writing log entries; the format isn't documented but appears to be
8-byte structured records:

- bytes 0..3: u32 LE — PC pointer (or address being logged)
- bytes 4..7: u32 LE — timestamp / counter / parameter

PCs in the `0x9xxxxxxx` range correspond to CORE CPU code memory;
`0x8xxxxxxx` for APP CPU.

### Captured sequence (post-boot → post-load → post-failed-runmodel)

CORE chip_offset advances 32 → 164 → 268 across the three states.

**Smoking-gun finding:** in the post-runmodel CORE buffer (offsets
0xa0..0x110), an 8-iteration poll loop is visible:

```
[00a0] ... 20 45 00 90 | 98 66 01 00 | 05 00 00 00
[00b0] 01 00 00 00 | 20 45 00 90 | a8 8d 01 00 | 05 00 00 00
[00c0] 02 00 00 00 | 20 45 00 90 | b8 b4 01 00 | 05 00 00 00
[00d0] 03 00 00 00 | 20 45 00 90 | c8 db 01 00 | 05 00 00 00
[00e0] 04 00 00 00 | 20 45 00 90 | d8 02 02 00 | 05 00 00 00
[00f0] 05 00 00 00 | 20 45 00 90 | e8 29 02 00 | 05 00 00 00
[0100] 06 00 00 00 | 8c 01 00 90 | 6d 47 02 00 | 20 45 00 90
[0110] f8 50 02 00 | 05 00 00 00 | 07 00 00 00
```

- PC=`0x90004520` called 8 times (a 7-iteration loop with counter
  going 0→7). Plausibly the boundary-credit / `num_avail` poll
  on the CORE CPU.
- Timestamps spaced uniformly ≈ 0x2710 (10000) per iteration —
  ten "ticks" of some internal time unit.
- **Between iterations 6 and 7 an exception fires at PC=`0x9000018c`**
  with timestamp `0x0002476d`. Iteration 7 then completes and the
  loop ends.

The `0x00001000` CPU_ECC_ERROR D2H notification arrives after
loop exit. This is the first concrete fw-side address tied to the
bit-12 trigger.

### Specific decoding asks

In addition to the bit-12 region question, please decode against
fw v4.23 symbols:

1. **PC = `0x9000018c`** — what function is this? It's where the
   exception (presumably the ECC fault) is taken or handled.
2. **PC = `0x90004520`** — what's the loop body? Almost certainly
   tied to boundary-input handling on VDMA channel 2.
3. The earlier-fired CS RPC PCs for context: `0x90003e24`,
   `0x90001fd8`, `0x90003d94`, `0x90003da4` (load), and the boot
   init sequence `0x90000030 / 0x90008b80 / 0x90000b64 /
   0x900003f4`.

If the answer to (1) is "an `__exception_handler_ecc()` style
catch-all," the line above it will tell us the actual instruction
that faulted (likely a load from the SAGE1_ISP region). If (2) is
named something like `wait_for_boundary_input_credit`, we know
the loop is what we expect — and the fact that it never observes
a credit confirms our reading that the boundary input pipeline
is gated on something that requires SAGE1_ISP to be in a valid
state.

### Capture method (reproducer)

```
hailo probe
hailo boot                 (PMCSR D0->D3->D0 cycle is now in our flow)
hailo fwloghex 256         (snapshot 1: post-boot, pre-RPC)
hailo load /mnt/files/user.hef sched
hailo fwloghex 256         (snapshot 2: post-load)
hailo runmodel 1 1
hailo fwloghex 320         (snapshot 3: post-failed-runmodel)
```

Full hex output is captured and we can attach it to the ticket.
The serial baud rate is 115200; `hailo fwloghex 0` to dump the
full ring takes ~8 s and tends to overflow the labctl ser2net
buffer, so we cap at 256-320 B per call.

### Reproducibility — multiple runmodel attempts, deterministic fault PC

Re-issuing `runmodel` (without reload) reproduces the same
fault. Second runmodel CORE diff:

```
[00a0] iter 0  PC=0x90004520 ts=0x00007801
[00b0] iter 1  PC=0x90004520 ts=0x00009f11
[00c0] iter 2 + EXCEPTION PC=0x9000018c ts=0x0000a2c9
[00d0] iter 3  PC=0x90004520 ts=0x0000c621
[00e0] iter 4  PC=0x90004520 ts=0x0000ed31
[00f0] iter 5  PC=0x90004520 ts=0x00011441
[0100] iter 6 + EXCEPTION PC=0x9000018c ts=0x00015890
[0110] iter 7  PC=0x90004520 ts=0x00016261
```

**The exception at PC=`0x9000018c` fires multiple times in a
single 500 ms window.** fw catches it, returns to the loop,
faults again a few iterations later. This rules out a one-off
transient memory glitch and indicates SAGE1_ISP is in a
persistent invalid state that fw keeps trying to access.

Both runs produce the same D2H notification body
`0x00001000 0x028xxxxx ...` with bit-12 set, confirming the
exception correlates with the bit-12 ECC error notification.

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
