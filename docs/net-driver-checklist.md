# NIC Driver Review Checklist

When a new NIC driver is proposed (`virtio_net.c`, `virtio_net_pci.c`,
future `bcm_genet.c` for Pi 5, `eqos.c` for Jetson, etc.), reviewers
should walk this checklist before approving.

The checklist exists because PR #212 found three latent bugs in the
original VirtIO-Net driver — 10-byte virtio header with VERSION_1,
hardcoded slot 0, legacy vs. modern MMIO — that were invisible under
the old "unit-tests-only" regime. The fix was a live-integration test
suite that every new NIC driver must pass. This file is the reviewer
hook for that discipline. See **#214** for the tracking issue.

---

## Driver contract

- [ ] Implements `struct net_driver` from `kernel/include/net_driver.h`
      with all six ops (`name`, `init`, `send`, `recv`, `get_mac`,
      `link_status`)
- [ ] Exports a registration function (e.g. `bcm_genet_register`) that
      calls `net_register_driver(&my_driver)` — not a constructor,
      called explicitly from `kernel/src/main.c` platform init
- [ ] `send()` blocks no longer than `VIRTIO_NET_TX_TIMEOUT_MS`
      (100 ms) — see `kernel/include/virtio.h` constant
- [ ] `recv()` returns 0 promptly when no packet is available (polled
      from `net_poll()` — must not block)
- [ ] `get_mac()` returns a non-zero MAC address after `init()` has
      run successfully
- [ ] `link_status()` reflects the current physical link state
- [ ] Bumps `net_stats_rx_no_buffers_inc()` from `net.h` whenever the
      RX descriptor pool is exhausted (silent drops are a review
      blocker)

## Build wiring

- [ ] `CMakeLists.txt`: driver `.c` file gated on the correct
      `PLATFORM == ...` expression under the networking section
- [ ] `kernel/src/main.c`: registration call under matching
      `#if defined(PLATFORM_...)` guard, located alongside the existing
      `virtio_net_register()` / `virtio_net_pci_register()` calls
- [ ] `ENABLE_NETWORKING` default for the platform flipped to `ON` in
      `CMakeLists.txt` (otherwise the driver compiles but lwIP doesn't)
- [ ] `Makefile` `QEMU_NET` variable extended if the new driver has
      a QEMU equivalent (most real-hardware drivers don't)

## Live integration tests

All eight tests below must pass on the target platform. For real
hardware they run via `labctl boot_test`; for QEMU-testable drivers
they run in `make test`. Source: `kernel/tests/test_net.c`.

- [ ] `test_net_driver_registered` — registration hook fires
- [ ] `test_net_init_live` — driver probes device, reads MAC, brings
      link UP (skips cleanly if device absent)
- [ ] `test_net_poll_after_init` — recv path doesn't fault on empty
      ring
- [ ] `test_net_auto_dhcp_at_boot` — DHCP state machine starts
- [ ] `test_net_dhcp_binds` — end-to-end DHCP exchange reaches BOUND
- [ ] `test_net_dhcp_bind_notification` — `[INFO] DHCP bound:` line
      logged (verifies `net_poll()` is being invoked at a reasonable
      cadence)
- [ ] `test_net_dhcp_fallback` — timeout → `NET_DHCP_FAILED`, static IP
      restored (uses `net_set_dhcp_timeout_ms(0)` +
      `net_dhcp_check_timeout()` for determinism)
- [ ] `test_net_driver_tx` — raw 64-byte frame traverses the TX
      virtqueue to completion (validates `send()` without going through
      lwIP)

If the driver skips a test (e.g. no DHCP infrastructure on the test
network), the test must `TEST_IGNORE_MESSAGE` rather than `PASS`
silently.

## Platform-specific tests to add

Include one test per identified risk area for the new hardware:

- [ ] Cache coherency: DMA buffers correctly flushed/invalidated (see
      `docs/networking.md` §"Descriptor Ring Cache Maintenance" and
      the verification plan in
      [`docs/net-dma-coherence.md`](net-dma-coherence.md) for the
      first real-hardware driver). Only applicable on platforms with
      `PLATFORM_HAS_NC_MEMORY` (Pi 5, Jetson) — x86-64 is coherent,
      QEMU ARM64 is trivially coherent
- [ ] Large packet (MTU-sized) TX and RX round-trip
- [ ] Back-to-back burst of ≥64 packets without `rx_no_buffers`
      increment (validates the re-post path)
- [ ] Link-down / link-up toggle — `link_status()` reflects it,
      pending TX fails with the correct error
- [ ] If the driver implements IRQ-driven TX completion (#204):
      a test that fills the TX ring, verifies completion via IRQ,
      and confirms no stall when completion is delayed

## Docs

When the driver lands, every item below must be updated in the same
PR. A review that merges a new driver without these changes breaks
the discipline this checklist exists to maintain.

- [ ] `docs/networking.md` — new `### <Driver> Driver (<platform>)`
      subsection under `## Implementation`, describing the transport
      (MMIO / PCI / USB / etc.), discovery sequence, feature set, and
      any platform quirks
- [ ] `docs/networking.md` — Platform Support table: change the row
      from "Not implemented" to "Implemented" with the driver file
      reference
- [ ] `docs/networking-expansion-plan.md` — mark the corresponding
      Phase as LANDED with the merge commit reference
- [ ] `docs/architecture.md` — add a row under the Networking Subsystem
      table
- [ ] `docs/pi5-platform-audit.md` (for Pi 5) or equivalent platform
      audit doc — update the Networking row
- [ ] If the driver introduces a new tunable CMake option, update
      `docs/building.md` and list it in the networking README

## Reviewer smoke test

Even after all the above passes in CI, the reviewer should manually
walk through an interactive session on a dev box / lab rig:

```
SLM-OS> net init
SLM-OS> ifconfig            # MAC present, link UP, IP assigned
SLM-OS> ping 10.0.2.2       # or the lab gateway
SLM-OS> netstat             # no_buffers=0 after idle
```

Flaky or garbled output on this smoke test is a review blocker even
if the automated tests pass — `make test` runs at QEMU time dilation,
real hardware stresses timing differently.

---

*Checklist origin: PR #212 (networking multi-platform expansion).
 Tracking: #214.*
