# L4T nvgpu GA10B ACR bring-up — source analysis

Sources: [OE4T/linux-nvgpu@l4t/l4t-r36.5](https://github.com/OE4T/linux-nvgpu/tree/l4t/l4t-r36.5)
(closest published branch to the stated L4T r36.4.7 — r36.4.7 is not published as
a separate branch in OE4T/linux-nvgpu; r36.5 is the immediate successor and is the
same tree modulo a handful of bug fixes that do not touch ACR).

All files referenced below are cached locally under `~/slmos-ref/` with the
`nvgpu-<subpath>-<file>.<ext>` naming convention listed in the summary section.

## Executive summary

On GA10B (Orin GPU) the ACR-HS workload runs **on the GSP RISCV core (Falcon2)**,
not on PMU or SEC2 — this is radically different from Turing/GA10x desktop,
where ACR runs on SEC2 Falcon. The firmware triplet

```
acr-gsp.manifest.encrypt.bin.out.bin.prod    2048 B (PKC manifest — BROM input)
acr-gsp.text.encrypt.bin.prod               28672 B (RISCV code → IMEM)
acr-gsp.data.encrypt.bin.prod                  ~* (RISCV data → DMEM)
```

is loaded exactly as in the nvgpu path `nvgpu_acr_bootstrap_hs_ucode_riscv()`
(`nvgpu-common-acr-acr_bootstrap.c:360–419`). The manifest is **not** wrapped
in a `bin_hdr` + `acr_fw_header`; it is consumed as-is by the RISCV BROM.

Bootstrap owner (`nvgpu-common-acr-acr_sw_ga10b.c:526`):

```c
acr->bootstrap_owner = FALCON_ID_GSPLITE;
```

Managed LS falcons (`acr_sw_ga10b.c:451–467`): `FALCON_ID_PMU`,
`FALCON_ID_FECS`, `FALCON_ID_GPCCS`. PMU is optional (`support_ls_pmu`); FECS
and GPCCS are mandatory. Both LS ctxsw falcons have
`is_lazy_bootstrap=true` when LSPMU is present — meaning after the ACR blob
authentication, **the LSPMU (if loaded) is the one that actually programs
FECS/GPCCS IMEM/DMEM and starts them**, not the ACR. If LSPMU is skipped, ACR
itself does the bootstrap (comments at `acr_sw_ga10b.c:408–416, 435–441`).

## A. Manifest (`acr-gsp.manifest.encrypt.bin.out.bin.prod`, 2048 B)

**Not a `bin_hdr`/`acr_fw_header` wrapper.** The manifest is the raw
PKC-parameter blob consumed by the GSP RISCV BootROM.

Evidence from `nvgpu-common-riscv-riscv.c:55–111`
(`nvgpu_riscv_hs_ucode_load_bootstrap`):

```c
err = nvgpu_falcon_reset(flcn);
nvgpu_falcon_mailbox_write(flcn, FALCON_MAILBOX_0, u64_lo32(desc_addr));
nvgpu_falcon_mailbox_write(flcn, FALCON_MAILBOX_1, u64_hi32(desc_addr));
g->ops.falcon.set_bcr(flcn);                                 /* write BCR_CTRL = 0x11 */
nvgpu_falcon_copy_to_imem(flcn, 0x0, code_fw->data, code_fw->size, ...);
nvgpu_falcon_copy_to_dmem(flcn, 0x0, data_fw->data, data_fw->size, 0);
nvgpu_falcon_copy_to_dmem(flcn, dmem_size - manifest_fw->size,
                           manifest_fw->data, manifest_fw->size, 0);
g->ops.falcon.bootstrap(flcn, 0x0);                          /* STARTCPU */
```

So the manifest is copied **verbatim** to the top of DMEM, and BROM consumes
it after `STARTCPU` is written. The 2048 B size matches **`RSA3K_PK_SIZE_BYTE`**
in `nvgpu-common-acr-nvgpu_acr_interface_v2.h:60`, which is the size of the
RISCV PKC-parameter block (see Turing reference in
`~/slmos-ref/nouveau/nouveau-nvfw-acr.h` — same shape):

```
struct rm_riscv_pkc_param {
    u8  pkc_signature[384];          /* RSA-3K signature */
    u8  pkc_signature_pad[128];
    u8  pkc_public_key[1536];        /* 12288-bit modulus + exponent etc. */
}
```

There is **no header**. The BROM already knows the file length (2048) and
parses the fields by fixed offsets. App-version and ucode-ID fields live
**inside the code image** (not the manifest) in RISCV ACR — see
`nvgpu_acr_interface_v2.h:62–74` `HS_FMC_PARAMS`. Boot vector is hard-coded
into the RISCV code at link time; there is no separate BOOTVEC field (the
`bootstrap(flcn, 0x0)` call at `falcon_ga10b_fusa.c:60–72` passes
`boot_vector==0`, so the `PRISCV_RISCV_BOOT_VECTOR_{LO,HI}` registers are
**not** written — the RISCV FMC uses whatever the BROM placed there after
authentication, which is the in-IMEM entry point).

**Important** — note at `falcon_ga10b_fusa.c:138–161` `ga10b_falcon_brom_config()`:
this function writes BCR_CTRL=**`0x111`** (explicit DMA-load mode with
`BCR_DMACFG_TARGET_NONCOHERENT_SYSTEM | LOCK`). The path invoked here in
nvgpu is `set_bcr(flcn)` → **`0x11`** (preloaded mode). So the SLM-OS port
picks one of:

* **`0x11` (preloaded)** — copy code to IMEM, data to DMEM low, manifest to
  DMEM-top ourselves (matches what we get from `/lib/firmware`). Simpler; no
  need to DMA-map the firmware buffers. **Recommend this path.**
* **`0x111` (DMA-load)** — write the 3 sysmem physical addresses into
  `BCR_DMAADDR_FMCCODE_{LO,HI}`, `BCR_DMAADDR_FMCDATA_{LO,HI}`,
  `BCR_DMAADDR_PKCPARAM_{LO,HI}`, set `BCR_DMACFG`, then `BCR_CTRL=0x111`, and
  BROM DMAs from sysmem directly. Requires a physically contiguous, IOMMU-mapped
  buffer for each of the three.

## B. `fecs_encrypt_prod.bin` (272 B) vs `fecs_pkc_sig_encrypt.bin` (2248 B)

Both filenames are hard-coded in nvgpu source:

* `fecs_encrypt_prod.bin` → `NVGPU_FECS_ENCRYPT_PROD_UCODE_IMAGE`
  (`nvgpu-hal-gr-falcon-gr_falcon_ga10b_fusa.c:40, 57`). Consumed by
  `ga10b_gr_falcon_get_fw_name()`, which is called from
  `nvgpu_gr_falcon_init_ctxsw_ucode()` in `common/gr/gr_falcon.c`.

* `fecs_pkc_sig_encrypt.bin` → `GA10B_FECS_UCODE_ENCRYPT_PKC_SIG`
  (`nvgpu-common-acr-acr_priv.h:95`). Consumed by
  `nvgpu_acr_lsf_fecs_ucode_details()` in `acr_blob_construct.c:181–184`.

> **Hypothesis test** — the user asked whether the 272 B file is a descriptor
> pointing into the `.text` blob. Source says **no**. The 272 B file really is
> the FECS ucode image (encrypted). 272 B is a plausible size for FECS **bootloader
> only** — NVIDIA's FECS ucode is historically built from three segments (`boot`,
> `code`, `data`), and the `app_start_offset`/`app_size` in
> `ls_falcon_ucode_desc` (`nvgpu-common-acr-acr_blob_construct.h:61–73`) describes
> the full image. The Linux nvgpu driver reads `fecs_encrypt_prod.bin` directly
> into the `ctxsw_ucode_surface` via `nvgpu_gr_falcon_init_ctxsw_ucode()`. So on
> reflection, 272 B is almost certainly **not** the full FECS ucode — it's more
> likely a header/trampoline, with the real ucode carried **inside the ACR blob**
> in the `.text` or compiled into the nvgpu driver image on other platforms.
>
> **Open** — we have not found source that confirms the 272 B breakdown. The
> nvgpu path explicitly reads this file as "the FECS ucode" and pushes it into
> a ctxsw surface, so if it really is 272 B on disk, one of the following is true:
>
> 1. The distro `fecs_encrypt_prod.bin` is a short-circuited stub and the real
>    code lives inside the acr-gsp `.text`/`.data` blobs (ACR-managed LS ucode).
>    This is **plausible** given `is_lsf_encrypt_support=true` in
>    `acr.c:165` — ACR handles decryption itself.
> 2. The 272 B on the Jetson corresponds to a **different** binary than the one
>    nvgpu source expects — the L4T packaging may have moved the real ucode
>    into the ACR blob's `.data`, and the 272 B file is a legacy stub kept for
>    backward-compat or for some intermediate parser.
>
> Either way: **do not rely on `fecs_encrypt_prod.bin` alone**. Plan to extract
> the FECS plaintext from whatever the ACR produces after authentication
> (Option 1). Re-verify this once we can sniff the ACR execution on hardware.

