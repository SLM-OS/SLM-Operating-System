# Falcon HS Boot + FWSEC DMEMMAPPER — distilled from nouveau

Sources: `nouveau-falcon-ga102.c` (ga102_flcn_fw_boot), `nouveau-gsp-fwsec.c`,
`nvidia-ampere-ga102-dev_falcon_second_pri.h`, `nouveau-gsp-tu102.c`.

## Falcon2 BROM register base (BAR0-absolute)

From `dev_falcon_second_pri.h`:

```
NV_FALCON2_GSP_BASE     = 0x00111000   // GSP Falcon
NV_FALCON2_SEC_BASE     = 0x00841000   // SEC2 Falcon
NV_FALCON2_NVDEC0_BASE  = 0x00849c00   // NVDEC0
```

BROM registers (offsets from FALCON2_xxx_BASE):

| Offset | Name                         | Notes                               |
| ------ | ---------------------------- | ----------------------------------- |
| 0x180  | FALCON_MOD_SEL               | ALGO[7:0] — write 0x1 (RSA3K)       |
| 0x198  | BROM_CURR_UCODE_ID           | VAL[7:0] — per-image ucode id       |
| 0x19c  | BROM_ENGIDMASK               | engine-id mask (per-image)          |
| 0x210  | BROM_PARAADDR(0)             | DMEM byte offset of signature       |

Note the spec'd offset is **0x180**, not 0x1180. Absolute BAR0 addresses:
- GSP Falcon: MOD_SEL=0x111180, ENGIDMASK=0x11119c, UCODE_ID=0x111198, PARAADDR0=0x111210
- SEC2 Falcon: MOD_SEL=0x841180, ENGIDMASK=0x84119c, UCODE_ID=0x841198, PARAADDR0=0x841210

## HS boot sequence (ga102_flcn_fw_boot)

After IMEM/DMEM uploaded via DMA, before STARTCPU:

```c
wr32(addr2 + 0x210, fw->dmem_sign);  // PARAADDR[0] = DMEM offset of sig
wr32(addr2 + 0x19c, fw->engine_id);  // BROM_ENGIDMASK
wr32(addr2 + 0x198, fw->ucode_id);   // BROM_CURR_UCODE_ID
wr32(addr2 + 0x180, 0x00000001);     // MOD_SEL = RSA3K
// then gm200_flcn_fw_boot: set BOOTVEC(0x104), write CPUCTL STARTCPU(0x100 bit 1)
```

`dmem_sign` = DMEM byte offset where the signature was patched in. For V3 desc
(nvkm_gsp_fwsec_v3): `fw->dmem_sign = desc->PKCDataOffset` and patch target is
`fw->dmem_base_img + PKCDataOffset`. engine_id / ucode_id come from the V3
descriptor directly (`EngineIdMask`, `UcodeId`) — matching the R535 metadata
values.

## FWSEC V3 descriptor layout (`nvkm_falcon_ucode_desc_v3`)

```c
struct {
    u32 Hdr;              // bit0=valid, bits8..15=version, bits16..31=size
    u32 StoredSize;
    u32 PKCDataOffset;    // signature offset in DMEM (also dmem_sign)
    u32 InterfaceOffset;  // offset of appif_hdr IN DMEM (relative to dmem_base_img)
    u32 IMEMPhysBase;
    u32 IMEMLoadSize;
    u32 IMEMVirtBase;
    u32 DMEMPhysBase;
    u32 DMEMLoadSize;
    u16 EngineIdMask;     // -> BROM_ENGIDMASK
    u8  UcodeId;          // -> BROM_CURR_UCODE_ID
    u8  SignatureCount;
    u16 SignatureVersions;
    u16 Reserved;
};
```

Image layout for v3: IMEM first (size = IMEMLoadSize), then DMEM
(dmem_base_img = IMEMLoadSize). `InterfaceOffset` is **relative to
dmem_base_img** (i.e. a byte offset within DMEM image region).

## Application interface table (`appif`)

Header at `dmem_base_img + InterfaceOffset`:

```c
struct nvfw_falcon_appif_hdr_v1 {
    u8 ver;   // must be 1
    u8 hdr;   // bytes before first entry
    u8 len;   // per-entry size
    u8 cnt;   // number of entries
};
```

Then `cnt` entries of:

```c
struct nvfw_falcon_appif_v1 {
    u32 id;          // DMEMMAPPER == 0x04
    u32 dmem_base;   // offset in DMEM image of the app's data block
};
```

## DMEMMAPPER v3 data block

At `dmem_base_img + app->dmem_base`:

```c
struct {
    u32 signature;
    u16 version;
    u16 size;
    u32 cmd_in_buffer_offset;   // where frts_region lives
    u32 cmd_in_buffer_size;
    u32 cmd_out_buffer_offset;
    u32 cmd_out_buffer_size;
    u32 nvf_img_data_buffer_offset;
    u32 nvf_img_data_buffer_size;
    u32 printf_buffer_hdr;
    u32 ucode_build_time_stamp;
    u32 ucode_signature;
    u32 init_cmd;               // <-- write 0x15 for FRTS, 0x19 for SB
    u32 ucode_feature;
    u32 ucode_cmd_mask0;
    u32 ucode_cmd_mask1;
    u32 multi_tgt_tbl;
};
```

`init_cmd` values:

- `NVFW_FALCON_APPIF_DMEMMAPPER_CMD_FRTS = 0x15` (create WPR2)
- `NVFW_FALCON_APPIF_DMEMMAPPER_CMD_SB   = 0x19`

## cmd_in_buffer layout (nvfw_fwsec_frts_cmd)

Written at `dmem_base_img + dmemmap->cmd_in_buffer_offset`:

```c
struct {
    struct {  // read_vbios — ALWAYS written
        u32 ver;    // = 1
        u32 hdr;    // = sizeof(read_vbios) = 24
        u64 addr;   // = 0
        u32 size;   // = 0
        u32 flags;  // = 2
    } read_vbios;
    struct {  // frts_region — ONLY for CMD_FRTS
        u32 ver;    // = 1
        u32 hdr;    // = sizeof(frts_region) = 20
        u32 addr;   // wpr2_frts.addr >> 12  (4K pages)
        u32 size;   // wpr2_frts.size >> 12  (4K pages, so 0x100)
        u32 type;   // = 2 (FB)
    } frts_region;
};
```

Shift is `>> 12` — addr/size are in 4 KiB pages.

## WPR2 FRTS region placement (top-of-FB, ga102+)

From `tu102_gsp_oneinit` + `r535_gsp_wpr_meta_init`:

```
fb.size               = nvkm_fb_vidmem_size(device)          // from PFB

// VGA workspace: read 0x625f04; if bit3 set use its pointer,
// else default to (fb.size - 0x100000). Fallback: (fb.size - 0x20000).
fb.bios.addr          = vga_workspace_addr(fb.size)
fb.bios.size          = fb.size - fb.bios.addr

// FRTS is 1 MiB, 128 KiB aligned, just below bios region
wpr2.frts.size        = 0x100000
wpr2.frts.addr        = ALIGN_DOWN(fb.bios.addr, 0x20000) - 0x100000
```

For RTX 3050 6GB (GA107), fb.size = 6 GiB = 0x180000000. Typical case (no bit3
set): `fb.bios.addr = 0x180000000 - 0x100000 = 0x17FF00000`, already 128 KiB
aligned, so `wpr2.frts.addr = 0x17FE00000`, size = 0x100000. That yields:

- `frts_region.addr = 0x17FE00000 >> 12 = 0x17FE00`
- `frts_region.size = 0x100000 >> 12  = 0x100`
- `frts_region.type = 2`

After FWSEC completes, verify by reading WPR2 hardware regs (BAR0):
- `0x1fa824` = WPR2 lo
- `0x1fa828` = WPR2 hi

Error status: `0x001400 + 0xe*4` top 16 bits (FRTS), `0x001400 + 0x15*4` low
16 bits (SB).
