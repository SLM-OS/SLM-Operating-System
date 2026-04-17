# Jetson UEFI Direct Boot — Probe Result

> Session notes 2026-04-16. D1 (shell from UEFI) not reached, but the
> debug-visibility problem from the first probe is resolved and four
> new root causes identified. The rest of the work is scoped more
> concretely than before. D2 (BR_RETCODE after `nvgpu acr`) remains
> N/A until D1.

---

## What changed vs. the first probe

The first probe (2026-04-16 morning) was blind — direct UARTC MMIO
writes from the EFI-application context faulted, so there was no
way to localize the crashes beyond reading the single ELR value
ArmCpuDxe printed. Since then:

- **UEFI `con_out` is now wired up** as a pre-EBS trace channel
  (`efi_stub.c` / `efi.h`). Markers A/B/C reliably land on the
  USB-C serial line, providing unambiguous pre-`ExitBootServices`
  checkpoints.
- **UEFI's exception vectors survive `ExitBootServices`** on Jetson
  firmware v36.4.7 — confirmed by planting a `brk` in `boot.S` and
  observing ArmCpuDxe's "Synchronous Exception at X" message at the
  brk's PC post-EBS. This provides a usable post-EBS debug channel:
  controlled `brk` instructions that report their own PC.
- The combination of the two turns "silent crash with one stale ELR"
  into systematic bisection.

---

## Findings (in order of discovery)

### 1. Self-relocating trampoline was counterproductive

`boot.S`'s self-reloc trampoline copied the image from UEFI's load
address to the link address (`0x80000000`) and `br`'d to the copy.
This was meant to restore absolute-address correctness in the
absence of a `.reloc` section.

In practice, UEFI does not hand SLM-OS `0x80000000` on Jetson — that
range is occupied by UEFI's own code/data — so the trampoline both
**overwrote UEFI memory** and **branched into a page that isn't
executable from the EFI-application context**. The reported
`Synchronous Exception at 0x0000000080010070` was the instruction
fetch at the trampoline's branch target failing, not the `mov x0,
x20` the disassembly suggested.

**Fix landed in this commit:** remove the trampoline. Stay at
UEFI's load address; the post-return code is all PC-relative so it
runs correctly at whatever address UEFI picked. Wider kernel
correctness still depends on approach B (real `.reloc` / PIC) for
absolute references inside `kernel_main` and beyond.

### 2. UEFI enters SLM-OS at **EL1**, not EL2 as previously documented

The second baseline crash, once the trampoline was gone, was at
offset `0x24` — `mrs x10, hcr_el2`. Reading an EL2 register from
EL1 traps; that matched the observed fault. The prior assumption
baked into `efi_stub.c` and the Jetson block of `boot.S` — "UEFI's
VHE is active (E2H=1, TGE=1)" — was wrong for this firmware.
Kexec-from-Linux enters at EL2 via TF-A; UEFI-direct enters at EL1.

**Fix landed in this commit:** gate the whole Jetson VHE/timer/UART
block on `CurrentEL == 2`. Kexec keeps its existing path; UEFI-
direct skips a block it cannot execute.

### 3. Pending exceptions were hanging UEFI's handler

After EBS and `efi_disable_mmu`, DAIF was still inherited from
UEFI. If any stale IRQ (timer in particular) fires before the
SLM-OS `VBAR_EL1` is installed, UEFI's still-active vectors handle
it with Boot Services torn down and hang silently.

**Fix landed in this commit:** `msr daifset, #0xF` as the first
instruction after `bl efi_stub_entry`. The existing DAIF mask in
`primary_cpu:` is retained for the kexec path; the UEFI-direct
path needs the mask earlier, before the Jetson block even runs.

### 4. PE had no real `.data` section — fixed in this commit

Before: the PE header declared `.text` (CNT_CODE+MEM_READ+MEM_WRITE+
MEM_EXECUTE) covering everything and a zero-sized `.data` placeholder.
Two problems with that layout:

- Modern UEFI enforces W^X: any section marked MEM_EXECUTE is mapped
  RO+X regardless of the MEM_WRITE bit. Writes to BSS therefore took
  a permission fault.
- The `.data` section being zero-sized meant UEFI never read the
  initialized-data file bytes at runtime — UEFI zero-filled the .data
  range instead, silently wiping the initial values of every `.data`
  global. A latent bug that would have broken many subsystems if
  `kernel_main` had been reached. Only the UEFI-direct path was
  affected; QEMU and kexec load the raw ELF and initialize `.data`
  from the ELF PT_LOAD segments, not from the PE header.

Fixed by splitting the section table into:

- `.text`: RX only (`0x60000020`, no MEM_WRITE), `VirtualSize =
  _kernel_code_size`, covers code + rodata only.
