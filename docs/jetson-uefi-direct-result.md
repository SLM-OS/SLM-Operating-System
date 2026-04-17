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

### 5. ArmCpuDxe reports exception PC off by 0x10000 — discovered 2026-04-16 probe session

The 2026-04-16 probe following PR #229 planted `brk` instructions
at two known offsets and measured where ArmCpuDxe's "Synchronous
Exception at X" message reported them:

| brk location                              | reported X           | delta     |
| ----------------------------------------- | -------------------- | --------- |
| `image_base + 0x10000` (real_start entry) | `image_base + 0`     | -0x10000  |
| `image_base + 0x10018` (post-bl)          | `image_base + 0x18`  | -0x10000  |
| `image_base + 0x10100` (0x100 into .text) | `image_base + 0x100` | -0x10000  |

Both off by exactly `SizeOfHeaders` (0x10000). The quirk is
consistent and repeatable. Mechanism hypothesized: ArmCpuDxe
computes display PC against its own notion of `image_base` that
differs from UEFI's LoadedImage.ImageBase by one SectionAlignment.
Not confirmed against source: upstream ArmPkg's
`DefaultExceptionHandler.c` publishes the baseline (tianocore/edk2
is public), and NVIDIA may carry local diffs on top, but neither
has been inspected for this finding.

**Hypothesis confirmed** (2026-04-17 probe added a third data
point): `brk` at `image_base + 0x10100` was reported at
`image_base + 0x100`. The -0x10000 subtraction is constant across
three points in `.text`, ruling out the competing "relative to
BaseOfCode" model.

**Implication for past findings:** the "silent hang after marker C"
in PR #226 and PR #229 may not have been silent at all. The brks
were firing, but the reported PCs pointed to the PE header region
(offset 0–0x10000) which contains mostly zeros (AArch64 UDF),
producing output that looked like "UDF at header" when in fact the
brk fired in `.text` exactly where it was planted.

Re-interpreting past data (all claims pending the third-probe
confirmation above):
- PR #219's "0x80010070" → actual PC ≈ 0x80020070. That's 64KB
  further into the image than the previous interpretation; the
  specific claim that it's "not `.Lreloc_done`" needs a PR #219
  binary disassembly at offset 0x20070 to verify.
- PR #226's diagnostic brk sequence → the brks likely fired as
  expected; the conclusion "execution doesn't reach past X" needs
  re-verification with the corrected offset.
