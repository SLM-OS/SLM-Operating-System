#!/usr/bin/env python3
"""Carve raw SASS out of nvcc cubins and write the SLM kernel op table (#475).

SLM-OS launches these kernels with no CUDA runtime, so everything a QMD needs
has to be known at build time. This reads it from the ELF instead of trusting
hand-kept numbers: code size, register count, shared memory, parameter offsets.
It also refuses a kernel SLM-OS could not launch: one with a relocation in its
code, a constant bank beyond cbuf 0, or stack use.

    oplib-extract.py sass <cubin> <kernel> <out.sass>
    oplib-extract.py manifest <toolkit> <out.json> <sass-dir> <cubin>...
"""

import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SHT_NOBITS = 8
EIATTR_KPARAM_INFO = 0x17
EIATTR_PARAM_CBANK = 0x0A

MM_ARGS = "x, W, out, bias, K, N, B, flags"
MM_DOC = ("out[B,N] = x[B,K] @ W[N,K]^T + bias[N]; flags bit 0 means FP16 out; "
          "x must be 16-byte aligned")

# Launch shapes and contracts that the ELF cannot tell. Every entry has block
# (32,1,1). The grid is a formula over the named parameters.
OPS = {
    "slm_mm_q4k_simt": ("matmul", "SIMT", "Q4_K", "N,B,1", MM_DOC + ". Any B."),
    "slm_mm_q4k_hmma_b8": ("matmul", "HMMA", "Q4_K", "ceil(N/15),1,1", MM_DOC + ". B 1..8."),
    "slm_mm_q4k_hmma_b64": ("matmul", "HMMA", "Q4_K", "ceil(N/15),1,1", MM_DOC + ". B 1..64."),
    "slm_mm_q6k_simt": ("matmul", "SIMT", "Q6_K", "N,B,1", MM_DOC + ". Any B."),
    "slm_mm_q6k_hmma_b8": ("matmul", "HMMA", "Q6_K", "ceil(N/15),1,1", MM_DOC + ". B 1..8."),
    "slm_mm_q6k_hmma_b64": ("matmul", "HMMA", "Q6_K", "ceil(N/15),1,1", MM_DOC + ". B 1..64."),
    "slm_embed_q4k": ("embed", "SIMT", "Q4_K", "B,1,1", "out[B,K] = table[ids[b]], FP16."),
    "slm_embed_q6k": ("embed", "SIMT", "Q6_K", "B,1,1", "out[B,K] = table[ids[b]], FP16."),
    "slm_rmsnorm": ("rmsnorm", "SIMT", "F32 gamma", "B,1,1", "x and out FP16 [B,n], n % 32 == 0."),
    "slm_add": ("add", "SIMT", "FP16", "ceil(total/32),1,1", "out[i] = a[i] + b[i], total = B*n."),
    "slm_add_bias": ("add_bias", "SIMT", "F32 bias", "n/32,B,1", "out[B,n] = x[B,n] + bias[n]."),
    "slm_swiglu": ("swiglu", "SIMT", "FP16", "ceil(total/32),1,1", "out[i] = silu(gate[i]) * up[i]."),
    "slm_rope": ("rope", "SIMT", "FP16", "nh,B,1",
                 "Half-split pairing, in place allowed; table [max_pos][hd] of FP32 cos,sin; "
                 "pos u32 [B]."),
    "slm_kv_append": ("kv_append", "SIMT", "FP16", "width/32,B,1",
                      "kv[pos[b]][0..width) = src[b][0..width); pos u32 [B]."),
    "slm_gqa": ("gqa", "SIMT", "FP16", "nhq,B,1",
                "Online softmax; query row b sees 0..=pos[b]. q,out [B][nhq][hd]; "
                "k,v [T][nhkv][hd]; hd % 32 == 0, hd <= 256."),
    "slm_gqa_w8": ("gqa", "SIMT", "FP16", "nhq,B,1",
                   "As slm_gqa, positions split across 8 warps. Prefer this one."),
    "slm_rmsnorm_w8": ("rmsnorm", "SIMT", "F32 gamma", "B,1,1",
                       "As slm_rmsnorm, across 8 warps. Prefer this one."),
}

