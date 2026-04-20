# Jetson Orin Nano PCIe Investigation (#25)

Bare-metal access to the Tegra T234 PCIe root complex C8 from SLM-OS
at EL2, needed to drive the RTL8168 NIC on the Super Developer Kit.

**Status (17 April 2026):** Blocked. Root cause is **clock gating
during kexec**, not CBB firewall as initially assumed. Linux's
`pex2_c8_core` clock (via BPMP) reads `0xFFFFFFFF` from APPL when
disabled, matching exactly what SLM-OS sees post-kexec. User-space
mitigations (refcount bumps, runtime-PM override, `mrq_rate_locked`)
don't survive the kexec transition — something at the BPMP or TF-A
firmware level re-gates the clock regardless of Linux's refcount.
Both plan §4.1 (PCIe RTL8168) and §4.2 (USB CDC-ECM) remain
infeasible. BCT firewall override no longer looks like the fix —
this is a kexec-shutdown-path issue, not a security-policy issue.

**Update (20 April 2026, evening):** Step 2 landed on branch
`jetson-bpmp-ipc`. SLM-OS can now reach BPMP over HSP + IVC + MRQ
directly, resolving #190 (BPMP MRQ rejection was a set of driver
bugs, not firmware policy). `bpmp pcie` from the SLM-OS shell
walks `CLK_ENABLE(PEX2_C8_CORE)` + `RESET_DEASSERT(PEX2_CORE_8)` +
`RESET_DEASSERT(PEX2_CORE_8_APB)`, all returning rc=0. APPL comes
alive: `APPL_CTRL` reads `0x00449000` instead of `0xffffffff` —
the controller wrapper is powered and held in a pre-link-training
state (LTSSM_EN bit 7 = 0). DBI still returns all-ones because the
link hasn't trained yet. That's Step 3 scope: toggle LTSSM_EN plus
whatever UPHY bring-up is required to complete link training.

