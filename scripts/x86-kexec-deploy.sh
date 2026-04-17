#!/usr/bin/env bash
# x86-kexec-deploy.sh — run on the DEV HOST (this machine).
#
# Copies a freshly-built slmos.elf (and the sec2_peek helper) to test-pc,
# then — unless --no-exec is passed — triggers the kexec via SSH.
#
# The bare-metal UEFI+SDWire deploy path is unchanged; this is an
# additional option for sessions where we want SEC2 to be
# nouveau-unlocked before SLM-OS boots.
#
# Usage:
#   scripts/x86-kexec-deploy.sh [--no-exec] [--host <ip>] [--elf <path>]
#
# Defaults:
#   host = root@192.168.4.136
#   elf  = $CMAKE_BUILD_DIR/kernel/slmos.elf (from $KERNEL_BUILD_DIR env
#          or discovered via build/kernel/ / build-x86/ / build/)
set -euo pipefail

HOST=root@192.168.4.136
ELF=""
DO_EXEC=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-exec) DO_EXEC=0; shift ;;
        --host) HOST="$2"; shift 2 ;;
        --elf)  ELF="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

# Locate the ELF.
if [[ -z "$ELF" ]]; then
    for candidate in \
        "${KERNEL_BUILD_DIR:-}/slmos.elf" \
        build/kernel/slmos.elf \
        build-x86/kernel/slmos.elf \
        build/kernel-x86_64/slmos.elf; do
        if [[ -n "$candidate" && -r "$candidate" ]]; then
            ELF="$candidate"; break
        fi
    done
fi
[[ -n "$ELF" && -r "$ELF" ]] \
    || { echo "ELF not found — build first: make kernel PLATFORM=X86_64" >&2; exit 1; }

SIZE=$(stat -c '%s' "$ELF")
echo "[deploy] ELF: $ELF ($SIZE bytes)"
echo "[deploy] host: $HOST"

# Copy the ELF + helper scripts.
scp -o ConnectTimeout=5 "$ELF" "$HOST:/root/slmos.elf"
scp -o ConnectTimeout=5 \
    "$(dirname "$0")/x86-kexec-slmos.sh" \
    "$HOST:/root/x86-kexec-slmos.sh"

# sec2_peek is built in scripts/x86-gpu-trace/ — compile+copy if the
# peek binary is missing on the host.
if ! ssh -o ConnectTimeout=5 "$HOST" 'test -x /root/sec2_peek' 2>/dev/null; then
    echo "[deploy] sec2_peek missing on test-pc — copying source + building"
    scp -o ConnectTimeout=5 \
        "$(dirname "$0")/x86-gpu-trace/sec2_peek.c" \
        "$HOST:/root/sec2_peek.c"
    ssh -o ConnectTimeout=5 "$HOST" \
        'cd /root && gcc -O2 -Wall -o sec2_peek sec2_peek.c'
fi

ssh -o ConnectTimeout=5 "$HOST" \
    'chmod +x /root/x86-kexec-slmos.sh; ls -lh /root/slmos.elf /root/x86-kexec-slmos.sh /root/sec2_peek'

if [[ "$DO_EXEC" != "1" ]]; then
    echo ""
    echo "[deploy] --no-exec: staged only. To fire:"
    echo "    ssh $HOST /root/x86-kexec-slmos.sh"
    exit 0
fi

echo ""
echo "[deploy] invoking kexec on $HOST — SSH will drop when it fires"
echo ""

# The script kexec's away → SSH connection dies. Don't treat that as
# an error; catch the specific exit codes.
set +e
ssh -o ConnectTimeout=5 -o ServerAliveInterval=2 "$HOST" \
    '/root/x86-kexec-slmos.sh'
RC=$?
set -e

case $RC in
    0|255) echo "[deploy] SSH dropped — likely kexec fired. Check serial console." ;;
    1) echo "[deploy] precondition failed (rc=1). See output above." ; exit 1 ;;
    2) echo "[deploy] kexec failed (rc=2). See output above." ; exit 2 ;;
    *) echo "[deploy] unexpected rc=$RC" ; exit $RC ;;
esac
