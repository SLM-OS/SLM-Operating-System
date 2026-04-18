#!/usr/bin/env bash
# x86-kexec-deploy.sh — run on the DEV HOST (this machine).
#
# Copies a freshly-built SLM-OS kexec artefact (and the sec2_peek
# helper) to test-pc, then — unless --no-exec is passed — triggers
# the kexec via SSH.
#
# Two kexec paths, selected via --mode:
#   mb2       (default) — copies slmos.elf to /root/slmos.elf, runs
#              the helper in KEXEC_MODE=mb2 (kexec --type=multiboot2-x86)
#   bzimage            — copies slmos.bzimage to /root/slmos.bzimage,
#              runs the helper in KEXEC_MODE=bzimage (kexec --type=bzImage)
#
# The bare-metal UEFI+SDWire deploy path is unchanged; this is an
# additional option for sessions where we want SEC2 to be
# nouveau-unlocked before SLM-OS boots.
#
# Usage:
#   scripts/x86-kexec-deploy.sh [--no-exec] [--host <ip>]
#                               [--mode {mb2|bzimage}]
#                               [--elf <path> | --bzimage <path>]
#
# Defaults:
#   mode = mb2
#   host = root@192.168.4.136
#   image = auto-discovered from build/kernel-kexec/slmos.elf (mb2)
#           or build/kernel-bzimage/slmos.bzimage (bzimage)
set -euo pipefail

HOST=root@192.168.4.136
ELF=""
BZIMAGE=""
MODE="mb2"
DO_EXEC=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-exec) DO_EXEC=0; shift ;;
        --host)    HOST="$2"; shift 2 ;;
        --elf)     ELF="$2"; shift 2 ;;
        --bzimage) BZIMAGE="$2"; MODE="bzimage"; shift 2 ;;
        --mode)    MODE="$2"; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    mb2|bzimage) ;;
    *) echo "invalid --mode '$MODE' (expected mb2|bzimage)" >&2; exit 2 ;;
esac

# Locate the image.
if [[ "$MODE" == "mb2" ]]; then
    if [[ -z "$ELF" ]]; then
        for candidate in \
            "${KERNEL_BUILD_DIR:-}/slmos.elf" \
            build/kernel-kexec/slmos.elf \
            build/kernel/slmos.elf \
            build-x86/kernel/slmos.elf; do
            if [[ -n "$candidate" && -r "$candidate" ]]; then
                ELF="$candidate"; break
            fi
        done
    fi
    [[ -n "$ELF" && -r "$ELF" ]] \
        || { echo "ELF not found — build first: make kernel-kexec PLATFORM=X86_64" >&2; exit 1; }
    IMAGE_LOCAL="$ELF"
    IMAGE_REMOTE="/root/slmos.elf"
else
    if [[ -z "$BZIMAGE" ]]; then
        DEFAULT_BZIMAGE=build/kernel-bzimage/slmos.bzimage
        [[ -r "$DEFAULT_BZIMAGE" ]] && BZIMAGE="$DEFAULT_BZIMAGE"
    fi
    [[ -n "$BZIMAGE" && -r "$BZIMAGE" ]] \
        || { echo "bzImage not found — build first: make kernel-bzimage PLATFORM=X86_64" >&2; exit 1; }
    IMAGE_LOCAL="$BZIMAGE"
    IMAGE_REMOTE="/root/slmos.bzimage"
fi

SIZE=$(stat -c '%s' "$IMAGE_LOCAL")
echo "[deploy] mode:  $MODE"
echo "[deploy] image: $IMAGE_LOCAL ($SIZE bytes)"
echo "[deploy] host:  $HOST"

# Copy the image + helper script.
scp -o ConnectTimeout=5 "$IMAGE_LOCAL" "$HOST:$IMAGE_REMOTE"
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
    "chmod +x /root/x86-kexec-slmos.sh; ls -lh $IMAGE_REMOTE /root/x86-kexec-slmos.sh /root/sec2_peek"

if [[ "$DO_EXEC" != "1" ]]; then
    echo ""
    echo "[deploy] --no-exec: staged only. To fire:"
    echo "    ssh $HOST KEXEC_MODE=$MODE /root/x86-kexec-slmos.sh"
    exit 0
fi

echo ""
echo "[deploy] invoking kexec on $HOST — SSH will drop when it fires"
echo ""

# The script kexec's away → SSH connection dies. Don't treat that as
# an error; catch the specific exit codes.
set +e
ssh -o ConnectTimeout=5 -o ServerAliveInterval=2 "$HOST" \
    "KEXEC_MODE=$MODE /root/x86-kexec-slmos.sh"
RC=$?
set -e

case $RC in
    0|255) echo "[deploy] SSH dropped — likely kexec fired. Check serial console." ;;
    1) echo "[deploy] precondition failed (rc=1). See output above." ; exit 1 ;;
    2) echo "[deploy] kexec failed (rc=2). See output above." ; exit 2 ;;
    *) echo "[deploy] unexpected rc=$RC" ; exit $RC ;;
esac
