#!/bin/bash
# Fetch upstream QEMU source, apply the hailo-stub-stub patch, and build a
# host-local qemu-system-x86_64 binary. Caches the source tarball and the
# built tree so re-runs are fast.
#
# Output: ~/slmos-ref/derivatives/hailort-vm-qemu/qemu-system-x86_64
#
# launch-capture.sh prefers this binary if present.
#
# This is throwaway: Task 0.2 ships a corpus-driven stub that this script's
# binary will be retired in favour of.

set -euo pipefail

QEMU_VERSION="${QEMU_VERSION:-8.2.10}"
QEMU_URL="${QEMU_URL:-https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz}"
# Final binary destination — can be anywhere (including paths with spaces).
QEMU_OUT_DIR="${QEMU_OUT_DIR:-$HOME/slmos-ref/derivatives/hailort-vm-qemu}"
# Working directory MUST live on a path without spaces. QEMU's configure script
# splits $python on whitespace at line ~955, which breaks when the source/build
# tree resolves through a path containing spaces (e.g. our ~/slmos-ref symlink
# targets "/home/dropbox/Dropbox/Projects/SLM-OS Reference Library/").
QEMU_BUILD_ROOT="${QEMU_BUILD_ROOT:-/var/tmp/qemu-stub-build}"
JOBS="${JOBS:-$(nproc)}"
FORCE_REBUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE_REBUILD=1; shift;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# //; s/^#//'
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH_DIR="${HERE}/qemu-patch"

mkdir -p "${QEMU_OUT_DIR}" "${QEMU_BUILD_ROOT}"

WORK_DIR="${QEMU_BUILD_ROOT}"
SRC_DIR="${WORK_DIR}/qemu-${QEMU_VERSION}"
TARBALL="${WORK_DIR}/qemu-${QEMU_VERSION}.tar.xz"
BUILD_DIR="${WORK_DIR}/build"
OUT_BIN="${QEMU_OUT_DIR}/qemu-system-x86_64"

# Refuse to build through a path containing spaces.
if [[ "${WORK_DIR}" == *" "* ]]; then
    echo "ERROR: build root ${WORK_DIR} contains spaces; QEMU configure cannot handle this." >&2
    echo "       Set QEMU_BUILD_ROOT to a space-free path." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Short-circuit if binary exists
# ---------------------------------------------------------------------------
if [[ -x "${OUT_BIN}" && "${FORCE_REBUILD}" -eq 0 ]]; then
    echo "Custom QEMU already built: ${OUT_BIN}"
    "${OUT_BIN}" --version | head -1
    if "${OUT_BIN}" -device help 2>&1 | grep -q hailo-stub-stub; then
        echo "Confirmed: -device hailo-stub-stub is registered."
        exit 0
    else
        echo "Binary exists but hailo-stub-stub not registered. Rebuilding." >&2
        FORCE_REBUILD=1
    fi
fi

# ---------------------------------------------------------------------------
# Build dependencies (system packages — required for QEMU configure step)
# ---------------------------------------------------------------------------
need_pkg() {
    dpkg -l "$1" 2>/dev/null | grep -q '^ii' && return 0
    echo "Missing apt package: $1 (sudo apt install $1)" >&2
    return 1
}
MISSING=0
for p in ninja-build python3-pip pkg-config libglib2.0-dev libpixman-1-dev libslirp-dev meson; do
    need_pkg "$p" || MISSING=$((MISSING+1))
done
if [[ ${MISSING} -gt 0 ]]; then
    echo "Install the packages above and re-run." >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# Fetch tarball
# ---------------------------------------------------------------------------
if [[ ! -f "${TARBALL}" ]]; then
    echo "Downloading ${QEMU_URL}..."
    curl -fL --output "${TARBALL}.partial" "${QEMU_URL}"
    mv "${TARBALL}.partial" "${TARBALL}"
fi

# ---------------------------------------------------------------------------
# Extract (idempotent)
# ---------------------------------------------------------------------------
if [[ ! -d "${SRC_DIR}" ]]; then
    echo "Extracting ${TARBALL}..."
    tar -xJf "${TARBALL}" -C "${WORK_DIR}"
fi

# ---------------------------------------------------------------------------
# Apply patch
# ---------------------------------------------------------------------------
"${PATCH_DIR}/apply.sh" "${SRC_DIR}"

# ---------------------------------------------------------------------------
# Configure (minimal — just x86_64-softmmu, KVM, slirp, no GUI).
#
# We wipe the build dir unconditionally because QEMU's meson-based configure
# is sensitive to leftover state from prior `configure` runs (especially when
# the apply.sh patch is iterated). Incremental rebuilds are not worth the
# debugging cost; full rebuild is ~3-4 min on a modern host.
# ---------------------------------------------------------------------------
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

"${SRC_DIR}/configure" \
    --target-list=x86_64-softmmu \
    --disable-werror \
    --disable-docs \
    --disable-tools \
    --disable-guest-agent \
    --disable-vnc \
    --disable-gtk \
    --disable-sdl \
    --disable-spice \
    --disable-cocoa \
    --disable-curses \
    --disable-libusb \
    --disable-libdaxctl \
    --disable-snappy \
    --disable-bzip2 \
    --disable-lzfse \
    --disable-rbd \
    --disable-cap-ng \
    --disable-curl \
    --enable-slirp \
    --enable-kvm \
    --enable-trace-backends=log

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
ninja -j "${JOBS}" qemu-system-x86_64

# ---------------------------------------------------------------------------
# Install: copy the binary AND its data files (pc-bios, etc.) so the binary
# can be invoked without -L pointing at the build tree. QEMU looks for data
# in $(dirname $0)/../share/qemu by default if compiled with --datadir; we
# bypass that by always passing -L explicitly in launch-capture.sh.
# ---------------------------------------------------------------------------
cp -f "${BUILD_DIR}/qemu-system-x86_64" "${OUT_BIN}"

PC_BIOS_OUT="${QEMU_OUT_DIR}/pc-bios"
rm -rf "${PC_BIOS_OUT}"
cp -a "${SRC_DIR}/pc-bios" "${PC_BIOS_OUT}"

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
"${OUT_BIN}" --version | head -1
if ! "${OUT_BIN}" -device help 2>&1 | grep -q hailo-stub-stub; then
    echo "ERROR: built QEMU does not register hailo-stub-stub" >&2
    exit 4
fi
if [[ ! -f "${PC_BIOS_OUT}/bios-256k.bin" ]]; then
    echo "ERROR: pc-bios/bios-256k.bin not copied to ${PC_BIOS_OUT}" >&2
    exit 4
fi
echo "OK: ${OUT_BIN} ready (hailo-stub-stub registered)."
echo "    pc-bios at ${PC_BIOS_OUT}"
