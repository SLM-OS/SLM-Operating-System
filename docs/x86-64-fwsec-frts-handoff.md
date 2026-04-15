# E3.4 FWSEC-FRTS hardware completion — handoff doc

**Last updated:** 2026-04-15 (session 2 — probes A, 4.1, 4.2, 4.3 done; new hypothesis: missing DEVINIT)
**Owner role:** open
**Tracking issue:** [#27](https://github.com/johnjezl/CS-496-Capstone-SLM-Operating-System/issues/27)
**Companion docs:**
- `docs/x86-64-capstone-gap-closure-plan.md` §E3 — overall plan
- `docs/reference/nvidia-gsp-bringup-sequence.md` — register map + Ampere bringup theory
- `docs/reference/nouveau-gsp-fwsec.c` — nouveau's reference implementation
- `docs/x86-64-gsp-fwsec-investigation.md` — earlier FWSEC discovery work (E2.5)

This document is self-contained: if you're a new agent (or human)
picking up E3.4, read this end-to-end and you should be able to make
the next probe without re-deriving the context.

---

## 0. Session-2 findings (2026-04-15, post-PR-#181)

Four probes from §4 executed. None made FWSEC succeed, but they
disambiguate the failure mode significantly.

### 0.1 Probe A (§4.1) — init_cmd = SB instead of FRTS

Added `--fwsec-sb` to the harness; runs FWSEC with `init_cmd=0x19`
(subsequent boot — no WPR2 setup, no frts_region sub-struct) instead
of FRTS (`0x15`). Result: **hangs identically**. Same
`DEBUGINFO=0xda550000`, `MAILBOX0` cleared from sentinel,
`CPUCTL=0x00000000` throughout the halt poll.

**Conclusion:** the hang is **upstream of the init_cmd dispatch** —
in FWSEC's common prologue that runs before both FRTS and SB paths.
The only code that runs in both is the `read_vbios` sub-struct
processing.

### 0.2 Probe 4.3 — ROM BAR state under VFIO

`lspci -vvv -s 01:00.0` reports `Expansion ROM at 54000000
[disabled] [size=512K]`. Initially flagged as a candidate root cause
but disproved on further reading:

- `read_vbios.flags=2` means **use BAR0+0x300000 PROM window**
  (NV_PROM_DATA), NOT the PCI Expansion ROM BAR. Confirmed against
  `docs/reference/nvidia-vbios-bar0-prom-access.md` and NVIDIA's
  `kgspExtractVbiosFromRom_TU102` in
  `docs/reference/nvidia-openrm-595-kernel-gsp-vbios-tu102.c:59-85`.
- FWSEC never touches the Expansion ROM BAR. nouveau doesn't enable
  it either.

**Conclusion:** disabled ROM BAR is a red herring.

### 0.3 Probe 4.2 — DEBUGINFO=0xda550000 time series

Added post-timeout sampling to `--fwsec-frts` (100 ms × 10 samples).
Observed on test-pc with `FALCON_HALT_TIMEOUT_US` set to both 2 s
and 5 s:

```
GSP Falcon state at timeout:
  CPUCTL=0x00000000   (NOT halted)
  DEBUGINFO=0xda550000
  MAILBOX0=0   OS=0   WPR2_LO=0   WPR2_HI=0xE00 (device default)

post-timeout samples (100 ms apart):
  t+100ms  CPUCTL=0x00000000  DEBUGINFO=0xda550000
  t+200ms  CPUCTL=0x00000010  DEBUGINFO=0xda550000  ← HALTED bit flips on
  t+300ms  CPUCTL=0x00000010  DEBUGINFO=0xda550000
  ... (stays halted, DEBUGINFO stays at 0xda550000)
```

**New facts:**

1. `DEBUGINFO` stays at `0xda550000` *from STARTCPU through halt* —
   this value was written early and never updated. Either the
   phase-ID register is only written once, or FWSEC never progressed
   past the point where `0xda55` was written.
2. FWSEC **does eventually halt** — it's not infinitely stuck. But
   the halt happens **~100–200 ms past our halt timeout**, whether
   that timeout is 2 s or 5 s. That strongly suggests one of:
   - FWSEC has its own internal timeout that fires shortly after
     our host-side timeout. We stop polling; its internal timer
     fires; it bails with a HALT.
   - The halt is *correlated* with us ending the MMIO poll loop —
     unlikely, but possible if FWSEC is waiting on a PRI bus
     condition that frees up when we stop hammering it.
3. When FWSEC halts this way: **OS=0, WPR2 unset, err_reg=0**. That
   is NOT the success path (OS would be a non-zero app version; WPR2
   would be populated; err_reg would still be 0). Nor is it the
   bail-with-error path (err_reg would hold an error code). FWSEC
   is halting in an *aborted-without-error* state.
4. The open NVIDIA/nouveau/nova-core sources contain **no** hits for
   `0xda55` — it's an FWSEC-internal marker that isn't documented
   externally. Searching the FWSEC ucode binary itself for the
   literal bytes is the only remaining source angle; a new probe
   would need to do that.

**Conclusion:** FWSEC is stuck in a wait state (`0xda55` phase),
eventually times out internally, and halts without doing FRTS/SB
work. We need to find what it's waiting for.

### 0.4 Hypothesis for the next session — missing DEVINIT

Strongest remaining candidate, not yet tested:

**test-pc's RTX 3050 may never have had VBIOS DEVINIT executed.**

- DEVINIT is the early-boot init script in the GPU's VBIOS. It sets
  up the memory controller (NV_PFB), clock trees, PCIe lane config,
  and other hardware state that later stages (including FWSEC)
  depend on.
- UEFI firmware runs DEVINIT only on the *primary display*. On
  test-pc, the primary display is the i7-6700's iGPU (the HDMI is
  wired to the mobo, not the dGPU). So UEFI likely never ran the
  RTX 3050's VBIOS.
