# arm-smmu-noshutdown (reference material only)

A one-function Linux kernel module that NULLs the `arm-smmu` platform
driver's `.shutdown` callback. Built as part of the #266 Phase 3A A.5
investigation (successor to A.4 — `scripts/tegra-xusb-noshutdown/`).

## Status: reference only

This module is a candidate workaround for the #266 Phase 3A blocker
(USBCMD.RUN=1 wedges the XHCI MMIO aperture post-kexec). The
hypothesis is that `arm-smmu`'s `.shutdown` writes sCR0's CLIENTPD
bit, dropping the xusb stream's SMMU translations before SLM-OS takes
over. NULLing that callback should keep translations live through the
handoff.

Phase 3A was mothballed on 2026-04-18 before this hypothesis could be
conclusively tested (see `docs/jetson-usb-networking-plan.md` §8).
The source is kept alongside `scripts/tegra-xusb-noshutdown/` so a
future investigator has both A.4 (tegra-xusb) and A.5 (arm-smmu) build
+ load scaffolding ready to go.

## Trade-off

Leaving SMMU translations live during the Linux→SLM-OS transition
means residual DMA from other devices (NVMe, networking, audio) can
still write into memory they're mapped into. In practice those
drivers' own `.shutdown` callbacks drain their pipelines first, so
the window is small — but this is NOT a safe production approach,
purely a diagnostic one.

## Building and loading

On a Jetson running the L4T kernel:

```bash
cd scripts/arm-smmu-noshutdown/
make
sudo insmod arm_smmu_noshutdown.ko
sudo dmesg | tail -1
```

Unload:

```bash
sudo rmmod arm_smmu_noshutdown
```

Unloading does NOT restore the original `.shutdown` pointer — see
the file-level comment in `arm_smmu_noshutdown.c` for rationale.

## Related

- `scripts/tegra-xusb-noshutdown/` — A.4 sibling (reported "already
  NULL" on L4T 36.4.7; that result motivated moving to A.5).
- `docs/jetson-usb-networking-plan.md` §8 — full investigation trail.
- GitHub issues #266, #285, #286.
