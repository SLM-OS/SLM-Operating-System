# tegra-xusb-noshutdown (reference material only)

A one-function Linux kernel module that NULLs the `tegra-xusb`
platform driver's `.shutdown` callback. Built and tested on
jetson-nano-1 as part of the #266 Phase 3A investigation
(Option A.4 of #285).

## Status: mothballed

This module does **not** unblock USB networking on Jetson. Phase 3A
was mothballed on 2026-04-18 after this module proved the wrong
hypothesis:

```
tegra-xusb-noshutdown: shutdown already NULL — no-op
```

The `tegra-xusb` driver on L4T 36.4.7 has no `.shutdown` hook in the
first place. The kexec-time blocker we were chasing (xHCI controller
wedging at `USBCMD.RUN=1`) is actually caused by `arm-smmu`'s own
`.shutdown` dropping the xusb stream's SMMU translations — a
different mechanism entirely. Full investigation writeup is in
[`docs/jetson-usb-networking-plan.md`](../../docs/jetson-usb-networking-plan.md) §8.

## Why it's still in the tree

The module source and Makefile are kept as reference for anyone
revisiting this area. Having the build + load scaffolding already
written saves the next investigator from rewriting it from scratch.

## Building and loading

On a Jetson running the L4T kernel:

```bash
# Build against the running kernel's headers
cd scripts/tegra-xusb-noshutdown/
make

# Load (reports "shutdown already NULL" on L4T 36.4.7)
sudo insmod tegra_xusb_noshutdown.ko
sudo dmesg | tail -1
```

Unload:

```bash
sudo rmmod tegra_xusb_noshutdown
```

Unloading does NOT restore the original `.shutdown` pointer — see
the file-level comment in `tegra_xusb_noshutdown.c` for rationale.
