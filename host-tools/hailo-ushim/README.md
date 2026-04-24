# hailo-ushim — Linux userspace shim for SLM-OS Hailo backend debug

A minimal Linux tool that drives `hailo_pci` (the HailoRT kernel driver)
directly via its IOCTL surface — bypassing the HailoRT userspace
library. The goal is to replay SLM-OS's exact control-channel byte
sequence + VDMA operations against real Hailo-8L hardware on Pi 5 from
a debuggable Linux environment, so we can narrow down where the
Phase 8 boundary-submit silence bug actually lives.

## Why

SLM-OS's boundary submit on Hailo-8L hangs: fw never advances
`num_proc` on ch=2 despite:
- Wire-identical MMIO sequence vs HailoRT (verified)
- Byte-identical SET_CONTEXT_INFO action bodies vs HailoRT (verified)
- Correct cache-clean → DRAM propagation (verified via dc ivac)
- Addresses in the reachable inbound-window range (verified)

The bug isn't in any host-observable data or MMIO. It's in some
execution-context or fw-state difference we can't see from SLM-OS's
bare-metal view alone.

This tool lets us:

1. Run exact SLM-OS-style control sequences through `hailo_pci` kernel
   driver (same kernel ioread32/iowrite32 path HailoRT uses)
2. Use `gdb` to single-step the userspace side
3. Add arbitrary `dmesg`-visible logging via hailo_pci's
   `trace_mmio`/`trace_ioctl` module params (already wired per
   `docs/reference/hailort-v4.23.0-trace-instrumentation.patch`)
4. A/B test: if our byte sequence works through `hailo_pci`, the bug
   is in SLM-OS's bare-metal kernel code. If it doesn't, the bug is
   in the bytes themselves (which would contradict the Pi OS wire
   capture finding — so a negative result here is also informative)

## Scope

**Currently implemented (v1, 2026-04-24):**

- Open `/dev/hailo0`
- Issue `HAILO_FW_CONTROL` with the IDENTIFY opcode (`--identify`)

**Planned (audit F-10, not yet implemented):**

- Allocate DMA buffers via `HAILO_VDMA_BUFFER_MAP` (low + high
  pools, to A/B-test the F-01 reachability hypothesis)
- Create desc lists via `HAILO_DESC_LIST_CREATE`
- Program descriptors via `HAILO_DESC_LIST_PROGRAM`
- Submit transfers via `HAILO_VDMA_LAUNCH_TRANSFER` and report
  whether `num_proc` advances (`--submit-probe`)

The submit-probe path would be the fastest A/B oracle for #253 —
it would let us isolate whether the boundary-input descriptor stall
is caused by SLM-OS's bare-metal kernel context (not seeing the
descriptor) or by the bytes themselves (which would also fail when
submitted through the official `hailo_pci` IOCTLs).

## Build

```bash
make hailo-ushim
```

Produces `build/host-tools/hailo-ushim` — a regular Linux ELF.

## Usage

```bash
# Identify the board (sanity check that /dev/hailo0 is there)
sudo ./build/host-tools/hailo-ushim --identify
```

`--submit-probe` is not yet implemented (see Scope above).

Requires:
- `hailo_pci` kernel module loaded (fine to be the instrumented
  version with `trace_mmio=1 trace_ioctl=1`)
- `/dev/hailo0` accessible
- Device firmware already booted (run `hailortcli fw-control identify`
  once to boot the fw, or this tool can optionally boot via
  `HAILO_FW_CONTROL` + the boot blob path)

## Files

- `main.c` — test driver + argv parsing
- `hailo_dev.c` — thin wrappers around `/dev/hailo0` IOCTLs
- `hailo_dev.h` — type mirrors + IOCTL magic numbers

## What this does NOT do

- Doesn't boot firmware (assume already booted — saves ~200 lines)
- Doesn't parse HEFs (use SLM-OS's parser output, hardcoded byte
  sequences for MNIST)
- Doesn't implement inference orchestration (just the specific failing
  sequence)
