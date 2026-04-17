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
- [ ] `send()` is **asynchronous** (#204): submit, kick the device,
      return immediately. Returns 0 on submit, `NET_E_BUSY` if the
      driver's TX pool is exhausted, or another `NET_E_*` for size /
      protocol errors. Must **not** spin waiting for completion. New
      regression test `test_net_send_returns_quickly` enforces a
      10 ms upper bound on the call
- [ ] Driver implements `tx_reap` op — drains TX used ring + frees
      pool buffers. Called by `net_poll()` once per poll. Sized
      pool ≥ 8 buffers so back-to-back lwIP sends don't blow through
      it during ARP / DHCP bursts
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

All twelve tests below must pass on the target platform. For real
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
- [ ] `test_net_driver_has_tx_reap` — driver exposes async TX reap op
      (#204)
- [ ] `test_net_send_returns_quickly` — `send()` returns in <50 ms
      (guards against spin-wait regression, #204)
- [ ] `test_net_send_oversized_rejected` — >1514 byte packet returns
      `NET_E_TOO_LARGE` (#204)
- [ ] `test_net_send_pool_exhaustion` — 32 submits without poll only
      return 0 or `NET_E_BUSY`; no unexpected errors, no spin (#204)
- [ ] `test_net_burst_8_sends_async` — 8 back-to-back async submits
      succeed without blocking (#204)

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
      - Register against the platform's dispatch table:
        `gic_register_handler` on ARM64, `irq_register` (vector-32)
        on x86-64. Both paths drain the TX used ring from the
        handler so pool slots free without waiting for `net_poll`.
      - Expose four accessors for tests and diagnostics:
        `<driver>_get_irq` (runtime IRQ/vector number),
        `<driver>_get_irq_count` (bumped on every handler entry),
        `<driver>_<bus>_enabled` (IRQ path is live vs polled
        fallback — e.g. `virtio_net_pci_msix_enabled`), and
        `<driver>_get_tx_stall_count` (watchdog fire count).
      - Mirror of the ARM64 MMIO tests on x86-64:
        `test_net_msix_enabled`, `test_net_msix_vector_is_in_range`,
        `test_net_msix_handler_drains_tx`. End-to-end hardware IRQ
        delivery is verified on real hardware; QEMU tests exercise
        the registration + handler-body paths directly.
- [ ] If the driver implements the stuck-descriptor watchdog
      (#204 item 4):
      - `tx_reap_locked` calls a separate `tx_watchdog_warn_if_stuck`
        helper that logs **exactly once** per stall episode (latch
        resets on the next successful reap). Threshold is
        `TX_STALL_THRESHOLD_MS = 5000`.
      - Expose `<driver>_get_tx_stall_count()` and a test-only
        `<driver>_test_trigger_watchdog()` that invokes the
        watchdog check with synthetic inputs (no-progress +
        in-flight + elapsed > threshold).
      - Two regression tests per driver: `_watchdog_quiet`
        (stays silent on healthy traffic) and
        `_watchdog_fires_on_stall` (counter advances by exactly 1
        when the trigger fires).

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
