# NIC DMA Coherence on Real ARM64 Hardware

Investigation of whether the current virtio-net cache maintenance
contract correctly supports a DMA-capable NIC on platforms where
secondary-CPU caches don't participate in coherency (Pi 5, Jetson).

Tracking: **#203**. Blocks or shapes #202 (Pi 5 BCM GENET) and #25
(Jetson EQOS).

Status: **investigation only**. The `virtio_net.c` cache-maintenance
calls were added for QEMU, which is trivially coherent — the path
has never been exercised against a device that actually DMAs through
the Point of Coherency (PoC) on Pi 5 or Jetson. This document
captures the current model, the open questions, and the verification
plan for the first real-hardware NIC driver (#202 or #25, whichever
lands first).

---

## Current model

### Cache-maintenance helpers

Defined in `kernel/include/cache.h`. On `PLATFORM_HAS_NC_MEMORY`
platforms (Pi 5, Jetson), they expand to real ARM64 cache
instructions; elsewhere they are barriers or no-ops.

| Helper | ARM64 instruction | Purpose |
|---|---|---|
| `cache_clean(addr)` | `dc cvac, %0` + `dsb sy` | Push dirty cacheline to PoC; device can now see the data |
| `cache_invalidate(addr)` | `dc civac, %0` + `dsb sy` | Write back if dirty, then invalidate; forces re-read from PoC on next load |
| `cache_clean_range(addr, size)` | loop of `dc cvac` + `dsb sy` | Same as `cache_clean`, multiple lines |
| `cache_invalidate_range(addr, size)` | loop of `dc civac` + `dsb sy` | Same as `cache_invalidate`, multiple lines |
| `cache_discard_range(addr, size)` | loop of `dc ivac` + `dsb sy` | Invalidate without writeback — destructive |

CACHE_LINE_SIZE = 64 (matches Cortex-A76 on Pi 5 and Cortex-A78AE on
Jetson).

### Where virtio-net uses them

`kernel/drivers/virtio_net.c::virtqueue_add_buf()` writes three regions
the device will DMA-read, then cleans each:

```c
/* After writing descriptor, avail ring slot, avail idx */
cache_clean_range(&vq->desc[desc_idx], sizeof(vq->desc[desc_idx]));
cache_clean_range(&vq->avail->ring[avail_idx], sizeof(vq->avail->ring[avail_idx]));
cache_clean_range(&vq->avail->idx, sizeof(vq->avail->idx));
```

`virtqueue_get_buf()` invalidates the used ring regions the device
will DMA-write:

```c
cache_invalidate_range(&vq->used->idx, sizeof(vq->used->idx));
/* ... check used->idx ... */
cache_invalidate_range(&vq->used->ring[used_idx], sizeof(vq->used->ring[used_idx]));
```

### Memory attributes

All virtqueue memory comes from `pmm_alloc_pages()` (the buddy
allocator), which returns Normal, Inner-Shareable, cacheable RAM.
RX and TX packet buffers come from static BSS arrays; same attributes.

Neither driver currently allocates DMA buffers from
`kernel/include/ncmem.h` (NC memory) — the existing virtio-net
drivers run on QEMU where every region is coherent regardless.

---

## Open questions

### Q1: Is `dc cvac` + `dsb sy` sufficient for device-visible writes?

On Pi 5 and Jetson, secondary-CPU L1/L2 caches don't participate in
the coherency protocol (no SMPEN). `dc cvac` pushes dirty data from
the issuing CPU's cache to PoC. A DMA-capable device that reads
through PoC sees the up-to-date data.

**Open:** does the device actually read through PoC, or through a
different ordering point (PoU, DSU write buffer, system MMU cache)?
This is platform-specific. The ARM ARM defines the PoC as the point
where memory is "coherent" between all agents (CPUs + devices), but
SoC designers can add caches between the PoC and the device that
aren't visible to `dc cvac`.

For **Pi 5 + GENET**: unknown. BCM2712 has a System MMU and RP1
interconnect between the CPU cluster and GENET. If either has a
non-coherent cache, the driver will need additional measures — most
likely allocating GENET's descriptor rings + buffers from NC memory
(`ncmem_alloc()`).

For **Jetson + EQOS**: Tegra's SMMU is involved. EQOS has its own
DMA engine. Similar uncertainty as Pi 5.

### Q2: Is `dc civac` correct on the RX read side?

`dc civac` cleans (writes back if dirty) *before* invalidating. If
CPU A is reading an RX buffer that CPU B wrote to earlier, and B's
write is still in B's L1, the `dc civac` on A's side writes A's
stale view *down to PoC*, clobbering B's write that was about to
arrive there. See `kernel/CLAUDE.md` §"DC CIVAC writes back dirty
data before invalidating."

For virtio RX, only the device writes to the buffer, not another CPU
— the risk is CPU reading stale data, not CPU trampling CPU. So
`dc civac` is safe. But:

**Open:** if the driver is ever upgraded to IRQ-driven RX with
multi-CPU dispatch (e.g., CPU 0 receives, CPU 1 consumes), the
reader must ensure it doesn't have a stale cacheline for the RX
buffer *at the time of receive*. A single `cache_invalidate_range()`
just before reading is correct; but if the reader had pre-fetched
stale data into L1 before the device DMA'd, the invalidate writes
that stale data back, corrupting the device's write.

