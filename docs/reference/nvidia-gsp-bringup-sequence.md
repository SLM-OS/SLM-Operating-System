# NVIDIA GSP-RM Bringup Sequence on Ampere (GA10x)

Reference document for E3 (Falcon / SEC2 / RISC-V bringup) targeting ASUS RTX 3050 6GB
(GA107, Ampere, chip id 0x177A1000 in `NV_PMC_BOOT_42`). All offsets here are verified
against the Linux nouveau driver (`drivers/gpu/drm/nouveau/nvkm/subdev/gsp`) and the
NVIDIA openrm `src/common/inc/swref/published/ampere/ga102/` register headers, both
for firmware version **535.113.01**.

Source of truth note: wherever this doc and the code in `docs/reference/nouveau-*.c`
disagree, trust the code. Flag XXXs are real uncertainties worth probing on hardware.

---

## 1. Register / BAR0 offsets on GA10x

All offsets are BAR0-relative. There are three relevant engines: **GSP Falcon** (Falcon
v4 + RISC-V co-core), **SEC2 Falcon** (Falcon v4, HS-signed, runs Booter and FWSEC),
and the **RISC-V boot-control register block** (a second PRI aperture that overlays
GSP Falcon).

### 1.1 Engine bases (absolute, BAR0-relative)

| Engine                     | Base        | Source                                             |
|----------------------------|-------------|----------------------------------------------------|
| `NV_PGSP` (GSP Falcon v4)  | `0x00110000` | `ampere/ga102/dev_gsp.h` — `NV_PGSP 0x113fff:0x110000` |
| `NV_FALCON2_GSP_BASE` (RISC-V PRI for GSP) | `0x00111000` | `ampere/ga102/dev_riscv_pri.h`          |
| `NV_FALCON2_SEC_BASE` (RISC-V/BROM PRI for SEC2) | `0x00841000` | `ampere/ga102/dev_falcon_second_pri.h` |
| SEC2 Falcon (primary)      | `0x00840000` | `nvkm/engine/sec2/ga102.c` — `const u32 addr = 0x840000` |

### 1.2 Falcon v4 register offsets (applied to both GSP base 0x110000 and SEC2 base 0x840000)

From `nvidia-ampere-ga102-dev_falcon_v4.h`:

| Reg                          | Falcon offset | Bits of interest                                          |
|------------------------------|---------------|-----------------------------------------------------------|
| `IRQSCLR`                    | `0x004`       | bit 4 = HALT_SET, bit 6 = SWGEN0                          |
| `IRQSTAT`                    | `0x008`       | bit 4 = HALT, bit 6 = SWGEN0                              |
| `IRQMASK` / `IRQMSET`/`IRQMCLR`/`IRQDEST` | `0x018 / 0x010 / 0x014 / 0x01c`                      |
| `MAILBOX0`                   | `0x040`       | RW, 32 bits. Input/output word to bootloader.             |
| `MAILBOX1`                   | `0x044`       | RW, 32 bits.                                              |
| `OS`                         | `0x080`       | RW — nouveau writes `app_version` to GSP OS reg here (`r535_gsp_init`). |
| `DEBUGINFO`                  | `0x094`       | RW                                                        |
| `CPUCTL`                     | `0x100`       | bit 1 STARTCPU (WO), bit 4 HALTED (RO), bit 6 ALIAS_EN    |
| `BOOTVEC`                    | `0x104`       | Falcon PC at STARTCPU                                     |
| `HWCFG`                      | `0x108`       | IMEM_SIZE in bits 8:0                                     |
| `DMACTL`                     | `0x10c`       | bit 0 REQUIRE_CTX, bit 1 DMEM_SCRUBBING, bit 2 IMEM_SCRUBBING |
| `DMATRFBASE`                 | `0x110`       | BAR0-shifted sys-mem DMA base (>>8 on Ampere — see ga102_flcn_dma_init) |
| `DMATRFMOFFS`                | `0x114`       | Falcon mem base (IMEM/DMEM byte offset)                   |
| `DMATRFCMD`                  | `0x118`       | bit 0 FULL, bit 1 IDLE, bits 3:2 SEC, bit 4 IMEM, bit 5 WRITE, bits 10:8 SIZE, bits 14:12 CTXDMA, bit 16 SET_DMTAG |
| `DMATRFFBOFFS`               | `0x11c`       | Source-side byte offset within the block pointed to by DMATRFBASE |
| `DMATRFBASE1`                | `0x128`       | Upper 9 bits of 40-bit sys-mem DMA base (nouveau clears to 0)|
| `CPUCTL_ALIAS`               | `0x130`       | bit 1 STARTCPU. Used when CPUCTL_ALIAS_EN is set (alternate start path, see r535 CORE_START sequence) |
| `IMEMC(i)`                   | `0x180+16*i`  | PIO IMEM control: OFFS [7:2], BLK [23:8], AINCW [24], SECURE [28] |
| `IMEMD(i)`                   | `0x184+16*i`  | PIO IMEM data                                             |
| `IMEMT(i)`                   | `0x188+16*i`  | PIO IMEM tag [15:0]                                       |
| `DMEMC(i)`                   | `0x1c0+8*i`   | PIO DMEM control: OFFS [7:2], BLK [23:8], AINCW [24]      |
| `DMEMD(i)`                   | `0x1c4+8*i`   | PIO DMEM data                                             |
| `ENGINE`                     | `0x3c0`       | bit 0 = RESET (self-clear)                                |
| `HWCFG2`                     | `0x0f4`       | bit 10 RISCV_ENABLE, bit 12 MEM_SCRUBBING (0 = done)      |