* `fecs_pkc_sig_encrypt.bin` (2248 B) = `struct lsf_ucode_desc_wrapper`:
  `acr_generic_header` (8 B) + `lsf_ucode_desc_v2` — the latter has
  `prod_sig[2][512]` + `debug_sig[2][512]` = 2048 B of signatures plus ~192 B
  of metadata (falcon_id, sig_size, dep_map, ls_ucode_version, ls_ucode_id,
  encryption IV — `nvgpu_acr_interface.h:299–323`). Total ≈ 2240 B; with
  8-byte generic header and some padding, 2248 B is correct.

## C. NET images (NETA/B/C/D, ~143 KB each)

**"NET" = "netlist"**, not network-mode. `NVGPU_NETLIST_PROD_IMAGE_{A,B,C,D}`
are defined in `nvgpu-hal-netlist-netlist_ga10b.h:38–41`. The comment at
`nvgpu-hal-netlist-netlist_ga10b.h:30, 43–45` is decisive:

```c
/* NVGPU_NETLIST_IMAGE_C is FNL for ga10b */
#define GA10B_NETLIST_IMAGE_FW_NAME NVGPU_NETLIST_IMAGE_C
#define GA10B_NETLIST_DBG_IMAGE_FW_NAME  NVGPU_NETLIST_DBG_IMAGE_C
#define GA10B_NETLIST_PROD_IMAGE_FW_NAME NVGPU_NETLIST_PROD_IMAGE_C
```

