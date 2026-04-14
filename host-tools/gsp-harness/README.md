# gsp-harness — Linux userspace harness for GSP-RM development

This tool runs the SLM-OS GSP-RM bringup code (shared core in
`kernel/gpu/nvidia/`) against a real NVIDIA Ampere GPU from Linux
userspace, through VFIO. Iteration under a few seconds per attempt
vs. the ~2-minute reflash-reboot cycle required for bare-metal
SLM-OS — critical during E3 (Falcon/RISC-V bringup) and E4 (RPC ring
bring-up) where silent hangs are the norm.

**Architectural reuse:** the harness is a third implementation of
`struct gsp_platform_ops` (the vtable defined in `kernel/gpu/nvidia/gsp.h`)
— alongside the x86-64 bare-metal implementation in
`kernel/arch/x86_64/nvidia_gsp_platform.c` and the (future) Jetson
one. The actual bringup code in `kernel/gpu/nvidia/gsp.c` runs
unchanged. If it works under the harness against real silicon, it
works bare-metal too.

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

# Attempt Phase 0 only (firmware manifest sanity check, no hardware).
./build/host-tools/gsp-harness --phase 0

# Full bringup attempt (phase 0 through 7). Blocks up to ~2 s waiting
# for GSP_INIT_DONE; reports which phase failed if bringup fails.
sudo ./build/host-tools/gsp-harness --bringup

# Tail register trace as phases run — dumps every BAR0 read/write.
sudo ./build/host-tools/gsp-harness --bringup --trace
```

Requires `CAP_SYS_RAWIO` to map `/sys/bus/pci/devices/*/resource*`;
`sudo` is the easiest way.

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
