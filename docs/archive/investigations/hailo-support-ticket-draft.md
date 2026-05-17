# Hailo Support Ticket Draft — Hailo-8L on Pi 5: ch=2 boundary IN never advances

> Draft for support@hailo.ai or community.hailo.ai forum post.
> Edit the salutation, account/contact info, and trim the rule-out
> table to fit the support channel's length cap before sending.
>
> Last refreshed 2026-05-09 after hyp-U/W bridge-error analysis,
> hyp-X dev-state divergence finding, and hyp-X-1 / X-2 / X-3
> disconfirmations. The boot-time CPU_ECC issue documented in
> earlier drafts of this ticket is resolved internally (see hyp-O
> in commit history); this version is about the *runtime* wedge
> that boot-time fix does not address.

---

## Subject

Hailo-8L (AI HAT+ on Pi 5): boundary IN VDMA channel `proc` never advances despite byte-identical RPCs vs HailoRT — EP detects internal UR + persistent SAGE1_ISP CPU_ECC during runtime RPCs

## Summary

We're driving a Hailo-8L (vendor=0x1e60, device=0x2864) on Raspberry
Pi 5 (BCM2712) from a custom bare-metal driver — no Linux, no
HailoRT userspace. HailoRT runs MNIST inference on the same chip
+ HEF + firmware blob successfully (17.8 FPS). Our driver issues
byte-for-byte identical FW_CONTROL request payloads, the same
descriptor list geometry, and an equivalent MMIO sequence — but
the boundary INPUT VDMA channel (ch=2 on this HEF) never advances
its `num_proc` past 0, and inference times out.

We've narrowed the failure to two correlated symptoms:

1. **`HEALTH_MONITOR_CPU_ECC_ERROR/FATAL`** notifications fire
   after every runtime CORE-CPU RPC (CLEAR_CONFIGURED_APPS,
   GET_HW_CONSTS, all four SET_CONTEXT_INFO contexts, the
   post-ENABLED settle pings). HailoRT on the same hardware,
   instrumented with `trace_notif`, gets ZERO of these events
   across 10 boots + a yolov6n inference run. `memory_bitmap`
   is consistently `0x00001000` (bit 12). On Hailo-8L this maps
   to `CONTROL_PROTOCOL__TOP_MEM_BLOCK_SAGE1_ISP_12` per the
   `hailort-control-protocol.h` enum.

2. **EP-internal UR** detected post-timeout. Reading
   `DEVSTA` (PCIe Cap +0x0A) at the EP after the boundary submit
   times out shows `UR=1`, `corr=1`, while `PCI_STATUS.rcv_mabort=0`
   (the EP did NOT receive a UR completion from the RC for any
   outbound request). The UR is fired by the EP itself, internally.
   The BCM2712 root complex bridge status is clean: `phylinkup=1`,
   `dl_active=1`, `AXI_READ_ERROR_DATA=0xFFFFFFFF` (unchanged
   seed), no aborts. So whatever fw is touching that triggers UR
   never reaches the bus — it's caught by the EP's own address
   decoder.

Working hypothesis we'd like Hailo to confirm or refute:

> Both symptoms are the same mechanism — fw reads from
> uninitialized SAGE1_ISP code/data during context-switch
> processing for the boundary IN channel. The address decoder
> rejects the access (UR), the ECC checker fires on the same access
> path, and fw silently aborts the per-channel arming work for ch=2
> while letting the rest of the load complete. HailoRT's
> kernel-driver init somehow primes SAGE1_ISP into a state where
> these accesses succeed.

We're asking for help understanding what step we're missing.

## Hardware

- Raspberry Pi 5, BCM2712, ARM Cortex-A76, 4 GB RAM
- AI HAT+ daughterboard: Hailo-8L (vendor=0x1e60, device=0x2864, rev=0x01)
- BAR0 (config) 16 KB, BAR2 (vDMA) 4 KB, BAR4 (fw access) 16 KB
- PCIe link: trained Gen2 x1 (downgraded by Pi 5; HailoRT trains the
  same on the same hardware)

## Firmware

