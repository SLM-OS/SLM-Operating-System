# Jetson GA10B (nvgpu-native) Bringup Research

**Target:** Jetson Orin Nano Super, GA10B integrated Ampere GPU, EL2+VHE bare-metal,
BAR0 at the MMIO fixed aperture (see §3). **Goal:** first FP32 compute method
executed from SLM-OS, without using Linux drivers or the discrete Ampere GSP-RM stack.

This is the nvgpu-native path — the legacy Tegra `nvgpu` driver (L4T
`drivers/gpu/nvgpu/`). It shares Falcon hardware and the broad shape of
"ACR → LS ucodes → PMU → GR → channel" with discrete Ampere, but none
of the GSP-RM / SEC2 Booter / WPR2 / 38 MB RISC-V ELF machinery from
`docs/nvidia-gsp.md` applies. Treat that document as "a sibling path
for discrete Ampere" — reuse the Falcon primitives, throw away the
GSP-specific phases.

> **Status:** Research only. No code exists yet. Handoff to an engineer
> (me) implementing against this doc. See GitHub issue #142 and the
> task list (items #10–#16).

---

## 0. Firmware Inventory Recap

On `jetson-nano-2`, `/lib/firmware/nvidia/ga10b/` ships (L4T R36.4.7):

| File                                          | Meaning                                                            |
| --------------------------------------------- | ------------------------------------------------------------------ |
| `acr-gsp.{text,data,manifest}.encrypt.bin.prod` | ACR image that runs on GSP Falcon; bootstraps all other LS ucodes |
| `safety-scheduler.{text,data,manifest}.encrypt.bin.prod` | Safety scheduler LS ucode (Tegra-safety variant)         |
| `fecs_encrypt_prod.bin` + `fecs_pkc_sig_encrypt.bin` | FECS context-switch ucode + PKC signature blob             |
| `gpccs_encrypt_prod.bin` + `gpccs_pkc_sig_encrypt.bin` | GPCCS context-switch ucode + PKC signature blob          |
| `gpmu_ucode_next_prod_image.bin` + `_desc.bin`| PMU ucode image + v4 descriptor (header tells us IMEM/DMEM split) |
| `pmu_pkc_prod_sig.bin`                        | PMU PKC signature (consumed by ACR when verifying PMU)            |
| `NET{A,B,C,D}_img_prod_encrypted.bin`         | Runtime NET images (fault/reset variants; A is the usual boot one) |

No `gsp-*.bin`, no `booter_*.bin`, no `bootloader-*.bin`. ACR is the root of trust here, not SEC2 Booter. `CMAKE` already gates `ENABLE_GSP_FIRMWARE` off on Jetson in anticipation of this.

---

## 1. High-Level Flow

Ordered sequence, "GPU MMIO readable" to "first compute method executed":

```
A. Pre-bringup sanity
   ├─ BAR0 readable (already true at EL2+VHE)
   ├─ NV_PMC_BOOT_0 reads 0xB7B000A1 (GA10B identity)
   └─ Clock & power lanes ungated by L4T pre-kexec helper
        (scripts/jetson-kexec-slmos.sh already handles GPU suspend;
         we need the *inverse* — leave the GPU unsuspended, or
         re-ungate in SLM-OS via BPMP MRQ_CLK/RESET. See §8 gotchas.)

B. Engine reset + PMC.ENABLE
   ├─ Toggle NV_PMC_ENABLE bits for PGRAPH, PFIFO, PMU, CE
   └─ Wait for engines to come out of reset (HWCFG2.MEM_SCRUBBING=0 per Falcon)

C. FBIF / TRANSCFG per Falcon
   └─ Program FBIF_TRANSCFG for the GSP Falcon so ACR DMA works
      (same pre-DMA poke sequence already in falcon_pre_dma_setup)

D. ACR-GSP bootstrap on GSP Falcon
   ├─ Parse acr-gsp.{text,data,manifest}.encrypt.bin.prod
   ├─ DMA-upload text → IMEM, data → DMEM
   ├─ Program BROM (PARAADDR/UCODE_ID/ENGIDMASK/MOD_SEL)
   ├─ STARTCPU, wait for ACR to HALT
   ├─ ACR reads its manifest, locates LS ucodes in a DMA'd blob,
   │  decrypts / verifies them, writes them into WPR (protected FB region),
   │  and hands control to each LS target (FECS, GPCCS, PMU) via
   │  Falcon boot.
   └─ Done signal: ACR HALT + mailbox status word = success code

E. Engine bring-up (post-ACR)
   ├─ FECS boot complete (mailbox poll on PGRAPH_FECS_MAILBOX(0/1))
   ├─ GPCCS boot complete (mailbox poll on PGRAPH_GPCCS_MAILBOX)
   ├─ PMU boot complete (QUEUE_HEAD / MAILBOX handshake)
   └─ Safety-scheduler: skip for bare-metal compute unless L4T DTB
      requires it; nvgpu has a code path that boots it but it's not
      mandatory for FP32 matmul.

F. Address space + BAR2/instance memory setup
   ├─ Allocate an instance block (inst_block) in FB or sysmem
   ├─ Build a minimal GMMU page directory (typically 2-level small-page)
   ├─ Bind the inst_block to PBDMA via BAR2 bind registers
   └─ This is where "BAR2 window into GPU address space" gets lit up

G. Channel + runlist construction
   ├─ Allocate GPFIFO ring (64 entries is plenty) in system memory,
   │  mapped into GPU VA space via the inst_block's page tables
   ├─ Allocate a pushbuffer (e.g. 64 KB) in system memory, mapped VA
   ├─ Build a runlist with a single entry pointing at our channel
   ├─ Write NV_PFIFO_RUNLIST_BASE/PTR and kick the runlist
   └─ Wait for PBDMA to report channel loaded (status mailbox /
      interrupt). PBDMA latches gpfifo_get/put on each method fetch.

H. First compute method
   ├─ On the pushbuffer, emit a SET_OBJECT(AMPERE_COMPUTE_B) header
   │  followed by minimal compute class state (SET_SHADER_LOCAL_MEMORY,
   │  SET_SHADER_EXCEPTIONS, minimal TPC masks — most have sane resets).
   ├─ Emit SET_REPORT_SEMAPHORE_{A,B,PAYLOAD,CONTROL} then
   │  REPORT_SEMAPHORE with operation=STRUCTURE_SIZE or RELEASE.
   ├─ Update gpfifo_put; PBDMA fetches the methods, GR processes them,
   │  semaphore payload lands at a known VA.
   └─ CPU reads the semaphore memory → "first compute method executed"
      latch. See §9 minimal demo.

FP32 matmul is a straightforward extension of (H) — it's just more
methods in the pushbuffer (COMPUTE_DISPATCH_GRID + a precompiled SASS
kernel bound via SET_PROGRAM_REGION). That's phase 4 of the task list
(#16); this doc gets us through phase 3 (#15).
```

---

## 2. Per-Phase Detail

### Phase A — Sanity

**Inputs:** None.
**Touches:** `NV_PMC_BOOT_0` at BAR0+0x0, `NV_PMC_BOOT_42` at BAR0+0xA00.
**Done:** `BOOT_0` == `0xB7B000A1` (GA10B). `BOOT_42` decodes Arch=0x17 (Ampere), Impl=0xB.
**What goes wrong:**
- CBB fence on MMIO, which we already worked around via EL2+VHE (see `docs/jetson-el2-bringup.md`). If `BOOT_0` reads 0xBADF_____, we never actually got MMIO — bail immediately.
- Reading from EL1 returns abort — the whole EL2+VHE setup is prerequisite.

### Phase B — PMC.ENABLE toggle

**Inputs:** Masks for engines we need.
**Touches:** `NV_PMC_ENABLE` at BAR0+0x200 (from `dev_boot.h` in openrm):
- Bit 12: PGRAPH
- Bit 8: PFIFO
- Bit 0: PMU (Falcon-shaped)
- Bits for CE (copy engine) depend on variant — GA10B exposes a single LCE sufficient for DMA'd data.

Pattern (same as nvgpu `nvgpu_pmc_enable_units`): clear all targeted bits, read-back, set them, read-back, wait ~1 ms.

**Done:** all target engines report `HWCFG2.MEM_SCRUBBING == 0` on their own base.
**What goes wrong:**
- Toggling PMC too fast can leave engines mid-reset — the read-back dance is not optional.
- On GA10B, some SKUs have only a subset of the CEs wired; if we bail on a missing CE we're being too strict.

### Phase C — FBIF / TRANSCFG

**Touches:** GSP Falcon base (0x00110000 on discrete Ampere; **needs verification** on GA10B — see §3):
- `FBIF_TRANSCFG(0)` at engine+0x600 — set `MEM_TYPE=1` (local FB or sysmem depending on target), `TARGET=VIDMEM` or `COHERENT_SYSMEM` (for Jetson unified memory, we want `COHERENT_SYSMEM` / `NONCOHERENT_SYSMEM`).
- `FBIF_REGIONCFG` (offset nearby, ~0x604) — sets the `T` region tags used by DMATRFCMD.CTXDMA.
- Already in `falcon.c`: `falcon_pre_dma_setup` writes `0x624 |= 0x80`, `DMACTL=0`, and `0x600 = (0<<16)|(1<<2)|1`. The last value assumes MEM_TYPE=1, TARGET=1 which is VIDMEM. **On Jetson we need TARGET=COHERENT_SYSMEM** — that's `(0<<16)|(0<<2)|1` or similar; value needs to be read from nvgpu's `gm20b/acr_gm20b.c` or `ga10b/*.c`. Tracked as follow-up.

**Done:** no observable status; it's a "just write it" step.
**What goes wrong:** silent wrong TRANSCFG → DMA reads the wrong aperture, IMEM fills with garbage, ACR fails BROM verify and Falcon never halts (exact same symptom as FWSEC bug #1 we already debugged).

### Phase D — ACR-GSP bootstrap

This is the big one. Reuses almost all of the Falcon driver we already have, but with different firmware and a different payload format.

**Firmware parsing:**
- `acr-gsp.text.encrypt.bin.prod` — IMEM payload (encrypted at rest; BROM decrypts in place after PKC verify using the manifest's key).
- `acr-gsp.data.encrypt.bin.prod` — DMEM payload (ditto).
- `acr-gsp.manifest.encrypt.bin.prod` — the PKC manifest consumed by BROM. Contains the RSA-3K signature(s), the BROM ucode_id/engine_id, and pointers to key material. **Format is NOT the `nvkm_falcon_ucode_desc_v3` we handle for FWSEC.** nvgpu uses `struct bin_hdr` + `struct acr_fw_hdr` + `struct hs_acr_header_v1` laid out as:
  ```
  bin_hdr        { bin_magic, bin_ver, bin_size, header_offset, data_offset }
  acr_fw_hdr     { hdr_size, patch_loc, patch_sig, hdr_offset, data_offset }
  hs_acr_header { non_sec_code_off/size, sec_code_off/size, data_off/size,
                  sig_dbg_off, sig_prd_off, patch_loc_off, patch_sig_off,
                  hs_fmc_params { pkc_algo, engid_mask, ucode_id, fuse_ver, ... } }
  ```
  These are nvgpu-native (not openrm / not nouveau). Source: L4T
  `drivers/gpu/nvgpu/common/acr/acr_bin.h`, `acr_blob_construct_*.c`.
  **Not in `~/slmos-ref/` yet — must fetch before coding.**

**Upload:**
- Falcon DMA is the same as what we already do (256-byte chunks). `kernel/gpu/nvidia/falcon.c:falcon_dma_upload` works unchanged.
- One nvgpu-specific twist: ACR expects its ucode+data+manifest to live in a single contiguous blob in sysmem (the "WPR blob" in nvgpu parlance), and ACR walks it internally using offsets from its own DMEM. So instead of N `falcon_dma_upload` calls, we may need to:
  1. Upload a small "ACR bootloader" (non-sec section) into IMEM.
  2. Upload the full blob description into DMEM.
  3. ACR then DMAs its own body from sysmem once it's running.
  This matches nvgpu's `acr_blob_construct_wpr_blob` + `acr_execute`.

**Kick:**
- BROM register sequence identical to FWSEC: `PARAADDR0 = sig_offset_in_DMEM`, `BROM_ENGIDMASK`, `BROM_CURR_UCODE_ID`, `MOD_SEL = 1 (RSA3K)` last.
- `BOOTVEC = 0` (same FWSEC v3 rule — see the ALIAS_EN / BOOTVEC=0 bug fixed in E3.4 audit).
- `CPUCTL.STARTCPU` via `CPUCTL_ALIAS` (0x130) if `ALIAS_EN` is set (our existing fix).

**Done:**
- `CPUCTL.HALTED == 1` within a few seconds.
- Success code in `MAILBOX0` or `MAILBOX1` (nvgpu convention: 0 = success, non-zero = error bitmap). Specific values in nvgpu `acr_error_codes.h` (fetch required).

**What goes wrong:**
- Wrong sig index — PKC signature blob has multiple sigs keyed by fuse version. Same shape as our FWSEC sig-index selection (`gsp_bringup_select_sig_index`); the fuse reg moves to a GA10B-specific offset, **needs verification**.
- Priv-locking: on Jetson post-kexec we may hit the same SEC2-style priv lock SLM-OS already handles for x86-64 (#185 — FLR sets PLM high). **Hypothesis:** BPMP holds GSP priv level > CPU priv level until BPMP's `MRQ_DRAM_FIREWALL_CO` or similar is sent; the fix is either a BPMP MRQ or we route ACR through PMU first (the L4T Linux boot path pokes PMU before GSP). Needs probing.
- DMA target wrong (see Phase C) — ucode contents corrupt, BROM verify fails, HALT never sets.

### Phase E — Engine boot (FECS / GPCCS / PMU)

ACR's job is to load + verify these; once ACR halts with success, each target Falcon is ready to be started.

**FECS (Front-End Context Switch):**
- Base: `NV_PGRAPH_PRI_FECS_FALCON` at BAR0+0x00409000 (GA10x convention; **verify on GA10B**).
- After ACR: IMEM/DMEM already populated, CPU halted.
- Start: set BOOTVEC per descriptor, `CPUCTL.STARTCPU`, wait for init complete.
- Done: `gr_fecs_ctxsw_mailbox_r(0)` returns `WFI_COMPLETE` (value 0x00000001 per legacy convention) or a known "ready" pattern. nvgpu `gp10b_gr_init_fecs_ctxsw_mailbox` is the reference.
- What goes wrong: FECS stays in its loader waiting for the host to write a method image — that's actually expected; it's waiting for golden ctx image setup.

**GPCCS (GPC Context Switch):**
- Base: `NV_PGRAPH_PRI_GPCS_GPCCS_FALCON` (per-GPC; GA10B has 1 GPC). Offset discovery via `gr_gpc0_gpccs_falcon_*` macros in L4T headers.
- Same start/halt pattern as FECS. Done on `gr_gpc0_gpccs_ctxsw_mailbox_r(0)`.

**PMU (Power Management Unit):**
- Base: `NV_PPWR_FALCON_*` at BAR0+0x00010a00 on GA10x (CHECK on GA10B — PMU may have moved).
- PMU boot has more ceremony: QUEUE_HEAD/TAIL registers establish a command queue; SLM-OS has to emit an init command (`PMU_CMD_INIT`) and wait for the response. Full init protocol in nvgpu `pmu_gk20a.c` + `pmu_gm20b.c` + GA10B-specific override.
- Done: PMU sends `PMU_INIT_MSG_INIT` back on its message queue with a descriptor of the DMEM heap layout.
- What goes wrong: wrong queue base (PMU allocates these from its own DMEM; we have to read the offsets back from PMU mailboxes after ACR halt, not hardcode them).

**Safety scheduler:** Skip. We aim for compute, not functional-safety dispatch. If something in the ACR boot chain refuses to start without safety-scheduler loaded too, we'll know by inspection.

### Phase F — Address space

This is where nvgpu diverges most from our existing code. GSP-RM on discrete Ampere manages all page tables from the GPU side; here, the CPU builds them.

**Instance block (inst_block):**
- 4 KB structure at the root of each channel's address space. Holds page directory base, engine state pointer, runlist pointer.
- Layout: see `nvgpu_mem_wr32` calls in nvgpu `channel_gk20a.c` / `ram_gk20a.c` — each engine has its own inst_block register window defined in `ram_gk20a.h`.

**GMMU page table:**
- GA10B uses a 5-level page table by default, but a 2-level (PDE3/PTE) config is supported for small address spaces and is what we want for a demo.
- Page size: 4 KB (small) or 64 KB (big). Demo: 64 KB big-page for everything, avoids aliasing with small-page PDEs.
- PDE/PTE encoding: `ram_gmmu_pde3_*` / `ram_gmmu_pte_*` in nvgpu headers.
- VA layout for demo:
  ```
  0x1000_0000  pushbuffer        (64 KB, RW, CACHED)
  0x2000_0000  gpfifo ring       (4 KB,  RW, UNCACHED — PBDMA polls)
  0x3000_0000  semaphore page    (4 KB,  RW, CACHED)
  ```

**BAR2 bind:**
- BAR2 is the "GPU page-table aperture" visible to the CPU — it's how the CPU writes into GPU VA space without going through PBDMA.
- `NV_PBUS_BAR2_BLOCK` register (offset in `dev_bus.h` — fetch required) takes the physical address of the BAR2 inst_block. BAR2 page tables then cover kernel VA.
- For the minimal demo we can probably skip BAR2 and write everything via CPU physmem + GMMU identity mapping for the pushbuffer. But channel allocation still needs per-channel inst_block programming.

**Done:** no single signal; we verify by DMA-reading a known pattern through PBDMA later.
**What goes wrong:**
- Wrong PDE format (PDE3 vs PDE0 encoding differs by Ampere generation).
- Forgetting to invalidate the TLB — nvgpu `nvgpu_mm_l2_flush` / `nvgpu_tlb_invalidate` hits `fb_mmu_invalidate_pdb_r` at a specific offset.
- Cache maintenance: the PDEs/PTEs are in system memory on Jetson (unified mem). Must `cache_clean` after writing them so the GPU's TLB walker sees the fresh values. The `gsp_platform_ops.cache_clean` vtable already has this primitive.

### Phase G — Channel + runlist

**Channel alloc:**
- Channel ID is just an index; nvgpu uses a bitmap. For demo, pick channel 0.
- Per-channel inst_block programmed with: page table root, ramfc (ring metadata), userd (submission mailbox).

**RAMFC (ring context):**
- Small block at a fixed offset in the inst_block. Stores gpfifo base/limit, PBDMA state.
- Fields: `ram_fc_gp_base`, `ram_fc_gp_base_hi`, `ram_fc_gp_put`, `ram_fc_gp_get`, `ram_fc_subdevice`, `ram_fc_signature`. Defined in nvgpu `ram_gk20a.h` with GA10B-specific deltas (new fields for semaphore reporting).

**Runlist:**
- A list of `(timeslice, channel_id)` entries the host scheduler picks from. For demo: one entry, infinite timeslice.
- `NV_PFIFO_RUNLIST_BASE_LO/HI`, `NV_PFIFO_RUNLIST`, `NV_PFIFO_RUNLIST_LENGTH` — per-engine on GA10B (the "host engine table" enumerates which runlist belongs to which engine). Offsets: nvgpu `fifo_gk20a.h` + GA10B override.
- Kick: write the runlist length register, which triggers PBDMA to fetch.

**Done:** `NV_PFIFO_RUNLIST_STATUS` clears the "pending" bit. PBDMA loads the channel.

**What goes wrong:**
- Host engine table not programmed → runlist goes to the wrong engine or gets rejected.
- Inst_block not committed to memory (cache_clean needed, same trap as page tables).
- GPFIFO entries not cache_cleaned before put-pointer update — PBDMA fetches stale data.

### Phase H — First compute method

Pushbuffer content for the absolute minimum (pseudo-assembly, Fermi method encoding):

```
# Bind the compute class to subchannel 0
METHOD 0x0000, 0xC1C0   # SUBCH_0, SET_OBJECT=AMPERE_COMPUTE_B (0xC1C0)

# Write a 32-bit payload via the SEMAPHORE method path
METHOD 0x0010, addr_lo  # LINE_LENGTH_IN (repurposed for semaphore; use
                        # the real SET_REPORT_SEMAPHORE_* offsets — see
                        # open-gpu-doc fermi_compute_b.h equivalent for
                        # Ampere compute B)
METHOD 0x0014, addr_hi
METHOD 0x0018, 0xCAFEF00D     # payload
METHOD 0x001C, SEMAPHORE_RELEASE_STRUCTURE_SIZE  # triggers write
```

Real offsets come from `open-gpu-doc/classes/compute/clc6c0.h` (for
AMPERE_COMPUTE_B = 0xC6C0, not C1C0 — C1C0 is Turing). **Fetch
required.** nvgpu also carries these in `clc6c0.h` under
`kernel/nvgpu/include/nvgpu/hw/`.

Gpfifo entry format (8 bytes, little-endian, per `pbdma_gp_entry0_*`):
- `entry0[31:2]` = pushbuffer VA[31:2], `entry0[1:0]` = 0
- `entry1[7:0]` = pushbuffer VA[39:32]
- `entry1[30:10]` = length in dwords
- `entry1[31]` = SYNC (end-of-segment)

**Done:** Poll the semaphore memory until it reads `0xCAFEF00D`. The CPU side uses `bar1_read`-equivalent (or direct sysmem read if the semaphore is in sysmem, which it will be for the demo). `cache_invalidate` before reading. Timeout: ~1 s.

---

## 3. Register Map — to verify against TRM / L4T source

This is an inventory of the registers we'll touch. **Offsets marked
"verify" are from discrete-Ampere openrm and may move on GA10B.** L4T
`nvidia-oot/drivers/gpu/nvgpu/hal/` has GA10B-specific register
redefinitions; fetch those before committing to addresses.

### BAR0 base (Jetson)
- Fixed at physical `0x17000000` (same as what SLM-OS uses today at EL2+VHE).
- CBB firewall: GPU registers were previously CBB-blocked from EL1; at EL2 they read `0xB7B000A1` for `BOOT_0`. Remains firewalled for some sub-apertures even at EL2 — UARTA, BPMP IPC, etc. GPU block itself: fully readable per our earlier tests.

### PMC (BAR0-absolute, verify vs L4T)
- `NV_PMC_BOOT_0`              = `0x00000000`
- `NV_PMC_BOOT_42`             = `0x00000a00`
- `NV_PMC_ENABLE`              = `0x00000200`
- `NV_PMC_INTR_EN(i)`, `NV_PMC_INTR_*` — we'll run IRQ-less for demo

### GSP Falcon (the ACR target)
On discrete Ampere, `NV_PGSP_BASE = 0x00110000`, `NV_PGSP_RISCV_BASE = 0x00111000`. **On GA10B, verify** — nvgpu `hal_gsp_ga10b.c` has the base. Existing `kernel/gpu/nvidia/falcon.h` definitions are reusable if bases match.

Since we want Falcon mode (not RISC-V) for ACR, the RISC-V BCR flow (`BCR_CTRL.CORE_SELECT = Falcon`) still applies — same helper `falcon_select_falcon_mode` we already have.

### PMU Falcon
- Discrete Ampere: `NV_PPWR_FALCON_*` around `0x00010a00`. GA10B-specific base: **fetch from L4T `pwr_gk20a.h`**.
- Same Falcon v4 register shape — reuse driver.

### FECS Falcon (inside PGRAPH)
- Openrm: `NV_PGRAPH_PRI_FECS_FALCON_*` around `0x00409000`.
- GA10B: confirm via nvgpu `hw/ga10b/hw_gr_ga10b.h`.

### GPCCS Falcon (per-GPC)
- `NV_PGRAPH_PRI_GPC0_GPCCS_FALCON_*` around `0x00502000`.
- GA10B: single GPC, but still the per-GPC stride — verify.

### PFIFO / PBDMA / Runlist
- `NV_PFIFO_RUNLIST_BASE/PTR/LENGTH` — per-runlist array on Ampere.
- Discrete Ampere uses the "host device enumeration table" to find runlist IDs; GA10B has a simpler fixed mapping.

### FB / GMMU
- `NV_PFB_PRI_MMU_*` around `0x00100cc0`.
- `NV_PFB_PRI_MMU_INVALIDATE` to kick TLB flushes.

### WPR (if ACR uses it — Tegra may not)
- On discrete Ampere, WPR is a hardware-enforced FB region. On Tegra unified memory, there is no equivalent "protected FB" — nvgpu uses a carveout managed by BPMP/HV. The ACR on Tegra still uses "WPR" terminology in its code but the semantics differ. Likely nothing for SLM-OS to program if BPMP already set up the carveout; if not, we need an MRQ to BPMP.

---

## 4. Firmware Header Formats

**These are the biggest unknowns.** The only format we already handle
cleanly is `nvkm_falcon_ucode_desc_v3` (FWSEC). Everything in
`/lib/firmware/nvidia/ga10b/` uses nvgpu's native headers, which are
different.

### ACR manifest (`acr-gsp.manifest.encrypt.bin.prod`)

**Hypothesis** (from nvgpu `drivers/gpu/nvgpu/common/acr/acr_bin_interface.h`):

```c
struct bin_hdr {
    u32 bin_magic;       // 0x10de for NVIDIA
    u32 bin_ver;
    u32 bin_size;
    u32 header_offset;   // offset to acr_fw_hdr
    u32 data_offset;     // offset to acr body
};

struct acr_fw_hdr {
    u32 hdr_size;
    u32 data_size;
    u32 sig_dbg_offset;  // offset of debug signature (0 on prod)
    u32 sig_prod_offset; // offset of prod RSA-3K signature
    u32 patch_loc;       // where in IMEM to patch the sig
    u32 patch_sig;       // which sig (prod/dbg) to use
    // HS-specific params follow — hs_acr_header or hs_fmc_params
};

struct hs_fmc_params {
    u8  hs_fmc;          // 1 = uses FMC (PKC) signing
    u8  padding[3];
    u16 pkc_algo;        // 1 = RSA3K
    u16 pkc_algo_version;
    u32 engid_mask;      // -> BROM_ENGIDMASK
    u32 ucode_id;        // -> BROM_UCODE_ID
    u32 fuse_ver;        // -> for sig-index select
    // PKC signature + key follow
};
```

**This is close to what we already parse for FWSEC** — but the outer
container (`bin_hdr`) is nvgpu, not openrm. We can't drop in the
existing `nvfw.c` parser unchanged.

### FECS / GPCCS (`fecs_encrypt_prod.bin`, `gpccs_encrypt_prod.bin`)

Pre-ACR pattern: header describes an IMEM load + DMEM load + boot vector. Under ACR, these blobs are consumed by ACR (not directly by SLM-OS) — ACR parses the headers internally. SLM-OS just concatenates them into the ACR WPR blob in a documented order.

**Critical:** the `*_pkc_sig_encrypt.bin` files are the detached signatures ACR uses to verify the encrypted ucodes. These get appended to the WPR blob at the offsets referenced by ACR's own metadata.

### PMU (`gpmu_ucode_next_prod_image.bin`, `_desc.bin`)

**Descriptor file** is typically `firmware_header_v1_t`:

```c
struct firmware_header_v1 {
    u32 bin_magic;
    u32 bin_ver;
    u32 bin_size;
    u32 header_offset;   // -> falcon_ucode_header_v1
    u32 data_offset;     // -> ucode body
};

struct falcon_ucode_header_v1 {
    u32 os_code_offset, os_code_size;
    u32 os_data_offset, os_data_size;
    u32 num_apps;
    // per-app entries follow
};
```

This is the format the `gpmu_ucode_next_prod_desc.bin` sidecar will
match. SLM-OS reads the descriptor to know how to split the image
file into IMEM/DMEM/app sections.

### NET images (`NET{A,B,C,D}_img_prod_encrypted.bin`)

These are runtime-loadable "overlay" images. For FP32 compute we
likely only need NETA (the default) — the B/C/D variants are for
safety fault-reset paths. Format: same `bin_hdr` container, body is
encrypted IMEM/DMEM pair.

**Action item:** fetch the following L4T files into `~/slmos-ref/` before coding:
- `drivers/gpu/nvgpu/common/acr/acr_bin_interface.h`
- `drivers/gpu/nvgpu/common/acr/acr_blob_construct.c` (or the version matching R36)
- `drivers/gpu/nvgpu/include/nvgpu/firmware_hdr.h`
- `drivers/gpu/nvgpu/hal/gr/gr/gr_ga10b.c`
- `drivers/gpu/nvgpu/hal/init/hal_ga10b.c`
- `open-gpu-doc/classes/compute/clc6c0.h`

---

## 5. What SLM-OS Already Has That Can Be Reused

| Module                                    | Reuse? | Notes                                                                                            |
| ----------------------------------------- | ------ | ------------------------------------------------------------------------------------------------ |
| `kernel/gpu/nvidia/falcon.{c,h}`          | **Yes, ~90%** | Generic Falcon driver: reset, mem-scrub wait, HWCFG probe, DMA upload, BROM program, STARTCPU/ALIAS, HALT wait. The register set and sequences are identical on GA10B. Only the BROM register base (`0x111000` / `0x841000`) may need a GA10B-specific constant — verify. |
| `kernel/gpu/nvidia/falcon.c:falcon_hs_boot` | **Yes** | Signed-ucode boot sequence is literally what ACR-GSP wants. Needs the right `engine_id`/`ucode_id`/`sig_offset` from the nvgpu manifest instead of the FWSEC v3 desc. |
| `kernel/gpu/nvidia/falcon.c:falcon_pre_dma_setup` | **Needs Jetson variant** | TRANSCFG target=VIDMEM hardcoded. Jetson needs TARGET=COHERENT_SYSMEM. Easiest: parameterize `falcon_pre_dma_setup(target)`. |
| `kernel/gpu/nvidia/gsp.h` vtable (`struct gsp_platform_ops`) | **Yes, all of it** | Abstractions (read32/write32/bar1/dma/cache/mb/firmware) are platform-shaped, not GSP-RM-shaped. Rename to `struct nvidia_gpu_platform_ops` at most — the interface is already right. |
| `kernel/gpu/nvidia/bringup.c` phases | **Throw away — but keep patterns** | The 7-phase state machine is GSP-RM-specific. We want a new `nvgpu_bringup.c` with its own phases A–H. But the *patterns* inside it (halt-wait with timeout, mailbox polling, cache_clean before DMA kick, priv-lock probe) all transfer. |
| `kernel/gpu/nvidia/nvfw.{c,h}` | **Partial** | Our FWSEC v3 descriptor parser won't work on nvgpu blobs. But the helper style (bounds-checked reader, endian conversion via `nv_endian.h`) is right. Write a sibling `nvgpu_fw.c`. |
| `kernel/gpu/nvidia/nvidia_vbios.c` | **No** | Jetson has no VBIOS; BIT-table parsing is not applicable. `gsp_platform_ops.vbios_get_fwsec` returns NULL on Jetson per contract. |
| `kernel/gpu/nvidia/rpc.{c,h}` | **No** | GSP-RM RPC; doesn't exist on GA10B nvgpu. |
| `kernel/gpu/nvidia/bringup.c:gsp_bringup_select_sig_index` | **Yes, same logic** | PKC sig-index selection from fuse register. Move to a shared helper; nvgpu ACR uses the same math but reads a different fuse register. |
| `host-tools/gsp-harness/` | **Adapt** | The harness framework (mock platform ops, test discovery) carries over directly. Tests themselves need new cases for nvgpu blob parsing, ACR boot, FECS/GPCCS mailbox polling. |
| `host-tools/gsp-harness/test_falcon.c` | **Yes, all of it** | Falcon-level tests (`test_start_uses_cpuctl_alias_when_en_set`, DMA upload, reset, priv-lock) are GPU-family-agnostic. Keep them. |
| `kernel/arch/arm64/nvidia_gsp_platform*.c` | **Use as template** | The Jetson platform stub is where the real MMIO + DMA + cache primitives get implemented; this was already the plan in `docs/nvidia-gsp.md` §"ARM64 Implementation Checklist". nvgpu bringup calls the same vtable. |

**What's net-new:**
- `kernel/gpu/nvidia/acr.{c,h}` — ACR manifest parsing, WPR blob construction, ACR bootstrap.
- `kernel/gpu/nvidia/gr.{c,h}` — PGRAPH init, FECS/GPCCS boot, golden ctx setup.
- `kernel/gpu/nvidia/pmu.{c,h}` — PMU boot + queue handshake. (Minimal variant: just boot and idle; no actual PM features needed for demo.)
- `kernel/gpu/nvidia/gmmu.{c,h}` — page table construction, TLB invalidate.
- `kernel/gpu/nvidia/fifo.{c,h}` — channel / inst_block / runlist / PBDMA kick.
- `kernel/gpu/nvidia/compute.{c,h}` — pushbuffer builder, method encoding, AMPERE_COMPUTE_B class state.

---

## 6. Divergence from Discrete Ampere

| Aspect                        | Discrete Ampere (GA107 etc.)                                 | GA10B (Jetson)                                          |
| ----------------------------- | ------------------------------------------------------------ | ------------------------------------------------------- |
| Root of trust                 | SEC2 Booter Load verifies GSP-RM                            | **ACR on GSP Falcon** verifies all LS ucodes           |
| Resource manager              | 38 MB RISC-V ELF (`gsp-*.bin`) runs on GSP core             | **No RM ucode** — CPU drives everything directly       |
| RPC ring to GPU               | LibOS message queues in FB                                   | **Direct MMIO + PBDMA** — no RPC                       |
| FWSEC / FRTS / WPR2           | VBIOS FWSEC sets up WPR2 before GSP-RM load                 | **Skip entirely** — no VBIOS, no WPR2 concept          |
| Memory model                  | Discrete FB + BAR1 aperture                                  | **Unified memory** — sysmem mapped via GMMU            |
| Booter Load                   | Separate SEC2 Falcon ucode                                   | **No Booter Load** — ACR does this                     |
| PMU firmware                  | `gsp-rm` handles PMU internally                              | **SLM-OS loads + boots PMU directly**                  |
| FECS/GPCCS                    | GSP-RM loads them                                            | **SLM-OS loads + boots directly (via ACR)**            |
| Channel allocation            | Goes through GSP-RM RPC                                       | **Direct inst_block / runlist programming**            |
| Page tables                   | Managed by GSP-RM                                            | **CPU-built GMMU tables**                              |
| Security/Priv levels          | GSP-RM raises to L2+; we stay at L0                         | **Similar PLM gates**; BPMP may hold some locked until MRQ |

**Same:**
- Falcon v4 engine (same registers, same DMA, same BROM flow)
- Ampere shader core / TPC / warp / SM architecture — the compute side
- Pushbuffer / PBDMA / gpfifo / runlist protocol (PBDMA hasn't changed since Maxwell)
- GMMU architecture (page table shape evolved but same idea)
- Method encoding for compute classes (AMPERE_COMPUTE_B on both)
- PKC-RSA3K signing algorithm for HS ucodes

---

## 7. Order of Difficulty

Sorted by "how much new code + debug cycles":

1. **Phase A (sanity)** — free, already done at EL2+VHE.
2. **Phase B (PMC.ENABLE)** — trivial, <50 LOC.
3. **Phase C (FBIF)** — trivial, existing helper with one new parameter.
4. **Phase D (ACR)** — **the hard unknown**. Existing Falcon driver does the heavy lifting once manifest parsing is correct. Biggest risk: nvgpu blob format discovery + sig-index math + BPMP / priv-lock gotchas. Budget: 2–3 sessions of hardware iteration.
5. **Phase E (FECS/GPCCS/PMU)** — mostly "start, poll mailbox, hope". Budget: 1 session per engine. PMU is the hardest of the three because of the queue handshake.
6. **Phase F (GMMU + inst_block)** — net-new code but deterministic once the PDE/PTE encoding is correct. Pure CPU-side work, lots of tests possible in the host harness. Budget: 1–2 sessions.
7. **Phase G (runlist + channel)** — net-new but again deterministic. The PBDMA is stateless until it gets a runlist; we can dump its status registers freely. Budget: 1 session.
8. **Phase H (compute method)** — once G works, H is appending the right byte pattern. Budget: <1 session if the semaphore fires; debug cycles if not.

**Total net-new code estimate:** ~2500 LOC kernel, ~1500 LOC host tests,
plus reference extraction.

**Biggest risk item:** ACR-GSP (D). If the manifest format is
different enough from FWSEC v3 that our existing BROM helper path
doesn't apply without refactor, add another session.

**Second-biggest risk:** PMU init (E). Queue handshake is protocol,
not just register poking; can silently hang if the DMEM heap offsets
are read from the wrong mailbox.

**Lowest risk:** F/G/H — everything is documented register layout,
no signed code involved after ACR halts successfully.

---

## 8. Gotchas

### From prior art (FWSEC / Booter Load / GSP-RM)

**Still apply:**
- `BOOTVEC = 0` for v3/v4 signed ucodes. nvgpu ACR is v1-container + hs_fmc_params; assume same rule unless proven otherwise.
- `CPUCTL.ALIAS_EN` honor at STARTCPU. Write to `CPUCTL_ALIAS (0x130)`, not `CPUCTL (0x100)`.
- DMEM signature patch location from manifest, not built-in.
- Sig-index selection via fuse-version register + `fls32` math.
- Radix-style page tables: nvgpu uses GMMU which is not the FB radix3 from GSP-RM, but the same "cache-clean before GPU reads" rule applies.
- Priv-lock poison pattern (`0xbadfXXXX`) when PLM is set too high — same detection helper `falcon_is_priv_locked` works.

**Do NOT apply:**
- WPR2 hardware region registers (`0x1FA824/28`) — no WPR2 on GA10B.
- FWSEC-FRTS — no VBIOS, no FRTS.
- GspFwWprMeta — no GSP-RM.
- Radix3 page tables for GSP-RM — nvgpu uses flat GMMU page tables instead.

### New on GA10B

- **BPMP clock/reset gating:** Linux's `scripts/jetson-kexec-slmos.sh` helper power-gates the GPU before kexec (to stop nvgpu's RAS errors). For SLM-OS nvgpu bringup, we need the **opposite** — GPU ungated. Either: (a) modify the kexec helper to *not* suspend, or (b) send BPMP `MRQ_CLK` / `MRQ_RESET` from SLM-OS to re-ungate. BPMP is at the HSP mailbox interface; complicates the whole bringup because BPMP needs a functioning doorbell.
- **Unified memory + cache coherency:** the entire GPU / CPU address space is system memory. `cache_clean` on PTEs, inst_blocks, pushbuffers, gpfifo entries before any GPU-side read. `cache_invalidate` on semaphore/status memory before CPU read. This is where Jetson will bite us repeatedly if we miss a single step.
- **SMMU / IOVA:** GA10B's GMMU is a bypass option when SMMU is set up identity, but L4T's default boot leaves SMMU programmed for nvgpu's specific StreamID. Bare-metal SLM-OS either (a) tears SMMU down (risky, BPMP may have opinions) or (b) matches nvgpu's StreamID when programming GMMU. Simplest: use the same StreamID L4T left configured and hope for the best.
- **Host engine table / runlist routing:** Ampere changed how PBDMAs/runlists map to engines. GA10B-specific.
- **ALIAS_EN / priv-lock post-kexec:** SLM-OS already has this pattern from x86-64 investigation (#185). Expect similar story on Jetson — BPMP may hold GSP PLM high until L4T normally sends an MRQ.
- **NETA etc. overlays:** ACR may mandate loading at least NETA before halting. Unknown whether this is mandatory for compute-only bringup; treat as "if ACR errors out after verifying FECS/GPCCS, try adding NETA."
- **Mixed encrypted / plain ucodes:** the `_encrypt_prod` suffix means the body is encrypted; ACR decrypts in place. We never see plaintext IMEM from the CPU side. Implications: no static verification of ucode content, harder debugging. Count on ACR's mailbox codes.
- **Multiple security domains on GSP Falcon:** nvgpu code has references to "HS" (heavy-secure), "LS" (light-secure), and "NS" (non-secure) ucode tiers. ACR runs HS; FECS/GPCCS run LS; the demos we want to run stay in NS. BROM flags determine the resulting privilege level of the Falcon after boot.

### Jetson-wide gotchas (already known but worth listing)

- **LSE atomics pre-MMU** — not a GPU bringup issue, but affects any kernel init we route through the scheduler.
- **Pre-MMU MMIO:** SLM-OS currently runs MMU-on from early init, so GPU code will always run in a sane memory environment. No pre-MMU gotcha.
- **UARTC only:** our debug output goes through UARTC → TCU → USB-C. If the GPU bringup hangs and needs to be debugged, that's our only channel. No JTAG available.

---

## 9. Minimal Demo Path ("First Compute Method")

**Goal:** From SLM-OS shell, run a single command (e.g. `gpucompute`)
that:

1. Brings up ACR → FECS → GPCCS → PMU (Phases B–E)
2. Builds one GMMU address space, one channel (Phase F–G)
3. Submits a 4-method pushbuffer: SET_OBJECT, SET_REPORT_SEMAPHORE_A/B/PAYLOAD, SEMAPHORE_RELEASE
4. Reads the semaphore memory and prints the payload value
5. Expected output:
   ```
   [gpu] ACR boot: ok (2.1 s)
   [gpu] FECS/GPCCS/PMU: ok
   [gpu] channel 0: loaded
   [gpu] pushbuffer submitted: 32 bytes
   [gpu] semaphore: 0xCAFEF00D  <-- target
   [gpu] first compute method executed OK
   ```

**Gate before real matmul:** this demo proves the full stack works end-to-end without the complexity of a precompiled SASS kernel, register file setup, or DRAM-resident tensor data. It isolates the bringup from the "does my compute kernel actually run" question.

**After the gate passes:** switch the pushbuffer to load a 4×4 FP32 matmul kernel (precompiled offline with nvcc to CUBIN and disassembled to SASS methods). That's issue #16 / task item #16 territory.

---

## 10. Open Questions / Next Actions

Before writing a single line of new kernel code:

1. **Fetch L4T nvgpu sources** into `~/slmos-ref/`:
   - `drivers/gpu/nvgpu/common/acr/acr_bin_interface.h`
   - `drivers/gpu/nvgpu/common/acr/acr_blob_construct_v1.c` (or matching R36 variant)
   - `drivers/gpu/nvgpu/include/nvgpu/firmware_hdr.h`
   - `drivers/gpu/nvgpu/hal/init/hal_ga10b_init.c`
   - `drivers/gpu/nvgpu/hw/ga10b/hw_pwr_ga10b.h`, `hw_gr_ga10b.h`, `hw_fifo_ga10b.h`, `hw_pbdma_ga10b.h`, `hw_ram_ga10b.h`, `hw_fb_ga10b.h`
   - `open-gpu-doc/classes/compute/clc6c0.h` (AMPERE_COMPUTE_B method numbers)
2. **Dump the actual firmware headers** from `/lib/firmware/nvidia/ga10b/` on jetson-nano-2 — read first 256 bytes of each and match byte-by-byte against the hypothesized `bin_hdr` / `acr_fw_hdr` structs. A 30-minute exercise that settles §4 unknowns.
3. **Probe BAR0 register accessibility at EL2** for the sub-apertures we care about — does `NV_PFB_PRI_MMU_*` read? Does `NV_PGRAPH_PRI_FECS_FALCON_HWCFG2` read without `0xbadf` poison? Can be done from SLM-OS shell with a new `gpuprobe` diag command, minimal code.
4. **Decide BPMP strategy:** either modify kexec helper to skip GPU suspend, or build a minimal BPMP MRQ sender in SLM-OS. The latter is cleaner long-term; the former is faster to get us to the first ACR boot.
5. **File a GitHub issue per phase** (or reuse #142) — track per-phase progress separately so "stuck on PMU queue handshake" is distinguishable from "stuck on ACR manifest parsing" in the project log.

Once 1–3 are done, coding can start against a concrete register map
and a verified manifest layout. Phase D (ACR) is the natural first
milestone — "ACR HALTs with success code" is a well-defined gate
that exercises almost all of the existing Falcon driver.

---

## 11. References

**Cached in `~/slmos-ref/`:**
- `nouveau-nvfw-acr.h` — `wpr_header`, `lsb_header`, `flcn_acr_desc` (discrete-Ampere ACR shapes; related but not identical to Tegra ACR)
- `nouveau-nvfw-flcn.h` — `loader_config`, `flcn_bl_dmem_desc` (Falcon bootloader descriptors)
- `nouveau-falcon-hs-boot.md` — distilled HS boot + BROM register sequence (applies verbatim)
- `nouveau-falcon-ga102.c` — ga102 Falcon implementation (applies)
- `nvidia-ampere-ga102-dev_falcon_v4.h` — Falcon v4 register layout (shared across GA10x)
- `nvidia-ampere-ga102-dev_falcon_second_pri.h` — BROM register offsets (verify on GA10B)
- `nvidia-ampere-ga102-dev_gsp.h` — GSP Falcon engine layout
- `linux-nova-core-falcon*.rs` — Rust port of the Falcon driver; useful as a second source of register semantics
- `nvidia-gsp-bringup-sequence.md` — Discrete Ampere GSP bringup (sibling path, not applicable)

**To fetch (L4T R36-ish or NVIDIA/jetson_nvgpu mirror):**
- See §10 item 1 for the list.

**Related SLM-OS docs:**
- `docs/nvidia-gsp.md` — discrete-Ampere sibling research
- `docs/jetson-el2-bringup.md` — how we got BAR0 accessible
- `docs/jetson-nvidia-support.md` — CBB firewall + historical investigation
- `docs/jetson-capstone-gap-analysis.md` — where this work fits in the capstone plan

---

*Research doc authored: April 2026. Supersedes the "Jetson compute
blocked by proprietary firmware" framing from `docs/capstone-thesis-framing.md`
by identifying a concrete (if long) bringup path.*