**FNL = Final Netlist.** On production (FUSA) builds only `NETC` is used.
`NETA/B/D` only compile in when `CONFIG_NVGPU_NON_FUSA` is set
(`netlist_ga10b_fusa.c:45, 105`). The four slots are an emulation/simulation
feature — each slot matches a different GPU microarch variant during
pre-silicon bring-up. On production silicon, `ga10b_netlist_silicon_get_name()`
returns `NETC` unconditionally (`netlist_ga10b_fusa.c:37–43`).

Netlist content: **register tables** used by FECS/GPCCS to save/restore
context during a context switch — essentially a list of (register-offset,
value) pairs grouped by engine/GPC/TPC hierarchy. It is consumed by the
ctxsw code, not by ACR.

**SLM-OS picks** via the same build switch the prod driver uses (`is_debug_mode_enabled`
— `pgsp_falcon_hwcfg2_dbgmode_v`, `gsp_ga10b.c:125–137`). For retail Orin
Nano (fused prod), **load NETC only** and ignore A/B/D.

## D. ACR / GSP register map

GSP Falcon base (BAR0 offset): **`0x00110000`**
(`nvgpu-hw-ga10b-hw_pgsp_ga10b.h:63` — `pgsp_falcon_irqsset_r()`).
GSP RISCV (Falcon2) base: **`0x00111000`**
(`hw_pgsp_ga10b.h:62` — `pgsp_falcon2_gsp_base_r()`).

