# Boot / Platform Bring-Up — Fact Sheet

How SLM-OS enters execution on each platform and reaches a running shell.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Boot chain | QEMU `-kernel` direct load | Raspberry Pi firmware (`start4.elf`) → `kernel_2712.img` | Linux + kexec `HVC_SOFT_RESTART` (UEFI-direct WIP) | GRUB multiboot2 (ISO or UEFI disk image) |
| Entry exception level | EL1 | EL2 then drop to EL1 | NS EL2 + VHE (inherits from Linux) | CPL=0 long mode |
| Entry address | `0x40000000` | `0x80000` | `0x80000000` (kexec load addr) | multiboot2 header |
| Kernel format | ELF | raw binary (`slmos.bin`) | PE/COFF for UEFI; kernel image for kexec | multiboot2 ELF (ISO) + UEFI disk image |
| Bootloader → kernel hand-off | `-kernel` arg | config.txt `kernel=kernel_2712.img` | `slmos-kexec` script (`/usr/local/bin/`) | `menu.lst` or `efibootmgr` entry |
| Platform descriptor source | DTB from QEMU | DTB from Pi firmware | DTB inherited from Linux | ACPI RSDP + MADT |
| DTB / ACPI parser | `kernel/src/dtb.c` | same | same | `kernel/arch/x86_64/acpi.c` |
| First-stage UART | PL011 @ `0x09000000` | RP1 UART0 @ `0x1F00030000` via bit-bang | UARTC @ `0x0C280000` via TCU | 16550 @ `0x3F8` COM1 |
| Exception vectors | `VBAR_EL1` | `VBAR_EL1` | `VBAR_EL1` (aliased to EL2 under VHE) | IDT |
| MMU / paging enable site | `vmm_init` | `vmm_init` | `vmm_init` | 32-bit trampoline → long mode |
| PMM init | `pmm_init` from DTB | same | `pmm_add_region` × 3 (skips OP-TEE carveout) | `pmm_init` from multiboot2 memory map |
| GIC / IRQ controller init | GICv2 | GICv2 | GICv3 (per-CPU redistributor) | 8259 PIC mask + LAPIC + IOAPIC |
| Boot-time-to-shell | ~200 ms | ~1.5 s (firmware + kernel) | ~45 s (Linux boot + kexec) | ~18 s (bare metal) |
| Safe-to-reboot mechanism | PSCI `SYSTEM_RESET` (QEMU exit) | PSCI reset | PSCI `SYSTEM_OFF` | ACPI shutdown |

## Skipped / Blocked

- **Jetson direct UEFI boot** — WIP. UEFI loads PE and enters at NS EL2 (not EL1 as originally assumed). Two remaining blockers: (a) boot.S's unconditional `msr hcr_el2` clobbers UEFI-set bits; (b) UARTC MMIO faults from EFI-app context even at EL2 (UEFI-app CBB permission profile differs from kexec-from-Linux). Full write-up in `docs/archive/investigations/jetson-uefi-direct-result.md`.
- **Pi 5 `armstub8-2712.bin`** — disabled due to ~60% boot-garble rate. GIC Group 1 configuration moved into `boot.S` from EL2 (incompletely — suspected cause of #99 timer-IRQ delivery failure). Cooperative preemption works around it.
- **Jetson OP-TEE carveout (0xBE000000–0xC2000000)** — cannot touch. PMM allocator uses three non-contiguous regions around it.
- **Jetson BPMP IVC** — unreachable post-kexec, so SLM-OS inherits whatever clock/power state Linux left. No dynamic re-configuration possible.

## See also

- `docs/boot-sequence.md` (narrative)
- `docs/jetson-el2-bringup.md` — EL2 + VHE specifics
- `docs/archive/investigations/jetson-uefi-direct-result.md` — UEFI direct boot status
- `docs/jetson-cbb-report.md` — CBB permissions by entry path

*Last updated: 18 April 2026*
