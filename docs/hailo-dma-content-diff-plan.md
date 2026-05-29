# Hailo DMA buffer content diff plan (#1001 follow-up)

**Status (2026-05-28):** EXECUTED. SLM-OS side landed in PR
[#1010](https://github.com/SLM-OS/SLM-Operating-System/pull/1010); the
matching Linux `hailo_pci` patch landed cross-side during the same session.
Capture ran 2026-05-27 on `pi-5-1` against HailoRT reference. Outcome:
**CCWS payload + boundary IN/OUT descriptor-list payloads byte-faithful to
HailoRT, wedge persists.** This was THE remaining host-observable test
(see §"Definitive-ness of the result" below) — its conclusion exhausts
the host-side surface area, and
[#1001](https://github.com/SLM-OS/SLM-Operating-System/issues/1001) was
closed 2026-05-28 on that basis. The plan below is retained as
historical reference and as a template for any future DMA-content
investigation.

## Why

Every BAR0/BAR2/BAR4 MMIO write SLM-OS issues during MNIST load + first-frame
inference is byte-faithful to HailoRT v4.23 on Pi 5 (see PR #999 + #1004 +
#1008 chain and the per-byte BAR2 trace comparison in
`~/slmos-ref/derivatives/slmos-traces/`). HailoRT runs MNIST end-to-end at
~11k FPS on this exact silicon; SLM-OS wedges at `ch=2 num_proc=0` with all
8 IN descriptor `status` bytes untouched (sentinel probe 2026-05-27 proved
fw never writes them).

The IOVA reachability hypothesis is also disconfirmed: fw DOES write to our
0x100xxx IOVA range for cfg-channel CCW upload (`ch=0 proc=0x006e006d`
post-load). So the wedge is at fw decision level, not transport level.

What remains untraced is **the content of host RAM that fw reads via DMA**.
The wire bytes that program WHERE to read are equal between sides. The
bytes fw actually reads from those addresses have never been compared.

Two distinct content regions are candidates:

1. **CCWS data** at IOVAs the cfg-channel descriptors point to. PR #1007's
   `hailo_ccw_dump_fingerprint` exists but dumps only the first 64 B and
   only one action at a time. We need full content dumps for byte-diff.
2. **Boundary descriptor list bytes** (per-descriptor: `page_size_desc_control`,
   `addr_l_rsvd_data_id`, `addr_h`, `remaining_page_size_status`). 16 bytes
   × 8 IN descs + 16 × 8 OUT descs = 256 bytes. Already dumpable via
   `hailo_vdma_dump_desc` (the variant that shows all four fields per desc,
   not the `_status` one), but not in a parser-friendly format.
3. **CCWS desc list bytes** — similar shape, for ch=0/ch=1 (cfg) channels.

Boundary INPUT DMA buffer content itself (the 784-byte input image) is
zeros for `model infer mnist`, so a byte-diff against Linux only matters
if Linux differs from "zeros" in any subtle way (alignment, padding).
Probably not load-bearing but cheap to include.

## What to dump

### SLM-OS side

Single new shell command `hailo dma-dump` that runs immediately after
`hailo load /mnt/files/user.hef sched` and before `hailo runmodel 1`. It
prints to UART, one region per logical artifact:

```
[dma-dump:ccws_block] iova=0x... size=112256
  0x00000000: 00 00 00 00 84 06 20 00 00 00 00 01 00 00 00 00
  0x00000010: ...
  ... full hex dump, 16 B per line ...

[dma-dump:cfg_ch0_desc_list] iova=0x... count=8 stride=16
  desc[0]: page_size_desc_control=0x... addr_l_rsvd_data_id=0x... addr_h=0x... rps_full=0x...
  desc[1]: ...

[dma-dump:cfg_ch1_desc_list] ...
[dma-dump:boundary_in_desc_list] ...
[dma-dump:boundary_out_desc_list] ...

[dma-dump:boundary_in_buf] iova=0x... size=784
  0x00000000: 00 00 00 00 ...
  ...
[dma-dump:boundary_out_buf] iova=0x... size=10
  0x00000000: 00 00 00 00 ...
```

UART throughput at 115200 baud is ~11 KB/s. Total dump volume:
- CCWS block: 112256 B → 7016 lines × 60 chars ≈ 420 KB → ~40 s
- Descriptor lists: 8 descs × 4 = 32 descs × ~80 chars = ~2.5 KB → <1 s
- Boundary buffers: 784 + 10 B → ~50 lines → <1 s

CCWS dominates. Either capture full (accept 40 s pause) or sample (first
+ last + every Nth byte). Recommend full dump given this runs once per
investigation session.

A `hailo dma-snap-iova <iova> <len>` variant for ad-hoc dumps is also
worth having.

### Linux side

`hailo_pci` doesn't expose host RAM by default. The PR #997 framework
adds an in-source trace hook (NOT a kprobe — the v2 patch is a kbuild-
applied source patch under DKMS).

**Drafted approach: extend the v2 trace-instrumentation patch with a
sysfs `dma_dump` trigger.** Patch saved at
`~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-dma-dump.patch`
(2026-05-27). On `echo 1 > /sys/module/hailo_pci/parameters/dma_dump`
the module walks every open file context's `descriptors_buffer_list`
and `mapped_user_buffer_list`, emitting `hailo-trc: [dma-dump:...]`
lines via `pr_info`. Capture goes to dmesg, not UART — so unlike the
SLM-OS side the full 112 KB CCWS dump takes <100 ms.

Companion deploy script at
`~/slmos-ref/derivatives/hailort-traces/hailort-v4.23.0-dma-dump-capture.sh`
covers patch apply + DKMS rebuild + dump trigger + dmesg capture in
one run.

**Status (originally drafted): draft skeleton, deploy-pending.** Several field names
(`hailo_pcie_board.vdma.controller.file_context_list`,
`hailo_descriptors_list_buffer.dma_address`, the per-fd context
tracking inside the controller struct) are inferred from the .c files
in `~/slmos-ref/hailo/v4.23.0/linux/` since the corresponding headers
aren't in the reference cache. Expect 1-2 edit-recompile cycles on
the Pi 5 itself to validate struct field names before first successful
dump.

Alternative (deferred): user-space `LD_PRELOAD` shim that hooks the
HAILO_VDMA_LAUNCH_TRANSFER ioctl in HailoRT. Avoids kernel module
work entirely but requires reverse-engineering the ioctl arg layout
to find buffer pointers. Considered, not pursued — the kernel patch
path is more robust given we already have the v2 instrumentation
framework working.

## Hex format

The two sides MUST emit byte-for-byte identical hex format so a literal
`diff` is meaningful. Suggested:

```
[dma-dump:<region_name>] iova=0x<hex> size=<dec>
0x<offset_hex_8>: bb bb bb bb bb bb bb bb bb bb bb bb bb bb bb bb
```

- One leading line per region with region-name + iova + size.
- Hex bytes in lowercase, single-space separated, 16 per line.
- No ASCII column (collides with multi-byte UART newline handling on
  Pi 5; we've seen `\r\r\n` artifacts in serial captures).
- Trailing newline at end of region.

## Capture procedure

1. Build SLM-OS with the `hailo dma-dump` command, deploy to pi-5-1
   (SLMOS card). Run `hailo probe → boot → load → dma-dump`. Save UART
   capture to `~/slmos-ref/derivatives/slmos-traces/slmos-dma-dump-mnist-<date>.txt`.
2. Build patched `hailo_pci.ko` with debugfs `dma_dump` entry. Swap to
   Pi OS card. Run `hailortcli run /tmp/mnist.hef -c 1` and after the
   load phase trigger the dump via debugfs. Save dmesg capture to
   `~/slmos-ref/derivatives/slmos-traces/hailort-linux-dma-dump-mnist-<date>.log`.
3. Extract region-by-region hex from both captures (Python helper —
   one input per side, output one file per region). Format identical.
4. `diff -u` each pair. Expected outcomes:
   - **All regions identical → no DMA-content divergence; wedge cause
     is even deeper inside fw than we can probe from host.**
   - **A region differs → starting point for a fix.** Especially
     interesting differences:
     - CCWS bytes differ → HEF parser is emitting different bytes than
       HailoRT (similar shape to the original #1005 misread, but now
       provable).
     - Descriptor list bytes differ → SLM-OS programs descriptors
       differently. The 4-field-per-desc dump should make this jump
       out.
     - Boundary IN/OUT buffer differs → initial-state differs.

## Effort estimate

- SLM-OS `hailo dma-dump` command: small (~2 hours). Reuses existing
  desc list pointers and CCWS buffer reference; just need format +
  hex helper + shell wiring.
- Linux-side `hailo_pci` patch + debugfs entry: moderate (~half day to
  a day). Custom module build + Pi OS deploy is well-trodden via
  PR #997's framework.
- Diff procedure + Python helper: small (~1 hour).

Total: ~1-2 sessions to execute end-to-end.

## Definitive-ness of the result

This is THE remaining host-observable test. If all DMA buffer contents
match byte-for-byte and the wedge still reproduces, host-side investigation
is mathematically exhausted (every byte SLM-OS hands to fw is identical to
HailoRT's). At that point the only remaining directions are:

1. **fw memory inspection** — BAR4 region content dumps pre/post-load,
   looking for fw-internal state divergence. The PR #997 framework
   covers BAR0/BAR2; need to extend it to BAR4 windowed reads.
2. **Hailo support escalation** — present the byte-faithful evidence
   and request fw-side debug.
3. **Capstone writeup** — accept the chain as complete; the limit is
   in the closed firmware.

## Cross-references

- PR #999: channel-index direction fix (the only landed fix in #682 chain)
- PR #1004: load-time RPC byte-faithfulness
- PR #1007: CCW fingerprint dump (template for `hailo dma-dump`)
- PR #1008: CCW total_bytes display bug fix + modern Op-graph walker
- Memory `hailo_682_root_cause_channel_index`: full disconfirmation chain
- Memory `hailo_trace_toolkit`: PRs #780/#783 trace framework
- Memory `hailo_re_corpus_bar_coverage`: structural BAR coverage limit
- Sentinel-probe trace (2026-05-27): proves fw never writes to IN desc
  list — the wedge is at fw decision level, not PCIe transport