- At Linux boot, `vfio-pci` binds the dGPU without running DEVINIT.
  Nouveau / nova-core *do* run DEVINIT at probe time; our harness
  does not.
- If DEVINIT hasn't run, FB memory is uninitialized, some PRI
  blocks may be gated off, and FWSEC's expectation of "VBIOS already
  applied" fails → it waits for something that will never happen →
  internal timeout → halt with no output.

**Next probes:**

1. **Check if DEVINIT was run.** Read `NV_PGC6_BSI_VBIOS_GOLD_DONE`
   (or equivalent) — nouveau checks this as the "devinit already
   ran" flag. If clear, DEVINIT wasn't run.
2. **Plumb DEVINIT into the harness.** The VBIOS carries a DEVINIT
   script (in the BIT table). Nouveau's
   `nvkm_devinit_post()` / `ga100_devinit_post()` walks it. This is
   a significant piece of work but is the most likely root-cause
   fix.
3. **Alternative fast test:** temporarily boot the dGPU as primary
   display (swap HDMI cable, or disable iGPU in BIOS) so UEFI runs
   DEVINIT, then rebind to `vfio-pci` and re-run `--fwsec-frts`. If
   that works, we've confirmed the hypothesis and know the long-term
   fix is to plumb DEVINIT into SLM-OS itself.

### 0.5 Harness additions landed this session

- `--fwsec-sb` — probe variant using init_cmd=SB. Preserves
  frts_region write semantics (nouveau only writes it for FRTS).
- `falcon_hs_kick()` — split of `falcon_hs_boot` into kick + wait,
  for callers that need to intervene between STARTCPU and halt poll.
  `falcon_hs_boot` still works as before (calls kick + wait).
- `struct gsp_bringup::init_cmd`, `trace_mode` — knobs the harness
  sets before calling `gsp_bringup_fwsec_frts`. Default init_cmd
  remains FRTS.
- `gsp_bringup_patch_dmemmapper(... init_cmd ...)` — generic patcher
  used by both FRTS and SB paths. Skips frts_region sub-struct when
  init_cmd != FRTS. Legacy `_frts` name preserved as a wrapper so
  test_bringup keeps passing.
