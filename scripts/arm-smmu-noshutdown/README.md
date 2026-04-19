# arm-smmu-noshutdown

Two Linux kernel modules that together investigate — and partially
unblock — the Jetson Orin Nano kexec handoff into SLM-OS (#266 Phase
3A, Path 1). Both build against the running L4T kernel's headers.

| File                              | Role                                                                                                                    |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `arm_smmu_noshutdown.c`           | NULLs `arm-smmu`'s `platform_driver.shutdown` so Linux's kexec path does NOT fire `arm_smmu_device_shutdown()`.         |
| `smmu_probe.c`                    | Diagnostic. Prints the live `arm-smmu` platform_driver callbacks + bound devices. Used to verify the field-fix worked. |

## Why both modules exist

The previous single-module version stored `NULL` into the WRONG struct
field — it patched `device_driver.shutdown` (base-struct), but the
platform bus dispatches shutdown through `platform_driver.shutdown`
via `platform_drv_shutdown`. The base field is unused for platform
drivers, so the template silently no-op'd on every tested L4T kernel
and reported "shutdown already NULL" while `arm-smmu ... disabling
translation` continued to fire at every kexec.

The field bug was diagnosed by adding a second module (`smmu_probe`)
that reads both halves of the struct and prints what's actually in
memory. The diagnostic is shipped alongside the fix so that anyone
reproducing the work — or maintaining it as L4T revs forward — can
verify in a single kexec-free reboot that the correct field is being
patched. See `docs/jetson-usb-networking-plan.md` §10.3 and §10.8 for
the full investigation.

## Building

On a Jetson running the L4T kernel:

```bash
cd scripts/arm-smmu-noshutdown/
make
# → arm_smmu_noshutdown.ko  smmu_probe.ko
```

## Verification procedure (hardware-only, five kexec-free minutes)

This is the functional test for `arm_smmu_noshutdown`. It runs on a
live Jetson without needing to kexec into SLM-OS, and covers all
observable claims the fix makes.

1. **Observe the baseline.** Before loading anything, the platform
   driver's shutdown pointer should be pointing at the real
   `arm_smmu_device_shutdown`:

   ```bash
   sudo dmesg -C
   sudo insmod smmu_probe.ko
   sudo dmesg | grep 'smmu-probe:'
   ```

   Expected:

   ```
   smmu-probe: arm-smmu driver @ ffffXXXXXXXXXXXX
   smmu-probe:   .probe    = arm_smmu_device_probe+0x0/0xc80
   smmu-probe:   .remove   = arm_smmu_device_remove+0x0/0x1a0
   smmu-probe:   .shutdown = arm_smmu_device_shutdown+0x0/0x40
   smmu-probe:   .driver.pm= arm_smmu_pm_ops+0x0/0xb8
   smmu-probe:   .driver.bus->shutdown = platform_shutdown+0x0/0x60
   smmu-probe:   bound dev=8000000.iommu
   smmu-probe:   bound dev=10000000.iommu
   smmu-probe:   bound dev=12000000.iommu
   ```

   The key line is `.shutdown = arm_smmu_device_shutdown+0x0/0x40` —
   that is the pointer `platform_drv_shutdown` reads during
   `device_shutdown()` on the kexec path.

2. **Apply the fix.**

   ```bash
   sudo rmmod smmu_probe
   sudo insmod arm_smmu_noshutdown.ko
   sudo dmesg | tail -1
   ```

   Expected:

   ```
   arm-smmu-noshutdown: NULLed arm-smmu platform_driver->shutdown
     (#266 Phase 3A) — arm_smmu_device_shutdown() will no longer fire
     at kexec
   ```

3. **Confirm the fix took effect.**

   ```bash
   sudo insmod smmu_probe.ko
   sudo dmesg | grep 'smmu-probe:   .shutdown'
   ```

   Expected:

   ```
   smmu-probe:   .shutdown = (null)
   ```

   Anything else — e.g. `arm_smmu_device_shutdown+0x0/0x40` — means
   the fix is not reaching the field `platform_drv_shutdown` reads,
   and must be investigated before relying on this module during a
   kexec.

4. **(Optional) End-to-end kexec check.** Capture serial during a
   kexec into SLM-OS (via `slmos-kexec /path/to/slmos.elf`) and
   confirm the Linux dmesg just before `kexec_core: Starting new
   kernel` does NOT contain `arm-smmu ... disabling translation`.
   When the fix is working, only the `arm-smmu 10000000.iommu` and
   `arm-smmu 12000000.iommu` instances (the ones this module does
   NOT patch) should log that message; the `8000000.iommu` line —
   the instance that serves the xusb stream — must be silent.

## What this fix does NOT achieve

`arm_smmu_device_shutdown` no longer firing is necessary but not
sufficient for #266 Phase 3A's success criterion (`USBCMD.RUN=1` does
not wedge the xHCI aperture + NO_OP round-trip OK). With the fix
applied, the SMMU stays running with Linux's IOVA-bound context
banks, and SLM-OS's raw physical addresses for DCBAAP / CRCR still
fail to translate; the xHCI aperture still wedges to `0xFFFFFFFF` on
most kexec attempts.

Five follow-on variants were tested this session and all either
regressed or behaved no better than plain NULL — see
`docs/jetson-usb-networking-plan.md` §10.8 for the full matrix.
Bringing NO_OP to success requires either a proper Path 2 port of
arm-smmu-v2 into SLM-OS (~350-500 lines, programs a context bank
for SLM-OS's PAs) or a much more careful pre-kexec sequence that
quiesces every live bus master before flipping bypass.

## Unloading

```bash
sudo rmmod smmu_probe             # diagnostic only
sudo rmmod arm_smmu_noshutdown    # does NOT restore the original pointer
```

Unloading `arm_smmu_noshutdown` does NOT restore
`platform_driver.shutdown` — the module is designed as
load-and-forget-until-reboot. Reboot the Jetson to return to the
unmodified arm-smmu state.

## Related

- `scripts/tegra-xusb-noshutdown/` — A.4 sibling (tegra-xusb has no
  `.shutdown` on any tested L4T kernel; that result motivated moving
  to A.5, i.e. this module).
- `docs/jetson-usb-networking-plan.md` §10.3, §10.6, §10.8.
- GitHub issues #266, #285, #286.
