#!/bin/bash
#
# test-oplib-shaders.sh - host test for `make oplib-shaders` (#475).
#
# Without a toolkit: the build must stop with exit 1 and instructions, and the
# extractor must read launch facts from a synthetic cubin and refuse kernels
# SLM-OS could not launch. With nvcc present: a real build, then every manifest
# entry must match its .sass by size and hash and carry a sane ABI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

# 1. Missing toolkit gives a clear message and exit 1.
mkdir -p "$TMP/bin"
for t in bash sed dirname cat mktemp rm; do ln -s "$(command -v "$t")" "$TMP/bin/$t"; done
rc=0
PATH="$TMP/bin" NVCC=/nonexistent/nvcc bash "$ROOT/scripts/cuda/build-oplib-shaders.sh" \
    "$TMP/none" > "$TMP/missing.log" 2>&1 || rc=$?
if [ -x /usr/local/cuda/bin/nvcc ]; then
    echo "SKIP: missing-toolkit case, /usr/local/cuda/bin/nvcc exists"
else
    [ "$rc" = 1 ] || fail "missing toolkit: exit $rc, expected 1"
    grep -q "nvcc not found" "$TMP/missing.log" || fail "missing toolkit: no message"
    grep -q "stays on the CPU" "$TMP/missing.log" || fail "missing toolkit: consequence not stated"
    [ ! -e "$TMP/none" ] || fail "missing toolkit: created output"
    echo "PASS: missing toolkit stops with instructions"
fi

# 1b. A toolkit other than the pinned one stops with exit 3 unless allowed.
mkdir -p "$TMP/fakecuda"
cat > "$TMP/fakecuda/nvcc" <<'EOF'
#!/bin/sh
echo "Cuda compilation tools, release 12.6, V12.6.68"
EOF
chmod +x "$TMP/fakecuda/nvcc"
rc=0
NVCC="$TMP/fakecuda/nvcc" bash "$ROOT/scripts/cuda/build-oplib-shaders.sh" "$TMP/pin-out" \
    > "$TMP/pin.log" 2>&1 || rc=$?
[ "$rc" = 3 ] || fail "toolkit pin: exit $rc, expected 3"
grep -q "pinned to" "$TMP/pin.log" || fail "toolkit pin: no message"
[ ! -e "$TMP/pin-out" ] || fail "toolkit pin: created output"
echo "PASS: unpinned toolkit stops with exit 3"

# 2. Extractor on a synthetic cubin.
python3 - "$ROOT/scripts/cuda/oplib-extract.py" "$TMP" <<'EOF'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("x", sys.argv[1])
x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x)
tmp = sys.argv[2]

def cubin(sections):
    """ELF64 with [(name, type, body, info)], section 0 null, strtab last."""
    names = b"\0"
    offs = []
    for s in sections:
        offs.append(len(names))
        names += s[0].encode() + b"\0"
    strn = len(names)
    names += b".shstrtab\0"
    blob = b""
    base = 0x40
    hdrs = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for s, n in zip(sections, offs):
        hdrs.append(struct.pack("<IIQQQQIIQQ", n, s[1], 0, 0, base + len(blob),
                                len(s[2]), 0, s[3], 4, 0))
        blob += s[2]
    hdrs.append(struct.pack("<IIQQQQIIQQ", strn, 3, 0, 0, base + len(blob), len(names),
                            0, 0, 1, 0))
    blob += names
    shoff = base + len(blob)
    eh = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    eh += struct.pack("<HHIQQQIHHHHHH", 2, 190, 1, 0, 0, shoff, 0, 64, 0, 0, 64,
                      len(hdrs), len(hdrs) - 1)
    return eh + blob + b"".join(hdrs)

def info(params):
    b = struct.pack("<BBHIHH", 4, x.EIATTR_PARAM_CBANK, 8, 1, 0x160, 0x10)
    for ordinal, off, size in params:
        b += struct.pack("<BBHIHHI", 4, x.EIATTR_KPARAM_INFO, 12, 0, ordinal, off,
                         (size << 18) | 0x1F000)
    return b

m = "_Z7slm_addPKfi"
code = bytes(range(32))
secs = [(".text." + m, 1, code, 0x0C00000A),
        (".nv.shared." + m, 8, b"\0" * 0, 0),
        (".nv.info." + m, 0x70000000, info([(1, 8, 4), (0, 0, 8)]), 0)]
path = tmp + "/a.cubin"
open(path, "wb").write(cubin(secs))
k = x.read_kernels(path)["slm_add"]
assert k["code"] == code and k["regs"] == 12, k
assert k["param_base"] == 0x160 and k["param_size"] == 0x10, k
assert k["params"] == [(0, 0, 8), (1, 8, 4)], k["params"]
assert not k["problems"], k["problems"]
print("PASS: extractor reads code, regs and params from the ELF")