**Update (20 April 2026):** Gen1 link-speed fallback (Step 1 of the
#25 revived plan) is **ruled out**. The RTL8168 endpoint is a
Gen1-only device, so the link already trains at Gen1 natively —
there is no higher speed to fall back from. An empirical
confirmation via `setpci CAP_EXP+30.w=0001` on the RC still left
APPL reading `0xffffffff` post-kexec. Next action is Step 2: port
the edk2-nvidia BPMP IPC client to SLM-OS so the PCIe RC can be
re-initialised from bare metal. See §"Gen1 link-speed fallback
experiment" below.

---

## Hardware

The Super Developer Kit carrier board wires the RJ45 to a **PCIe
RTL8168** (Realtek 0x10EC:0x8168 rev 0x15), not to an integrated
Tegra MAC. All five integrated Ethernet controllers
(`nveqos@2310000` + 4× `mgbe@6800000..6b00000`) are marked
`status = "disabled"` in the carrier-board device tree. The NIC sits
behind Tegra PCIe root complex C8 (`pcie@140a0000`), with the
bridge at bus 0 dev 0 and the endpoint at bus 1 dev 0.

This contradicts `docs/networking-expansion-plan.md` §4.1, which
targeted an EQOS+RTL8211F MDI path. That plan is accurate for some
Jetson Orin Nano Dev Kit variants but not for the Super Dev Kit in
the lab.

## Tegra PCIe register layout

From DT (`/proc/device-tree/bus@0/pcie@140a0000/reg`) and verified by
reading from Linux via `/dev/mem`:

| Region | CPU physical | Size | Purpose |
|---|---|---|---|
| APPL | `0x140a0000` | 128 KB | Controller wrapper registers (LTSSM_EN, link debug) |
| CFG  | `0x2a000000` | 256 KB | iATU-retargeted config window (bus 1+ goes here) |
| ATU  | `0x2a040000` | 256 KB | iATU + eDMA registers |
| DBI  | `0x2a080000` | 256 KB | DesignWare native regs (bus 0 dev 0 fn 0 config) |
| MEM  | `0x3528000000` | 128 MB | Non-prefetchable MEM BAR window (RTL8168 BAR2/BAR4 land here) |

## Evidence chain

### 1. From Linux (EL1), the RC is alive and the RTL8168 is visible

```
peek 0x140a0004    # APPL_CTRL:  0x009490e0   (LTSSM_EN set — link up)
peek 0x2a080000    # DBI bus0:   0x229c10de   (NVIDIA bridge 10DE:229c)
peek 0x2a000000    # CFG bus1:   0x816810ec   (Realtek 10EC:8168 ✓)
```

Linux's iATU programming is in place at `0x2a000000` and forwards
config reads to bus 1. Endpoint responds normally.

### 2. `pcie_tegra194.ko` has `.shutdown = NULL`

The L4T 5.15 variant of the Tegra PCIe driver does not install a
`.shutdown` callback in its `platform_driver` struct. Verified by
inspecting the module's `.data` relocations:

```
.rela.data at offset 0xf288, 9 entries:
  0x28 → .text+0x23b0   (probe)
  0x30 → .text+0x1ca0   (remove)
  [no reloc at 0x38]     (shutdown — stays NULL)
  0x40 → .text+0x1bb0   (suspend)
```

`platform_drv_shutdown()` in `drivers/base/platform.c` dispatches via
`drv->shutdown(dev)` only when that pointer is non-NULL. With it
NULL, `device_shutdown()` during kexec is a no-op for this driver.
The RC hardware state therefore **survives** the kexec transition.

### 3. From SLM-OS at EL2 post-kexec, every PCIe MMIO read returns 0xFFFFFFFF

```
APPL_CTRL:   0xffffffff  (expected 0x009490e0)
APPL_DEBUG:  0xffffffff
DBI bus0:    vendor=0xffff  device=0xffff  (expected 0x10DE:0x229c)
```

No SError, no RAS report in the serial log — the reads *complete*
with all-ones. This is the same pattern as the GPU read at
`0x17000000`, documented in `CLAUDE.md` and `jetson-nvidia-support.md`
as **CBB firewall** silently rejecting unauthenticated EL2 accesses.

### 4. Elimination

Since (2) shows the RC state is preserved through kexec, and (1)
shows the RC is alive in the moment before kexec, and (3) shows the
RC is unreadable from SLM-OS at EL2 — the remaining explanation is
that the CBB firewall blocks PCIe MMIO for non-secure EL2 accesses
on this board's fusing. The same hardware-enforced restriction that
blocks UARTA and the GPU register bank applies here.

## What this rules out

- **The slmos-kexec `.shutdown`-skip hack** — the shutdown doesn't
  run in the first place; skipping it changes nothing.
- **An SLM-OS r8169 driver** — even if perfectly ported, every MMIO
  read into APPL/DBI/CFG/BAR2 returns all-ones from EL2.
- **The "port tegra194-pcie init to SLM-OS" Option C from the
  previous sync** — the BPMP path is blocked (#190) *and* the MMIO
  path is firewalled. Both layers of the problem need solving.

## What remains on the table

### Path A — Drop SLM-OS to EL1 — **RULED OUT (17 April 2026)**

Ran a one-shot smoke test (`kernel/tests/jetson_el1_smoke.S`, gated
on `JETSON_EL1_SMOKE=ON`) that at boot:

1. Reads APPL[4] + XHCI[0] from EL2+VHE (for reference).
2. Exits VHE (clears HCR_EL2.E2H + TGE).
3. `ERET`s to EL1.
4. Writes 'ABCD' via raw UARTC MMIO (tests write-at-EL1).
5. Reads UARTC LSR (tests read-at-EL1 on an unfirewalled peripheral).
6. Reads APPL[4] and prints as 8 hex digits.

Captured output on jetson-nano-1:

```
EL2:APPL=0xffffffff
EL2:XHCI=0xffffffff
EL2:PRE-ERET
ABCD                     (EL1 writes work)
LSR=0                    (EL1 LSR read works — returns expected value)
P=ffffffff               (EL1 APPL[4] read — still firewalled)
```

Same APPL address from Linux via `/dev/mem` returns `0x009490e0`.
Linux and SLM-OS are both running at EL1 when they make that read
(Linux normally, SLM-OS via the smoke test). Same EL, same address,
different result → the firewall is **not** EL-gated; it's
trust-chain-gated.

The `jetson-nvidia-support.md` doc already phrased this correctly —
"blocks all peripheral access from unsigned/unauthenticated code" —
it was EL2-specific only by coincidence because SLM-OS happens to
run at EL2. Rephrasing the boot model would have no effect.

### Path B — USB CDC-ECM — **RULED OUT (17 April 2026)**

Plan §4.2. Relied on Tegra XHCI at `0x03610000`. Empirically tested
via the `xhcidiag` shell command:

Linux at EL1 (via `/dev/mem`):
```
HCD CAPLENGTH|HCIVER: 0x01200020   (xHCI 1.20, caplen 0x20)
HCD HCSPARAMS1:       0x08000524
FPCI dev/vendor:      0x229810de   (NVIDIA Tegra xHCI)
```

SLM-OS at EL2 post-kexec (same MMIO addresses):
```
HCD CAPLENGTH|HCIVER: 0xffffffff
HCD HCSPARAMS1:       0xffffffff
FPCI dev/vendor:      0xffffffff
```

Every XHCI register returns the CBB all-ones signature, exactly like
PCIe. The firewall rejects DMA-capable peripherals *as a class* at
EL2 — not peripheral-by-peripheral. Path B is off the table for the
same reason as the full r8169 port.

### Path C — OEM BCT firewall override

NVIDIA Forum thread referenced in `jetson-nvidia-support.md`
(`tegra234-mb2-bct-scr-p3701-0000-override.dts`) documents
per-peripheral firewall permission overrides. Adding PCIe C8 to the
allow-list at EL2 would require a custom BCT flash. Out of scope for
a kexec-based workflow.

### Path D — Abandon Jetson NIC; close #25 as infeasible under EL2

Document what was learned, leave the branch's scaffolding + diagnostic
commands as infrastructure for future work if/when CBB access is
widened, and stop.

## Clock teardown investigation (follow-up, 17 April 2026)

Rebuttal of the CBB-firewall diagnosis, prompted by research into
BCT overrides that pointed at non-firewall root causes:

1. **`echo 0 > /sys/kernel/debug/bpmp/debug/clk/pex2_c8_core/state`
   from Linux at EL1** → APPL[0x04] flips from `0x009490e0` to
   `0xffffffff`. The same symptom SLM-OS sees post-kexec.

2. **Refcount can be bumped to 100+** via repeated
   `echo 1 > .../state` writes. Linux respects the user ref and
   keeps the clock enabled in steady state.

3. **Pre-kexec clock ref bump does NOT survive.** Tested:
   - Bump refcount to 108
   - Set PCIe RC `power/control = on` (runtime PM disabled)
   - Set `mrq_rate_locked = 1` on the clock
   - `kexec -e`
   - SLM-OS post-kexec still reads APPL = `0xffffffff`

Something in the kexec shutdown path (BPMP firmware or TF-A SMC
handler) forcibly gates `pex2_c8_core` regardless of the Linux
clock framework's refcount. User-space mitigations don't reach it.

## Kernel-module clock-hold experiment (17 April 2026)

**Result: NEGATIVE.** Added `scripts/slmos-pcie-keepalive/`
containing a minimal Linux kernel module
(`pcie_clk_keepalive.ko`) that does `clk_get("core")` +
`clk_prepare_enable()` on the PCIe C8 platform device at init and
deliberately skips the release on `exit`. Tested on the target
(nvidia-l4t-kernel-headers installed, module built on-board and
insmod'd). Module successfully grabs the clock — `dmesg` shows
"holding core clock at 62500000 Hz".

Post-kexec, SLM-OS's `rtldiag` still shows APPL = `0xffffffff`,
DBI = `0xffffffff`. Combinations also tested, all negative:

- Module + sysfs refcount bump (100+).
- Module + `echo on > power/control` (runtime-PM override).
- Module + `mrq_rate_locked = 1`.
- Module + `echo 1 > powergate/pciex8a/state` (and pciex8b).
- All of the above combined.

**Interpretation:** BPMP firmware, not Linux's clock framework, is
what gates the clock during the kexec transition. No Linux-side
intervention — sysfs, kernel module, runtime PM, or per-domain
powergate override — reaches the point where BPMP makes its
decision.

The module source, Makefile, and a README with the negative result
are kept in `scripts/slmos-pcie-keepalive/` as documented
infrastructure for anyone revisiting this problem.

## Gen1 link-speed fallback experiment (20 April 2026)

**Result: NEGATIVE — and hypothesis was structurally wrong for this
hardware.**

Issue #25's revived plan proposed a cheap-first "Gen1 fallback"
experiment, on the theory (borrowed from NVIDIA's kdump-on-Orin-NX
workaround, forum thread 365485) that forcing PCIe to Gen1
sidesteps BPMP's retrain step and therefore its clock teardown.

Probing the actual link before running the experiment invalidated
the premise: **the RTL8168 endpoint is a Gen1-only device.**

```
# Endpoint (RTL8168, 0008:01:00.0)
LnkCap:   Port #0, Speed 2.5GT/s, Width x1
LnkCap2:  Supported Link Speeds: 2.5GT/s          ← Gen1 only

# RC (0008:00:00.0)
LnkCap:   Port #0, Speed 8GT/s, Width x2          ← Gen3 capable
LnkCtl2:  Target Link Speed: 8GT/s                ← RC default target
LnkSta:   Speed 2.5GT/s (downgraded), Width x1    ← trained to EP's cap
```

The link is already trained at Gen1 naturally because the endpoint
caps it there. Nothing in the kexec path needs to "retrain to Gen1"
because there is no higher speed to come down from. The NVIDIA
workaround targeted Orin-NX boards with Gen2/Gen3 NICs where Gen1
was a fallback speed; on the Super Dev Kit carrier, Gen1 is the
only operating speed.

Ran the experiment anyway for rigor:

1. `setpci -s 0008:00:00.0 CAP_EXP+30.w=0001` — forced RC
   `LnkCtl2.TLS = 2.5GT/s`.
2. `setpci -s 0008:00:00.0 CAP_EXP+10.w=0460` — triggered
   `LnkCtl.RL` (Retrain Link).
3. Confirmed both sides still trained at Gen1; RC `LnkCtl2` now
   reads `Target Link Speed: 2.5GT/s` (was `8GT/s`).
4. `busybox devmem 0x140a0004 32` → `0x009490E0` (APPL live).
5. `slmos-kexec /root/slmos.elf`.
6. `rtldiag` in SLM-OS shell → `APPL_CTRL: 0xffffffff`,
   `DBI bus0: vendor=0xffff device=0xffff` — identical
   post-kexec symptom.

**Interpretation:** forcing the RC's target link speed to Gen1
pre-kexec does not change anything about the post-kexec MMIO
symptom. The BPMP teardown path is indifferent to link speed —
this was always plausible in hindsight (clock gating is per-domain,
not per-trained-speed), and empirically confirmed here.

**Cost:** ~15 minutes including probing, experiment, and writeup.
**Value:** Step 1 of the #25 revived plan is foreclosed. Move
directly to Step 2 (port the edk2-nvidia BPMP IPC client to
SLM-OS) as the actual unblocking work.

## BPMP IPC port (Step 2 landed, 20 April 2026)

**Result: POSITIVE.** Port complete on branch `jetson-bpmp-ipc`.
Four-file driver under `kernel/drivers/bpmp/` (hsp, ivc, mrq, bpmp)
that correctly implements the Tegra234 BPMP IPC protocol.

### Three bugs in the previous driver

Inherited from #190 (`kernel/drivers/bpmp.c`, 455 LOC, deleted in
this branch). Each was load-bearing on its own — no single fix
would have worked.

1. **IVC channel struct layout wrong.** Offset 0x04 is `tx.state`,
   not `r_count`. `rx.count` is at 0x40. The frame's `mrq` word is
   at 0x80, data at 0x88. The old driver treated the whole header
   as packed at offsets 0, 4, 8, 12 — so `bpmp_init`'s counter
   reset was clobbering `State`, which the handshake state machine
   reads. BPMP firmware saw garbage and returned silent failures
   (which on some kexec boots escalated to a TF-A RAS Uncorrectable
   Error, the symptom that made `bpmp_init` look actively dangerous).

2. **Doorbell offset hardcoded.** Old code used `HSP + 0x10000 +
   master * 0x100`. Correct formula (from edk2-nvidia's
   `HspDoorbellInit` and Linux's `tegra_hsp_doorbell_setup`) reads
   `HSP_DIMENSIONING` at `HSP + 0x380` and computes
   `HSP + (1 + num_sm/2 + num_ss + num_as) * 0x10000 + master_idx * 0x100`.
   On Tegra234 the dimensioning register reads `0x0008a228`
   (num_sm=8, num_ss=2, num_as=2), producing a doorbell region at
   `HSP + 0x90000`, not `HSP + 0x10000`. The old code was writing
   `TRIGGER` into a shared-mailbox register — no BPMP notification
   happened, and every MRQ timed out.

3. **No IVC handshake.** The Sync → Ack → Established protocol from
   `linux-tegra-ivc.c` was not implemented. A post-kexec BPMP keeps
   Linux's `tx.count` / `rx.count` values (observed as 70400 on a
   hot kexec; 11729 on a cold boot) and will silently discard frames
   whose counter looks ancient compared to its internal state. The
   new driver drives `Sync → (wait for peer Ack) → Established` via
   `hsp_ring_bpmp()` and waits for the peer's `tx.state` transition
   to Ack before declaring success — catching the initial
   "peer=Established" condition as "BPMP hasn't reacted yet" rather
   than "handshake done".

### End-to-end validation

SLM-OS boot log on jetson-nano-1 (after slmos-kexec from Linux,
no PCIe clock pre-hold in the helper):

```
[INFO] HSP@0x3c00000: DIMENSIONING=0x0008a228 (SM=8 SS=2 AS=2)
[INFO] HSP doorbells: CCPLEX@0x3c90100 BPMP@0x3c90300
[INFO] BPMP/IVC: TX@0x40070000 RX@0x40071000 (pre-handshake)
[INFO] IVC: handshake complete (us=Established, peer observed Ack)
[INFO] BPMP init: rc=0
[INFO] BPMP: MRQ_PING OK (reply=0xbd5b7dde)
```

Then from the shell:

```
slmos> bpmp pcie
  CLK_ENABLE(PEX2_C8_CORE):          rc=0
  RESET_DEASSERT(PEX2_CORE_8):       rc=0
  RESET_DEASSERT(PEX2_CORE_8_APB):   rc=0
  PEX2_C8_CORE post-enable state:    1

slmos> rtldiag
  APPL_CTRL:   0x00449000  (LTSSM_EN=0)
  APPL_DEBUG:  0x00002000
  DBI bus0:    vendor=0xffff  device=0xffff
```

APPL is alive (was `0xffffffff` pre-enable). DBI still reads all-ones
because `LTSSM_EN` hasn't been set — the controller is powered but
not yet training the link. That is Step 3's problem.

### Step 3 partial result (20 April 2026, late)

Wrote `kernel/drivers/pcie/pcie_tegra194.c` + `pcie_tegra194.h`
(~350 LOC). Covers BPMP UPHY/clock/reset, APPL RP programming
(DM_TYPE, SYS_PRE_DET_STATE, ARCACHE, CFG_BASE_ADDR,
CFG_IATU_DMA_BASE_ADDR), P2U PHY init on both lanes of PCIe C8,
CLKREQ override, 100 ms PEX_RST assertion, LTSSM_EN set, and iATU
region-0 CFG1 programming for bus-1 endpoint access.

Added `pcietrain` shell command to drive the full sequence and
report state.

**Hardware result:** partial. LTSSM stalls at `0x03 (POLLING.COMPLIANCE)` —
the RC is sending training ordered-sets but the RTL8168 endpoint
isn't ACKing them. Final state after the sequence on jetson-nano-1:

```
slmos> pcietrain
  pcie host init:       rc=0
  pcie-tegra: P2U lane 0 + lane 1 init OK
  APPL_CTRL:            0x004490e0  (LTSSM_EN=1)
  APPL_DEBUG:           0x00002018  (LTSSM=0x03)
  APPL_PINMUX:          0x00001807  (PEX_RST=1, CLKREQ_OVERRIDE_EN=1)
  APPL_LINK_STATUS:     0x00000002  (RDLH_LINK_UP=0)
  DBI bus0 VID:DID:     0x229c10de  (RC bridge alive)
```

Things verified to work:
- BPMP IPC (MRQ_UPHY, MRQ_CLK, MRQ_RESET)
- UPHY controller power-up (CMD 4) accepted
- Clock and reset deassertion accepted
- P2U lane 0 + lane 1 MMIO writes complete cleanly
- APPL writes stick (including LTSSM_EN at APPL_CTRL bit 7)
- DBI RC bridge identity is read-valid (`0x229c10de`)

Things tried that did NOT fix the `LTSSM=0x03` stall:
- Longer PEX_RST assertion (200 µs → 100 ms)
- Longer L0 timeout (500 ms → 2 s)
- Explicit PEX_RST de-assert in host_init (so `start_link`'s
  clear-then-set generates a real edge)
- CLKREQ override: force CCPLEX to supply RefClk regardless of
  endpoint CLKREQ# (Linux's !supports_clkreq path)

Left for a future Step 3.5 session:
1. **DBI RC setup from `dw_pcie_setup_rc`.** Linux programs
   PCIE_PORT_LINK_CONTROL (link-capable width), GEN2_CTRL,
   LINK_CAPABILITIES, and the PCI Type 1 header before LTSSM_EN.
   Reference: `docs/reference/linux-pcie-designware-host.c`.
   Most plausible missing piece — if the RC's advertised link width
   doesn't match the endpoint's x1, training doesn't complete.
2. **Force endpoint power cycle.** The RTL8168 is soldered on the
   carrier board, always-powered from 3.3 V. PEX_RST brings its
   PCIe side to DETECT but its internal state machine might be
   wedged from Linux's driver. No software-reachable power control
   on this board, but worth exploring whether BPMP can gate +3V3
   via MRQ_POWERGATE.
3. **Signal-integrity / lane ordering.** Multiple P2U lanes
   configured but DT uses two (`p2u_nvhs_0` @ 0x3F40000,
   `p2u_nvhs_1` @ 0x3F50000). The RTL8168 is x1 — might be lane
   selection or polarity issue on the board.
4. **Compare APPL_DEBUG bits to Linux trace.** Bit 13 is set
   (undocumented in tegra194 header). Could indicate PM_LINKST
   or training-specific status.

The `pcie_tegra_*` driver + `pcietrain` shell command stay in
tree. Anyone revisiting this has empirical evidence that:
- The RC starts, reaches POLLING, and sends training sequences.
- The RTL8168 is NOT responding to those sequences.

That's a narrower scoping than "PCIe is gated off" — Step 2 cleared
that question entirely.

### r8169 driver (Step 4, not yet started)

Scaffolding is in `kernel/drivers/eth_rtl8169.c`. Stage 2+ work
blocked on Step 3.5 landing.

## Immediate next steps

BCT firewall override is no longer the clear option — the problem
isn't firewall policy. Kernel-module clock-hold has been tried and
doesn't help. The remaining mitigations are all non-trivial:

- **Kernel module with `clk_force_enable`** — may prevent the
  teardown by using kernel-level clock-flag semantics beyond
  refcount. Needs on-board cross-compilation (kernel headers not
  installed). [RULED OUT — see experiment above. Standard
  `clk_prepare_enable` doesn't help; `clk_force_enable` isn't
  exported in the L4T 5.15 kernel, and even if it were, the
  teardown is in BPMP firmware, not in the Linux clock framework.]
- **Crash-kernel path (`kexec -p` + sysrq-c)** — skips
  `device_shutdown()` entirely. Needs `crashkernel=` on kernel
  cmdline (requires editing `/boot/extlinux.conf` + reboot + SLM-OS
  adaptation for the reserved-memory boot location).
- **Port the full Tegra PCIe RC bring-up sequence** — complex
  because BPMP MRQs don't work from SLM-OS (#190). Would need to
  either fix #190 or replace the MRQ-dependent steps with direct
  MMIO (if those registers aren't CBB-gated at EL2 — UNTESTED).

Alternative: close #25 and document bare-metal networking on the
Jetson Orin Nano Super Developer Kit as infeasible without one of
the above. The `rtldiag` / `xhcidiag` / `JETSON_EL1_SMOKE`
scaffolding stays in the tree as diagnostic infrastructure — anyone
revisiting the problem can re-run the clock-gate correlation test
without re-discovering it.

---

*Investigation: 17 April 2026. Branch `jetson-rtl8169-driver`.
 Cached Linux references: `docs/reference/linux-pcie-tegra194.c`,
 `docs/reference/linux-r8169-main.c`. See commits 856d6d2
 (scaffolding), ec979df (correct addressing + RC-cold evidence),
 1e02b78 (investigation doc + shutdown-is-NULL finding), plus this
 update adding the XHCI test.*