On Jetson Orin GPU the BAR0 starts at **`0x17000000`**, so absolute addresses:

| Region                    | Abs. addr        |
|---------------------------|------------------|
| GSP Falcon block start    | `0x17110000`     |
| GSP Falcon IRQSSET        | `0x17110000`     |
| GSP Falcon IRQSCLR        | `0x17110004`     |
| GSP Falcon IRQSTAT        | `0x17110008`     |
| GSP Falcon HWCFG2         | `0x171100f4`     |
| GSP Falcon NXTCTX         | `0x17110054`     |
| GSP Falcon MAILBOX0/1     | via mailbox_read/write ops |
| GSP Falcon ECC_STATUS     | `0x17110878`     |
| GSP RISCV block start     | `0x17111000`     |
| PRISCV CPUCTL             | `0x17111388`     |
| PRISCV BR_RETCODE         | `0x1711165c`     |
| PRISCV BCR_CTRL           | `0x17111668`     |
| PRISCV BCR_DMAADDR_PKCPARAM_LO/HI | `0x17111670`/`0x17111674` |
| PRISCV BCR_DMAADDR_FMCCODE_LO/HI  | `0x17111678`/`0x1711167c` |
| PRISCV BCR_DMAADDR_FMCDATA_LO/HI  | `0x17111680`/`0x17111684` |
| PRISCV BCR_DMACFG         | `0x1711166c`     |
| PRISCV BOOT_VECTOR_LO/HI  | `0x17111380`/`0x17111384` |
| PRISCV IRQMASK            | `0x17111528`     |
| PRISCV IRQDEST            | `0x1711152c`     |

All offsets are from `nvgpu-hw-ga10b-hw_priscv_ga10b.h:62–82`.

**ACR itself touches no other BAR0 region.** The "blob" lives in sysmem
(allocated via `nvgpu_dma_alloc_flags_sys(... PHYSICALLY_ADDRESSED)` at
`acr_sw_ga10b.c:125–128`) and ACR accesses it via the RISCV DMA engine
using the physical address handed over in MAILBOX0/1 (low/high halves). The
blob layout inside sysmem is the standard WPR format (WPR header → LSB
header per LS-falcon → bootloader-desc → LS ucode image), described in
`nvgpu_acr_interface.h:43–46`.

## E. Bring-up order

Validated by reading `acr.c:92–113` (`nvgpu_acr_construct_execute`):

```c
int nvgpu_acr_construct_execute(struct gk20a *g) {
    err = g->acr->prepare_ucode_blob(g);     /* 1. blob-build in sysmem  */
    err = nvgpu_acr_bootstrap_hs_acr(g, g->acr); /* 2. bootstrap ACR on GSP */
}
```

With `prepare_ucode_blob = nvgpu_acr_prepare_ucode_blob_v2` on GA10B
(`acr_sw_ga10b.c:533`) — see `acr_blob_construct_v2.c:490` — the sequence is:

1. **FECS/GPCCS ctxsw ucode load** from filesystem (`nvgpu_gr_falcon_init_ctxsw_ucode()`
   at `acr_blob_construct_v2.c:506`, implementation in
   `common/gr/gr_falcon.c:300+`). Parses bootloader/code/data segments and
   writes them into a sysmem surface. **This happens BEFORE ACR runs — the ACR
   just re-reads the same surface.**
2. **LS-falcon config enumeration** (`ga10b_acr_lsf_config` at
   `acr_sw_ga10b.c:451`) — populates PMU/FECS/GPCCS entries in `acr->lsf[]`
   each with an ucode-details callback.
3. **Per-LSF ucode detail fetch** — reads `fecs_pkc_sig_encrypt.bin`,
   `gpccs_pkc_sig_encrypt.bin` (and `pmu_pkc_sig.bin` if LSPMU) as
   `lsf_ucode_desc_wrapper` and fills `struct flcn_ucode_img` per LSF.
4. **WPR blob assembly in sysmem** (`lsfm_init_wpr_contents` in
   `acr_blob_construct_v2.c`) — builds the WPR-header → LSB-header → ucode
   layout.
