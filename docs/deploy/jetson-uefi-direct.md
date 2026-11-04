# Booting SLM-OS on Jetson Orin Nano directly from UEFI

SLM-OS can be launched by the Jetson's UEFI firmware as an EFI
application, with no Linux in the boot chain (#26). This guide covers
how the hand-off works, the one-time board setup, the build-deploy-test
loop, and what does and does not work after a UEFI-direct boot.

The kexec-from-Linux path ([`jetson-kexec.md`](jetson-kexec.md)) remains
available. Both paths boot the same `slmos` build: kexec loads
`slmos.elf`; UEFI loads `slmos.bin`.

**Status (2026-11-03):** boots to the `slmos>` shell 10/10 on
jetson-nano-2 (UEFI 36.4.7), and 10/10 on jetson-nano-1 with the
pre-#1088 build. On jetson-nano-1 the cold GPU bring-up and
`nvgpu cold check` also pass (scheduler and eviction, single boot, AI_SCHED=ON
build). Both boards: EL2 with VHE, 6 CPUs via PSCI, hardware
timer preemption on every CPU, serial console input and output, and
USB networking (#1084): SLM-OS powers XUSB up itself, enumerates the
onboard hub and the RTL8153 behind it, and gets a DHCP lease; 10/10
boots answered ping and the telnet shell. GPU compute works too
(#1088): SLM-OS brings GA10B up from cold during boot and runs the MNIST
pipeline on a channel it creates itself (see
[GA10B cold bring-up](#ga10b-cold-bring-up)); 10/10 consecutive
UEFI-direct boots completed the bring-up and returned the expected
MNIST result on the GPU, and `slm.model_infer_file` classifies the ten
MNIST test digits correctly on the GPU. The scheduler MLP, the eviction
Q-net and the operator library (SLM per-op kernels) run on the same
channel (#1092); none of the Linux GPU helpers is needed.

**Known issue:** sustained all-core CPU load (for example
`bench eviction-e2e`) silently stops the board after a UEFI-direct
boot, also on builds without #1092; see #1093.

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

### Provisioning a board from scratch

jetson-nano-1 was rebuilt into the same layout on 2026-11-03 (#1043),
starting from a USB Force Recovery flash. The steps, in order:

1. **Flash QSPI + microSD from the host.** Close the recovery strap
   (labctl `enter_recovery`), check that exactly one `0955:7523 APX`
   device is on the host's USB, then from an R36.4.4 BSP whose
   `bootloader/tos-optee_t234.img` is the patched TOS
   (`tools/tfa-patches/README.md`):
   `sudo ./flash.sh jetson-orin-nano-devkit-super mmcblk0p1` (about
   20 min). Root SSH keys can be baked in beforehand at
   `rootfs/root/.ssh/authorized_keys`.
2. **Upgrade to R36.4.7 with apt.** R36.4.7 is not on NVIDIA's download
   archive (the BSP archive goes from 36.4.4 to 36.5.0); it ships only
   as the JetPack 6.2 OTA in the `r36.4` apt channel.
   `apt-get dist-upgrade` installs it and the
   `nvidia-l4t-bootloader` postinst tries to stage the bootloader
   capsule. On nano-1 that step reported "Trigger Capsule update is
   done" but staged nothing (it runs `fwupdtool install-blob` with its
   output discarded, while fwupd itself is being upgraded), so the
   first reboot stayed on UEFI 36.4.4. Staging the capsule by hand
   works:
   ```bash
   mkdir -p /boot/efi/EFI/UpdateCapsule
   cp /opt/ota_package/t23x/TEGRA_BL_3767_super.Cap /boot/efi/EFI/UpdateCapsule/
   printf '\x07\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00' > /tmp/osind.bin
   dd if=/tmp/osind.bin of=/sys/firmware/efi/efivars/OsIndications-8be4df61-93ca-11d2-aa0d-00e098032b8c bs=12
   systemctl reboot
   ```
   A capsule updates the inactive bootloader slot and switches to it,
   so do it twice to put 36.4.7 on both slots (`nvbootctrl
   dump-slots-info`, `/sys/class/dmi/id/bios_version`).
3. **Put the patched BL31 back.** The capsule installs NVIDIA's stock
   TOS. Re-enter recovery and run
   `sudo ./flash.sh -k A_secure-os jetson-orin-nano-devkit-super mmcblk0p1`,
   then the same with `B_secure-os`. The RCM cold boot at the end of a
   flash does not re-sample the strap, so power-cycle the board (with
   the strap still closed) between the two. Verify with a
   `COOP_PREEMPT=OFF` build (the default): `timdiag`'s
   `timer_handler_count` must advance.
4. **Install the SLM-OS extras** on the microSD install
   ([`jetson-kexec.md`](jetson-kexec.md#slm-os-extras-on-the-development-install),
   [`jetson-helper-setup.md`](jetson-helper-setup.md),
   `tools/nvgpu-patches/README.md`) and check a kexec boot.
5. **Make the NVMe.** Partition it as `APP` (ext4) plus a 512 MiB
   `SLMOS-ESP` (`sgdisk -o -n 1:2048:-512M -c 1:APP -n 2:0:0 -t 2:EF00
   -c 2:SLMOS-ESP`), copy the running rootfs
   (`rsync -aAXH --one-file-system`), then in the copy change every
   `root=/dev/mmcblk0p1` in `/boot/extlinux/extlinux.conf` to
   `root=/dev/nvme0n1p1` and mark the `/boot/efi` line in `/etc/fstab`
   `nofail`.
6. **Prove the ESP and entry before staging SLM-OS.** Copy
   `/boot/efi/EFI/BOOT/BOOTAA64.efi` (L4TLauncher) to
   `\EFI\SLMOS\SLMOS.efi`, create the entry with `efibootmgr -C`, boot
   it once with `efibootmgr -n`, and check that `findmnt -n -o SOURCE /`
   says `/dev/nvme0n1p1`. If the copy still said `mmcblk0p1`, the
   launcher would boot the microSD rootfs and the check would pass
   without proving anything. A power cycle must then return to the
   microSD. Also leave a copy at `\EFI\L4T\l4t.efi` on the same ESP:
   it is what the UEFI Shell recovery path in
   [Troubleshooting](#troubleshooting) runs.
7. Deploy SLM-OS with `scripts/jetson-uefi-deploy.sh` as below.

After a Linux shutdown, jetson-nano-1 needs its outlet off for at least
14 s before it powers back on. With anything shorter it stays off: no
APX device and a silent console, which on the way into recovery looks
like a strap problem. This was measured on 2026-11-03 in 1 s steps from
5 s. Disconnecting the USB-C cable made no difference, so the port is
not back-feeding the board. The board's labctl cycle delay is set to
15 s, so `power_cycle`, `enter_recovery` and `exit_recovery` wait that
long by default.

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
| USB / network | Works (#1084). `xhci_init` brings XUSB up from cold (see [XUSB cold bring-up](#xusb-cold-bring-up)); the hub on usb2-1 and the RTL8153 behind it enumerate, CDC-ECM binds; DHCP, ping and the telnet shell work. USB2 only: the SuperSpeed lanes are not used |
| Camera RCE | Works: `rcediag` completes HELLO, PING and `CH_SETUP`. MB2 loads the RCE firmware before UEFI, and `camrtc_init` powers RCE through BPMP the same way it does after kexec |
| GPU | Works (#1088). Brought up from cold during boot (see [GA10B cold bring-up](#ga10b-cold-bring-up)); the MNIST GPU pipeline (`slm.gpu_run_mnist`, `slm.model_infer_file` with `gpu use inference on`) runs on an SLM-OS-created channel. The sched and eviction GPU consumers still need the kexec path's helper channels |
| PCIe | No backend on Jetson on either path (`pcie: no backend available`); nothing to gate |

### XUSB cold bring-up

`kernel/drivers/usb/xhci/xusb_t234_cold.c` repeats what NVIDIA's own
UEFI drivers do when they start the controller
(`XhciControllerDxe`, `UsbPadCtlTegra234.c` in edk2-nvidia; cached at
`~/slmos-ref/tegra-l4t/edk2-nvidia-usb/`). UEFI's ExitBootServices
handler halts the controller, powers the pads down and power-gates
XUSBA/XUSBC, so all of it has to be redone:

1. BPMP: deassert `XUSB_PADCTL` reset; enable the fuse, `pll_e`,
   `utmip_pll`, `usb2_trk` and XUSB core/falcon/fs/ss clocks.
2. BPMP: power XUSBA (SuperSpeed partition) and XUSBC (host).
3. Pads: read `FUSE_USB_CALIB_0` / `_EXT_0`, power the bias pad and run
   tracking, program the USB2 pads of the host ports (usb2-1 hub,
   usb2-2 M.2), override VBUS/ID to host. The USB-C port (usb2-0) is a
   device-mode port and is left alone.
4. FPCI: BAR0/BAR2 decode, `AXI_CFG = 5`, memory space + bus master.
5. Wait for `USBSTS.CNR` to clear.

No falcon firmware is loaded: Tegra234 boots the falcon from IFR
firmware (MB2 logs `xusb: Authentication Finalize Done` / `Binary xusb
loaded`), which is why Linux's `tegra234_soc` has no `.firmware`. The
pad values the helpers produce match the registers Linux programs on
jetson-nano-2 (pinned in `kernel/tests/test_xhci_tegra.c`).

The driver then treats the bus as freshly reset: the first connected
USB2 root port is enumerated without waiting for a re-plug, and devices
are addressed with `ADDRESS_DEVICE` (BSR=0) so the controller assigns
the USB address. The kexec-only paths (slot handoff, retained-address
probes) are skipped.

### GA10B cold bring-up

After a UEFI-direct boot nothing has touched the GPU: no PRI ring, no
ACR, no ctxsw firmware, no channels. `main.c` therefore runs
`ga10b_cold_run()` (`kernel/gpu/nvidia/ga10b_cold.c`) when
`boot_source_is_uefi()`; the kexec path keeps inheriting Linux's GPU
state. The steps follow nvgpu's `nvgpu_finalize_poweron` (L4T R36.4.x)
and were checked against an MMIO trace of nvgpu's own first power-on on
jetson-nano-2 (instrumented `nvgpu_writel/readl`; trace and patch in
`~/slmos-ref/derivatives/nvgpu-traces/`). `nvgpu cold [step]` runs or
resumes them from the shell; it is refused after kexec, where it would
reset the GPU state inherited from Linux.

| Step | What it does |
|---|---|
| power | BPMP: GPU partition on, gpusysclk, gpcclk of each non-floorswept GPC (GPC1 is swept on Orin Nano) |
| pri | PRI ring enumerate/start, PBUS error enables |
| engines | FIFO/runlist/PBDMA setup, GR reset cycle (`reset_gpcs`), PERFMON reset |
| fb | NISO sysmem flush page, MMU debug pages |
| gr_hw | Netlist (NETC) non-context register loads |
| acr | ctxsw ucode surface + non-WPR LS blob (PMU, GPCCS, FECS) + `flcn2_acr_desc`; ACR-GSP on the GSP RISC-V copies them into the WPR carveout (MB2 programs it on every boot path) |
| pmu | LS PMU RTOS in its own VM: boot args via MAILBOX0/1, RISC-V start, INIT message (FBQ queues in the super surface) |
| falcons | PMU RPCs ACR `INIT_WPR_REGION` + `BOOTSTRAP_GR_FALCONS`, SM ids, FECS/GPCCS start, context image size |
| gr_init | Atomics, GPC MMU copies, constant GR programming (table generated from the trace by `scripts/tools/gen-ga10b-reg-table.py`), copy-engine enables |
| golden | Global context buffers, golden context image (FECS `GOLDEN_IMAGE_SAVE`) |
| channel | VM, TSG, subcontext, instance block/RAMFC, USERD, GPFIFO, runlist, CHRAM; installed as the active handoff |
| mnist | MNIST v7 pipeline: shaders from the embedded operator library, weights from the embedded `mnist.onnx`, cbufs, outputs, QMD pool |
| policies | Scheduler MLP (kind 1) and eviction Q-net (kind 2) v7 pipelines built and stored next to MNIST's; operator library attached to the channel's VM (#1092) |

The firmware is the L4T set embedded at build time
(`GA10B_FIRMWARE_DIR`, auto-detected at `~/jetson-ga10b-firmware`); the
MNIST shaders are the `scripts/cuda/operator_library/*_shader.sass`
build artifacts. Without them the bring-up stops at the step that needs
them and MNIST stays on the CPU. Smoke checks after boot:
`nvgpu submit` (host semaphore through PBDMA) and `nvgpu submit-compute`
(AMPERE_COMPUTE_B semaphore through GR).

### Scheduler and eviction GPU consumers (#1092)

On the kexec path, `gpu-kernel-sched-mlp` and `gpu-kernel-eviction-qnet`
build these pipelines on Linux and leave v6 handoffs in DRAM.
`ga10b_cold_pipelines.c` builds the same pipelines on the cold channel
instead:
- the same shaders (GEMM, add-bias-ReLU, sigmoid from the operator
  library) and the same op shapes;
- eviction batch 64; add-bias-ReLU with 32-thread CTAs (#1036/#1040);
- the scheduler's own `ai_mlp_*` weights, transposed at build time; the
  scheduler pipeline needs AI_SCHED=ON, the Jetson default;
- the eviction weights from `runtime/src/mm/eviction/generated/mlp_policy_f32.rs`,
  extracted by `cmake/gen_eviction_mlp_weights.cmake`.

Each kind gets a v7 handoff (fresh QMD per launch, own QMD pool), and
`step_policies` stores them per kind next to MNIST's. The first dispatch
of a kind, from `ensure_bringup` in `slm_ffi.c`, installs that kind's
handoff and carries the channel's live GPFIFO cursor over
(`ga10b_cp_switch_handoff`). So MNIST, `gpu use sched` and
`gpu use eviction` share the one channel and switch freely; each switch
costs one warmup dispatch. A kind whose shaders or weights are missing
from the build is skipped with a warning and stays on the CPU.

`nvgpu cold check` runs both pipelines on the helpers' synthetic inputs
and compares them with the helpers' CPU reference: argmax 8 and every
logit within 1e-3 for the scheduler, row-0 score 0.673633 within 1e-4
for eviction. The scheduler reference assumes the default
`AI_WEIGHTS=synthetic`.

| Command | Expected |
|---|---|
| `nvgpu cold` | `cold: rc=0 done=policies` |
| `nvgpu cold check` | `check sched: PASS - argmax 8 ...`, `check eviction: PASS - row 0 score bits 0x3f2c733x ...` |
| `gpu use inference on`, `gpu use sched on`, `sched policy ai_mlp`, `bench sched-policy`, `sched stats` | `ok` > 0 under `GPU dispatch (ai_mlp)` |
| `gpu use eviction on`, `eviction policy mlp`, `eviction stats` after eviction traffic | `ok` > 0, `scores rejected 0` |

### Operator library on the cold channel (#1092)

On the kexec path the operator library (SLM per-op kernels:
`slm_runtime_dispatch_*_simt`, `slm_runtime_stage_weight`, the
`nvgpu oplib ...` verbs) depends on state Linux's helper created: it
maps its SASS pool, cbuf and scratch slots into the helper's channel
through `ga10b_gmmu.c` (keyed off the inherited instance block, found by
handoff / FECS / DRAM walk), stages weights into the helper's 1.5 GB
IOVMM pool (handoff v8/v9) with the copy engine, and fires through the
handoff's QMD pool. None of that exists after a UEFI-direct boot, and
`ga10b_gmmu.c` must not write the cold channel's page tables: it writes
dual PDE0 entries with an 8-byte stride (#1090), while the cold
channel's `struct ga10b_vm` uses nvgpu's 16-byte entries.

`oplib_cold_attach()` (`kernel/gpu/oplib_cold.c`) moves the operator
library onto the cold channel. It is called by the cold bring-up once
the channel's handoff is installed, and it:

1. selects the channel's `ga10b_vm` as the operator library's address
   space (`struct oplib_gpu_as`, `kernel/include/oplib_gpu_as.h`; the
   kexec path keeps a `ga10b_gmmu.c` backend behind the same interface);
2. adds a QMD pool (1024 x 256 B) to the active handoff if it has none
   (channel-only handoff, MNIST step failed) and re-installs it;
3. stages the SASS/scratch pool and the cbuf page into the VM. SASS
   fills as many 64 KB slots as it needs from offset 0, and the three
   scratch slots follow it (`oplib_pool_self_bytes`). With every SLM op
   packed the SASS is ~88 KB, so the pool is 320 KB;
4. can add a weights pool from SLM-OS memory, in 64 MB physically
   contiguous chunks behind one GPU VA range. The boot passes no size,
   like `slmos-kexec`'s helper without `--weights-pool-size`; run
   `nvgpu oplib wpool <MB>` (1536 for Qwen2.5-1.5B Q4_K_M) before
   `slm load`. Every chunk is CPU-reachable, so
   `slm_runtime_stage_weight` copies with memcpy + cache clean: no
   bounce slot, no copy engine;
5. invalidates the VM's TLB once, then runs `oplib_probe_run()` (the
   tier flip `nvgpu oplib stage` does on the kexec path).

While the attached channel's handoff is the active one
(`oplib_cold_active()`), `oplib_pool_stage_to_gpu`,
`oplib_weights_pool_*` and `slm_oplib_prepare_dispatch` use the cold
address space, and the PTE-writing `ga10b_gmmu.c` entry points refuse
that channel's instance block. Checks after boot:

| Command | Expected |
|---|---|
| `nvgpu oplib cold` | `attached: channel 1 ... active=yes`, pool and cbuf GPU VAs whose `GPU walk ->` equals their phys, QMD pool, `weights pool: none` until `nvgpu oplib wpool` |
| `nvgpu oplib status` | `GPU staging: pool gpu_va=0x...` |
| `nvgpu oplib probe 0 2 2` | RMSNORM SIMT FP16 kernel VA (needs `rmsnorm_f16_shader.sass` in the build) |
| `nvgpu oplib smoke 0` (also 2, 3, 5, 6, 12) | `PASS (N output bytes verified zero)` |
| `nvgpu oplib dispatch-rmsnorm 4 16` | output matches the CPU reference |
| `nvgpu oplib wpool 64`, then `nvgpu weights-pool smoke 0x1000` | `smoke PASS ... via GPU page-table walk ... (cold channel)` |

Without the SLM-op SASS (`rmsnorm_f16_shader.sass` etc., extracted on a
Jetson, gitignored) the library only holds the MNIST kernels: lookups
for the SLM ops miss and every op stays on the CPU.

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
   EL3". `xhci_init` now powers XUSB up (#1084) before the first
   aperture access. Unbacked registers inside a powered block do the
   same: `peek 0x03520000 256` stopped the board at padctl offset
   `0x38`, so read padctl one known register at a time.
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
7. **UEFI leaves the DTB above the RAM identity map.** The blob sits in
   BootServicesData at `0x2679DF000`, above the `0x240000000` limit of
   the kernel's RAM mapping. `dtb_parse` runs before the MMU is on and
   reads it fine; the first FDT walk afterwards (the xHCI handoff
   loaders searching for `/slmos-handoff`) ended in a RAS
   "Address Range Error" (`ADDR = 0x80000000401ffd40`) and a powered-off
   core. `vmm_map_firmware_dtb()` now maps the blob read-only at 4 KB
   granularity when no RAM mapping covers it (#1084).
8. **The PORTSC reset ack disabled the port.** After `PRC`, the ack was
   built as `(portsc & ~RW1CS) | PRC`, which still carries `PED`. PED is
   write-1-to-disable, so the write disabled the port the reset had just
   enabled: the hub went from `0x00200e03` (enabled, high speed) to
   `0x000006e1` (PED=0, Polling) on every attempt. All PORTSC
   read-modify-writes now start from a Linux-style neutral value
   (`xhci_portsc_neutral`). The kexec path ran the same code, which is
   the likely source of its "PED=0 / PLS=7 after reset" retries.
9. **A cold port has no speed until it is reset.** A connected USB2
   port reports CCS=1, PED=0, speed 0 and PLS=Polling, so the kexec
   port scan, which keys on a decoded speed, found no device.

10. **FECS/GPCCS need the LS PMU.** Bootstrapped by ACR alone (nvgpu's
    `support_ls_pmu = false` mode) the L4T ctxsw ucode runs and then
    halts with ctxsw mailbox 6 = `0x98`. Linux nvgpu with
    `support_ls_pmu` forced off fails the same way ("falcon ucode init
    timeout"), so this is the firmware's requirement, not a porting
    error: the LS PMU RTOS must load them.
11. **The PMU reports its sw-managed DMEM area at offset 0.** nvgpu's
    bitmap allocator moves a zero base up one block; without that the
    first command buffer lands at offset 0, which the allocator reports
    as failure, and every PMU command fails.
12. **The GSP-RM path was wrong for GA10B** (#579). `gsp_init()` drives
    the discrete-Ampere GSP-RM stack and only printed "firmware missing"
    on Jetson; it is no longer called there.
13. **Every v7 dispatch spent ~146 ms writing to the UART** (#1092). The
    #844 FECS recovery and an op-by-op dump printed about 1.6 KB per
    dispatch, at 115200 baud, with IRQs masked under the dispatch lock.
    Timed with the CPU counter, MNIST spent 146 ms before the doorbell
    and 0.36 ms waiting for the GPU. The lines now print only under
    `gpu debug on`. MNIST went from 146 ms to 0.41 ms per run; on the
    kexec path the same change took it from 154 ms to 0.30 ms. The kexec
    sched and eviction helpers publish v6 handoffs, which take another
    path, so their 0.23–0.25 ms numbers never included this cost.
14. **oplib dispatches read their own sentinel.** `slm_oplib_dispatch`
    waited for the trailing semaphore and then read the output from
    the scratch slots, without the post-launch L2 evict that
    `ga10b_bringup_read_pipeline_output` does for the pipelines (#715).
    On the cold channel, every `nvgpu oplib smoke` read back its 0xCAFE
    sentinel. It passed on kexec only because those slots sat in the
    helper's own buffer. The evict now runs after every oplib dispatch.
15. **The full operator library does not fit one 64 KB slot.** The
    pool layout put SASS in slot 0 and scratch in slots 1-3. With all 14
    kernels packed (88 KB), staging failed, and every SLM op silently
    stayed on the CPU. Self-allocated pools now size the SASS area to
    the library and move the scratch slots behind it. The helper-staged
    kexec pool is used only when the SASS fits slot 0, because a larger
    copy would overwrite the helper's scratch slots.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| L4TLauncher / Linux boots instead of SLM-OS | BootNext not set (deploy script failed), or the image on the ESP is not a PE file |
| `reloc: ABORT ...` then Linux boots | The relocation check refused the move; the line names the reason |
| Console stops after `C calling ExitBootServices` | Post-EBS fault before `primary_cpu`; the early VBAR_EL2 handler prints `!FAULT` with ESR/ELR/FAR over UARTC if it is a synchronous exception |
| `Unhandled Exception in EL3` later in boot | A driver touched an unpowered block (or an unbacked register); power it first or gate it on `boot_source_is_uefi()` |
| `xusb-cold: step N/5` is the last console line | The step stopped the board: 1-2 are BPMP calls, 3 pads/fuse MMIO, 4 FPCI, 5 the CNR wait |
| `xusb-cold: controller not ready, USBSTS=...` | CNR never cleared: the falcon did not start from IFR firmware |
| `cdc_ecm: no USB device` after a UEFI boot | No connected USB2 root port at enumeration; check the `xhci: cold boot — device on USB2 PORTSC[...]` line |
| Board stuck in SLM-OS on every power cycle | A `BOOTAA64.efi` reached the NVMe ESP, or BootOrder was changed. Pick the SD entry from the UEFI boot manager (F11 at the prompt, via `serial_send keys=["F11"]`) and fix it from Linux |
| `GA10B cold bring-up stopped at step N` at boot | The log names the step; `nvgpu cold` resumes from it. `acr`/`pmu`/`falcons` failures print the falcon mailboxes and the PMU's RISC-V debug-buffer lines (`pmu-fw:`) |
| `nvgpu cold check` FAIL, or `sched stats` / `eviction stats` show only fallbacks | `nvgpu cold` must reach `done=policies`; the boot log names a skipped kind (`sched-mlp-gpu:` / `eviction-qnet-gpu:` warnings: shader missing from the operator library, or AI_SCHED=OFF). `gpu debug on` traces each dispatch (slow: it prints under the dispatch lock) |
| Board goes silent during a long CPU-heavy bench | #1093 (UEFI-direct only, cause under investigation) |
| SD Linux does not come up (no SSH, silent console) | Recover without a re-flash: F11 at the firmware prompt (`serial_send keys=["F11"]`), choose UEFI Shell, run `FS3:\EFI\L4T\l4t.efi` (an L4TLauncher copy on the NVMe ESP, so it boots the NVMe rootfs), then mount `mmcblk0p1` from there and fix it. The UEFI Shell sees the SD ext4 rootfs read-only. Type one short command per line; long lines get mangled on this console |