for extra, want in [(".rel.text." + m, "relocation"), (".nv.constant3." + m, "constant bank"),
                    (".nv.local." + m, "stack")]:
    bad = tmp + "/b.cubin"
    open(bad, "wb").write(cubin(secs + [(extra, 1, b"\0" * 8, 0)]))
    probs = x.read_kernels(bad)["slm_add"]["problems"]
    assert any(want in p for p in probs), (extra, probs)
print("PASS: extractor flags relocations, extra constant banks and stack use")

assert x.kernel_name("_Z13slm_kv_appendPK6__half") == "slm_kv_append"
assert x.kernel_name("plain") == "plain"
assert set(x.ARGS) <= set(x.OPS)
print("PASS: names and op table")
EOF

# 3. The harness's CPU reference judges every GPU kernel on the board, so it is
#    checked against the ggml goldens here.
if command -v g++ >/dev/null 2>&1; then
    g++ -O2 -std=c++17 "$ROOT/scripts/cuda/harness/test_ref.cpp" -o "$TMP/test_ref" -lm
    "$TMP/test_ref" "$ROOT/scripts/fixtures/slm/golden_ops.rec" > "$TMP/ref.log" \
        || { cat "$TMP/ref.log"; fail "CPU reference vs goldens"; }
    echo "PASS: harness CPU reference matches the ggml goldens"
else
    echo "SKIP: CPU reference check, no g++"
fi

# 4. Real build when a toolkit is there.
if ! command -v nvcc >/dev/null 2>&1 && [ ! -x /usr/local/cuda/bin/nvcc ]; then
    echo "SKIP: real build, no nvcc"
    exit 0
fi
REAL_NVCC="$(command -v nvcc 2>/dev/null || echo /usr/local/cuda/bin/nvcc)"
REAL_VER="$("$REAL_NVCC" --version | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)"
if [ "$REAL_VER" != "12.0" ]; then
    echo "NOTE: real build with CUDA $REAL_VER, not the pinned 12.0 (ALLOW_CUDA_MISMATCH=1)"
    export ALLOW_CUDA_MISMATCH=1
fi
MANIFEST_OUT="$TMP/MANIFEST.json" bash "$ROOT/scripts/cuda/build-oplib-shaders.sh" "$TMP/out" \
    > "$TMP/build.log" 2>&1 || { cat "$TMP/build.log"; fail "real build"; }
python3 - "$TMP/MANIFEST.json" "$TMP/out" "$ROOT/scripts/cuda/slm/MANIFEST.json" <<'EOF'
import hashlib, json, os, sys
m = json.load(open(sys.argv[1]))
want_tiers = {"slm_mm_q4k_hmma_b8": "HMMA", "slm_mm_q4k_simt": "SIMT",
              "slm_mm_q6k_hmma_b64": "HMMA", "slm_gqa": "SIMT"}
names = {e["name"]: e for e in m["entries"]}
for n, t in want_tiers.items():
    assert names[n]["tier"] == t, n
for e in m["entries"]:
    data = open(os.path.join(sys.argv[2], e["sass"]), "rb").read()
    assert len(data) == e["code_bytes"] and hashlib.sha256(data).hexdigest() == e["sha256"], e["name"]
    assert len(data) % 128 == 0 and data, e["name"]
    assert e["block"][2] == 1, e["name"]
    if e["name"] in ("slm_add_w8", "slm_swiglu_w8", "slm_swiglu_q8"):
        assert e["block"] == [256, 1, 1], e["name"]
    else:
        assert e["block"][0] == 32, e["name"]
        want = 8 if e["name"].endswith(("_w8", "_bs", "_bsq", "_q8")) else "nw" if e["name"].endswith("_b1") else 1
        assert e["block"][1] == want, e["name"]
    assert 0 < e["regs"] < 256, e["name"]
    off = 0x160
    for a in e["abi"]:
        assert a["offset"] >= off and a["offset"] % a["size"] == 0, (e["name"], a)
        off = a["offset"] + a["size"]
    assert off == e["cbuf0_size"], e["name"]
    if e["tier"] == "HMMA":
        assert e["hmma_instructions"] > 0, e["name"]
# The checked-in table agrees on the ABI whatever toolkit built it.
ref = {e["name"]: e for e in json.load(open(sys.argv[3]))["entries"]}
assert set(ref) == set(names)
for n, e in names.items():
    assert ref[n]["abi"] == e["abi"] and ref[n]["grid"] == e["grid"], n
print(f"PASS: real build, {len(names)} SLM kernels match the manifest and the checked-in ABI")
EOF
n=$(ls "$TMP/out"/*_shader.sass | wc -l)
[ "$n" -ge 42 ] || fail "only $n shaders built"
echo "PASS: $n shaders built"
