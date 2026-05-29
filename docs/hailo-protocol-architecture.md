# Hailo NPU Protocol Architecture

**Status:** Reference document, anchored to empirical findings on Hailo-8L AI HAT+ with firmware v4.23 on Raspberry Pi 5. Initial capture 2026-05-12 (§"Empirical evidence" — Linux ftrace + kprobe on `hailo_pcie_write_firmware_control`). Extended 2026-05-28 (§"Update 2026-05-28" — host-side wire byte-diff + DMA-content byte-diff, [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) closed exhausted). Re-validate before assuming the same shape for future firmware revisions or different Hailo SoCs.

## TL;DR

Hailo's NPU configuration uses **two protocols simultaneously**:

1. **fw_control RPC protocol** — documented in `hailort-control-protocol.h`. Opcodes for IDENTIFY, READ_MEMORY, READ_LOG, RESET_NN_CORE, CHANGE_CONTEXT_SWITCH_STATUS, CLEAR_CONFIGURED_APPS, GET_HW_CONSTS, SET_NETWORK_GROUP_HEADER, SET_CONTEXT_INFO, etc. **All accepted by firmware**, but insufficient on their own to fully wire boundary channels into the inference data path on v4.23.

2. **Direct BAR4 memory writes** — undocumented. HailoRT (the proprietary userspace runtime) mmaps the device's PCIe resources and writes configuration directly into specific BAR4 regions. This is the **primary configuration path** — fw is designed around it. No public schema exists for the layout / semantics of these writes.

A host that uses only the documented fw_control RPC protocol completes ~90% of the load (firmware accepts every RPC with rc=0) but cannot complete the final channel-to-inference-path binding. The wedge surfaces as: `hailo runmodel <h>` writes `num_avail=2` to ch=2; firmware never advances `num_proc`; timeout after 500 ms.