- `gsp_bringup_free(b)` — extracted DMA cleanup.
- `--fwsec-trace` — scaffolding for a kick-then-poll trace mode. Not
  working on current hardware (engine doesn't re-halt between runs;
  is_idle check fails at kick). Kept in the tree because the
  *concept* is still useful once we can FLR / device-reset the GPU.
  Don't delete without reconsidering.
- Post-timeout DEBUGINFO time-series sampling in the `--fwsec-frts`
  failure dump (10 × 100 ms). This is how §0.3 was measured.
- `FALCON_HALT_TIMEOUT_US` bumped from 2 s to 5 s. Still catches the
  halt one sample too late on test-pc, but closer.

---

## 1. The one-line goal

Make `gsp-harness --fwsec-frts` halt the GSP Falcon cleanly with
`WPR2_LO`/`WPR2_HI` populated and `NV_FWSEC_FRTS_ERR_REG` (0x001438)
top half-word == 0, on the live RTX 3050 in test-pc. That's the
gating milestone for everything downstream in Phase E (Booter Load,
GSP-RM RISC-V, RPC, compute).

---

## 2. Current ground truth (run that produced this doc)

```
$ ssh root@192.168.4.136 './gsp-harness --fwsec-frts'
[GSP-HARNESS] BAR0 mapped at 0x... size 16777216
[GSP-HARNESS] BAR1 mapped at 0x... size 268435456
[GSP-HARNESS] VFIO session open — DMA enabled
[VBIOS] parsed 1048576 bytes via BAR0+0x300000 PROM, 19 BIT entries
[GSP-HARNESS] FWSEC ucode: imem=58112 bytes dmem=2432 bytes
                engine_id=0x400 ucode_id=9 pkc_data_off=0x724
                imem_virt_base=0x0 interface_off=0x1c
[GSP-HARNESS] WPR2 target: addr=0x17fe00000 size=0x100000
[GSP-HARNESS] sig selection: fuse_reg[0x8241e0]=0x3 sig_count=4
                sig_versions=0xf → sig_index=2
[GSP-HARNESS] FWSEC-FRTS FAILED at phase 6 (rc=-1)
                FWSEC err reg = 0x00000000 (err_code=0)
                WPR2 lo = 0x00000000  hi = 0x00000e00
                GSP Falcon state:
                  CPUCTL=0x00000000 → 0x00000000 (halted=0, alias_en=0, iinval=0)
                  MAILBOX0=0x00000000 MAILBOX1=0x00000000  OS=0x00000000 DEBUGINFO=0xda550000
                  IRQSTAT=0x00000000 (halt=0, swgen0=0)
                  HWCFG2=0x000047f7 ENGINE=0x00000000 DMACTL=0x00000080 DMATRFCMD=0x00000002
                  BCR_CTRL=0x00000001 MOD_SEL=0x00000001 PARAADDR0=0x00000724
```

**What this tells us:**