- The BSS-clear hypothesis collapsing (PR #229) and the probe-brk
  hangs this session both look different under this lens.

**Next-session pickup:** re-run the bisection brks from PR #226
with the corrected interpretation. Several "silent hangs" may turn
out to be the brk firing, reported to look like "UDF at header."
The actual hang may be much later in the boot path — possibly
kernel_main itself, which means the bigger remaining work is
approach B (real `.reloc` / PIC), not the intermediate steps
listed below.

### 5a. Secondary finding — DAIF.D masks BRK exceptions

Discovered when diagnostic brks stopped firing midway through
`boot.S`'s post-bl path. Per ARM ARM D1.10.4/5, "BRK instruction
exception" is classified as a debug exception, and `PSTATE.D = 1`
masks all debug exceptions targeted at the current EL. The brk
fires but the exception is deferred indefinitely; the CPU
continues past the brk as if it were a NOP.

This means `msr daifset, #0xF` (which sets D among A, I, F) at
post-`bl efi_stub_entry` silently disabled every subsequent brk in
the boot path. Using `#0x7` (AIF only) plus an explicit
`msr daifclr, #0x8` unmasks D while keeping interrupt masking.

Past `brk` probes placed after the daifset — i.e. every attempt to
bisect inside the Jetson block or primary_cpu in PR #226, #229, or
the first half of this session — silently no-opped for this reason.
This compounds finding #5's effect on past conclusions.

### 5b. Primary finding — UEFI on Jetson runs at EL2, not EL1

PR #226 concluded UEFI hands SLM-OS EL1 based on `mrs hcr_el2`
trapping at offset `0x10024` (reported via ArmCpuDxe). Under
finding #5's corrected -0x10000 offset and finding #5a's DAIF.D
story, that evidence is re-readable two different ways — and when
a fresh probe placed a brk in the EL2 fall-through branch of
`boot.S`'s CurrentEL gate, **the brk fired**. CurrentEL returned
EL2; b.ne was NOT taken; execution fell into the EL2-only code.

So:
- UEFI-direct boot on Jetson firmware v36.4.7 enters at EL2+VHE,
  same as kexec-from-Linux.
- The `CurrentEL == EL2` gate added in PR #226 evaluates TRUE
  under UEFI-direct too, not just under kexec. Both paths enter
  the EL2 block.
- The original PR #226 "mrs hcr_el2 traps" evidence had another
  explanation: the instruction at that offset wasn't `mrs hcr_el2`
  in the old layout once the -0x10000 offset is applied, or the
  apparent trap was a brk firing elsewhere that rendered weirdly.
  Not precisely re-verified, but the EL-2-fall-through brk firing
  is strong evidence the kernel runs at EL2 end-to-end.

### 5c. `msr hcr_el2` re-write hangs silently at EL2 under UEFI

Bisecting inside the EL2 block: a brk placed **after** `msr
hcr_el2, x10` never fires, but a brk placed **before** the same
msr fires reliably. The instruction itself either traps in a
UEFI-specific way or produces a nested exception UEFI's handler
can't cleanly print.

Likely cause: the unconditional write of `(E2H | RW | TGE)` clobbers
every other HCR_EL2 bit UEFI had set — e.g. `API`, `APK`, `AMO`,
`IMO`, `FMO`, `HCD`, `VM` — and UEFI's ongoing reliance on some of
those makes the CPU state inconsistent. Kexec from Linux is fine
because Linux's VHE host state closely matches SLM-OS's target
state; UEFI's state is different.

**Proposed fix (next session):** read-modify-write instead of
unconditional overwrite. Read HCR_EL2, ensure `E2H | RW | TGE` are
set (OR them in), write back. Preserves UEFI's other bits.

### 5d2. Early EL2 vector table (2026-04-17, Path-2 P2)

The primary reason the HCR_EL2 write (and every subsequent
UEFI-direct blocker) is *silent* is that post-EBS, VBAR_EL2 still
points at ArmCpuDxe's handler, which depends on Boot Services state
that EBS just freed. Any synchronous fault bounces to code that
can't coherently report itself.

**Fix (landed):** `efi_stub_entry()` now installs an
SLM-OS-owned VBAR_EL2 table immediately after a successful
`ExitBootServices`. The handler (in `kernel/arch/arm64/boot.S`,
symbol `jetson_early_vbar_el2`):

1. Disables EL2 MMU + caches so subsequent MMIO/DRAM accesses
   don't recurse through the (potentially broken) translation that
   caused the fault.
2. Saves `ESR_EL2`, `ELR_EL2`, `FAR_EL2`, `SPSR_EL2`, `HCR_EL2`,
   and `CurrentEL` to `jetson_early_fault_slot` (BSS, 64 bytes),
   prefixed with magic `"__EL2FAT"` so a memory dump (e.g. via
   kexec-from-Linux recovery or watchdog warm reset) can
   distinguish "handler fired" from "BSS zeroed".
3. Emits `"!FAULT\r\n"` over UARTC (0x0C280000) as a real-time
   visible signal. Single-shot writes, SError masked so a CBB
   firewall block or translation failure on the UARTC write itself
   doesn't recurse.
4. WFE-loops forever.

Gated on `PLATFORM_JETSON_ORIN_NANO` and `CurrentEL == 8` (EL2).
Pre-EBS and kexec-from-Linux paths are unaffected.

**What this enables:**
- §5c's HCR_EL2 hang becomes observable: if the write faults, we
  either see `!FAULT` on UARTC, or we see nothing but can still
  detect handler-ran via post-recovery DRAM dump.
- §5d's UARTC MMIO fault hypothesis can be re-tested: with the
  handler in place, a faulting UARTC store at EL2 now goes to the
  handler instead of hanging silently.
- All future UEFI-direct probes (§5c alternatives a/b/c) have a
  fault-visible baseline.

### 5d. UARTC MMIO at 0x0C280000 still faults at EL2 under UEFI-direct

Skipping the HCR_EL2 write lets execution continue into the timer
disables (which all run — CNT*_CTL writes succeed at EL2) and into
the UARTC probe. The first `str w12, [0x0C280000]` faults at its
exact PC (file offset 0x10044 + image_base), reported via
ArmCpuDxe per the -0x10000 offset.

So at EL2 under UEFI-direct, direct MMIO to UARTC is blocked —
consistent with the earlier observation (PR #226) that UARTC
writes from the EFI-application context fault. The prior
interpretation "UEFI runs SLM-OS at EL1 so CBB firewall blocks
UARTC" was wrong about EL (SLM-OS is at EL2), but the CBB blocking
itself is
real. UEFI's EFI-app context appears to sit behind a different
CBB permission profile than kexec-from-Linux despite both being
at EL2.

### 6. Remaining downstream blocker — NOT BSS clear as previously hypothesized

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
