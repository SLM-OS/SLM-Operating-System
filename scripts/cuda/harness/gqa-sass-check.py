#!/usr/bin/env python3
"""Check that the HMMA GQA kernels are launchable by SLM-OS (#1167).

Applies scripts/cuda/oplib-extract.py's own rules to the cubin (no relocation,
no constant bank beyond cbuf 0, no stack, parameters from 0x160) and two more
that the SLM program depends on: the cbuf 0 ABI equals slm_gqa_w8's (the
encoder can swap the kernel without new argument code), and no instruction
reads cbuf 0 below 0x160 except 0x28 and 0x118, which every production kernel
reads (slm_prog.c zeroes that area, so blockDim and gridDim would read 0).
Prints one line per kernel; exit status 1 on any problem.

    gqa-sass-check.py <cubin>
"""
import importlib.util
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("oplib_extract", HERE.parent / "oplib-extract.py")
ox = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ox)

ABI = ["q", "k", "v", "pos", "out", "nhq", "nhkv", "hd", "B"]
W8_OFFSETS = [0x160, 0x168, 0x170, 0x178, 0x180, 0x188, 0x18C, 0x190, 0x194]
ALLOWED_LOW = {0x28, 0x118}
# The operator library's build (no GQA_P16_VARIANTS).
EXPECT = {"slm_gqa_hmma_dec", "slm_gqa_hmma_pf"}


def low_cbuf_reads(cubin, mangled):
    tool = "nvdisasm"
    out = subprocess.run([tool, "-c", cubin], capture_output=True, text=True, check=True).stdout
    found = set()
    in_kernel = False
    for line in out.splitlines():
        if line.startswith(".text."):
            in_kernel = line.startswith(".text." + mangled)
        elif in_kernel:
            for m in re.finditer(r"c\[0x0\]\[(0x[0-9a-f]+)\]", line):
                off = int(m.group(1), 16)
                if off < 0x160 and off not in ALLOWED_LOW:
                    found.add(off)
    return sorted(found)


def main():
    cubin = sys.argv[1]
    kernels = ox.read_kernels(cubin)
    bad = 0
    if set(kernels) != EXPECT:
        print(f"kernels {sorted(kernels)}, expected {sorted(EXPECT)}")
        bad += 1
    for name in sorted(kernels):
        k = kernels[name]
        hmma, stack = ox.count_instr(cubin, k["mangled"])
        problems = list(k["problems"])
        if stack:
            problems.append(f"{stack} stack or absolute-call instructions")
        if not hmma:
            problems.append("no HMMA instruction")
        offs = [k["param_base"] + off for _, off, _ in k["params"]]
        if offs != W8_OFFSETS:
            problems.append(f"cbuf ABI {[hex(o) for o in offs]}, slm_gqa_w8 has "
                            f"{[hex(o) for o in W8_OFFSETS]}")
        low = low_cbuf_reads(cubin, k["mangled"])
        if low:
            problems.append("reads cbuf 0 below the arguments at " + ", ".join(hex(o) for o in low))
        cbuf0 = k["param_base"] + k["param_size"]
        print(f"{name}: regs {k['regs']} smem {k['smem']} code {len(k['code'])} B cbuf0 {cbuf0} "
              f"HMMA {hmma} abi {','.join(ABI)} @ {hex(offs[0]) if offs else '-'}: "
              + ("ok" if not problems else "PROBLEM " + "; ".join(problems)))
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
