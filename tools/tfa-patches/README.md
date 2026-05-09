# SLM-OS TF-A Patches for Pi 5 + Jetson Orin Nano

These patches build a custom Trusted Firmware-A `bl31.bin` so SLM-OS
can receive hardware timer interrupts at NS-EL2/VHE.

- **Pi 5** (`0001-*`, `0002-*`, `0003-*`) — addresses #134 (timer IRQ
  delivery on Pi 5).
- **Jetson Orin Nano / Tegra234** (`0004-*`) — addresses #380 (timer
  IRQ delivery on Jetson).

Both targets share the same root cause and the same two-pronged fix:
clear `SCR_EL3.IRQ`/`FIQ` for the NS context at ERET, and promote
PPIs/SGIs to Group 1 NS so they're delivered to NS-EL2 instead of
trapping to EL3.

## What the patches do

`0001-SLM-OS-Pi-5-IRQ-routing-patches.patch` is a single squashed
commit on top of upstream TF-A `master`. Three changes:

1. **Clear `SCR_EL3.IRQ` and `SCR_EL3.FIQ`** in `setup_ns_context`
   for the BL33 entry context (`lib/el3_runtime/aarch64/context_mgmt.c`).
   Gated on `PLAT_RPI5` so other platforms are unaffected.

2. **Mark all PPIs as Group 1 NS** from `plat_rpi_bl31_custom_setup`
   in `plat/rpi/rpi5/rpi5_setup.c`. TF-A's default
   `gicv2_spis_configure_defaults` only touches SPIs (IRQs ≥32);
   PPIs (16–31, including timer 30) stay in Group 0 unless we
   write `GICD_IGROUPR[0]` from EL3 ourselves.

3. **`PLAT_RPI5` define** added to `plat/rpi/rpi5/platform.mk` so
   `context_mgmt.c` can platform-gate the change above.

Plus diagnostic `NOTICE()` prints that go to the BCM2712 PL011
(not the user-visible RP1 UART, so they're invisible without
hardware-level instrumentation).

## How to build

