# x86-64 GPU Inference — Live Status & Handoff

**Last updated:** 2026-04-16
**Owner role:** open
**Companion docs:**
- `docs/x86-64-capstone-gaps.md` — top-level capstone gap map (this handoff is the live detail for its §3).
- `docs/archive/plans/x86-64-capstone-gap-closure-plan.md` — original Phase E plan (archived).
- `docs/archive/handoff/x86-64-fwsec-frts-handoff.md` — session-by-session hardware diary that produced the current state (archived; superseded by this doc).
- `docs/archive/handoff/x86-64-port.md` — architectural reference for the x86-64 port (archived).
- `docs/nvidia-gsp.md` — general GSP-RM background.
- `docs/jetson-nvidia-support.md` / `docs/jetson-el2-bringup.md` — Jetson-side GPU state.

This document is the single-page authoritative summary of where x86-64
GPU inference actually is — what works on hardware, what's blocked,
what was discovered along the way, and which of the remaining paths
forward are realistic.

---

## 0. One-page executive summary

- **FWSEC-FRTS works on retail Ampere (GA107 / RTX 3050) under VFIO.**
  The first secure Falcon ucode in NVIDIA's GSP bringup chain halts
  cleanly on test-pc, 3 consecutive runs, ERR_REG=0, WPR2 populated.
  This is the gating E3.4 capstone milestone — delivered.
- **Booter Load (E3.4.d) and everything downstream is hard-blocked**
  on this platform. Root cause: the BSI (Bootstrap Sequencer
  Instruction) re-runs VBIOS DEVINIT after every vfio-pci FLR, and
  the DEVINIT script raises SEC2's Priv-Level-Mask above our
  userspace-VFIO access level. We verified this by scanning SEC2's
  first 4 KB of register space: 865 / 1024 offsets priv-locked,
  including every standard Falcon-v4 PLM candidate offset. Tracked
  as **issue #185**.
- **The portable half of the GSP bringup code transfers cleanly to
  Jetson and bare-metal SLM-OS.** ~60% of the nouveau GSP-loader
  port is done, all of it platform-agnostic (VBIOS parse, Falcon
  driver, DMEMMAPPER patcher, sig-index algorithm, RPC skeleton,
  HS firmware container parser).
- **For downstream work, Jetson is the strongest next target.** The
  Jetson Orin Nano doesn't use VFIO or FLR, so the structural blocker
  doesn't exist; the remaining work is ~1–3 weeks of ARM64 platform
  shim + GA10B firmware sourcing.

---

## 1. What works today on test-pc (x86-64)

### 1.1 Hardware baseline

| Component | State |
|---|---|
| Host | Gigabyte H610M S2H V2, i7-6700, 16 GB DDR4 |
| GPU | NVIDIA RTX 3050 6 GB — chip GA107, device id `0x2584` |
| OS | Ubuntu 24.04 on external SSD, `vfio-pci` bound to the dGPU at boot |
| Remote | `root@192.168.4.136`, passwordless SSH from dev box |
| IOMMU group | 10 |
| Display | Monitor HDMI on dGPU output (so UEFI POST runs VBIOS DEVINIT on the dGPU) |

### 1.2 End-to-end state

```
$ ssh root@192.168.4.136 './gsp-harness --fwsec-frts'
[GSP-HARNESS] BAR0 mapped at 0x... size 16777216
[GSP-HARNESS] BAR1 mapped at 0x... size 268435456
[GSP-HARNESS] VFIO session open — DMA enabled
[GSP-HARNESS] BSI DEVINIT recovered in 300 ms
[VBIOS] parsed 1048576 bytes via BAR0+0x300000 PROM, 19 BIT entries
[GSP-HARNESS] FWSEC ucode: imem=58112 bytes dmem=2432 bytes
                engine_id=0x400 ucode_id=9 pkc_data_off=0x724
                imem_virt_base=0x0 interface_off=0x1c
[GSP-HARNESS] init_cmd: 0x15 (FWSEC-FRTS)
[GSP-HARNESS] WPR2 target: addr=0x17fe00000 size=0x100000
[GSP-HARNESS] sig selection: fuse_reg[0x8241e0]=0x3 sig_count=4
                sig_versions=0xf → sig_index=2
[GSP-HARNESS] FWSEC-FRTS ok — Falcon halted cleanly
                WPR2_LO   = 0x1ffffe00
                WPR2_HI   = 0x00000000
                ERR_REG   = 0x00000000  (code=0)
                MAILBOX0  = 0x00000000  OS = 0x00000000  DEBUGINFO = 0xda550000
```

### 1.3 What's code-shipped in-tree

| File | Lines | What it does |
|---|---|---|
| `kernel/gpu/nvidia/bringup.{h,c}` | 1050+ | State-machine orchestration for FWSEC-FRTS, Booter Load, RISC-V startup; DMEMMAPPER patcher; sig-index algorithm |
| `kernel/gpu/nvidia/falcon.{h,c}` | 500+ | Falcon v4 register protocol — probe, reset, DMA, PIO, HS-boot, priv-lock detection |
| `kernel/gpu/nvidia/nvfw.{h,c}` | 180+ | NVIDIA HS-firmware container parser |
| `kernel/gpu/nvidia/nvidia_vbios.{h,c}` | 700+ | VBIOS BIT-table parser, FWSEC discovery, PCIR walker |
| `kernel/gpu/nvidia/rpc.{h,c}` | 216 | GSP-RM RPC ring skeleton (ring math, pointer publishing) |
| `kernel/gpu/nvidia/gsp.{h,c}` | 130+ | Error-code constants, platform-ops vtable |
| `kernel/arch/x86_64/nvidia_gsp_platform.c` | 300 | x86-64 platform shim — all vtable ops wired (BAR0/BAR1 via nvidia_gpu.c, DMA via PMM identity-mapped) |
| `kernel/arch/arm64/nvidia_gsp_platform_stub.c` | 19 | Jetson linker stub (returns -1 for `nvidia_vbios_platform_load`); ARM64 platform ops to be written |
| `host-tools/gsp-harness/` | 2000+ | Linux userspace VFIO harness — the primary test driver |

