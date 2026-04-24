# Incremental num_avail bump — test result (2026-04-22)

Branch: `phase-8-incremental-num-avail`. Built with `AI_SCHED=ON HAILO_FW_BLOB=... USER_HEF_BLOB=build/hailo/mnist.hef`. Deployed to `pi-5-1`, ran `hailo runmodel 1 1`.

## Hypothesis under test

From `hailort-trace-findings-2026-04-22.md`: HailoRT issues **8 separate launch_transfer calls per output channel** before the input submit, each writing num_avail = current+1 (so fw sees `1, 2, 3, ..., 8` as eight distinct MMIO writes, not a single 0→8 jump). SLM-OS pre-PR was writing num_avail once. Hypothesis: fw's boundary-credit state machine recognizes incremental bumps but not multi-credit jumps.

## Code change

`kernel/inference/inference_device_hailo.c` `hailo_backend_run`: replaced the single `hailo_vdma_write_num_avail(out_channel, 1)` (and the desc-fill loop that ran before it) with one loop:

```c
for (uint16_t i = 0; i < HAILO_BOUNDARY_OUT_PREFETCH_DEPTH; i++) {
    if (i > 0) {
        hailo_vdma_program_buffer(&slot->boundary_out_list, i, ...);
    }
    hailo_vdma_write_num_avail(out_channel, (uint16_t)(i + 1));
}
```

8 iterations, each one programs OUT[i] then bumps num_avail by 1.

## Observed result

The bumps fire correctly:

```
[vdma] ch=16 write_avail host_off=0x10 pre=0x00002801 post=0x00012801 avail=1
[vdma] ch=16 write_avail host_off=0x10 pre=0x00012801 post=0x00022801 avail=2
[vdma] ch=16 write_avail host_off=0x10 pre=0x00022801 post=0x00032801 avail=3
[vdma] ch=16 write_avail host_off=0x10 pre=0x00032801 post=0x00042801 avail=4
[vdma] ch=16 write_avail host_off=0x10 pre=0x00042801 post=0x00052801 avail=5
[vdma] ch=16 write_avail host_off=0x10 pre=0x00052801 post=0x00062801 avail=6
[vdma] ch=16 write_avail host_off=0x10 pre=0x00062801 post=0x00072801 avail=7
[vdma] ch=16 write_avail host_off=0x10 pre=0x00072801 post=0x00082801 avail=8
[vdma] ch=2 new_avail=2 base_pre=0x00002801 base_post=0x00022801 proc_pre=0x00000000
```

Then INPUT submit on ch=2 with avail=2. **Fw never moves num_proc on either channel.** Same 500 ms timeout symptom as before:

```
[vdma-poll] ch=2 t=499900 us heartbeat host(proc=0x..0 base=0x..22801) dev(proc=0x..0 base=0x..2c01)
[vdma] ch=2 TIMEOUT proc_end=0x00000000 base_end=0x00022801
```

`hailo last_err` shows no fw-side error reported (major=0, minor=0).

## Conclusion

**Headline hypothesis was wrong.** The incremental num_avail bump pattern matches HailoRT exactly at the MMIO layer, including:
- 8 distinct RMW writes to the channel BASE_DWORD
- Correct host-side offset (+0x10 for ch=16 D2H)
- Correct increment-by-one progression
- Correct read-back of the previous value before each RMW

Fw is still silent. Every signal we previously had (no fw error, no notification, no num_proc movement) is unchanged.

This conclusively rules out "fw needs to see incremental num_avail" as the bug. The behavior gap is somewhere else.

## Status of the change

The incremental loop is structurally identical to HailoRT's pattern, so leaving it in keeps the SLM-OS code closer to the reference for future comparisons. Reverting to the single-bump version would not help and would add a known divergence back.

## Next probes (in suggested order)

1. **Cache-clean verification** — read back our IN desc list IOVA from CPU after cache_clean to confirm it actually flushed. If we see different bytes than what we think we wrote, that's the bug.
2. **PCIe RC inbound translation policy** — audit BCM2712 `pcie1` ATU/ATR settings against Linux's pcie-brcmstb (BAR vs system memory translation; MPS/MRRS).
3. **The userspace HailoRT IOCTLs we don't replicate** — INTERRUPTS_WAIT and BUFFER_SYNC are the most-suspicious. INTERRUPTS_WAIT specifically might do per-channel arming inside the kernel that fw expects to see (registering a waiter on a specific channel).
4. **Linux userspace shim** — port SLM-OS Hailo backend to compile on Linux + UIO Hailo, A/B test on real hardware. The variant from the previous proposal that becomes more attractive after this negative result.

## Where the code lives

- Branch: `phase-8-incremental-num-avail` (uncommitted on top of `main`)
- Single behavioral file changed: `kernel/inference/inference_device_hailo.c` (the OUT pre-arm loop in `hailo_backend_run`)
- New constants in same file: `HAILO_BOUNDARY_OUT_PREFETCH_DEPTH = 8u`
