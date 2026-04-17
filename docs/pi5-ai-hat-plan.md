# Pi 5 AI HAT+ (Hailo-8/8L) Support Plan

**Target:** GeeekPi AI HAT+ (Hailo-8L, 13 TOPS) and AI HAT+ 26 TOPS (Hailo-8) on Raspberry Pi 5, end-to-end to running AI inference workloads from SLM-OS.

**Status:** Planning — no code written. Tracked in [#253](https://github.com/SLM-OS/SLM-Operating-System/issues/253).

---

## 1. Summary

The AI HAT+ is a PCIe Gen3 x1 add-in board exposing a Hailo-8/8L NPU through the Pi 5's external PCIe connector (CM4-style FFC). Linux drives it through the `hailo_pci` + `hailort` userspace stack; there is no bare-metal reference.

Getting it working on SLM-OS requires crossing three boundaries the project has not crossed before:

1. **A real PCIe host controller driver on ARM64.** The kernel has x86-64 PCIe (ECAM + legacy I/O) and has touched RP1's internal PCIe link indirectly through hardcoded MMIO, but there is no general PCIe enumerator for BCM2712 and no code at all for `pcie1` (the external RC at `0x1000110000` that the AI HAT+ attaches to).
2. **A closed-vendor accelerator driver.** Hailo's firmware load sequence, doorbell/ring-buffer protocol, and `.hef` (Hailo Executable Format) binary layout are partially documented in their open-source Linux driver (`hailort/hailo_pci`, dual-licensed GPLv2). The bring-up work is analogous in spirit to the NVIDIA GSP bring-up — reuse that pattern: a shared core plus a platform shim.
3. **An "inference device" abstraction in the kernel.** Today, all AI inference in SLM-OS is CPU-only via NEON/SSE (AI scheduler MLP/PPO, CACHEUS eviction). A new vtable is needed so the scheduler (and, eventually, page eviction and Lua scripting) can dispatch to a Hailo backend instead of the CPU path.

**Estimated effort:** 10–15 weeks of focused work, staged across 7 phases. Realistic capstone-scale target is **Phase 0–4 (probe + boot + basic inference)**; Phases 5–7 are follow-on work.

---

## 2. Hardware Context

### 2.1 AI HAT+ variants

| Variant | Accelerator | TOPS | PCIe link | Power |
|---|---|---|---|---|
| AI HAT+ 13 TOPS | Hailo-8L | 13 | Gen3 x1 | ~2.5 W |
| AI HAT+ 26 TOPS | Hailo-8 | 26 | Gen3 x1 | ~3.5 W |

Both use the same Hailo PCIe programming model — the driver is identical, only the compiled `.hef` model file differs.

### 2.2 Pi 5 PCIe topology

The BCM2712 exposes two PCIe root complexes relevant here:

| RC | Base (config) | Purpose | Current SLM-OS support |
|---|---|---|---|
| `pcie1` | `0x1000110000` | External connector (AI HAT+, NVMe, etc.) | None |
| `pcie2` | `0x1000120000` | Internal, fixed to RP1 I/O controller | Hardcoded MMIO only (no enumeration) |

**Key consequence:** The AI HAT+ is on `pcie1`, which has zero existing code. The RP1-specific MSIX_CFG bug tracked in #247 does **not** apply — `pcie1` endpoints use the standard PCIe MSI-X capability, writing directly to a MIP (Message-Signaled Interrupt Peripheral) address programmed into the endpoint's MSI-X table.

### 2.3 Pi 5 firmware constraints

The Pi 5 VideoCore firmware brings up `pcie1` and trains the link during early boot if `dtparam=pciex1` is set (the default on Pi 5 HAT+ builds). SLM-OS inherits a live, trained link — it does not need to own the PCIe controller reset or retraining sequence. This matches how `pcie2`/RP1 is inherited today.

What SLM-OS **does** need to do:
- Read the device's BARs via config space.
- Map BARs into SLM-OS's MMU.
- Enable bus master on the device.
- Allocate MSI(-X) vectors and program the device's MSI-X table.
- Route MSI-X writes through MIP1 (the external-PCIe counterpart to MIP0) into GIC SPIs.

### 2.4 MIP1 (external-PCIe MSI)

Per the BCM2712 address map, MIP1 is at an address parallel to MIP0 (which is `0x1000130000`). The exact base needs confirmation from the Pi 5 downstream kernel device tree (`arch/arm/boot/dts/broadcom/bcm2712-rpi-5-b.dts` in rpi-6.6.y) — this is a Phase 0 research item.

---

## 3. Phased Implementation Plan

Each phase is self-contained and deliverable. Later phases assume earlier ones landed.

### Phase 0: Research & Prerequisites (1 week)

**Deliverables:**
- Annotated notes in `docs/reference/` from Hailo's open-source Linux driver source:
  - Firmware upload protocol (boot control message, firmware image layout, boot ack).
  - Control channel (VDMA, descriptor rings, doorbells).
  - `.hef` file format — header, network group descriptors, weight/bias layout, layer graph.
  - Inference submit/complete protocol.
- `docs/pi5-pcie1-registers.md` capturing `pcie1` RC register layout (mirror of `pcie2` at `0x1000120000`) and MIP1 base/programming model, with line references into the Pi kernel's device tree.
- A "minimum viable Hailo model" picked and frozen (e.g. MobileNetV1 INT8 from Hailo Model Zoo). Compile it to `.hef` from the Hailo compiler on the dev workstation — not on SLM-OS. Ship the `.hef` as a VFS asset (like the trained scheduler weights today).
- Confirm the AI HAT+ powers up and the Pi 5 link trains under stock Linux. This validates the hardware setup before SLM-OS gets near it.

**Agent to kick off:** Research-only agent with access to Hailo's open-source driver GitHub mirror and the Pi kernel tree. Output is docs + a short "gotchas" list.

### Phase 1: ARM64 PCIe Host Controller (2–3 weeks)

**Deliverables:**
- `kernel/drivers/pcie/pcie_bcm2712.c` — Root complex driver:
  - EXT_CFG mechanism (bus/devfn selector, data window) — same pattern the UART driver uses for RP1 today, but owned by a proper driver.
  - Config space read/write API (`pcie_read_config_u32(bus, dev, func, offset)`, `pcie_write_config_u32(...)`).
  - BAR enumeration and remapping into kernel virtual address space via VMM.
  - Capability list walk (find MSI-X capability, BIST, power management).
- `kernel/include/pcie.h` — public API for device drivers:
  - `pcie_find_device(vendor_id, device_id) -> pcie_dev_t *`
  - `pcie_enable_bus_master(dev)`
  - `pcie_map_bar(dev, bar_num, flags) -> void *`
  - `pcie_alloc_msix(dev, count) -> msix_handle_t`
  - `pcie_bind_msix_handler(msix, vector, fn, ctx)`
- Refactor `kernel/drivers/uart_rp1.c` to consume this API for RP1 discovery (optional but proves the abstraction is right). Keep polled IRQ fallback in place — see #247.
- Unit test with QEMU's emulated PCIe (add a virtio device to QEMU ARM64 machine) — exercises enumeration and BAR mapping without real hardware.

**Blockers:** None — purely software work. All information is in BCM2712 downstream kernel sources.

### Phase 2: Inference Device Abstraction (1 week, can overlap Phase 1)

**Deliverables:**
- `kernel/include/inference_device.h`:
  ```c
  struct inference_device_ops {
      int (*init)(struct inference_device *dev);
      int (*load_model)(struct inference_device *dev, const void *model, size_t size, model_handle_t *out);
      int (*run)(struct inference_device *dev, model_handle_t m, const tensor_t *in, tensor_t *out);
      int (*free_model)(struct inference_device *dev, model_handle_t m);
      void (*shutdown)(struct inference_device *dev);
  };
  ```
- Registration dispatcher (like `gpu_register_driver()`).
- CPU backend stub that wraps the existing NEON MLP so the abstraction is exercised end-to-end before any Hailo code exists.
- Integrate into the scheduler policy vtable: `ai_policy_ops->assign_cpu()` can route through the inference device instead of calling `ai_schedule_mlp()` directly.
- Rationale lives in `docs/nvidia-gsp.md` §"Platform Shim Contract" (the pattern is the same — one shared core, multiple platform/device implementations).

### Phase 3: Hailo Device Probe & Control Plane (2 weeks)

**Deliverables:**
- `kernel/ai_accel/hailo/` — driver directory, structured like `kernel/gpu/nvidia/`:
  - `hailo.h` — public API and the `hailo_platform_ops` vtable (mirrors `gsp_platform_ops`).
  - `hailo_core.c` — platform-independent core: register layout, control channel state machine, firmware loader, VDMA descriptor ring management.
  - `hailo_pi5.c` — Pi 5 platform shim: BAR map via `pcie_map_bar()`, DMA alloc via PMM, cache sync via `cache_clean/invalidate_range()`, MSI-X handler registration.
  - `hailo_stub.c` — QEMU/Jetson/x86-64 stub that returns `-ENODEV` so other platforms continue to link.
- First milestone: `hailo` shell command that
  - probes the device via PCIe,
  - dumps the vendor/device ID and HW version register,
  - reports firmware version from the boot ROM.
- `make test-hailo` — offline unit tests against a mocked PCIe + register vtable (same pattern as `make test-falcon` for GSP).

### Phase 4: Firmware Load & Model Load (2 weeks)

**Deliverables:**
- Firmware image embedded via `.incbin` (same as scheduler weights, AES unlock not required — Hailo firmware is open-distribution and ships with the Linux driver).
- Firmware upload via the VDMA control channel, boot-ack poll, and post-boot register sanity check.
- `.hef` parser: header validation, network group walk, weight/activation buffer layout. Derived from Hailo's open-source parser but rewritten in C (the reference is C++).
- Model load API: allocate NC DMA buffers for weights, DMA them to device, program control-plane descriptors, confirm "model ready" state.
- `hailo load <vfs-path>` shell command, wired to VFS via the existing LittleFS path.

### Phase 5: Single-Model Inference (2 weeks)

**Deliverables:**
- Tensor buffer API: allocate input/output tensors in NC DMA memory with platform cache sync handled by the driver.
- Inference submit: post descriptors, ring doorbell, wait on MSI-X completion (or polled CNTPCT timeout fallback — see #247 for why polling is a valid long-term fallback on Pi 5).
- `hailo infer <model> <input-tensor>` shell command; output tensor dumped as hex or post-processed per a known model's output layout.
- Latency histogram integration (#196) — add `hailo_infer` as a first-class bench histogram source.
- Cross-platform inference bench doc (`docs/cross-platform-inference-bench.md`) updated with a "Hailo-8L on Pi 5" row.

### Phase 6: AI Scheduler Integration (1–2 weeks)

**Deliverables:**
- New scheduler policy: `ai_policy_hailo` that uses the inference device abstraction (Phase 2) to run the scheduler MLP on Hailo instead of NEON.
- Requires retraining/recompiling the scheduler MLP as a `.hef`. Constraint: Hailo's tooling supports a narrower op set than full ONNX; the 108-dim → 24/42-action MLP with ReLU should compile.
- Benchmark comparison: `bench sched-policy` comparing:
  - Round-robin (baseline).
  - CPU MLP (current AI scheduler).
  - Hailo MLP (new).
  - Metrics: decisions/sec, end-to-end task makespan, device power.
- Policy switch via existing `sched_set_policy()` runtime API — the whole point of the Phase 2 abstraction.

### Phase 7: Shell Integration & Demo Polish (1 week)

**Deliverables:**
- Lua binding: `slm.hailo.load(path)`, `slm.hailo.infer(model, tensor)`, `slm.hailo.status()`.
- Demo script that runs MobileNetV1 classification on a small embedded image (test data in VFS), shows live FPS in `top`-style UI.
- `docs/demo.md` updated with an AI HAT+ demo path.
- Update `docs/capstone-feature-status.md` GPU Inference row: Pi 5 column goes from "—" to "Hailo-8L via AI HAT+".

---

## 4. Risks & Open Questions

| Risk | Likelihood | Mitigation |
|---|---|---|
| Hailo firmware upload protocol is undocumented outside their Linux driver — might take longer to reverse | Medium | Phase 0 research budget; fall back to `hailort` userspace as a cross-check in Linux |
| MIP1 programming model differs from MIP0 in non-obvious ways | Low | Cross-reference Pi 5 downstream DTS and rpi-6.6.y driver; if it really differs, fall back to polled completion (Phase 5 already has this path) |
| `.hef` format is versioned; Hailo Model Zoo models may require a compiler version the capstone timeline can't match | Medium | Freeze a specific Hailo Compiler version in Phase 0 and document it; treat `.hef` as a build artifact, not a source asset |
| Pi 5 PCIe Gen3 link instability under thermal load (AI HAT+ runs hot) | Low | Use passive heatsink that ships with AI HAT+; monitor link status register during inference |
| Effort exceeds capstone timeline | **High** | Land Phases 0–4 as a hard target (minimum viable demo: "SLM-OS loaded a model onto a real NPU and ran a forward pass on bare metal"). Phases 5–7 as stretch. |

---

## 5. Dependencies

- **Codebase:** None on other in-flight PRs. Independent workstream.
- **Hardware:** GeeekPi AI HAT+ physical unit; Pi 5 in lab with the HAT mounted and `dtparam=pciex1` in config.txt.
- **Tooling:** Hailo Compiler (Linux x86-64 only, runs on dev workstation). Free for non-commercial use.
- **Labctl:** If the AI HAT+ becomes part of the standard Pi 5 lab setup, `labctl`'s claims feature (SPEC in `../Embedded-Lab-Control/docs/SPEC_claims.md`) should cover it implicitly — no labctl changes needed.

---

## 6. Test Plan

Per the project's post-change checklist (run on every phase):

1. **QEMU unit tests** (`make test`) — PCIe enumeration, inference-device abstraction, Hailo core via mocked BAR vtable.
2. **Pi 5 hardware smoke test** — `labctl sdwire_update` + `serial_send "hailo probe"` — must pass before every commit to Phase 3+.
3. **`make test-hailo`** — offline driver tests (added in Phase 3).
4. **Demo script** — end-to-end from `slm.hailo.load()` to classification output (Phase 7).
5. **Reliability** — `labctl boot_test --count 10` with a `hailo probe` step in the boot-verification script.

---

## 7. Out of Scope

- Multi-context / multi-model concurrent inference (Hailo supports this, but adds significant driver complexity; follow-on issue).
- Hailo integrated memory features like power gating, DVFS (trivial; follow-on).
- Model hot-swap (#232) — orthogonal; can layer on top once the basic load/run path works.
- x86-64 AI accelerator support — x86-64's path forward is the RTX 3050 via GSP-RM (separate work).
- Jetson Hailo support — the Jetson already has an integrated NPU (NVDLA) plus GPU; adding Hailo on Jetson is non-capstone-relevant.

---

*Last updated: 2026-04-17*