5. **Descriptor for ACR** (`ga10b_safety_acr_patch_wpr_info_to_ucode`,
   `acr_sw_ga10b.c:79–202`) — fills a `RM_RISCV_ACR_DESC_WRAPPER` with
   `nonWprBlobStart` = phys addr of the ucode_blob, and a `mode` bitmask for
   MIG/emulate/simulation.
6. **Bootstrap ACR on GSP** (`nvgpu_acr_bootstrap_hs_ucode_riscv` at
   `acr_bootstrap.c:360`). Loads manifest/code/data to GSP, waits for BROM
   `RETCODE_PASS` (`falcon_ga10b_fusa.c:108–135`), then waits for the
   RISCV ACR to halt with MAILBOX0==0 (`acr_bootstrap.c:137–153`).
7. **LS falcon bootstrap follows** — either:
   * LSPMU is up (`is_lazy_bootstrap=true`) and *it* programs FECS/GPCCS.
   * No LSPMU (`is_lazy_bootstrap=false`) and ACR itself programs FECS/GPCCS
     inside step 6 before halting.
8. **FECS ctxsw boot** (on LS) — handled inside FECS; host driver then waits
   on `gr_fecs_ctxsw_mailbox_r(0)` for `FECS_ARB_SET_DEFAULT_MAILBOX0_COLDBOOT`
   (`gr_falcon_ga10b_fusa.c`). FECS and GPCCS **must be started before any
   method flies** — the host sends its first ctxsw method via FECS.

**Hidden dependencies:**

* PMU (optional) does **not** need to be up before FECS — on Jetson with
  `support_ls_pmu=false`, there is no LSPMU at all. In that mode ACR directly
  starts FECS/GPCCS.
* GSP must be at least minimally booted into "NS bootstrap" state before ACR
  runs — but that's just a reset + IMEM/DMEM load + CPUCTL start, which is
  what the ACR-load itself is anyway. There is **no separate** "GSP NS FW"
  we have to load first for the ACR path (unlike the GSP-RM flow on desktop).

## F. Absolute minimum boot

For "GPU accepts a method" (i.e., you can push a host-channel method and see
FECS respond with a ctxsw mailbox update), the bare-minimum set is:

* **ACR blob load + bootstrap on GSP** — authenticates FECS + GPCCS, places
  them into WPR, starts them.
  * Firmware needed: `acr-gsp.manifest.encrypt.bin.out.bin.prod`,
    `acr-gsp.text.encrypt.bin.prod`, `acr-gsp.data.encrypt.bin.prod`,
    `fecs_encrypt_prod.bin`, `fecs_pkc_sig_encrypt.bin`,
    `gpccs_encrypt_prod.bin`, `gpccs_pkc_sig_encrypt.bin`.
* **NETC netlist** (`NETC_img_prod_encrypted.bin`) — register table for
  ctxsw.
* **Host-side engine init** — GR register init (reset de-assert, clock gating
  bypass, MMU setup, bundle-init list). Most of this is software, not firmware.

**LSPMU can be skipped.** `support_ls_pmu` is a nvgpu build-time knob and on
Jetson iGPU the driver often runs with it false. Effects of skipping PMU:

* GPU runs at default / max clocks (no DVFS via PMU). OK for prototyping.
* No GR power-gating (ELPG). OK for prototyping.
* No FECS recovery path. Catastrophic errors will require a full re-bootstrap.
* `is_lazy_bootstrap=false` is then set for FECS/GPCCS at `acr_sw_ga10b.c:417,
  443`, which **forces the ACR to program FECS/GPCCS directly**. This is the
  simpler path.

**GPCCS can NOT be skipped** on a 1-GPC part. The GR front-end (FE) talks to
both FECS (host-side ctxsw) and GPCCS (per-GPC ctxsw). Host methods route to
FE → FECS → GPCCS[0]. Skipping GPCCS means the first method will hang in
GPCCS.

**Recommended minimum for first "hello world" method:** ACR triplet + FECS
prod + FECS PKC sig + GPCCS prod + GPCCS PKC sig + NETC + our own host-side
GR init (registers) + a host channel pointing at a pushbuffer with a single
`GET_GPU_INFO` method (NVC597 class). Skip PMU entirely.

