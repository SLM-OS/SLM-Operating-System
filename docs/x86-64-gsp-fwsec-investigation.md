# FWSEC Discovery on RTX 3050 (GA107) — Investigation Notes

**Status:** Open — tracked by #143. Blocks E3 (Falcon/RISC-V bringup).

**Target card:** ASUS RTX 3050 6GB (GA107), PCI vendor/device `10de:2584`,
chip id 0x177 (Ampere), BAR0 16 MB at 0x53000000, ROM BAR 512 KB at
0x54000000.

## What works

- `gsp-harness --probe` reads `BOOT_42 = 0x177a1000` (Ampere, GA107) over
  VFIO-mapped BAR0. Engine register at `0x400000` returns `0xBADF5040`
  (documented "GSP not loaded" poison pattern).
- `gsp-harness --vbios` reads the PCI Expansion ROM via
  `/sys/bus/pci/devices/0000:01:00.0/rom` (149,504 bytes — the Linux
  kernel stops at PCIR's LAST marker) and parses the BIT table: 19
  entries starting at offset 0x1B0, including entry 14 (id=0x70 'p',
  ver=2, len=4, data_offset=0x041F) which carries the
  `BIT_TOKEN_FALCON_DATA_V2` structure.
- Reading `u32 FalconUcodeTablePtr` from that BIT 'p' entry yields
  **0x00076B9E** (486,302 bytes).

## Where we got stuck

The openrm/nova-core documentation cached in `docs/reference/` assumes
`FalconUcodeTablePtr` addresses into a concatenated virtual buffer
`PciAt | FwSec#1 | FwSec#2` (possibly with an EFI image stripped).
None of the arithmetic schemes documented there lands on a valid
`FALCON_UCODE_TABLE_HDR_V1` on this specific card:

| Interpretation | Target offset | Result |
|---|---|---|
| Absolute offset in 512 KB ROM dump | 0x076B9E | Random bytes `0d 67 32 1b ...` |
| `PciAt_len + ptr` (= skip PciAt) | 0x1669E | Still in EFI body |
| `ext_rom_off + ptr` where ext_rom_off=FwSec image start | Overflows ROM | — |
| nova-core: `ptr - PciAt_len` into FwSec#1, else `- first_fwsec_len` into FwSec#2 | Overflows ROM | — |

## The catch — NPDS not PCIR

To dump past the Linux sysfs cap we enabled the ROM BAR via `setpci`
and read 524,288 bytes directly from the physical address
(`/tmp/dump_rom_mem.c`). That dump reveals the full ROM has more
sub-images than PCIR describes:

| Offset | Header | `code_type` | `indicator` | Note |
|---|---|---|---|---|
| 0x00000 | PCIR | 0x00 (x86) | 0x00 | PciAt, 65 KB |
| 0x0FE00 | PCIR | 0x03 (EFI) | **0x80 (LAST)** | Linux stops here |
| 0x29D00 | **NPDS** | 0x0E (in "NPDS"-offset of 0x14) | 0x80 | **Proprietary NVIDIA** |
| 0x48580 | (not a sub-image; ucode content starts with 55AA by chance) | — | — | — |

NPDS is an NVIDIA proprietary replacement for PCIR. Field layout is
PCIR-compatible (vendor/device/`img_len`/`code_type`/`indicator` at the
same offsets) but the sig bytes are `4E 50 44 53` instead of
`50 43 49 52`. openrm's `s_locateExpansionRoms` doesn't mention NPDS;
nova-core's `ImageIter` matches only "PCIR".

The NPDE for image 1 (EFI) says `last_image_byte = 0x1D` (bit 7 clear —
"not last"), so openrm would walk past it. Our walker does too, but
the "next image" position `PciAt_len + EFI_NPDE_sublen = 0x22800`
contains random-looking data, not a 0x55AA header. The real FwSec
image lives ~29 KB further, at `0x29D00`, behind the NPDS header.

## What this means

- FWSEC discovery on GA107 can't be done by copying the openrm /
  nova-core algorithm literally. It needs to either (a) parse NPDS
  sub-images in addition to PCIR, or (b) reverse-engineer the NVIDIA
  private "VN"-prefixed table that sits at 0x24800 (~21 KB of
  structured data with lots of offset-looking fields) which may be the
  real PMU ucode descriptor table for this chip.
- `FalconUcodeTablePtr = 0x76B9E` probably addresses into a virtual
  concatenation we haven't reverse-engineered yet. Could be
  `PciAt + NPDS_image` minus some region; could be a different
  base entirely.

## Next steps (when work resumes)

1. **Read the "NV" table at 0x24800.** Identify structure: signature,
   length, table of `(app_id, offset, size)` entries. This is likely
   the actual PMU ucode descriptor table on Ampere VBIOSes that use
   NPDS.
2. **Grep recent openrm commits** for "NPDS" — NVIDIA may have
   added handling in a newer revision that the cached 535.113.01
   reference doesn't cover.
3. **Capture nouveau's behavior** under a VM, running the same
   driver against this GPU — if it extracts FWSEC, `ftrace` or
   `bpftrace` on `nvkm_bios_*` will reveal the offsets it reads.
4. Alternative: implement `gsp-harness --dump-rom` that saves the
   raw /dev/mem ROM bytes, publish a stripped/hashed fingerprint so
   the community can cross-check — several Reddit/Linux-hw threads
   have discussed GA107 VBIOS quirks.

## Investigation tools

Left in `/tmp` on the dev box (not committed — see reference files
in `docs/reference/` for what we pulled from openrm and nova-core):

- `/tmp/dump_rom_mem.c` — reads GPU Expansion ROM via `/dev/mem`
  after caller enables the ROM BAR. Ran on test-pc 2026-04-14.
- `/tmp/walk_npde.c` — NPDE-aware PCIR chain walker. Correctly
  walks the first two sub-images but misses the NPDS-only image.
- `/tmp/scan_pcir.c` — PCIR-only walker. Stops at "LAST" like the
  Linux kernel does.
- `/tmp/check_npde.c` — per-image PCIR/NPDE dumper. Used to
  discover that image 2 signs as NPDS instead of PCIR.

These are throwaway — the re-usable parts will be integrated into
`kernel/gpu/nvidia/nvidia_vbios.c` once the algorithm is confirmed.

## Cost/benefit note

Implementing this correctly without official docs is probably 2–4
days of focused reverse-engineering. The capstone deliverable timeline
may want to consider whether GPU inference on Ampere (this card) is
worth that cost vs. a CPU fallback (already working via `sse_kernels.c`
at ~2 GFLOPS FP32). The answer is likely still "yes" — a single
matmul on Ampere is ~100–1000× faster — but the calendar cost is now
visible.
