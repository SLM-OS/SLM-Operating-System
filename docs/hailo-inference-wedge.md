# Hailo-8L Inference Wedge — Investigation Write-up

**Status:** Closed as a documented limitation (2026-10-18). No further
host-side work is scheduled.
**Hardware:** Raspberry Pi 5 (`pi-5-1`) + AI HAT+ 13 TOPS (Hailo-8L),
PCIe Gen2 x1 link.
**Firmware:** `hailo8_fw.bin` v4.23.0, rev `0x20000000`. The same blob
serves Hailo-8 and Hailo-8L; both enumerate as `1e60:2864`.
**Model:** MNIST (28×28×1 → 10), compiled with Hailo DFC 3.33.1 for
`hailo8l`. It is a single-context HEF of 240 KB.
**Reference stack:** Pi OS Bookworm + `hailo_pci` + HailoRT v4.23.0 on
the same board, HAT, firmware, and HEF.
**Investigation window:** Phase 8 first-inference bring-up began on
2026-04-21. The boundary-channel wedge was the only remaining blocker from
2026-05-06 until the second-pass closure on 2026-05-28.
**Tracking:** [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682)
(original), [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001)
(post-fix follow-on), [#1002](https://github.com/SLM-OS/SLM-Operating-System/issues/1002)
(firmware-state / ECC correlation),
[#1003](https://github.com/SLM-OS/SLM-Operating-System/issues/1003)
(ECC-trigger bisect). All four are closed.

## Abstract

SLM-OS drives the Hailo-8L NPU from bare metal with no Linux and no
HailoRT. The driver trains the PCIe link, boots the firmware, parses a
compiled HEF, and runs the full context-switch load sequence. Every
firmware RPC returns success and the configuration channels finish their
DMA. Inference never completes, though. When the host posts the first
input frame on boundary IN channel 2, the firmware never consumes it.
`num_proc` stays at zero, and the firmware never writes a descriptor
status byte.

HailoRT runs the same HEF on the same board at 4,000–11,000 FPS, so the
silicon, HAT, firmware, and model are all sound. Over two investigation
passes, about 30 hypotheses were tested on hardware. One real host-side
defect was found and fixed: a VDMA channel-direction mismatch, fixed in
[#999](https://github.com/SLM-OS/SLM-Operating-System/pull/999). Fixing it
did not clear the wedge.

The final pass checked every byte the host gives the firmware against
HailoRT's byte for byte. This covers control-RPC bodies, MMIO register
writes, descriptor rings, and the DMA payloads the firmware reads from host
RAM. All of it matches, apart from expected IOVA differences. The remaining
cause is therefore firmware-internal state that cannot be observed from
the host. Closing the gap would take either firmware-memory inspection or
vendor support, and neither was pursued.

---

## 1. What works end-to-end

The following stages are verified on `pi-5-1` hardware and stay
functional in tree.

| Stage | Result | Evidence |
|---|---|---|
| PCIe link training (`pcie1`) | Pi 5 firmware does not train `pcie1`. SLM-OS ports Linux's `brcm_pcie_setup`, and the link comes up Gen2 x1, the same as under Linux. | [`pi5-ai-hat-plan.md`](pi5-ai-hat-plan.md) Phase 1.5 |
| Probe | `hailo probe` enumerates `1e60:2864` and maps BAR0/BAR2/BAR4. | Phase 3 |
| Firmware boot | Both the app and core firmware sections are uploaded through the ATR[0] window into BAR4. After the ATR[1] handshake, upload plus handshake takes 120 ms (Linux takes 282 ms). A 500 ms post-BOOT_IRQ settle brings boot-time CPU_ECC events to 0/10 boots, which matches Linux. | [`pi5-ai-hat-plan.md`](pi5-ai-hat-plan.md) §8 |
| IDENTIFY | Firmware reports 4.23, rev `0x20000000`. | `hailo boot` log |
| Control RPCs | All of these return `rc=0` with `last_err=0`: CHANGE_STATUS(RESET), CLEAR_CONFIGURED_APPS, GET_HW_CONSTS ×4, SET_NETWORK_GROUP_HEADER, SET_CONTEXT_INFO ×4, CHANGE_STATUS(ENABLED), IDENTIFY and GET_DEVICE_INFORMATION. Request bytes, cadence, and flags match HailoRT's wire capture. | [#1004](https://github.com/SLM-OS/SLM-Operating-System/pull/1004) |
| HEF parse | `ngs=1 pads=2 ccws=28`, with input 784 B and output 10 B. The parser walks both the legacy and the modern `ops[].core_op` layout, so the CCW total is the correct 56,128 B. | [#1008](https://github.com/SLM-OS/SLM-Operating-System/pull/1008) |
| Context-switch load | The firmware accepts ACTIVATION (63 B), BATCH_SWITCHING (114 B), PRELIMINARY (489 B), and DYNAMIC (122 B) in its required order. All four bodies are byte-identical to HailoRT's, apart from `dma_address` IOVA bytes. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) Hyp #2 |
| CCW config upload | Config channel ch=0 reaches `proc=0x006e006d` (109 of 109 descriptors), and ch=1 completes. The firmware DMA-reads the network configuration from host RAM. | [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) |
| MSI | MIP1 external-PCIe MSI is delivered, with endpoint `MSGADDR=0x000000ff_ffffe000`. FW_NOTIFICATION handling is IRQ-driven ([#793](https://github.com/SLM-OS/SLM-Operating-System/pull/793)). Control-RPC completion wakes the waiting task through an MSI-driven wait queue ([#1018](https://github.com/SLM-OS/SLM-Operating-System/pull/1018)). | PRs listed |
| Boundary channel arming | The host arms ch=2 (H2D) and ch=16 (D2H) at the same BAR2 offsets and values HailoRT uses, including the 8-deep OUT prefetch. | [#999](https://github.com/SLM-OS/SLM-Operating-System/pull/999) |

The pipeline gets this far and no further:

```
hailo probe ──► hailo boot ──► hailo load ─────────────────► hailo runmodel 1
 [ok] PCIe       [ok] fw up     [ok] 4 contexts, rc=0          [ok] ch=16 OUT prefetch 1..8
 [ok] BARs       [ok] IDENTIFY  [ok] CCW ch=0 proc=109         [ok] ch=2 IN num_avail=2
                                [ok] CCW ch=1 done             [X]  fw never consumes ch=2
```

---

## 2. The wedge signature

The failing step is the first input submit in `hailo runmodel`. This log
is from the post-#999 build, but the shape is the same in every recorded
capture:

```
hailo: runmodel handle=1 in=784 out=10 iters=1
[hailo] run: pre-submit CORE_IDENTIFY rc=0 resp_len=44 latency=107 us
[vdma] ch=2 TIMEOUT proc_end=0x00000000 base_end=0x00022801
[hailo] run: IN submit_and_wait rc=-4 (avail=2)
[desc-status] IN desc[0..7] status=0x00 rps_full=0x00000000
[desc-status] OUT desc[0..7] status=0x00 rps_full=0x00000000
  iter 0: inference_run rc=-7
```

What characterizes the wedge:

- **The host's write lands, and the firmware never acknowledges it.** The
  host-side channel register for ch=2 reads `0x00022801` (ctrl=START,
  depth=32 entries, `num_avail=2`). The device-side half of the channel
  never mirrors that avail. A BAR4 diff shows how this differs from the
  channels that work:

  | Channel | Host avail / proc | Device avail / proc |
  |---|---|---|
  | ch=0 CFG IN (works) | 110 / 109 | 12 / 22 |
  | ch=1 CFG OUT (works) | 1 / 1 | 0 / 24 |
  | **ch=2 boundary IN (wedged)** | **2 / 0** | **0 / 0** |
  | ch=16 boundary OUT (wedged) | 8 / 0 | 0 / 0 |

- **The firmware never touches the IN descriptor ring.** All descriptor
  `status` bytes are still `0x00` after 500 ms, and after 30 s on the smoke
  path. In one test, a `0xABABABAB` sentinel was written into the IN
  descriptor list. It survived unchanged
  ([#1009](https://github.com/SLM-OS/SLM-Operating-System/pull/1009)),
  which shows the firmware never started DMA on the channel.
- **The firmware is alive.** CORE_IDENTIFY round-trips just before the
  submit. D2H notifications keep arriving during the timeout. Five BAR4
  dwords in the firmware state region change and 185 dwords in the app-log
  region advance. The firmware runs its main loop; it just never schedules
  ch=2.
- **The failure is deterministic.** A 10-boot quantification had 9 valid
  boots, and all 9 wedged with `proc_end=0x00000000`. The tenth boot was
  excluded because a serial-FIFO overrun meant the workload never ran
  ([#1002](https://github.com/SLM-OS/SLM-Operating-System/issues/1002)).
- **CPU_ECC events fire that Linux never shows.** On every load, the
  firmware raises CPU_ECC_ERROR / CPU_ECC_FATAL notifications with
  `memory_bitmap=0x00001000`. The bitmap decoder
  ([#695](https://github.com/SLM-OS/SLM-Operating-System/pull/695)) maps
  this to SAGE1_MIPI_RX_13; earlier records label the same bit
  `SAGE1_ISP`. The first event fires right after CLEAR_CONFIGURED_APPS,
  and roughly one more follows each later load RPC. Across the 9 valid
  boots the averages were 4.4 correctable and 5.2 fatal events per load.
  Instrumented Linux `hailo_pci` saw **zero** such events across 10 boots
  plus inference. In every sample, ECCs and the wedge occur together, so
  the data cannot show whether one causes the other or whether both are
  symptoms of one firmware-state divergence.
- **The PCIe fabric is clean.** The root-complex bridge shows
  `phylinkup=1 dl_active=1` and no AXI errors. After the timeout the
  endpoint reports `DEVSTA.UR=1` without `rcv_mabort`, an Unsupported
  Request that the endpoint detected internally. It did not come from a
  dropped TLP.

**Reference behaviour on the same hardware.** In
`hailortcli run mnist.hef`, the firmware pre-arms ch=2 during load, with
device-side `dev_proc=0x00020002` at the first submit. `num_proc` then
advances on every frame. Measured throughput was 4,332 FPS (2026-05-07)
and 11,481 FPS (2026-05-25).

---

## 3. Methodology

The investigation compared SLM-OS against HailoRT on the same hardware at
every layer the host can see. Each hypothesis got its own deploy, changing
one variable at a time on `pi-5-1`. Disconfirmed experiments were kept
behind build flags or as defensive parity code, so they can be re-run.

### 3.1 Reference captures and symmetric tracing

- **HailoRT wire captures.** These are byte-level dumps of every
  fw_control request HailoRT sends for MNIST (27 RPCs) and MobileNet. They
  were decoded with the published `hailort-control-protocol.h` layout,
  including a 20-byte request header with a `parameter_count` field.
- **Instrumented `hailo_pci`.** This is a source patch to the Linux driver
  that logs RPCs, `set_num_avail` calls, and firmware notifications, using
  the same phase/mechanism trace format SLM-OS emits.
- **Boundary-trace toolkit**
  ([#780](https://github.com/SLM-OS/SLM-Operating-System/pull/780),
  [#783](https://github.com/SLM-OS/SLM-Operating-System/pull/783)). This
  is per-phase, per-mechanism tracing on the SLM-OS side, built with
  `HAILO_WIRE_DEBUG=ON`. It dumps channel, descriptor, and IRQ state at
  submit and at timeout. With the same format on both sides, the traces
  can be diffed line by line.

### 3.2 QEMU-stub capture of HailoRT ([#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795))

The aim was to record exactly what HailoRT does to the device without
swapping SD cards for every step:

1. A QEMU device model (`hw/misc/hailo8.c`) claims `1e60:2864` and logs
   every BAR0/BAR2/BAR4 access made by HailoRT running in an x86 VM.
2. When HailoRT reads an offset the stub cannot answer, the VM freezes.
   SLM-OS's `hailo replay-step <N>` then replays the first N captured
   writes on the real Hailo and records the real response.
3. That response is added to a corpus, and HailoRT is re-run.
   Re-validation passes check the whole trace periodically, so that an
   early wrong stub answer cannot quietly steer HailoRT down the wrong
   code path.

The pipeline cleared its Phase 1 exit gate on 2026-05-12: 46 corpus
operations, about 144 s per iteration, and byte-identical corpora across
two determinism runs. It also exposed a **structural blind spot**. The
stub did not model the per-channel VDMA registers in BAR2, and the SLM-OS
trace did not cover BAR4. The corpus and the trace therefore covered
mutually exclusive register ranges, and the wedge happens in BAR2.

### 3.3 Linux ftrace capture of HailoRT's BAR2 traffic ([#997](https://github.com/SLM-OS/SLM-Operating-System/pull/997))

To fill that gap, `scripts/capture-hailort-ftrace.sh` places kprobes on
`iowrite*`/`ioread*` while HailoRT runs on Pi OS.
`hailo_re_driver/ftrace_translate.py` maps the results to BAR-relative
operations. `hailo-re-native-diff --source ftrace` aligns them against the
SLM-OS trace.

The first side-by-side diff (2026-05-25) found that HailoRT touched only
ch=2 and ch=16, while the SLM-OS smoke path armed ch=0 and ch=1. This led
to the one real fix (§4.2).

### 3.4 Byte-level diffs

After #999, every host-visible surface was diffed directly:

| Surface | How it was captured | Outcome |
|---|---|---|
| Control-RPC request bytes (all load-time RPCs) | HailoRT wire capture compared with SLM-OS `HAILO_WIRE_DEBUG` dumps | Identical, apart from 16 `dma_address` bytes across the four contexts. Those differ only because the allocators pick different IOVAs: HailoRT `0x10_3a…` vs SLM-OS `0x10_01…`. |
| BAR0 register footprint | HailoRT MMIO trace | Same 6 offsets, values, and order: IMASK, ISTATUS, per-channel SRC/DST, ATR0, and the FW_TRIGGER doorbell |
| BAR2 channel-register sequence | Linux ftrace compared with the SLM-OS boundary trace | The first 9 writes are identical (8× ch=16 prefetch, then ch=2 `avail=2`). After that, HailoRT keeps posting while SLM-OS polls. |
| Descriptor rings (IN and OUT, all 32 slots, 16 B each) | `hailo dma-dump` ([#1010](https://github.com/SLM-OS/SLM-Operating-System/pull/1010)) and a Linux `hailo_pci` `dma_dump` hook | Two divergences were found and corrected ([#1012](https://github.com/SLM-OS/SLM-Operating-System/pull/1012)). After that, the rings are identical. |
| CCW payload and boundary buffers in host RAM | Same pair of dumps | Identical |

The DMA-content diff was named in advance as the last host-observable
test ([`hailo-dma-content-diff-plan.md`](hailo-dma-content-diff-plan.md)
§"Definitive-ness of the result"). If every region matched and the wedge
persisted, host-side investigation would be finished. That is what
happened.

---

## 4. Hypotheses tested

Every row was tested on `pi-5-1` hardware unless the Test column says
"source read". "Kept" means the change stayed in tree for parity with
HailoRT even though it did not affect the wedge.

### 4.1 First pass — #682 (2026-05-07 → 2026-05-12)

| # | Hypothesis | Test | Result | Evidence |
|---|---|---|---|---|
| A1 | IN[1] descriptor is malformed (`page=272`, `ctrl=0x2e`) | Compared with the HailoRT VDMA trace | Disconfirmed. `0x2e` is HailoRT's last-descriptor pattern, and 784 B = 512 + 272. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A2 | The IRQ cycle is missing the `IMASK_HOST=0` step that Linux does | Added it | Disconfirmed, kept | [#688](https://github.com/SLM-OS/SLM-Operating-System/pull/688) |
| A3 | The firmware needs HailoRT's ~5 ms gap between OUT and IN | Added the delay | Disconfirmed. HailoRT's gap is ioctl overhead. | [#689](https://github.com/SLM-OS/SLM-Operating-System/pull/689) |
| A4 | CHANGE_STATUS(ENABLED) fires too early | Deferred it to runmodel | Disconfirmed | [#690](https://github.com/SLM-OS/SLM-Operating-System/pull/690) |
| A5 | The PCIe inbound translation window caps reachability | Read `bcm2712.dtsi` `dma-ranges` and the brcmstb setup | Disconfirmed. A single 64 GiB window covers every allocation. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A6 | Per-channel SRC/DST IRQ-enable bits are not set for ch=2/ch=16 | Read back at 4 points, then a read-modify-write | Disconfirmed. These registers are write-1-to-clear pending status, not enables. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A7 | Action-stream content is wrong (missing REPEATED_ACTION wrapper, actions, or fields) | Byte diff of all 4 contexts against HailoRT | Disconfirmed. Only IOVA bytes differ. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A8 | A 4-ping barrier is needed before ENABLED | Added GDI/ID/GDI/ID | Disconfirmed | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A9 | The same pings are needed inside runmodel, at HailoRT's live timing | Moved them between the OUT prefetch and the IN bump | Disconfirmed | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A10 | The firmware VDMA path depends on the host's MSI setup | Polling only, with MSI registration removed | Disconfirmed. The wedge is identical. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A11 | The avail value never propagates to the device-side channel half | BAR2 layout review against `hailo-vdma-common.c` | Disconfirmed. This was a misreading: +0x10 is the independent `device_regs` block for a SRC channel. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A12 | PCIe bridge errors or a wrong SCB0 size (hyp-U/V/W) | Dumped and cleared RC/EP error state; widened SCB0 to Linux's encoding | Disconfirmed. The bridge is clean. The SCB0 change was kept. | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A13 | Stale pending IRQs (hyp-X-1), the ATR configuration, or diagnostic MMIO (hyp-X-2) | Drained IRQs before submit; audited ATR[0..3]; built a production build without `HAILO_WIRE_DEBUG` | Disconfirmed | [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) |
| A14 | Prefetch=1 (hyp-R), holding the device in D3 (hyp-S), or VDM QoS (hyp-T) | A/B on hardware | Disconfirmed | [`fact-sheets/gpu-inference.md`](fact-sheets/gpu-inference.md) |
| A15 | Settle time before submit, or before CLEAR_CONFIGURED_APPS | 500 ms delay at each point | Disconfirmed | [`hailo-protocol-architecture.md`](hailo-protocol-architecture.md) |
| A16 | Skipping CLEAR_CONFIGURED_APPS or RESET | Removed each one | Disconfirmed. Skipping CLEAR fails the load (`0x40130016`). Skipping RESET raises ECC events from 9 to 20 and the wedge remains. | [`hailo-protocol-architecture.md`](hailo-protocol-architecture.md) |
| A17 | Draining notifications before submit disturbs firmware state | Made notification handling IRQ-driven, as in Linux | Disconfirmed, kept | [#793](https://github.com/SLM-OS/SLM-Operating-System/pull/793) |

### 4.2 The real defect: VDMA channel direction ([#999](https://github.com/SLM-OS/SLM-Operating-System/pull/999))

| | |
|---|---|
| **Hypothesis** | The host arms different VDMA channels from the ones the firmware was told to use. |
| **Test** | The first symmetric BAR2 diff (§3.3) on the `hailo infer` smoke path. HailoRT touched only ch=2 and ch=16. SLM-OS touched ch=0 and ch=1, and read `num_proc` 4,774 times without it moving. |
| **Root cause** | Two host-side call sites (`hailo_shell.c` and `inference_device_hailo.c`) hardcoded `.input_channel = 0, .output_channel = 1`. The context-switch translator, meanwhile, told the firmware ch=2 and ch=16 in the `OPEN_BOUNDARY_*` actions. |
| **Fix** | Both call sites now derive the indices from the translator's constants (`HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL + HAILO_CS_BOUNDARY_{INPUT,OUTPUT}_CHANNEL_OFFSET`). `hailo_infer_run` now rejects inputs in the D2H range [16, 31] and outputs in the H2D range [0, 15]. Four regression tests in `kernel/tests/test_hailo.c` cover this. |
| **Result** | Real and verified. After the fix, SLM-OS touches the same channels and offsets HailoRT does. **The wedge did not move.** The recorded production-path (`hailo runmodel`) logs already showed ch=2 both before and after the fix, with the same timeout signature. This makes #999 a genuine defect, but not the cause of the production wedge. |

### 4.3 Second pass — #1001 / #1002 / #1003 (2026-05-25 → 2026-05-28)

The #1001 closure comment numbers its chain as 1 fix plus 9 entries. Row
B8 below adds the DMA-content diff itself as a tenth disconfirmed
hypothesis: "the bytes the firmware DMA-reads from host RAM differ from
HailoRT's". That hypothesis is distinct from the two divergences the diff
turned up (B9, B10).

| # | Hypothesis | Test | Result | Evidence |
|---|---|---|---|---|
| B1 | Control RPCs should carry `sequence=0`, as HailoRT's do | A/B with sequence fixed at 0 | Disconfirmed, kept | [#1003](https://github.com/SLM-OS/SLM-Operating-System/issues/1003), [#1004](https://github.com/SLM-OS/SLM-Operating-System/pull/1004) |
| B2 | The extra pings SLM-OS sent between RESET and CLEAR_CONFIGURED_APPS disturb the firmware | A/B with the pings removed | Disconfirmed, kept | [#1003](https://github.com/SLM-OS/SLM-Operating-System/issues/1003), [#1004](https://github.com/SLM-OS/SLM-Operating-System/pull/1004) |
| B3 | The post-BOOT_IRQ settle is too short for the ECC region | 500 ms → 2,000 ms | Disconfirmed. ECC counts and the wedge were unchanged. | [#1003](https://github.com/SLM-OS/SLM-Operating-System/issues/1003) |
| B4 | The PMCSR D0→D3→D0 cycle during `hailo boot` (which Linux does not do) disturbs the firmware | `HAILO_D3HOT_AT_BOOT=OFF` build | Disconfirmed | [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) |
| B5 | MSI-driven and polled completion behave differently from the firmware's side | Source read against HailoRT v4.23 | Disconfirmed. Both use MSI first with a polling fallback, and VDMA IRQs only fire once the firmware processes a channel. | [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) |
| B6 | BAR4 request padding differs (40 B vs ≥64 B) | Source read | Disconfirmed. `FW_CODE_SECTION_ALIGNMENT = 4`, so both sides write `PO2_ROUND_UP(len, 4)` bytes. | [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) |
| B7 | The firmware cannot reach SLM-OS's IOVA range | Sentinel bytes in the IN descriptor list | Disconfirmed. The firmware reads CCW data from the same range on ch=0/ch=1. It never touches the ch=2 list at all. | [#1009](https://github.com/SLM-OS/SLM-Operating-System/pull/1009) |
| B8 | The bytes the firmware DMA-reads from host RAM differ from HailoRT's (CCW payload, descriptor lists, boundary buffers) | `hailo dma-dump` plus the Linux `hailo_pci` `dma_dump` hook, with all 8 regions diffed | Disconfirmed. Identical once B9 and B10 were aligned. | [#1010](https://github.com/SLM-OS/SLM-Operating-System/pull/1010), [`hailo-dma-content-diff-plan.md`](hailo-dma-content-diff-plan.md) |
| B9 | The ring must be fully pre-programmed: HailoRT fills all 32 slots, SLM-OS filled only the active ones | A/B | Disconfirmed, kept | [#1012](https://github.com/SLM-OS/SLM-Operating-System/pull/1012) |
| B10 | The last-descriptor LIRQ bits (`0x2e` vs HailoRT's `0x02`) matter | A/B | Disconfirmed, kept. Side finding: clearing them removes a spurious `event_id=0 ETHERNET_RX_ERROR` notification, so that notification is a firmware side effect and not a wedge signal. | [#1012](https://github.com/SLM-OS/SLM-Operating-System/pull/1012) |

Further second-pass checks that were not counted as separate hypotheses:

- **Source-reading candidates from the #1001 issue body.** These were a
  missing "go" RPC, the `stream_index`, a missing context-switch action,
  `num_avail` semantics, and the boundary buffer config. Each turned out to
  match HailoRT already, or had already been addressed in tree. The
  channel ID is not encoded in PCIe descriptor addresses: HailoRT's mask
  function returns 0 on PCIe.
- **PR #1004's load-time parity cleanup.** In addition to B1 and B2, it set
  GET_HW_CONSTS to 4 calls (was 6), added a second GDI before RESET, set
  `preliminary_run_asap=1` and `can_fast_batch_switch=1`, set
  `external_action_list_address=0`, and cut the inter-RPC settle floor
  from 50 ms to 250 µs. Each change matches HailoRT's wire capture. None
  moved the wedge or the ECC counts.
- **HEF parse coverage** ([#1008](https://github.com/SLM-OS/SLM-Operating-System/pull/1008)).
  The parser was found to skip the modern `ops[].core_op` layout, and it
  reported the CCW region at twice its real size. Both were fixed. The CCW
  bytes actually uploaded were already correct, which B8 confirmed.
- **Post-#332 regression check** (2026-05-28). After
  [#1018](https://github.com/SLM-OS/SLM-Operating-System/pull/1018)
  reworked the control-RPC wait path, the wedge was reproduced with an
  identical signature. That rules out host thread or lock state as a
  factor.

### 4.4 Earlier load-phase findings (2026-04 → 2026-05-06)

These came before the wedge was isolated. They are listed because they
shaped later method.

- A diagnosis that DYNAMIC lacked LCU and sequencer actions was
  **retracted**. The comparison had mixed up wire size (which includes a
  39-byte framing prefix) with body size. The bodies match
  ([#361](https://github.com/SLM-OS/SLM-Operating-System/issues/361)).
- Calling GET_HW_CONSTS 4 times did not suppress ECC events. APP-CPU
  settle pings moved the FATAL event off ENABLED in synthetic loads, but
  had no effect on the real MNIST load
  ([#361](https://github.com/SLM-OS/SLM-Operating-System/issues/361)).
- A load-time rejection that looked like a multi-context dispatch problem
  (`0x40130016`) also reproduced with single-context MNIST, and later
  stopped reproducing altogether. Real multi-context dispatch remains an
  unimplemented feature, unrelated to the wedge
  ([#347](https://github.com/SLM-OS/SLM-Operating-System/issues/347)).

---

## 5. Conclusion

At the point where the firmware should start consuming ch=2, SLM-OS and
HailoRT give it the same inputs on every surface the host can observe:

```
┌────────────────────── Host-observable: equal to HailoRT ──────────────────────┐
│  fw_control RPC request bytes, count, order, cadence    (BAR4 + BAR0 doorbell)│
│  BAR0 IRQ / ATR / doorbell register writes                                    │
│  BAR2 channel-register writes: channels, offsets, values, order               │
│  Descriptor rings, all 32 slots, IN and OUT                 (host RAM via DMA)│
│  CCW configuration payload and boundary buffers             (host RAM via DMA)│
└───────────────────────────────────────┬───────────────────────────────────────┘
                                        │ identical inputs, modulo IOVA
┌───────────────────────────────────────▼───────────────────────────────────────┐
│  Firmware-internal state (closed firmware, no symbols, no documented layout)  │
│    HailoRT host: ch=2 pre-armed during load, num_proc advances                │
│    SLM-OS host:  ch=2 avail never acknowledged, ECC events on every load RPC  │
└───────────────────────────────────────────────────────────────────────────────┘
```

The firmware gets the same bytes and still ends up in a different state.
So the deciding difference sits in firmware-internal state that the host
cannot read or set through any documented interface. The SAGE1 ECC
events, which fire under SLM-OS starting at CLEAR_CONFIGURED_APPS and
never fire under Linux, are the one externally visible sign of that
divergence. Their trigger could not be isolated either (#1003).

Not every input to the firmware has been compared exhaustively. These
classes were only partly compared:

- PCIe configuration space and link-level state set by Linux's PCI core
  and the brcmstb driver. Only SCB0 sizing, the MSI capability, and the
  error/status registers were compared.
- The firmware's boot-time history. SLM-OS boots the firmware about twice
  as fast as Linux does.
- Timing finer than the wire captures resolve.

The host cannot tell whether any of these is the cause.

**About the 2026-05-12 interim conclusion.** #682 was first closed on the
theory that HailoRT configures the device through undocumented direct
BAR4 writes and uses fw_control only for IDENTIFY. That theory came from
a kprobe that read an opcode field. Later evidence contradicts it:

- HailoRT's own wire captures decode the same 27 RPCs as the full
  load-time sequence: CHANGE_STATUS, CLEAR_CONFIGURED_APPS, GET_HW_CONSTS,
  SET_NETWORK_GROUP_HEADER, and SET_CONTEXT_INFO ×4.
- SLM-OS's bytes match those captures.

The matching call count (27) suggests the kprobe read a different field,
though this was never confirmed. The reopen comment on #682 explicitly
overturned that close reason. This write-up's conclusion — identical
host-visible inputs, divergent firmware-internal state — rests on the
later byte-level evidence.

---

## 6. Paths not taken

### 6.1 Windowed BAR4 firmware-memory inspection

**What it would involve:**

1. Snapshot firmware memory through the ATR/BAR4 window at the same
   lifecycle point on both stacks: after load, before the first submit.
   SLM-OS already has `hailo_bar4_snap` pre/post hooks under
   `HAILO_WIRE_DEBUG`. The Linux side would need a matching extension of
   the #997 instrumentation.
2. Diff the snapshots.
3. Work out which differing words are the channel-state or credit
   structures. That means reverse-engineering a closed firmware image: a
   stack of about 40 Thumb-2 sections with no symbols.

**What was done.** Only the SLM-OS-only pre/post diff, which found 5
changing dwords and confirmed the firmware is alive (#1002).

**Estimate.** Several weeks, comparable to the whole of the second pass,
because step 3 has no reference material. It was not started before the
capstone deadline, and it was not scheduled afterwards.

### 6.2 Escalation to Hailo

The evidence is ready to hand over: byte-faithful wire and DMA parity, a
deterministic reproducer, the channel-state tables in §2, and the ECC
pattern. Hailo has the firmware source and could check firmware-side
state directly. No ticket was filed. The AI HAT+ ships only with the
Linux userspace SDK, and nothing documents a bare-metal or non-HailoRT
host path.

### 6.3 Reopen criteria

Reopen this investigation only if at least one of the following holds:

- Hailo publishes the firmware/host protocol beyond
  `hailort-control-protocol.h`, releases HailoRT source, or confirms the
  firmware-side condition through a support channel.
- A new firmware revision changes boundary-channel behaviour or the
  documented RPC set.
- Firmware-memory inspection (§6.1) identifies a specific internal-state
  word that differs between the two hosts at the same lifecycle point.
- A non-HailoRT host is seen completing inference on a Hailo-8/8L using
  only documented interfaces.

A new idea about descriptor bytes, RPC ordering, settle timing, channel
registers, or DMA content is **not** enough on its own. Every one of those
surfaces has been byte-compared or A/B-tested in §4.

---

## 7. Lessons learned

1. **Run the reference stack on the same board early.** The Linux +
   HailoRT run on `pi-5-1` (2026-05-07) settled several weeks of
   hardware-versus-software uncertainty in one session. It should have
   been the first experiment after the load path came up.
2. **Use the same instrumentation on both sides.** Source reading and
   one-sided traces had judged the channel arming correct. The defect in
   §4.2 only showed up once both stacks produced the same trace format and
   could be diffed mechanically.
3. **Know what each capture cannot see.** The QEMU corpus and the SLM-OS
   trace covered disjoint BARs, so "no divergence found" said nothing
   about BAR2 until the ftrace path filled the gap. Every tool needs its
   blind spots stated.
4. **Keep a single source of truth for protocol constants.** The channel
   bug came from two hardcoded copies of indices the translator already
   owned. Range checks at the API boundary now catch direction mismatches
   immediately instead of letting them wedge silently.
5. **Check decoders and readings against known values.** Three
   conclusions were later retracted or overturned: the wire-size vs
   body-size comparison (§4.4), the misread device-side mirror register
   (A11), and the kprobe opcode reading (§5). Each rested on an
   interpretation that nobody had checked against a known ground truth.
6. **Treat sample size and correlation carefully.** ECC behaviour was
   nondeterministic in early work, and N=5 runs misled. Later, 9/9
   co-occurrence of ECC events and the wedge established that the two are
   inseparable, but not which causes which.
7. **Re-test old conclusions once a real defect is fixed.** Several
   settings in tree were justified by tests run while other divergences
   were still present, or by traces that could not be reproduced. One
   example: the 50 ms inter-RPC floor was about 250× slower than HailoRT
   with no measurable benefit. PR #1004 brought them in line with the
   wire capture.
8. **Keep disconfirmed experiments, and write down the ruled-out set.**
   Flag-gated experiment code and parity changes kept in tree let any
   hypothesis be re-run cheaply. The ruled-out tables stop the same ground
   being covered again.
9. **Decide the final test before running it.** Naming the DMA-content
   diff in advance as the last host-observable test made the stopping
   point a fact rather than a judgment call. A thorough negative result,
   with the boundary precisely located, is a legitimate deliverable.
10. **With a closed accelerator, matching bytes does not guarantee matching
    state.** When the firmware and runtime are both closed, a host can
    match every byte it sends and still fail, because the deciding state
    lives where only the vendor can see it.

---

## Artifacts

**In-tree tooling** (reusable for any future Hailo bring-up):

- Boundary-trace toolkit: `kernel/ai_accel/hailo/hailo_trace.{h,c}`,
  `scripts/capture-hailo-trace.sh`
- Linux-side BAR capture: `scripts/capture-hailort-ftrace.sh`,
  `host-tools/hailo-re-driver/` (ftrace translator, `hailo-re-native-diff`)
- QEMU-stub RE pipeline: `hailo replay-step`;
  [`hailo-re-corpus-format.md`](hailo-re-corpus-format.md),
  [`hailo-re-replay-step.md`](hailo-re-replay-step.md)
- DMA-content dump: `hailo dma-dump <handle>`
- Diagnostics under `HAILO_WIRE_DEBUG=ON`: CPU_ECC bitmap decoder, IRQ
  state readback, bridge-error dump, BAR4 pre/post snapshot, CCW
  fingerprint dump

**Captures.** These are kept outside the repository, in the developer's
local reference cache under `~/slmos-ref/derivatives/`:

- `slmos-traces/hailort-linux-mnist-pi-5-1-2026-05-25.log`: HailoRT BAR2
  baseline
- `slmos-traces/slmos-runmodel-mnist-pi-5-1-2026-05-25-postfix.txt`:
  SLM-OS trace after #999
- `slmos-traces/slmos-dma-dump-mnist-pi-5-1-2026-05-27.txt`: SLM-OS DMA
  content
- `slmos-traces/quant_1002_v3_20260525T155831/`: 10-boot ECC/wedge
  quantification
- `hailort-traces/hailort-v4.23.0-trace-instrumentation-v2.patch`,
  `hailort-traces/hailort-v4.23.0-dma-dump.patch`: Linux `hailo_pci`
  instrumentation

## Related documents

- [`hailo-protocol-architecture.md`](hailo-protocol-architecture.md):
  protocol reference and ruled-out list
- [`pi5-ai-hat-plan.md`](pi5-ai-hat-plan.md): phase-by-phase bring-up
  history (§8–§10 cover this investigation)
- [`hailo-dma-content-diff-plan.md`](hailo-dma-content-diff-plan.md):
  the final host-observable test
- [`hailo-lifecycle.md`](hailo-lifecycle.md): boundary-channel state
  machine and BAR2 register layout
- [`fact-sheets/gpu-inference.md`](fact-sheets/gpu-inference.md):
  cross-platform accelerator status

*Last updated: 18 October 2026*
