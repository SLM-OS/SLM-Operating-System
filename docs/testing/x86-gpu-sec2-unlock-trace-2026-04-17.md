# SEC2 Unlock Mechanism on GA107 — nouveau mmiotrace + bpftrace Investigation

**Date:** 2026-04-17
**Hardware:** test-pc (Gigabyte H610M S2H V2, i7-6700, 16 GB, GA107 RTX 3050 6GB)
**Kernel:** Ubuntu 24.04 / 6.17.0-20-generic, `CONFIG_MMIOTRACE=y`
**Related issue:** #185 (SEC2 priv-lock blocks GSP Booter Load on x86-64)
**Related finding:** Jetson Orin Nano (2026-04-17) — `--no-gpu-suspend` prevents Falcon BROM from reasserting priv-lock bit 13 across power-gate

---

## 1. Hypothesis under test

Going in: nouveau's `modprobe` runs a narrow sequence of CPU-side MMIO writes
(a handful of register writes, perhaps a VBIOS DEVINIT replay) that transitions
SEC2 CPUCTL from the priv-locked sentinel `0xbadf5620` to the accessible
`0x20` (HALTED) state. If true, the recipe could be ported to SLM-OS's
`gpu init` path and unblock Booter Load (#185).

Jetson's 2026-04-17 finding (nvgpu clears Falcon priv-lock bit during its
re-enable path; a custom re-enable path that doesn't replicate that write
leaves the lock asserted) was the motivating analogue — a narrow register
write unblocks the whole subsystem.

## 2. Method

Four increasingly refined passes on test-pc (see `scripts/x86-gpu-trace/`):

| Script | Strategy | Outcome |
|---|---|---|
| `capture.sh` | Pre/post CPUCTL snapshot, `mmiotrace` from modprobe→ +5s | Buffer overflowed (32k / 4.3M events retained). Trace held only post-init polling. |
| `capture_v3.sh` | Bigger buffer (256 MB), shorter window | Same overflow; kept 756k / 4.3M. Only 288 W + 672 R events retained, all post-init. |
| `capture_v4.sh` | Stop tracing at first `nouveau.*GA107` dmesg line | Captured first 17k events (1 W, 1 MAP, 17286 R). Unlock happens much later. |
| `capture_v5.sh` | Stop tracing at `nouveau.*fb: N MiB` dmesg line (late probe); probe CPUCTL continuously from userspace | 142k retained events; **zero writes to SEC2 aperture 0x5384xxxx**. CPUCTL transition sampled post-trace. |
| `kprobe_acr.sh` | bpftrace kprobe on `nvkm_acr_init`, `ga102_acr_load`, `nouveau_run_vbios_init`, etc. | **None of the ACR probes fired** during nouveau load, indicating GA10x doesn't go through the legacy ACR codepath. CPUCTL sampler captured the unlock moment with sub-ms resolution. |

SEC2 CPUCTL was read via a custom `/dev/mem` mmap helper (`sec2_peek.c`) at
BAR0+0x840100.

## 3. Raw observations

### 3.1 CPUCTL transition timeline (from `kprobe_acr.sh` sampler)

```
ts (ns, epoch)           value     note
1776454461907560537      badf5620  first sample (nouveau not yet loaded)
...(many samples)...
1776454465343376025      00000000  FIRST UNLOCK — ~3.44 s into sampling
1776454465357061596      00000010  (HALT bit)
1776454465360072222      00000000
1776454465363099842      00000020  (HALTED bit)
1776454465396466181      00000000
1776454465402148188      00000020
1776454465524821589      00000000
1776454465527560551      00000020
1776454465602128417      00000000
```

The rapid 0x00 ↔ 0x10 ↔ 0x20 oscillation after unlock is normal Falcon
BROM lifecycle — the CPU halts itself, BROM runs, halts again, etc.

### 3.2 mmiotrace access patterns (v5, 142k events retained)

No writes or reads in any of these apertures during the captured window:

- SEC2 aperture (BAR0 + 0x84xxxx)
- GSP aperture (BAR0 + 0x11xxxx)
- PMC device-enable / engine-reset (BAR0 + 0x000xxx, 0x006xxx)

The bulk of retained events were `UNKNOWN` opcodes (ioread_rep /
iowrite_rep bursts) concentrated in BAR1 (0x40xxxxxx–0x4Fxxxxxx), which is
the BAR1 PRAMIN / instance-memory aperture used for bulk transfers to VRAM.

### 3.3 dmesg timeline (representative, kprobe_acr.sh run)

```
[96.239] nouveau 0000:01:00.0: NVIDIA GA107 (b77000a1)
[96.337] nouveau 0000:01:00.0: bios: version 94.07.a0.00.4c
[96.855] nouveau 0000:01:00.0: vgaarb: deactivate vga console
[96.855] nouveau 0000:01:00.0: fb: 6144 MiB GDDR6
[96.904] nouveau 0000:01:00.0: drm: VRAM: 6144 MiB
[96.907] [drm] Initialized nouveau 1.4.0 for 0000:01:00.0 on minor 0
```