This is the root cause of [#682](https://github.com/SLM-OS/SLM-Operating-System/issues/682) (closed 2026-05-12 as Hailo-side architectural limit) and its downstream [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) (closed 2026-05-28 after a second-pass disconfirmation chain — see §"Update 2026-05-28" below for the additional evidence).

## Empirical evidence

Captured on `pi-5-1` 2026-05-12 by reloading Linux's `hailo_pcie` kernel module with the SLM-OS v2 trace patch (`~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-trace-instrumentation-v2.patch`) plus a kprobe on `hailo_pcie_write_firmware_control` that dereferences the request payload's opcode field.

**Linux HailoRT MNIST run: 84,003 inferences over 2 seconds + load + teardown.**

| Function | Calls | Notes |
|---|---|---|
| `hailo_pcie_write_firmware_control` | 27 | **100% opcode=0 (IDENTIFY)** |
| `hailo_pcie_read_firmware_control` | 27 | Paired response reads |
| `hailo_pcie_read_firmware_notification` | 0 | fw never raised FW_NOTIFICATION_IRQ |
| `hailo_pcie_read_interrupt` | 51,910 | ~1 per inference (IRQ-driven completion) |
| `hailo_vdma_program_descriptors_in_chunk` | 0 | Descriptors programmed elsewhere |

HailoRT's NNC ioctl surface (`~/slmos-ref/hailo/v4.23.0/linux/pcie/src/nnc.c:213`) exposes exactly four operations:

```c
case HAILO_FW_CONTROL:            // → hailo_fw_control (the 27 calls above)
case HAILO_READ_NOTIFICATION:     // → hailo_read_notification_ioctl
case HAILO_DISABLE_NOTIFICATION:  // → hailo_disable_notification
case HAILO_READ_LOG:              // → hailo_read_log_ioctl
```

There is **no `HAILO_SET_CONTEXT_INFO` ioctl, no `HAILO_CONFIGURE_NETWORK` ioctl**. The actual configuration work — context-switch state, channel enables, descriptor list contents, transfer launches — all goes through the VDMA ioctl namespace (`HAILO_VDMA_ENABLE_CHANNELS`, `HAILO_VDMA_LAUNCH_TRANSFER`, `HAILO_VDMA_BUFFER_MAP`, ...), and the kernel module mediates DMA mappings but does not see the BAR4 write contents — those happen in userspace against mmap'd PCIe resources.

The HailoRT log corroborates: every `control__parse_identify_results` line is one of the 27 fw_control calls. No log entries about SET_CONTEXT_INFO, SET_NETWORK_GROUP_HEADER, or CHANGE_STATUS exist on the HailoRT side.

Capture artifacts (local-only, paths reference each developer's machine):
- `~/slmos-ref/derivatives/hailort-traces/hailort-mnist-ftrace-pi5-1-2026-05-12.txt`
- `~/slmos-ref/derivatives/hailort-traces/hailort-mnist-kprobe-pi5-1-2026-05-12.txt`
- `~/slmos-ref/derivatives/hailort-traces/hailort-mnist-hrtlog-pi5-1-2026-05-12.log`

## How Hailo's design compares to other PCIe accelerator vendors

Hailo combines four properties that, taken together, are uncommon among PCIe accelerator vendors:

1. **Closed firmware** — `hailo8_fw.bin` is shipped as a binary blob; no source.
2. **Closed userspace runtime** — `libhailort.so` is shipped as a binary; no source.
3. **Undocumented primary configuration protocol** — the BAR4 writes HailoRT issues during `Configure()` and `Activate()` have no public schema.
4. **Documented RPC interface is intentionally incomplete** — sufficient for inspection (IDENTIFY, READ_MEMORY) and one-shot operations (RESET_NN_CORE), insufficient for primary configuration.

How this compares against other PCIe accelerators:

| Vendor / Family | Kernel-only config supported? | Documentation level |
|---|---|---|
| NVIDIA GPUs (GA10B, GA107, …) | ✅ — nouveau works without proprietary userspace | Kernel modules open-sourced (`open-gpu-kernel-modules`). SLM-OS dispatches GA10B compute kernels successfully. |
| Intel iGPUs (i915 / Xe) | ✅ | Open-source driver, public PRM docs |
| AMD GPUs (amdgpu / radv / amdkfd) | ✅ | Open-source driver, public ISA + register docs |
| Google Coral Edge TPU | Partial — `libedgetpu` is closed userspace, but PCIe Coral has open kernel interface | Open-source kernel module + closed userspace |
| Standard PCIe (NVMe, NICs, USB controllers, …) | ✅ | Datasheet-documented MMIO |
| **Hailo-8 / Hailo-8L** | ❌ for full functionality — fully exercising the hardware requires HailoRT userspace | **Closed firmware + closed userspace + undocumented primary protocol** |

Hailo sits alone in the "fully closed black-box accelerator" corner. This is a valid commercial design choice — it lets Hailo iterate the firmware/runtime contract without breaking customers — but it constrains who can drive the hardware to "users running HailoRT on a supported Linux distribution".

## What this means for non-HailoRT hosts

Any host that wants to drive a Hailo NPU without HailoRT (SLM-OS, custom embedded runtime, different OS, microcontroller bringup) has three options:

- **Use the documented fw_control RPC interface.** Reaches ~90% of load. Firmware accepts every RPC. Boundary channels exist, descriptors get programmed, num_avail bumps land. But the final channel-to-inference-binding step lives in the undocumented direct-memory protocol — submit-time wedge results.
- **Reverse-engineer HailoRT's BAR4 protocol.** Read `~/slmos-ref/hailo/v4.23.0/libhailort/src/`, instrument HailoRT in gdb, capture every BAR4 write during `Configure()` + `Activate()`. Replicate. Weeks of work; produces an officially-unsupported configuration path that may break on the next firmware update.
- **Wait for Hailo to publish the protocol.** Or release HailoRT source. As of 2026-05-12 neither is publicly committed to.

## Issue #682 ruled-out causes

These hypotheses were investigated and **disproven** during the #682 bisection. Do not re-investigate without new evidence.

| Hypothesis | Status | Evidence |
|---|---|---|
| Descriptor content (ps_ctrl, page_size, data_id) | DISPROVEN | Bit-identical to Linux MNIST reference VDMA trace. IN[0]=0x00020002, IN[1]=0x0001102e. |
| Channel state at submit | DISPROVEN | Both IN ch=2 and OUT ch=16 STARTED (ctrl=0x01), did=0/4 host/device split correct |
| Pre-submit drain perturbation | DISPROVEN | PR #793 made notification handling IRQ-driven (matches Linux); wedge unchanged |
| Settle timing pre-submit | DISPROVEN | 500 ms udelay before IN submit: null effect |
| Settle timing pre-CLEAR_CONFIGURED_APPS | DISPROVEN | 500 ms udelay before first CORE-CPU RPC: null effect |
| Skipping CLEAR_CONFIGURED_APPS | DISPROVEN | Load fails earlier at SET_CONTEXT_INFO with major=0x40130016 |
| Skipping CHANGE_CONTEXT_SWITCH_STATUS(RESET) | DISPROVEN | Total ECC count rises 9→20, wedge persists. RESET suppresses ECCs, not causes them. |
| CPU_ECC_ERROR on SAGE1_MIPI_RX_13 | NOT THE CAUSE | fw boot-scrub artifact on unused MIPI block (no camera). Decoupled from wedge timing per multiple bisects. |
| HEF-specific MNIST issue | RULED OUT | Wedge reproduces shape-identically across other model attempts; descriptor list shape matches Linux exactly for MNIST. |
| BAR4 misalignment | RULED OUT | All bar4_write/read paths are 32-bit aligned; verified empirically via wire-debug. |
| Cache coherency (descriptor list DMA) | RULED OUT | Descriptor list is cleaned via `cache_clean` before submit; HAILO_WIRE_DEBUG dump from DRAM matches programmed values. |
| IN ring descriptor pre-fill (full ring vs partial) | DISPROVEN (PR #1012, hyp-X) | HailoRT pre-programs all 32 ring slots before any submit; SLM-OS pre-programmed only the first N. Aligning to full-ring pre-fill: null effect, wedge persists. |
| Last-descriptor LIRQ bit (`0x2e` vs `0x02`) | DISPROVEN (PR #1012, hyp-Y) | HailoRT clears the LIRQ bit on the final descriptor of the transfer; SLM-OS left it set. Aligning to the cleared form: null effect on wedge (side-finding: clearing LIRQ also suppressed the spurious `event_id=0 ETHERNET_RX_ERROR` d2h notification, which is therefore a fw side-effect of LIRQ, not a wedge signal). |
| DMA buffer content (CCWS + boundary desc-list payload) | RULED OUT (PR #1010, 2026-05-27) | Per the plan in `docs/hailo-dma-content-diff-plan.md` — full host-RAM content dump on both sides; byte-faithful match across cfg-channel CCWS and boundary IN/OUT desc-list payloads. fw rejects ch=2 dispatch on something invisible to host-observable host wire AND host RAM. |

## Update 2026-05-28: DMA content byte-faithful, wedge persists

Between 2026-05-25 and 2026-05-28 the BAR4-RE attempt (PR #795 et seq.) and the DMA-content byte-diff plan (this file's neighbour `hailo-dma-content-diff-plan.md`) were executed end-to-end against [#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) — the downstream "host has all observable wedge state matching HailoRT, fw still rejects ch=2" follow-on to #682.

**Ten additional hypotheses** were tested on hardware (`pi-5-1`) during that pass: channel-direction, seq=0, drop-pings, longer-settle, PMCSR cycle, MSI/polled-only, BAR4 padding, IOVA reachability, full-ring pre-program (hyp-X), and last-descriptor LIRQ clear (hyp-Y). All ten disconfirmed.

**Two real wire divergences were found** (full-ring pre-program + LIRQ-clear) and corrected as defensive parity in PR [#1012](https://github.com/SLM-OS/SLM-Operating-System/pull/1012). Neither moved the wedge. They are kept in-tree because byte-parity with HailoRT is independently load-bearing for any future investigation.

**The decisive new evidence** is the DMA-content byte-diff. Prior bisections had only proven the *wire* bytes (MMIO writes into BAR0/BAR2/BAR4) were byte-faithful between SLM-OS and HailoRT. The 2026-05-27 capture extended that to **host-RAM content read via DMA**: CCWS data at the cfg-channel IOVAs, and the full 256-byte boundary descriptor-list payloads (IN + OUT, all 32 slots each). Both byte-faithful to HailoRT. The wedge persists.

This closes the host-observable surface area. Every byte SLM-OS hands to firmware — by MMIO write, by DMA read, in every order, with every observable timing — matches HailoRT. The firmware still refuses to advance `num_proc` on ch=2.

**Remaining paths forward** (all external to the host-observable surface):

- fw-memory inspection via BAR4 windowed reads — extend the PR #997 framework from BAR0/BAR2 to BAR4 and dump fw-internal state pre/post-load.
- Hailo support escalation — present the byte-faithful evidence and request fw-side debug.
- Live HailoRT decompilation / dynamic instrumentation under userspace gdb to capture the BAR-write sequence Linux uses for the same load.

None of these can land before the SLM-OS capstone deadline. #1001 is closed and the chain accepted as the most thorough negative result the project can produce on host hardware alone.

## Reopen criteria

#682 should only be reopened if one of these conditions holds:

- Hailo publishes the BAR4 configuration protocol (or releases HailoRT source).
- A new firmware revision exposes additional fw_control opcodes that complete channel binding.
- A reverse-engineering effort produces a verified mapping from HailoRT operations to BAR4 writes **AND** demonstrates that replicating those writes alone unblocks the wedge. (Note: a partial RE attempt was made 2026-05-25 → 2026-05-28; producing byte-faithful host wire + DMA content was not sufficient — see §"Update 2026-05-28". A re-attempt that does not address what fw reads from BAR4 *itself* post-load will reproduce the same exhaustion.)
- A different host runtime (not HailoRT) is observed completing MNIST inference on Hailo-8L using only documented interfaces.

Mere "I have a new hypothesis about descriptor bytes / settle timing / channel state / wire ordering / DMA content" is not sufficient — those surfaces have been exhaustively bisected across two passes (the 2026-05-12 #682 chain and the 2026-05-28 #1001 chain) and are closed.

## Capture procedure for future Hailo investigations

The boundary-trace toolkit (PRs [#780](https://github.com/SLM-OS/SLM-Operating-System/pull/780), [#783](https://github.com/SLM-OS/SLM-Operating-System/pull/783)) is the first move on any new Hailo wedge:

> Reference-cache paths below (`~/slmos-ref/...`) are developer-local — the reference cache is a flat tree on each developer's machine, not checked into the repo. See memory `reference_cache_location.md` or the top-level `CLAUDE.md` "Reference File Cache" section.

1. **SLM-OS side.** Build with `make kernel PLATFORM=RASPI5 HAILO_WIRE_DEBUG=ON`. Deploy via `scripts/capture-hailo-trace.sh --phase all --mech irq,rpc,dma --scenario <name> --shell-cmd "..."`. Output lands under `~/slmos-ref/derivatives/slmos-traces/`. Provides per-phase + per-mechanism trace lines plus channel/descriptor/desc-status dumps on timeout.

2. **Linux side.** Swap to Pi OS card on the lab board. Reload `hailo_pci` with trace masks: `modprobe -r hailo_pci && modprobe hailo_pci trace_phase=0x3f trace_mech=0x0e`. The v2 trace patch is at `~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-trace-instrumentation-v2.patch`. Provides cross-correlatable trace lines.

3. **Linux fw_control inspection.** Use ftrace + a kprobe on `hailo_pcie_write_firmware_control` to log opcodes. Example used 2026-05-12:
   ```sh
   echo "p:hailo_fw_ctl_write hailo_pcie_write_firmware_control opcode=+28(%x1):u32" \
       > /sys/kernel/debug/tracing/kprobe_events
   echo 1 > /sys/kernel/debug/tracing/events/kprobes/hailo_fw_ctl_write/enable
   echo 1 > /sys/kernel/debug/tracing/tracing_on
   hailortcli run --time-to-run 2 /path/to/model.hef
   echo 0 > /sys/kernel/debug/tracing/tracing_on
   cat /sys/kernel/debug/tracing/trace
   ```
   The `+28(%x1)` dereferences `command->buffer[8]` (opcode field inside the wire-format common header). Per the empirical capture, HailoRT issues only opcode=0 (IDENTIFY) through this path.

4. **Cross-correlation.** Linux's LINKUP / FW_BOOT / POSTBOOT phase markers fire on both sides cleanly. MODEL_LOAD / INFERENCE phases fire only on SLM-OS — see memory `hailo_trace_toolkit.md` for why.

## Related files

- Source: `kernel/ai_accel/hailo/` (control protocol implementation, VDMA, context-switch translator)
- Source: `kernel/inference/inference_device_hailo.c` (backend integration)
- Plan: `docs/pi5-ai-hat-plan.md` (Phase 8 closeout points here)
- Review: `docs/archive/investigations/hailo-ai-hat-architecture-review.md` (audit findings — some hypotheses there are now disproven; this doc is the current ground truth)
- Lifecycle: `docs/hailo-lifecycle.md` (boundary-channel state machine)
- Toolchain: `docs/hailo-toolchain.md` (HEF compilation pipeline)
- Trace toolkit: `scripts/capture-hailo-trace.sh`, `kernel/ai_accel/hailo/hailo_trace.{h,c}`
- Linux v2 trace patch: `~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-trace-instrumentation-v2.patch` (developer-local)
