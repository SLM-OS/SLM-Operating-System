# Jetson Orin Nano PCIe Investigation (#25)

Bare-metal access to the Tegra T234 PCIe root complex C8 from SLM-OS
at EL2, needed to drive the RTL8168 NIC on the Super Developer Kit.

**Status (17 April 2026):** Blocked at the CBB-firewall level. The
restriction is not specific to PCIe — USB 3.0 (XHCI) is blocked the
same way, confirming that CBB rejects all DMA-capable peripherals
from EL2 on this board. Both of the plan's Jetson networking paths
(PCIe RTL8168 §4.1 and USB CDC-ECM §4.2) are infeasible under the
current EL2+VHE boot model.

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

### Path A — Drop SLM-OS to EL1

Linux at EL1 reads these registers fine. Running SLM-OS at EL1
instead of EL2+VHE would likely lift the PCIe CBB restriction. But:
- UARTA at EL1 is CBB-blocked on this board (the reason for the
  earlier EL2 move). A different console would be needed (UARTC?
  TCU?).
- Giving up VHE means all the VHE-transparent EL1-register redirect
  tricks that make SLM-OS's existing ARM64 code work have to be
  undone. Large refactor.
- Secondary CPU bring-up at EL1 through PSCI may differ; needs
  re-verification.

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

## Immediate next steps

Two options remain for bare-metal Jetson networking; the
peripheral-level paths are both out:

- **Path A** — drop SLM-OS to EL1 (big refactor, ripples into UART,
  VHE, and secondary CPU bring-up).
- **Path C** — OEM BCT firewall override (custom L4T flash).

Or close #25 and document that bare-metal networking on Jetson Orin
Nano Super Developer Kit is infeasible under the EL2+VHE boot model.
The `rtldiag` / `xhcidiag` shell commands stay in the tree as
diagnostic scaffolding if the CBB situation ever changes.

---

*Investigation: 17 April 2026. Branch `jetson-rtl8169-driver`.
 Cached Linux references: `docs/reference/linux-pcie-tegra194.c`,
 `docs/reference/linux-r8169-main.c`. See commits 856d6d2
 (scaffolding), ec979df (correct addressing + RC-cold evidence),
 1e02b78 (investigation doc + shutdown-is-NULL finding), plus this
 update adding the XHCI test.*