## Files cached (58 new files under `~/slmos-ref/`)

### ACR framework (common)
* `nvgpu-common-acr-acr.c` — top-level init + construct/execute
* `nvgpu-common-acr-acr_bootstrap.{c,h}` — HS ucode load (Falcon + RISCV paths);
  contains `struct bin_hdr` and `struct acr_fw_header` (Falcon path only)
* `nvgpu-common-acr-acr_blob_alloc.{c,h}` — WPR space allocation
* `nvgpu-common-acr-acr_blob_construct.{c,h}` — v0/v1 blob layout + LS ucode
  detail fetchers (reads `*_pkc_sig*.bin`, fecs_sig.bin etc.)
* `nvgpu-common-acr-acr_blob_construct_v2.{c,h}` — v2 PKC blob layout used by
  GA10B; writes `LSF_WPR_HEADER_WRAPPER`, `LSF_LSB_HEADER_WRAPPER` into sysmem
* `nvgpu-common-acr-acr_wpr.{c,h}` — WPR carveout info (HW-driven via
  `read_wpr_info`)
* `nvgpu-common-acr-acr_priv.h` — filename macros, `struct nvgpu_acr`
* `nvgpu-common-acr-nvgpu_acr_interface.h` — v0/v1 ABI: `bin_hdr`,
  `acr_fw_header`, `flcn_bl_dmem_desc`, `lsf_wpr_header`, `lsf_lsb_header`,
  `flcn_acr_desc`, `flcn2_acr_desc`
* `nvgpu-common-acr-nvgpu_acr_interface_v2.h` — v2 ABI: `HS_FMC_PARAMS`,
  `RM_RISCV_ACR_DESC_V1/_WRAPPER`, `LSF_LSB_HEADER_V2`, PKC sizes
* `nvgpu-include-nvgpu-acr.h` — public API + prose docs
* `nvgpu-include-gops-acr.h` — HAL op table
* `nvgpu-include-pmuif-acr.h` — PMU↔ACR RPC

### ACR per-chip SW
* `nvgpu-common-acr-acr_sw_ga10b.{c,h}` — **GA10B entry point** (`ga10b_bootstrap_hs_acr`,
  `ga10b_safety_acr_patch_wpr_info_to_ucode`, `ga10b_acr_lsf_config`, firmware
  file-name macros `GSPPROD_RISCV_ACR_FW_*`)
* `nvgpu-common-acr-acr_sw_ga100.{c,h}` — sibling dGPU for comparison

### Falcon + BROM
* `nvgpu-common-falcon-falcon.c` — generic Falcon ops
* `nvgpu-common-falcon-falcon_sw_ga10b.{c,h}` — GA10B Falcon IDs, memory-size
  constants
* `nvgpu-hal-falcon-falcon_gk20a.{c,h}` — generic Falcon HAL
* `nvgpu-hal-falcon-falcon_gk20a_fusa.c` — Falcon mem copy + reset
* `nvgpu-hal-falcon-falcon_ga10b.h` + `falcon_ga10b_fusa.c` — **GA10B
  falcon2/RISCV BROM** (`ga10b_falcon_set_bcr`, `ga10b_falcon_bootstrap`,
  `ga10b_falcon_brom_config`, `ga10b_falcon_dump_brom_stats`, etc.)
* `nvgpu-include-nvgpu-falcon.h` — `struct nvgpu_falcon`, ops, flcn2_base
* `nvgpu-common-riscv-riscv.c` — **`nvgpu_riscv_hs_ucode_load_bootstrap`** (the
  exact sequence we need)
* `nvgpu-include-nvgpu-riscv.h` — RISCV helper headers
* `nvgpu-hw-ga10b-hw_priscv_ga10b.h` — **BCR + CPUCTL + BR_RETCODE + boot-vector
  register offsets**