| Observation | Interpretation |
|---|---|
| `MAILBOX0: 0xCAFEBEEF (sentinel) → 0x00000000` | The ucode started executing — it cleared the sentinel. |
| `CPUCTL = 0x00000000` (stable across two reads) | Not HALTED (bit 4), not in IINVAL fault (bit 0), not waiting on alias (bit 6 clear). The engine is in some "internal wait/sleep" state — not a tight loop, not crashed. |
| `DEBUGINFO = 0xda550000` | **FWSEC progress marker** we weren't capturing pre-PR-#181. Decode is unknown — needs symbol map or empirical bisection. |
| `OS = 0x00000000` | FWSEC never wrote its app-version handshake. So FWSEC didn't complete its init flow. |
| `WPR2_LO = 0`, `WPR2_HI = 0xE00` | WPR2 register pair untouched by FWSEC. The `0xE00` in HI is likely the reset/default value of the register, **not** anything FWSEC wrote. |
| `FWSEC err reg = 0` | No err code was raised. So FWSEC didn't reach a `bail_with_error` path either. |
| `IRQSTAT = 0` | No HALT IRQ, no SWGEN0. |
| `MOD_SEL = 0x1`, `PARAADDR0 = 0x724` | BROM was programmed correctly. RSA3K verify mode. |
| `BCR_CTRL = 0x1` | VALID set, CORE_SELECT clear → Falcon mode (correct for FWSEC; we're not RISC-V here). |
| `DMACTL = 0x80` | Surprising — `falcon_pre_dma_setup()` writes `DMACTL = 0`. Bit 7 isn't a documented Falcon v4 DMACTL bit. May be an FWSEC-internal write, or a register we don't fully understand. |

**Bottom line:** FWSEC entered execution, made some unknown amount of
progress (DEBUGINFO=0xda550000), then entered a wait state from which
the host is supposed to do something — but we don't yet know what. 2 s
timeout fires while it's still waiting.

---

## 3. What's been verified to be *correct*

These are blocked-off lines of investigation — re-checking them is
unproductive without new evidence.

- **GPU identification** — BOOT_42 = `0x177a1000` → GA107 confirmed.
- **VBIOS parse** — full 1 MB read via BAR0+0x300000 PROM window;
  19 BIT entries; FWSEC sub-image extracted cleanly with
  `engine_id=0x400 ucode_id=9 pkc_data_off=0x724 interface_off=0x1c
  imem=58112 bytes dmem=2432 bytes`. These match nouveau's view.
- **Signature selection** — fuse_reg=0x3 → bit 2 → idx=2; matches the
  documented nouveau ga102 algorithm. Verified against nova-core.
- **DMA upload** — IMEM and DMEM upload via Falcon DMA completes
  without DMATRFCMD timeout (else we'd fail at phase 4 or 5, not 6).
- **BROM programming** — PARAADDR/ENGIDMASK/UCODE_ID/MOD_SEL written
  in the right order (last-write-wins MOD_SEL triggers the verify).
  PARAADDR0 reads back as 0x724 = `pkc_data_off` ✓.
- **MAILBOX sentinel** — 0xCAFEBEEF → 0 transition proves the ucode
  entered, so the BROM verify *succeeded*. If verify had failed, the
  Falcon would have stayed at CPUCTL=0 with no MAILBOX change.
- **Falcon mode select** — BCR_CTRL VALID|CORE_SELECT=0 → Falcon (not
  RISC-V), correct for FWSEC.
- **WPR2 placement** — fixed in PR #181 to `fb_size - 0x200000` =
  `0x17FE00000`, matching nouveau's `tu102_gsp_oneinit` exactly. The
  old hardcode (0x17FEE0000) overlapped the VGA workspace — a real
  bug that was *not* the root cause but had to be fixed before we
  could trust subsequent results.
- **DMEMMAPPER patching** — verified against nouveau
  `nvkm_gsp_fwsec_patch`. `init_cmd=0x15` (FRTS), `read_vbios`
  sub-struct first (24 B, addr=0/size=0/flags=2), then `frts_region`
  (20 B, addr>>12, size>>12, type=2 (FB)). Matches. Tests in
  `host-tools/gsp-harness/test_bringup.c`.
- **Audit fixes from PR #173** (BOOTVEC=0, CPUCTL.ALIAS_EN routing)
  are *correct* but proved to be no-ops on this specific hardware:
  GA107's FWSEC descriptor has `IMEMVirtBase=0x0` already, and
  ALIAS_EN is clear at the relevant point. Keep them — they'd matter
  on different ucode descriptors.

---

## 4. Recommended next probes (ranked)

### 4.1 Bisect the DMEMMAPPER request

The simplest experiment that disambiguates "FWSEC always hangs" from
"FRTS request specifically hangs":

**Probe A** — change `DMEMMAPPER_INIT_CMD_FRTS` (0x15) to
`DMEMMAPPER_INIT_CMD_SB` (0x19) in `kernel/gpu/nvidia/bringup.c`.
SB = "Subsequent Boot" — a lifecycle no-op FWSEC variant that
nouveau uses post-resume.

- If SB also hangs identically → FWSEC isn't reaching the request
  dispatch at all. Look upstream (DMA contents, signature, init flow).
- If SB completes (Falcon HALTs, OS writes app version) → FRTS
  request is the trigger. Probably the `frts_region` sub-struct
  contents are being rejected. Try varying `addr`, `size`,
  `type` (`type=1` = SYSMEM instead of FB).

**Probe B** — issue `init_cmd = 0` (or any unknown command). FWSEC
should bail with an error code. If it instead hangs the same way →
the hang is happening before init_cmd dispatch.

### 4.2 Decode `DEBUGINFO = 0xda550000`

Search the open NVIDIA sources (`open-gpu-kernel-modules`,
`nova-core`, nouveau) for the literal `0xda55` or `DA55`. Could be:
- A magic-number sentinel ("waiting for caller").
- A phase ID written by FWSEC at known points in init.
- A bus/PRI fault indicator.

If a match shows up, it pins down what FWSEC is waiting for. If not,
sample DEBUGINFO at multiple time points (e.g., 100 ms, 500 ms, 1000 ms,
2000 ms post-STARTCPU) — see if the value evolves. A static value
across the timeout means FWSEC stalled at a single point; a changing
value means it's progressing through phases.

### 4.3 Check whether VFIO leaves the ROM BAR enabled

FWSEC's `read_vbios` sub-struct passes `addr=0, size=0` — telling
FWSEC to "auto-discover" the VBIOS by walking the ROM BAR. If
VFIO disabled the ROM BAR when it claimed the device, FWSEC's read
would silently return zeros and FWSEC might wait forever for a valid
VBIOS image.

Check on test-pc:

```bash
ssh root@192.168.4.136 'lspci -vvv -s 01:00.0 | grep -A 3 "Expansion ROM"'
```

If "ROM at ... [disabled]" → enable it via VFIO config-space writes
before STARTCPU. Or set `read_vbios.flags` differently to bypass the
ROM BAR walk and have FWSEC use the PROM window directly.

### 4.4 Compare DMEM contents post-upload to a known-good capture

`falcon_pio_upload_dmem` (or DMA upload) should land bytes verbatim.
If a write is dropping bytes (alignment, register order), the DMEMMAPPER
patch could be silently corrupted and FWSEC might be reading garbage.

To verify:
1. Add a `--dump-dmem` action to the harness that does `--fwsec-frts`
   setup but dumps the SEC2/GSP DMEM contents back to host before
   STARTCPU.
2. Hex-diff against the host-side DMA buffer (`b->dma_dmem_va`).

If they differ → upload bug (PIO ordering, DMA chunk boundary).
If identical → the hang is purely on the FWSEC side.

### 4.5 Try Booter Load anyway

FWSEC-FRTS sets up WPR2 *for the booter to consume*. But maybe the
booter doesn't actually need WPR2_LO/WPR2_HI populated to make
*some* progress — it might fail with a clear MAILBOX0 error code that
tells us what FWSEC was supposed to have done.

```
ssh root@192.168.4.136 './gsp-harness --booter-load'
```

The booter halts even with zero WprMeta (we observed this in unit
tests). Check whether the SEC2 reaches HALT, what MAILBOX0 says, and
whether WPR2_HI changes. This is informational but cheap.

---

## 5. Reproduction recipe

### One-time setup (already done; documented for resilience)

- test-pc: Gigabyte H610M S2H V2, i7-6700, 16 GB DDR4, RTX 3050 6 GB
  (GA107, device id 0x2584).
- Ubuntu 24.04 LTS on external SSD with `vfio-pci` bound to RTX 3050.
  See `docs/testing/test-pc-linux-vfio-setup.md`.
- root@192.168.4.136 — passwordless SSH from the dev box (key is
  authorized in `/root/.ssh/authorized_keys`).
- IOMMU group 10 = the GPU.
- UEFI boot order — for *this* test the SD card / SLM-OS isn't
  needed; we boot Ubuntu and run the userspace harness against
  vfio-pci. (P1-2 needs SD-first; E3.4 doesn't.)

### Per-iteration loop

```bash
# 1. Edit code in this repo (kernel/gpu/nvidia/, host-tools/gsp-harness/).
# 2. Rebuild:
make gsp-harness

# 3. Push to test-pc:
scp build/host-tools/gsp-harness root@192.168.4.136:/root/gsp-harness

# 4. Kill any stale harness holding /dev/vfio/10:
ssh root@192.168.4.136 'pkill -9 -f gsp-harness 2>/dev/null; sleep 1'

# 5. Run the FRTS test (or --booter-load, --riscv-start, --probe, --vbios):
ssh root@192.168.4.136 'timeout 30 ./gsp-harness --fwsec-frts'

# 6. Analyze the dump (the failing-state block has 13 register reads now).
```

**Important:** If a previous harness run hung and was Ctrl-C'd, it
may still hold the VFIO container fd. `pkill` it before re-running or
you'll get `Device or resource busy` on `/dev/vfio/10`. Step 4 makes
this safe.

For a quieter dump add `--trace` (per-MMIO logs to stderr — verbose).

---

## 6. Code map

### Files you'll touch

| File | What lives here |
|---|---|
| `kernel/gpu/nvidia/bringup.c` | `gsp_bringup_fwsec_frts()` orchestrates the whole FWSEC run. **Phase 6** (`falcon_hs_boot` halt poll) is the failure point today. |
| `kernel/gpu/nvidia/bringup.h` | State machine + WPR2 register addresses (`NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI`, `NV_FWSEC_FRTS_ERR_REG`). |
| `kernel/gpu/nvidia/falcon.c` | Falcon driver (probe, reset, DMA upload, `falcon_hs_boot`, PIO upload). |
| `kernel/gpu/nvidia/falcon.h` | Register map (`FALCON_OS`, `DEBUGINFO`, etc.). |
| `kernel/gpu/nvidia/nvidia_vbios.c` | VBIOS parse + FWSEC extraction. **Don't touch** without re-running test-vbios. |
| `host-tools/gsp-harness/main.c` | CLI dispatch + the failing-state diagnostic dump (the big block in the `ACT_FWSEC_FRTS` case). Add new register reads here. |
| `host-tools/gsp-harness/linux_platform.c` | VFIO container/IOMMU setup, BAR0/BAR1 mmap, firmware loading from `/lib/firmware/nvidia/ga107/`. |

### Test surfaces

| Suite | What it covers |
|---|---|
| `make test-bringup` | Sig-index algorithm, DMEMMAPPER patcher, state-machine guards. **Run after every bringup.c change**. 20 cases. |
| `make test-falcon` | Falcon driver — probe, reset, halt poll, DMA framing, PIO upload, ALIAS_EN routing. **Run after every falcon.c change**. 30 cases. |
| `make test-vbios` | VBIOS parser. 30 cases. Run if you touch `nvidia_vbios.c`. |
| `make test-nvfw` | NVIDIA HS firmware container parser. 14 cases. |
| `make test-rpc` | RPC ring math + barrier discipline. 17 cases. |
| `make test` | ARM64 QEMU full kernel suite. **Run after any shared-core change**. |

All host suites are pure-logic — they cover the byte-shuffling and
state-machine paths but cannot exercise live hardware. The harness is
the only path to hardware. Five host suites total = 111 cases, all
must stay green before pushing.

---

## 7. Known gotchas

1. **VFIO container leak.** If a harness run is interrupted, the VFIO
   container fd leaks and the next run gets `Device or resource busy`
   on `/dev/vfio/10`. `pkill -9 -f gsp-harness` before each run.

2. **labctl SDWire ≠ E3.4.** SD card flashing is for P1-2 (bare-metal
   SLM-OS). For E3.4 you boot Ubuntu and run the userspace harness;
   the SD card is irrelevant.

3. **#141 / mathf workaround.** Building on x86-64 requires the
   `mathf` workaround for `libm::sqrtf` / `libm::tanhf`. Already
   landed (PR #173). If you ever delete `runtime/src/inference/mathf.rs`,
   `make x86-disk PLATFORM=X86_64` will regress to the
   "Do not know how to soften this operator's operand!" abort.

4. **FB size is hardcoded to 6 GB** in `bringup.c`
   (`GA107_FB_SIZE_BYTES = 0x180000000`). If you swap to a different
   GPU (RTX 3050 8 GB, RTX 3060, etc.), update this *first* — WPR2
   placement is computed off it.

5. **The 2-second halt timeout** in `FALCON_HALT_TIMEOUT_US` is what
   we hit. Don't bump it without a hypothesis — if FWSEC is truly
   stuck, longer timeouts just delay the diagnostic. To probe
   "is it slow vs stuck", read DEBUGINFO at multiple time points
   instead.

6. **Endianness.** All Falcon registers are little-endian on the
   wire; we run on x86-64 (little-endian host) so this is moot in
   practice. Don't introduce byte-swap helpers without a reason.

---

## 8. What success looks like

After a successful FWSEC-FRTS run on test-pc you should see:

```
[GSP-HARNESS] FWSEC-FRTS ok — WPR2 registers:
                WPR2_LO = 0x0017FE00
                WPR2_HI = 0x0017FEFF
```

Where `WPR2_LO = (start >> 12)` and `WPR2_HI = ((end - 1) >> 12)` for
the WPR2 region we requested (`addr=0x17FE00000`, `size=0x100000`).

Then the next gates (in order) become unblocked:

1. `gsp-harness --booter-load` — SEC2 should HALT with MAILBOX0 != 0
   (because WprMeta is still zero-init; this is expected and
   diagnosable). Once WprMeta is properly populated (Phase 4 of E4),
   MAILBOX0 should be 0.
2. `gsp-harness --riscv-start` — `RISCV_CPUCTL.ACTIVE_STAT` (bit 7)
   should set within ~2 s.
3. E4 RPC ring marshalling becomes wireable. GSP-RM posts
   `NV_VGPU_MSG_EVENT_GSP_INIT_DONE` on the message queue.

After that: E5 compute / matmul, E6 full pipeline.

---

## 9. Open tickets

| Issue | Status | Relevance |
|---|---|---|
| **#27** | open | Umbrella tracking issue for E3 hardware completion. Comment from 2026-04-15 has the full hardware run details. |
| #141 | open (workaround landed) | mathf workaround for libm f16 crash. Don't delete mathf without solving #141 properly. |
| #169 | open (P3) | RPC defensive bounds checks. Doesn't block E3.4. |
| #170 | open (P3) | DMA alignment verification. Doesn't block E3.4 but worth checking if you suspect DMA issues. |
| #171 | open (P2) | `slm_get_time_ns()` overflow on x86-64. Doesn't block E3.4 (harness uses host time). |
| #172 | open (P3) | `bench smp` completion gate. Unrelated. |
| #177 / #178 / #179 | open (P3) | mathf adaptive iter, hardcoded timeouts in P1-2 script, tanhf doc nit. Unrelated. |

---

## 10. Recent commits relevant to E3.4

```
93f107c E3.4: fix WPR2 placement (was 0xE0000 above the real region) + extend FWSEC diagnostics  (PR #181)
27386f2 Phase E: explicit memory barriers around shared-mem ring publishes  (PR #168)
f6cf12c Phase E: cache_clean shared DMA buffers for ARM64 coherency  (PR #168)
414127d Phase E review fixes: error codes, named constants, doc nits  (PR #168)
23cb009 Phase E: E3.4 audit fixes + E3.4.d/e + E4 RPC skeleton  (PR #168)
bed6115 Phase E: comprehensive bringup tests + doc audit  (PR #165)
5f418a6 E3.4: nouveau ga102 sig-index algorithm + read_vbios sub-struct (#27 WIP)  (PR #162)
2326571 E3.4: FWSEC-FRTS bringup scaffolding (#27 WIP)  (PR #162)
```

Read these in the order shown — each one builds on the previous.

---

## 11. If you only have 30 minutes

Try Probe A (§4.1) — flip `DMEMMAPPER_INIT_CMD_FRTS` to
`DMEMMAPPER_INIT_CMD_SB` and re-run. If SB completes, the FRTS
request is the failure trigger and you have a clean bisection.
If SB also hangs identically, the issue is upstream of init_cmd
dispatch and Probes 4.2 / 4.4 become more interesting.

Both outcomes are useful. The probe takes one edit + rebuild + scp +
ssh run = ~3 minutes per cycle.

Update #27 with whichever way it goes before logging off.
