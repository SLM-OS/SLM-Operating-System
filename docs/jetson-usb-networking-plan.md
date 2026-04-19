# Jetson USB Networking Plan

Detailed implementation plan for USB-based networking on Jetson Orin
Nano. **Option A (USB-A host port + CDC-ECM dongle)** is the primary
target. **Option B (USB-C device mode + CDC-ECM gadget)** is a fallback
only pursued if the Phase 0 CBB probes rule Option A out.

**Status:** Phases 0-2 landed on main; Phase 3A partially landed and
then mothballed after a blocker traced to Linux's kexec-time SMMU
shutdown (#285, closed wontfix). See §8 for the full investigation
writeup. Tracked in
[#266](https://github.com/SLM-OS/SLM-Operating-System/issues/266).

| Phase | Status | Notes |
|---|---|---|
| 0 — CBB probe | ✅ done (2026-04-17) | Option A viable; xHCI clock-gated, not firewalled. See §3 Phase 0. |
| 1 — USB core (`kernel/usb/core/`) | ✅ merged | PR #272. URB / descriptor / enumeration; 37 unit tests against a mock HCD. |
| 2 — CDC-ECM class driver | ✅ merged | PR #276. Probe + MAC parse + bulk IN/OUT data path + net_driver glue; 25 unit tests. |
| 3A — XHCI host driver | ⛔ SMMU-blocked (2026-04-18); IFR bringup landed | Phase 3A.1 scaffolding + caps parse + rings + NO_OP on `feature/usb-networking-phase2` (merged #289). Phase 3A.2 IFR bringup (FPCI/BAR2 mapping, `tegra_xusb_config`, CSB paging, mailbox probe) on `feature/xhci-ifr-bringup`. 34 unit tests total across 2 files: 19 in `test_xhci_ring.c` (ring primitives, ERDP, ERST layout) + 15 in `test_xhci_tegra.c` (Tegra234 CSB math + Linux-reference offset pinning). Still blocked at USBCMD.RUN=1 by the SMMU teardown — now definitively identified as the only remaining obstacle. Root cause in §8 and §9; three-path handoff in §10. |
| 4 — lwIP netif integration | ☐🔗 pending | Requires a working Phase 3A, which is blocked. |
| 5 — testing, reliability, docs | ☐🔗 pending | Requires Phases 3A + 4. |

---

## 1. Summary

Two possible USB networking paths on the Orin Nano Dev Kit:

| | Option A | Option B |
|---|---|---|
| **Role** | Jetson as USB host | Jetson as USB device (gadget) |
| **Controller** | Tegra234 XHCI host (`xhci@3610000`) | Tegra234 XUDC device (`usb@3550000`) |
| **Port** | USB-A port on carrier board | USB-C debug port |
| **Peer** | External USB-A CDC-ECM Ethernet dongle | A Linux dev PC with the USB-C cable plugged in |
| **Reach** | Lab network (DHCP, gateway, internet) | Point-to-point link to one PC |
| **Extra hardware** | ~$10 CDC-ECM USB-A dongle | None — uses the existing USB-C debug cable |
| **Coordinates with** | Independent | Issue #24 (USB serial console via XUDC) |

Everything except the controller driver itself and the PHY/role-switch
bring-up is shared between the two paths. This plan runs the shared
work first, then does the XHCI host work (Option A). If Phase 0 rules
XHCI out, the same codebase switches to XUDC with the Phase 3B work
replacing 3A.

---

## 2. Hardware Context

### Tegra234 USB subsystem

The Orin SoC has two independent USB controllers with different roles:

- **XHCI host controller** at `xhci@3610000` (from the L4T DTS). Drives
  both USB-A ports on the dev kit carrier board. USB 2.0 (480 Mbps) +
  USB 3.2 (Gen2). For this plan, we only need USB 2.0 paths.
- **XUDC device controller** at `usb@3550000`. Drives the USB-C debug
  port in device mode. Used by L4T to present the dev kit as a mass
  storage device to flash, and for its built-in "USB device mode"
  network gadget at `192.168.55.1`.

The two controllers share UPHY (Unified Physical layer) resources but
are otherwise independent — a driver for one does not get the other
"for free." That's why falling back from A to B costs a controller
driver's worth of work, roughly 2-3 weeks.

### USB-A dongle choice (Option A)

Target: a USB-A CDC-ECM or CDC-NCM based Gigabit adapter. Cheap and
widely available. Realtek RTL8153 and ASIX AX88179 are the two most
common chips; both have open-standard class-driver support.

- **CDC-ECM** (RFC-style framing, one Ethernet frame per URB): simpler
  to implement, supported by virtually every Linux USB Ethernet chip.
- **CDC-NCM** (multi-frame framing in one URB): higher throughput but
  more complex framing. Start with ECM; NCM is an optimization.

**Do not target RNDIS.** RNDIS is a Microsoft protocol, closed spec,
and requires more complex signalling. CDC-ECM is a cleaner fit.

### USB-C dev link (Option B)

Stock L4T's gadget stack presents the Jetson as a composite USB device
with ACM (serial) + NCM (network) functions. Under Option B, SLM-OS
would expose a single CDC-ECM function. Linux on the connected PC
recognizes this automatically; no special driver needed.

### Ports, power, VBUS

Option A does not require SLM-OS to source VBUS — the dev kit's USB-A
ports are powered directly from the carrier board's 5V rail. Host mode
just needs the controller initialized; VBUS stays on.

Option B relies on the PC sourcing VBUS over USB-C. The OTG role
should resolve to "device" without any intervention because the PC is
the stronger party.

### CBB status (critical unknown)

From `docs/jetson-cbb-report.md` §3, USB has never been probed from
NS EL2. Phase 0 of this plan is the first experiment.

---

## 3. Phased Implementation Plan

Phases run in order. Phase 3B is only entered if Phase 0 rules Phase 3A
out.

### Phase 0 — CBB Probe (1 day) — ✅ done 2026-04-17

**Goal:** decide whether Option A is even viable before doing the
deep driver work.

**Tasks:**
1. From the `slmos>` shell (already booting on jetson-nano-2), run
   `peek 0x03610000 0x20` to attempt reads of XHCI MMIO.
2. Same for XUDC at `peek 0x03550000 0x20`.
3. Also probe the USB PHY registers (`tegra_xusb_padctl@3520000`).
4. Interpret results:
   - **Successful non-poison reads** → that controller is reachable
     from NS EL2; proceed.
   - **`0xbadf1100` PRI poison or RAS-kill** → that controller is
     CBB-blocked; that option is off the table.
   - **Zeros or garbage that looks uninitialized** → controller is
     reachable but not configured; need to check whether Linux left
     it live.

**Decision gate:**
- If XHCI is reachable → Option A. Proceed through Phases 1, 2, 3A, 4, 5.
- If XHCI is blocked but XUDC is reachable → Option B. Proceed through
  Phases 1, 2, 3B, 4, 5.
- If both are blocked → abandon USB networking entirely; the only
  remaining Jetson networking path is EQOS (#25) with a BCT-reconfig
  effort (see `docs/jetson-cbb-report.md` §6.A).

#### Results (jetson-nano-1, post-kexec NS EL2, commit `5d282b1`)

| Aperture | Reads | Verdict |
|---|---|---|
| XHCI `0x03610000` (32 words) | all `0xffffffff` | not CBB-blocked — clock-gated (no RAS, no `0xbadf1100`) |
| XUDC `0x03550000` (8 words) | all `0xffffffff` | not CBB-blocked — clock-gated |
| UPHY padctl `0x03520000` (`0x00..0x38`) | `0x55, 0x113, 0x31, 0xfff, 0x1111, 0xf7bde` | live at NS EL2 |

**Linux-side confirmation:** `tegra-xusb 3610000.usb` is running today
with HCC params `0x0180ff05` and USB 2.0 + SuperSpeed devices
enumerated. BPMP debugfs exposes `xusb_core_host`, `xusb_falcon`,
`xusb_fs`, `xusb_ss`, `xusb_core_dev` clock knobs and `xusba/b/c`
powergate knobs — same interface shape used by
`scripts/jetson-kexec-slmos.sh` for GPU clocks today.

**Decision: Option A.** XHCI is reachable; clock-gate state is mitigable
in Phase 3A by extending the kexec helper to hold
`xusb_core_host` / `xusb_falcon` / `xusb_fs` + powergates `xusba` / `xusbc`
on through the handoff. Full discussion in the Phase-0 comment on #266.

### Phase 1 — USB Core (1-2 weeks) — ✅ scaffolded

Applies to both Option A and Option B.

**Deliverables:**
- `kernel/usb/core/` directory with:
  - **URB framework** — a transfer primitive analogous to Linux's
    `struct urb`: submit, complete, cancel. This is the abstraction
    the controller driver implements *against* and the class driver
    builds *upon*.
  - **Device model** — `struct usb_device` with address, speed,
    config/interface/endpoint tables. Enumeration populates this.
  - **Endpoint state** — per-endpoint queues, max packet size, toggle
    state (for non-xHCI completeness; xHCI manages this in hardware).
  - **Descriptor parsing** — walk the standard descriptor chain
    (device → config → interface → endpoint) and populate the device
    model.
  - **Control transfer helpers** — `usb_control_msg()` equivalent for
    standard device requests (GET_DESCRIPTOR, SET_ADDRESS,
    SET_CONFIGURATION, etc.).

- `kernel/include/usb.h` — public API consumed by class drivers
  (CDC-ECM in Phase 2).

**Non-deliverables:**
- No controller driver yet — Phase 1 is pure in-kernel abstraction.
  Unit-test against a mock controller (mirrors how the GSP platform
  shim is tested).
- No hot-plug logic beyond "device connected at boot, stays connected"
  — enough for a fixed-dongle demo.

**What landed (feature/usb-networking-phase0):**

| File | Lines | Purpose |
|---|---|---|
| `kernel/include/usb.h` | ~260 | Public API: device model, URB, HCD ops, std request/descriptor constants |
| `kernel/usb/core/usb_core.c` | ~330 | HCD registration, URB submit/wait/cancel, descriptor parser, 10-step root-port enumeration |
| `kernel/tests/test_usb_core.c` | ~1100 | 37 unit tests against a mock HCD — URB lifecycle, descriptor parse (alt-setting skip, orphan EP, invalid header, undersized input, bad-bLength header, oversized length rejection, interface + endpoint table overflow), full enumeration, every error-injection branch of enumerate, device_close symmetry on every error path, control-msg return semantics + timeout cancel, return-code normalisation, cancel of a pending URB, silent same-HCD re-registration |

Architecture notes:
- **No dynamic memory in Phase 1 core.** `struct usb_device root_device`
  is a single static for the Phase-1 scope (one device on one root
  port). The HCD may allocate its own ring memory via `ncmem_alloc`
  on Jetson/Pi 5.
- **Enumeration zeroes state at entry** so a failed retry doesn't
  leave the previous device visible via `usb_core_first_device()` —
  regression-guarded by `test_enumerate_resets_previous_device`.
- **Alt-settings other than 0 are deliberately ignored** by the
  parser — a CDC-ECM dongle that exposes data-iface alt 1 will see
  its endpoints skipped until Phase 2 handles the
  SET_INTERFACE(alt=1) path explicitly.
- **URB completion is HCD-driven.** `usb_wait_urb` polls `hcd->poll()`
  between status checks, which lets the mock HCD and the real XHCI
  driver share the same control-msg path without requiring IRQs.
- **Return-code contract is uniformly negative.** `usb_submit_urb` /
  `usb_cancel_urb` / `usb_control_msg` all return 0 on accept and a
  negative errno-style value on error. If an HCD accidentally returns
  a positive `usb_urb_status` enum value, the core normalises it to
  `-USB_URB_IO_ERROR` so callers can rely on `if (rc < 0)`.
- **Device-close symmetry.** Once `hcd->device_open` has succeeded,
  every error path in `usb_core_enumerate` routes through the
  `err_close` label which invokes `hcd->device_close`. This matters
  for Phase 3A XHCI, which allocates a slot context in `device_open`
  and must release it on failed enumeration to avoid leaking slots.
  The core does **not** call `device_close` on pre-open bail-outs
  (`port_reset` failure or `device_open` itself failing) — the HCD
  owns its own partial-state cleanup in those cases.
- **Concurrency model.** `usb_core_register_hcd` is expected to run
  from the primary CPU before any secondary is brought up; after that
  `active_hcd` is effectively read-only and the submit / cancel /
  poll paths read it without locking. Phase 3A adds synchronisation
  when (and if) any post-boot mutation becomes legal.
- **Timeouts are a Phase-1 placeholder.** `usb_wait_urb` runs a
  fixed iteration cap (`timeout_ms × 1000`), not a wall-clock
  deadline. Good enough for the mock HCD (synchronous) and host
  QEMU tests; Phase 3A XHCI must switch to `CNTPCT_EL0`-based
  deadlines (same pattern as `hw_timeout_start` /
  `hw_timeout_expired` in `component_runtime.c`) before relying on
  real-time bounds on hardware.
- **Stack-allocated URB lifetime is a Phase-3A contract.**
  `usb_control_msg` builds its URB on the caller's stack and blocks
  in `usb_wait_urb` until completion or timeout. Today this is safe
  because the mock HCD completes synchronously inside `submit_urb`,
  and on timeout `usb_wait_urb` calls `usb_cancel_urb` before
  returning. Phase 3A XHCI will deliver completions from IRQ context
  — its `cancel_urb` implementation MUST block until no in-flight
  completion can still write to the URB (either the URB has left
  the hardware ring, or a pending completion IRQ has already fired)
  before returning, otherwise the stack-dead URB becomes a
  use-after-return dereference. A per-CPU static URB pool is an
  alternative if that invariant is hard to guarantee in the XHCI
  driver.

**Reference material:**
- FreeBSD's `usb4bsd` is a cleaner reference than Linux's monolithic
  stack for bare-metal porting.
- TinyUSB's core is simpler still but device-mode-biased.
- Linux `drivers/usb/core/` — authoritative but big.

### Phase 2 — CDC-ECM Class Driver (3-4 days) — ✅ scaffolded

Applies to both Option A and Option B.

**Deliverables:**
- `kernel/usb/class/cdc_ecm.c`:
  - Probe an enumerated USB device for CDC-ECM interfaces (bulk IN +
    bulk OUT + interrupt notification endpoints).
  - Parse the Ethernet functional descriptor for the MAC address
    (or generate one if the device doesn't report it).
  - TX path: take an Ethernet frame, submit as a bulk OUT URB.
  - RX path: keep a bulk IN URB queued; on completion, hand the frame
    to the lwIP netif (Phase 4) and re-queue.
- `struct net_driver` implementation that plugs the ECM device into
  the existing networking abstraction (`kernel/include/net_driver.h`
  from the landed networking work).

**What landed (feature/usb-networking-phase2):**

| File | Lines | Purpose |
|---|---|---|
| `kernel/include/cdc_ecm.h` | ~70 | Public API: `cdc_ecm_probe_and_register`, `cdc_ecm_poll`, plus test-visible MAC parser and counters |
| `kernel/usb/class/cdc_ecm.c` | ~340 | Probe (iface scan + functional descriptor walk + MAC string decode), static RX/TX slot pools, `struct net_driver` ops, fallback MAC synthesis when `iMACAddress == 0` |
| `kernel/tests/test_cdc_ecm.c` | ~800 | 25 tests against a CDC-ECM-shaped mock HCD — MAC parser edge cases (too short, wrong type, non-ASCII, non-hex, null args), full probe + registration + MTU extraction, `net_init` RX queuing, `send` happy path + pool exhaustion + oversized + null, `recv` happy path + empty + small-buffer truncation, `iMACAddress==0` fallback, rejection of non-CDC devices, probe-failure clears public MAC, net_driver ops refuse post-probe-failure, `cdc_ecm_poll` no-op when unprobed + dispatches to `hcd->poll` when probed, default MTU when `wMaxSegmentSize==0`, probe success without functional descriptor, RX-error drop |

Architecture notes:
- **Static slot pools, no heap.** 4 RX + 4 TX slots × 2 KB each = 16 KB BSS. Keeps Phase 2 free of dynamic allocation; Phase 3A XHCI decides whether these get relocated to NC memory for DMA coherence.
- **Completion callbacks tolerate IRQ context.** RX/TX completion paths touch only release-published slot flags and relaxed-atomic counters, not the net_driver dispatch — good for the Phase 3A XHCI IRQ path. All cross-context slot fields use `__atomic_*` GCC builtins with explicit ACQUIRE / RELEASE semantics: the RX completion publishes `len` + `buf` with a RELEASE store on `ready`, the consumer pairs an ACQUIRE load on `ready` before reading them; TX completion publishes `completed` RELEASE, tx_reap ACQUIREs before clearing `in_use`. TX-slot reservation uses `__atomic_compare_exchange_n` so concurrent `send()` callers can't claim the same slot. Diagnostic counters use RELAXED because they never gate another load.
- **Probe is self-resetting.** Each call clears `cdc.probed` at entry so a retry that finds no device (or a non-CDC device) doesn't leave a stale MAC visible via `cdc_ecm_get_mac()`.
- **MAC fallback is non-zero.** If `iMACAddress == 0` (or parsing fails), the driver synthesises `02:53:4C:4D:xx:xx` — locally administered, unicast, with "SLM" in the high bytes.
- **DMA discipline deferred to Phase 3A.** Slot buffers are cacheable BSS today. The XHCI driver will either issue DC CVAC/CIVAC around submit/complete or relocate these to `ncmem_alloc`; the class driver stays agnostic.

**Reference material:**
- CDC-ECM spec is public (CDC 1.2 ECM subclass spec).
- Linux `drivers/net/usb/cdc_ether.c` — ~1000 LoC, well-commented.

### Phase 3A — XHCI Host Driver (3-4 weeks, Option A primary) — ⛔ mothballed 2026-04-18

Steps 1-3 verified working on real hardware; Step 4 landed as code
but cannot complete end-to-end. Full investigation in §8. The
deliverables below are documented as-planned; what actually landed
and what's blocked is captured in the per-step log at the end of
this section.

**Deliverables:**
- `kernel/drivers/usb/xhci/` with:
  - Controller reset + HCRST polling, HCCPARAMS parsing for context
    size and address capabilities.
  - **DCBAA** (Device Context Base Address Array) + **ERST** (Event
    Ring Segment Table) + **Command Ring** + per-endpoint Transfer
    Rings in DMA-coherent memory. Use NC memory on Jetson (same as
    the virtio-net driver) to match the project's established
    cross-CPU DMA pattern.
  - Port status scanning for the USB-A ports, debounce + reset
    sequencing for any device found.
  - **ADDRESS_DEVICE** + **CONFIGURE_ENDPOINT** commands so the core
    can assign USB addresses and set up the endpoint rings.
  - TRB (Transfer Request Block) builder for control, bulk, and
    interrupt transfers. Setup Stage / Data Stage / Status Stage
    assembly for control.
  - Event ring consumer that fires URB completions.
- PHY / pad bring-up code for the `tegra_xusb_padctl@3520000` block —
  minimum viable: verify Linux left the pads programmed and skip the
  config. Full PHY bring-up is only required if the probe in Phase 0
  finds the pad state wiped.

**Risks specific to 3A:**
- **Clock state:** if BPMP-owned XHCI clocks get gated after kexec,
  the controller is dark. Mitigation: script the pre-kexec helper
  (`scripts/jetson-kexec-slmos.sh`) to hold the clock enabled via
  the debugfs knobs it already uses for the GPU.
- **xHCI spec complexity:** ~500 pages. The implementation plan
  scopes to USB 2.0 full/high speed only — USB 3.x SuperSpeed adds
  another controller layer (xHCI Operational + Runtime registers in
  separate address spaces) that can be deferred.
- **Dongle compatibility:** if the dongle is picky about reset
  timing or uses a vendor-specific config request, debugging needs
  a Linux host to diff against. Mitigation: test with two different
  dongles (Realtek + ASIX) from the start.

**Reference material:**
- Intel xHCI spec (free from usb.org / Intel).
- FreeBSD `xhci.c` — ~3500 lines, notably cleaner than Linux's for
  porting.

**Per-step log (what actually happened):**

| Step | Status | Evidence |
|---|---|---|
| 1 — kexec xusb clock hold (`scripts/jetson-kexec-slmos.sh`) | ✅ done (`66b7ad9`) | `peek 0x03610000` reads HCIVERSION=0x0120, HCCPARAMS1=0x0180ff05 from SLM-OS post-kexec. |
| 2 — driver scaffolding (`kernel/drivers/usb/xhci/`) | ✅ done (`47664e4`) | Compiles clean on all four targets. |
| 3 — capability probe + halt | ✅ done (`47664e4`) | `slmos> xhci` shows 36 slots, 8 ports, 5 interrupters, 64-bit addr, 64-byte ctx. Linux's halt state honoured (USBCMD=0, HCH=1). HCRST on Tegra is lethal (writes brick the aperture) so it's skipped. |
| 4 — DCBAA + command ring + event ring + NO_OP | ⛔ blocked (`57bdeb8`) | All pre-RUN register writes land cleanly (USBSTS stable at 0x1). USBCMD.RUN=1 wedges the aperture (reads return 0xffffffff). Root cause: SMMU disable — see §8. Ring primitives (cycle-bit handling, Link TRB wrap, event-ring dequeue) have 19 unit tests in `kernel/tests/test_xhci_ring.c` that run on every target. Post-merge review round added ERDP-tracking coverage + ERST-layout regression test after ERST storage moved to `ncmem_alloc` for DMA coherency (#289 follow-up). |
| 5 — port status + ENABLE_SLOT / ADDRESS_DEVICE | not started | Blocked on Step 4. |
| 6 — control-transfer TRB builder | not started | |
| 7 — CONFIGURE_ENDPOINT + bulk transfers | not started | |

### Phase 3B — XUDC Device Driver (2-3 weeks, Option B fallback)

Only entered if Phase 0 ruled out XHCI. Significant overlap with
issue #24 (USB serial console via XUDC) — if #24 is ever picked up
in parallel, this phase reduces to adding an ECM function on top of
the already-built XUDC driver.

**Deliverables:**
- `kernel/drivers/usb/xudc/` — the Tegra XUDC device controller
  driver:
  - Endpoint 0 handler for the standard device requests (enumeration
    state machine on the device side).
  - Bulk endpoint transfer submission + completion.
  - OTG role negotiation (pinned to device mode).
- `kernel/usb/gadget/` — a minimal gadget framework:
  - Composite-device descriptors.
  - Function registration API (matches the TinyUSB pattern).
- CDC-ECM function implementation (on the gadget side — the
  descriptor tables differ between host ECM and device ECM, but the
  framing is the same).

**Risks specific to 3B:**
- **OTG role:** Tegra UPHY has an OTG role detection that may or may
  not work without BPMP. Mitigation: hard-pin the role to device in
  early driver init, bypassing OTG detection entirely.
- **Descriptor tables:** composite gadgets require careful descriptor
  ordering. Easy to get wrong; Linux dev PC's `dmesg` is the
  diagnostic.

**Reference material:**
- Linux `drivers/usb/gadget/udc/tegra-xudc.c` — the upstream driver
  for exactly this controller.
- TinyUSB device stack — for the gadget framework pattern.

### Phase 4 — lwIP netif Integration (~1 week)

Applies to both Option A and Option B.

**Deliverables:**
- The CDC-ECM `net_driver` (from Phase 2) registers with the existing
  lwIP glue (`kernel/net/lwip_slm.c`).
- DHCP client runs at boot (same auto-DHCP logic as the landed
  networking expansion work).
- `ifconfig usb0` shows up in the shell.

No new architecture here — reuses the `net_driver` abstraction that
already services virtio-net on QEMU and MACB on Pi 5.

### Phase 5 — Testing & Hardening (1-2 weeks)

- **QEMU unit tests (`make test`)** — mock controller exercising the
  USB core and CDC-ECM class driver.
- **Jetson hardware smoke test** — `labctl sdwire_update` + serial
  session:
  - `ifconfig usb0` shows IP.
  - `ping <lab-gateway>`.
  - `ping 8.8.8.8` if the lab has internet.
- **Reliability sweep** — `labctl boot_test --count 10` with a USB
  enumeration + DHCP + ping verification step in the boot script.
- **Dongle-compatibility matrix** — repeat smoke test with at least
  two different dongles (Realtek-based, ASIX-based).
- **Disconnect/reconnect handling** — unplug the dongle mid-session,
  verify the netif goes down cleanly and comes back up when
  reconnected. (Basic hot-plug.)
- **Doc updates:**
  - `docs/capstone-feature-status.md` networking row for Jetson.
  - `docs/networking.md` — USB networking section.
  - `docs/jetson-cbb-report.md` §3 — fill in the USB row of the
    reachable-peripherals table with the Phase 0 findings.

---

## 4. Risks & Open Questions

| Risk | Likelihood | Mitigation |
|---|---|---|
| Phase 0 shows XHCI blocked by CBB | Medium | Fall back to Option B (Phase 3B). |
| Phase 0 shows both XHCI and XUDC blocked | Low | USB networking is off the table; EQOS (#25) becomes the only remaining Jetson networking path. |
| XHCI clocks gated post-kexec | Medium | Extend `scripts/jetson-kexec-slmos.sh` to hold the clocks enabled, same pattern used today for the GPU. |
| xHCI driver scope creep (USB 3.x, hot-plug, multi-device) | **High** | Strict USB 2.0 + single-device + boot-time-only enumeration at first. Defer everything else. |
| Dongle incompatibility | Medium | Test with two dongles (Realtek + ASIX) from the start. |
| Framing bugs in CDC-ECM at line rate | Low | 480 Mbps USB 2.0 is well below Ethernet's bursty patterns; back-pressure from the bulk OUT queue handles the rest. |
| Underflow on a single RX URB | Low | Keep 2-4 bulk IN URBs queued at all times so RX never stalls waiting for a re-submit. |

---

## 5. Dependencies

- **Codebase:** none — new driver tree under `kernel/usb/` and
  `kernel/drivers/usb/` that nothing else depends on.
- **Hardware:**
  - Option A: USB-A CDC-ECM or CDC-NCM Ethernet dongle (two models
    recommended for compatibility testing).
  - Option B: USB-C cable to a Linux dev PC with an available USB port.
- **Related issues:**
  - **#266** — the tracking issue for this plan.
  - **#24** — USB serial console via XUDC. If scheduled in parallel
    with Option B, the XUDC driver work is shared. If Option A
    succeeds, #24 remains independent.
  - **#25** — EQOS Jetson networking. This plan is the USB fallback
    for the scenario where #25 hits a hard CBB blocker.
  - **`docs/jetson-cbb-report.md`** — CBB blocker analysis for
    context.

---

## 6. Out of Scope

- USB 3.x SuperSpeed data rates (defer to future optimization).
- USB storage, audio, HID device classes.
- RNDIS (closed-spec Microsoft variant; use open CDC-ECM/NCM).
- Full hot-plug topology discovery (hubs, multiple devices on one
  controller). Single device only.
- Power management / USB runtime suspend.
- Full OTG role switching (Option B pins to device mode).

---

## 8. Phase 3A Investigation & Mothball (2026-04-18)

Full trace of what stopped Phase 3A mid-Step-4. GitHub issues #282
and #285 contain the live investigation log; this section is the
stable narrative summary.

### 8.1 What worked

- **Kexec clock hold (Step 1).** Extending
  `scripts/jetson-kexec-slmos.sh` to pin `xusb_core_host`,
  `xusb_falcon`, `xusb_fs` clocks and the `xusba`/`xusbc`
  powergates via BPMP debugfs keeps the xHCI controller's MMIO
  aperture addressable from SLM-OS post-kexec. Before the hold
  was in place the aperture read `0xffffffff` uniformly;
  afterward it reads live capability values that match Linux's
  reported state.
- **Capability probe (Steps 2-3).** From SLM-OS at NS EL2:
  CAPLENGTH=0x20, HCIVERSION=0x0120, HCCPARAMS1=0x0180ff05 —
  identical to what Linux reports via `dmesg | grep hcc`. 36
  device slots, 8 ports, 5 interrupters, 64-bit addressing,
  64-byte contexts. Halt state is honoured (USBCMD=0, HCH=1).
- **MMIO writes pre-RUN (Step 4, partial).** All register writes
  to DCBAAP, CRCR, CONFIG, ERST*, ERDP land cleanly — USBSTS
  stays at 0x1 throughout. No Falcon-level error triggers from
  the CPU-side register writes alone.

### 8.2 What broke

Writing `USBCMD.RUN = 1` causes the entire XHCI MMIO aperture to
return `0xffffffff` on subsequent reads. USBSTS, USBCMD, capability
registers, everything. The controller has entered an error state
that bricks its MMIO interface.

Notably, the wedge happens **regardless of what addresses are
programmed into DCBAAP/CRCR/ERSTBA** — tested with:

- SLM-OS's own NC-memory addresses (0xBDE0xxxx): wedge.
- Linux's leftover DMA addresses (DCBAAP=0x7ffffff000): wedge.
- The pre-RUN programming step skipped entirely (use whatever
  Linux left in the registers): wedge.

### 8.3 Diagnosis: SMMU disable in the kexec path

Linux's kexec handoff logs include these lines immediately before
the new kernel takes over:

```
arm-smmu 12000000.iommu: disabling translation
arm-smmu 10000000.iommu: disabling translation
arm-smmu 8000000.iommu: disabling translation
kexec_core: Starting new kernel
```

The xHCI controller lives in IOMMU group 2 (visible in Linux's
pre-kexec `dmesg`: `tegra-xusb 3610000.usb: Adding to iommu
group 2`). When `arm-smmu` shuts down as part of `device_shutdown()`,
the xusb stream's translations are dropped.

Writing RUN=1 to a running xHCI controller triggers its first DMA
attempt (fetching the Device Context Base Address Array entry).
On a Tegra234 system with SMMU translations disabled, that DMA
faults inside the xusb context bank. The controller enters an
internal error state that manifests as the MMIO aperture
returning all-ones.

### 8.4 Options explored

Four variants of Option A (Linux-cooperative pre-kexec
interventions) were tested on jetson-nano-1:

- **A.1** — `echo on > .../tegra-xusb/power/control`: harmless
  but ineffective. Runtime PM pin only prevents idle
  auto-suspend; it doesn't stop `device_shutdown()`.
- **A.2** — unbind tegra-xusb before kexec: **strictly worse**.
  The driver's `.remove()` callback zeros the controller's
  state, so even the first DCBAAP write from SLM-OS wedges the
  aperture (vs only wedging at RUN=1 without unbind).
- **A.3** — panic kexec (`kexec -p` + `crashkernel=256M`):
  skips `device_shutdown()`, BUT loads SLM-OS at the
  crashkernel-reserved region (0xefe00000) rather than its
  link address (0x80000000). SLM-OS still advertises RAM at
  0x80000000+, misidentifies free memory, and hangs early.
  Making this work would require relocatable-boot support
  across all of SLM-OS — out of scope for Phase 3A.
- **A.4** — kernel module that NULLs
  `tegra-xusb driver->shutdown`: on load, the module reported
  `shutdown already NULL — no-op`. **The tegra-xusb driver
  has no `.shutdown` hook on L4T 36.4.7 in the first place.**
  Every Option-A variant premised on "skip the .shutdown
  callback" was targeting the wrong mechanism.

After A.4's "already NULL" result, one more diagnostic confirmed
that the true blocker is SMMU translation disable, not any Falcon
shutdown path: writing RUN=1 with Linux's own DMA pointers intact
still wedges the aperture.

### 8.5 Why this isn't fixable in SLM-OS scope

Unblocking the DMA fault needs one of:

- **Prevent `arm-smmu`'s own `.shutdown`** from dropping
  translations. Risky: leaving SMMU translations live during
  the kexec transition means any device capable of DMA (not
  just xHCI) can write stale data into the new kernel's memory
  before SLM-OS takes over. Would need careful coordination
  with the kexec path.
- **Reprogram the xusb stream's SMMU context from SLM-OS**
  directly. The SMMU control registers are behind the CBB
  firewall (see `docs/jetson-cbb-report.md` §3) — unreachable
  from NS EL2.
- **Load Tegra XUSB Falcon firmware ourselves** (issue #286)
  and drive the controller cold, without relying on inherited
  Linux state. Tegra Orin's Falcon may run in HS mode with
  signed firmware, in which case an unsigned SLM-OS load
  won't execute. 3-6 weeks of focused work with ~40% success
  probability.

All three paths go significantly beyond the capstone's scope.

### 8.6 Decision: mothballed

Phase 3A scaffolding (the XHCI driver tree, ring allocation,
NO_OP command code, `xhci` shell diagnostic) is kept on
`feature/usb-networking-phase2` as reference for any future
revival. The Option-A.4 kernel module source is kept in
`scripts/tegra-xusb-noshutdown/` for the same reason.

Issues **#282** (HCRST brick — symptom of the same SMMU fault)
and **#285** (Phase 3A blocker) closed as wontfix.
**#286** (standalone Falcon firmware load) remains open as the
long-term direction if Jetson USB networking re-enters scope.

### 8.7 Lessons for the next attempt

1. **Read what the driver actually does before designing
   interventions.** Option A assumed tegra-xusb had a destructive
   `.shutdown` callback. It didn't.
2. **Test the kexec-inherit model with a no-op first.** The
   "leave Linux's DCBAAP in place, just RUN=1" diagnostic that
   definitively identified SMMU as the blocker should have been
   the first experiment, not the eighth.
3. **The CBB is not the only kexec-time barrier.** `docs/jetson-cbb-report.md`
   §3 covers what the firewall permits; but the SMMU adds a
   separate, orthogonal DMA-path blocker that kicks in during
   the Linux→SLM-OS handoff regardless of CBB state.
4. **Audit every HC-visible allocation for cacheability.** Post-merge
   review of PR #289 found the Event Ring Segment Table sitting in
   cacheable BSS (`static struct xhci_erst_entry xhci_erst[1]`) while
   every other HC-DMA-read structure (DCBAA, scratchpads, rings) was
   `ncmem_alloc`-backed. The HC would have read stale zeros from
   DRAM if the SMMU blocker were ever removed. Fix: move ERST to NC
   memory via a new `xhci_alloc_erst()` helper. Rule for next time:
   grep every `ncmem_alloc` call site for consistency before
   declaring a DMA-heavy driver "code-complete", because one
   cacheable-BSS outlier is very easy to miss.
5. **Doorbell + poll pairs need an explicit `dsb sy` on ARM64.**
   Same review round. The bare-cast doorbell store could be visible
   to the compiler as ordered with the polling read, but the
   HC doesn't see it until the posted write drains — without a
   barrier, a polled event-ring read can sample the cacheline
   before the HC has had a chance to respond. Mitigation: use the
   `w32` helper + explicit `dsb sy` before every poll loop that
   depends on the doorbell reaching the device.
6. **Event-ring polling must update ERDP on every consumed slot.**
   The NO_OP loop originally wrote ERDP only on a matching
   completion, so any skipped stale event left the HC's ERDP view
   frozen while ours advanced. On a mothballed path that never
   produces more than one event this is invisible; on any transfer
   path it would wedge the ring after a few laps. Fix updates ERDP
   on every consumed event (match or skip); regression coverage in
   `test_event_ring_peek_skip_then_match_advances_through_all` +
   `test_event_ring_peek_then_dequeue_phys_tracks_advance`.
7. **Audit memory ordering between NC stores and MMIO writes.**
   Same review round surfaced the flip side of lesson 4: once
   HC-visible data lives in Normal-Non-Cacheable memory, the path
   `CPU stores → write-combine buffer → DRAM → HC DMA read` still
   needs explicit ordering against any MMIO write that tells the
   HC to consume it. ARM ARM B2.7.2 permits Normal-NC stores to be
   reordered relative to Device-nGnRE stores; without a `dsb sy`
   between "fill NC buffer" and "program MMIO that points at it",
   the HC can DMA-read stale contents. Same story for two Normal-NC
   stores that must be visible in program order to a DMA observer
   (the classic TRB payload → cycle-bit pattern): a `dmb oshst`
   between the groups is required. The `ncmem_alloc` move from
   lesson 4 doesn't replace these barriers — it only removes the
   cache-flush half of the problem. Rule for next time: anywhere
   the code reads like "CPU writes X, then CPU tells DMA to read
   X", verify there's a barrier between the two.

---

## 9. Phase 3A.2 IFR Revival — findings (2026-04-18)

Follow-on work to §8's mothball. Hypothesis: `tegra234_soc` has no
`.firmware` field, so the Falcon boots from on-die IFR — which
means SLM-OS may not need HS-mode signed firmware after all. A
clean revival branch (`feature/xhci-ifr-bringup`) ported the minimal
set of Linux's Tegra234 init routines and deployed them to
jetson-nano-1 for hardware verification.

### 9.1 Code landed on the revival branch

| Task | Commit | What it does |
|---|---|---|
| 3A.2.1 | Map FPCI + BAR2 apertures in SLM-OS; add `fpci_r32/w32`, `bar2_r32/w32` helpers; sanity-check dev/vendor = `0x229810de`. |
| 3A.2.2 | Port `tegra_xusb_config` — program `XUSB_CFG_4/7/1` (BARs + bus-master). |
| 3A.2.3 | Port `bar2_csb_r32/w32` — CSB paging via `BAR2+0x9c` (control) and `BAR2+0x2000+ofs` (data). |
| 3A.2.4 | Port `tegra_xusb_wait_for_falcon` — poll `USBSTS.CNR` for 200 ms. |
| 3A.2.5 | Port `tegra_xusb_read_firmware_header` — IFR mailbox IOCTL via `BAR2+0x1000`. |

### 9.2 Hardware-verified findings (jetson-nano-1, L4T 36.4.7 /
kernel 5.15.148-tegra)

- **FPCI wrapper, HCD aperture, BAR2 base** — all readable at NS EL2
  post-kexec. Dev/vendor = `0x229810de` matches Linux's probe. `BAR2[0]
  = 0x00140009` non-dead.
- **Wrapper config** — `CFG_1 = 0x00b00007`, `CFG_4 = 0x0360000c`,
  `CFG_7 = 0x0365000c`. Linux leaves all three correctly programmed
  across kexec; `BUS_MASTER_EN` already set. The #285 "wrapper lost
  bus-master" hypothesis is disproved.
- **BAR0 decoder quirk** — writing `CFG_4` with `TEGRA_XHCI_HCD_BASE`
  (`0x03610000`) is silently rejected; bit 16 is hardwired in the
  BAR decoder. Linux uses `tegra->hcd->rsrc_start` which is
  `0x03600000` (the FPCI aperture base). SLM-OS matches.
- **BAR2 CSB paging control** — writes to `BAR2+0x9c` take effect
  (readback of page-select returns the written value `0x80c`).
- **BAR2 CSB data window** — returns `0xffffffff` for `FALC_CPUCTL`
  and `MP_APMAP` from SLM-OS. **Same result when probed from
  Linux via `/dev/mem`** while the xHCI is active — so CSB reads
  returning `0xffffffff` are NOT a post-kexec signal. On Tegra234
  with IFR boot, these CSB registers appear unpopulated or
  bus-fault-default from direct CPU probing; Linux's tegra-xusb
  driver only touches them on the non-IFR firmware-load error path.
- **BAR2 mailbox region (`BAR2+0x1000`)** — writing triggers a
  TF-A RAS Uncorrectable (SNOC Write Error + GIC ACE-Lite Interface
  Error) that powers off the CPU core. Same failure class as the
  nvgpu post-kexec RAS that `slmos-kexec` already works around for
  the GPU. No equivalent pre-kexec path for xusb. Kept in the code
  as reference but `__attribute__((unused))`; do NOT call at
  runtime.
- **Power domains** — `xusba=1`, `xusbc=1` throughout pre-kexec.
  Clocks `xusb_core_host=1`, `xusb_falcon=1`, etc. all on. Tegra-
  xusb runtime-PM has never been suspended since boot. Power
  is not the blocker.
- **arm-smmu `.shutdown`** — on L4T 36.4.7's 5.15-tegra kernel,
  `drv->shutdown` for the `arm-smmu` platform driver is already
  NULL (A.5 module re-verified this session). But `arm-smmu
  8000000/10000000/12000000.iommu: disabling translation` still
  appears in the kexec dmesg. Something other than the platform-
  driver `.shutdown` callback is invoking `arm_smmu_device_shutdown`.
  Candidates: a reboot notifier, a syscore_ops shutdown, or an
  NVIDIA-specific Tegra bus teardown. Not yet identified.

### 9.3 Definitive conclusion

The Phase 3A blocker is **SMMU teardown during Linux's kexec
handoff**. Wrapper, Falcon, power, and clocks are all fine. The
Falcon is alive; it can read its own registers. But when the
Falcon tries to DMA on `USBCMD.RUN=1`, the translations needed for
the xusb stream have been dropped by whatever code path produces
the "disabling translation" log, and the DMA faults internally —
wedging the aperture.

This definitively confirms #285's original SMMU hypothesis (which
was circumstantial at the time) and rules out: Falcon liveness,
wrapper config, power state, IFR initialization.

### 9.4 Unblocking Phase 3A requires one of

1. **Identify what calls `arm_smmu_device_shutdown` on L4T** and
   patch that code path with a one-function module (same shape as
   scripts/arm-smmu-noshutdown/, targeting whatever the actual
   caller is).
2. **Program the Tegra SMMU from SLM-OS at NS EL2**. The three
   SMMU instances are at `0x08000000`, `0x10000000`, `0x12000000`
   and are NS-accessible. SLM-OS would need to re-enable
   translation for the xusb stream after kexec. Requires porting
   the minimum subset of Linux's arm-smmu-v2 driver (~500 lines
   of relevant code from `docs/reference/linux-arm-smmu.c`'s 2395
   total).
3. **Pre-kexec SMMU bypass.** Globally flip all Tegra SMMUs to
   passthrough mode before kexec via a kernel module; SLM-OS then
   runs without SMMU. Risk: stale DMA from any device can
   corrupt SLM-OS memory. The `iommu.passthrough=1` cmdline we
   tried previously breaks tegra-xusb's own probe on Linux, so
   it can't be set at boot — but it might be programmable
   runtime via the SMMU's `ARM_SMMU_GR0_sCR0` register.

Options (2) and (3) are substantial. Option (1) is the most
aligned with the prior A.4/A.5 module approach.

---

## 10. Handoff — Phase 3A next-phase work

This section is a standalone handoff for a fresh agent (or capstone
session) picking up Phase 3A. Assume the reader has not followed
any of the prior sessions — §10 contains everything needed to
start work on any of the three paths without re-investigation.

### 10.1 What's landed and what it does

On `feature/xhci-ifr-bringup` (this branch), the Tegra234 IFR
bringup path is correctly ported from Linux. The driver is
**functionally correct up to the SMMU blocker** — everything that
SLM-OS can do from NS EL2 without SMMU cooperation is now in place.

Source files to read, in order:

| File | Purpose |
|---|---|
| `kernel/drivers/usb/xhci/xhci_tegra.h` | Tegra234 FPCI + BAR2 register map, cited line-by-line from the cached Linux source |
| `kernel/drivers/usb/xhci/xhci.c` | `tegra_xusb_config`, `bar2_csb_r32/w32`, `tegra_xusb_wait_for_falcon`, `tegra_xusb_read_firmware_header` — all ported from Linux with paragraph-level comments citing the upstream line numbers |
| `docs/reference/linux-xhci-tegra.c` | Cached Linux tegra-xusb source (2849 lines). Always consult this before writing new Tegra code |
| `docs/reference/linux-arm-smmu.c` | Cached Linux arm-smmu-v2 source (2395 lines). Primary reference for any SMMU-port or shutdown-caller-hunt work |

### 10.2 Known-good deploy/test workflow

Any path needs hardware iterations on jetson-nano-1. The workflow:

```
# Build
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO

# Claim the board via labctl MCP (or CLI) for 30+ minutes
# Power-cycle to Linux
# scp build/kernel/slmos.elf to root@192.168.4.20:/root/slmos.elf
# Kexec: ssh root@192.168.4.20 /usr/local/bin/slmos-kexec /root/slmos.elf

# Capture boot via labctl serial_capture
# Expected post-kexec output (from baseline task 3A.2.3 state):
#   xhci: FPCI dev/vendor=0x229810de (expect 0x229810de)
#   xhci: pre-config CFG_1=0x00b00007 CFG_4=0x0360000c CFG_7=0x0365000c
#   xhci: post-config ... (idempotent)
#   xhci: CSB probe FALC_CPUCTL=0xffffffff   (not a bug; see §9.2)
#   xhci: CSB probe CSBRANGE readback=0x0000080c  (paging control works)
#   xhci: HCI v1.20, 36 slots, 8 ports, ...
#   xhci: controller failed to start (USBSTS=0xffffffff)  ← the blocker
```

After `slmos-kexec` runs successfully, SSH to the board is dead
(SLM-OS has no network stack) — interact via serial only, or
power-cycle to return to Linux for the next iteration.

**Two hardware hazards documented by this branch's tests:**

- Writing to `BAR2 + 0x1000` (`XUSB_BAR2_ARU_FW_SCRATCH`, the
  mailbox IOCTL register) from NS EL2 triggers a TF-A RAS
  uncorrectable that powers off the CPU core. Currently guarded by
  `__attribute__((unused))` on `tegra_xusb_read_firmware_header`;
  do NOT un-guard without a plan to survive the RAS.
- Writing `XHCI_CMD_HCRST` (the Intel-spec xHCI reset bit) bricks
  the MMIO aperture on Tegra — it triggers a Falcon-level reset
  that only BPMP can complete. Guarded in `xhci_reset` with a
  comment; do NOT lift the guard.

### 10.3 Path 1 — Hunt the arm_smmu_device_shutdown caller

**Hypothesis:** On L4T 36.4.7 / kernel 5.15.148-tegra, `drv->shutdown`
for `arm-smmu` is NULL, yet the log `arm-smmu 8000000.iommu:
disabling translation` fires on kexec. Something ELSE calls
`arm_smmu_device_shutdown`. If identified and suppressed, SMMU
translations stay live across kexec and SLM-OS inherits working
xusb-stream DMA.

**Starting-point commands on jetson-nano-1:**

```
# Candidates — what could call a platform-driver function outside .shutdown
ssh root@192.168.4.20 'grep -E "(reboot_notifier|syscore|pm_power_off|sdei)" /proc/kallsyms | grep -i smmu'

# Find static registrations via source — L4T kernel headers live
# somewhere under /usr/src/. The exact directory name is version-
# dependent, so look it up first:
ssh root@192.168.4.20 'ls /usr/src/ | grep linux-headers'
# Then (substitute the actual path from the ls output):
ssh root@192.168.4.20 'KDIR=$(ls -d /usr/src/linux-headers-*tegra*/ | head -1); find "$KDIR" -type f \( -name "*.c" -o -name "*.h" \) 2>/dev/null | xargs grep -l "arm_smmu_device_shutdown\|\"disabling translation\"" 2>/dev/null'

# Binary scan fallback: find addresses that contain the shutdown function
# pointer somewhere OTHER than the driver struct
ssh root@192.168.4.20 'grep arm_smmu_device_shutdown /proc/kallsyms'
# Use the address to hunt for cross-references in loaded .ko's (unlikely here)
# or in the vmlinux if available
```

**Once the caller is found, the fix is a kernel module mirroring
scripts/arm-smmu-noshutdown/ that NULLs the relevant hook or patches
the calling code.** The existing module is a template — ~70 lines
including the license boilerplate.

**Effort estimate:** 4-12 hours depending on whether the caller is
a common-pattern reboot_notifier (easy) or a proprietary NVIDIA
Tegra iommu framework call (harder). The caller hunt is the only
unknown.

**Success criterion:** after the module loads, kexec dmesg no longer
contains `arm-smmu ... disabling translation`. Then `slmos-kexec
/root/slmos.elf` and check SLM-OS's xhci init log — `USBCMD.RUN=1`
should succeed, `NO_OP round-trip OK` should appear, and Phase 3A
Steps 5-7 (port scan + transfer rings) are then in-scope.

### 10.4 Path 2 — Port arm-smmu-v2 subset to SLM-OS

**Hypothesis:** Rather than preventing Linux from tearing down SMMU
translations, have SLM-OS re-establish them at NS EL2 after kexec.
Three SMMU instances at `0x08000000`, `0x10000000`, `0x12000000`
are NS-accessible on Tegra234 (`ls /sys/bus/platform/drivers/arm-smmu/`
on the Jetson confirms).

**Minimum subset to port from `docs/reference/linux-arm-smmu.c`:**

- `arm_smmu_write_context_bank` (~40 lines) — programs a context
  bank's translation registers
- `arm_smmu_alloc_context_bank` / assignment logic (~60 lines)
- `arm_smmu_gr0_write` / base-addressing helpers (~30 lines)
- S1/S2 page-table walk setup for IOVA→PA (~150 lines, can use
  identity mapping only to start)
- `arm_smmu_context_fault` handler stub (~30 lines)
- MMIO access helpers + register-bit macros (already partly ported
  into SLM-OS's existing MMIO infrastructure)

Estimated ~350-500 lines of actual SLM-OS code, drawing from
~800 lines of the reference. Then:

- Identify the xusb stream ID by reading the DT fragment on the
  Jetson: `cat /proc/device-tree/bus@0/usb@3610000/iommus` (binary,
  decode the `iommus` property — the second cell is the stream ID).
- In SLM-OS `xhci_init`, after `tegra_xusb_config`, call the new
  `tegra_smmu_setup(stream_id, pgtable_root)` helper. `pgtable_root`
  can be `ncmem_alloc`'d and identity-mapped for simplicity (every
  IOVA == PA, which is what Linux would've given us anyway for a
  DMA-coherent device).
- Verify by watching post-kexec `slmos>` log for successful RUN=1
  and NO_OP completion.

**Effort estimate:** 1-2 weeks of focused work. Most of the risk
is in hitting the right register sequence; Linux's implementation
is the gold standard to match.

**Success criterion:** same as Path 1 — RUN=1 succeeds, NO_OP
round-trips. Additional bonus: this gives SLM-OS a general-purpose
SMMU capability that unblocks #286 (standalone Falcon firmware load)
too, and helps any future Tegra DMA-user (PCIe, NVMe via xHCI,
other platform devices).

### 10.5 Path 3 — Pre-kexec global SMMU bypass

**Hypothesis:** Before kexec, flip every arm-smmu instance into
CLIENTPD-off passthrough mode via a kernel module, so SMMU
translations stay live-but-identity for everyone. SLM-OS then sees
raw physical addresses and doesn't need to touch the SMMU at all.

**Design:**

- Kernel module installed alongside `slmos-kexec`, loaded by the
  script BEFORE `kexec -e`.
- For each `arm_smmu_device` registered (via `/sys/bus/platform/drivers/arm-smmu/*`),
  acquire the platform data pointer, read `sCR0`, write `sCR0 & ~CLIENTPD`
  (enable all clients bypassing) and set `sACR` to disable faulting.
- Best-effort. Errors log but don't fail — we'd rather crash on RAS
  than fail the kexec outright.

**Precedent:** `scripts/arm-smmu-noshutdown/` is the template; this
is the same shape (one-function kernel module) but writes to the
SMMU instead of NULL-ing a pointer.

**Risk:** With SMMU globally in passthrough, any residual in-flight
DMA from other drivers (NVMe, network, audio) can scribble into
SLM-OS's memory. In practice those drivers' own teardown paths drain
pipelines first — but this is diagnostic-only.

**Effort estimate:** 2-4 hours to write the module + 2-4 hours of
on-hardware iteration until it doesn't crash.

**Success criterion:** same — RUN=1 succeeds, NO_OP completes.

**Note:** the `iommu.passthrough=1` cmdline flag we tried earlier
this investigation breaks tegra-xusb's probe on Linux (Falcon
firmware load fails), so that's NOT equivalent to this runtime
approach. A runtime post-probe bypass is the key distinction.

### 10.6 What's been tried and ruled out — don't re-chase these

- **Keep Falcon alive via tegra-xusb `.shutdown`=NULL (A.4)**:
  `scripts/tegra-xusb-noshutdown/`. Works but was the wrong
  problem — tegra-xusb has no `.shutdown` callback on any tested
  L4T kernel.
- **Keep SMMU alive via arm-smmu `.shutdown`=NULL (A.5)**:
  `scripts/arm-smmu-noshutdown/`. The original template version
  reported "shutdown already NULL" on L4T 36.4.7 because it was
  NULLing the wrong struct field — §10.8 has the follow-up
  investigation. The current version NULLs the correct field and
  demonstrably prevents arm_smmu_device_shutdown() from firing,
  but on its own does NOT unblock NO_OP.
- **iommu.passthrough=1 cmdline**: breaks tegra-xusb probe (Falcon
  firmware load fails); aperture is not readable post-boot. Do not
  re-add this flag.
- **Panic-kexec path (`kexec -p`)**: skips device_shutdown() but
  loads kernel at the crashkernel reserved region (0xEFE00000),
  which doesn't match SLM-OS's 0x80000000 link address. Would
  require significant SLM-OS relocation work.
- **HCRST (xHCI reset)**: bricks the Tegra XHCI aperture
  permanently. Guarded in `xhci_reset`; do not call.
- **Wrapper programming** (`tegra_xusb_config`): correctly ported,
  but confirmed that Linux leaves the wrapper in a good state
  post-kexec; this is defensive-only, not a fix.
- **BAR2 FW_SCRATCH IOCTL probe**: triggers RAS uncorrectable →
  core power-off. Guarded with `__attribute__((unused))`.

### 10.8 After-action from the 2026-04-18 Path 1 session

This section updates §10.3 / §10.5 with what was learned on
jetson-nano-1 hardware during a full Path 1 / Path 3 attempt.
Everything here is observed behaviour, not hypothesis.

**Caller of `arm_smmu_device_shutdown` identified.** On L4T 5.15.148
the caller is `platform_drv_shutdown` (the platform bus's own shutdown
dispatch, installed as `platform_bus_type.shutdown = platform_shutdown`).
A diagnostic module that reads both halves of the driver struct
shows:

```
smmu-probe:   .shutdown = arm_smmu_device_shutdown+0x0/0x40
smmu-probe:   .driver.bus->shutdown = platform_shutdown+0x0/0x60
```

The previous template's "shutdown already NULL" result came from a
struct-field bug: it was NULLing `device_driver.shutdown` (the base
struct), but the platform bus dispatches shutdown through
`platform_driver.shutdown`, a SEPARATE field on the outer
platform_driver struct. NULLing the base field is a no-op for a
platform driver; the correct field is reached via
`to_platform_driver(drv)`. That fix is now in
`scripts/arm-smmu-noshutdown/arm_smmu_noshutdown.c` and verified on
hardware (post-load, the diagnostic reports `.shutdown = (null)`
and kexec dmesg no longer contains `arm-smmu ... disabling
translation`).

**Path 1 alone does NOT unblock NO_OP, and does not reliably even
unblock `USBCMD.RUN=1`.** Four empirical runs (plus a baseline
control) on a freshly-booted jetson-nano-1 all ended in the same
state SLM-OS reported before this session:

```
[WARN] xhci: controller failed to start (USBSTS=0xffffffff)
```

A single earlier run on the same hardware (board had been up for
~112 minutes with miscellaneous module-load / probe operations in
between) saw `USBCMD.RUN=1` succeed (`USBSTS=0x00000000`) followed
by NO_OP timeout, but that result was not reproducible. The aperture
wedge is the more common outcome; the Path 1 field fix alone is
INSUFFICIENT to meet §10.3's "RUN=1 does not wedge the aperture"
success criterion by itself.

**Why the bypass variants do not improve matters.** Four variants
beyond plain NULL were tested and all regressed or behaved no
better:

1. Replacement `.shutdown` that writes `sCR0 = CLIENTPD` but skips
   the clock teardown — wedges the aperture. Tegra-xusb has no
   `.shutdown`, so the xHCI is still actively DMA-ing when the
   bypass flips; in-flight IOVAs (e.g. DCBAAP = 0x7ffffff000) get
   reinterpreted as raw PAs in unmapped regions and the HC's next
   DMA trips a fabric error.
2. Same replacement but targeting only SMMU0 (the instance that
   serves xusb per the DT iommus phandle → 0xf0 / stream ID 0x0e)
   — wedges identically.
3. Same replacement preserving existing sCR0 bits (`value | CLIENTPD`
   instead of `value = CLIENTPD`) — wedges identically.
4. SLM-OS-side `sCR0 = CLIENTPD` write after `xhci_halt()`, with
   the NULL-field module preserving SMMU clocks across kexec —
   wedges identically. Reverted; see
   `git log scripts/arm-smmu-noshutdown/` for the full session
   history.
5. reboot_notifier firing from `kernel_restart_prepare()` —
   produces `tegra-mc: EMEM address decode error` messages during
   Linux's remaining shutdown steps, because the notifier fires
   BEFORE `device_shutdown()` drains xhci / other DMA masters.
   `syscore_shutdown()` is NOT called on the kexec path in 5.15
   (only on `kernel_restart()`), so there is no cross-subsystem
   hook that fires AFTER `device_shutdown()`.

**New follow-on blocker to track.** What the Path 1 field fix
achieves in practice is "Linux no longer writes CLIENTPD into the
SMMU on kexec, and no longer tears down SMMU clocks". The fabric
is therefore in a cleaner state on entry to SLM-OS, but the xHCI
controller's state and the SMMU's active Linux context banks are
unchanged. The next step — either Path 2 (port arm-smmu-v2 subset
so SLM-OS can program its own context bank that maps its raw PAs)
or a more careful pre-kexec quiescing sequence — is now the
documented follow-on.

### 10.7 Contact points for a blocked investigation

- **GitHub issues**: #266 (Phase 3A umbrella), #285 (original SMMU
  blocker, wontfix — re-open if a path pans out), #286 (standalone
  Falcon firmware load, deprioritized by the IFR finding).
- **Reference code**: `docs/reference/linux-{arm-smmu,xhci-tegra}.c`.
  When pulling new Linux files, save them here — don't re-fetch.
- **Lab hardware**: `labctl` MCP. See `kernel/CLAUDE.md` for board
  claim / serial / power workflows. Jetson-nano-1 has serial
  access via labctl but SSH only works when it's in Linux (SLM-OS
  has no network stack).

---

*Last updated: 18 April 2026*
