#!/usr/bin/env bash
#
# test-qemu-uefi-models.sh — UEFI model delivery (ADR 0002) under QEMU's
# real AArch64 UEFI firmware.
#
# Each case boots the QEMU_VIRT PE image with \slmos\models on the ESP
# and a different way for the file read to go. The stub's verdict is on
# the console, and a failed read must never stop the boot, so the cases
# require the shell prompt.
#
#   good        small GGUF: read, verified, /boot/models, zero-copy slm load
#   no-file     no \slmos\models at all
#   read-error  QEMU blkdebug fails the file's sectors: the firmware's
#               Read returns EFI_DEVICE_ERROR part-way through the file
#   too-big     the directory entry claims 3 GiB
#   no-memory   500 MiB claimed under 512 MiB of RAM: under the size cap,
#               no contiguous range in the UEFI memory map (this case stops
#               at the kernel banner; see below)
#
# The last two patch the FAT directory entry rather than write huge files.
# The firmware's own AllocatePages refusing a range, and everything else
# only a fake firmware can do on demand, is in kernel/tests/test_boot_models.c.
#
# Usage: scripts/test-qemu-uefi-models.sh <slmos.bin> [timeout-seconds]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?usage: $0 <slmos.bin> [timeout-seconds]}"
TIMEOUT="${2:-120}"
WORK="$ROOT/build/qemu-uefi-models"
BOOT="$ROOT/scripts/qemu-uefi-boot.sh"
MODEL_NAME="qwen2.5-1.5b-instruct-q4_k_m.gguf"

rm -rf "$WORK"
mkdir -p "$WORK/files" "$WORK/oversize"

# A real file name, past the old 31-character VFS limit.
python3 -I "$ROOT/scripts/gguf-boot-fixture.py" "$WORK/files/$MODEL_NAME"
# Padded past the firmware's cluster cache so the read reaches the disk
# as its own request, and blkdebug can fail it after the magic read.
python3 -I - "$WORK/files/$MODEL_NAME" "$WORK/oversize/big.gguf" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "wb") as f:
    f.write(data + bytes(2 * 1024 * 1024))
PY

# Patch the size in big.gguf's directory entry, or locate its data.
cat >"$WORK/esp_edit.py" <<'PY'
import re, struct, sys
esp, mode, arg = sys.argv[1], sys.argv[2], sys.argv[3]
SHORT = re.compile(rb"BIG[ ~0-9]{5}GGU")   # the 8.3 alias mtools gives big.gguf
with open(esp, "r+b") as f:
    img = f.read()
    hits = [m.start() for m in SHORT.finditer(img)]
    assert len(hits) == 1, "directory entry"
    at = hits[0]
    if mode == "size":
        f.seek(at + 28)
        f.write(struct.pack("<I", int(arg)))
    else:   # blkdebug rules for a sector well into the file
        sec, spc, rsvd, nfats = struct.unpack_from("<HBHB", img, 11)
        fat_sz = struct.unpack_from("<I", img, 36)[0]
        hi = struct.unpack_from("<H", img, at + 20)[0]
        lo = struct.unpack_from("<H", img, at + 26)[0]
        first = rsvd + nfats * fat_sz + ((hi << 16 | lo) - 2) * spc
        target = first + (1024 * 1024) // sec
        with open(arg, "w") as c:
            c.write('[inject-error]\nevent = "read_aio"\nerrno = "5"\n'
                    'sector = "%d"\n' % target)
PY

hook() {
    cat >"$WORK/hook-$1.sh" <<EOF
#!/usr/bin/env bash
exec python3 -I "$WORK/esp_edit.py" "\$1" $2 $3
EOF
    chmod +x "$WORK/hook-$1.sh"
    echo "$WORK/hook-$1.sh"
}

