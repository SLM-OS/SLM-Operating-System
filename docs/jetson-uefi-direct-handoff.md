# Jetson UEFI Direct Boot — Agent Handoff

> Session handoff written 2026-04-16. Read this before touching code.
> When this doc and the long-form docs disagree, the long-form docs are
> authoritative — file an update here.
>
> **2026-04-17 update (post-P1/P2/P3 merged):** five investigation
> deltas captured in `docs/jetson-uefi-direct-result.md`:
> - §5b: UEFI enters at EL2 (not EL1 as PR #226 thought).
> - §5c: `HCR_EL2 = 0x88000000` (E2H=0, no VHE). P3 fixes the
>   resulting hang via an E2H-aware `efi_disable_mmu` + a RMW
>   of HCR_EL2 in boot.S.
> - §5d2: SLM-OS installs its own VBAR_EL2 post-EBS;
>   `!FAULT\r\n` + register hex dump over UARTC.
> - §5c (hardware verification): post-EBS `efi_print` calls fault
>   on v36.4.7 — were "working by accident" pre-P2 because UEFI
>   silently absorbed the faults. Removed from `efi_stub_entry`.
> - §5c (new blocker): `primary_cpu` BSS clear raises "CBB
>   Interface Error" because MMU-disabled ARM64 forces
>   Device-nGnRnE and Tegra rejects that for DRAM. **Path 2
>   pivot recommended** per #190 plan §5. Fixing this requires
>   setting up SLM-OS page tables pre-BSS-clear — substantial
>   boot.S rework.
>
> The three-approach `.reloc` scoping in §4 below is still
> orthogonal to those findings, but may be moot if Path 3 wins.

---

## 1. Mission

Get SLM-OS booting **directly from UEFI** on jetson-nano-2, bypassing Linux
and kexec entirely, and verify the GSP Falcon stays unlocked so GPU
compute becomes reachable.

This is **Path 2** of issue #190 (GSP Falcon priv-lockdown blocks ACR HS
load). See `docs/capstone-feature-status.md` §"GPU-Based Inference" and
`docs/jetson-capstone-handoff.md` §3d for the full context — the
short version is that our current kexec-from-Linux workflow somehow
asserts `HWCFG2.RISCV_BR_PRIV_LOCKDOWN` (bit 13), preventing our ACR
HS ucode load. A 2026-04-16 fast-test confirmed bit 13 is **0** in
live Linux with nvgpu running, so the lockdown is software-induced
during the kexec transition. UEFI direct should skip whatever causes
it.

### Exit criteria

Two concrete deliverables. You can stop at either.

1. **D1: SLM-OS boots from UEFI on jetson-nano-2 to the shell.**
   Verified by: serial output shows the SLM-OS banner + "slmos>"
   prompt, with no Linux in the boot path. You can set a UEFI boot
   entry or use the UEFI Shell `load` command — whichever works.

2. **D2: After D1, run `nvgpu acr` from the shell. Report whether
   `BR_RETCODE.result == 3 (PASS)` or still `2 (FAIL)`.**
   That single data point settles whether Path 2 actually unlocks
   compute. Don't try to fix anything downstream of ACR — that's the
   next agent's scope.

### Explicit NON-goals

- **Don't implement FECS/GPCCS/PMU/channel/matmul.** Those are the
  *next* agent's scope. Your deliverable ends at "ACR passes BROM or
  doesn't."
- **Don't chase EL2+VHE bringup subtleties.** That's working today via
  kexec. If UEFI-direct boot needs a different EL setup, note it and
  move on to whatever gets the shell up.
- **Don't touch `kernel/gpu/nvidia/`, `kernel/arch/arm64/nvidia_gsp_platform.c`,
  or `kernel/gpu/nvidia/ga10b_bringup.c`.** The ACR loader is already
  implemented and hardware-verified end-to-end (reset, upload, STARTCPU,
  halt-polling, BR_RETCODE decode). When UEFI-boot SLM-OS runs, `nvgpu
  acr` will exercise it untouched. If it fails you'll see the same
  diagnostics as today.

---

## 2. Why this task gets a dedicated agent

This is a substantially different codebase area from the GPU bringup
that just landed. The skills needed:

- PE/COFF file format (UEFI's required image format on ARM64)
- `.reloc` section layout and base-relocation record encoding
- UEFI boot services (ExitBootServices, GetMemoryMap, AllocatePages)
- Early-boot assembly for EL2 takeover from UEFI
- Memory-map handoff (UEFI's identity-mapped world → SLM-OS's page tables)

The prior agent's cached context (nvgpu ACR, Falcon BROM, GPU firmware
layout) is irrelevant here. Ramp up on PE/COFF + UEFI instead.

---

## 3. Starting state

### What already exists

- **`kernel/arch/arm64/efi_stub.c`** (221 lines) — `efi_stub_entry()`
  that finds the DTB, calls ExitBootServices with retry, and disables
  the MMU in a VHE-compatible way (`sctlr_el1` + `tlbi vmalle1`).
  **Keep this; it works.**

- **`kernel/arch/arm64/boot.S`** — EFI detection path:
  - Checks `x1` for the `EFI_SYSTEM_TABLE` signature at entry
  - If set, calls `efi_stub_entry()`
  - Self-relocating trampoline: after ExitBootServices, copies the
    image from UEFI's load address to the link address `0x80000000`
    and jumps to the copy. Skips the copy when already at link address.

- **`cmake/toolchain-aarch64-none-elf.cmake`** and the Jetson linker
  script (`kernel/arch/arm64/kernel-jetson.ld`) — produce an ELF with
  PE-compatible alignment:
  - `.data` section aligned to PE SectionAlignment (64 KB)
  - `__kernel_end` aligned to 64 KB for PE SizeOfImage
  - `.data` padded to PE FileAlignment (512 bytes)
  - Build-time PE alignment assertions in the linker script

- **`kernel/arch/arm64/pe_header.S`** (if it exists; check) — the
  MS-DOS stub + PE/COFF header prepended to the ELF so UEFI accepts it
  as a PE application. Handwritten, not a proper linker-emitted PE.

### What's known broken

Per `docs/jetson-el2-bringup.md` §"Remaining Work" item 5
(2026-04-13):

> Direct UEFI boot — WIP. EFI stub handles ExitBootServices with
> VHE-compatible MMU disable. Self-relocating trampoline in boot.S
> copies image to link address (0x80000000) when UEFI loads
> elsewhere. PE/COFF header with ImageBase=0x80000000 accepted when
> preferred address available. **Blocked when UEFI can't use
> preferred address** — no `.reloc` section for relocation.

In plain terms: the existing PE header claims `ImageBase=0x80000000`.
When UEFI can honor that (address is free), it loads the image there
and `.text` references work because the image is at its link address.
When UEFI can't honor it (e.g., physical RAM at `0x80000000` is
already allocated for some UEFI structure), UEFI will relocate the
image — but our PE has no `.reloc` section for UEFI to consume, so
references to absolute addresses break. The self-relocating
trampoline helps only *after* the image has started executing, and it
doesn't.

### Unknowns

- **Does UEFI on jetson-nano-2 actually honor `ImageBase=0x80000000`
  in practice?** If it always does, your work is mostly "set up a
  UEFI boot entry and see what happens."
- **What's the state of the GSP Falcon after UEFI Boot Services but
  before Linux?** The fast-test was in live Linux. You need to verify
  `HWCFG2` bit 13 is still 0 right after `efi_stub_entry` returns.
- **Does the Jetson's UEFI enforce secure-boot signature checks?**
  Per `docs/jetson-nvidia-support.md`, production silicon has PK/KEK/db
  keys. If secure boot is enforced, unsigned SLM-OS won't run without
  disabling secure boot in BIOS setup.

---

## 4. Three approaches to the `.reloc` blocker

Ranked by effort × probability of success:

### A. Force UEFI to use the preferred address (fastest path)

Tell UEFI to allocate at `0x80000000` exactly via
`AllocatePages(AllocateAddress, …)` as a pre-load step. If UEFI
rejects (address in use), fall through.

- Effort: low — a small C fragment in the EFI stub or a UEFI shell
  script that chains `AllocatePages` then `load`
- Probability: medium — depends on whether UEFI has already allocated
  `0x80000000` for something else

### B. Add a real `.reloc` section to the PE header