**Absolute GSP-Falcon mailbox addresses** (from `dev_gsp.h`): `NV_PGSP_FALCON_MAILBOX0 = 0x110040`, `NV_PGSP_FALCON_MAILBOX1 = 0x110044`, `NV_PGSP_FALCON_ENGINE = 0x1103c0` (engine reset). SEC2 counterparts: `0x840040`, `0x840044`, `0x8403c0`.

### 1.3 RISC-V PRI registers (on GSP only; SEC2 Ampere doesn't go RISC-V)

From `nvidia-ampere-ga102-dev_riscv_pri.h`. Base is `NV_FALCON2_GSP_BASE = 0x111000`:

| Reg               | RISC-V offset | Absolute  | Purpose                                       |
|-------------------|---------------|-----------|-----------------------------------------------|
| `RISCV_CPUCTL`    | `0x388`       | `0x111388`| bit 4 HALTED, bit 7 ACTIVE_STAT (1 = RISC-V running) |
| `RISCV_IRQMASK`   | `0x528`       | `0x111528`| IRQ mask when in RISC-V mode                  |
| `RISCV_IRQDEST`   | `0x52c`       | `0x11152c`|                                               |
| `RISCV_BCR_CTRL`  | `0x668`       | `0x111668`| **Boot-control select**: bit 0 VALID (RO), bit 4 CORE_SELECT (0=Falcon / 1=RISC-V), bit 8 BRFETCH |

Nouveau reads `RISCV_CPUCTL + 0x388` = `0x111388` to confirm "RISC-V active" (`ga102_flcn_riscv_active`). It also uses `addr2 + 0x668 = 0x111668` for `BCR_CTRL`, which `ga102_flcn_select()` toggles to force Falcon-mode access to Falcon registers while still in a dual-mode core.

**GSP alive mailbox**: GSP Falcon's own `MAILBOX0` (0x110040) receives the Booter result code (0 = success). Actual "GSP-RM init done" is NOT signaled by a mailbox value — it is signaled by an **RPC message** on the message queue in sysmem (see §4 and §6).

### 1.4 Other GA10x registers E3 will touch

| Register       | Offset     | Source                                                  |
|----------------|------------|---------------------------------------------------------|
| `NV_PMC_BOOT_42` | `0x00000a00` | Confirm chip id (`0x177A1000` for GA107)             |
| WPR2_LO        | `0x001fa824` | FB write-protected region 2 low  (set by FWSEC-FRTS)  |
| WPR2_HI        | `0x001fa828` | FB write-protected region 2 high (set by FWSEC-FRTS)  |
| FWSEC-FRTS err status | `0x001400 + 0xe*4 = 0x001438` | upper half word = error |
| FWSEC-SB err status   | `0x001400 + 0x15*4 = 0x001454` | low half word = error |
| VGA workspace reg | `0x00625f04` | base of BIOS scratch (top of FB minus offset)      |
| Disp/SEC2 "alive" bit | `0x001180f8` | bit 0x04000000 — SEC2 OK after core-resume     |
| GA10x fuse versions   | `0x008241c0 + (ucode_id-1)*4` | per-ucode fuse-lock (for signature select) |

---

## 2. Falcon DMA protocol