require() {  # require <case> <regex>
    local log="$WORK/$1/console.log"
    if ! grep -a -q -E "$2" "$log"; then
        echo "test-qemu-uefi-models: $1: '$2' not on the console ($log)" >&2
        tail -c 2500 "$log" >&2
        exit 1
    fi
}

refuse() {  # refuse <case> <regex>
    local log="$WORK/$1/console.log"
    if grep -a -q -E "$2" "$log"; then
        echo "test-qemu-uefi-models: $1: '$2' must not appear ($log)" >&2
        exit 1
    fi
}

run_case() {  # run_case <case> [ENV=value ...]
    local name="$1"; shift
    env OUT_DIR="$WORK/$name" "$@" bash "$BOOT" "$IMG" \
        "${CASE_TIMEOUT:-$TIMEOUT}" "${CASE_EXPECT:-slmos>}"
}

# --- good: the whole path, relocated the way Jetson's firmware forces. ---
run_case good FORCE_IMAGE_BASE=0x1000 \
    MODEL_FILES="$WORK/files/$MODEL_NAME" \
    SHELL_CMDS="ls /boot/models|slm load /boot/models/$MODEL_NAME|slm info 0|slm unload 0|slm load /boot/models/$MODEL_NAME|slm unload 0|rm /boot/models/$MODEL_NAME|ls /boot/models"
require good "reloc: copy to link address"
require good "\[slmos\] model $MODEL_NAME: read [0-9]+ ms"
require good "Boot model $MODEL_NAME: .*kernel verify"
require good "1 boot model\(s\) at /boot/models"
require good "Boot source: UEFI-direct"
# The full name, in the listing.
require good "$MODEL_NAME"
require good "\[slm\] loaded handle=0 .*blocks=28"
require good "zero-copy from boot memory"
refuse good "failed its check"
# A second load after an unload means the pages were left alone. Two
# "loaded" lines, and rm is refused rather than cast to an LFS mount.
[ "$(grep -a -c '\[slm\] loaded handle=0' "$WORK/good/console.log")" -ge 2 ] || {
    echo "test-qemu-uefi-models: good: second slm load did not succeed" >&2; exit 1; }
require good "Not a mounted filesystem"

# --- no-file ---
run_case no-file
require no-file "\[slmos\] model: none loaded"
refuse no-file "Boot model"
refuse no-file "/boot/models"

# --- read-error: a real EFI_DEVICE_ERROR from the firmware's Read. ---
run_case read-error \
    MODEL_FILES="$WORK/oversize/big.gguf" \
    ESP_HOOK="$(hook read-error blkdebug "$WORK/blkdebug.conf")" \
    BLKDEBUG_CONF="$WORK/blkdebug.conf"
require read-error "\[slmos\] model big.gguf: read error"
require read-error "\[slmos\] model: none loaded"
refuse read-error "Boot model"

# --- too-big ---
run_case too-big \
    MODEL_FILES="$WORK/oversize/big.gguf" \
    ESP_HOOK="$(hook too-big size 3221225472)"
require too-big "\[slmos\] model big.gguf: too big, skipped"
refuse too-big "Boot model"

# --- no-memory ---
# The kernel's QEMU_VIRT PMM is fixed at 1 GiB (RAM_SIZE) and faults on
# its first free-list walk under less, so this case can't reach a prompt
# for a reason unrelated to the stub. What it proves is the stub's part,
# which real firmware has to be short of memory to show: it gave up on
# the file, called ExitBootServices, and the kernel started.
CASE_TIMEOUT=30 CASE_EXPECT="Boot successful" run_case no-memory QEMU_MEM=512M \
    MODEL_FILES="$WORK/oversize/big.gguf" \
    ESP_HOOK="$(hook no-memory size 524288000)"
require no-memory "\[slmos\] model big.gguf: no memory, skipped"
require no-memory "\[slmos\] C calling ExitBootServices"
refuse no-memory "Boot model"

echo "test-qemu-uefi-models: PASS (good, no-file, read-error, too-big, no-memory)"
