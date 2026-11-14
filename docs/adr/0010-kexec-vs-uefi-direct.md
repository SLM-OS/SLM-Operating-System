# ADR 0010: keep two Jetson boot paths, kexec from Linux and UEFI-direct, selected at run time

Status: accepted (backfilled).

## Context

SLM-OS first ran on Jetson Orin Nano by kexec from L4T Linux, which hands over at EL2 (ADR 0009, `d37c1149` 2026-04-05). Linux has by then powered, clocked and configured XUSB, PCIe, the GPU and the camera RCE, and several drivers inherit that state (`kernel/include/boot_source.h`). Kexec also leaves residue that SLM-OS has had to undo: stale GPU DMA (#9), stale interrupts, a PAN/SPAN state that faulted EL0 syscalls (#1079), and a second CPU cluster at 115.2 MHz (#1053) (root `CLAUDE.md`, "Known Issues").

UEFI-direct boot was tracked as #26. April 2026 work (PRs #226, #239, #245) did not reach the shell; PR #1085 traced the remaining "RAS Uncorrectable Error in IOB" blocker to set/way cache cleaning while C code ran off the stack with the MMU off. Why UEFI-direct was pursued, beyond booting without Linux in the chain, is not written down: unknown, ask the user.

## Decision

Both paths boot the same build: kexec loads `slmos.elf`, UEFI loads `slmos.bin` (`docs/deploy/jetson-uefi-direct.md`).

- **kexec:** `scripts/jetson-kexec-slmos.sh`, installed as `slmos-kexec`, holds GPU, XUSB and SMMU state across the handoff, starts the Linux GPU helpers that build channels and write handoff blocks, and pins cpufreq (`docs/deploy/jetson-kexec.md`).
- **UEFI-direct:** `efi_stub_entry` checks the memory map, runs ExitBootServices, cleans by VA, and `boot.S` copies the image to the link address with the MMU off. It does not apply PE relocations (PR #1085).
- The stub records `slmos_boot_source = BOOT_SOURCE_UEFI` in `.data`. Drivers that depend on Linux state call `boot_source_is_uefi()` and either bring their hardware up from cold or refuse.

| Consumer | After kexec | After UEFI-direct |
|---|---|---|
| USB networking (`xhci.c`) | adopts Linux-left controller and slot handoffs | `xusb_t234_cold_bringup()` (#1084, PR #1087) |
| GPU MNIST, sched MLP, eviction Q-net, oplib (`slm_ffi.c ensure_bringup`) | Linux helper channels | `ga10b_cold_run()` from `main.c`, cold pipelines (#1088 PR #1091; #1092 PR #1095) |
| SLM GPU program (`slm_prog.c`), channel recovery (ADR 0003, ADR 0006) | not available; SLM on CPU or per-op hybrid path | cold channel only |
| Camera sensor (`imx219.c`) | works | refused (#1239) |
| EMC floor (`emc_hold`) | not applied | applied (#1161) |
| Model files at `/boot/models` | empty | ADR 0002 |

For x86-64, the bare-metal GRUB/multiboot2 build (`kernel/kernel-x86_64.ld`, `0x100000`) stays first-class. Two kexec variants link at `0x20000000` to stay clear of the running Linux: `make kernel-kexec` (`kernel/kernel-x86_64-kexec.ld`, multiboot2 loader) and `make kernel-bzimage` (`kernel/kernel-x86_64-bzimage.ld`, entry stub at offset `0x200` for kexec-tools' bzImage64 loader). Their purpose is to inherit SEC2 unlocked by nouveau (CPUCTL `0x20`), which bare metal cannot reach because of the priv-lock in #185 (`docs/x86-64-gpu-inference-status.md` §0, §4.2.k).

## Alternatives considered

- **Apply PE base relocations (`.reloc`, the original #26 scope).** Rejected in PR #1085: the kernel would still sit at a physical address its page tables, linker symbols and `smp_boot.S` literals do not expect.
- **Ask BPMP whether a block is powered instead of recording the boot source.** Rejected: `MRQ_PG`/`MRQ_CLK` queries read "off" even for XUSB that Linux left running (`boot_source.h`, measured 2026-11-03).
- **Retire kexec once UEFI-direct worked.** Not done; the reasons for keeping it beyond the PR #1085 note "kexec stays the primary path for that work" are unknown, ask the user.
- **x86-64:** three kexec loaders were tried (multiboot2, elf-x86_64, bzImage); all were silent until `--console-serial` was added (`docs/x86-64-gpu-inference-status.md` §4.2.k). Why both the multiboot2 and bzImage variants are kept: unknown, ask the user.

## Consequences

- Every Jetson driver that touches Linux-initialised hardware needs a `boot_source_is_uefi()` branch; on Tegra234 an access to an unpowered block stops the board in EL3.
- The two paths diverge in features: the SLM GPU program and cold-channel recovery exist only on UEFI-direct; a retest of the kexec SLM path is open as #1169.
- `docs/deploy/jetson-uefi-direct.md` "What works" still says the sched and eviction GPU consumers need kexec helper channels; #1092 moved them to the cold channel, so that row is stale.
- `docs/deploy/jetson-kexec.md` calls kexec the "primary deploy path".

## Evidence

- `eb094b73` PR #1085 (2026-11-03) "Jetson: boot SLM-OS directly from UEFI (#26)": 10/10 UEFI-direct boots on jetson-nano-2; kexec regression checked; `make test-qemu-uefi`, `test_efi_reloc.c`.
- `e7f9aa51` PR #1087, `faa49469` PR #1091, `1e2cf433` PR #1095 (2026-11-03/04).
- Early UEFI work: `26d6f4cd` 2026-04-05 "Add UEFI boot support: self-relocating trampoline, VHE EFI stub"; `8ac05c3d` PR #226, `07a80d93` PR #239, `dfa657c7` PR #245.
- x86-64: `a91bc0e1` PR #264 (2026-04-17) "x86-64 kexec path: SEC2 unlock investigation + three-loader scaffolding + SLM-OS boots via kexec"; `2e06f97d` PR #268 (LAPIC x2APIC inheritance); `peek 0x53840100` read `0x00000020` after kexec (§4.2.k).