Ampere GSP and SEC2 Falcons both support DMA-based IMEM/DMEM upload. On Ampere this is
the mandatory path (nouveau's `imem_pio` is not installed for ga102_gsp_flcn; `imem_dma`
and `dmem_dma` are). Pre-Ampere (TU10x) Falcons also expose a PIO path via IMEMC/IMEMD
which the FWSEC bootloader descriptor still uses for a tiny write. Prefer DMA.

### 2.1 DMA upload — the Ampere path (`ga102_flcn_dma`, `ga102_flcn_fw_load`)

Prereqs:
- The ucode image (IMEM blob concatenated with DMEM blob, possibly signed) must already
  be in **sys-mem** with a stable physical address `dma_addr` — the Falcon will pull
  256 bytes at a time from there.
- `dma_addr` must be 256-byte aligned. Per-chunk length `dmalen = 256` (forced in
  `nvkm_falcon_dma_wr`).

Step-by-step (see `ga102_flcn_fw_load` + `nvkm_falcon_dma_wr` + `ga102_flcn_dma_init`/`xfer`):

```
// 1. Put the Falcon in "no-VA, context 0" mode so DMA works in physical addresses.
falcon[0x624] |= 0x80           // mask-set bit 7: disable VA context
falcon[0x10c]  = 0              // DMACTL: clear REQUIRE_CTX
falcon[0x600] |= (1<<2) | 1     // FBIF_CTL ctxdma slot 0 => PHYS_SYS_NCOH
                                // (equivalent of DMAIDX_PHYS_SYS_NCOH = 5)
// 2. Program DMATRFBASE: bus address shifted right by 8.
falcon[0x110] = dma_addr >> 8   // low 32 of sys phys
falcon[0x128] = 0               // DMATRFBASE1 (upper 9 bits; 0 for low-4GB DMA)

// 3. For each 256-byte chunk (of both IMEM and DMEM):
cmd = (ilog2(256) - 2) << 8     // = 6 << 8 = 0x600 (SIZE_256B)
if (uploading to IMEM) cmd |= 0x10  // DMATRFCMD_IMEM_TRUE
if (sec)               cmd |=  0x04 // DMATRFCMD_SEC

for chunk in ucode chunks (256B each):
    falcon[0x114] = mem_base         // DMATRFMOFFS — Falcon-side IMEM/DMEM byte offset
    falcon[0x11c] = src_offset_in_fw // DMATRFFBOFFS — offset within the blob at DMATRFBASE
    falcon[0x118] = cmd              // DMATRFCMD — kicks the transfer
    wait until (falcon[0x118] & 0x02) // IDLE == 1 (ga102_flcn_dma_done)
    mem_base      += 256
    src_offset_in_fw += 256
```

For the **booter** (which is HS-signed), nouveau uses the same DMA path but also patches
the signature into the DMEM image before the DMA and writes `BROM_*` registers after
boot (see §3.2). The PIO path (IMEMC/IMEMD) is still used exactly once on TU102 for
the FWSEC bootloader descriptor:

```
// tu102_gsp_fwsec_load_bld: write a 44-byte flcn_bl_dmem_desc_v2 to Falcon DMEM slot 0.
falcon[0x600 + 5*4] = (reg & ~7) | 5   // select ctxdma slot 5 = PHYS_SYS_NCOH
nvkm_falcon_pio_wr(DMEM, offset=0, len=sizeof(desc))   // via DMEMC/DMEMD
```

The DMEMC PIO init uses `falcon[0x1c0+port*8] = BIT(24) | dmem_base`, then 4-byte
writes to `falcon[0x1c4+port*8]` walk through the buffer.

### 2.2 IMEM PIO (legacy; still useful for small descriptors)

```
falcon[0x180 + port*16] = (sec ? BIT(28) : 0) | BIT(24) | imem_base   // IMEMC
falcon[0x188 + port*16] = tag                                         // IMEMT
for each 4-byte word:
    falcon[0x184 + port*16] = word                                    // IMEMD
```

See `gm200_flcn_pio_imem_wr`. Tag is the instruction address >> 8.

---

## 3. SEC2 bringup: FWSEC-FRTS, then Booter Load

This is the step that creates WPR2 (so Booter can place GSP-RM into it) and then
actually boots GSP-RM. Nouveau's order — read from `tu102_gsp_oneinit` (shared by
Ampere via `.oneinit = tu102_gsp_oneinit` in `ga102_gsp_r535`) — is:

1. Construct booter_load + booter_unload as `nvkm_falcon_fw` objects against the SEC2
   falcon (`booter.ctor` = `ga102_gsp_booter_ctor`). The sigs+IMEM+DMEM come from
   `/lib/firmware/nvidia/ga107/gsp/booter_load-535.113.01.bin`, which is an NVIDIA
   "HS v2" format (has a V2 header with `meta_data_offset`, `patch_loc`, `patch_sig`,
   `num_sig`, then sig table, then the raw IMEM|DMEM blob). `ga102_gsp_booter_ctor`
   parses header, copies sigs to a side buffer, and fills `imem_base_img`,
   `dmem_base_img`, `dmem_sign`, `boot_addr`, plus BROM meta (`fuse_ver`, `engine_id`,
   `ucode_id`).
2. Run `r535_gsp_oneinit`: parse the GSP-RM ELF (`gsp.bin`), extract `.fwimage` and
   `.fwsignature_ga10x`, build a radix3 page-table mapping `.fwimage` → sys-mem pages.
3. Compute WPR2 layout inside FB (see §5 for exact formula).
4. Build `GspFwWprMeta` struct in sysmem (nouveau `tu102_gsp_wpr_meta_init`).
5. Run **FWSEC-FRTS** on the **GSP Falcon** (yes, GSP, not SEC2 — see next paragraph).
6. Reset GSP into RISC-V mode (`ga102_gsp_reset`).
7. Write `gsp->libos.addr` to GSP falcon regs `0x040` / `0x044` (these are the
   normal MAILBOX0/MAILBOX1 offsets; nouveau repurposes them as the libos pointer for
   the RISC-V bootloader to pick up).
8. In `tu102_gsp_init`: write WPR-meta low32 → MAILBOX0, high32 → MAILBOX1 on SEC2
   falcon via `tu102_gsp_booter_load` → `nvkm_falcon_fw_boot` (which runs the FWSEC
   `reset → load → boot` funcs, which for the booter == `ga102_flcn_fw_reset` (==
   `nvkm_falcon_reset` of SEC2) + `ga102_flcn_fw_load` (DMA of IMEM+DMEM+sig patch)
   + `ga102_flcn_fw_boot` (BROM programming + STARTCPU)).
9. `r535_gsp_init` → write app_version to GSP `0x080`, confirm RISC-V is active
   (read `NV_PRISCV_RISCV_CPUCTL 0x111388 & 0x80`), then block waiting for the
   `NV_VGPU_MSG_EVENT_GSP_INIT_DONE` RPC on the sysmem message queue.

### 3.1 Clarifying who runs what ucode

- **FWSEC-FRTS** is loaded by nouveau onto the **GSP Falcon** (not SEC2!). See
  `nvkm_gsp_fwsec_init` — it passes `&gsp->falcon` into `nvkm_falcon_fw_ctor`. The
  FWSEC ucode is extracted from the VBIOS PMU table (type 0x85), patched with the
  DMEMMAPPER init_cmd and the FRTS region (FB type, addr>>12, size>>12), then booted.
  It returns via halt, and nouveau verifies by reading `0x001438 >> 16` (error code)
  and `0x1fa824` / `0x1fa828` (WPR2 low/high).
- **Booter Load** runs on **SEC2**. It receives `MAILBOX0 = low32(wpr_meta_phys)` and
  `MAILBOX1 = high32(wpr_meta_phys)`. The booter itself then DMAs the GSP-RM image
  (via the radix3 table, into WPR2) and the GSP-RM bootloader, and kicks GSP RISC-V.
- **Booter Unload** is only needed on fini/suspend and takes meaningless mbox values
  (0xff/0xff on power off).

### 3.2 Booter file format (`booter_load-535.113.01.bin`)

