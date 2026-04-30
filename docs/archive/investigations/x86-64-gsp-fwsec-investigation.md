# FWSEC Discovery on RTX 3050 (GA107) — Investigation Notes

**Status (2026-04-14, end-of-day):** **RESOLVED.** FWSEC extracts
cleanly on the RTX 3050. Both #143 and #150 can close.

Final session arc:
1. First attempt failed because the cached openrm 535.113.01
   doesn't handle NPDS-format FwSec images.
2. Fresh references (openrm 595, nova-core mainline) gave the
   correct algorithm: PciAt | FwSec1 | FwSec2 concatenation with
   two-subtraction pointer math.
3. Implementing it produced a correct pointer that landed past
   the 512 KB PCI Expansion ROM BAR → appeared to need ACPI `_ROM`.
4. **Actual fix:** the proprietary / open-RM driver never uses the
   Expansion ROM BAR. It reads from **BAR0 + 0x00300000** — the
   1 MB `NV_PROM_DATA` window that directly mirrors SPI flash.
   Switching the harness to that path gave us all 4 sub-images
   and the FWSEC pointer resolves to valid descriptor data.
5. Two additional one-liners after that: relax the table-header
   `DescVersion` check (table-level field is informational;
   per-descriptor version is authoritative) and drop `descSize`
   upper cap (1,580 bytes on this card is legitimate — it's 44-byte
   V3 header + 4 × 384-byte RSA-3K signatures).

On the RTX 3050 at test-pc, `gsp-harness --vbios` now reports:
```
[GSP-HARNESS] VBIOS image: 1048576 bytes @ ... via BAR0+0x300000 PROM
[GSP-HARNESS] sub-images (4):
              [0] @0x000000  code_type=0x00 (PciAt)  len=65024
              [1] @0x00fe00  code_type=0x03 (EFI)    len=84480
              [2] @0x024800  code_type=0xE0 (FwSec)  len=22016
              [3] @0x029e00  code_type=0xE0 (FwSec)  len=399872
[GSP-HARNESS] FWSEC: 62124 bytes @ 0x...
```

The 62,124 bytes break down as 44 (V3 descriptor header) + 1,536
(4 signatures × 384) + 58,112 (IMEM) + 2,432 (DMEM). Matches what
openrm's `kgspExtractVbiosFromRom_TU102` produces.

Algorithm ships in `kernel/gpu/nvidia/nvidia_vbios.c`.
Access path in harness is `host-tools/gsp-harness/linux_platform.c`
(`read_vbios_via_prom_window`). The bare-metal x86-64 platform
shim will need the same treatment — tracked as a small follow-up.

Details below preserved for future sessions debugging a different
card or SKU.

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

The openrm/nova-core documentation cached in `../slmos-reference-cache/` assumes
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
in `../slmos-reference-cache/` for what we pulled from openrm and nova-core):

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

---

## 2026-04-14 session resolution

**The algorithm dead-ends from the first write-up were caused by
out-of-date reference material.** openrm 535.113.01 does not handle
NPDS-format FwSec images; openrm 595+ and nova-core mainline both do.
With the newer references in hand the sub-image chain becomes:

| Offset | Signature | code_type | Size | Role |
|---|---|---|---|---|
| 0x00000 | 55AA + PCIR | 0x00 | 65,024  | PciAt |
| 0x0FE00 | 55AA + PCIR | 0x03 | 84,480  | EFI (skipped) |
| 0x24800 | (no 55AA) + NPDS @ 0x24960 | 0xE0 | 22,016 | FwSec1 |
| 0x29E00 | 55AA + NPDS @ 0x29E20 | 0xE0 | 399,872 (declared) | FwSec2 (LAST) |

The "VN" signature at 0x24800 was a red herring — it's simply the
first two bytes of an NPDS-only sub-image that doesn't lead with
0x55AA. The real sub-image marker is the NPDS record reached via
`pcir_ptr` at offset 0x18.

**Shipped in this session:**
- Full PCIR/NPDS chain walker in `kernel/gpu/nvidia/nvidia_vbios.c`.
- Nova-core-style pointer resolution (subtract PciAt length, then
  optionally FwSec1 length, to land in the right FwSec image).
- FALCON_UCODE_TABLE_HDR_V1 walker matching `ApplicationID == 0x85`
  (FWSEC_PROD).
- FalconUCodeDescV2/V3 header parsing → full payload size
  (desc + signatures + IMEM + DMEM).
- 28 synthetic tests covering the full chain + truncation,
  bad-version, bad-app-id, and entry-overlap edge cases.

**Remaining blocker — #150:** The ROM BAR on this card exposes
512 KB but the VBIOS declares a 568 KB image. `FalconUcodeTablePtr`
resolves to absolute offset 0x8B59E (past the 0x80000 BAR end),
so the algorithm correctly returns -1. Actual bytes exist on the
flash; getting them requires either ACPI `_ROM` traversal or a
VFIO ROM ioctl path. See #150 for the plan.

**Validated 2026-04-14:** `gsp-harness --vbios` on test-pc RTX 3050
now reports the 2-image chain (sysfs-truncated view), parses 19 BIT
entries cleanly, and prints a specific "ROM dump truncated, needs
ACPI _ROM or full VFIO read" diagnostic instead of the original
generic failure.
