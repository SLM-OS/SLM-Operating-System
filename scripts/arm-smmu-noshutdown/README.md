# arm-smmu-noshutdown

Two Linux kernel modules that together investigate — and partially
unblock — the Jetson Orin Nano kexec handoff into SLM-OS (#266 Phase
3A, Path 1). Both build against the running L4T kernel's headers.

| File                              | Role                                                                                                                    |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `arm_smmu_noshutdown.c`           | Two-step fix: (1) NULL `arm-smmu`'s `platform_driver.shutdown` so Linux's kexec path does NOT fire `arm_smmu_device_shutdown()`; (2) call `iommu_map()` on the xusb stream's existing domain to add an identity mapping over SLM-OS's NC memory region (0xBDE00000, 2 MB). |
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

## Option 1: Linux-side identity IOMMU mapping (added 2026-04-18)

In addition to NULLing `.shutdown`, `arm_smmu_noshutdown_init` now
calls `iommu_map()` on the xusb stream's existing
`iommu_domain` (type `IOMMU_DOMAIN_DMA`) to register an identity
mapping covering SLM-OS's 2 MB NC memory region at 0xBDE00000. After
this call, SLM-OS can program DCBAAP / CRCR with raw physical
addresses in that range and have them translate straight through
the SMMU — no bypass, no context-bank replacement, no race with
in-flight Linux DMA.

Hardware verification on jetson-nano-1 (L4T 36.4.4):

```
arm-smmu-noshutdown: NULLed arm-smmu platform_driver->shutdown
  (#266 Phase 3A) — arm_smmu_device_shutdown() will no longer fire at kexec
arm-smmu-noshutdown: xusb iommu_domain type=3
  (IOMMU_DOMAIN_DMA=3, IDENTITY=4, UNMANAGED=1)
arm-smmu-noshutdown: added identity IOMMU mapping IOVA 0xbde00000..0xbe000000
  (PA identical) on xusb domain (#266 Phase 3A Path 1 option 1)
```

After kexec, SLM-OS's xhci init prints both halves of the
#266 Phase 3A success criterion:

```
[INFO] xhci: controller running (USBSTS=0x00000000)
[INFO] xhci: skipping non-command event type 32
[INFO] xhci: skipping non-command event type 32
[INFO] xhci: NO_OP round-trip OK (cc=SUCCESS, cmd_trb @0xbde04180)
```

- `controller running (USBSTS=0x00000000)` — RUN=1 no longer wedges
  the aperture.
- `NO_OP round-trip OK (cc=SUCCESS, cmd_trb @0xbde04180)` — the HC
  successfully DMA-read the NO_OP TRB from SLM-OS's command ring at
  0xbde04180 (inside our identity-mapped region), executed it, and
  DMA-wrote the completion event back to the event ring. Full
  DMA round-trip through the SMMU with translation still enabled.

The two `skipping non-command event type 32` lines before the
success are Port Status Change events left in the event ring by
Linux's prior xHCI session; SLM-OS's NO_OP handler walks past
them to find its own completion.

## What earlier attempts to unblock NO_OP ruled out

Prior to Option 1, four variants of bypass were tested:

1. Replacement `.shutdown` that writes `sCR0 = CLIENTPD` but skips
   the clock teardown. Wedges the aperture — tegra-xusb has no
   `.shutdown`, so the xHCI is still actively DMA-ing when the
   bypass flips; in-flight IOVAs (e.g. DCBAAP = 0x7ffffff000) get
   reinterpreted as raw PAs in unmapped regions.
2. Same replacement but targeting only the xusb-instance SMMU
   (iommu@8000000). Wedges identically.
3. Same replacement preserving existing sCR0 bits. Wedges
   identically.
4. SLM-OS-side `sCR0 = CLIENTPD` write after `xhci_halt()`, with
   the NULL-field module preserving SMMU clocks across kexec.
   Wedges identically.

Option 1 avoids all four failure modes by leaving the SMMU fully
enforcing translations throughout, and simply adding a valid
identity mapping for SLM-OS's memory region.

See `docs/jetson-usb-networking-plan.md` §10.8 for the full matrix
and the order in which options were attempted.

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
