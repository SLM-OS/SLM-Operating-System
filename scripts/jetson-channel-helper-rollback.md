# Jetson Channel Helper — Rollback Instructions

Files added to jetson-nano-2 for the Phase 6 channel-inherit experiment.
Run these commands to restore the original state if the path is abandoned.

## Baseline (before any changes)

```
/root/slmos.elf                  — md5: fa7c12ab9fa2fc9f098ea626b8293939
/usr/local/bin/slmos-kexec       — md5: 0c433f3331eb21661370496a78f5df00
```

## Files to remove on rollback

```bash
# Channel helper binary (compiled on Jetson)
rm -f /root/gpu-channel-helper
rm -f /root/gpu-channel-helper.c

# Updated kexec helper (restore original from this repo's main branch)
# scp scripts/jetson-kexec-slmos.sh root@192.168.4.93:/usr/local/bin/slmos-kexec

# Handoff block in DRAM (volatile — cleared on reboot, no action needed)
```

## How to verify clean state

```bash
ssh root@192.168.4.93 'ls /root/gpu-channel-helper* 2>/dev/null && echo "NOT CLEAN" || echo "CLEAN"'
```