Nouveau's entire init sequence was under 700 ms. No ACR/SEC2/GSP debug
lines appeared, even at max `debug=all=debug`.

### 3.4 bpftrace probes

18 kprobes attached, zero events fired. Probed functions included
`nvkm_acr_init`, `nvkm_acr_oneinit`, `nvkm_acr_load`, `ga102_acr_load`,
`tu102_acr_load`, `ga102_acr_wpr_build`, `nouveau_run_vbios_init`,
`nvkm_device_pci_preinit`. **None were called.** This is the most
informative negative result of the whole investigation.

## 4. Interpretation

### 4.1 The initial hypothesis is falsified.

Nouveau does not issue CPU MMIO writes to the SEC2 register aperture
(directly or indirectly) to unlock SEC2. The unlock is not a sequence
of CPU register writes — there is no recipe to port.

### 4.2 The unlock is performed by on-GPU firmware, not the CPU.

GA10x uses GSP-RM, not the legacy ACR. The sequence is:

1. **VBIOS FWSEC-FRTS (Phase 1)** — FWSEC microcode runs on the GSP
   Falcon at GSP-level privilege. FWSEC establishes the FRTS (Falcon
   Runtime Services) region and bootstraps basic Falcon management.
2. **Booter Load (Phase 2)** — a signed blob runs on SEC2 to
   authenticate the larger GSP-RM firmware. For this step to succeed,
   SEC2 must already be accessible.
3. **GSP-RM (Phase 3)** — RISC-V GSP boots and thereafter manages all
   other Falcons (PMU, SEC2, NVDEC, etc.).

SEC2 becomes accessible as a side effect of FWSEC-FRTS completing
successfully — FWSEC writes to SEC2's priv-mask registers *from within
the GPU*, with GSP-level privilege that the CPU lacks.

### 4.3 FWSEC invocation diff — what we thought vs what it is.

Initial hypothesis: nouveau runs a second FWSEC invocation (FWSEC-SB,
cmd=0x19) during init that SLM-OS omits, and SB is what clears the
SEC2 priv-lock.

**Source diff on 2026-04-17 (this session) rules that out.** Both
`nvkm_gsp_fwsec_frts` and `nvkm_gsp_fwsec_sb` exist in
`drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c`, but `_sb` is called
**only from `tu102_gsp_fini`** (suspend/shutdown path), never during
init. Init path for GA10x is: `tu102_gsp_oneinit` calls
`nvkm_gsp_fwsec_frts` (cmd=FRTS=0x15), then resets GSP to RISC-V
mode, then `tu102_gsp_init` calls `tu102_gsp_booter_load` on SEC2.

SLM-OS's `gsp_bringup_fwsec_frts` uses the same FRTS command (0x15)
and the same DMEMMAPPER patching sequence. The FWSEC command
invocation is **not** the delta.

### 4.4 Real candidate deltas (from source diff, not confirmed on hardware).

- **SEC2 Falcon reset skipped.** Nouveau's `ga102_sec2_flcn` has
  `reset_pmc = true`, so `gm200_flcn_enable` calls `nvkm_mc_enable`
  which writes `NV_PMC_ENABLE` to toggle the SEC2 engine. It also
  runs `ga102_flcn_select` (checks/clears 0x1668 bit 4) and
  `ga102_flcn_reset_prep` (reads 0x0f4, polls bit 31). SLM-OS's
  `falcon_reset` is a bare `FALCON_ENGINE.RESET` + `MEM_SCRUBBING`
  poll — missing select, reset_prep, PMC toggle, interrupt disable,
  and the `BOOT_0` → `0x084` copy. Worse: when SEC2 is priv-locked
  (`0xbadf5620`), SLM-OS's `gsp_bringup_booter_load` phase 5
  **skips falcon_reset entirely**.

- **PIO IMEM upload misses per-block tag writes.** Nouveau's
  `gm200_flcn_pio_imem_wr` writes IMEMT once per 256-byte chunk with
  an incrementing tag (`pio->max = 0x100`, `pio->wr(... tag++)` per
  chunk). SLM-OS's `falcon_pio_upload_imem` writes IMEMT once and
  streams all u32s, so blocks past the first get the wrong
  PC-to-page mapping. For the booter blob (NS IMEM ≈ 20 KB, secure
  IMEM ≈ 30–40 KB — many blocks) this would produce a corrupted
  instruction stream that fails BROM signature verification. Note:
  SEC2 priv-lock masks this as the primary failure; the PIO bug
  would surface the moment the lock is cleared.

- **DMA vs PIO.** GA10x uses `ga102_flcn_fw` (DMA-based load) for
  the booter on nouveau, not `gm200_flcn_fw` (PIO-based). Our code
  comment cites `gm200_flcn_fw` — but on GA10x, DMA is the
  production path and the one exercised by NVIDIA firmware team.