# Kernels whose block is not (32,1,1).
BLOCKS = {"slm_gqa_w8": [32, 8, 1], "slm_rmsnorm_w8": [32, 8, 1]}

# Parameter names per kernel, checked against the count in the ELF.
ARGS = {
    "slm_embed_q4k": "ids, table, out, K, B",
    "slm_embed_q6k": "ids, table, out, K, B",
    "slm_rmsnorm": "x, gamma, out, n, B, eps",
    "slm_add": "a, b, out, total",
    "slm_add_bias": "x, bias, out, n, B",
    "slm_swiglu": "gate, up, out, total",
    "slm_rope": "x, out, table, pos, nh, hd, B",
    "slm_kv_append": "src, kv, pos, width, B",
    "slm_gqa": "q, k, v, pos, out, nhq, nhkv, hd, B",
    "slm_gqa_w8": "q, k, v, pos, out, nhq, nhkv, hd, B",
    "slm_rmsnorm_w8": "x, gamma, out, n, B, eps",
}


class ExtractError(Exception):
    pass


def parse_elf(data):
    """Return the section list of a little-endian ELF64 as (name, type, off, size, info)."""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ExtractError("not an ELF64 cubin")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    raw = []
    for i in range(shnum):
        raw.append(struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize))
    strs = raw[shstrndx]
    strtab = data[strs[4]:strs[4] + strs[5]]
    out = []
    for s in raw:
        end = strtab.index(b"\0", s[0])
        out.append((strtab[s[0]:end].decode(), s[1], s[4], s[5], s[7]))
    return out


def kernel_name(mangled):
    """_Z7slm_addPK... -> slm_add; an unmangled name passes through."""
    m = re.match(r"_Z(\d+)", mangled)
    if not m:
        return mangled
    n = int(m.group(1))
    return mangled[m.end():m.end() + n]


def parse_info(blob):
    """Walk a .nv.info.<kernel> section into (param_base, param_size, params)."""
    params = []
    base = None
    size = None
    o = 0
    while o + 4 <= len(blob):
        fmt, attr = blob[o], blob[o + 1]
        if fmt == 4:
            n = struct.unpack_from("<H", blob, o + 2)[0]
            val = blob[o + 4:o + 4 + n]
            o += 4 + n
        else:
            val = b""
            o += 4
        if attr == EIATTR_KPARAM_INFO:
            _, ordinal, off, flags = struct.unpack("<IHHI", val)
            params.append((ordinal, off, (flags >> 18) & 0x3FFF))
        elif attr == EIATTR_PARAM_CBANK:
            _, base, size = struct.unpack("<IHH", val)
    params.sort()
    return base, size, params


def read_kernels(path):
    """All kernels of one cubin as dict name -> facts and code bytes."""
    data = Path(path).read_bytes()
    sections = parse_elf(data)
    by_name = {s[0]: s for s in sections}
    kernels = {}
    for name, _type, off, size, info in sections:
        if not name.startswith(".text."):
            continue
        mangled = name[len(".text."):]
        kname = kernel_name(mangled)
        shared = by_name.get(".nv.shared." + mangled)
        inf = by_name.get(".nv.info." + mangled)
        base, psize, params = (None, None, [])
        if inf:
            base, psize, params = parse_info(data[inf[2]:inf[2] + inf[3]])
        problems = []
        for other in sections:
            oname = other[0]
            if oname in (".rel.text." + mangled, ".rela.text." + mangled):
                problems.append("relocation section " + oname)
            if re.match(r"\.nv\.constant[1-9]\.", oname) and oname.endswith(mangled):
                problems.append("extra constant bank " + oname)
            if oname == ".nv.local." + mangled:
                problems.append("uses stack")
        kernels[kname] = {
            "mangled": mangled,
            "code": data[off:off + size],
            "regs": info >> 24,
            "smem": shared[3] if shared else 0,
            "param_base": base,
            "param_size": psize,
            "params": params,
            "problems": problems,
        }
    return kernels


