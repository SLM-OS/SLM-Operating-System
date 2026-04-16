# Jetson UEFI Direct Boot — Probe Result (Approach C)

> Session notes 2026-04-16. Negative-but-informative result from the
> 30-minute probe recommended in `docs/jetson-uefi-direct-handoff.md` §4C.
> D1 (shell from UEFI) NOT reached. D2 (BR_RETCODE.result) N/A.

---

## What was tried

The handoff recommended probing approach (C) — boot UEFI Shell and
`load` the existing `slmos.efi` — before touching code. Executed
against jetson-nano-2 via labctl serial console.

Boot path: `efibootmgr --bootnext 0007` → reboot → UEFI Shell >
`fs4:\EFI\BOOT\SLMOS.efi`. (FS4 is the SD card's ESP on Jetson;
Boot0007 is the UEFI Shell; Boot0009 is the pre-existing "SLM-OS
Direct" entry pointing at the same file.)

---

## Key findings

### 1. UEFI accepts the PE/COFF image

Shell's `load` command reports "not a driver" (correct for an
`EFI_APPLICATION`), so the PE parser accepts it. Launching by typing
the path as a command starts execution.

### 2. Existing deployed binary was stale

The deployed `/boot/efi/EFI/BOOT/SLMOS.efi` from 04/06 had
`SizeOfRawData = 0x10000` (only 64 KB), truncating everything past
the 64 KB header. Fresh `make kernel PLATFORM=JETSON_ORIN_NANO`
builds produce `SizeOfRawData = 0x120000` (1.17 MB), matching the
actual text+rodata. Always redeploy before testing.

### 3. UEFI honors `ImageBase = 0x80000000` — sometimes

On the first fresh-build run the crash PC was `0x80010070`, meaning
UEFI loaded the image at its preferred address. On subsequent
reboots UEFI placed the image at high DRAM (e.g. `0x25DCC0000`).
Load address varies boot-to-boot depending on UEFI's memory map —
confirms the handoff's "unknown" #3.1: UEFI does *not* reliably
honor the preferred address, so approach (A) is a gamble even when
the image is fully intact.

### 4. UARTC MMIO is inaccessible from EFI-application context

Attempting the handoff's "watch for a trace marker on serial"
approach: added a 1-byte `str w12, [0x0C280000]` to `boot.S`,
rebuilt, redeployed. Result: the store **itself faults** — both
with UEFI's MMU active (before `bl efi_stub_entry`) and with MMU
disabled (after `efi_stub_entry` returns via the existing
`efi_disable_mmu` routine). Exception fires at the `str`
instruction.

This matters because the kexec path *does* write to UARTC at EL2
successfully. Something about the UEFI-application context (pages
not mapped to us, or CBB firewall permissions differing, or MMU
disable not actually landing) blocks raw MMIO. **All UART-trace
debugging strategies are blocked until this is resolved.**

### 5. Baseline crash PC is `0x80010070` (`mov x0, x20`)

With fresh build + preferred-address load, the crash report is
`Synchronous Exception at 0x0000000080010070` followed by ArmCpuDxe's
default ASSERT. Offset `0x10070` disassembles to `mov x0, x20` — a
register-to-register move that cannot itself fault. Options:

- UEFI's exception handler reports a stale/offset ELR.
- The fault is on the instruction immediately after
  (`ldr x10, 0x10168`), a PC-relative literal load inside our
  loaded image.
- Some pending trap (e.g. from the prior `bl efi_stub_entry` work,
  or from `br x10` in the relocation trampoline) is delivered at
  the first post-trampoline instruction.

Cannot disambiguate without debug visibility (see #4).

### 6. The handoff's "unknown" #3.2 is confirmed software-only

The fast-test had already shown HWCFG2 bit 13 = 0 in live Linux
with nvgpu; the UEFI probe doesn't move that data point. No
HWCFG2 read attempted here — we never got past the boot stub.

---

## Negative outcomes

- **Approach C (Shell `load`) fails.** The Shell's PE loader is
  not more permissive than the boot manager — UEFI has the same
  loader behind both paths.
- **Approach A (force preferred address) is fragile.** UEFI
  sometimes already has `0x80000000` occupied. Even when it does
  load us there, we crash for unrelated reasons (#5).
- **Debug via UARTC is blocked (#4).** This blocks iterative
  fault-isolation for the remaining crashes; every code change is
  a blind-deploy without visibility into whether it helped.

---

## What's deployable next

The week-long budget is not well matched to what's left. Remaining
work, each of which blocks the one after it:

1. **Get debug visibility under UEFI.** Rewrite `efi_stub.c` to
   print via UEFI's `con_out->output_string()` (SimpleTextOutput
   protocol) before `ExitBootServices`. 1 day. After that we lose
   ConOut and need a working post-EBS channel — which leads to…
2. **Diagnose why UARTC MMIO fails post-`efi_disable_mmu`.** Could
   be: (a) `efi_disable_mmu` not actually disabling MMU under the
   specific UEFI VHE configuration (TGE may not be 1 as efi_stub.c
   comments assume); (b) UARTC at `0x0C280000` physically
   unreachable from the EFI-application context due to a CBB
   firewall permission UEFI carries through; (c) a post-EBS clock
   or power state UEFI doesn't enable that kexec-from-Linux does.
   2–5 days depending on which.
3. **Add a real `.reloc` section** (approach B). Linux's
   `arch/arm64/kernel/efi-header.S` uses a single dummy
   `IMAGE_REL_BASED_ABSOLUTE` entry plus a position-independent
   early path in `head.S`. Retrofitting SLM-OS to be PIC enough
   for that pattern is 1–2 weeks.

Even step 1 + 2 together exceed what remains of a one-week budget.

---

## Recommendation

**Pivot to Path 3** (preserve nvgpu's ACR state through kexec),
per the handoff's §9 bail condition. The probe has extracted its
value — it ruled out approaches A and C, gave concrete blockers for
B, and confirmed the UEFI-direct environment is further from our
kexec-from-Linux assumptions than prior notes implied.

If the team still wants to pursue Path 2, sequence the work as
(1) → (2) → (3) with each step a hard gate; do not start (3)
until (1) and (2) land.

---

## Files left in a changed state

- `/boot/efi/EFI/BOOT/SLMOS.efi` on jetson-nano-2 now contains a
  fresh clean build matching HEAD (not the stale 04/06 binary).
  `Boot0009 "SLM-OS Direct"` still points at it. BootOrder is
  unchanged (NVMe first, SLM-OS last) — default boots Linux.
- Nothing committed in the repo; `boot.S` is pristine against
  HEAD after reverting the debug markers that didn't pan out.

---

## Reproducing the probe

```sh
# 1. Fresh build
make kernel PLATFORM=JETSON_ORIN_NANO

# 2. Deploy
scp build/kernel/slmos.bin root@192.168.4.93:/boot/efi/EFI/BOOT/SLMOS.efi

# 3. Reboot into UEFI Shell
ssh root@192.168.4.93 'efibootmgr --bootnext 0007 && reboot'

# 4. Via labctl serial (after ~30s for the Shell prompt)
#    Shell> fs4:\EFI\BOOT\SLMOS.efi
```

Expect a one-line `Synchronous Exception at 0xXXXXXXXX` where the
high bits reveal where UEFI actually loaded the image this boot.