It is NOT raw IMEM|DMEM. The prefix is an NVIDIA `nvfw_bin_hdr`:
```
struct nvfw_bin_hdr {
    u32 bin_magic;       // 0x10de or 0x3b1d14f0
    u32 bin_ver;
    u32 bin_size;
    u32 header_offset;   // -> nvfw_hs_header_v2
    u32 data_offset;     // -> IMEM|DMEM blob
    u32 data_size;
};
struct nvfw_hs_header_v2 {
    u32 sig_prod_offset, sig_prod_size, patch_loc, patch_sig, num_sig;
    u32 header_offset;   // -> nvfw_hs_load_header_v2
    u32 meta_data_offset; size_of_meta_data, num_sig (again), header_offset2 ...
};
struct nvfw_hs_load_header_v2 {
    u32 os_code_offset, os_code_size, os_data_offset, os_data_size;
    u32 num_apps;
    struct { u32 offset, size; } app[num_apps];
};
```
Meta data at `meta_data_offset` is three u32: `fuse_ver`, `engine_id`, `ucode_id` —
the BROM (Boot ROM) on SEC2 needs these in `NV_PFALCON2_FALCON_BROM_ENGIDMASK = 0x19c`,
`BROM_CURR_UCODE_ID = 0x198`, and sig/fuse selection. See `ga102_flcn_fw_boot`:
```
sec2[addr2 + 0x210] = dmem_sign    // BROM_PARAADDR(0) — where sig was patched in DMEM
sec2[addr2 + 0x19c] = engine_id
sec2[addr2 + 0x198] = ucode_id
sec2[addr2 + 0x180] = 1             // MOD_SEL = RSA3K
// then the standard gm200_flcn_fw_boot: write mbox0/1, BOOTVEC, CPUCTL=STARTCPU, poll HALTED
```
`addr2` for SEC2 is `0x1000` in the falcon (the "falcon2" PRI shadow), giving absolute
SEC2-BROM registers at `0x840000 + 0x1000 + 0x180/0x198/0x19c/0x210` = `0x841180/198/19c/210`.

### 3.3 FWSEC DMEMMAPPER patching (you already have this for FRTS)

FWSEC IMEM is the signed payload; before booting it, the caller must patch the
DMEMMAPPER application interface entry in DMEM:
- locate `InterfaceOffset` in the v3 descriptor (the 44-byte desc you already parse),
- walk app entries looking for `id == NVFW_FALCON_APPIF_ID_DMEMMAPPER (0x04)`,
- write `dmemmap->init_cmd = 0x15 (FRTS)` or `0x19 (SB)` at the dmem base pointed to
  by that app entry,
- write the `frts_region` sub-struct with `type = 2 (FB)`, `addr = wpr2_frts.addr>>12`,
  `size = wpr2_frts.size>>12`.

The FWSEC-SB (`0x19`) variant is used by nouveau as a "lifecycle touch" to let FWSEC
sync state on resume / after booter unload — not required for the initial E3 bring-up
if we never suspend. FRTS is the one that actually creates WPR2.

**Observable signal that FWSEC-FRTS succeeded:** `BAR0[0x1fa828]` (WPR2_HI) is
non-zero. `BAR0[0x001438] >> 16` is zero. Nouveau logs this as
`"fwsec-frts: WPR2 @ %08x - %08x"`. See `nvkm_gsp_fwsec_frts`.

**Observable signal Booter Load succeeded:** `nvkm_falcon_fw_boot` returns 0, meaning
SEC2 CPUCTL bit 4 (HALTED) was observed set within 2000 ms and MAILBOX0 == 0. If
MAILBOX0 is non-zero after halt, booter returned an error code.

---

## 4. GSP-RM RISC-V bringup after WPR is up

After §3.5 (FWSEC-FRTS), WPR2 exists in FB. Before booting GSP, nouveau does:

```
// a. Reset GSP falcon into RISC-V mode (ga102_gsp_reset, tu102_gsp_oneinit step 5):
ga102_flcn_reset_eng(gsp_falcon)         // toggle 0x3c0 bit 0
wait_mem_scrubbing                       // HWCFG2 bit 12 clears
// then nouveau does `nvkm_falcon_mask(&gsp->falcon, 0x1668, 0x00000111, 0x00000111);`
//   — absolute 0x111668 = BCR_CTRL, setting CORE_SELECT=RISCV (bit 4),
//   plus VALID (bit 0) and BRFETCH (bit 8). This wires the GSP core into RISC-V
//   mode so the next STARTCPU boots the RISC-V bootloader.

// b. Hand the RISC-V bootloader the address of the libos arg struct (in SYSMEM).
gsp_falcon[0x110040] = low32(gsp->libos.addr)   // MAILBOX0 repurposed
gsp_falcon[0x110044] = high32(gsp->libos.addr)  // MAILBOX1 repurposed
```

Then when we actually want to run GSP-RM (`tu102_gsp_init`):

