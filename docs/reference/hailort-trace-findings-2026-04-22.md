# Pi OS instrumented HailoRT trace findings — 2026-04-22

Captured a single yolov6n inference on `pi-5-1` Pi OS with hailo_pci v4.23 instrumented to log:
- Every MMIO RD32/WR32 (gated by `trace_mmio` module param)
- Every IOCTL entry from userspace (gated by `trace_ioctl` module param)
- Per-channel SRC/DST IRQ bitmaps + raw ISTATUS_HOST (already present from prior session)
- launch_transfer entry params per submit

Files:
- Trace: `docs/reference/hailort-v4.23.0-full-trace-yolov6n-pi5.txt` (4893 lines, 2916 hailo-trc events for one inference)
- Patch: `docs/reference/hailort-v4.23.0-trace-instrumentation.patch` (reproducible)

## Headline finding: HailoRT bumps `num_avail` ONCE PER LAUNCH, not once per inference

For yolov6n the inference structure is:

```
for output_channel in [16,17,18,19,20,21,22,23,24]:    # 9 output streams
    for i in range(8):
        launch_transfer(channel=ch, starting_desc=i * <stride>, buffers=1, ...)
INTERRUPTS_WAIT(...)                                   # arm a kernel wait
launch_transfer(channel=2, starting_desc=0, buffers=1, ...)   # input submit
INTERRUPTS_WAIT(...)                                   # block for completions
INTERRUPTS_WAIT(...)
launch_transfer(channel=16, starting_desc=...)         # incremental refill
...
```

That's **82 launch_transfer calls per inference** for yolov6n: 1 input on ch=2 + 9 outputs × 9 launches each (8 pre-fill + 1 refill).

Each `launch_transfer` call internally:
1. Updates ONLY the last descriptor's `PageSize_DescControl` with IRQ bits (`should_bind=0` confirms no re-binding).
2. Computes `new_num_avail = (last_desc + 1) % desc_count`.
3. Writes that value to the channel's `BASE_DWORD` via RMW (`hailo_vdma_set_num_avail`).
4. Updates host-side `ongoing_transfer` bookkeeping.

So for an output channel with 8 pre-fill launches, fw sees `num_avail` tick 1, 2, 3, 4, 5, 6, 7, 8 across 8 separate MMIO writes — not a single jump from 0 → 8.

**SLM-OS today writes num_avail once with the final value.** PR #350's pre-fill loop populates desc[1..7] via `program_buffer` and then writes num_avail = 1 once. If firmware's boundary-credit state machine needs to *observe* num_avail incrementing one credit at a time to advance its internal counters, that's the gap — and it would explain why fw's `num_proc` never moves on our boundary submits.

**Hypothesis to test (highest-leverage probe):** in `hailo_backend_run`, replace the single `hailo_vdma_write_num_avail(out_channel, 1)` with a loop that calls it 8 times bumping by 1 each time, with the corresponding desc re-program before each. If `num_proc` advances after this change, we've found the mechanism. If not, the bug is elsewhere (likely in INTERRUPTS_WAIT-equivalent semantics or BUFFER_SYNC).

## Secondary findings

### IOCTL distribution (per single yolov6n inference)

| Count | IOCTL                          | Notes                                                  |
|-------|--------------------------------|--------------------------------------------------------|
| 82    | HAILO_VDMA_LAUNCH_TRANSFER     | One per descriptor batch — see headline finding.       |
| 27    | HAILO_FW_CONTROL               | Firmware control RPCs. SLM-OS does these in-kernel.    |
| 20    | HAILO_VDMA_BUFFER_MAP          | Maps user buffers — SLM-OS does inline.                |
| 20    | HAILO_VDMA_BUFFER_UNMAP        |                                                         |
| 16    | HAILO_DESC_LIST_PROGRAM        | Programs descriptors before launch.                    |
| 12    | HAILO_DESC_LIST_RELEASE        |                                                         |
| 12    | HAILO_DESC_LIST_CREATE         |                                                         |
| 9     | HAILO_VDMA_INTERRUPTS_WAIT     | Userspace blocks in kernel waiting for VDMA IRQs.      |
| 4     | HAILO_VDMA_BUFFER_SYNC         | Cache sync — only 4 calls for 9 outputs.               |
| 2     | HAILO_MARK_AS_IN_USE           |                                                         |
| 1     | HAILO_VDMA_DISABLE_CHANNELS    |                                                         |
| 1     | HAILO_VDMA_ENABLE_CHANNELS     |                                                         |
| 1     | HAILO_READ_NOTIFICATION (NNC)  | One D2H notification at startup.                       |
| 1     | HAILO_DISABLE_NOTIFICATION (NNC) |                                                       |
| 1     | HAILO_QUERY_DRIVER_INFO        |                                                         |
| 1     | HAILO_QUERY_DEVICE_PROPERTIES  |                                                         |

### IRQ patterns

ISTATUS_HOST = `0x04800000` fired 27 times — bit 26 (FW_CONTROL_DONE) + bit 23 (one of the VDMA aggregate bits). The 27 hits matches the 27 FW_CONTROL IOCTLs exactly: every FW control RPC produces one IRQ.

Per-channel DST_IRQ bitmaps fire individually: `0x00010000` (ch 16), `0x00020000` (ch 17), ..., `0x01000000` (ch 24) — confirming each output channel signals its own completion IRQ rather than coalescing.

### Channel layout (yolov6n_h8l)

- ch=2: input (1 launch per inference, desc_count=32768)
- ch=16..24: outputs (9 streams, 9 launches each per inference, desc_count varies 512–8192 by stream)

## Source layout reminders

The instrumentation patch lives in `docs/reference/hailort-v4.23.0-trace-instrumentation.patch`. To re-apply on a fresh Pi OS install:

```bash
# Pi OS side
sudo cp -an /usr/src/hailort-pcie-driver /usr/src/hailort-pcie-driver.bak.pre-trace
cd /usr/src/hailort-pcie-driver
sudo patch -p1 < /tmp/hailort-v4.23.0-trace-instrumentation.patch
cd linux/pcie && sudo make all && sudo make install
sudo modprobe -r hailo_pci && sudo modprobe hailo_pci
# Toggle at runtime:
echo 1 | sudo tee /sys/module/hailo_pci/parameters/trace_mmio
echo 1 | sudo tee /sys/module/hailo_pci/parameters/trace_ioctl
```

## Next steps (recommended order)

1. **Test the "8 separate num_avail bumps" hypothesis on SLM-OS.** Smallest possible code change to `hailo_backend_run`:
   - Replace single `hailo_vdma_write_num_avail(out_channel, 1)` with a loop that calls 8 separate `launch_transfer`-equivalent operations, each bumping num_avail by 1.
   - Re-program desc[i] just before bumping num_avail to i+1.
   - Same for INPUT.
2. If that fixes it: ship the fix, study the WAIT/SYNC patterns later for completeness.
3. If that doesn't: capture matching SLM-OS trace under `HAILO_WIRE_DEBUG`, diff per-MMIO against this Pi OS trace, look for the FIRST divergent register write.
