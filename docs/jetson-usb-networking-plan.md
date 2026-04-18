# Jetson USB Networking Plan

Detailed implementation plan for USB-based networking on Jetson Orin
Nano. **Option A (USB-A host port + CDC-ECM dongle)** is the primary
target. **Option B (USB-C device mode + CDC-ECM gadget)** is a fallback
only pursued if the Phase 0 CBB probes rule Option A out.

**Status:** Phase 0 complete (Option A chosen); Phase 1 scaffolding landed
on `feature/usb-networking-phase0`. Tracked in
[#266](https://github.com/SLM-OS/SLM-Operating-System/issues/266). Per-phase
status lives in §7; outcomes from the Phase 0 decision gate are in §3.

| Phase | Status | Notes |
|---|---|---|
| 0 — CBB probe | ✅ done (2026-04-17) | Option A viable; clock-gated, not firewalled. See §3 Phase 0. |
| 1 — USB core (`kernel/usb/core/`) | ✅ scaffolded | URB / descriptor / enumeration with 37 unit tests against a mock HCD. |
| 2 — CDC-ECM class driver | ✅ scaffolded | Probe + MAC parse + bulk IN/OUT data path + net_driver glue; 18 unit tests. |
| 3A — XHCI host driver | ☐🔗 pending | Requires Phase 2 + kexec clock-hold extension. |
| 4 — lwIP netif integration | ☐🔗 pending | Requires Phase 2. |
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
| `kernel/tests/test_cdc_ecm.c` | ~510 | 18 tests against a CDC-ECM-shaped mock HCD — MAC parser edge cases (too short, wrong type, non-ASCII, non-hex, null args), full probe + registration + MTU extraction, `net_init` RX queuing, `send` happy path + pool exhaustion + oversized + null, `recv` happy path + empty + small-buffer truncation, `iMACAddress==0` fallback, rejection of non-CDC devices |

Architecture notes:
- **Static slot pools, no heap.** 4 RX + 4 TX slots × 2 KB each = 16 KB BSS. Keeps Phase 2 free of dynamic allocation; Phase 3A XHCI decides whether these get relocated to NC memory for DMA coherence.
- **Completion callbacks tolerate IRQ context.** RX/TX completion paths touch only volatile slot flags and atomic counters, not the net_driver dispatch — good for the Phase 3A XHCI IRQ path.
- **Probe is self-resetting.** Each call clears `cdc.probed` at entry so a retry that finds no device (or a non-CDC device) doesn't leave a stale MAC visible via `cdc_ecm_get_mac()`.
- **MAC fallback is non-zero.** If `iMACAddress == 0` (or parsing fails), the driver synthesises `02:53:4C:4D:xx:xx` — locally administered, unicast, with "SLM" in the high bytes.
- **DMA discipline deferred to Phase 3A.** Slot buffers are cacheable BSS today. The XHCI driver will either issue DC CVAC/CIVAC around submit/complete or relocate these to `ncmem_alloc`; the class driver stays agnostic.

**Reference material:**
- CDC-ECM spec is public (CDC 1.2 ECM subclass spec).
- Linux `drivers/net/usb/cdc_ether.c` — ~1000 LoC, well-commented.

### Phase 3A — XHCI Host Driver (3-4 weeks, Option A primary)

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

*Last updated: 17 April 2026*