```
// c. Booter Load does the hard work: GSP RISC-V image is mapped via radix3 in sysmem,
//    booter copies it to WPR2, then releases GSP RISC-V from reset.
mbox0 = low32(gsp->wpr_meta.addr)
mbox1 = high32(gsp->wpr_meta.addr)
run booter_load on SEC2 with MBOX0/1 = those values
  -> SEC2 boots FWSEC-internal path, reads gsp->wpr_meta, DMAs GSP-RM into WPR2,
     programs GSP RISC-V BOOTVEC, releases RISC-V from reset.

// d. Confirm RISC-V is active:
loop: if (gsp_falcon[0x111388] & 0x80) break;   // CPUCTL.ACTIVE_STAT

// e. Hand GSP-RM its app_version:
gsp_falcon[0x110080] = gsp->boot.app_version    // desc->appVersion from bootloader.bin

// f. Block until the GSP-RM ELF entrypoint has posted
//    NV_VGPU_MSG_EVENT_GSP_INIT_DONE on the sysmem message queue.
poll sysmem cmdq/msgq until we see a message with function == GSP_INIT_DONE.
```

### The "GSP-RM alive" token

There is NO magic constant in a Falcon mailbox that says "GSP-RM alive." What there
IS, per `r535_gsp_init`:

1. The SEC2 booter_load returns 0 in its MAILBOX0 (SEC2 halted, no error).
2. The GSP RISC-V core asserts `NV_PRISCV_RISCV_CPUCTL.ACTIVE_STAT` (`0x111388 & 0x80`).
3. GSP-RM processes its libos init, fills out `msgqTxHeader` in the shared-mem ring,
   increments `msgq.tx.writePtr`, and posts an `NV_VGPU_MSG_EVENT_GSP_INIT_DONE`
   message. This IS the alive signal.

For E3, plan to consume this by setting up a minimal shared-memory ring
(§5.3) and polling `*msgq.wptr != 0` with a timeout. Don't expect anything in
MAILBOX0 — that's the libos addr pointer, it's input only.

XXX: the exact numeric value of `NV_VGPU_MSG_EVENT_GSP_INIT_DONE` changes across
R535 vs R570. For R535 it's defined in the openrm headers
(`src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gsp.h` or nearby) — probe this
from the first message you receive rather than hard-coding.

---

## 5. Minimum VRAM layout / radix3 page tables

### 5.1 WPR2 layout (in FB, from `tu102_gsp_oneinit`)

Computed top-down from the VGA workspace at the end of FB (`vga_workspace.addr`
resolved from either reg `0x625f04` bits 26:8 or `fb_size - 0x100000` fallback).
Sizes are approximate minimums — the heap grows with FB size:

```
+-- end of FB ---------------------------------------------+
| VGA workspace (bios.vga_workspace, ~1 MB)                |
+-- 20000-aligned boundary -------------------------------+
| FRTS region  (frts, size=0x100000)        <- WPR2 top   |  <- FWSEC-FRTS fills this
+-- 1000-aligned ----------------------------------------+
| GSP bootloader code + data (boot, ~64 KB)               |
+-- 10000-aligned ---------------------------------------+
| GSP-RM ELF image (elf, gsp->fw.len, typ ~38 MB)         |
+-- 100000-aligned --------------------------------------+
| GSP-RM heap (heap, >= wpr->heap_size_min)               |
+-- 100000-aligned --------------------------------------+
| GspFwWprMeta (wpr_meta) — sizeof(GspFwWprMeta) rounded  |  <- WPR2 base
+---------------------------------------------------------+
|   (non-WPR heap, 1 MB, just below WPR2)                 |
+---------------------------------------------------------+
```

`GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB`, `os_carveout_size`, `base_size`,
`heap_size_min` come from the version-specific `nvkm_rm_wpr` struct
(`rm/r535/rm.c`). For R535 GA10x these are roughly 8 MB base + 8 MB per GB FB.
Plan for **~64 MB** of WPR2 on a 6 GB card to be safe; the exact size fix is to
copy the arithmetic verbatim from `tu102_gsp_oneinit` and `tu102_gsp_wpr_heap_size`.

### 5.2 Radix3 page table (in sysmem)

The GSP-RM ELF **image itself** lives in sysmem (not FB) while booter is running —
Booter consumes it via DMA through a 3-level page table:

- Level 0: 1 × 4 KB page; contains exactly 1 u64 = phys_addr(level1).
- Level 1: 1 × 4 KB page; up to 512 u64 entries, each = phys_addr of a level 2 page.
- Level 2: up to 512 × 4 KB pages; each 4 KB page holds 512 u64 entries, each =
  phys_addr of a 4 KB page of the GSP-RM ELF image.