- `.data`: RW, NX (`0xc0000040`, no MEM_EXECUTE), `VirtualAddress =
  _kernel_data_va`, `VirtualSize = _kernel_data_virtual_size` (covers
  through `__kernel_end`, so UEFI zero-fills the BSS + stack portion),
  `SizeOfRawData = _kernel_data_size`, `PointerToRawData =
  _kernel_data_file`.

Also fixed `SizeOfCode` (`_kernel_code_size` instead of `_kernel_size`,
which included BSS), added `SizeOfInitializedData` (`_kernel_data_size`
instead of 0), and corrected `SizeOfImage` from `_kernel_size + 0x10000`
to just `_kernel_size` (the `+ 0x10000` was right for the old
single-section layout where `.text` started at VA 0x10000 and covered
everything, but is off by one header under the split).

New linker symbol `_kernel_data_virtual_size` added to
`kernel-jetson.ld` and `kernel.ld` (both used by non-RASPI5 ARM64
builds that include the PE header).

### 5. Remaining downstream blocker — NOT BSS clear as previously hypothesized

The pre-fix session assumed the silent hang after marker C was
BSS-clear faulting. With the `.data` section split in place, BSS is
now mapped RW-NX by UEFI and writes to it don't fault — but the
silent hang still occurs. So BSS clear wasn't actually where the
previous session's execution stopped; the bisection up to that
point was over-confident.

During this session, diagnostic `brk` instructions at
`.Ljetson_post_setup` (the UEFI-direct landing point), at the top of
the Jetson block, and right after BSS clear all failed to fire —
meaning execution doesn't reach any of them. In one test run the
crash PC landed at `image_base + 0x1C` (inside the PE header region,
where the 4-byte word is zero → AArch64 UDF), which suggests either
stack corruption redirecting a `ret`, or the `efi_disable_mmu`
sequence failing subtly and subsequent instruction fetches going
through broken translations. Not yet root-caused.

---

## Current baseline behavior (UEFI-direct, after this commit)

Running `fs4:\EFI\BOOT\SLMOS.efi` from the UEFI Shell still produces:

```
[slmos] A efi_entry
[slmos] B find_fdt done
[slmos] C calling ExitBootServices
<silent hang — no reset, no exception message, no further output>
```

Externally the observable behavior is unchanged from before the `.data`
section fix. Internally two real improvements landed (BSS now RW-NX
instead of RO+X, initialized data actually loaded from file), but
the remaining blocker is upstream of where those improvements kick in.

The Jetson hangs in a state that requires `labctl power_cycle`
to recover; Linux does not come back on its own.

---

## What's left

In rough order of how much each unblocks:

1. **Diagnose why post-EBS execution doesn't reach any of the
   diagnostic brks.** ~3 points to investigate: (a) does
   `efi_disable_mmu` actually complete and disable the MMU on this
   firmware? (b) is the stack SP inherited from UEFI post-EBS
   actually valid, or does something in primary_cpu's stack setup
   land on a bad page? (c) does the `ret` from efi_stub_entry
   actually land on the post-`bl` instruction or somewhere else?
   A useful next probe is a `brk` placed at the *very first*
   instruction after `bl efi_stub_entry` (before the DAIF mask),
   to confirm `ret` landed where expected.
2. **Install the SLM-OS `VBAR_EL1` before `bl efi_stub_entry`.**
   Currently VBAR_EL1 is set deep inside primary_cpu. If anything
   faults between `bl efi_stub_entry` and that point, UEFI's still-
   installed vectors handle it with Boot Services gone and hang
   silently. Installing the SLM-OS vectors first — even with just a
   minimal handler that prints an EL + ESR + ELR tuple to some
   memory scratch region — turns silent hangs into structured
   diagnostic output.
3. **Approach B: real `.reloc` section + PIC early boot.** Without
   this, `kernel_main` and everything downstream still sees
   absolute addresses pointing at the link address
   (`0x80000000`), not the actual load address. Shape of the fix
   is well-understood (Linux `arch/arm64/kernel/efi-header.S` + a
   self-relocation pass in the stub), but the work is ~1–2 weeks.
4. **Decide on EL1 vs EL2.** Even with (1)–(3), the kernel currently
   assumes EL2+VHE on Jetson (e.g. `kexec_boot_jetson.c`, various
   EL2-specific timer/GIC/GPU paths). UEFI-direct puts SLM-OS at
   EL1. Options: (a) accept the Jetson kernel path runs at EL1
   post-UEFI-direct and audit every EL2-only sequence for a
   CurrentEL guard, (b) SMC back to TF-A to request a transition
   to EL2. (a) is more mechanical; (b) is cleaner but needs a
   Jetson-specific SMC handler that may not exist.

