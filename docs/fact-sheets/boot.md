# Boot / Platform Bring-Up — Fact Sheet

How SLM-OS enters execution on each platform and reaches a running shell.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Boot chain | QEMU `-kernel` direct load | Raspberry Pi firmware (`start4.elf`) → custom TF-A `bl31.bin` → `kernel_2712.img` | Linux + kexec `HVC_SOFT_RESTART` (UEFI-direct WIP) | GRUB multiboot2 (ISO or UEFI disk image) |
| Custom firmware (built in-tree) | — | `make tfa-pi5` builds patched `bl31.bin` (clears `SCR_EL3.IRQ/FIQ`, writes `GICD_IGROUPR[0]=0xFFFFFFFF`); deployed manually | — | — |
| Entry exception level | EL1 | **NS EL2 + VHE** (`HCR_EL2.{E2H,TGE}=1`) | NS EL2 + VHE (inherits from Linux) | CPL=0 long mode |
| Entry address | `0x40000000` | `0x80000` | `0x80000000` (kexec load addr) | multiboot2 header |
| Kernel format | ELF | raw binary (`slmos.bin`) | PE/COFF for UEFI; kernel image for kexec | multiboot2 ELF (ISO) + UEFI disk image |
| Bootloader → kernel hand-off | `-kernel` arg | config.txt `kernel=kernel_2712.img` | `slmos-kexec` script (`/usr/local/bin/`) | `menu.lst` or `efibootmgr` entry |
| Platform descriptor source | DTB from QEMU | DTB from Pi firmware | DTB inherited from Linux | ACPI RSDP + MADT |
| DTB / ACPI parser | `kernel/src/dtb.c` | same | same | `kernel/arch/x86_64/acpi.c` |
| First-stage UART | PL011 @ `0x09000000` | RP1 UART0 @ `0x1F00030000` via bit-bang | UARTC @ `0x0C280000` via TCU | 16550 @ `0x3F8` COM1 |
| Exception vectors | `VBAR_EL1` | `VBAR_EL2` (under VHE, `_EL1` accesses redirect to `_EL2`) | `VBAR_EL2` (aliased to EL1 under VHE) | IDT |
| MMU / paging enable site | `vmm_init` | `vmm_init` | `vmm_init` | 32-bit trampoline → long mode |
| PMM init | `pmm_init` from DTB | same | `pmm_add_region` × 3 (skips OP-TEE carveout) | `pmm_init` from multiboot2 memory map |
| GIC / IRQ controller init | GICv2 | GICv2 | GICv3 (per-CPU redistributor) | 8259 PIC mask + LAPIC + IOAPIC |
| Boot-time-to-shell | ~200 ms | ~1.5 s (firmware + kernel) | ~45 s (Linux boot + kexec) | ~18 s (bare metal) |
| Safe-to-reboot mechanism | PSCI `SYSTEM_RESET` (QEMU exit) | PSCI reset | PSCI `SYSTEM_OFF` | ACPI shutdown |

## Runtime kernel replacement (Pi 5 only)

A second boot path lives entirely inside SLM-OS: **stage a new
kernel image into `0:/slmstore/staged.img` on the FAT boot
partition, arm one-shot tryboot, and reboot.** The Pi firmware
loads `tryboot.txt` instead of `config.txt` once, the bootloader
flag self-clears, and `kernel promote` (or `kernel rollback`)
makes the swap permanent or undoes it.

| Surface | Where | What it does |
|---|---|---|
| `kernel status` | `kernel/src/kernel_cmd.c` | Reports boot-media availability, staged image presence, tryboot-armed flag, last activate/promote outcomes |
| `kernel stage <vfs-path>` | same | Copies a built image from VFS into `0:/slmstore/staged.img`; drives BCM2712 EMMC2 via `boot_media_acquire/release` keep-alive ref |
| `kernel activate` | same | Arms tryboot via BCM mailbox `RPI_FIRMWARE_SET_REBOOT_FLAGS=1` + `RPI_FIRMWARE_NOTIFY_REBOOT`; expects `tryboot.txt` to exist on boot FAT |
| `kernel promote` | same | After successful tryboot: copies `staged.img` over `kernel_2712.img` and clears the staged slot |
| `kernel rollback` | same | Clears the tryboot flag (`SET_REBOOT_FLAGS=0`) and removes `0:/slmstore/staged.img` + `staged.sha`. Used to abort an arming before reboot, or to clean up after a failed tryboot session. |
| Boot-media gate | `kernel/src/boot_media.c` | Refcounted SDHCI bring-up; gate opens after `vmm_init`, holds for kernel lifetime |
| Tryboot config | `deploy/pi5/tryboot.txt` | Separate file (NOT a `[tryboot]` filter section); `kernel=tryboot.img` |

Cross-platform note: on QEMU, Jetson, and x86-64 the
`kernel` command is registered but every sub-command short-circuits
on `boot_media_acquire()` returning unsupported — the boot-media
backend is Pi-specific because the BCM2712 EMMC2 base and BCM
mailbox tags are not portable.

## Skipped / Blocked

- **Jetson direct UEFI boot** — WIP. UEFI enters at NS EL2; two blockers remain: (a) `boot.S`'s unconditional `msr hcr_el2` clobbers UEFI-set bits, (b) UARTC MMIO faults from EFI-app context even at EL2 (CBB permission profile differs from kexec-from-Linux).
- **Pi 5 vendor `armstub8-2712.bin`** — replaced by custom in-tree TF-A `bl31` (`make tfa-pi5`), required for hardware timer IRQ delivery. Vendor armstub remains the fallback if TF-A is not flashed; cooperative preemption is required in that case.
- **Jetson OP-TEE carveout (0xBE000000–0xC2000000)** — cannot touch. PMM allocator uses three non-contiguous regions around it.
- **Jetson BPMP IVC** — unreachable post-kexec, so SLM-OS inherits whatever clock/power state Linux left. No dynamic re-configuration possible.

## See also

- `docs/boot-sequence.md` (narrative)
- `docs/archive/plans/dynamic-kernel-replace-plan.md` — runtime kernel-replace design, sub-task status
- `docs/archive/test-runs/pi5-tryboot-verification.md` — Pi 5 tryboot one-shot mechanism verification
- `docs/archive/test-runs/pi5-stage-promote-rollback-verification.md` — full round-trip hardware test log
- `docs/archive/test-runs/pi5-sdhci-real-card-verification.md` — BCM2712 EMMC2 SDHCI on real cards
- `docs/archive/investigations/jetson-pcie-regression-check.md` — verifies pcie_core changes are inert on Jetson
- `docs/jetson-cbb-report.md` — CBB permissions by entry path

*Last updated: 8 May 2026*