Mitigation: always `cache_discard_range()` (DC IVAC — invalidate
without writeback) before handing an RX buffer back to the device.
This is what Linux drivers do.

### Q3: Cacheline granularity

`CACHE_LINE_SIZE = 64` matches both Pi 5 and Jetson targets today.
Future platforms may differ (Apple M-series: 128, some ARMv9:
variable). For each new platform:

- [ ] Verify `CTR_EL0` reports the expected cacheline size
- [ ] If it differs from 64, either use `CACHE_LINE_SIZE` dynamically
  or add a static assert in `cache.h`

### Q4: Should descriptor rings be in NC memory?

`kernel/include/ncmem.h` provides a bump allocator for Normal
Non-Cacheable memory on Pi 5 (`0xFFE00000`, 2 MB) and Jetson
(`0xBDE00000`, 2 MB, before OP-TEE carveout). Scheduler run queues,
task tables, and current-task pointers already live there (see
`kernel/CLAUDE.md` §"Non-Cacheable Shared Memory").

**For descriptor rings** specifically:

- Pros: writes are instantly visible to device, no cache maintenance
  needed, eliminates Q1 uncertainty entirely.
- Cons: every CPU read / write goes to DRAM, which is ~10× slower
  than L1. Matters less for rings (cold path) than for packet buffers
  (hot path).
- NC-memory budget is tight: 2 MB total, most consumed by the
  scheduler. A 128-descriptor ring with 64-byte descriptors is
  8 KB — fine. 16 RX packet buffers × ~1536 bytes is 24 KB — also
  fine. But scaling to higher queue depths or more drivers will
  pressure the budget.

**Proposed split** (to validate against real hardware):
- Descriptor rings (desc table, avail ring, used ring): **NC memory**
- Packet buffers: **cacheable**, with explicit `cache_clean_range()`
  before TX and `cache_discard_range()` before re-posting RX

This keeps hot-path packet access fast and puts the ring metadata
(which the device reads/writes continuously) in the NC region where
there's no maintenance overhead.

### Q5: Write buffers and ordering

`dsb sy` waits for all prior memory accesses to complete at the
System level. In practice, on Cortex-A76/A78AE, this also drains the
store buffer. But on some microarchitectures, there's a write-combining
buffer downstream of `dsb sy` that a subsequent non-cacheable device
access can overtake.

**Open:** does issuing `virtqueue_kick()` (a store to MMIO notify
address) after `dsb sy` guarantee that the preceding ring updates
reach the device before the kick? The ARM ARM says MMIO stores are
Device-nGnRE which has stricter ordering than Normal, so yes — but
worth verifying against Linux's r8169 / bcmgenet drivers for
comparison.

---

## Verification plan (first real-hardware driver PR)

When #202 (Pi 5 GENET) or #25 (Jetson EQOS) lands the first
real-hardware NIC driver, the PR must include:

### Instrumented tests

- [ ] **Sustained TX burst**: 10,000 small UDP packets at wire speed;
  verify zero TX failures, zero TX retransmits from the device side
  (capture via port mirror)
- [ ] **Sustained RX burst**: receive 10,000 incoming packets from
  the lab `iperf` generator; verify `rx_no_buffers == 0` and all
  packets' contents decode correctly at the application layer
- [ ] **Large frame**: MTU-sized (1500 B) packets round-trip correctly
- [ ] **Concurrent TX from multiple CPUs**: stress test that
  CPU 0..3 can all call `net_ping()` / `udp_send()` without corrupting
  the descriptor ring — validates the existing spinlock + cache
  maintenance combo

### Direct coherence probes

- [ ] **Descriptor readback test**: write a known pattern into a
  descriptor from the driver, issue `cache_clean_range()`, trigger
  the device to consume the descriptor, then verify the device saw
  the pattern (via a loopback device or a test packet generator)
- [ ] **RX buffer read test**: device DMA-writes a known pattern
  into an RX buffer; driver reads it after
  `cache_invalidate_range()`; assert the pattern matches

### Pcap evidence

- [ ] Capture a pcap on the other side of the cable during test runs;
  compare against the driver's TX stats. Mismatch means the device
  saw different bytes than the driver thought it sent — almost
  certainly a cache-maintenance bug.

---

## Documentation to update when verified

- `docs/networking.md` — §"Descriptor Ring Cache Maintenance"
  already discusses this at a high level; expand with the real-
  hardware evidence once it's available
- `kernel/CLAUDE.md` — §"Non-Cacheable Shared Memory" add a row
  for NIC descriptor rings if Q4's proposal is confirmed
- Driver file (virtio_net.c, bcm_genet.c, etc.) — comment block
  explaining which allocations are cacheable vs NC and why

---

## References

- ARM Architecture Reference Manual ARMv8, D5.9 "Cache support"
- BCM2712 Peripheral Specification (restricted, Broadcom NDA)
- Tegra X1 TRM Chapter 20 "EQOS" — similar IP block to Orin's
- Linux `drivers/net/ethernet/broadcom/genet/` — reference for how
  an upstream-quality driver handles coherence on BCM GENET
- Linux `drivers/net/ethernet/stmicro/stmmac/` — reference for
  Synopsys DWC-EQOS (Jetson's controller)
- `docs/networking.md` §"Descriptor Ring Cache Maintenance"
- `kernel/CLAUDE.md` §"Cache Maintenance (Pi 5 / No SMPEN)"

---

*Last updated: 16 April 2026. Initial publication as investigation
 hook for #203.*
