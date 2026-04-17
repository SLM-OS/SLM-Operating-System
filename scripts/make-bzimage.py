#!/usr/bin/env python3
"""
make-bzimage.py — wrap an SLM-OS ELF as a Linux-compatible bzImage.

Produces `<OUT>` = [ 1024-byte setup area | flat payload ] such that
`kexec --type=bzImage --load <OUT>` on a 64-bit host is accepted by
kexec-tools' bzImage64 loader, which then loads the payload at
`pref_address` (0x20000000) and jumps to `load_addr + 0x200` in
64-bit long mode.

The setup area is hand-crafted in Python rather than linked into the
ELF because the setup area isn't part of the payload — kexec reads
setup separately from the file preamble and only loads the payload
into memory.

Required input ELF conventions:
  - Linked with kernel-x86_64-bzimage.ld (KERNEL_PHYS = 0x20000000
    with a .bzimage_entry section starting at KERNEL_PHYS + 0x200).
  - Built with BZIMAGE_BUILD=1 so the bzimage_entry.S stub is
    present.

Usage:
  make-bzimage.py <input.elf> <output.bzimage>

The bzImage setup_header layout we populate is the minimal subset
the bzImage64 loader validates in kexec-tools 2.0.28
(kexec/arch/x86_64/kexec-bzImage64.c:46-94):

| Offset | Field           | Value we write
| 0x000  | boot-sector     | "\\xeb\\x3d\\x90" (harmless NOP for kexec)
| 0x1F1  | setup_sects     | 1  (1 sector of extra setup → 1024-byte total)
| 0x1FE  | boot_flag       | 0xAA55
| 0x200  | jump            | "\\xeb\\x66" (short jmp, unused but valid)
| 0x202  | header_magic    | "HdrS"
| 0x206  | version         | 0x020D (2.13)
| 0x211  | loadflags       | 0x01 (LOADED_HIGH)
| 0x230  | kernel_align    | 0x00200000 (2 MB)
| 0x234  | relocatable     | 1
| 0x236  | xloadflags      | 0x0003 (KERNEL_64 | CAN_BE_LOADED_ABOVE_4G)
| 0x238  | cmdline_size    | 0x00000800
| 0x258  | pref_address    | 0x0000000020000000
| 0x260  | init_size       | <computed>
| 0x201  | setup_hdr_end   | 0x6E (covers through init_size+4)

All other bytes in 0x000..0x400 are zero.

Payload construction:
  Walk the ELF program headers, concatenate PT_LOAD segments at their
  (p_paddr - KERNEL_PHYS) offsets, zero-fill any holes, extend to
  p_memsz (beyond p_filesz) with zeros — so the first byte of the
  payload maps to phys 0x20000000 and byte 0x200 maps to 0x20000200
  (the bzImage entry stub). init_size is the total MemSiz of the
  payload = the last p_paddr+p_memsz minus KERNEL_PHYS, aligned up.
"""

import struct
import sys
from pathlib import Path

KERNEL_PHYS = 0x20000000
PREF_ADDRESS = KERNEL_PHYS
SETUP_AREA_SIZE = 0x400  # 1024 bytes: boot sector (512) + 1 extra sector


def die(msg: str) -> None:
    print(f"make-bzimage: error: {msg}", file=sys.stderr)
    sys.exit(1)


def load_elf_payload(elf_path: Path) -> bytes:
    """Read PT_LOAD segments from a 64-bit ELF and flatten to a payload.

    Returns a bytes object whose [i] corresponds to phys KERNEL_PHYS + i.
    The payload extends through the highest p_paddr+p_memsz of any
    PT_LOAD, zero-filling any gaps.
    """
    blob = elf_path.read_bytes()

    if blob[:4] != b"\x7fELF":
        die(f"{elf_path}: not an ELF file")
    if blob[4] != 2:
        die(f"{elf_path}: expected ELF64 (EI_CLASS=2), got EI_CLASS={blob[4]}")
    if blob[5] != 1:
        die(f"{elf_path}: expected little-endian (EI_DATA=1)")

    # ELF64 header: e_phoff @ 32 (u64), e_phentsize @ 54 (u16), e_phnum @ 56 (u16)
    e_phoff = struct.unpack_from("<Q", blob, 32)[0]
    e_phsize = struct.unpack_from("<H", blob, 54)[0]
    e_phnum = struct.unpack_from("<H", blob, 56)[0]

    # Find highest phys endpoint across PT_LOAD phdrs.
    max_end = KERNEL_PHYS
    loads = []
    for i in range(e_phnum):
        ph = e_phoff + i * e_phsize
        (p_type, p_flags, p_offset, p_vaddr, p_paddr,
         p_filesz, p_memsz, _align) = struct.unpack_from("<IIQQQQQQ", blob, ph)
        if p_type != 1:  # PT_LOAD
            continue
        if p_paddr < KERNEL_PHYS:
            die(f"{elf_path}: PT_LOAD at p_paddr=0x{p_paddr:x} below "
                f"KERNEL_PHYS=0x{KERNEL_PHYS:x} — wrong linker script?")
        end = p_paddr + p_memsz
        if end > max_end:
            max_end = end
        loads.append((p_offset, p_paddr, p_filesz, p_memsz))

    if not loads:
        die(f"{elf_path}: no PT_LOAD segments")

    payload_size = max_end - KERNEL_PHYS
    payload = bytearray(payload_size)

    # Copy file-backed bytes; bytes beyond p_filesz stay zero (BSS).
    for (p_offset, p_paddr, p_filesz, _p_memsz) in loads:
        dst_off = p_paddr - KERNEL_PHYS
        payload[dst_off:dst_off + p_filesz] = blob[p_offset:p_offset + p_filesz]

    return bytes(payload)


