# Hailo AI HAT+ Architecture and Implementation Review

Date: 2026-04-24
Branch reviewed: `ai-hat-audit`
Scope: static review only. No Hailo hardware was used, and no production code was changed.

> **Status update 2026-04-26 (post-this-review):** all 11 audit findings (F-01..F-11) have been addressed and merged via PR #355. Subsequent hardware bisect work (PR #359 `hailo-ushim`, PR #405 BIST + D3hot + fwlog) has **disconfirmed the strongest hypothesis in this review** — DMA reachability/coherency for the descriptor list is no longer the leading suspect. Replaying SLM-OS's exact byte sequences through the official `hailo_pci` ioctl path on Pi OS produces the identical hang, so the issue is not in our wire format, descriptor geometry, or bare-metal MMIO/cache path. Firmware faults at PC=`0x9000018c` during a boundary-credit poll loop at PC=`0x90004520`; the CPU_ECC bit-12 region (SAGE1_ISP per the BIST top-block enum) is the believed root-cause locus, but it cannot be tested directly via BIST (whitelist excludes bit 12) and decoding the PC requires Hailo firmware symbols. See `docs/hailo-support-ticket-draft.md` for the consolidated state, and `docs/pi5-ai-hat-plan.md` §"Phase 8 progress — 2026-04-26" for the post-review investigation log. The recommendations in this document are still valid for code-quality reasons but are not, by themselves, expected to unblock #253.