- `hailort-pcie-driver 4.23.0` (Pi OS apt package)
- Firmware blob: 164 560 bytes, identifies as v4.23 rev=0x20000000
- HEF: `mnist.hef`, 240 869 bytes, hw_arch=hailo8l, sdk_version=3.33.1.
  Single network group: 1 input pad 28×28×1, 1 output pad 1×1×10,
  28 CCW actions split across cfg_channel[0]=22 and cfg_channel[1]=6,
  total 112 256 bytes of CCW microcode.

## Reproducer

HailoRT path (works):

```
$ sudo hailortcli run mnist.hef --frames-count 1
Network mnist/mnist: 100% | 1/1 | FPS: 17.81 | ETA: 00:00:00
```

`dmesg` clean. Zero ECC events captured by an instrumented
`hailo_pci` with `trace_notif`.

SLM-OS path (fails):

```
hailo probe                  # PCI enable, BAR map → state=probed
hailo boot                   # fw upload + handshake → state=running
hailo load /mnt/files/user.hef sched   # CCW upload, context-switch
                                       # load completes (last_err=0),
                                       # CPU_ECC fires after every RPC
hailo runmodel 1 1           # IN submit times out after 500 ms,
                             # ch=2 num_proc stays 0
```

Final result: `IN submit_and_wait rc=-4 (avail=2)`, all 32 IN
descriptors show `RemainingPageSize_Status` low byte = 0x00 (fw
never tried to fetch them), bar4-diff during the timeout window
shows fw IS alive (~185 dwords change in the APP CPU log region).

## Smoking-gun evidence

### 1. Channel state divergence at first IN-submit

Pi OS instrumented `hailo_pci` MNIST trace
(`hailort-v4.23.0-mnist-inference-pios-pi5.txt`, line 3187),
chronologically:

```
hailo-sub: set_num_avail regs=...fbd23e16
           prev=0x00002801 post=0x00022801
           avail=2  dev_base=0x001f2c01  dev_proc=0x00020002
```

Same workload on SLM-OS (HEF + fw blob byte-identical):

```
[vdma] ch=2 new_avail=2 base_pre=0x00002801 base_post=0x00022801
       proc_pre=0x00000000
[vdma-poll] t=0 us  dev base_dword 0xffffffff -> 0x00002c01
[vdma-poll] t=0 us  dev proc_dword 0xffffffff -> 0x00000000
[vdma-poll] t=49900 us heartbeat  ... base=0x00022801
                                  dev base=0x00002c01 dev_proc=0x00000000
[vdma-poll] t=499900 us heartbeat ... (full 500 ms timeout, no change)
[vdma] ch=2 TIMEOUT proc_end=0x00000000 base_end=0x00022801
```

On HailoRT, fw has *already* populated `dev_base[31:16]=0x001f`
and `dev_proc=0x00020002` BEFORE the host bumps `num_avail`. fw
pre-armed the channel during the load sequence. On SLM-OS,
`dev_base[31:16]` stays `0x0000` and `dev_proc` stays `0x00000000`
indefinitely — fw never advances ch=2 internal state.

### 2. EP-internal UR (post-timeout)

```
[bridge-err] post-timeout: PCIE_STATUS=0x0003e0b0
              (phylinkup=1 dl_active=1 port_or_l23=0)
              UBUS_CTRL=0x00082000 AXI_INTF_CTRL=0x0000004f
              AXI_READ_ERROR_DATA=0xffffffff MISC_CTRL_1=0x00000020
[bridge-err] post-timeout: EP PCI_STATUS=0x0018
              (sig_tabort=0 rcv_tabort=0 rcv_mabort=0 sig_serr=0 parity=0)
              DEVSTA=0x0009 (corr=1 nonfatal=0 fatal=0 ur=1)
[bridge-err] post-timeout: EP MSI cap=+0xe0 ctrl=0x0081
              (en=1 64bit=1 multi_en=0 multi_cap=0)
              MSGADDR=0x000000ff_ffffe000 MSGDATA=0x0000
```

We W1C the EP's `PCI_STATUS` and `DEVSTA` immediately before the
IN-submit, so this dump represents *only* errors that fired
DURING the boundary submit window. Read:

