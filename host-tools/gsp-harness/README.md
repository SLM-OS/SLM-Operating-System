# gsp-harness — Linux userspace harness for GSP-RM development

This tool runs the SLM-OS GSP-RM bringup code (shared core in
`kernel/gpu/nvidia/`) against a real NVIDIA Ampere GPU from Linux
userspace, through VFIO. Iteration under a few seconds per attempt
vs. the ~2-minute reflash-reboot cycle required for bare-metal
SLM-OS — critical during E3 (Falcon/RISC-V bringup) and E4 (RPC ring
bring-up) where silent hangs are the norm.

**Architectural reuse:** the harness is one of several implementations of
`struct gsp_platform_ops` — the shared bringup code in
`kernel/gpu/nvidia/` runs unchanged across all platforms. If it works
under the harness against real silicon, it works bare-metal too.
See `docs/nvidia-gsp.md` §"Platform Shim Contract" for the full vtable
specification and the list of all platform implementations.

See `docs/testing/test-pc-linux-vfio-setup.md` for the one-time
Linux + VFIO prerequisites.

## Build

```bash
make gsp-harness PLATFORM=X86_64
```

Produces `build/host-tools/gsp-harness` — a regular Linux ELF.

## Usage

```bash
# Baseline probe — verifies BAR0/BAR1 are mapped, reads GPU ID.
sudo ./build/host-tools/gsp-harness --probe

# Read + parse the VBIOS via the BAR0 PROM window (NV_PROM_DATA at
# offset 0x300000). Reports the sub-image map, BIT entries, and
# FWSEC presence. Doesn't touch GSP.
sudo ./build/host-tools/gsp-harness --vbios

# Probe GSP + SEC2 Falcon engines. Reads HWCFG to report IMEM/DMEM
# sizes, reset/halt state, RISC-V capability. Hardware smoke test
# for the E3.1 Falcon driver — no engine state change. (E3)
sudo ./build/host-tools/gsp-harness --falcons

# Allocate + IOMMU-map + free three DMA buffers via VFIO. Confirms
# the E3.2 DMA plumbing works end-to-end and the IOMMU returns
# IOVAs in the expected high range. (E3)
sudo ./build/host-tools/gsp-harness --dma-test

# Run FWSEC-FRTS on GSP Falcon. The first real GSP-RM bringup
# step — sets up the WPR2 region in FB. Reports sig-index
# selection, WPR2 target, and post-boot Falcon state. (E3.4 WIP)
sudo ./build/host-tools/gsp-harness --fwsec-frts

# Attempt Phase 0 only (firmware manifest sanity check, no hardware).
./build/host-tools/gsp-harness --phase 0

# Full bringup attempt (phase 0 through 7). Blocks up to ~2 s waiting
# for GSP_INIT_DONE; reports which phase failed if bringup fails.
sudo ./build/host-tools/gsp-harness --bringup

# Tail register trace as phases run — dumps every BAR0 read/write.
sudo ./build/host-tools/gsp-harness --bringup --trace
```

Requires `CAP_SYS_RAWIO` to map `/sys/bus/pci/devices/*/resource*`;
`sudo` is the easiest way. The GPU must be in D0 (not D3hot) — set
via `echo on > /sys/bus/pci/devices/0000:01:00.0/power/control`
before running. The VFIO post-install script
`docs/testing/test-pc-vfio-postinstall.sh` does this.

## Host-side tests

The following Makefile targets run without hardware and are safe
to wire into CI:

```bash
# VBIOS parser — synthetic images + malformed / edge-case inputs +
# Ampere multi-sub-image chain + FWSEC split helper.
make test-vbios

# Firmware extraction script — exit-code coverage for missing zstd,
# missing firmware dir, and (if locally installed) a full
# /lib/firmware/nvidia/ga107/ happy path.
make test-gsp-extract

# Falcon v4 driver — mock BAR0 vtable, exercises probe / reset /
# halt poll / DMA protocol / 40-bit IOVA splitting / HS-boot
# BROM programming.
make test-falcon

# nvfw container parser — both bin_magic variants + every
# rejection path. Validated against real R535 booter_load.bin
# at run time.
make test-nvfw

# GSP-RM bringup pure-logic helpers — sig-index algorithm
# (matches nouveau ga102_gsp_fwsec_signature) + DMEMMAPPER
# patcher. The full FWSEC-FRTS sequence is hardware-only and
# runs via `--fwsec-frts` above.
make test-bringup

# GA10B (Jetson integrated Ampere) nvgpu-native bringup — mock
# BAR0 vtable + synthetic firmware blobs emitted via inline asm.
# Covers firmware accessor, prepare guards, phase-ordering state
# machine, and the ACR HS load sequence (reset assert/deassert,
# PIO byte-exact transport, BCR_CTRL=0x11, STARTCPU, halt, retcode).
# 10 test cases. The on-hardware BROM authentication is Jetson-only
# and requires a real GPU — this test catches logic regressions in
# the bringup state machine and PIO plumbing without a GPU.
make test-ga10b-bringup
```

## Recovering from a hung GPU

A stuck Falcon hangs the whole GSP complex. Usually FLR recovers:

```bash
sudo sh -c 'echo 1 > /sys/bus/pci/devices/0000:01:00.0/reset'
```

If that doesn't recover, force PCIe re-enumeration:

```bash
sudo sh -c '
  echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove
  sleep 1
  echo 1 > /sys/bus/pci/rescan
'
```

Worst case, `sudo reboot`. In practice, reboots are the ~5% case —
most iteration stays under 5 seconds.
