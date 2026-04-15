# NVIDIA VBIOS access via BAR0 PROM window (NV_PROM_DATA)

Reference note captured 2026-04-14 during FWSEC extraction investigation
(GitHub issue #150, E2.5). This is the authoritative answer to "how does
the proprietary driver read VBIOS when the PCI Expansion ROM BAR is too
small to expose the full image" for Turing through (at least) Blackwell.

## Key register

From open-gpu-kernel-modules,
`src/common/inc/swref/published/turing/tu102/dev_ext_devices.h`:

    #define NV_PROM_DATA(i)                            (0x00300000+(i))

`NV_PROM_DATA` is a 1 MB indexed MMIO window located at **BAR0 +
0x00300000**. It is a direct read window onto the full SPI flash
EEPROM that holds the VBIOS; the PCI Expansion ROM BAR (Config space
0x30) is only one way to see part of it and is not used by GSP-RM.

## HAL wiring — Ampere reuses TU102

`kgspExtractVbiosFromRom_TU102` (in
`src/nvidia/src/kernel/gpu/gsp/kernel_gsp_vbios_tu102.c`) is the
implementation wired into the VBIOS-extract HAL for all chips from
Turing onward. The Ampere-specific file
`src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_ga102.c` does
**not** override VBIOS extraction — it only overrides Falcon config
and ucode storage. So GA100/GA102/GA103/GA104/GA106/GA107 all use the
same NV_PROM_DATA-based extraction.

Cached copy of that file:
`docs/reference/nvidia-openrm-595-kernel-gsp-vbios-tu102.c`.

## Algorithm (summary of kgspExtractVbiosFromRom_TU102)

1. `s_getBaseBiosMaxSize_TU102` returns 1 MB (0x100000) — the full
   PROM window size, **not** the 512 KB Expansion ROM BAR cap.
2. All reads go through `s_promRead32` / `s_promRead08`, which wrap
   `GPU_REG_RD32_UNCHECKED(pGpu, NV_PROM_DATA(offset))`. This is a
   BAR0 MMIO register read, not a PCI ROM BAR read.
3. If the image does not start at offset 0 with a valid 0xAA55 PCI
   Expansion ROM signature, `s_romImgFindPciHeader_TU102` walks the
   IFR (`NV_PBUS_IFR_FMT_FIXED0/1/2`) and the ROM directory
   ("RFRD" = 0x44524652) to find where the PCI-formatted image
   starts. This is needed on GSP-capable cards whose flash begins
   with the IFR (init-from-ROM) header, not the PCI Expansion ROM.
4. `s_locateExpansionRoms` walks the PCI Expansion ROM image chain
   (PciAt → EFI → FwSec1 → FwSec2 → …), using the NPDE
   (PCI Data Extension, sig "NPDE") when present to pick up
   subImgLen/lastImage, computing the total ROM size and the offset
   of the VBIOS_EXT image (code type 0xE0).
5. The whole bios (up to biosSizeFromRom, up to 1 MB) is copied out
   of the PROM window dword-by-dword into a system-memory buffer.
   FWSEC extraction (BIT 'p' → FalconUcodeTablePtr → FWSEC_PROD
   app 0x85) is then applied to that buffer, not to the ROM BAR.

## Why the ROM BAR cap does not matter

The PCI Expansion ROM BAR (Config 0x30) on the ASUS RTX 3050 6GB
exposes 512 KB — smaller than the 568 KB the VBIOS declares. But
the proprietary / open-RM driver **never uses the Expansion ROM BAR**
for VBIOS reads. It maps BAR0 and reads from offset 0x300000.
The full SPI image, including the FwSec2 region at offset 0x8B59E
where FWSEC_PROD lives, is directly visible through that window.

## Implication for SLM-OS / nova-core port

The path forward is to read VBIOS via BAR0 + 0x300000, not via
`/sys/devices/.../rom` or the ROM BAR. In userspace the harness
already uses `/dev/mem`; switching to BAR0 + 0x300000 would give us
up to 1 MB of VBIOS directly, with no size truncation.

Caveat: before Turing, PROM access had to be enabled via
`NV_PBUS_PCI_NV_20_ROM_SHADOW = DISABLED` (write to a BIF register)
on some chips, and holding the lock across the read. On
Turing+/Ampere, `kbifPreOsGlobalErotGrantRequest_HAL` is called
first, but the actual reads are plain MMIO at 0x300000+.

## FWSEC is NOT shipped as a standalone firmware file

As of linux-firmware-nvidia 20260309-1 (and Debian/Arch/NVIDIA
.run distributions 535.x and 570.x), the files shipped under
`/lib/firmware/nvidia/<chip>/` are:

  - `booter_load-<ver>.bin.zst`
  - `booter_unload-<ver>.bin.zst`
  - `bootloader-<ver>.bin.zst`
  - `gsp-<ver>.bin.zst`
  - `scrubber.bin.zst` (or `scrubber-<ver>.bin.zst` on Ada)

There is **no** `fwsec-<ver>.bin`. The NVIDIA design is that FWSEC
is per-board-signed and lives only on the board's SPI flash. So the
capstone has to implement VBIOS-based extraction; there is no
"just copy a .bin" shortcut.

GA107 symlinks its `gsp/` directory to `ga102/gsp/` in the Arch
package, confirming GSP firmware is chip-family-wide, but FWSEC is
still not there.