### 1.4 Host-side test coverage

All tests run on the dev machine, not on hardware:

| Suite | Count | Notable coverage |
|---|---|---|
| `make test-vbios` | 30 | BIT parser, PCIR walker, FWSEC discovery |
| `make test-falcon` | 37 | Probe/reset/halt-poll/DMA/PIO/HS-boot protocol, plus this session: `falcon_hs_kick` (3), `falcon_is_priv_locked` (3), `falcon_wait_halted` early-bail on `0xbadfXXXX` (1) |
| `make test-nvfw` | 14 | `nvfw_bin_hdr` / `hs_header_v2` / `hs_load_header_v2` framing |
| `make test-bringup` | 25 | Sig-index algorithm, DMEMMAPPER patcher (legacy FRTS + generic init_cmd parameterised, +3 this session), `gsp_bringup_free` null-safety and idempotence (+2 this session), state-machine guards |
| `make test-rpc` | 17 | Ring math, init/dtor, null/oversize/not-alive rejection |
| **Total** | **123** | |

### 1.5 In-kernel test coverage (x86-64 platform shim)

QEMU-runnable Unity tests in `kernel/tests/test_x86_boot.c`. Exercise
the platform shim ops vtable through `x86_gsp_get_ops_for_testing()`
which returns the same `&x86_gsp_ops` registered with the shared GSP
core; reads/writes are safe in QEMU because nvidia_gpu.bar0/bar1
remain NULL when no NVIDIA GPU is discovered.

| Test | What it pins down |
|---|---|
| `test_nvidia_gpu_bar_mmio_accessors_safe` | BAR0/BAR1 pointer + size accessors return NULL/0 when no GPU |
| `test_gsp_platform_bar0_null_without_gpu` | BAR0 pointer is NULL precondition for sentinel behavior |
| `test_gsp_dma_alloc_via_pmm` | PMM allocation returns page-aligned address inside the 4 GB identity-map window |
| `test_x86_gsp_ops_complete` | All 11 vtable slots are non-NULL (catches accidental stub) |
| `test_x86_gsp_read32_null_bar0_returns_sentinel` | read32 returns 0xBADF5040 for any offset when BAR0 NULL |
| `test_x86_gsp_write32_null_bar0_no_crash` | write32 is silent no-op when BAR0 NULL |
| `test_x86_gsp_bar1_null_safe` | bar1_read leaves dst untouched + bar1_write silent when BAR1 NULL |
| `test_x86_gsp_dma_alloc_zero_size` | dma_alloc(0, ...) returns NULL and zeros out_dma |
| `test_x86_gsp_dma_alloc_oversized_align` | align > PAGE_SIZE rejected with NULL |
| `test_x86_gsp_dma_alloc_happy_path` | Aligned VA, VA == PA (identity), buffer zeroed, dma_free returns memory |
| `test_x86_gsp_dma_free_null_safe` | dma_free(NULL, ...) is no-op |
| `test_x86_gsp_cache_ops_no_crash` | cache_clean / cache_invalidate / mb don't crash, NULL-safe |
| `test_x86_gsp_firmware_get_manifest` | All four blobs present + plausible size when ENABLE_GSP_FIRMWARE; NULL/0 otherwise |
| `test_x86_gsp_firmware_get_invalid_kind` | Out-of-range kind returns {NULL, 0, NULL} |
| `test_x86_gsp_vbios_get_fwsec_no_gpu` | Returns -1 with NULL out_data when no GPU |
| `test_gpu_shell_subcommands_safe_without_gpu` | `gpu init` / `gpu sec2` / `gpu vram` / `gpu regs` don't crash without GPU |
| `test_gsp_firmware_manifest` | Pre-existing — embedded blob sizes, structural |
| `test_gsp_init_graceful_without_gpu` | Pre-existing — Phase 0 fails cleanly when no platform installed |

---

## 2. The blocker (#185) — details

### 2.1 What's locked

`--falcons` (which does vfio open → BSI wait → probe) reports:

```
[GSP-HARNESS] SEC2 Falcon @0x00840000  IMEM=64 KB  DMEM=64 KB  RISC-V=no
              idle=no  priv-locked=yes
                CPUCTL=0xbadf5620  HWCFG2=0x000067f7
```

`0xbadf5620` is NVIDIA's PRI-arbiter poison pattern: the access
arbiter denies the read because its Priv-Level-Mask (PLM) requires
a higher priv level than our bus-master context provides. HWCFG /
HWCFG2 are at a lower PLM tier and still readable.

`--sec2-plm-scan` walked every 4-byte offset in SEC2 `base+0x000 ..
+0xFFF`. Result: **865 of 1024 offsets** priv-locked, including
every common Falcon-v4 PLM candidate offset (+0x2A0, +0x4A8, +0x4B8,
+0x4C8). We can't even read the PLMs, let alone write them.

### 2.2 Why the blocker is platform-intrinsic, not a code bug

The sequence that raises SEC2's PLM is:

1. Userspace opens `/dev/vfio/GROUP`. vfio-pci's open handler calls
   `pci_try_reset_function(pdev)`, which performs a PCI FLR (Function
   Level Reset).
2. FLR clears the GPU's post-UEFI DEVINIT state.
3. GA107's on-chip BSI (Bootstrap Sequencer Instruction) observes the
   FLR event and automatically re-runs the VBIOS DEVINIT script from
   on-chip storage. Takes ~500 ms.
