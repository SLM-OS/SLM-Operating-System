# SLM-OS TF-A Patches for Pi 5

These patches build a custom Trusted Firmware-A `bl31.bin` (the
EL3 firmware Pi 5 loads as `armstub8-2712.bin`) to address #134 —
hardware timer IRQ delivery to the SLM-OS kernel.

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
