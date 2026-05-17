# USB Host Generalization Plan

Detailed follow-on plan for turning the current Jetson USB networking
path into broader USB host support.

**Tracking issue:** [#384](https://github.com/SLM-OS/SLM-Operating-System/issues/384)

**Current state (April 2026):**
- `usb_core` can enumerate one validated USB 2.0 topology on Jetson:
  a retained high-speed root hub plus one downstream CDC-ECM NIC.
- `cdc_ecm` is the only production class driver on this path.
- The shipped path is intentionally narrow: one-tier hub transport,
  boot-time enumeration, no generic hotplug, no generic multi-device
  policy, no general alternate-setting support.

This document covers the next step: **generalize the USB host stack
beyond the current NIC-specific path** while preserving the validated
Jetson `kexec` recovery flow that already landed.

---

## 1. Goal

Expand SLM-OS USB host support from the current "one retained hub + one
downstream NIC" path into a reusable USB 2.0 host stack that can handle:

- multiple devices on one hub
- deeper hub topologies
- disconnect / reconnect
- interface alternate settings
- additional USB classes beyond CDC-ECM

The immediate objective is not "support every USB device." It is to
replace the current narrow lab-path assumptions with a generic host
model that other class drivers can build on safely.

---

## 2. Non-Goals

This plan does **not** attempt to land all of USB in one step.

Explicitly out of scope for this phase:

- USB 3.x SuperSpeed-specific bring-up
- isochronous transfers (audio/video)
- full suspend / resume / remote wakeup
- Type-C policy / role-switch / PD negotiation
- OTG / gadget-mode work
- arbitrary class-driver coverage on day one

Those can follow once the USB 2.0 host topology and lifecycle model are
sound.

---

## 3. Success Criteria

This plan is complete when all of the following are true:

1. `usb_core` can represent more than one live device at a time.
2. Hub traversal is generic rather than hardcoded to the current
   retained-root-hub shape.
3. Disconnect and reconnect are first-class events, not special Jetson
   recovery branches.
4. Interface alternate settings are represented explicitly and can be
   activated with `SET_INTERFACE` where needed.
5. Class drivers bind by interface / descriptor matching rather than by
   assuming one globally interesting device.
6. The existing Jetson CDC-ECM networking path still passes after the
   generic changes.
7. At least one non-NIC USB class is working on top of the new model.

---

## 4. Current Gaps

### 4.1 Core model is still single-device shaped

`usb_core` still treats the current root/hub/NIC path as a special case:

- one exposed `root_device`
- one special `hub_device`
- one **bound** downstream child — the walker (#575) iterates every
  downstream port looking for a CDC-ECM-capable device, but it
  commits to exactly one and ignores the rest
- one global enumeration result the rest of the system consumes

That is enough for the current NIC demo, but it is not a generic host
topology model.

### 4.2 Hub support is transport scaffolding, not a general subsystem

The current hub handling is sufficient for:

- one retained root hub on Jetson
- one CDC-ECM downstream child anywhere in the hub's port table
  (since #575 the walker skips non-CDC peripherals on lower-numbered
  ports — a USB-A pass-through dongle with a keyboard plugged in
  works correctly)

It is not sufficient for:

- binding multiple downstream devices simultaneously
- multi-tier hub traversal (hub-of-hub)
- generic port change handling
- disconnect cleanup

### 4.3 Alternate-setting handling is too limited

The parser currently keeps only one alternate setting per interface
number. That was acceptable for the current NIC path, but not for
general USB support.

Generic USB needs:

- storage of all discovered alternate settings
- an explicit "active alt" state
- HCD support for `SET_INTERFACE`
- endpoint reconfiguration when the active alt changes

### 4.4 Class-driver binding is still narrow

`cdc_ecm` works, but the host stack still lacks a general class-binding
framework. The system needs a clean way to say:

- "this interface matches a driver"
- "claim this interface"
- "do not let two drivers bind the same interface"
- "unbind and clean up on disconnect"

### 4.5 Hotplug lifecycle is not generalized

The current hotplug / stale-attach logic is still mostly there to make
the Jetson `kexec` path reliable. That is different from a true
platform-independent lifecycle model for:

- attach
- debounce
- enumerate
- disconnect
- re-enumerate

---

## 5. Proposed Execution Plan

### Phase A — Core Topology Model

Replace the current single-root/special-hub structure with a generic
device graph.

#### Deliverables

- Introduce a fixed-capacity USB device table for Phase A.
- Each device tracks:
  - parent device pointer or slot id
  - parent-facing port number
  - route string / root-hub port
  - address
  - speed
  - active configuration
  - interface set
  - lifecycle state
- Distinguish:
  - root-hub device
  - hub device
  - ordinary function device
- Add explicit ownership / binding state per interface.

#### Implementation notes

- Keep static capacity at first; do not require dynamic heap allocation.
- Preserve the current NC-memory / ring ownership model for xHCI.
- Keep the public API narrow until the internal model stabilizes.

#### Exit criteria

- The current Jetson path still works using the new device table.
- Unit tests cover multi-device bookkeeping without a live controller.

---

### Phase B — Generic Hub Enumeration

Promote hub handling from "transport detail" to a reusable subsystem.

#### Deliverables

- Generic hub descriptor + port-status traversal logic.
- Breadth-first or depth-first enumeration of downstream devices.
- Address allocation for more than one child.
- A configurable maximum topology depth for Phase B
  (for example: 2 tiers first, then lift later if needed).
- A generic per-port attach state machine for hubs.

#### Implementation notes

- Reuse the current USB 2.0 hub control-request helpers where possible.
- Keep the current Jetson retained root-hub path as one producer of the
  generic attach events, not a special enumeration path.
- Preserve route-string and root-port information all the way into
  xHCI device state.

#### Exit criteria

- One hub with multiple children enumerates correctly.
- A two-tier USB 2.0 hub tree enumerates correctly.
- Existing one-tier Jetson NIC flow still passes unchanged.

---

### Phase C — Alternate Settings and Interface Activation

General USB support needs explicit control over active interface
settings.

#### Deliverables

- Parse and store all alternate settings for each interface number.
- Add "active alt setting" to the device model.
- Add `usb_set_interface()` to the core API.
- Add HCD support for endpoint-context changes required by
  `SET_INTERFACE`.
- Update endpoint lookup so it resolves against the active alt rather
  than the last parsed descriptor that happened to win.

#### Why this matters

This is required for many real devices, not just networking:

- some NICs expose operational data endpoints on non-default alts
- audio and video devices use alternates heavily
- composite devices may expose operational interfaces only after an alt
  switch

#### Exit criteria

- Tests cover multi-alt interface parsing and switching.
- At least one device path requiring `SET_INTERFACE` works end-to-end.

---

### Phase D — Generic Attach / Detach / Re-enumeration

Turn the current Jetson-biased recovery logic into a general USB device
lifecycle model.

#### Deliverables

- Generic per-port lifecycle:
  - disconnected
  - debounce / wait-stable
  - enumerating
  - configured
  - disconnect cleanup
- Device teardown that reliably:
  - unbinds class drivers
  - cancels URBs
  - releases HCD resources
  - frees device-table slots
- Re-enumeration after disconnect without reboot.

#### Implementation notes

- Keep Jetson-specific retained-slot recovery in the xHCI layer, but
  make it feed the same generic attach/detach model as a clean boot.
- Avoid entangling generic USB hotplug policy with Tegra-only handoff
  heuristics.

#### Exit criteria

- Repeated unplug/replug works on the validated Jetson hub path.
- Disconnect of one child does not poison the rest of the hub tree.

---

### Phase E — Class Binding Framework

Generalized topology is only useful if drivers can bind to interfaces in
a disciplined way.

#### Deliverables

- Add class-driver registration with match callbacks.
- Match criteria should allow:
  - interface class/subclass/protocol
  - VID/PID overrides where needed
  - composite-device interface claims
- Add bind / unbind hooks.
- Add interface-claim tracking so only one class driver owns an
  interface.

#### First consumers

- `cdc_ecm` migrated to the generic class-binding framework
- one additional non-network class driver, chosen for practical value:
  - **USB HID boot keyboard** is the best first target
  - **USB mass storage (BOT)** is the next most valuable follow-on
  - **USB CDC ACM serial** is also high-value and straightforward

#### Exit criteria

- `cdc_ecm` no longer depends on a single globally interesting device.
- At least one non-NIC driver binds and works through the same framework.

---

### Phase F — Validation Matrix and Regression Harness

The current success path is real but narrow. This phase turns it into a
repeatable matrix.

#### Required test layers

- unit tests for:
  - device-table bookkeeping
  - hub traversal
  - alternate-setting resolution
  - attach/detach state machine
  - class-binding rules
- controller-facing unit tests for:
  - xHCI slot bookkeeping under multi-device load
  - endpoint reconfiguration on `SET_INTERFACE`
  - disconnect teardown paths
- hardware smoke tests for:
  - current Jetson retained-root-hub + RTL8153 path
  - multiple downstream devices on one hub
  - disconnect / reconnect of one child
  - keyboard attach + usable input path
  - storage or ACM path, depending on which driver lands first

#### Practical lab matrix

Minimum matrix for signoff:

- `jetson-nano-2`:
  - hub + RTL8153 already attached before `kexec`
  - hub + RTL8153 + second child attached
  - disconnect / reconnect second child
- non-Jetson mock / QEMU-hosted tests:
  - generic USB core unit tests
  - class-binding tests
  - no-regression test for `make test`

#### Exit criteria

- The generalized path has an explicit regression checklist, not just an
  ad hoc lab recipe.

---

## 6. File-Level Impact

This work will primarily touch:

- `kernel/include/usb.h`
- `kernel/usb/core/usb_core.c`
- `kernel/usb/class/cdc_ecm.c`
- `kernel/drivers/usb/xhci/xhci.c`
- `kernel/drivers/usb/xhci/xhci_device.c`
- `kernel/drivers/usb/xhci/xhci_xfer.c`
- `kernel/tests/test_usb_core.c`
- `kernel/tests/test_xhci_device.c`
- `kernel/tests/test_cdc_ecm.c`

Likely new files:

- generic class-driver registration header/source
- one additional class driver (`hid_boot.c`, `usb_acm.c`, or `usb_msc.c`)
- new unit-test files for topology / binding / hotplug

---

## 7. Recommended Order of Execution

Recommended landing order:

1. Phase A — core topology model
2. Phase B — generic hub enumeration
3. Phase C — alternate-setting support
4. Phase D — generic attach/detach lifecycle
5. Phase E — class-binding framework
6. Phase F — broadened lab validation and cleanup

This keeps the work layered:

- topology first
- lifecycle second
- drivers on top

That is the lowest-risk way to preserve the already-working Jetson path
while generalizing the stack.

---

## 8. Risks

### 8.1 Regressing the current Jetson success path

The validated `nano-2` path is the best hardware proof we have today.
Generic changes must not casually throw that away.

Mitigation:

- keep the current Jetson smoke test in the gating set
- land topology/model changes before broad driver churn
- preserve retained-slot/handoff logic behind generic interfaces rather
  than deleting it early

### 8.2 Letting generic policy leak into xHCI quirks

Jetson kexec recovery contains platform-specific behavior. That should
stay in the xHCI/Tegra layer, not spread into generic USB policy.

Mitigation:

- generic USB core should consume normalized attach/detach events
- controller-specific recovery remains below that line

### 8.3 Trying to land too many class drivers at once

General USB support can sprawl quickly.

Mitigation:

- land only one additional class driver first
- prefer HID boot keyboard as the first generic proof point

---

## 9. Concrete Follow-On Issues

This plan should break down into follow-on implementation issues after a
tracking ticket exists:

- multi-device USB core model
- generic hub traversal beyond one child
- alternate-setting / `SET_INTERFACE`
- generic disconnect / reconnect
- class-driver binding framework
- first non-NIC USB class driver
- regression harness for USB topology cases

---

## 10. Recommended First Milestone

The best first milestone is:

**Generalize `usb_core` from the current root/hub/child special case to
a fixed-capacity multi-device topology model, while preserving the
existing Jetson CDC-ECM success path.**

That milestone unlocks everything else without prematurely committing to
HID, storage, or other class specifics.

---

## 11. Summary

The next USB step is not "more DHCP work." It is to convert the current
validated Jetson USB networking path into a general USB host foundation.

Do that in this order:

1. topology model
2. generic hub traversal
3. alternate settings
4. attach/detach lifecycle
5. class binding
6. broader hardware validation

That is the smallest coherent path from today's working NIC demo to
real generalized USB host support.