- `phylinkup=1, dl_active=1` — link healthy throughout.
- `AXI_READ_ERROR_DATA` unchanged from the RC's seed — no AXI
  fabric error during the timeout window.
- `rcv_mabort=0` — the EP received NO UR completions from the
  bus for outbound requests. So fw is not issuing TLPs that the
  RC is dropping.
- `DEVSTA.UR=1` — the EP's internal address decoder rejected a
  request as Unsupported. Since `rcv_mabort=0`, the request that
  triggered UR never went outbound at all — it was caught by the
  EP itself.
- `MSGADDR=0x000000ff_ffffe000` matches the host's MIP1
  programming, confirming MSI delivery is configured correctly.

### 3. SAGE1_ISP CPU_ECC body

Drained from BAR4+0x640 after every runtime CORE-CPU RPC:

```
header: version=0 sequence=N priority=1 module_id=22
        event_id=7 (CPU_ECC_ERROR) | 8 (CPU_ECC_FATAL — random per run)
        param_count=1 payload_len=4
body[0..3]: 0x00001000 0x02800050 0xeafff6fb 0xb77fe77f
```

`memory_bitmap = 0x00001000` (bit 12 set, exclusively) every time.
The lower 16 bits of body[1] vary slightly between RPCs while
body[2..3] mostly stable; the variability is consistent with the
ECC being raised on a real read of *uninitialized* memory whose
bit pattern depends on residual SRAM state.

**Note (2026-05-10): the affected SRAM *block* is not constant
across runs.** Most of our captures show `SAGE1_ISP_12`. A
fresh SLM-OS reproduction the same day produced the same
`memory_bitmap=0x00001000` syndrome on `SAGE1_MIPI_RX_13`
instead. The runtime-ECC pattern appears to be "fw reads
uninitialized SRAM in whichever block it walks first" — the
500 ms post-BOOT settle (hyp-O) eliminates *boot-time* ECC on
SAGE1_ISP cleanly, but it does not eliminate the *runtime*
ECC, which can fire in a different memory block than the
boot-time one. We don't have a theory for what determines
which block fw walks during the wedge.

### 4. fw debug log: deterministic exception PC

We dump the fw debug rings (BAR4[0x2000] APP CPU, BAR4[0x3000]
CORE CPU, 4 KB each — header `host_offset` and `chip_offset`
advance cleanly during the wedge so fw IS executing). Decoding
8-byte records with bytes [0..3] = u32 LE PC and bytes [4..7] =
counter or timestamp, the post-failed-runmodel CORE buffer
shows a tight loop:

```
[0xa0] iter 0  PC=0x90004520 ts=0x00006698
[0xb0] iter 1  PC=0x90004520 ts=0x00018da8
[0xc0] iter 2 + EXCEPTION at PC=0x9000018c ts=0x000247ad
[0xd0] iter 3  PC=0x90004520 ts=0x0002c621
[0xe0] iter 4  PC=0x90004520 ts=0x0002ed31
[0xf0] iter 5  PC=0x90004520 ts=0x00031441
[0x100] iter 6 + EXCEPTION at PC=0x9000018c ts=0x00035890
[0x110] iter 7  PC=0x90004520 ts=0x00036261
```

PC=`0x9000018c` is the exception entry/handler. PC=`0x90004520`
is the loop body — almost certainly the boundary-credit /
`num_avail` poll on the CORE CPU. The exception fires multiple
times in a single 500 ms wedge window: fw catches it, returns
to the loop, faults again a few iterations later, and the
`0x00001000` ECC notification arrives shortly after the loop
exits. Both PCs are deterministic across reruns and across
power cycles.

This is the most direct evidence we can produce that something
inside fw's `wait_for_boundary_input_credit`-equivalent
repeatedly accesses an invalid SAGE1_ISP location.

## What we believe we've ruled out (host side)