4. The DEVINIT script, as part of platform hardening, writes PLM bits
   that raise SEC2 CPUCTL / DMACTL / DMATRFCMD / etc. above PL0.

Nouveau and NVIDIA's open-gpu-kernel-modules never hit this chain
because they're kernel-mode drivers that bind *without* triggering
FLR. Once DEVINIT has been applied once (by UEFI at POST), their
SEC2 access stays at whatever priv level UEFI left it (accessible
from kernel-mode).

### 2.3 Why "run from kernel mode" doesn't solve it

The obvious hypothesis — "userspace is the problem, run from kernel
mode" — turns out to be wrong. The PRI arbiter judges access based
on the TLP fields of the request arriving over PCIe, not whether
the CPU was in ring 0 or ring 3 when the MMIO instruction executed.
A custom kernel driver would face the same 0xbadfXXXX reads as long
as it still triggers the FLR-and-BSI chain at bind time.

The actual difference is "runs FLR vs doesn't". nouveau doesn't.
vfio-pci does, unconditionally, on every open. And the FLR is what
triggers BSI, which is what raises the PLM.

### 2.4 What we tried to work around it

| Attempt | Result |
|---|---|
| Read PLM registers from userspace to relax them | Priv-locked too (they're under their own PLM) |
| Empty `reset_method` in sysfs to disable FLR | vfio open still triggers a reset via a different path; subsequent bringup hung |
| Find the CPUCTL PLM offset in NVIDIA's public headers | Not published (`dev_falcon_v4.h`, `dev_sec_pri.h`, `dev_falcon_v4_addendum.h` on main all checked) |
| Look for a nouveau/openrm unlock sequence | None exists — those drivers never see the locked state |
| **Run from bare-metal SLM-OS (no VFIO, no FLR)** | **Also locked. See §2.5 below.** |

### 2.5 Bare-metal bypass attempt — blocked by UEFI POST DEVINIT (2026-04-15)

The hypothesis in §2.2 was that UEFI leaves SEC2 "accessible from
kernel-mode" after POST. Hardware testing on bare-metal SLM-OS
(test-pc, no VFIO in the picture at all) invalidates this:

```
slmos> gpu sec2
SEC2 Falcon state (PSEC2_BASE=0x840000):
  CPUCTL        = 0xbadf5620  (priv-lock=yes)
  HWCFG2        = 0x000067f7  (priv-lock=no)
  IRQSTAT       = 0xbadf5620
  MAILBOX0      = 0x00000000  MAILBOX1 = 0x00000000
  OS            = 0x00000000  DEBUGINFO= 0x00000000
  ENGCTL        = 0xbadf5620
  BROM MOD_SEL  = 0xbadf5620
  BROM PARAADDR = 0xbadf5620
GSP Falcon state (PGSP_BASE=0x110000):
  CPUCTL        = 0x00000010  (priv-lock=no)
  HWCFG2        = 0x000047f7  (priv-lock=no)
```

Right after UEFI POST and before SLM-OS touches any Falcon register:
- **SEC2 CPUCTL / BROM / ENGCTL / IRQSTAT** are all priv-locked
  (0xbadf5620 poison). MAILBOX0/1 and OS/DEBUGINFO reads return 0,
  i.e. accessible but empty.
- **GSP Falcon is fully accessible** (CPUCTL=0x10 = HALTED bit,
  HWCFG2 readable). This is why FWSEC-FRTS runs fine on bare-metal
  (it targets GSP Falcon).

This means UEFI POST's DEVINIT script raises SEC2's PLM exactly the
same way VFIO's post-FLR BSI re-run does. There is no FLR on the
bare-metal path — UEFI itself did the lock. Bare-metal reaches
FWSEC-FRTS (primary E3.4 blocker on VFIO was already past that on
VFIO too — milestone for retail GA107), but Booter Load hangs at
phase 106 (STARTCPU + halt poll) because writing SEC2 CPUCTL.STARTCPU
has no effect through a priv-locked register.

```
slmos> gpu init
[GPU] Starting GSP-RM bringup on GA107...
[GSP] firmware loaded (version 535.113.01)
[GPU] Phase 1: FWSEC-FRTS — preparing bringup...
[VBIOS] read 1048576 bytes via BAR0 PROM window
[VBIOS] parsed 1048576 bytes, 19 BIT entries
[GPU] FWSEC: imem=58112 dmem=2432 engine=0x400 ucode=9
[GPU] WPR2 target: addr=0x17fe00000 size=0x100000
[GPU] sig: fuse[0x8241e0]=0x3 count=4 ver=0xf idx=2
[GPU] FWSEC-FRTS SUCCESS — WPR2: lo=0x1ffffe00 hi=0x00000000
[GPU] Phase 2: Booter Load on SEC2...
[GPU] Booter Load FAILED at phase 106
[GPU]   SEC2 CPUCTL=0xbadf5620 (halted=0)
[GPU]   SEC2 MBOX0=0x02d1d000 (persisted — write path reaches the register)
```

MAILBOX0 persists the WprMeta IOVA we wrote, confirming the SEC2
MAILBOX register tier is accessible. Only CPUCTL / BROM / IRQSTAT
(the tier needed to START the Falcon) is locked. This is the same
symptom VFIO showed — same root cause.

Conclusion: Option B (bare-metal) in §4.2 does not bypass #185 on
this particular board (H610M S2H V2 UEFI + GA107). The BSI/DEVINIT
hardening is baked into the VBIOS script, not the VFIO FLR path.
Any x86-64 host will hit the same lock after POST, regardless of
whether VFIO is in the picture.

**What still works on bare-metal that VFIO couldn't do:**
- End-to-end driver infrastructure validation (platform shim, BAR
  access, DMA via PMM, VBIOS parse, FWSEC-FRTS)
- Reproducible FWSEC-FRTS success on retail Ampere
- `gpu sec2` diagnostic for any future unlock attempt to measure
  against without the VFIO scaffolding

**What this pushes back to Jetson (Option A):**
Jetson has no DEVINIT — firmware runtime services come from QSPI
via SoC pre-boot. SEC2's PLM on Jetson GA10B is governed by SMMU
stream IDs + SoC-level security monitors (TF-A / BPMP), not a
VBIOS script. It's a different lock model, and therefore still the
pragmatic path for reaching Booter Load → GSP-RM.

---

## 3. What this session delivered

Five commits ahead of main as of this writeup (branch `worktree-x86-64-fwsec-frts`).

### 3.1 FWSEC-FRTS success (the headline)

Two production fixes in `kernel/gpu/nvidia/bringup.c`:

1. **BSI DEVINIT wait before bringup.** In `host-tools/gsp-harness/main.c`,
   poll `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0]` byte 0 for `0xff`
   (nouveau's `tu102_devinit_wait`) up to 2 s. Observed recovery
   time on test-pc: ~300 ms.
2. **Skip `falcon_reset` when the Falcon is already idle.** In
   `gsp_bringup_fwsec_frts`, gate the phase-3 reset on
   `falcon_is_idle()`. On VFIO paths FLR + BSI produce an
   already-idle Falcon; the redundant reset triggered a PRI-bus hang.

Together, these fixes take FWSEC-FRTS from "never-halts" to "halts
cleanly in <1s."

### 3.2 Latent DMEM-size mask bug

`FALCON_HWCFG_DMEM_SIZE_MASK` was `0x1ff0000` (bits 24:16) with shift
16. Nouveau's `nvkm_falcon_oneinit`
(`drivers/gpu/drm/nouveau/nvkm/falcon/base.c:273-275`) reads from
**bits 17:9** with mask `0x3fe00` and shift 9. Our mask
under-reported every Ampere Falcon's DMEM by ~4x. On SEC2/GA107 we
were reading 16896 bytes when the real value is 65536. Would have
caused Booter Load to fail the DMEM bounds check at PIO upload even
if the priv-lock weren't there. Fixed in `falcon.h` with a citation
comment.

### 3.3 Priv-lock detection API

New `falcon_is_priv_locked()` in `falcon.c/.h` — returns true when
CPUCTL reads `0xbadfXXXX`. Used by:

- `gsp_bringup_booter_load` phase 5 — skips `falcon_reset` when SEC2
  is priv-locked (write would be silently dropped anyway; reading
  HWCFG2 during the scrub poll would race).
- `falcon_wait_halted` — bails within a single poll iteration on the
  poison pattern rather than spinning its full timeout budget.
  Without this, the booter_load hang took 40-60 s wall time;
  with it, it returns `GSP_ERR_TIMEOUT` cleanly within a second.

### 3.4 Diagnostic harness actions

Added to `host-tools/gsp-harness`:

- `--fwsec-sb` — run FWSEC with `init_cmd=0x19` (SB) instead of FRTS.
  Proved the pre-fix hang was upstream of init_cmd dispatch.
- `--fwsec-trace` — kick-then-sample scaffolding for time-series
  DEBUGINFO observation.
- `--check-devinit` — preflight check for BSI DEVINIT completion.
- `--sec2-plm-scan` — scan SEC2 first 4 KB for PLM-pattern registers;
  evidence for the #185 priv-lock finding.

### 3.5 Infrastructure polish

- `falcon_hs_kick()` — split of `falcon_hs_boot` into kick-then-wait
  for callers that need to intervene between STARTCPU and halt poll
  (used by `--fwsec-trace`).
- `gsp_bringup_patch_dmemmapper()` — init_cmd-parameterised DMEMMAPPER
  patcher; writes the `frts_region` sub-struct only when `init_cmd ==
  FRTS` (matches nouveau `nvkm_gsp_fwsec_patch`). Legacy `_frts`
  wrapper kept so existing tests don't change.
- `gsp_bringup_free()` — explicit DMA cleanup helper (null-safe,
  idempotent).
- `FALCON_HALT_TIMEOUT_US` bumped from 2 s to 5 s.
- `setvbuf(stdout, NULL, _IOLBF, 0)` in `main()` — progress prints
  now flush line-by-line over SSH, which was critical for pinpointing
  the hang location during debugging.

### 3.6 Test coverage added

12 new host-side tests. See §1.4 table — all green under
`make test-falcon` and `make test-bringup`.

---

## 4. Paths forward (ranked)

### 4.1 Option A — **Jetson Orin Nano (GA10B) port** — RECOMMENDED

**Why it's the strongest next step:**

- The structural blocker (#185) **does not exist on Jetson.** No
  VFIO, no PCIe FLR, no BSI DEVINIT re-run.
- **GPU MMIO is already working** from EL2+VHE on jetson-nano-2:
  `0x17000000` responds, `BOOT_0 = 0xB7B000A1` confirms GA10B.
- **The entire portable half of what we built transfers verbatim.**
  `bringup.c`, `falcon.c`, `nvfw.c`, `nvidia_vbios.c`, `rpc.c`,
  `gsp.c` are all platform-agnostic. The DMEM-mask fix and
  `falcon_is_priv_locked` helper are already-paid-for value.

**What needs to be written:**

1. `kernel/arch/arm64/nvidia_gsp_platform.c` — the ARM64 platform
   shim. Sibling to `kernel/arch/x86_64/nvidia_gsp_platform.c`.
   Implements the ~10-function `gsp_platform_ops` vtable:
   - `read32`/`write32` — direct MMIO at `0x17000000` (no VFIO)
   - `dma_alloc`/`dma_free` — via the SMMU
   - `cache_clean`/`cache_invalidate` — existing DC CVAC/CIVAC helpers
   - `mb` — `dsb sy`
   - `firmware_get` — load GA10B firmware (source below)
   - `vbios_get_fwsec` — integrated GPU has no ROM BAR / PROM window;
     VBIOS lives in Tegra firmware files, not on an SPI flash the way
     the discrete RTX 3050 does. Plumb from L4T's
     `/lib/firmware/nvidia/tegra234/`.

2. **GA10B firmware sourcing.** Different chip → different FWSEC
   ucode, different signatures. The gsp-harness repo ships GA107
   firmware (`/lib/firmware/nvidia/ga107/`); we need the GA10B
   equivalents from NVIDIA L4T BSP. Licensing permits redistribution
   for development but needs capture.

3. **GA10B sig-index algorithm.** The nouveau `ga102_gsp_fwsec_signature`
   variant we ported to `gsp_bringup_select_sig_index` may differ for
   GA10B. Empirically testable against the live chip.

4. **Retire the `nvidia_gsp_platform_stub.c`** (Jetson linker stub
   that returns -1) once the real platform ops land.

**Estimated effort:** 1-3 weeks of focused work.

**Risk:** Jetson may have its own analogous priv-lock on SEC2 that we
haven't seen yet because we haven't tried. The chance is non-trivial
but lower than on x86-64 VFIO because the locking is FLR+BSI-triggered
and Jetson doesn't have that chain. If it does appear, it'll be in a
different form — probably a Tegra-specific CBB firewall rule — and
the EL2+VHE workaround we already use for the rest of Jetson bringup
is at a higher priv level than vfio-pci, so the range of potentially-
accessible PLMs is broader.

**Recommendation:** pursue this, even post-capstone, as the pragmatic
way to actually reach GPU inference.

### 4.2 Option B — **Bare-metal SLM-OS on x86-64**

**What's in the tree already:**

- `kernel/arch/x86_64/nvidia_gsp_platform.c` — 300-line scaffold.
- `kernel/arch/x86_64/nvidia_gpu.c` — 467 lines; probe + identification
  (BOOT_42 read), BAR1 R/W verification.
- `kernel/arch/x86_64/pci.c` — 487 lines; ECAM + BAR mapping works.
- `kernel/arch/x86_64/nvidia_gsp_firmware.S` — firmware bundling via
  `.incbin`.

**What's wired (as of 2026-04-15):**

- `x86_gsp_bar0_read32` / `write32` — volatile access via BAR0 pointer from nvidia_gpu.c, bounds-checked
- `x86_gsp_bar1_read` / `bar1_write` — byte-level volatile copy from BAR1 VRAM aperture
- `x86_gsp_dma_alloc` / `dma_free` — PMM buddy allocator, identity-mapped VA == PA, zeroed
- `x86_gsp_cache_clean` / `invalidate` — no-ops (x86 is cache-coherent over PCIe)
- `gpu init` shell command — calls `gsp_init()` → `gsp_bringup_prepare()` → FWSEC-FRTS → Booter Load → RISC-V start

**Advantages over Option A:**

- Same physical hardware (RTX 3050) as existing validation target.
- Keeps the x86-64 / Ampere / GA107 story first-class.

**Previously assumed advantage, now FALSIFIED (2026-04-15):**
- ~~The #185 blocker goes away — no FLR, no BSI re-run, DEVINIT from
  UEFI POST persists.~~ → UEFI POST DEVINIT raises the SEC2 PLM
  itself. See §2.5. Bare-metal reaches FWSEC-FRTS but still can't
  start SEC2. Option B is no longer a path around #185.

**Risks / costs:**

- Writing a working bare-metal GPU driver against Ampere is
  substantial — DMA, interrupts, power management, a basic
  IOMMU, firmware management.
- SLM-OS's userspace / shell doesn't have the toolchain to drive
  end-to-end inference today without substantial additions.
- UEFI DEVINIT only runs on the primary display; if the monitor is
  ever plugged into the iGPU, DEVINIT doesn't run on the dGPU and
  we'd need our own DEVINIT interpreter. Currently avoided by having
  the HDMI on the dGPU but brittle.
- **#185 applies to bare-metal x86-64 just as it does to VFIO x86-64.**
  Unless a SEC2 PLM unlock sequence can be found, bare-metal stops
  at the same phase 106 (SEC2 STARTCPU) as VFIO.

**What bare-metal did deliver (validation, not unblock):**
- Reproducible FWSEC-FRTS success on retail Ampere without VFIO.
- Confirmed the 1 MB BAR0 PROM window is the right VBIOS source
  (bare-metal Expansion ROM BAR is unassigned on H610M UEFI).
- `gpu init` / `gpu sec2` shell commands for future PLM research
  without the VFIO layer.

**Estimated effort:** weeks-to-months. Significantly larger than
Option A. Not reachable inside a capstone timeline.

### 4.2.k Option B-kexec — **Linux→SLM-OS kexec handoff** (NEW 2026-04-17)

**Approach:** boot Ubuntu normally on test-pc, let nouveau probe the GPU
(which unlocks SEC2 some ~3 s after modprobe — mechanism not yet
pinpointed but reproducibly observed, see
`docs/testing/x86-gpu-sec2-unlock-trace-2026-04-17.md`), then kexec into
SLM-OS *without power-gating the GPU across the transition*. SEC2 stays
in its `CPUCTL=0x20` (halted, unlocked) state when SLM-OS starts.

**Consistent with Jetson's `--no-gpu-suspend` fix** — same principle:
preserve live GPU state across the kernel swap.

**Why this is additive, not a replacement for §4.2:** the bare-metal
UEFI+SDWire disk-image path (`make x86-disk` → `labctl sdwire flash`)
remains first-class. kexec is a second route specifically for the
downstream work (E3.4.d Booter Load) that needs SEC2 already
unlocked.

**What's in the tree (2026-04-17):**

- `scripts/x86-kexec-slmos.sh` — runs on test-pc; preflights nouveau
  + SEC2 state + GPU runtime-PM, then `kexec -l --type=multiboot2-x86`
  and `kexec -e`. kexec-tools ≥ 2.0.28 is on Ubuntu 24.04 and supports
  `multiboot2-x86` natively, so no kernel boot-code changes are
  required — the existing Multiboot2 entry in
  `kernel/arch/x86_64/trampoline32.S` handles the handoff.
- `scripts/x86-kexec-deploy.sh` — runs on the dev host; scp's
  `slmos.elf` + helpers to test-pc and optionally triggers the jump.
- `make kexec-deploy PLATFORM=X86_64` — build + deploy + exec. Set
  `KEXEC_NO_EXEC=1` to stage only. Set `KEXEC_HOST=user@ip` to
  override the default test-pc target.

**Prerequisites checked by the helper:**
- kexec-tools installed with multiboot2-x86 support
- nouveau loaded (`modprobe nouveau modeset=1`) — the unlock happens
  during nouveau init
- GPU `power/control = on` — runtime PM off prevents autosuspend from
  clobbering SEC2 across the kexec
- `/dev/mem` read of SEC2 CPUCTL confirms it's not `0xbadf5620` before
  firing kexec

**Known limits:**
- test-pc can't also run under VFIO when using this path; the GPU
  must be nouveau-bound. Switch back to VFIO with `modprobe vfio-pci`
  after reboot.
- Booter Load has two GA10x-specific bugs (see 2026-04-17 report
  §4.4) that will surface once SEC2 is unlocked. Those are separate
  fixes tracked independently.

**Status (2026-04-17, second iteration):**

1. ✅ **"Invalid memory segment" rejection resolved.** Root cause was
   kexec-tools' default `kexec_file_load` / auto-detect syscall
   refusing the image. The older `kexec_load` syscall (`-c` flag)
   accepts it cleanly: `kexec -c --load --type=multiboot2-x86
   /root/slmos.elf` returns rc=0 and sets
   `/sys/kernel/kexec_loaded=1`. Fix committed in
   `scripts/x86-kexec-slmos.sh`. Also fixed a `set -o pipefail` +
   `lsmod | grep -q` SIGPIPE bug that was failing the nouveau
   preflight.

2. ❌ **New blocker — silent handoff.** `kexec -c --exec` fires
   (Linux dies: SSH drops, ping fails, ping drops), but **SLM-OS
   never emits serial output**, not even the single `'K'` byte
   written as the very first action of `_start` (trampoline32.S
   lines 60-62). Verified the byte IS in the compiled ELF at
   `0x20001000` after a clean rebuild.

   CPU is reaching some state (Linux is gone, so kexec executed),
   but not SLM-OS's entry — or UART output is suppressed. The `K`
   write uses a 3-instruction sequence (`mov $0x3F8,%dx`, `mov
   $0x4B,%al`, `out %al,%dx`) that is valid in both 32-bit
   protected mode and 64-bit long mode — so the handoff CPU mode
   isn't the culprit.

   Most likely causes, in order:
   - kexec-tools multiboot2-x86 loader is jumping to the wrong
     address (ELF `e_entry` is 0x20001000 but segments may have
     been relocated by kexec despite `-c`/non-PIE). Purgatory
     handoff could be jumping to 0x0 or similar.
   - UART state reset across handoff (Linux may gate the UART
     during its shutdown phase; SLM-OS doesn't re-init the UART
     before the `'K'` write).
   - Multiboot2 handoff requires a specific header tag (entry
     address tag, address tag) that our minimal header lacks.

**Experiments tried (2026-04-17, third iteration) — all still silent:**

| Experiment | Result |
|---|---|
| Full 16550 UART reinit at `_start` + emit "KEX\r\n" | No output |
| `MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS` (type=3, entry=_start) | No output |
| Multiboot v1 header alongside MB2 + `kexec --type=multiboot-x86` | kexec rejects: *"Wrong file type multiboot-x86, file matches type multiboot2-x86"* — it auto-detects MB2 and refuses MB1 on the same file |
| `kexec -c --load --type=elf-x86_64` (different loader) | Loads successfully (rc=0, kexec_loaded=1), but same silent handoff |
| Unload KVM + retry multiboot2-x86 | No change — KVM/VMX state isn't the cause |

All experiments leave the machine with network down, no serial
output, no `'K'`/`'E'`/`'X'` bytes — even though the UART init is
hardware-level (8250/16550 port writes, no Linux state needed) and
the ELF entry address is unambiguous.

**Conclusion:** kexec-tools 2.0.28 on Ubuntu 24.04 appears to load
our non-standard kernel via both `multiboot2-x86` and `elf-x86_64`
loaders (rc=0, kexec_loaded=1) but its purgatory never reaches our
entry — the CPU is executing *something* (Linux dies, net drops)
but emits no observable I/O. Either purgatory is spinning on an
internal error path or jumping to an unmapped address.

**Remaining next-session path:**

- Build a **bzImage wrapper** around `slmos-kexec.elf`. kexec's
  `--type=bzImage` is x86's most thoroughly-tested kexec loader,
  with a precisely documented handoff state (32-bit protected
  mode, specific register values, boot_params at EBX). The
  wrapper is a ~200-line 32-bit stub linked as a bzImage: it
  reads the bzImage `setup_header.cmd_line_ptr` (or a fixed
  offset) to find the appended slmos.elf, copies PT_LOAD
  segments to their `p_paddr`, then jumps to `e_entry`. Everything
  else in-tree (linker script, deploy scripts, Makefile target)
  stays usable.

- **Optional diagnostics if bzImage also stays silent** — tells us
  the blocker is upstream of kexec-tools' loader choice (hardware,
  purgatory, BIOS runtime services):
  - `kexec --console-serial` to get the purgatory itself to emit
    over COM1 as it runs.
  - Attach a physical POST card to the LPC bus (or use a
    motherboard with one built-in) to see port 0x80 writes during
    handoff.
  - Try the same kexec path under QEMU with `-d int -monitor
    stdio`; any triple-fault dumps registers. Rules in/out
    "test-pc UEFI firmware state" as the blocker vs. "kexec
    itself is broken on this build."

**Original "Invalid memory segment" investigation notes:**

Initial `kexec --load --type=multiboot2-x86` (and
`--type=elf-x86_64`) rejected the image with *"Invalid memory
segment 0x<addr> - 0x<end>"*. Investigated in this order:

1. First tried the default 1 MiB load (`KERNEL_PHYS = 0x100000`) —
   rejected because the running Ubuntu kernel is assumed to occupy
   low memory.
2. Added a parallel linker script `kernel-x86_64-kexec.ld` that links
   at `0x20000000` (512 MiB) — rejected too, even with Ubuntu's
   actual kernel code/rodata/data/bss at `0x48dc00000-0x490ffffff`
   (above 4 GiB, well clear of 0x20000000).
3. Debug run confirms `/proc/iomem` reports one contiguous `System
   RAM` span at `0x100000-0x35fa0fff` (830 MiB) that would easily
   contain 44 MiB at 0x20000000. `--mem-min`, `elf-x86_64` loader,
   and auto-detect all give the same rejection.

The rejection is from kexec-tools' internal `valid_memory_segment`
check, which requires the segment to fit within a single memory
range reported by the loader's builder. The multiboot2-x86 loader
(`kexec-tools 2.0.28`) in particular appears to not honour the
full `System RAM` span — possibly because it expects a relocatable
(PIE) ELF and ours is linked `-no-pie`, possibly because of a
loader-specific address limit. Needs further kexec-tools source
reading to root-cause.

**Infrastructure in tree and working (2026-04-17):**

- `kernel/kernel-x86_64-kexec.ld` — alternate linker script.
- CMake option `KEXEC_BUILD=1` that swaps in the alt script.
- `make kernel-kexec PLATFORM=X86_64` — produces
  `build/kernel-kexec/slmos.elf` linked at 0x20000000. Verified: the
  ELF is well-formed (`readelf -l` shows entry 0x20001000, LOAD
  segment at paddr 0x20000000, 44 MiB MemSiz).
- `make kexec-deploy PLATFORM=X86_64` — scp's the ELF + helper to
  test-pc and (would) fire kexec. Currently fires but is rejected at
  `kexec --load`.
- `scripts/x86-kexec-slmos.sh` — test-pc helper; preflights nouveau
  + SEC2 state + GPU runpm, then does `kexec -l && kexec -e`.
- `scripts/x86-kexec-deploy.sh` — dev-host wrapper; scp + invoke.
- `scripts/tests/verify-kexec-build.sh` + `make kexec-verify
  PLATFORM=X86_64` — 16-check structural validator. Confirms both the
  bare-metal ELF (0x100000) and the kexec ELF (0x20000000) link at the
  expected addresses, both carry MB1 (0x1BADB002) and MB2 (0xE85250D6)
  magic within the first 8 KiB, both include a `MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS`
  tag pointing at `_start`, and the trampoline's 16550 UART reinit +
  "KEX\r\n" diagnostic is in the compiled entry code. Also runs
  `shellcheck -S warning` against the two Linux-side helper scripts.
  Runs in under a second; suitable for CI once the X86_64 job calls
  `make kernel` + `make kernel-kexec`.

**What's left to unblock:**

1. Root-cause the kexec-tools rejection. Build kexec-tools from
   source locally, add instrumentation around `valid_memory_segment`
   and `get_memory_ranges`. Expected outcome: a specific constraint
   (PIE requirement / loader address limit / e820 subdivision logic)
   that either we can work around via a different loader
   (`--type=bzImage` with a wrapper) or a linker change.
2. If the block is "multiboot2 loader wants a relocatable kernel",
   the cleanest workaround is a **bzImage wrapper**: a tiny stub
   linked as a Linux-compatible bzImage (well-tested kexec path)
   that decompresses/copies `slmos.elf` to its final address and
   jumps to its entry. ~100 lines of Rust or C.
3. Alternative: use `kexec -p` (panic kernel) with a
   `crashkernel=64M@0x10000000` reservation on the Ubuntu cmdline.
   Panic-kernel loads may relax the range check.

**Estimated effort to unblock:** half a day to a day, depending on
which of the three paths pans out first. All the hard parts (SEC2
unlock understanding, scripts, build system integration, bare-metal
preservation) are in tree.

### 4.3 Option C — **Kernel-shim / patched vfio-pci**

**Approach:** modify Linux to skip FLR on vfio-pci open for this
specific device, or provide a no-FLR VFIO-like interface via an
out-of-tree module.

**Why it might not help:** the priv-lock trigger is BSI+DEVINIT. If
we skip FLR but something else in the bind path still triggers BSI
(e.g., D-state transition, link-training event), we'd still face the
lock. Empirical test required to know.

**Costs:** Linux kernel engineering, out-of-tree maintenance, PR
upstream if we want long-term support.

**Estimated effort:** unknown. Not recommended for capstone or
immediate post-capstone work.

### 4.4 Options descoped

- **Write an Ampere SASS assembler.** The NVIDIA-supplied `ptxas` is
  closed-source; Ampere SASS community tools are incomplete. If we
  ever reach E5 (compute kernels), we'd revisit whether hand-SASS or
  a pre-compiled cubin is the path. For now, the bottleneck is E3.4.d,
  not E5.
- **VirtIO-GPU under QEMU.** Would demo inference on a virtual GPU
  but nothing transfers to real hardware. Not the capstone's story.

---

## 5. What we know about the downstream work (E4 / E5 / E6)

Even though E3.4.d is blocked on x86-64, there's productive dry code
work available for the Jetson push, because most of E4 / E5 is
platform-independent:

- **E4 (RPC ring) completion.** Our skeleton does ring-pointer publish
  and cache-clean. Missing: the actual GSP-RM message envelope
  format (function code, argument TLVs, seq numbers), individual
  message types (`NV01_ALLOC_MEMORY`, `NV2080_CTRL_CMD_*`,
  `NV_VGPU_MSG_EVENT_GSP_INIT_DONE`), response parsing, connection
  state machine. All testable against a fake ring. **~5-10 days.**
- **E5 (compute submission).** Pushbuffer encoder (METHOD+DATA
  stream), GPFIFO ring math, channel allocation, basic GMMU page
  tables, VRAM allocator, synchronization semaphores. All testable
  against byte-precise fixtures. **~10-15 days.**
- **E5/E6 runtime side.** FFI boundary (`gpu_matmul_submit`,
  `gpu_sync`), `select_backend` extension for Conv2D/MaxPool/Relu,
  tensor residency bookkeeping, eviction policy. All pure Rust;
  no hardware needed. **~5-10 days.**

Real risk of doing it dry: NVIDIA's RPC formats and pushbuffer
encodings are byte-precise. Hardware contact always reveals small
errors in struct layouts and field widths. But the code skeleton
has high enough value (and transfers to Jetson) to justify building
it ahead of hardware validation.

---

## 6. Reproduction recipe

### 6.1 Build

```bash
make gsp-harness
make test-vbios test-falcon test-nvfw test-bringup test-rpc  # 123 host tests
make test                                                     # ARM64 QEMU
```

### 6.2 Deploy to test-pc

```bash
scp build/host-tools/gsp-harness root@192.168.4.136:/root/gsp-harness
ssh root@192.168.4.136 'pkill -9 -f gsp-harness 2>/dev/null; sleep 1'
```

### 6.3 Run

```bash
# Preflight: DEVINIT done?
ssh root@192.168.4.136 'timeout 10 /root/gsp-harness --check-devinit'
# Falcon probe + SEC2 priv-lock detection
ssh root@192.168.4.136 'timeout 10 /root/gsp-harness --falcons'
# Main event: FWSEC-FRTS (should succeed, <1s post BSI)
ssh root@192.168.4.136 'timeout 15 /root/gsp-harness --fwsec-frts'
# Blocker surface: Booter Load (fails cleanly with GSP_ERR_TIMEOUT)
ssh root@192.168.4.136 'timeout 15 /root/gsp-harness --booter-load'
# Evidence for #185: PLM scan
ssh root@192.168.4.136 'timeout 15 /root/gsp-harness --sec2-plm-scan | head -40'
```

### 6.4 Gotchas

- **VFIO container leak.** If a harness run is interrupted mid-flight,
  the VFIO container fd leaks and the next run gets `Device or
  resource busy` on `/dev/vfio/10`. `pkill -9 -f gsp-harness` before
  each run.
- **HDMI location matters.** UEFI only runs VBIOS DEVINIT on the
  primary display. Keep the monitor HDMI plugged into the dGPU
  (not the motherboard / iGPU) or `--check-devinit` will show
  DEVINIT NOT done even after BSI recovery (because BSI only re-runs
  what UEFI originally did).

---

## 7. Commit history (session 2)

Branch `worktree-x86-64-fwsec-frts`, five commits ahead of main:

| Commit | Subject |
|---|---|
| `2579412` | E3.4 probes: `--fwsec-sb`, `falcon_hs_kick`, post-timeout DEBUGINFO sampling |
| `f4b2eac` | E3.4 root cause: `--check-devinit` proves VBIOS DEVINIT never ran |
| `e4940ec` | E3.4: FWSEC-FRTS succeeds on GA107 — BSI wait + skip-idle-reset |
| `f38a006` | E3.4.d investigation: SEC2 priv-locked on VFIO + HWCFG DMEM mask fix |
| `02859c8` | E3.4: rule out PLM unlock; adopt 'FWSEC-FRTS success as milestone' |

Plus a docs / test / archive refresh landing the contents of this
handoff.

---

## 8. Proposed next actions

In rough priority order:

1. **Merge the current branch** — the session's capstone-relevant
   output is the FWSEC-FRTS win, the priv-lock diagnosis, and the
   test/doc hardening. All forward-looking work builds on this being
   on main.
2. **File #185 as a blocker** (done — issue #185 open).
3. **Close / re-scope #142** (GSP bare-metal loader future-work):
   the original 6-12 month estimate is superseded. What's left is
   "ARM64 platform shim + GA10B firmware sourcing + empirical
   debugging" — a 1-3 week slice.
4. **If Jetson GSP inference is in scope:** start the ARM64 platform
   shim. See §4.1 for the concrete file list.
5. **If GPU work is paused:** leave the branch merged, keep the
   harness + diagnostics, and the portable bringup code continues
   to accrue value for any future platform.

---

*Maintainer note: this document is the live handoff. If it and the
top-level `docs/x86-64-capstone-gaps.md` disagree, this one wins for
anything GPU-inference-related. File cross-referencing updates to the
gaps doc.*
