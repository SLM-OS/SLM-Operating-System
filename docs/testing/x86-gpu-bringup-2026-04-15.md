# x86-64 GSP bringup validation — 2026-04-15

**Board:** test-pc (Gigabyte H610M S2H V2, i7-6700, 16 GB DDR4, RTX 3050 / GA107).
**Image:** bare-metal SLM-OS, branch `worktree-x86-gpu-inference` at commit
`0682e4f`, built with `make x86-disk PLATFORM=X86_64`.
**Deploy:** `labctl sdwire flash --no-reboot test-pc build/kernel/slmos-x86.img`
(26.2 s, 5.1 MB/s). UEFI forced to SDWire via `efibootmgr --bootnext 0001`
plus `labctl power_cycle test-pc`.

---

## Goal

Exercise the full GSP-RM bringup sequence (FWSEC-FRTS → Booter Load →
RISC-V start) on bare-metal. Primary hypothesis: bare-metal avoids
VFIO's mandatory FLR and therefore avoids the BSI DEVINIT re-run that
raises SEC2's PLM (#185), so Booter Load should succeed here even
though it hangs under VFIO.

## Summary

- ✅ **Platform shim wired end-to-end** — BAR0/BAR1, DMA via PMM,
  VBIOS via BAR0 PROM window.
- ✅ **Phase 0 (firmware load)** — all four GSP blobs present
  (gsp 38 MB, bootloader 20 KB, booter_load 60 KB, booter_unload 40 KB).
- ✅ **VBIOS load + parse** — 1 MB via BAR0 PROM window, 19 BIT entries.
- ✅ **Bringup prepare** — FWSEC parts extracted: imem=58112, dmem=2432,
  engine=0x400, ucode=9.
- ✅ **FWSEC-FRTS SUCCESS** — WPR2 populated at 0x1ffffe00 (addr 0x17fe00000,
  size 0x100000). Same milestone VFIO hit (E3.4), but on bare-metal, proving
  the platform shim is correct end-to-end for the GSP Falcon path.
- ❌ **Booter Load FAILED at phase 106** (SEC2 STARTCPU + halt poll).
  SEC2 CPUCTL reads `0xbadf5620` (priv-lock sentinel) from the moment
  UEFI hands off to SLM-OS, before the bringup touches any register.
  Primary hypothesis falsified: UEFI POST's DEVINIT raises SEC2 PLM
  just like VFIO's post-FLR BSI re-run does.

## Raw output

Boot log (elided to the GPU section):

```
[GPU] NVIDIA device at 01:00.0 (device 0x2584)
[GPU] BAR0: 0x53000000 (16 MB MMIO registers)
[GPU] BAR1: 0x40000000 (256 MB VRAM aperture)
[GPU] BOOT_0:  0xb77000a1
[GPU] BOOT_42: 0x177a1000
[GPU] Chip: GA107 (0x177) — Ampere architecture
[GPU] Revision: 10.1
[GPU] PMC_ENABLE: 0x40000000
```

`gpu sec2` immediately after boot (no bringup run yet):

```
slmos> gpu sec2
SEC2 Falcon state (PSEC2_BASE=0x840000):
  CPUCTL        = 0xbadf5620  (priv-lock=yes)
  HWCFG2        = 0x000067f7  (priv-lock=no)
  IRQSTAT       = 0xbadf5620
  MAILBOX0      = 0x00000000  MAILBOX1 = 0x00000000
  OS            = 0x00000000  DEBUGINFO= 0x00000000
  ENGCTL        = 0xbadf5620
  BROM MOD_SEL  = 0xbadf5620
  BROM PARAADDR = 0xbadf5620
GSP Falcon state (PGSP_BASE=0x110000):
  CPUCTL        = 0x00000010  (priv-lock=no)
  HWCFG2        = 0x000047f7  (priv-lock=no)
```

SEC2's CPUCTL, IRQSTAT, ENGCTL, and BROM tier (MOD_SEL, PARAADDR) are
already priv-locked. SEC2 MAILBOX0/1 and OS/DEBUGINFO are accessible
but empty. GSP Falcon is fully accessible (CPUCTL=0x10 = HALTED bit —
ready for software to start it).

`gpu init` end-to-end:

```
slmos> gpu init
[GPU] Starting GSP-RM bringup on GA107...
[GSP] starting Ampere GSP-RM bringup
[GSP] firmware loaded (version 535.113.01):
[GSP]   gsp: 38061600 bytes
[GSP]   bootloader: 20588 bytes
[GSP]   booter_load: 59768 bytes
[GSP]   booter_unload: 39544 bytes
[GSP] phase 1+ not yet implemented — see gsp.h
[GPU] Phase 1: FWSEC-FRTS — preparing bringup...
[VBIOS] read 1048576 bytes via BAR0 PROM window
[VBIOS] parsed 1048576 bytes, 19 BIT entries
[GPU] FWSEC: imem=58112 dmem=2432 engine=0x400 ucode=9
[GPU] WPR2 target: addr=0x17fe00000 size=0x100000
[GPU] sig: fuse[0x8241e0]=0x3 count=4 ver=0xf idx=2
[GPU] FWSEC-FRTS SUCCESS — WPR2: lo=0x1ffffe00 hi=0x00000000
[GPU] Phase 2: Booter Load on SEC2...
[GPU] Booter Load FAILED at phase 106
[GPU]   SEC2 CPUCTL=0xbadf5620 (halted=0)
[GPU]   SEC2 MBOX0=0x02d1d000 MBOX1=0x00000000 OS=0x00000000
[GPU]   SEC2 IRQSTAT=0xbadf5620 DEBUGINFO=0x00000000
[GPU]   SEC2 BROM MOD_SEL=0xbadf5620 PARAADDR=0xbadf5620
Command returned error: -1
```

Observations:

1. FWSEC-FRTS matches the VFIO harness' result exactly: same sig
   selection (fuse 0x8241e0 = 0x3, sig_count=4, ver=0xf, idx=2),
   same WPR2 low word (0x1ffffe00 — top-of-FB sentinel pattern).
2. MAILBOX0 shows `0x02d1d000` after Booter Load attempt — that's the
   WprMeta IOVA (our PMM handed out the buffer at 0x02d1d000). The
   write to MAILBOX0 **did** reach the register. Only the CPUCTL tier
   is locked.
3. Repeating `gpu init` after a failed Booter Load does not change
   SEC2 state — CPUCTL stays `0xbadf5620`. No drift.

## What this validates

- **Bare-metal platform shim is correct** (BAR0/BAR1 MMIO, DMA allocator,
  VBIOS reader, memory barrier). FWSEC-FRTS is the most demanding
  Falcon-level operation we can run without SEC2 access, and it succeeds
  3/3 runs on this board.
- **VBIOS via BAR0 PROM window works** on retail UEFI where the PCI
  Expansion ROM BAR is unassigned. This is the same path openrm and
  the proprietary driver take.
- **Reproducibility**: identical outcome across three `gpu init` runs
  in the same boot session.

## What this invalidates

- The "bare-metal bypass" hypothesis for #185. Prior status doc
  (§4.2 of `x86-64-gpu-inference-status.md`) claimed the FLR-driven
  PLM raise was VFIO-specific. Hardware contradicts this: UEFI POST
  already raises SEC2's PLM. Bare-metal and VFIO stop at the same
  phase on the same board.

## Next steps (ranked)

1. **Option A (Jetson Orin Nano, GA10B)** — still the pragmatic path.
   Jetson has no DEVINIT script (firmware services come from QSPI via
   TF-A/BPMP), so the SEC2 PLM mechanism that affects x86-64 doesn't
   apply. Shim code written for x86-64 ports cleanly — the state
   machine in `kernel/gpu/nvidia/` is platform-agnostic.
2. **Research whether nouveau GA10x has a SEC2 PLM unlock sequence we
   can replay.** The prior investigation said "none exists" — but we
   know nouveau works on GA107 somehow. Re-examine given this new
   constraint (bare-metal sees the same lock, so the bypass must be
   software-visible, not dependent on FLR absence).
3. **Try direct-GSP RISC-V boot without SEC2 Booter.** Since GSP
   Falcon is fully accessible and WPR2 is populated, we might be able
   to write BCR_CTRL + start GSP directly. Requires either
   (a) signing GSP-RM ourselves (not possible) or
   (b) GSP's own HS boot ROM verifying the image without the SEC2
   intermediary step (unclear if GA10x supports this mode).

## Artifacts

- Shell commands added: `gpu init`, `gpu sec2`.
- Platform shim changes: BAR0 PROM window reader, 1 MB VBIOS buffer.
- Status doc update: `docs/x86-64-gpu-inference-status.md` §2.5
  (bare-metal bypass falsified) and §4.2 (Option B now a validation
  path, not a #185 unblock).