| Class | Test | Result |
|---|---|---|
| Wire bytes | Byte-by-byte diff vs HailoRT MMIO trace for RESET, CLEAR_APPS, GET_HW_CONSTS, NETWORK_GROUP_HEADER, all 4 SET_CONTEXT_INFO, ENABLED | byte-identical modulo IOVA fields |
| Body sizes | SET_CONTEXT_INFO: 102/153/528/161 (after 39 B framing prefix) | byte-identical to HailoRT |
| Periph values | `periph_bytes_per_buffer=784`, `periph_buffers_per_frame=1`, `initial_credit_size=0x10000` | match HailoRT wire capture exactly |
| HEF parse | action types, packed_vdma channel ids, page sizes, desc counts | match `libhailort` view |
| CCW upload | cfg_channel[0] reaches `num_proc=109`, cfg_channel[1] reaches `num_proc=1` | bulk weight upload completes |
| Cache flush | `dc civac` probe before reads; `dc cvac` after writes | descriptor contents visible in DRAM |
| Firmware blob | tested original blob + Pi OS `/lib/firmware/hailo/hailo8_fw.bin` | both produce same wedge |
| MMIO sequence | `trace_mmio` capture (1 557 events) replicated | matches |
| Settle pings | `(IDENTIFY, GET_DEVICE_INFO) × 2` between OUT prefetch and IN submit, matching HailoRT cadence | rc=0 from each ping; wedge unchanged |
| GET_HW_CONSTS count | called 6× (matching HailoRT) | done; wedge unchanged |
| MSI binding | host MSI cap programmed before fw trigger write at `0xE0980` | done; matches `hailo_pcie_enable_interrupts` |
| Per-channel IRQ masks | `BCS_SOURCE_INTERRUPT_PER_CHANNEL = BCS_DESTINATION_INTERRUPT_PER_CHANNEL = 0xFFFFFFFF` armed pre-trigger | done |
| Pre-IN-submit IRQ drain | host reads/W1Cs `BCS_ISTATUS_HOST` + per-channel SRC/DST registers right before avail bump (mirrors `hailo_pcie_read_interrupt`) | drains cleanly; wedge unchanged |
| ATR table | save → retarget → access → restore around fw memory window accesses, ATR[0] only, canonical PARAM=0x17 / SRC=0 / TRSL_PARAM=6 | matches Linux's pattern |
| Descriptor geometry | desc_count=32, page_size=512 (in) / 64 (out) / 512 (cfg), non-circular, ch=2 (in), ch=16 (out) | accepted by `hailo_pci` validation paths via `host-tools/hailo-ushim` |
| IOVA / inbound window | RC `SCB0` size widened to 64 GB to match `dma-ranges` union (Linux `brcm-pcie` derivation) | matches Linux; wedge unchanged |
| BCM2712 VDM QoS | `EN_VDM_QoS_CONTROL=1`, both VDM map registers programmed | matches Linux; wedge unchanged |
| D3hot/D0 round-trip | PCI PM `PMCSR=0x2008→0x200b→0x2008` between fw load and first RPC | matches Linux; wedge unchanged |
| Post-BOOT_IRQ settle | 500 ms `udelay` between BOOT_IRQ ack and first RPC | suppresses *boot-time* ECC at IDENTIFY (60% → 0/10 boots); does NOT suppress runtime ECCs |
| Longer settle | 2000 ms variant tested | runtime ECC rate unchanged (rules out wall-clock as the lever) |
| BIST `RUN_BIST_TEST` (0x3C) | bit 12 (`SAGE1_ISP_12`) rejected with `major=0x400300b2` per BIST whitelist; L4 banks pass with all-zero result | confirms L4 healthy; can't directly probe SAGE1_ISP |
| Multi-stage fw upload | Hailo-8 has only stage-1 upload (stage-2 is `HAILO_BOARD_TYPE_HAILO10H`) | confirmed; not applicable |
| HailoRT-via-ushim | drove `hailo_pci` ioctl surface (`HAILO_FW_CONTROL`, `HAILO_VDMA_BUFFER_MAP`, `HAILO_DESC_LIST_CREATE`, `HAILO_VDMA_LAUNCH_TRANSFER`) with our exact byte sequences from a Linux userspace tool | reproduces wedge identically — confirms issue is not in our bare-metal MMIO/cache/IRQ paths |

## Specific questions

