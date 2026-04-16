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

### 4. BSS clear in `primary_cpu` faults against UEFI's W^X

This is the currently-unfixed blocker. `primary_cpu:`'s zero-fill
loop writes to `__bss_start..__bss_end`, which lives inside the
SLM-OS PE's `.text` section (the only section declared with real
content — the `.data` section in `pe_header.S` is a zero-sized
placeholder). Modern UEFI enforces W^X and maps any page marked
as code as read-only regardless of the PE characteristics word
saying otherwise. Writes to BSS therefore take a permission fault,
which on this firmware manifests as a silent hang (the SLM-OS
`VBAR_EL1` isn't set yet, UEFI's handler runs with Boot Services
gone).

**Not fixed in this commit.** The clean fix is a proper `.data`
section in `pe_header.S`: non-executable, writable, covering the
region from `__data_start` to `__bss_end` (or `__stack_top` if
the stack is in scope too). The linker script already aligns
`.data` to `SectionAlignment = 0x10000` for this purpose, so the
PE-side declaration is the remaining piece.

---

## Current baseline behavior (UEFI-direct, after this commit)

Running `fs4:\EFI\BOOT\SLMOS.efi` from the UEFI Shell produces:

```
[slmos] A efi_entry
[slmos] B find_fdt done
[slmos] C calling ExitBootServices
<silent hang — no reset, no exception message, no further output>
```

A/B/C are the pre-EBS con_out markers. The silence after C is
**BSS clear faulting** (finding #4 above), confirmed by bisecting
`brk` instructions up to the point right before the BSS loop.

The Jetson hangs in a state that requires `labctl power_cycle`
to recover; Linux does not come back on its own.

---

## What's left

In rough order of how much each unblocks:

1. **Add a real `.data` section to `pe_header.S`.** RW, NX, covering
   `__data_start..__bss_end`. This unblocks the BSS clear and lets
   `primary_cpu` complete. ~1 day, including verifying UEFI actually
   honors the characteristics (some firmwares are still conservative
   — a fallback is to mark the region as `EfiLoaderData` via
   `AllocatePages` inside `efi_stub_entry` and copy/zero-init
   ourselves). Low-risk: the kexec path is unaffected.
2. **Install the SLM-OS `VBAR_EL1` before any post-EBS operation
   that might fault.** Move the `vbar_el1` write from `primary_cpu`
   to right after `bl efi_stub_entry`. This turns silent hangs into
   structured faults through the SLM-OS handlers, which can log via
   UARTC (once UARTC is mapped in the SLM-OS page tables — still
   EL2-gated) or via a DRAM scratch region.
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

- `kernel/arch/arm64/efi.h` — adds `efi_simple_text_output_protocol_t`
  and the `efi_char16_t` typedef; tightens `con_out`'s type in
  `efi_system_table_t`. Also adds `static_assert` compile-time checks
  on every relevant field offset (ConOut=0x40, BootServices=0x60,
  ExitBootServices=0xE8, etc.) so a future reorder of the struct
  can't silently mis-index UEFI memory — the build fails loudly
  instead. Nothing here breaks the kexec path.
- `kernel/arch/arm64/efi_stub.c` — adds `efi_print()` helper plus
  trace markers at: entry, after FDT lookup, before EBS, on EBS
  failure, after EBS, before `efi_disable_mmu`, before return. The
  post-EBS markers (D/E/F) are retained even though ConOut is
  torn down by EBS on this firmware — they're harmless no-ops if
  ConOut's vtable is zeroed, and they cost ~40 bytes of .rodata.
- `kernel/arch/arm64/boot.S`:
  - `msr daifset, #0xF` immediately after `bl efi_stub_entry` to
    mask stale UEFI IRQs before UEFI's vectors teardown manifests.
  - Remove the self-relocating trampoline and its `.Llink_address`
    / `.Limage_size` literals — see finding #1.
  - Gate the Jetson EL2 block (HCR_EL2 write, CNT*_CTL writes,
    UARTC `"EL2\r\n"` probe) on `CurrentEL == 2` via a new
    `.Ljetson_post_setup` skip label — see finding #2. Re-writing
    HCR_EL2 at EL2 with the same `(E2H|RW|TGE)` bits a VHE-aware
    Linux kexec left in place is architecturally a no-op store,
    so no guard on the write is needed.

QEMU `make test` is green. Kexec path on Jetson is untouched —
the kexec entry runs at EL2, takes the same Jetson block, and the
new `CurrentEL == 2` gate evaluates true. Any kexec regressions
will surface on the next hardware deploy.

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