### GSP
* `nvgpu-common-gsp-gsp_bootstrap.c` — `nvgpu_gsp_bootstrap_ns` (NS load+start)
* `nvgpu-common-gsp-gsp_init.c` — gsp init wiring
* `nvgpu-hal-gsp-gsp_ga10b.{c,h}` — GSP-specific HAL (ECC, IRQ, EMEM copy,
  inst-block setup, `ga10b_gsp_validate_mem_integrity`)
* `nvgpu-include-nvgpu-gsp.h` — `struct nvgpu_gsp`, `gsp_fw`
* `nvgpu-include-gops-gsp.h` — GSP op table
* `nvgpu-hw-ga10b-hw_pgsp_ga10b.h` — **GSP base + Falcon register offsets**
  (0x00110000 base, RISCV subunit at +0x1000)
* `nvgpu-hw-ga10b-hw_falcon_ga10b.h` — Falcon register layout

### PMU (optional on Jetson)
* `nvgpu-common-pmu-pmu.c` + `pmu_rtos_init.c` — PMU init + RTOS start
* `nvgpu-common-pmu-fw.c`, `fw_ns_bootstrap.c`, `fw_ver_ops.c` — PMU
  firmware load helpers
* `nvgpu-hal-pmu-pmu_ga10b.c` — GA10B PMU HAL (shows the expected PMU FW-load
  sequence; we can ignore)
* `nvgpu-include-pmu-fw.h` — PMU fw accessors

### GR / FECS / GPCCS / netlist
* `nvgpu-common-gr-gr_falcon.c` — `nvgpu_gr_falcon_init_ctxsw_ucode()` — loads
  FECS/GPCCS from filesystem into the ctxsw surface
* `nvgpu-hal-gr-falcon-gr_falcon_ga10b.c` + `gr_falcon_ga10b_fusa.c` —
  firmware-name resolver (`fecs_encrypt_prod.bin` etc.) + mailbox helpers
* `nvgpu-hal-gr-gr-gr_ga10b.c` — GR init / reset
* `nvgpu-hal-gr-ecc-ecc_ga10b.c` — GR ECC scrub
* `nvgpu-hal-netlist-netlist_ga10b.h` + `netlist_ga10b_fusa.c` — **NETC is
  the FNL / production image for GA10B**
* `nvgpu-common-netlist-netlist.c` — generic netlist parser
* `nvgpu-include-nvgpu-netlist_defs.h` — slot macros

### Misc
* `nvgpu-include-nvgpu-firmware.h` — `nvgpu_firmware` struct
* `nvgpu-include-nvgpu-flcnif_cmn.h` — common flcn interface types (`falc_u64`)

## Key open questions

1. **272-byte `fecs_encrypt_prod.bin`** — nvgpu source treats this as the
   FECS ucode image but 272 B is implausible for real ucode. Possibilities
   listed in §B. **Needs on-HW verification**: dump the actual file, decode
   the first 16 bytes (`ls_falcon_ucode_desc` magic? `bin_hdr` magic 0x10de?)
   before writing the loader. Fallback plan: extract FECS from the ACR-GSP
   `.data` blob after ACR authenticates.
2. **`FALCON_ID_GSPLITE` numeric value** — appears throughout but we didn't
   cache the `falcon.h` enum. It's in `include/nvgpu/falcon.h` but the
   constant is defined via a chip-generated header; confirm before writing
   code that compares falcon IDs.
3. **`BCR_CTRL = 0x11` vs `0x111` semantics** — inferred from code, not from
   register docs. The NVIDIA GPU RM manual (NVOC headers) has the field
   definitions; we should cache those if we hit issues.
4. **MIG_MODE** — on Jetson Orin Nano there is 1 GPC / 1 SM, MIG is N/A.
   Verify `NVGPU_SUPPORT_MIG` is NOT set so the `mode` field stays 0 in the
   descriptor.
5. **Signing keys** — the `.prod` files are signed with a retail production key.
   The GSP BROM will `RETCODE_FAIL` if we try a debug build against retail
   fuses (and vice versa). We already know from earlier investigation that
   the Orin is retail-fused; must use `.prod` only. If RETCODE_FAIL, we have
   no way to swap fuses.