Max size of image addressable: 512 × 512 × 4 KB = 1 GB. GSP_PAGE_SIZE is always 4 KB
inside the GSP even if the host is using 64 KB pages. See `nvkm_gsp_radix3_sg` in
`nouveau-gsp-r535.c`. The `wpr_meta.sysmemAddrOfRadix3Elf` field must point to the
physical address of Level 0.

### 5.3 Sys-mem layout for libos + message queues

From `r535_gsp_libos_init` and `r535_gsp_shared_init`:

- `libos` buffer: 4 KB. Array of 4 `LibosMemoryRegionInitArgument` entries
  (id8, physaddr, size, kind=CONTIGUOUS, loc=SYSMEM) — LOGINIT (64 KB), LOGINTR
  (64 KB), LOGRM (64 KB), RMARGS (4 KB). Nouveau also writes PTE arrays into the
  log buffers themselves starting at offset 8.
- `rmargs` buffer: 4 KB. Contains `GSP_ARGUMENTS_CACHED`:
  - `messageQueueInitArguments.sharedMemPhysAddr` = base of shm.mem
  - `messageQueueInitArguments.pageTableEntryCount` = shm.ptes.nr
  - `cmdQueueOffset` / `statQueueOffset` = offsets inside shm of cmdq and msgq
  - `srInitArguments`: zeroed for cold boot
- `shm.mem`: a single contiguous sysmem allocation holding: PTE array, then cmdq
  (0x40000 = 256 KB), then msgq (0x40000). cmdq/msgq headers contain
  `msgqTxHeader` (version=0, size, entryOff=4KB, msgSize=4KB, writePtr=0,
  flags=1, rxHdrOff=offsetof(readPtr)).

For E3 we only need the minimum to receive `GSP_INIT_DONE`. Plan for:
- 1 × 4 KB libos args page (sysmem)
- 3 × 64 KB log buffers (sysmem)
- 1 × 4 KB RMARGS (sysmem)
- ~512 KB cmdq + msgq shared region (sysmem)
- 3-level radix3 covering the ~38 MB GSP-RM ELF

Total extra sysmem for bringup: ~40 MB. The bulk is the ELF.

---

## 6. Key code references

All URLs are raw Linux-mainline or NVIDIA openrm on GitHub. Local copies in
`docs/reference/` are listed after each.

### GSP subdev (nouveau)

- Top-level GA10x entry point: `nouveau-gsp-ga102.c` → `ga102_gsp_r535` struct,
  `ga102_gsp_reset`, `ga102_gsp_booter_ctor`, `ga102_gsp_fwsec_signature`.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c>
- Shared Turing/Ampere orchestration: `nouveau-gsp-tu102.c` → `tu102_gsp_oneinit`
  (WPR layout + booter ctor), `tu102_gsp_init` (booter_load with WPR-meta mbox),
  `tu102_gsp_wpr_meta_init`, `tu102_gsp_fwsec_load_bld` (DMEM desc v2 for TU's
  FWSEC path), `tu102_gsp_fini` (booter_unload lifecycle).
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c>
- R535 heavy lifting: `nouveau-gsp-r535.c` → `r535_gsp_oneinit` (ELF parse, radix3
  build, libos+rmargs+shm setup), `r535_gsp_init` (RISC-V active poll + INIT_DONE
  wait), `nvkm_gsp_radix3_sg`, `r535_gsp_libos_init`, `r535_gsp_shared_init`,
  `r535_gsp_set_rmargs`, `r535_gsp_msg_run_cpu_sequencer` (handles GSP's
  register-replay sequencer that toggles `0x624`, `0x10c`, `0x100`, etc., around
  core reset/start).
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c>
  *(mainline path is now `.../gsp/rm/r535/gsp.c` since kernel 6.10ish — same content)*
- R535 RPC + msgq: `nouveau-gsp-rpc-r535.c` → `r535_gsp_msg_recv`,
  `r535_gsp_rpc_poll`, `r535_gsp_rpc_send` (how to actually drive the
  sysmem msgq ring after init is done).
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c>
- FWSEC loader + DMEMMAPPER patcher: `nouveau-gsp-fwsec.c` → `nvkm_gsp_fwsec_frts`,
  `nvkm_gsp_fwsec_sb`, `nvkm_gsp_fwsec_init`, `nvkm_gsp_fwsec_v3` (Ampere),
  `nvkm_gsp_fwsec_patch`. Use this file as the reference for the init_cmd
  mechanics; it mirrors what we already have in `host-tools/gsp-harness/` for E2.5.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/subdev/gsp/fwsec.c>