The TF-A source tree is expected at `~/slmos-ref/tf-a/` (part of the
local reference library — see top-level `CLAUDE.md` "Reference File
Cache"). It's not a git repo anymore; just a working tree of the
ARM-software/arm-trusted-firmware sources with this patch applied.

```bash
# 1. Set up the TF-A tree (one-time, if not already present)
mkdir -p ~/slmos-ref/tf-a
cd ~/slmos-ref/tf-a
git clone https://github.com/ARM-software/arm-trusted-firmware.git .
git checkout master
git am <SLM-OS-checkout>/tools/tfa-patches/0001-SLM-OS-Pi-5-IRQ-routing-patches.patch
# (After the initial setup, you can remove the .git dir — the source
# tree is read-only reference material from this point.)

# 2. Build with the kernel toolchain
cd ~/slmos-ref/tf-a
PATH=/opt/arm-gnu-toolchain/bin:$PATH \
    make PLAT=rpi5 CROSS_COMPILE=aarch64-none-elf- DEBUG=0 LOG_LEVEL=40 -j4 bl31

# Output: build/rpi5/release/bl31.bin (~32 KB)
```

Or use the Makefile target in this repo (see `make tfa-pi5`), which
expects `TFA_DIR=~/slmos-ref/tf-a`.

## Jetson Orin Nano (#380, t234) — what `0004-*` does

`0004-SLM-OS-Jetson-IRQ-routing-patches.patch` ports the same idea to
NVIDIA's downstream TF-A for Tegra234. Three changes — narrower than
the Pi 5 patch in two important places because Tegra234's BL31 uses
SDEI to deliver RAS uncorrectable-error notifications back to NS, and
SDEI requires (a) Group 0 FIQs routed to EL3 and (b) Event 0 bound to
a Secure SGI:

1. **Clear `SCR_EL3.IRQ` only** in `setup_ns_context`
   (`lib/el3_runtime/aarch64/context_mgmt.c`), gated on
   `PLAT_TEGRA234_SLMOS_PREEMPT`. **`SCR_EL3.FIQ` is left set** —
   clearing it routes Group 0 FIQs to NS, which breaks SDEI's RAS
   delivery path. Symptom of the wider variant we tested first:
   `RAS Uncorrectable Error in IOB ... sdei_dispatch_event returned
   -1 / Powering off core` on first IOB error after boot. Pi 5's
   `0001-*` patch can clear both bits because RPi firmware doesn't
   use SDEI; Tegra cannot.

2. **OR `GICR_IGROUPR0 |= 0xFFFF0000U`** (PPI bits 16..31 only) in
   `tegra_gicv3.c` on every per-CPU init entry: `tegra_gic_init`
   (primary boot), `tegra_gic_pcpu_init` (secondary CPU_ON warmboot),
   and `tegra_gic_restore` (after suspend/resume). Because Tegra234
   is GICv3, the PPI/SGI Group bits live in each CPU's redistributor,
   not the distributor — so the write must run per-CPU. **SGI bits
   0..15 are left untouched** — promoting them to Group 1 NS makes
   SDEI Event 0 fail its `is_secure_sgi(map->intr)` assertion at
   `services/std_svc/sdei/sdei_main.c:161` during BL31 init.

3. **`PLAT_TEGRA234_SLMOS_PREEMPT` define** added to
   `plat/nvidia/tegra/soc/t234/platform_t234.mk`.

NVIDIA's downstream BL31 source ships in the L4T BSP. After running
NVIDIA's `flash.sh` once with the BSP, the source tree is at:

```
~/slmos-ref/nvidia/Linux_for_Tegra/source/arm-trusted-firmware/
```

(The L4T `public_sources.tbz2` archive's `atf_src.tbz2` extracted in
place. See top-level `CLAUDE.md` "Reference File Cache" for the
sibling-directory layout convention.)

```bash
cd ~/slmos-ref/nvidia/Linux_for_Tegra/source/arm-trusted-firmware
git apply <SLM-OS-checkout>/tools/tfa-patches/0004-SLM-OS-Jetson-IRQ-routing-patches.patch

# Build flags mirror NVIDIA's L4T `source/nvbuild.sh::build_atf_sources`.
# `SPD=opteed` is critical — without it, BL31 has no Secure Payload
# Dispatcher and OP-TEE's first SMC call into BL31 silently lands in
# oblivion, surfacing as a deterministic RAS Uncorrectable error in
# IOB at 0x03270000 (QSPI0) on first OP-TEE-side QSPI access.
PATH=/opt/arm-gnu-toolchain/bin:$PATH \
    make PLAT=tegra TARGET_SOC=t234 \
         CROSS_COMPILE=aarch64-none-elf- \
         SPD=opteed BRANCH_PROTECTION=3 ARM_ARCH_MINOR=3 \
         DEBUG=0 LOG_LEVEL=20 -j4 bl31

# Output: build/tegra/t234/release/bl31/bl31.bin
```

Or use the in-tree Makefile target which encapsulates all of the above:

```bash
make tfa-jetson    # builds bl31.bin in TFA_JETSON_DIR
```

### Deploying on Jetson Orin Nano

Two paths, prefer (a):

**(a) UEFI capsule update** — runs from booted Jetson Linux, no
recovery cable. Needs the BSP's capsule packaging tooling
(`Linux_for_Tegra/tools/ota_tools/`). Faster iteration loop.

**(b) USB Force Recovery + `flash.sh`** — definitive fallback. Boot
the Jetson into recovery mode (jumper J14 pins 9-10, then power on)
and run `flash.sh -k A_bl31` (or whatever the BSP's BL31 partition
name resolves to in `flash.xml.in`) on the host. Same flow as
re-flashing the entire BSP, just scoped to BL31.

Archive the stock `bl31.bin` first — recovery flash is the rollback
path if the patched binary misbehaves.

## How to deploy

Copy `bl31.bin` to the Pi 5 boot partition under the name
`armstub8-2712.bin`, and ensure `armstub=armstub8-2712.bin` is in
`config.txt`:

```bash
labctl sdwire_update <SBC> --partition 1 \
    --copies "/path/to/bl31.bin:armstub8-2712.bin"
```

## Status

Patches build cleanly. Custom TF-A boots cleanly on pi-5-2 — PSCI
continues to work (4 CPUs come up). However, **timer IRQ delivery
to the kernel is still not observed**: per-CPU IRQ counters in the
`diag` shell command remain at 0 across all CPUs.

Most likely root cause: the kernel's tasks run with `DAIF.I=1` and
the idle task on CPU 0 (which would `daifclr+wfi`) rarely runs
because the shell + `net_pump` keep CPU 0 busy. Even with our SCR
patch, the CPU never holds an unmasked IRQ state long enough to
take the pending timer IRQ.

Next step (Stage 2.5): kernel-side change to unmask `DAIF.I` in
tasks (paired with `SECONDARY_PREEMPT=ON` so `schedule()`-from-IRQ
goes through the ELR trampoline). With unmask in place, the
timer IRQ should deliver.

## Provenance

Each patch has the SLM-OS commit SHA-1 in its commit message. The
upstream TF-A baseline is captured by the `From:` line of the
`am`-formatted patch.
