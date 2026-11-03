# Booting SLM-OS on Jetson Orin Nano directly from UEFI

SLM-OS can be launched by the Jetson's UEFI firmware as an EFI
application, with no Linux in the boot chain (#26). This guide covers
how the hand-off works, the one-time board setup, the build-deploy-test
loop, and what does and does not work after a UEFI-direct boot.

The kexec-from-Linux path ([`jetson-kexec.md`](jetson-kexec.md)) remains
the primary path for work that needs state Linux leaves behind (USB
networking, GPU compute channels, the camera RCE). Both paths boot the
same `slmos` build: kexec loads `slmos.elf`; UEFI loads `slmos.bin`.

**Status (2026-11-03):** boots to the `slmos>` shell 10/10 on
jetson-nano-2 (UEFI 36.4.7): EL2 with VHE, 6 CPUs via PSCI, hardware
timer preemption on every CPU, serial console input and output.

---

## How the hand-off works

```
UEFI (EL2, E2H=0, MMU on)
  │  loads slmos.bin (PE32+) at an address of its choosing
  ▼
boot.S real_start ── x1 = EFI system table? ──no──► kexec / -kernel path
  │ yes
  ▼
efi_stub_entry()                                  (MMU on, ConOut alive)
  1. find the DTB in the EFI configuration table
  2. GetMemoryMap; check the link-address range   ── unsafe ──► return
     with efi_check_relocation()                     EFI_LOAD_ERROR to
  3. ExitBootServices                                 UEFI (next boot
  4. install the early VBAR_EL2 fault handler         option runs)
  5. record slmos_boot_source = UEFI (.data)
  6. DC CIVAC the loaded image, the destination
     and the DTB to the PoC
  ▼
boot.S .Lefi_continue                              (no stack use)
  7. mask IRQ/FIQ/SError
  8. MMU + caches off in the regime UEFI left active
     (E2H=0 → SCTLR_EL2 / TLBI ALLE2)
  9. copy code + .data to the link address, IC IALLU
 10. br into the copy ──► normal Jetson EL2 setup (VHE on),
                          primary_cpu, kernel_main
```

Design decisions:

- **Relocate to the link address instead of adding a `.reloc` section.**
  The kernel is linked to run with PA == link address (`0x80000000` on
  Jetson); linker symbols, function-pointer tables, `smp_boot.S`
  literals and the identity-mapped page tables all assume it, and the
  kexec path provides exactly that. Applying PE base relocations would
  still leave the kernel at a different physical address than every one
  of those assumptions. Copying the image restores the invariant for the
  whole kernel at once. Jetson UEFI never grants `0x80000000` to the PE
  loader (the image lands near `0x25C870000`), but the range is
  ConventionalMemory in the UEFI map and therefore the OS's after
  `ExitBootServices`.
- **Check before committing.** `efi_check_relocation()`
  (`kernel/arch/arm64/efi_stub.c`) refuses the copy when the destination
  holds anything that must survive `ExitBootServices` (runtime services,
  ACPI, MMIO, reserved), is not fully described by the map, overlaps the
  loaded image, or contains the DTB. It runs before `ExitBootServices`,
  so a refusal returns `EFI_LOAD_ERROR` to the firmware, which moves on to
  the next boot option instead of hanging the board.
- **ImageBase is the link address** (`.quad _start` in the PE header), so
  firmware that can honour it (QEMU's) loads the image in place and no
  copy is needed.
- **Cache maintenance by VA.** Everything used after the MMU goes off is
  cleaned with `DC CIVAC` while UEFI's identity map is still live, and
  the MMU is switched off in assembly that touches no stack. See
  [Findings](#findings) for why the previous set/way approach failed.
- **Boot source.** `slmos_boot_source` (`kernel/include/boot_source.h`)
  tells drivers whether Linux initialised the hardware first. It lives in
  `.data` so the value written by the stub survives the copy and the BSS
  clear.

---

## One-time board setup

The layout below is what jetson-nano-2 runs (set up 2026-11-02). It
keeps SD-card Linux as the default boot and the recovery path, so every
UEFI-direct attempt is one-shot and a power cycle always returns the
board to Linux.

| Medium | Partition | Role |
|---|---|---|
| microSD | `mmcblk0p1` APP (ext4) | JetPack R36.4.7 — default cold boot, deploy host, recovery |
| microSD | `mmcblk0p10` esp | L4TLauncher (stock) |
| NVMe | `nvme0n1p1` APP (ext4, 118.7 GiB) | spare L4T rootfs (bootable through entry `000E` with the stock launcher) |
| NVMe | `nvme0n1p2` SLMOS-ESP (FAT32, 512 MiB) | `\EFI\SLMOS\SLMOS.efi` — the SLM-OS image |

Steps, run from SD-card Linux (the NVMe rootfs must not be mounted):

```bash
# Back up the partition table, then shrink the ext4 rootfs and add an ESP.
sgdisk --backup=/root/nvme0n1-gpt-backup-$(date +%Y%m%d).bin /dev/nvme0n1
e2fsck -f -y /dev/nvme0n1p1
resize2fs /dev/nvme0n1p1 115G
# Recreate p1 with the same start, PARTUUID and name, ending 512 MiB early.
sgdisk -d 1 -n 1:2048:249020415 -t 1:8300 -c 1:APP \
       -u 1:<original PARTUUID> /dev/nvme0n1
sgdisk -n 2:249020416:0 -t 2:EF00 -c 2:SLMOS-ESP /dev/nvme0n1
partprobe /dev/nvme0n1
resize2fs /dev/nvme0n1p1           # grow back to fill p1
mkfs.vfat -F 32 -n SLMOS-ESP /dev/nvme0n1p2

# Boot entry for the image, WITHOUT touching BootOrder (-C).
efibootmgr -C -d /dev/nvme0n1 -p 2 -L "SLM-OS NVMe" -l '\EFI\SLMOS\SLMOS.efi'
```

The sector numbers are for the 119.2 GiB SPCC drive on jetson-nano-2;
take the end of the disk from `sgdisk -p`.

Do **not** put `\EFI\BOOT\BOOTAA64.efi` on the NVMe ESP. The firmware's
own NVMe entry (`0008`) is first in BootOrder and boots that removable-
media path; with the file present every cold boot would start SLM-OS
and the board would no longer fall back to Linux on a power cycle.
Without it, `0008` fails and the firmware continues to the SD card.

---

## Build, deploy, test

```bash
make kernel PLATFORM=JETSON_ORIN_NANO          # produces build/kernel/slmos.bin
scripts/jetson-uefi-deploy.sh <board-ip>       # IP from labctl lab://sbcs
```

`scripts/jetson-uefi-deploy.sh` waits for SD Linux to answer SSH, copies
the image to `\EFI\SLMOS\SLMOS.efi` on the NVMe ESP (checksum-verified),
sets `efibootmgr --bootnext` to the "SLM-OS NVMe" entry and reboots.
Then capture the console with labctl:

```
serial_capture(jetson-nano-2, timeout=120,
               until_pattern="slmos>|Unhandled Exception|Powering off|ABORT")
```

From the SD-Linux reboot to the prompt takes about 40 s. To go again,
`power_cycle` the board (BootNext was consumed, so it returns to SD
Linux) and rerun the script.

The pre-`ExitBootServices` stub prints its decisions on the console:

```
[slmos] A efi_entry
[slmos] CurrentEL=0x0000000000000008
[slmos] HCR_EL2  =0x0000000088000000
[slmos] load     =0x000000025C870000
[slmos] link     =0x0000000080000000
[slmos] size     =0x0000000001950000
[slmos] fdt      =0x00000002679DF000
[slmos] fdt_size =0x00000000000F39DC
[slmos]  mm 07 0x0000000080000000 +0x0003E000 link
[slmos]  mm 01 0x000000025C863000 +0x00001960 load
[slmos]  mm 04 0x0000000266CAC000 +0x00003CA4 fdt
[slmos] reloc: copy to link address after EBS
[slmos] C calling ExitBootServices
EL2
...
[INFO] Boot source: UEFI-direct (EFI stub)
```

`mm` lines are UEFI memory descriptors (type, start, pages) for the
link range, the loaded image and the DTB. A `reloc: ABORT ...` line
names the reason the relocation was refused; the board then continues
to the next boot option.

### Host-side checks

| Check | What it covers |
|---|---|
| `make test` | `kernel/tests/test_efi_reloc.c`: relocation decisions on synthetic memory maps, boot-source default |
| `make test-qemu-uefi` | Boots the QEMU_VIRT `slmos.bin` under QEMU's AArch64 UEFI (AAVMF) twice: in place, and with ImageBase forced to a non-RAM address so the stub must relocate. Needs `qemu-efi-aarch64` and `mtools` |
| `make test-jetson-uefi-layout` | Structural checks on the Jetson ELF: early VBAR table, MMU-off arms, VA cleaning (no set/way), ImageBase == link address |

`scripts/qemu-uefi-boot.sh` is the script behind `test-qemu-uefi`; it
accepts `FORCE_IMAGE_BASE=` and `QEMU_ACPI=` for manual runs.

---

## What works after a UEFI-direct boot

| Area | State |
|---|---|
| Console | UARTC output; input through the TCU HSP mailbox |
| CPUs | 6 online at EL2h through PSCI `CPU_ON`, NDIVs equalised to CPU 0's (1728 MHz) |
| Preemption | Hardware timer ticks on every CPU (patched BL31) |
| Memory | Same PMM regions as kexec; UEFI's only firmware-owned RAM below `0x240000000` is 128 KB of RuntimeServicesData at `0xFFFE0000`, which the PMM range already stops at |
| BPMP | IVC handshake and MRQ calls work |
| GPU | GA10B probe works; no compute (no Linux-created channels to inherit) |
| USB / network | **Not available.** `xhci_init` skips the controller: XUSB is unpowered and its falcon firmware is not loaded without Linux |
| PCIe, camera | Not tested after a UEFI-direct boot; both drivers depend on state Linux sets up (PCIe already reports no backend on Jetson) |

## Findings

These are the causes found while making the path work, recorded so the
next change to it does not rediscover them. They supersede the
conclusions of `docs/archive/investigations/jetson-uefi-direct-result.md`
where the two disagree.

1. **The RAS "ADDR" field moves PA bit 31 to bit 63.** TF-A's
   `RAS Uncorrectable Error in IOB ... ADDR = 0x8000000000157000` was a
   reference to PA `0x80157000`, and `0x8000000000000080` to PA
   `0x80000080`. The archived write-up read the first as a non-DRAM
   address.
2. **The archived "Device-nGnRnE writes to DRAM are rejected" blocker was
   not the cause.** The kernel writes DRAM with the MMU off on both the
   kexec and the UEFI path. The failure came from
   `efi_disable_mmu()`: it cleaned the caches **by set/way**, turned the
   MMU off, and then kept running C code that loaded its saved frame
   from the stack. Set/way maintenance only operates on the executing
   PE's caches and is not a way to reach the Point of Coherency (Arm ARM
   D7.5.9), so the frame read back from DRAM with the caches off can be
   stale. Bisecting with spin loops placed the fault in exactly that
   window: after the MMU-off, before the next UART write in `boot.S`.
   Cleaning by VA and moving the MMU-off into stack-free assembly removed
   the fault (10/10 boots); the stale-frame mechanism is the explanation
   consistent with that, not separately confirmed.
3. **Unpowered peripherals stop the board instead of reading
   `0xFFFFFFFF`.** The first XUSB aperture read after a UEFI-direct boot
   produced an SError that BL31 escalated to "Unhandled Exception in
   EL3". `xhci_init` now checks the boot source before touching the
   hardware.
4. **BPMP state queries do not show what Linux left on.** `MRQ_PG`
   `GET_STATE` for XUSBC and `MRQ_CLK` `IS_ENABLED` for `xusb_core_host`
   both read "off" from SLM-OS after kexec, while the controller was
   running and enumerating the USB network adapter. They report this
   requester's own requests, so they cannot replace the boot-source
   check.
5. **PSCI-booted cores request an out-of-table frequency.** UEFI leaves
   CPU 0 at NDIV 135 (1728 MHz) while cores started with `CPU_ON`
   request NDIV 157 (2009 MHz). The cpufreq sync now moves every core
   to CPU 0's NDIV in either direction.
6. **UEFI 36.4.7 runs at EL2 with `HCR_EL2 = 0x88000000`** (TGE and RW,
   E2H clear), confirming the archived §5b correction.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| L4TLauncher / Linux boots instead of SLM-OS | BootNext not set (deploy script failed), or the image on the ESP is not a PE file |
| `reloc: ABORT ...` then Linux boots | The relocation check refused the move; the line names the reason |
| Console stops after `C calling ExitBootServices` | Post-EBS fault before `primary_cpu`; the early VBAR_EL2 handler prints `!FAULT` with ESR/ELR/FAR over UARTC if it is a synchronous exception |
| `Unhandled Exception in EL3` later in boot | A driver touched an unpowered block; gate it on `boot_source_is_uefi()` |
| Board stuck in SLM-OS on every power cycle | A `BOOTAA64.efi` reached the NVMe ESP, or BootOrder was changed. Pick the SD entry from the UEFI boot manager (F11 at the prompt, via `serial_send keys=["F11"]`) and fix it from Linux |