### 4.5 The deeper question — what actually unlocks SEC2.

`kprobe_acr.sh`'s zero-event result remains the strongest signal.
No ACR or FWSEC-SB function fires during nouveau load on GA10x.
The CPUCTL transition is sampled ~3.4 s after modprobe starts —
well after the init chain reports complete in dmesg (fbcon up at
~700 ms).

This strongly suggests a **deferred workqueue** or DRM-init
trigger clears the lock after probe returns. Without attaching
probes to a wider set of nouveau functions or walking the
post-probe code paths, the exact trigger can't be pinpointed from
the existing captures.

### 4.4 The Jetson analogy holds partially.

Jetson's fix is about preventing the Falcon BROM from reasserting the
priv-lock bit after a power-gate. On x86-64, UEFI POST's VBIOS DEVINIT
leaves SEC2 priv-locked by design (for VFIO safety), and no power-gate
happens during SLM-OS boot. The CPU register write Linux issues to
clear bit 13 on Jetson does not have an x86-64 counterpart — because
on x86-64 the CPU cannot clear the bit; only GPU firmware can.

The analogy that does hold: **Linux unlocks SEC2 via a mechanism that
a custom bringup can miss.** On Jetson the mechanism is a single CPU
write (portable). On x86-64 the mechanism is FWSEC-FRTS used
correctly (not portable as a register recipe — requires matching the
firmware command interface nouveau uses).

## 5. Impact on the x86-64 SEC2 unlock strategy

### 5.1 Ruled out

- **Short CPU-side register recipe** — no such recipe exists. ✗
- **VBIOS DEVINIT bytecode replay from SLM-OS.** The script's
  SEC2-relevant effect is triggering FWSEC — which we already run
  correctly. ✗
- **FWSEC-SB (cmd=0x19) as the missing init step.** Reference shows
  `nvkm_gsp_fwsec_sb` is only called from `tu102_gsp_fini`. ✗

### 5.2 Still open (but not obviously tractable)

Even if the real trigger were identified, SLM-OS still has two
GA10x-specific Booter Load bugs that will surface the moment SEC2 is
unlocked:

1. `falcon_reset` is missing the SEC2-specific reset_pmc / select /
   reset_prep steps nouveau runs via `gm200_flcn_enable`.
2. `falcon_pio_upload_imem` doesn't write IMEMT per 256-byte block,
   which would corrupt the booter's secure IMEM tag layout.

These are fixable on their own (a few hours of C), but fixing them
only matters after SEC2 is actually accessible.

### 5.3 Most viable path

- **Linux→SLM-OS kexec.** Boot to Ubuntu, let nouveau probe and
  whatever deferred work unlocks SEC2 run, then kexec to SLM-OS with
  the GPU still powered. Jetson's `--no-gpu-suspend` finding confirms
  the only constraint. This inherits an *already-unlocked* SEC2 and
  sidesteps every issue in §4 and §5.2. Fastest demonstration path.

## 6. Recommended next steps

1. **Implement Linux→SLM-OS kexec handoff** on test-pc, reusing the
   Jetson `slmos-kexec` pattern. Preserve GPU power across the
   transition. Validate SEC2 CPUCTL reads `0x20` from SLM-OS shell
   immediately after kexec — that alone unblocks E3.4 Booter Load.

2. **Fix the two Falcon bugs independently** (`falcon_reset` +
   `falcon_pio_upload_imem` per-block IMEMT) so that once SEC2 is
   unlocked (by whatever means), the rest of the booter path is
   correct. File as separate issues; they're testable in isolation.

3. **If curiosity outweighs schedule pressure**: rerun the mmiotrace
   with a bpftrace that probes `nouveau_drm_*`, `r535_gsp_init`,
   `r535_gsp_postinit`, and anything with "fwsec" or "booter" in the
   name. The zero-event result from the ACR probes pinpointed that
   the unlock isn't ACR-mediated; widening the probe set should
   pinpoint the function that triggers it.

## 7. Artifacts

All under `scripts/x86-gpu-trace/`:

- `sec2_peek.c` — minimal `/dev/mem` BAR0 reader
- `capture.sh`, `capture_v3.sh`, `capture_v4.sh`, `capture_v5.sh` —
  evolution of the mmiotrace capture logic
- `kprobe_acr.sh` — bpftrace ACR-probing + sub-ms CPUCTL sampler
- `nouveau-mmio.trace`, `nouveau-mmio-v3.trace` — raw mmiotrace output
- `sec2-pre-nouveau.txt` — SEC2/GSP registers pre-nouveau
- `sec2-post-nouveau-acr.txt` — SEC2/GSP registers post-nouveau
- `sec2-probe-acr.log` — timestamped CPUCTL samples spanning the unlock
- `nouveau-dmesg-acr.txt` — dmesg during the final run
- `bpftrace-acr.out` — bpftrace attach confirmation (zero events)