1. **Bit 12 of `D2H_EVENT_health_monitor_cpu_ecc_event_message_t.memory_bitmap`
   on Hailo-8L fw v4.23**: the enum names this region
   `TOP_MEM_BLOCK_SAGE1_ISP_12`. Is that name documented further?
   Specifically, what kind of memory is it (code / data / cache /
   scratch SRAM), what address range does it cover, and what is the
   expected post-boot initialization state?

2. **What host-side (or RPC-side) action triggers fw to scrub /
   prime SAGE1_ISP** such that the runtime CORE-CPU RPCs and the
   boundary-channel arming don't fault on uninitialized reads? We
   demonstrably do everything in the open-source `hailo_pci` source
   (down to the BCS register order and the per-channel IRQ
   pre-arming), so the missing step is plausibly in `libhailort` or
   in fw-internal logic not surfaced through the kernel driver.

3. **Decode for the deterministic fault PC `0x9000018c`** in
   fw v4.23. The exception fires repeatedly during the
   boundary-input poll loop at `0x90004520`. If `0x9000018c` is an
   ECC fault handler / synchronous abort vector / breakpoint at a
   well-known fw routine, that name + the surrounding code tells us
   exactly what fw is trying to read that doesn't exist.

4. **Decode for `0x90004520`**: is this
   `wait_for_boundary_input_credit` or equivalent? Confirming the
   loop body's name lets us anchor whether the wedge is in
   "fw never received the credit" (host side) or "fw can't proceed
   past credit because internal init is incomplete" (fw-internal,
   matches our reading).

5. **Does `libhailort` issue any RPC during normal inference init
   that the open-source `hailo_pci` doesn't surface** — e.g., a
   memory-setup, capability-init, or scrub command that we wouldn't
   see in the MMIO trace because it's a FW_CONTROL message, not a
   register write? `hailo_pci` exposes `HAILO_DISABLE_NOTIFICATION`;
   are there parallel "enable scrubber" / "init memory region"
   opcodes in `libhailort`?

6. **Is there a fw-side switch to make `0x00001000` ECC events
   non-fatal during the load + first-frame window** (e.g., a
   debug-mode firmware variant that masks ECC notifications until
   the first inference completes)? That would let us isolate
   whether the wedge is gated on the ECC itself (fw aborts
   ch=2 prep on detection) or the ECC is purely a side-effect of
   the same uninitialized read that the EP is also UR'ing.

## Artifacts available on request (private channel preferred)

- Boot-time `trace_mmio` from instrumented `hailo_pci` (1 646 lines
  covering fw upload + boot trigger + first inference RPC sequence)
- Inference-time `trace_mmio` (1 631 lines: all FWCTL TX bodies +
  doorbell writes + ISTATUS reads)
- Side-by-side wire diff for RESET (byte-identical) and ACTIVATION
  (byte-identical except for IOVA fields)
- Pi OS MNIST trace (3 913 lines) and yolov6n trace (9 463 lines)
  with `trace_mmio` + `trace_notif`
- Our complete bare-metal driver source (~5 KLOC C, public MIT
  repo)
- `host-tools/hailo-ushim` source — minimal Linux userspace tool
  (~600 lines C) that drives the `hailo_pci` ioctl surface with
  our byte sequences and reproduces the wedge
- Serial captures of failing inference attempts including full
  D2H notification dumps and BAR4 fw-debug-log hex
- Bridge-error MMIO trace captured under `HAILO_WIRE_DEBUG`
  showing `PCIE_STATUS`, `AXI_READ_ERROR_DATA`, EP `PCI_STATUS`,
  `DEVSTA`, MSI cap, at pre-IN-submit and post-timeout

## Environment context

This is a university capstone project building a small bare-metal
operating system that targets AI accelerators directly without a
host OS. We're not redistributing any Hailo IP — we link against
the firmware blob shipped via `apt install hailort-pcie-driver`
(`modinfo hailo_pci` reports version 4.23.0) and our driver is
published under MIT license. Happy to share traces, source, or
repro instructions on whatever channel works best.

Best regards,
[Your name]
[Your email]
[Capstone affiliation]