def count_instr(cubin, mangled):
    """Count HMMA, and stack or absolute-call instructions, via nvdisasm if present."""
    tool = shutil.which("nvdisasm") or "/usr/local/cuda/bin/nvdisasm"
    if not Path(tool).exists():
        return None, None
    out = subprocess.run([tool, "-c", cubin], capture_output=True, text=True, check=False).stdout
    in_kernel = False
    hmma = 0
    bad = 0
    for line in out.splitlines():
        if line.startswith(".text."):
            in_kernel = line.startswith(".text." + mangled)
        elif in_kernel:
            if re.search(r"\bHMMA\b", line):
                hmma += 1
            if re.search(r"\b(LDL|STL|CAL\.ABS)\b", line):
                bad += 1
    return hmma, bad


def cmd_sass(args):
    cubin, kernel, out = args
    kernels = read_kernels(cubin)
    if kernel not in kernels:
        raise ExtractError(f"{cubin}: no kernel {kernel}, have {sorted(kernels)}")
    Path(out).write_bytes(kernels[kernel]["code"])
    print(f"  {kernel}: {len(kernels[kernel]['code'])} bytes -> {out}")


def cmd_manifest(args):
    toolkit, out, sass_dir = args[0], args[1], Path(args[2])
    entries = []
    for cubin in args[3:]:
        for name, k in sorted(read_kernels(cubin).items()):
            if name not in OPS:
                raise ExtractError(f"{cubin}: kernel {name} has no entry in OPS")
            op, tier, dtype, grid, doc = OPS[name]
            hmma, bad = count_instr(cubin, k["mangled"])
            problems = list(k["problems"])
            if bad:
                problems.append(f"{bad} stack or absolute-call instructions")
            if tier == "HMMA" and hmma == 0:
                problems.append("HMMA tier with no HMMA instruction")
            if problems:
                raise ExtractError(f"{name}: not launchable: {'; '.join(problems)}")
            argn = [a.strip() for a in ARGS.get(name, MM_ARGS).split(",")]
            if len(argn) != len(k["params"]):
                raise ExtractError(f"{name}: {len(k['params'])} params, {len(argn)} named")
            if k["param_base"] != 0x160:
                raise ExtractError(f"{name}: param base {k['param_base']:#x}, expected 0x160")
            sass = sass_dir / (name + "_shader.sass")
            entries.append({
                "name": name,
                "op": op,
                "tier": tier,
                "dtype": dtype,
                "sass": sass.name,
                "source": "scripts/cuda/slm/" + Path(cubin).stem + ".cu",
                "code_bytes": len(k["code"]),
                "sha256": hashlib.sha256(k["code"]).hexdigest(),
                "grid": grid,
                "block": BLOCKS.get(name, [32, 1, 1]),
                "smem_bytes": k["smem"],
                "regs": k["regs"],
                "hmma_instructions": hmma,
                "cbuf0_size": k["param_base"] + k["param_size"],
                "abi": [
                    {"name": n, "offset": k["param_base"] + off, "size": size}
                    for n, (_, off, size) in zip(argn, k["params"])
                ],
                "doc": doc,
            })
    missing = sorted(set(OPS) - {e["name"] for e in entries})
    if missing:
        raise ExtractError(f"kernels in OPS but in no cubin: {missing}")
    manifest = {
        "version": 1,
        "comment": "Generated by scripts/cuda/oplib-extract.py from the cubins; do not edit. "
                   "regs, code_bytes and sha256 belong to the named toolkit, so regenerate with "
                   "`make oplib-shaders` whenever the shaders are rebuilt. All entries launch "
                   "with no stack and no extra constant banks.",
        "toolkit": toolkit,
        "arch": "sm_87",
        "entries": entries,
    }
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"  manifest: {len(entries)} kernels -> {out}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    try:
        if sys.argv[1] == "sass" and len(sys.argv) == 5:
            cmd_sass(sys.argv[2:])
        elif sys.argv[1] == "manifest" and len(sys.argv) >= 6:
            cmd_manifest(sys.argv[2:])
        else:
            raise SystemExit(__doc__)
    except ExtractError as e:
        print(f"oplib-extract: {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
