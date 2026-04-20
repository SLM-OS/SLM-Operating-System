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