> **Status update 2026-05-12 (this review is largely historical now):** the boundary-input wedge has been investigated to root cause and the ECC hypothesis from the April update has also been **disproven** as the wedge cause (it's a real fw boot-scrub artifact on unused SAGE1_MIPI_RX_13, but decoupled from the wedge timing — see `docs/hailo-protocol-architecture.md` §"Issue #682 ruled-out causes"). The actual cause is **protocol-level**: HailoRT uses fw_control RPCs only for IDENTIFY and does all real configuration via direct BAR4 mmap writes from userspace, on an undocumented protocol that SLM-OS cannot reproduce without reverse-engineering HailoRT. **#682 has been closed as a Hailo-side architectural limit.** Treat the recommendations in this review as code-quality / structural improvements; the inference-blocker analysis here is superseded by `docs/hailo-protocol-architecture.md`, which is the current ground truth on the wedge's root cause and the full ruled-out hypothesis list.

## Executive summary

The Hailo AI HAT+ work is significantly past "device detected" bring-up: it has BCM2712 `pcie1` link training, Hailo BAR mapping, firmware boot/control RPCs, context-switch command emission, CCW VDMA upload, boundary VDMA setup, Lua-facing model sizing, and a Raspberry Pi OS/HailoRT trace comparison. That is a large amount of difficult platform work.

The current implementation is not yet a general Hailo AI HAT+ inference backend. It is best described as a Hailo-8L/MNIST-oriented bring-up backend with partial HEF parsing and several HailoRT byte-replay elements. The highest-risk areas are not the high-level inference API; they are the low-level DMA contract, the context-switch translator, and state management around VDMA rings.

The specific "can't get inference data back" blocker appears, from the docs and traces, to start before output data return. Firmware accepts enough context to reach boundary input submission, but the input H2D VDMA channel does not advance `num_proc`, and the first descriptor status remains zero. That means the device most likely did not successfully read/process the boundary input descriptor list. The strongest offline root-cause candidate is therefore DMA reachability/coherency for the descriptor list or buffer, not the user-facing output path.

The most important evidence is the VDMA trace comparison in `~/slmos-ref/derivatives/notes/hailort-trace-findings-vdma-2026-04-23.md`: HailoRT writes the input channel `num_avail` and `num_proc` advances, while SLM-OS writes the same class of channel registers and `num_proc` stays zero. The same doc notes an address-pattern split: CCW DMA from a low host physical address worked, while the failing boundary input descriptor list was near the top of 4 GB. Current code has moved some boundary allocations to a "low" allocator, but that allocator is only low-biased, not bounded to a proven DMA-safe aperture.

## Architecture reviewed

The feature spans these layers:

- Raspberry Pi 5 PCIe root complex: `kernel/drivers/pcie/pcie_bcm2712.c`
- Hailo platform shim and DMA mapping: `kernel/ai_accel/hailo/hailo_pi5.c`
- Hailo core/control protocol: `kernel/ai_accel/hailo/hailo_core.c`, `hailo_control.*`
- HEF parsing and context-switch translation: `kernel/ai_accel/hailo/hef_parser.c`, `hailo_cs_translator.*`
- VDMA descriptors/channels/tensors: `kernel/ai_accel/hailo/hailo_vdma.*`, `hailo_tensor.*`
- Inference backend integration: `kernel/inference/inference_device_hailo.c`
- Shell/scheduler/Lua call sites: `kernel/ai_accel/hailo/hailo_shell.c`, `kernel/sched/ai/sched_ai.c`, `kernel/src/lua_slm.c`
- Host-side comparison tooling: `host-tools/hailo-ushim/`

Primary internal references used:

- `docs/pi5-ai-hat-plan.md`
- `docs/hailo-support-ticket-draft.md`
- `~/slmos-ref/derivatives/notes/hailort-trace-findings-2026-04-22.md`
- `~/slmos-ref/derivatives/notes/hailort-trace-findings-vdma-2026-04-23.md`
- `~/slmos-ref/derivatives/notes/hailort-vs-slmos-source-comparison.md`
- `~/slmos-ref/derivatives/notes/hailo-driver-notes.md`
- `~/slmos-ref/hailo/hailo-pcie.c`
- `~/slmos-ref/rpi/rpi-linux-pcie-brcmstb.c`
- `~/slmos-ref/linux/linux-bcm2712.dtsi`

Primary external references checked:

- Raspberry Pi AI HAT+ documentation: https://www.raspberrypi.com/documentation/accessories/ai-hat-plus.html
- Raspberry Pi AI software documentation: https://www.raspberrypi.com/documentation/computers/ai.html
- HailoRT repository: https://github.com/hailo-ai/hailort
- Hailo PCIe driver repository: https://github.com/hailo-ai/hailort-drivers

The external references matter mainly as sanity checks: Raspberry Pi documents the AI HAT+ as a Pi 5 Hailo NPU accessory using the host Pi memory, and its expected Linux logs include the Hailo PCIe driver enabling 64-bit DMA, using userspace VDMA buffers, forcing a descriptor page size, and disabling ASPM L0s. Hailo's own repositories describe the PCIe driver as the component responsible for firmware load, device communication, and host/device data transfer. SLM-OS is reimplementing a large part of that stack in-kernel.

Key local evidence anchors:

- DMA mapping and low allocator: `kernel/ai_accel/hailo/hailo_pi5.c:241`, `kernel/ai_accel/hailo/hailo_pi5.c:246`, `kernel/ai_accel/hailo/hailo_pi5.c:269`, `kernel/mm/pmm.c:525`
- HailoRT VDMA success and low-vs-high address clue: `~/slmos-ref/derivatives/notes/hailort-trace-findings-vdma-2026-04-23.md:20`, `~/slmos-ref/derivatives/notes/hailort-trace-findings-vdma-2026-04-23.md:45`
- BCM2712 inbound window hard-coding: `kernel/drivers/pcie/pcie_bcm2712.c:728`, `kernel/drivers/pcie/pcie_bcm2712.c:732`, `kernel/drivers/pcie/pcie_bcm2712.c:740`
- Linux reference for parsed DMA aperture: `~/slmos-ref/rpi/rpi-linux-pcie-brcmstb.c:1068`, `~/slmos-ref/rpi/rpi-linux-pcie-brcmstb.c:1072`, `~/slmos-ref/linux/linux-bcm2712.dtsi:1062`
- Hailo PCIe descriptor page-size workaround: `~/slmos-ref/hailo/hailo-pcie.c:83`
- Debug default and intrusive VDMA debug path: `CMakeLists.txt:114`, `Makefile:22`, `kernel/ai_accel/hailo/hailo_vdma.c:535`
- MNIST-specific translator hooks: `kernel/ai_accel/hailo/hailo_cs_translator.c:373`, `kernel/ai_accel/hailo/hailo_cs_translator.c:411`, `kernel/ai_accel/hailo/hailo_cs_translator.c:448`
- Single-pad selection and synthetic fallback area: `kernel/inference/inference_device_hailo.c:429`, `kernel/inference/inference_device_hailo.c:1498`, `kernel/inference/inference_device_hailo.c:1502`
- VDMA absolute submit/wait helpers: `kernel/ai_accel/hailo/hailo_vdma.c:492`, `kernel/ai_accel/hailo/hailo_vdma.c:795`
- Runtime slot race and output pre-arm: `kernel/inference/inference_device_hailo.c:1655`, `kernel/inference/inference_device_hailo.c:1810`, `kernel/inference/inference_device_hailo.c:1927`
- Scheduler size mismatch: `kernel/ai_accel/hailo/hailo_shell.c:873`, `kernel/ai_accel/hailo/hailo_shell.c:884`, `kernel/sched/ai/sched_ai.c:375`, `kernel/sched/ai/sched_ai.c:506`
- Host shim documentation mismatch: `host-tools/hailo-ushim/README.md:65`, `host-tools/hailo-ushim/main.c:165`, `host-tools/hailo-ushim/main.c:181`

## Blocker reconstruction

From the local docs:

- The same board, firmware, and HEF run under Raspberry Pi OS with HailoRT.
- SLM-OS reaches firmware control, event draining, context switch setup, and CCW fetches.
- The persistent failing point is boundary input submit: H2D channel 2 is armed, `num_avail` is published, `num_proc` does not advance, and descriptor status remains zero.
- The context bytes for the critical RESET/ACTIVATION paths have been compared closely against HailoRT; several earlier wire-format bugs were fixed.
- Incremental `num_avail` was investigated and later disconfirmed as the primary issue.
- The VDMA-layer HailoRT trace shows the HailoRT channel-register sequence that succeeds: read channel base, write new `num_avail`, then read `num_proc` advanced.
- The strongest trace clue is address locality: a low CCW DMA address worked while a high boundary input descriptor-list address failed. On paper the BCM2712 inbound window should map it; empirically the working/failing split still points at DMA reachability or memory attributes.

Interpretation:

The failure currently looks less like "output data did not come back" and more like "input data was never consumed, so no output can be produced." The first fix path should prove that Hailo firmware can read the boundary input descriptor list and its target tensor memory from the exact IOVAs SLM-OS gives it.

## Major findings

### F-01: DMA-safe allocation is not a hard contract

Severity: Critical for the current blocker.

Evidence:

- `hailo_pi5.c` maps host physical memory into endpoint IOVA by adding the `pcie1` DMA offset. It has `pi5_dma_alloc_low()`, but "low" is implemented by `pmm_alloc_pages_low()`, not by a bounded DMA zone.
- `pmm_alloc_pages_low()` scans for the lowest currently available block. It does not enforce a maximum physical address, does not know a Hailo/PCIe DMA aperture, and can return high memory if low memory is fragmented or exhausted.
- Boundary tensors and descriptor lists now use low allocation in parts of `inference_device_hailo.c`, but CCW allocations still use the normal Hailo tensor/list allocation path in several places.
- `pcie_bcm2712.c` hard-codes the inbound window and SCB size instead of deriving the aperture from device tree `dma-ranges` and `brcm,scb-sizes`.
- The cached Linux `pcie-brcmstb` source reads `dma-ranges` and `brcm,scb-sizes` and validates the inbound memory view. SLM-OS does not yet model that.

Why it matters:

This is the highest-confidence defect candidate for the current stall. If the descriptor list is placed in an address range the endpoint cannot reliably reach, the channel register writes will look correct but `num_proc` and descriptor status will never advance. That matches the observed failure.

Recommendation:

- Treat "DMA addressable by Hailo" as a platform resource, not an allocator hint.
- Add a named low-DMA pool for Hailo descriptor lists and data buffers with a hard upper bound derived from measured behavior or parsed platform data.
- Log and assert the CPU physical address and endpoint IOVA for every Hailo DMA allocation, including CCW, boundary tensors, descriptor lists, context buffers if DMA-visible, and any future stream buffers.
- During hardware debug, force all Hailo DMA objects below the same conservative ceiling, ideally below 1 GB first, then bisect upward.
- Do not accept a fallback to high memory for Hailo DMA unless the PCIe inbound aperture has been proven with a device-side readback test.

### F-02: The cache/coherency model is fragile for device-owned rings

Severity: High.

Evidence:

- `hailo_vdma_program_buffer()` writes descriptors into normal PMM-backed memory and manually cleans cache lines before the device reads them.
- The descriptor list contains host-written descriptor fields and device-written completion/status fields. Those are coherent-object semantics, not simple one-way buffer semantics.
- Debug paths discard/inspect descriptor cache lines to verify DRAM contents.
- `HAILO_WIRE_DEBUG` is enabled by default in both `CMakeLists.txt` and `Makefile`, while the VDMA code comments say the verbose polling/readback is intrusive and should not be part of a merge-ready build.

Why it matters:

Manual clean/invalidate can work, but it is easy to get wrong when the device and CPU share descriptor cache lines. A stale descriptor view can produce exactly the observed symptom: the Hailo firmware sees an empty or invalid descriptor and never advances processing. The docs say cache flush has been investigated, so I do not rank this above DMA aperture, but it remains a serious architecture risk.

Recommendation:

- Put descriptor lists in a coherent or uncached mapping if the MMU supports it.
- If that is not available yet, define strict ownership transitions: CPU writes descriptors, clean to point of coherency, no CPU reads until after completion, invalidate only after the device is known done.
- Disable `HAILO_WIRE_DEBUG` by default. Keep it as an explicit diagnostic build option.
- Separate diagnostic descriptor dumps from the hot submit/wait path so debug instrumentation cannot perturb timing or cache ownership.

### F-03: PCIe root-complex setup is still too hard-coded

Severity: High for platform robustness, Medium for the current MNIST-specific blocker.

Evidence:

- `pcie_bcm2712.c` hard-codes `RC_BAR2` and `SCB0_SIZE` values for a 4 GB memory view.
- The cached Raspberry Pi Linux driver reads `dma-ranges` and `brcm,scb-sizes` before programming the inbound view.
- Local comments are internally inconsistent: the code sets `CFG_READ_UR_MODE`, while a later comment still says SLM-OS leaves it cleared.
- There is no clear audit trail that SLM-OS programs or verifies Max Payload Size/Max Read Request Size for both the RC and endpoint.
- The cached Hailo PCIe driver has a specific workaround: if MaxReadReq is below 512 bytes, it changes the maximum descriptor page size. SLM-OS currently uses fixed descriptor page sizes for several paths.

Why it matters:

Even if the current MNIST trace uses compatible 512-byte input and 64-byte output pages, the backend should not assume those sizes are always safe. PCIe read request size affects how the endpoint fetches host descriptors and buffers. This is adjacent to the current failure mode and should be brought under explicit control.

Recommendation:

- Parse or otherwise centralize the platform PCIe DMA aperture instead of spreading constants across Hailo and PCIe code.
- Read and log RC and endpoint PCIe capabilities after link training: MPS, MRRS, ASPM, bus master, 64-bit DMA capability, and AER/status errors if available.
- Mirror the Hailo driver descriptor-page-size decision: derive `desc_max_page_size` from actual PCIe capability state unless explicitly forced for debug.
- Update stale comments around `CFG_READ_UR_MODE` and firmware-vs-kernel link training.

### F-04: The context-switch translator is MNIST-template driven

Severity: High for feature completeness.

Evidence:

- `hailo_cs_translator.c` has `hef_matches_mnist_template()`, `mnist_switch_lcu_batch_template`, MNIST-specific sequencer/LCU byte templates, and special preliminary/boundary sequences guarded by the MNIST matcher.
- The non-MNIST path is explicitly partial in comments and behavior. Batch switching and boundary prologue logic are not generated from a complete HEF graph.
- `inference_device_hailo.c` contains hard-coded dual-channel CCW constants that match the current MNIST/HailoRT trace: descriptor counts and byte patterns are not generally derived.
- `pick_largest_pads()` selects only one largest input and one largest output.

Why it matters:

This implementation can prove the hardware path for a known model, but it cannot yet claim generic AI HAT+ support. A different HEF can require different boundary streams, dynamic actions, LCU sequencing, descriptor page sizes, or multi-input/multi-output behavior.

Recommendation:

- Reframe the current state in docs and status as "MNIST/Hailo-8L bring-up backend" until the translator is graph-derived.
- Remove or isolate MNIST byte templates behind an explicit compatibility mode.
- Make non-MNIST load fail loudly when required graph/action constructs are unsupported.
- Add HEF-driven channel, stream, pad, and action generation before exposing this as general inference support.

### F-05: Synthetic output pad fallback can create invalid output streams

Severity: High.

Evidence:

- `inference_device_hailo.c` has a fallback that synthesizes a 1000-byte output pad when output pad discovery fails.
- That fake pad includes invented stream identity and sizing.

Why it matters:

For a production inference backend, inability to parse an output pad is a hard load failure. Creating a fake output stream can misprogram ACTIVATION/DYNAMIC context and can directly produce "no output" or "wrong output" symptoms. It also masks parser defects that should be fixed at load time.

Recommendation:

- Remove synthetic pad fallback from production paths.
- Keep synthetic pads only in unit tests or shell smoke tests where no real HEF is involved.
- Fail `load_model()` with a specific error when boundary input/output pads cannot be mapped from HEF metadata.

### F-06: VDMA ring state is modeled as one-shot absolute counters

Severity: High for repeated inference and output correctness.

Evidence:

- `hailo_vdma_submit_and_wait()` publishes an absolute `num_avail` and waits for `num_proc` to equal that value.
- `hailo_vdma_channel_wait_proc()` also waits for an absolute target.
- `hailo_backend_run()` pre-arms output descriptors multiple times. The trace docs later disconfirmed the incremental `num_avail` hypothesis as the root cause, but the code still carries stateful pre-arm behavior.
- There is no persistent per-channel software ring cursor comparable to HailoRT's ongoing-transfer bookkeeping.

Why it matters:

Absolute targets can work for first-frame bring-up but are brittle once a channel processes more than one transfer, wraps counters, or has stale `num_proc` from earlier arming. Output pre-arming can make later waits pass against old state or wait for an impossible target.

Recommendation:

- Track VDMA ring state per channel: producer index, expected completion index, descriptor ownership, and wrap behavior.
- Stop using repeated output pre-arm as a production strategy unless it exactly matches HailoRT semantics for the active network.
- Make first-inference debug and steady-state inference use different code paths until the ring model is complete.

### F-07: `run()` races with `free_model()`

Severity: High, independent of the current hardware blocker.

Evidence:

- `hailo_backend_run()` reads a slot and checks `slot->in_use` without holding the slot lock or taking a model reference.
- `free_model()` clears the slot under lock and then frees model resources.
- `hailo_backend_model_sizes()` does take the lock, so the inconsistency is localized.

Why it matters:

A concurrent free can produce use-after-free of DMA buffers, descriptor lists, context buffers, and parsed HEF state. On hardware, that could look like random DMA failure or firmware wedge.

Recommendation:

- Add slot reference counting or hold the lock across stable resource acquisition.
- Prevent `free_model()` from releasing Hailo DMA resources while an inference is in progress.
- Serialize context-switch load/run/free for a given model handle.

### F-08: Scheduler integration uses semantic tensor sizes, not backend sizes

Severity: Medium.

Evidence:

- The shell's `hailo load <path> sched` path passes `AI_STATE_DIM` and `AI_SCHED_N_ACTIONS` into the Hailo scheduler policy.
- `sched_ai.c` allocates fixed scheduler state/action buffers and calls `inference_run()` with those semantic lengths.
- `hailo_backend_run()` requires exact match with `cfg.input_bytes` and `cfg.output_bytes`.
- The Lua path is better: it queries `hailo_backend_model_sizes()` and allocates based on backend-reported sizes.

Why it matters:

For HEFs with padding or different compiled tensor sizes, the scheduler path can reject otherwise valid models with `BAD_TENSOR`, or pressure developers to add unsafe padding hacks outside the backend.

Recommendation:

- Make the scheduler path query backend model sizes after load.
- Store semantic dimensions separately from transport byte counts.
- Let the backend own quantized transport buffer sizing and expose only the scheduler's logical output count to policy code.

### F-09: Diagnostics and production behavior are mixed

Severity: Medium.

Evidence:

- `hailo_backend_run()` performs diagnostic `CORE_IDENTIFY` and D2H event drains around inference.
- `HAILO_WIRE_DEBUG` defaults to ON.
- Several comments and docs describe previous assumptions that have since been superseded.

Why it matters:

The current blocker needs diagnostics, but production paths should not depend on diagnostic RPCs, verbose polling, or timing-sensitive register reads. Mixing them makes A/B comparison with HailoRT harder.

Recommendation:

- Make a minimal production inference path with only required control/VDMA operations.
- Gate all trace dumps, descriptor dumps, event drains, and identify probes behind explicit debug flags.
- Keep one "HailoRT comparison mode" that intentionally emits trace-compatible operations for A/B work.

### F-10: Host-side A/B tooling is not implemented to the level described

Severity: Medium for diagnosis velocity.

Evidence:

- `host-tools/hailo-ushim/README.md` describes VDMA buffer map, descriptor list programming, launch transfer, and `--submit-probe`.
- `host-tools/hailo-ushim/main.c` currently implements only `--identify`; comments say VDMA support is future work.

Why it matters:

The blocker is below high-level context bytes. The fastest way to isolate it would be a Linux userspace shim that uses the official Hailo driver ioctls but submits the same descriptor lists and buffers SLM-OS intends to use. The current tool cannot do that yet.

Recommendation:

- Either update the README to match the current tool, or finish the VDMA submit probe.
- Prioritize a minimal A/B probe that allocates low and high buffers through the Hailo driver, programs a descriptor list, launches the same channel type, and reports whether `num_proc` advances.

### F-11: Multi-input/multi-output support is not present

Severity: Medium.

Evidence:

- Pad selection picks one largest input and one largest output.
- Context-switch configuration and runtime buffers are organized around one input and one output boundary path.

Why it matters:

Many real Hailo HEFs have multiple output heads, multiple streams, or non-trivial postprocessing layouts. The current backend can silently choose the wrong pad if used beyond the MNIST bring-up shape.

Recommendation:

- Represent boundary pads as arrays throughout `hailo_model_slot`.
- Require all HEF boundary pads to be mapped or fail load.
- Make API-level output handling explicit for multi-output models before claiming general HAT+ support.

## Findings most relevant to the current data-return bug

Most likely:

- Boundary input descriptor list or tensor is not reachable by the endpoint at the IOVA provided.
- The low allocator mitigation is incomplete because it has no hard ceiling and does not cover every DMA object.
- Cache ownership for descriptor lists is still fragile enough to corrupt or hide the first descriptor.

Plausible but lower confidence:

- PCIe MRRS/MPS or descriptor page size does not match the actual link configuration.
- The context translator still misses a firmware state transition that HailoRT performs outside the byte-diffed RESET/ACTIVATION/DYNAMIC paths.
- HailoRT userspace or driver resource-manager ioctls perform channel/resource initialization that SLM-OS has not replicated.
- The persistent `HEALTH_MONITOR_CPU_ECC_ERROR` event is a meaningful early warning that firmware is reading invalid state, not just a side-effect.

Less likely as the primary blocker:

- MSI/IRQ acknowledgement. The failing symptom is `num_proc` stuck at zero and descriptor status unchanged. If the device had processed the descriptor but IRQ was mishandled, the descriptor or `num_proc` would be expected to move first.
- Incremental `num_avail`. The trace investigation already disconfirmed it as the primary mismatch for the first failing submit.
- High-level Lua/scheduler output consumption. The device appears not to complete the input boundary transfer, so there is no output to consume yet.

## Recommended next investigation sequence

No-hardware/static actions:

1. Make a complete inventory of every Hailo DMA allocation site and classify it as control, CCW, descriptor list, input tensor, output tensor, or scratch.
2. Define the required DMA contract for each allocation: alignment, max physical address, cacheability, ownership direction, and lifetime.
3. Audit all comments/docs that still reference obsolete assumptions, especially `dtparam=pciex1`, firmware-owned link training, `CFG_READ_UR_MODE`, and "QEMU-only" inference comments.
4. Decide whether current scope is "MNIST proof of hardware path" or "generic HEF runtime"; enforce that decision in load-time validation.

Hardware actions when available:

1. Force every Hailo DMA object below a conservative physical limit, starting below 1 GB. Include CCW, boundary descriptor lists, and tensors.
2. Boot with `HAILO_WIRE_DEBUG=OFF` and capture only minimal channel register state before and after submit.
3. Log exact physical address and IOVA for every descriptor list and tensor; compare working CCW and failing boundary paths.
4. Add a device-side readback test if possible: ask firmware or a known-good VDMA path to read a few bytes from the descriptor-list IOVA before publishing `num_avail`.
5. Compare RC and endpoint PCIe capability state against Raspberry Pi OS on the same board: MPS, MRRS, ASPM, bus master, DMA mask, BAR assignment, and errors.
6. Reproduce the failure with one variable changed at a time: low DMA only, debug off, descriptor page size forced, cache policy changed, and output pre-arm removed.
7. If low DMA fixes the first submit, expand the DMA zone upward until the failure returns. That gives a concrete platform aperture instead of a guess.
8. If low DMA does not fix it, prioritize missing HailoRT resource-manager/channel setup and the persistent ECC health event.

## Acceptance criteria before this should be called supported

- The backend runs at least one known HEF end-to-end on hardware without diagnostic-only steps in the hot path.
- All Hailo DMA allocations come from a proven DMA-safe, explicitly bounded pool or a parsed platform aperture.
- Descriptor lists have a coherent memory strategy with documented CPU/device ownership transitions.
- VDMA channels maintain software ring state across repeated inferences.
- Load fails loudly for unsupported HEFs instead of synthesizing pads or silently choosing the largest pad.
- Scheduler integration uses backend-reported transport sizes and separately tracks logical policy dimensions.
- The context-switch translator is either graph-derived for supported models or explicitly rejects unsupported action graphs.
- `host-tools/hailo-ushim` either implements the documented VDMA probe or its README is corrected.
- Debug tracing is off by default and can be enabled without changing production behavior.

## Bottom line

The feature is close to proving a first hardware inference path, but the remaining blocker is probably not in the surface inference API. The most defensible hypothesis is that Hailo firmware is not successfully reading the boundary input descriptor list or buffer from the address/cache state SLM-OS provides. The next fix should make Hailo DMA placement and coherency explicit and testable, then re-run the boundary input submit with all DMA objects in a proven safe range and debug perturbation disabled.

In parallel, the implementation should stop presenting MNIST-specific templates and synthetic fallbacks as general AI HAT+ support. That distinction matters: it keeps the immediate bring-up focused while preventing architectural shortcuts from becoming permanent API behavior.