def build_setup_area(init_size: int) -> bytes:
    """Build the 1024-byte bzImage setup header + boot sector.

    `init_size` is the runtime memory footprint kexec should allocate
    for the payload — MemSiz, not FileSiz. We round up to 4 KiB
    because kexec does too and the math is easier.
    """
    setup = bytearray(SETUP_AREA_SIZE)

    # Boot sector: must start with something that looks like x86 code
    # (`jmp short 0x3f; nop` => 0xeb 0x3d 0x90). Not executed by kexec.
    setup[0:3] = b"\xeb\x3d\x90"

    # 0x1F1 setup_sects = 1  → setup is (1+1)*512 = 1024 bytes total.
    setup[0x1F1] = 1

    # 0x1FE boot_flag = 0xAA55 (required).
    struct.pack_into("<H", setup, 0x1FE, 0xAA55)

    # 0x200 2-byte "jump" — unused by kexec but some code paths look
    # at it. `jmp short 0x202 + 0x66` is a safe placeholder.
    setup[0x200:0x202] = b"\xeb\x66"

    # 0x202 header_magic = "HdrS"
    setup[0x202:0x206] = b"HdrS"

    # 0x206 protocol_version = 0x020D (2.13) — kexec requires >= 2.12
    struct.pack_into("<H", setup, 0x206, 0x020D)

    # 0x211 loadflags: bit 0 LOADED_HIGH = 1
    setup[0x211] = 0x01

    # 0x230 kernel_alignment = 2 MB
    struct.pack_into("<I", setup, 0x230, 0x00200000)

    # 0x234 relocatable_kernel = 1 (lets kexec place us at pref_address)
    setup[0x234] = 0x01

    # 0x235 min_alignment exponent = 21 (1 << 21 = 2 MB).
    setup[0x235] = 21

    # 0x236 xloadflags: bit 0 KERNEL_64, bit 1 CAN_BE_LOADED_ABOVE_4G
    struct.pack_into("<H", setup, 0x236, 0x0003)

    # 0x238 cmdline_size = 0x7FF (generous)
    struct.pack_into("<I", setup, 0x238, 0x000007FF)

    # 0x258 pref_address = 0x20000000 (u64)
    struct.pack_into("<Q", setup, 0x258, PREF_ADDRESS)

    # 0x260 init_size (runtime footprint, rounded up to 4 KiB)
    rounded_init = (init_size + 0xFFF) & ~0xFFF
    struct.pack_into("<I", setup, 0x260, rounded_init)

    # 0x201 setup_header_end = size of header beyond 0x202. kexec
    # copies from 0x1F1 through 0x202 + kernel[0x201]. Our last
    # populated field is at 0x260+4 = 0x264, so we need 0x201 >=
    # 0x62. Use 0x6E for margin (= 0x270 - 0x202).
    setup[0x201] = 0x6E

    return bytes(setup)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make-bzimage.py <input.elf> <output.bzimage>",
              file=sys.stderr)
        return 2

    inp = Path(sys.argv[1])
    out = Path(sys.argv[2])

    if not inp.is_file():
        die(f"input not found: {inp}")

    payload = load_elf_payload(inp)

    # Sanity: payload[0x200] must be the bzImage entry stub's first
    # instruction (`cli` = 0xfa). Catches KERNEL_PHYS drift and
    # missing .bzimage_entry section.
    if len(payload) < 0x201:
        die(f"payload only {len(payload)} bytes — too small to contain "
            f"the bzImage entry at offset 0x200")
    if payload[0x200] != 0xfa:
        die(f"payload[0x200] = 0x{payload[0x200]:02x}, expected 0xfa "
            f"(cli — bzimage_entry.S marker). Was the kernel built "
            f"with BZIMAGE_BUILD=1 and the bzimage linker script?")

    setup = build_setup_area(init_size=len(payload))

    bzimage = setup + payload
    out.write_bytes(bzimage)

    print(f"bzImage: {out}")
    print(f"  setup area : {len(setup)} bytes")
    print(f"  payload    : {len(payload)} bytes ({len(payload)//1024} KiB)")
    print(f"  total      : {len(bzimage)} bytes ({len(bzimage)//1024} KiB)")
    print(f"  pref_addr  : 0x{PREF_ADDRESS:x}")
    print(f"  init_size  : 0x{(len(payload) + 0xFFF) & ~0xFFF:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