UEFI's PE loader walks `.reloc` entries to patch absolute references
in the image when relocating. Generating one requires either:
- Emit it manually in `pe_header.S` — list every absolute reference
  in the image, which is tedious but self-contained
- Use a pre-built tool (`mingw-w64-binutils`'s `objcopy --target
  pei-aarch64`?) to convert the ELF to a proper PE/COFF with real
  `.reloc` generated by the linker
- Build an "ARM64 Linux kernel image" format instead — UEFI handles
  those natively via `EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION` or the
  Linux kernel's own stub discovery (see `arch/arm64/include/asm/efi.h`
  in Linux). The Linux kernel is position-independent in a specific
  way; we'd need to match that.

- Effort: high
- Probability: high if executed correctly

### C. UEFI Shell `load` command (probing approach)

Boot into the UEFI Shell (available on most Jetson UEFI builds via
ESC at boot), then `load fs0:\EFI\SLMOS\slmos.efi`. The shell's PE
loader is sometimes more permissive than the boot manager. Worth
trying early as a 10-minute probe.

- Effort: tiny (just a boot-time command)
- Probability: low-to-medium, but cheap to try

### Recommended order

1. **Probe first (C)** — 30 min. If it works, stop here.
2. **If C fails, try (A)** — 1-2 days.
3. **If A fails, commit to (B)** — 1-2 weeks.

Budget a 1-week checkpoint. If you're not at D1 by then, report back
and we'll pivot to Path 3 (preserve nvgpu's ACR state through kexec).

---

## 5. Hardware + deploy workflow

The Jetson is **jetson-nano-2**. Accessed via `labctl` MCP (or its CLI).

| Access | How |
|--------|-----|
| Serial console | `labctl serial_capture/send` — port alias `jetson-nano-2` |
| Power cycle | `labctl power_cycle jetson-nano-2` |
| SSH (Linux mode) | `ssh root@192.168.4.93`, password `slmos` |
| SD card access | The Jetson has no SDWire; boot is from microSD that's already in the slot |
| BIOS/boot menu | Press ESC during UEFI "firmware version …" banner |

### Current boot flow (from cold boot)

```
BootROM → MB1 → MB2 → UEFI → L4TLauncher menu → [Option 0: primary kernel (Linux)]
                                               → [Option 1: SLM-OS (currently unused)]
```

There's already an "Option 1: SLM-OS" in the L4TLauncher menu. Check
what path it expects — repurposing it for a UEFI-direct SLM-OS ELF/PE
might be the cleanest integration.

### Known flakiness

- Kasa power plug (`power_cycle`) occasionally returns an auth error.
  Retry, or reboot via serial `reboot` command.
- Ethernet interface `enP8p1s0` sometimes needs `dhclient` re-run
  after reboot to pick up its IP.
- SSH connection drops mid-command when the Jetson reboots. Use
  `nohup` + background for kexec-like transitions, but that's not
  relevant for this task.
- Serial port occasionally reports "Port already in use" for a few
  seconds after a disconnect — wait 30s and retry.

### Verification recipe once you have a UEFI boot

```
# On the jetson, in live Linux (before trying UEFI boot):
# 1. Note the current HWCFG2 value as a baseline
ssh root@192.168.4.93 'python3 -c "
import mmap, os
fd = os.open(\"/dev/mem\", os.O_RDONLY)
buf = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x17110000)
print(hex(int.from_bytes(buf[0xf4:0xf8], \"little\")))"'
# Expected today: 0x00018733 (bit 13 = 0)

# 2. After UEFI-direct boot, from the SLM-OS shell:
slmos> gpu read 1100f4
# Expected if Path 2 unlocks: HWCFG2 bit 13 = 0
# Decoded: ((result >> 13) & 1) should be 0

# 3. Then the real test — run the ACR loader:
slmos> nvgpu acr
# Expected on success: br_retcode.result = 3 (PASS)
# Anything else = Path 2 doesn't unlock compute on its own
```

---

## 6. Build system + repo layout

```
kernel/arch/arm64/
  boot.S           EL2 entry, EFI detect, self-reloc trampoline
  efi_stub.c       ExitBootServices, DTB find, VHE MMU disable
  pe_header.S      (if exists) MS-DOS stub + PE/COFF header
  kernel-jetson.ld Jetson linker script with PE alignment
  efi.h            UEFI protocol structs

cmake/toolchain-aarch64-none-elf.cmake   aarch64 toolchain config
CMakeLists.txt                           build entry
Makefile                                 top-level orchestration

docs/
  jetson-el2-bringup.md         current state (authoritative)
  jetson-nvidia-support.md      UEFI secure-boot notes
  jetson-capstone-handoff.md    cross-project handoff, §3d summarizes GPU branch
  capstone-feature-status.md    §"GPU-Based Inference" for the blocker narrative
  jetson-uefi-direct-handoff.md (this file)
```

### Build commands

```bash
# Primary build target for this work
make kernel PLATFORM=JETSON_ORIN_NANO

# QEMU regression sanity (should always pass):
make test

# Existing host test suites (should always pass):
make test-ga10b-bringup test-falcon test-bringup test-nvfw test-rpc

# Jetson UEFI-direct boot-image layout regression test.
# Verifies jetson_early_vbar_el2 alignment, fault-slot location,
# install-site reachability, VBAR_EL1 install preservation. Run
# this after any change to boot.S, efi_stub.c, or kernel-jetson.ld.
make test-jetson-uefi-layout
```

The resulting `build/kernel/slmos.elf` is the PE candidate. Rename to
`.efi`, copy to the ESP, and add a UEFI boot entry pointing to it.

### Don't break

- QEMU `make test` — 100% green today (modulo known flaky scheduler
  tests that pass on retry)
- All 5 host test suites — 133 tests total
- Jetson kexec-from-Linux path — still the fallback until UEFI-direct
  works; don't break `scripts/jetson-kexec-slmos.sh`
- x86-64 (`make kernel PLATFORM=X86_64`) — shares headers, don't
  introduce arm64-only syntax into shared files

---

## 7. What to check in and what to punt

### Do check in

- Any `.reloc` generation machinery
- Any UEFI boot entry setup scripts
- A short `docs/jetson-uefi-direct-result.md` documenting your result
  — especially the `nvgpu acr` BR_RETCODE.result observation
- Test coverage for any new C code you write (follow the pattern in
  `host-tools/gsp-harness/test_ga10b_bringup.c` for mock-vtable tests)
- Update `docs/jetson-el2-bringup.md` §"Remaining Work" item 5 with
  the new state

### Don't check in

- NVIDIA-proprietary firmware blobs (follow the pattern in
  `scripts/tools/fetch-ga10b-firmware.sh` if you need to pull any)
- UEFI secure-boot keys
- PE/COFF binaries over 1 MB — use `.incbin` + scripts instead

### Ask before pushing

- Anything that modifies `/boot` on jetson-nano-2 persistently — the
  capstone demo needs a working Linux fallback. A persistent change
  that bricks the UEFI boot menu is a hard block to un-brick remotely.

---

## 8. Commit + PR hygiene

- Branch off `main` — don't use the existing `worktree-jetson-gpu-inference`
  branch (its purpose is done).
- Commit messages follow the repo style: descriptive, multi-paragraph
  bodies, `Co-Authored-By: Claude ...` trailer.
- Ask before merging to main. No PRs expected for this work.
- Ask before pushing anything that modifies the Jetson's persistent
  storage (`/boot`, EFI variables, etc.).

---

## 9. If you get stuck

Report back with:
1. **What you tried.** Specific UEFI behavior observed (error messages,
   hangs at specific points).
2. **What the serial log says** at the point of failure.
3. **What `HWCFG2` reads as** if you managed to get any code running
   under UEFI — this alone is gold data for the next agent.

Explicit bail conditions:
- 1 week in, no shell-from-UEFI: report back. Probably pivot to Path 3.
- Secure boot blocks execution and can't be disabled: report back with
  the exact error; we'll discuss whether to pursue a signing path.
- Some UEFI subtlety requires sign-off from someone who owns the
  Jetson lab: ask.

Good luck. The fact that the priv-lockdown is software-induced and
not fuse-locked means there's a real chance this unlocks GPU compute
for the capstone. A negative result is also fine — at that point we
pivot to Path 3 with the diagnosis locked down.
