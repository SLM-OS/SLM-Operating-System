# Jetson Channel Helper — Rollback Instructions

Files added to jetson-nano-2 during the Phase 6 channel-inherit
experiment. Current status (2026-04-17): **ROLLED BACK** — the
experiment hit an ioctl blocker and all Jetson-side files have
been removed. Keeping this document for future re-attempts.

## Baseline (verified clean 2026-04-17)

```
/root/slmos.elf                  — md5: fa7c12ab9fa2fc9f098ea626b8293939
/usr/local/bin/slmos-kexec       — md5: 0c433f3331eb21661370496a78f5df00
(no /root/gpu-channel-helper* files)
```

## Files to remove if re-attempted and abandoned

```bash
ssh root@192.168.4.93 'rm -f /root/gpu-channel-helper /root/gpu-channel-helper.c'
```

The Jetson-side `slmos-kexec` helper was never modified — the
`--no-gpu-suspend` flag lives in the in-tree version at
`scripts/jetson-kexec-slmos.sh`. That script can be deployed to
`/usr/local/bin/slmos-kexec` when the full channel-inherit flow
is ready to test, but the current deployed version stays unchanged.

## How to verify clean state

```bash
ssh root@192.168.4.93 'ls /root/gpu-channel-helper* 2>/dev/null && echo "NOT CLEAN" || echo "CLEAN"'
```

## What we learned (blocker notes)

The first ioctl `NVGPU_GPU_IOCTL_ALLOC_AS` returns EINVAL on L4T
r36.4.7. `/dev/nvhost-as-gpu` also returns EINVAL on `open()`.
CUDA successfully creates channels via the same ioctl path (seen
in ftrace `gk20a_as_ioctl_alloc_space`), so the kernel side works
— our userspace invocation is missing something. L4T's nvgpu
source isn't on the Jetson (only headers + binary module), so
further diagnosis needs either strace on a CUDA program or L4T
source access.

See `scripts/gpu-channel-helper.c` top-of-file comment for the
full status.
