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
   `../slmos-reference-cache/derivatives/hailort-traces/hailort-v4.23.0-trace-instrumentation.patch`)
4. A/B test: if our byte sequence works through `hailo_pci`, the bug
   is in SLM-OS's bare-metal kernel code. If it doesn't, the bug is
   in the bytes themselves (which would contradict the Pi OS wire
   capture finding — so a negative result here is also informative)

## Scope

**Currently implemented:**

- `--identify` — Issue `HAILO_FW_CONTROL` IDENTIFY via `/dev/hailo0`.
  Sanity check that hailo_pci is loaded and the chip is reachable.
- `--submit-probe` — Replay SLM-OS's boundary-input VDMA descriptor
  layout (desc_count=64, page_size=512, channel=2, 784-byte buffer)
  through the official hailo_pci ioctl path. Diagnostic for #253.
- `--cs-handshake` — Fire SLM-OS's pre-context-info CS RPCs
  (CHANGE_STATUS RESET, CLEAR_CONFIGURED_APPS, GET_HW_CONSTS) through
  HAILO_FW_CONTROL. Validates SLM-OS's CS wire format (parameter_count
  framing + length-prefixed parameters + LE field conventions) against
  the official driver's path.
- `--full-handshake` — End-to-end replay: allocates 3 buffers + desc
  lists, patches the captured ctxsmoke action bodies with the IOVAs
  hailo_pci returned, fires the full handshake (RESET → CLEAR_APPS →
  GET_HW_CONSTS → SET_NETWORK_GROUP_HEADER → 4× SET_CONTEXT_INFO →
  ENABLED), kicks CCW upload on ch=1 and pre-arms ch=16, then
  LAUNCH_TRANSFERs ch=2. Optionally loads real MNIST CCW microcode
  from a co-located `mnist.hef` (see "MNIST HEF" below).

## Build

```bash
make hailo-ushim
```

Produces `build/host-tools/hailo-ushim` — a regular Linux ELF.
Requires `libcrypto` headers (`apt install libssl-dev`).

**Cross-compilation note:** the default `$(CC)` is the host compiler.
On an x86-64 dev box the produced ELF is x86-64 and cannot run on
Pi 5. For pi-5-1 use either:

1. SCP the sources to pi-5-1 (booted to Pi OS) and run `make
   hailo-ushim` there, or
2. Cross-compile: `make hailo-ushim CC=aarch64-linux-gnu-gcc` with
   the matching `-lcrypto` staging sysroot.

Option 1 is simpler for one-off diagnostic runs.

## Usage

```bash
# Sanity check — fw must be booted (by HailoRT or the hailortcli
# run command) before --identify works.
sudo ./build/host-tools/hailo-ushim --identify

# Diagnostic probe: replay SLM-OS's boundary-input descriptor
# geometry through the hailo_pci ioctls. No HEF / fw configuration
# required; the probe tests kernel-side parameter acceptance only.
sudo ./build/host-tools/hailo-ushim --submit-probe

# Validate SLM-OS's CS wire format (RESET, CLEAR_CONFIGURED_APPS,
# GET_HW_CONSTS) through HAILO_FW_CONTROL.
sudo ./build/host-tools/hailo-ushim --cs-handshake

# End-to-end replay of SLM-OS's full CS handshake + LAUNCH_TRANSFER.
# Place mnist.hef in the working directory (or /opt/hailort/models/)
# to upload real CCW microcode on ch=1.
sudo ./build/host-tools/hailo-ushim --full-handshake
```

### --submit-probe: what it does

Seven-step sequence through the official ioctl surface:

1. `mmap()` a 4 KB userspace buffer (rounded-up from 784 MNIST bytes)
2. `HAILO_VDMA_BUFFER_MAP` — pin the page, get an IOVA mapping
3. `HAILO_DESC_LIST_CREATE` — 64 descriptors × 512-byte page, non-
   circular; matches SLM-OS's `HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE`
   + `HAILO_CS_DEFAULT_BOUNDARY_DESC_COUNT`
4. `HAILO_DESC_LIST_PROGRAM` — bind the buffer to channel 2 (which
   is where ACTIVATION opens the boundary input channel)
5. `HAILO_VDMA_ENABLE_CHANNELS` — arm channel 2
6. `HAILO_VDMA_LAUNCH_TRANSFER` — the "kick" that writes num_avail
7. `HAILO_VDMA_INTERRUPTS_WAIT` — 2 s timeout, then the probe
   assumes fw wasn't configured and reports success if all prior
   steps accepted parameters.

### Interpreting results

- **Any ioctl rejects a parameter (steps 2-6)** → we've localized a
  bug in SLM-OS's descriptor geometry that the audit missed. The
  error message names the offending field.
- **All ioctls succeed; step 7 times out** → the kernel-side
  parameter validation is happy with SLM-OS's layout. The #253 bug
  is **not** in our descriptor geometry — it's deeper (firmware
  configuration state, or something in our bare-metal MMIO/cache
  path that the official driver handles differently).
- **Step 7 reports a completion** → the fw had something listening
  on channel 2 and our transfer went through. Most likely a leftover
  from a previous HailoRT run; interpret with care.

The diagnostic value is greatest when the probe produces a
**decisive signal** (either rejection or timeout). A successful
transfer is more ambiguous.

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
