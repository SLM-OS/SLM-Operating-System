# Jetson BPMP IPC Port — Design Plan (#25 Step 2)

Port the edk2-nvidia BPMP IPC client to SLM-OS so bare-metal code
can issue MRQ requests directly to the BPMP R5 firmware. Replaces the
existing broken `kernel/drivers/bpmp.c` (#190) which cannot send a
single successful MRQ post-kexec.

Target usage: Step 3 (replay `MRQ_UPHY` + `MRQ_CLK` + `MRQ_RESET` to
re-light the PCIe C8 root complex so the RTL8168 becomes reachable
from SLM-OS, unblocking #25 the rest of the way).

Secondary benefit: re-opens #190 as tractable — any Tegra234 peripheral
that needs BPMP to gate its clocks (GPU, XHCI, PCIe, UPHY) can reach
through this driver instead of requiring the kexec-helper's pre-hold
workaround.

---

## What's in tree today

`kernel/drivers/bpmp.c` (455 LOC) and `kernel/include/bpmp.h` (117 LOC)
landed as part of #190 and are compiled for Jetson but **not called**
— `main.c:425-437` explicitly skips `bpmp_init()` because invoking it
on top of a Linux-initialized channel triggers a TF-A RAS Uncorrectable
Error.

Empirically broken parts:

| Area | Bug | Evidence |
|---|---|---|
| `struct ivc_channel_header` layout | Places `r_count` at offset 0x04; real layout has `State` there. `ReadCount` is at 0x40. | `edk2-nvidia-bpmpipcprivate.h:17-28` |
| `bpmp_init` counter reset | Writes to what it thinks is `r_count` (offset 0x04) actually clobbers `State` and confuses a coherent BPMP memory region → TF-A RAS. | `bpmp.c:308-315`, `#190` comment on `main.c:431` |
| Doorbell offset | Hardcoded `HSP_DB_BASE_OFFSET = 0x10000`; real layout derived from `HSP_DIMENSIONING` (HSP+0x380) encoding SharedMailboxes/Semaphores count. | `edk2-nvidia-hspdoorbell.c:117-124` |
| IVC handshake | Missing. Code assumes Linux already finished Sync→Ack→Established. Piggy-backing on Linux state is what made `#190`'s rejections appear opaque (`rc=-1` with no visible explanation). | `edk2-nvidia-bpmpipcprivate.h:30-35` defines the states |
| Frame ACK | Missing. edk2-nvidia sets `IVC_FLAGS_DO_ACK` on every TX and increments `ReadCount` after consuming an RX; current code increments counts on both sides without understanding the protocol. | `edk2-nvidia-bpmpipc.c:189-191, 291-293` |
| Cache maintenance | Absent. The TX/RX SRAM is DMA-shared with BPMP's R5. Writes must be flushed (DC CVAC) before doorbell, reads invalidated (DC IVAC) before consuming data. The existing `dsb sy` barrier handles ordering but not coherence. | SLM-OS already uses this pattern for xhci and macb; not applied here. |

Correct pieces worth keeping:

- `HSP_TOP_BASE = 0x03C00000` — matches `hsp@3c00000` phandle 0x121 on jetson-nano-1 (the BPMP-side HSP).
- `BPMP_TX_BASE = 0x40070000`, `BPMP_RX_BASE = 0x40071000` — match `sram@70000` ("cpu-bpmp-rx" from BPMP's perspective) and `sram@71000` ("cpu-bpmp-tx" from BPMP's perspective).
- `HSP_DB_MASTER_BPMP = 3` — physical doorbell block index. Linux's `HSP_DB_MASTER_BPMP = 19` is the master-ID form; edk2-nvidia's `HSP_TARGET_BPMP_ID = 3` is the block index. Both observed on-target.

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  SLM-OS client code (e.g. pcie_tegra_init)                     │
│                                                                │
│  bpmp_clk_enable(TEGRA234_CLK_PEX2_C8_CORE)                    │
│         │                                                      │
├─────────▼──────────────────────────────────────────────────────┤
│  kernel/drivers/bpmp/mrq.c      (MRQ formatter)                │
│                                                                │
│  bpmp_send_mrq(mrq=22, sub=CMD_CLK_ENABLE, arg=clk_id, ...)    │
│         │                                                      │
├─────────▼──────────────────────────────────────────────────────┤
│  kernel/drivers/bpmp/ivc.c      (IVC channel protocol)         │
│                                                                │
│  ivc_write_frame() / ivc_read_frame()                          │
│  Sync → Ack → Established state machine                        │
│         │                                                      │
├─────────▼──────────────────────────────────────────────────────┤
│  kernel/drivers/bpmp/hsp.c      (HSP doorbell)                 │
│                                                                │
│  hsp_ring_doorbell() / hsp_check_pending()                     │
│  Dynamic doorbell offset via HSP_DIMENSIONING                  │
└────────────────────────────────────────────────────────────────┘
              │ MMIO: HSP@0x03C00000 (doorbell)
              │ MMIO: SRAM@0x40070000..0x40072000 (TX + RX)
              ▼
     ┌──────────────────────────────────────┐
     │  BPMP R5 firmware (separate CPU)     │
     │  Survives kexec independent of CCPLEX│
     └──────────────────────────────────────┘
```

The split into three files mirrors edk2-nvidia's layout and isolates
the three layers for unit testability. SLM-OS's current `bpmp.c`
mashes all three into one ~450 LOC blob.

## The IVC channel on the wire

Each direction (TX and RX) is an independent 4 KB region whose
first 136 bytes are a header, followed by per-frame data. Only
one in-flight frame — the channel is *not* a ring buffer in
BPMP-IVC; the `ChannelFree` check is "is `WriteCount - ReadCount`
equal to 1?" (frame in flight) or not (free).

**On-the-wire IVC_CHANNEL layout** (from `edk2-nvidia-bpmpipcprivate.h`):

```
Offset  Size  Field              Notes
0x00    4     WriteCount         Writer-side frame counter
0x04    4     State              IvcStateEstablished=0, Sync=1, Ack=2
0x08    56    write_reserved[14] Pads WriteCount into its own cacheline
0x40    4     ReadCount          Reader-side frame counter (ACK)
0x44    60    read_reserved[15]  Pads ReadCount into its own cacheline
0x80    4     MessageRequest     MRQ command ID (e.g., MRQ_CLK = 22)
0x84    4     Flags              IVC_FLAGS_DO_ACK=1, IVC_FLAGS_RING_DB=2
0x88    120   Data[120]          Variable-length payload (IVC_DATA_SIZE_BYTES)
```

`WriteReserved[14]` and `ReadReserved[15]` aren't padding for
alignment — they're **cacheline isolation** so a writer updating
its counter doesn't dirty the reader's cacheline. This matters on
Tegra where BPMP has its own cache.

## The IVC handshake

BPMP expects a three-state opening sequence before accepting MRQs.
Post-kexec SLM-OS inherits whatever state Linux left the channels
in, which on Orin Nano tends to be `IvcStateEstablished` from
BPMP's perspective but with `WriteCount`/`ReadCount` non-zero
(Linux made many MRQs and never cleaned up).

Two supported strategies:

1. **Resync (preferred for post-kexec).** Write `IvcStateSync` to
   the TX `State` field, ring the doorbell, wait for BPMP to
   mirror back `IvcStateAck` in the RX `State`, write
   `IvcStateEstablished`, ring again. Linux's reference does
   exactly this in `drivers/firmware/tegra/ivc.c`
   `tegra_ivc_notified()`.

2. **Trust Linux's state and just send.** What the current SLM-OS
   driver does. It fails because the counter-reset step also
   clobbers the State field.

Implementation picks strategy 1. Faster to debug: if the handshake
fails, we see it on the `State` register rather than a timeout
deep in the MRQ exchange.

## Doorbell-offset calculation

Currently hardcoded as `HSP_DB_BASE_OFFSET = 0x10000` +
`master * 0x100`. That's the *minimum* layout — correct on a
chip with zero shared mailboxes, zero shared semaphores, zero
arbitrated semaphores. Real Tegra234:

```
DoorbellLocation = HSP_BASE
                 + SIZE_64KB                         // 0x10000 common regs
                 + (SharedMailboxes   << 15)         // 32 KB each
                 + (SharedSemaphores  << 16)         // 64 KB each
                 + (ArbitratedSems    << 16)         // 64 KB each
                 + (3 * DOORBELL_REGION_SIZE)        // target=BPMP
```

The counts come from `MmioRead32(HSP_BASE + HSP_DIMENSIONING)`
where `HSP_DIMENSIONING = 0x380`. A runtime probe will tell us
what Tegra234 actually has. On Orin Nano it's likely:

- `SharedMailboxes = 8` → +0x40000
- `SharedSemaphores = 8` → +0x80000
- `ArbitratedSems = 2` → +0x20000
- Doorbell base at HSP + 0x140000
- BPMP doorbell at base + 3 × PcdDoorbellSize (Tegra234 DB size is
  0x100 per `tegra-hsp.c`) = +0x300
- Final: HSP + 0x140300

The current hardcoded value (HSP + 0x10300) is wrong by 0x130000.
Writing TRIGGER to the wrong address hits a shared-mailbox register,
which may explain the TF-A RAS fallout on top of the struct issue.

**First empirical step of the port: read and log `HSP_DIMENSIONING`
from the live HSP.** No MRQ needed. If the read returns
`0xffffffff` the HSP MMIO isn't mapped correctly. If it returns a
real value the subsequent arithmetic is knowable.

## Per-file plan

Branch: `jetson-bpmp-ipc` (created before first code change).

```
kernel/include/bpmp/
├── bpmp.h          Public API (existing, mostly kept)
├── mrq.h           MRQ opcodes + req/resp structs (imported)
├── ivc.h           IVC channel protocol (new)
└── hsp.h           HSP doorbell API (new)

kernel/drivers/bpmp/
├── hsp.c           ~120 LOC — doorbell trigger/pending/enable
├── ivc.c           ~180 LOC — frame write/read + handshake SM
├── mrq.c           ~150 LOC — MRQ formatter, blocking send
└── bpmp.c          ~80 LOC  — public API glue (init, clk, reset)

docs/reference/     Already cached (9810 LOC Linux + 1691 LOC edk2)
```

Replaces the existing `kernel/drivers/bpmp.c` +
`kernel/include/bpmp.h`. Public API (`bpmp_clk_enable`,
`bpmp_reset_deassert`, etc.) stays source-compatible.

Estimated ~530 LOC SLM-OS code, drawing from:

- ~200 LOC of `edk2-nvidia-bpmpipc.c` (stripped of the UEFI TPL/event
  runtime — SLM-OS is synchronous in this context)
- ~80 LOC of `edk2-nvidia-hspdoorbell.c`
- ~120 LOC of `linux-tegra-ivc.c` for the handshake state machine
- Cross-checked against `linux-bpmp-abi.h` for struct packings

## Boot wiring

`main.c` currently skips `bpmp_init()` on Jetson. After the port:

```c
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* HSP MMIO + BPMP SRAM must be mapped before bpmp_init.
     * Map as Device-nGnRnE (HSP) and Normal Non-cacheable (SRAM). */
    vmm_map_mmio(HSP_TOP_BASE, HSP_TOP_SIZE);
    vmm_map_noncacheable(BPMP_TX_BASE, BPMP_RX_END - BPMP_TX_BASE);

    int bpmp_rc = bpmp_init();
    if (bpmp_rc == 0) {
        bool bpmp_ok = bpmp_is_available();   // MRQ_PING round-trip
        INFO("BPMP: %s", bpmp_ok ? "responding" : "not responding");
    } else {
        INFO("BPMP init rc=%d — will skip", bpmp_rc);
    }
#endif
```

**MMU mapping for SRAM as Normal Non-cacheable**: this is critical.
BPMP's R5 cache and CCPLEX's L2 cache aren't coherent at the cacheline
level for this region; NC bypasses both. Pi 5's `macb.c` uses exactly
this approach for its MACB descriptor rings.

## Testing matrix

Unit-testable in QEMU (no BPMP — stubs):
- `bpmp_init()` returns 0 on platforms without `PLATFORM_JETSON_ORIN_NANO`
- All `bpmp_*` calls return 0 with no side effects

Live on jetson-nano-1 (via `labctl` orchestration, `boot_test --count 10`):
- **Test 1 (HSP layer):** `hspdiag` shell command reads
  `HSP_DIMENSIONING` and reports calculated doorbell address. No
  doorbell actions.
- **Test 2 (IVC handshake):** `bpmp_init()` completes the
  Sync→Ack→Established sequence without a RAS error. Log lines
  show each state transition on both TX and RX channels.
- **Test 3 (MRQ_PING):** `bpmp_is_available()` sends MRQ_PING with
  a non-zero challenge and gets the echo back. This is the
  empirical "BPMP is talking to us" gate.
- **Test 4 (MRQ_CLK query):** `bpmp_clk_is_enabled(id)` on a clock
  known to be enabled at Linux boot (e.g. UART-A). Should return
  `true` without toggling anything.
- **Test 5 (MRQ_CLK enable):** `bpmp_clk_enable(TEGRA234_CLK_PEX2_C8_CORE)`.
  Post-call `rtldiag` should show APPL_CTRL returning a non-
  `0xffffffff` value — the acid test for #25.
- **Reliability:** 10 consecutive clean boots with Tests 1-5 via
  `boot_test --count 10`.

## Explicit non-goals for this PR

- **MRQ_UPHY, MRQ_POWERGATE, MRQ_RESET on PCIe-specific IDs.** Step 3
  of #25 wires these; out of scope for the IPC port PR.
- **Interrupt-driven BPMP (HSP IRQ handler).** This port stays
  polling-only. Polling is fine for the one-shot init-time MRQs the
  port needs to enable; IRQs are a later optimization.
- **Multi-socket / Multi-BPMP.** edk2-nvidia supports multiple BPMP
  instances for THOR/Grace. Tegra234 has exactly one; SLM-OS hardcodes
  it.
- **Re-architect the existing `kernel/drivers/bpmp.c`.** The whole
  file is replaced — simpler than incremental refactoring.
- **Fixing the kexec helper (`scripts/jetson-kexec-slmos.sh`).**
  Once BPMP IPC works, the GPU/USB clock holds become redundant and
  can be removed in a follow-up. Not this PR.

## Success criterion (binds #25 Step 2 → Step 3)

Before this PR merges:

```
SLM-OS> hspdiag
HSP dimensioning: mailboxes=8 shared_sems=8 arb_sems=2
HSP doorbell base: 0x03D40000
BPMP doorbell at: 0x03D40300

SLM-OS> bpmp
[INFO] BPMP: responding (MRQ_PING round-trip OK)

SLM-OS> rtldiag
APPL_CTRL: 0x009490e0  (LTSSM_EN=1)
DBI bus0:  vendor=0x10de device=0x229c
RC alive:  YES
```

(Address `0x03D40000` is a guess pending the live `HSP_DIMENSIONING`
read — will be corrected by Test 1.)

## References cached

`docs/reference/`:
- `linux-bpmp-abi.h` (6750 LOC) — MRQ opcodes, req/resp struct layouts
- `linux-bpmp.c` (941 LOC) — Linux top-level BPMP driver
- `linux-bpmp-tegra186.c` (387 LOC) — HSP-backed mailbox glue
- `linux-tegra-ivc.c` (721 LOC) — IVC state machine reference
- `linux-tegra-hsp.c` (1011 LOC) — HSP mailbox controller
- `edk2-nvidia-bpmpipc.c` (752 LOC) — UEFI port target
- `edk2-nvidia-hspdoorbell.c` (127 LOC) — doorbell specifics
- `edk2-nvidia-*private.h` — struct layouts (the critical bits)

---

*Plan authored 20 April 2026. Follows empirical foreclosure of
Step 1 (Gen1 fallback) — see `docs/jetson-pcie-investigation.md`
§"Gen1 link-speed fallback experiment".*