Any one of (1)/(2) is a reasonable next checkpoint; together they'd
let the kernel start running absolute-address-broken C code, which
fails loudly where `.reloc`/PIC is needed — a big step toward
scoping (3).

### Recommendation

The one-week budget in the handoff is spent. (1) + (2) together
are a ~2-day follow-up that takes the probe from "silent hang
after C" to "SLM-OS runs its own code until it hits an absolute-
address reference" — a concrete, testable exit point. Beyond
that, approach B is a real project and should be scoped explicitly
before commitment — or punted in favor of Path 3 (preserve nvgpu's
ACR state through kexec).

---

## Changes landed in this commit

- `kernel/arch/arm64/boot.S` — replace the zero-sized `.data`
  section placeholder with a real one. `.text` is now RX only
  (`0x60000020`), covers `_kernel_code_size` (code + rodata); `.data`
  is RW-NX (`0xc0000040`), covers `_kernel_data_virtual_size` in
  memory (= initialized data + BSS + stack) with
  `SizeOfRawData = _kernel_data_size` (initialized portion only —
  UEFI zero-fills the BSS/stack delta). Also fixes `SizeOfCode`,
  `SizeOfInitializedData`, and `SizeOfImage` values in the optional
  header to match the split layout.
- `kernel/kernel-jetson.ld`, `kernel/kernel.ld` — add
  `_kernel_data_virtual_size = __kernel_end - __data_start` so the
  PE `.data` section's `VirtualSize` can extend through the full
  BSS + stack region. Fixes the QEMU linker's `_kernel_code_size`
  formula (was `__data_end - _start - 0x10000`, now
  `__data_start - _start - 0x10000` — matches Jetson and doesn't
  produce a PE section overlap). Both scripts because the PE
  header is assembled for all non-RASPI5 ARM64 builds (QEMU too,
  where the PE header is inert but still has to link).

### Regression defenses landed alongside

Bare-metal boot glue is hard to unit-test at runtime, so the PR
leans on build-time assertions and hardware smoke tests. Full
matrix:

- **Compile-time (every build, every platform):** 13 `static_assert`s
  in `kernel/arch/arm64/efi.h` pin the UEFI protocol struct offsets
  to spec values (ConOut=0x40, BootServices=0x60, ExitBootServices
  =0xE8, OutputString=0x08, …). Introduced in #226, still apply.
- **Link-time Jetson (`kernel-jetson.ld`):** seven `ASSERT`s on PE
  invariants — kernel-fits-in-16MB, `.data` VA and SizeOfRawData
  alignment, `.text` SizeOfRawData + `.data` PointerToRawData
  FileAlignment, SizeOfImage alignment, `.text`/`.data`
  non-overlap, SizeOfImage matches `.data` end VA. Each caught a
  specific scenario of symbol or characteristic drift.
- **Link-time QEMU (`kernel.ld`):** one `ASSERT` (non-overlap).
  The other invariants don't apply under QEMU's 4KB `.data`
  alignment, and the PE header is inert there anyway.
- **Assertion triggerability verified:** the `.text`/`.data`
  non-overlap, SizeOfImage-mismatch, and `.text` SizeOfRawData
  FileAlignment asserts were each exercised by perturbing the
  relevant symbol and observing the link failure with the
  expected message.
- **QEMU ARM64 runtime:** `make test` green.
- **Jetson kexec-from-Linux runtime:** verified post-commit via
  `slmos-kexec` — shell up, 6/6 CPUs, scheduler ticking, work-
  stealing healthy, shell responsive to `cpu` / `help`.
- **Jetson UEFI-direct runtime:** verified end-to-end behavior —
  A/B/C markers still land on serial, then silent hang per the
  documented remaining downstream blocker.
- **Pi 5, x86-64:** untouched by the diff, builds verified green.

All of these together exercise the compile-time, link-time, and
runtime surfaces that these changes touch. Adding a host-side
PE-parsing test is feasible (~100-line Python script) but was not
pursued — the link-time asserts cover the structural invariants,
and the static characteristic bytes (`0x60000020` / `0xc0000040`)
are single-line constants in assembly that appear in every code-
review diff.

---

## Reproducing

```sh
make kernel PLATFORM=JETSON_ORIN_NANO
scp build/kernel/slmos.bin root@192.168.4.93:/boot/efi/EFI/BOOT/SLMOS.efi
ssh root@192.168.4.93 'efibootmgr --bootnext 0007 && reboot'

# Wait ~30s for Jetson UEFI Shell prompt, then over labctl serial:
#   Shell> fs4:\EFI\BOOT\SLMOS.efi
# Expect A/B/C markers then silent hang until power cycle.
# `labctl power_cycle jetson-nano-2` recovers to Linux.
```
