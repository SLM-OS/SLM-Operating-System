# ADR 0017: Drive the Pi 5 Hailo-8L from a native bare-metal driver, without HailoRT

Status: accepted (backfilled).

## Context

The Raspberry Pi 5 AI HAT+ puts a Hailo-8/8L NPU on the external PCIe
port `pcie1`. Linux drives it with the `hailo_pci` kernel module and the
HailoRT userspace stack; "there is no bare-metal reference"
(`docs/pi5-ai-hat-plan.md` §1, plan landed in 9d31ea84 "docs: Pi 5 AI HAT+
(Hailo-8/8L) implementation plan (#253)", 2026-04-17). Pi 5 firmware does
not train `pcie1`, and SLM-OS had no ARM64 PCIe host driver.

## Decision

- Write a native driver in `kernel/ai_accel/hailo/` with a shared core and
  a platform shim (`hailo_core.c`, `hailo_pi5.c`, `hailo_stub.c`), modelled
  on the NVIDIA GSP bring-up (plan §1), behind the inference-device vtable
  `kernel/include/inference_device.h`.
- PCIe: `kernel/drivers/pcie/` with a BCM2712 backend that ports the
  `brcm,bcm2712-pcie` subset of Linux's `brcm_pcie_setup()` (plan Phase 1.5).
- HEF: parse the protobuf body in the kernel with vendored nanopb 0.4.9.1
  and Hailo's MIT-licensed `hef.proto` (`hef_parser.c`, `hef_header.c`);
  "Sidecar pre-parse has been ruled out" (plan §3, Phase 0 findings, item 1).
- Firmware control: implement the documented fw_control RPC protocol
  (`hailo_control.c`) and the context-switch action translator
  (`hailo_cs_translator.c`, `hailo_cs_builder.c`) to match HailoRT v4.23's
  wire bytes. The firmware blob (`hailo8_fw.bin`, HailoRT 4.23.0) is embedded
  only when `HAILO_FW_BLOB` is set (`hailo_fw.S`).
- HailoRT on Linux is used as the reference and for wire captures, not as
  code (`docs/hailo-protocol-architecture.md`).

## Alternatives considered

- Porting HailoRT or `hailo_pci` into the kernel: unknown, ask the user.
  The record does not say whether a port was weighed.
- Pre-parsing HEF outside the kernel (sidecar): ruled out (plan §3, Phase 0), with no
  reason recorded.
- After the wedge: firmware-memory inspection through the BAR4 window and
  escalation to Hailo were considered and not pursued
  (`docs/hailo-inference-wedge.md` §6.1, §6.2).

## Consequences

- Working on `pi-5-1` (Hailo-8L): `pcie1` trains Gen2 x1, probe, firmware
  boot, HEF parse, and the full load sequence with every RPC returning
  success (`docs/hailo-inference-wedge.md` §1).
- Inference does not complete. The firmware never consumes the first
  input frame on boundary IN channel 2. #682 and #1001 are closed as not
  planned; host-observable bytes match HailoRT, so the remaining cause is
  firmware-internal state. Reopen criteria: wedge doc §6.3.
- Scope is a Hailo-8L / MNIST bring-up backend, not general AI HAT+
  support (plan, "Scope honesty"): MNIST-specific translator templates,
  one input and one output pad, Hailo-8 (26 TOPS) not validated.
- The `ai_hailo` scheduler policy registers on Pi 5
  (`docs/fact-sheets/ai-scheduler.md`).

## Evidence

- 48361b40 "Pi 5 AI HAT+ Phase 0-4: PCIe host controller, inference device,
  Hailo scaffolding, nanopb (#260)", 2026-04-17.
- a57615c1 "Pi 5 AI HAT+ Phase 1.5: brcm-pcie link training (partial —
  config-space blocker) (#262)" and b3f5a2b3 "pcie1 config-space fix + Hailo
  Phase 3: hailo_boot state machine (#267)", 2026-04-17.
- 73d7d3ea "Pi 5 AI HAT+ Phase 4: nanopb HEF protobuf parser + hailo load
  (#270)", 2026-04-17; a76279a8 "Pi 5: Hailo-8 control-channel RPC transport
  + IDENTIFY (tier-1 of #281)", 2026-04-18.
- 7eb31f70 "hailo: fix VDMA boundary channel direction mismatch wedging
  IN-channel inference (#682) (#999)", 2026-05-25: the one host-side defect
  found; the wedge persisted.
- d39257ce "hailo: match HailoRT load-time control-RPC cadence (4
  unjustified divergences) (#1004)", 2026-05-26.
- 63ef53ae "docs(hailo): advisor-readable inference-wedge write-up +
  correct overturned BAR4 framing (#1021)", 2026-10-21.
- `docs/hailo-lifecycle.md`: hypothesis table for the second pass.
