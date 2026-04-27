#!/usr/bin/env bash
#
# scripts/check-deploy-pi5-configs.sh — sanity-check the canonical
# Pi 5 boot-partition configs at deploy/pi5/{config.txt,tryboot.txt}
# against the spec at docs/pi5-baremetal-status.md §Configuration.
#
# Catches drift if someone edits one of the files and silently drops
# a required key, swaps `kernel=` between the two by mistake, or
# adds the `[tryboot]` filter section that misparses on Pi 5
# firmware (see deploy/pi5/README.md).
#
# Run from repo root:
#
#     ./scripts/check-deploy-pi5-configs.sh
#
# Or via the make target:
#
#     make check-deploy-configs
#
# Exits 0 on pass, 1 on first failure.

set -euo pipefail

CONFIG="deploy/pi5/config.txt"
TRYBOOT="deploy/pi5/tryboot.txt"

fail=0

note() { printf '  %s\n' "$*"; }

err() {
    printf '\033[31mFAIL\033[0m %s\n' "$*" >&2
    fail=1
}

ok() {
    printf '\033[32mOK\033[0m   %s\n' "$*"
}

# Check that a file contains a literal key=value line (allowing
# leading whitespace; rejecting matches inside comments).
contains_kv() {
    local path="$1" key="$2" expected_value="$3"
    # Strip comments, then look for "key=expected_value" as a whole
    # token at start-of-line (after optional whitespace).
    if grep -E "^[[:space:]]*${key}=${expected_value}([[:space:]]|$)" "$path" \
            | grep -v '^[[:space:]]*#' >/dev/null; then
        return 0
    else
        return 1
    fi
}

# Pull "key=value" assignments out of a file, ignoring comments and
# filter-section headers (which is what would happen if `[tryboot]`
# ever leaked in — we'd rather fail-loud than silently treat the
# section header as a value).
extract_kvs() {
    local path="$1"
    # Drop comments (anything after #) and blank lines, drop section
    # headers (lines starting with `[`), normalize.
    sed -e 's/#.*$//' -e '/^[[:space:]]*$/d' -e '/^[[:space:]]*\[/d' "$path" \
        | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

if [ ! -f "$CONFIG" ] || [ ! -f "$TRYBOOT" ]; then
    err "deploy/pi5/{config.txt,tryboot.txt} not found — run from repo root"
    exit 1
fi

echo "== Required keys in $CONFIG =="
# Per docs/pi5-baremetal-status.md §Configuration — required keys.
for kv in \
    "arm_64bit=1" \
    "kernel_address=0x80000" \
    "kernel=kernel_2712.img" \
    "pciex4_reset=0" \
    "uart_2ndstage=1" \
    "os_check=0"; do
    key="${kv%=*}"
    val="${kv#*=}"
    if contains_kv "$CONFIG" "$key" "$val"; then
        ok "$kv"
    else
        err "$CONFIG: missing required \`$kv\`"
    fi
done

echo
echo "== Required keys in $TRYBOOT =="
# tryboot.txt must contain everything config.txt does EXCEPT the
# kernel filename, which must be tryboot.img.
for kv in \
    "arm_64bit=1" \
    "kernel_address=0x80000" \
    "kernel=tryboot.img" \
    "pciex4_reset=0" \
    "uart_2ndstage=1" \
    "os_check=0"; do
    key="${kv%=*}"
    val="${kv#*=}"
    if contains_kv "$TRYBOOT" "$key" "$val"; then
        ok "$kv"
    else
        err "$TRYBOOT: missing required \`$kv\`"
    fi
done

echo
echo "== Forbidden patterns =="
# `[tryboot]` filter section in config.txt confuses Pi 5 firmware
# (observed on pieeprom-2024-09-23.bin). The mechanism is the
# separate tryboot.txt file. Same goes for tryboot.txt — a
# `[tryboot]` filter inside it would mean infinite recursion in
# spirit and nonsense in practice.
for path in "$CONFIG" "$TRYBOOT"; do
    if grep -E '^[[:space:]]*\[tryboot\]' "$path" >/dev/null; then
        err "$path: contains a [tryboot] filter section (Pi 5 mechanism is the separate tryboot.txt file; remove)"
    else
        ok "$path: no [tryboot] filter section"
    fi
done

echo
echo "== tryboot.txt mirrors config.txt's settings =="
# Pull the (key, value) pairs from each file, drop the kernel= line
# (which differs by design), and confirm the residue is identical.
config_kvs=$(extract_kvs "$CONFIG"   | grep -v '^kernel=' | sort)
tryboot_kvs=$(extract_kvs "$TRYBOOT" | grep -v '^kernel=' | sort)

if [ "$config_kvs" = "$tryboot_kvs" ]; then
    ok "non-kernel settings match between config.txt and tryboot.txt"
else
    err "non-kernel settings differ between config.txt and tryboot.txt"
    note "diff (< only-in-config / > only-in-tryboot):"
    diff <(printf '%s\n' "$config_kvs") <(printf '%s\n' "$tryboot_kvs") | sed 's/^/    /' || true
fi

# Confirm each file picks the right kernel filename.
echo
echo "== kernel= filename per file =="
if contains_kv "$CONFIG" "kernel" "kernel_2712.img"; then
    ok "$CONFIG: kernel=kernel_2712.img"
else
    err "$CONFIG: must select kernel_2712.img"
fi
if contains_kv "$TRYBOOT" "kernel" "tryboot.img"; then
    ok "$TRYBOOT: kernel=tryboot.img"
else
    err "$TRYBOOT: must select tryboot.img"
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "deploy/pi5/ config check: FAILED" >&2
    exit 1
fi
echo "deploy/pi5/ config check: OK"