### Falcon engine (nouveau)

- Ampere Falcon funcs: `nouveau-falcon-ga102.c` → `ga102_flcn_dma`
  (init/xfer/done), `ga102_flcn_fw_load`, `ga102_flcn_fw_boot` (BROM programming),
  `ga102_flcn_riscv_active`, `ga102_flcn_reset_prep`, `ga102_flcn_select`
  (BCR_CTRL dance), `ga102_flcn_reset_wait_mem_scrubbing`.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c>
- Maxwell/common Falcon helpers: `nouveau-falcon-gm200.c` →
  `gm200_flcn_fw_boot` (STARTCPU + HALTED poll), `gm200_flcn_fw_load` (fallback
  PIO path), `gm200_flcn_enable` / `disable`, `gm200_flcn_imem_pio`,
  `gm200_flcn_dmem_pio`.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/falcon/gm200.c>
- Generic Falcon dispatcher: `nouveau-falcon-base.c` → `nvkm_falcon_dma_wr` (256 B
  chunking loop), `nvkm_falcon_pio_wr`, `nvkm_falcon_reset`, `nvkm_falcon_riscv_active`.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/falcon/base.c>
- HS-ucode boot + signature patching: `nouveau-falcon-fw.c` → `nvkm_falcon_fw_boot`
  (calls `reset→setup→load→boot` for any FW), `nvkm_falcon_fw_patch`,
  `nvkm_falcon_fw_sign`, `nvkm_falcon_fw_ctor_hs_v2`.
  <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/nouveau/nvkm/falcon/fw.c>
- Engine-reset helper: `nouveau-falcon-gp102.c` → `gp102_flcn_reset_eng` (toggle
  `0x3c0` bit 0 with 10 us delay).
- RISC-V active bit (shared by TU and Ampere for Falcon-mode check):
  `nouveau-falcon-tu102.c` → `tu102_flcn_riscv_active` reads `addr2 + 0x240`.

### Openrm cross-check (version 595.x, closest mainline analog)

- Kernel GSP orchestration: `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` — contains
  `kgspBootstrap_HAL`, `kgspCreateRadix3_IMPL`, `kgspCalculateFbLayout_GA102`.
  <https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c>
- Ampere HAL: `src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_ga102.c` —
  `kgspConfigureFalcon_GA102`, `kgspGetGspRmBootUcodeStorage_GA102`. Already
  cached: `nvidia-openrm-595-kernel-gsp-ga102.c`.
- Ampere register definitions: the `dev_falcon_v4.h`, `dev_riscv_pri.h`,
  `dev_gsp.h`, `dev_falcon_second_pri.h`, `dev_gsp_addendum.h` — all under
  `src/common/inc/swref/published/ampere/ga102/`. Cached as
  `nvidia-ampere-ga102-dev_*.h`.

---

## Open items / hardware probes

- **Absolute value of `NV_VGPU_MSG_EVENT_GSP_INIT_DONE`** for R535 — probe by
  capturing the first RPC message the GSP emits. (Fallback: the openrm header
  `src/nvidia/inc/kernel/gpu/gsp/kernel_gsp.h` or the `ctrl2080gsp.h` around it.)
- **GspFwWprMeta struct layout for R535.113.01**: the nouveau header
  `rm/r535/nvrm/gsp.h` has the canonical copy. If it drifts between R535 minor
  releases, field order breaks silently. Fetch
  `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h` and copy the struct
  verbatim — do not hand-roll.
- **Booter HS header patch_loc/patch_sig semantics on Ampere**: these are u32
  indirections — the value at `patch_loc` is the destination offset in DMEM, and
  the value at `patch_sig` is the offset into the sig table. Confirm by dumping
  the first 128 bytes of `booter_load-535.113.01.bin` and sanity-check against
  `ga102_gsp_booter_ctor`.
- **SEC2 IMEM/DMEM sizes on GA107**: GA107's SEC2 has smaller SRAM than GA102.
  Read `HWCFG` (Falcon `0x108`) and `HWCFG1` to confirm before DMA'ing a blob
  that's too big.
