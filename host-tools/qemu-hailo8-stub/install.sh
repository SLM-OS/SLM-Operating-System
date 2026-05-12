#!/usr/bin/env bash
# install.sh — wire hailo8.c into a checked-out QEMU source tree.
#
# Symlinks the device sources from this directory into hw/misc/ and
# idempotently appends the Kconfig + meson.build entries the QEMU build
# system needs to compile the device.
#
# Usage: ./install.sh <path-to-qemu-source>
#
# Re-run after `git pull` in the QEMU tree — the symlinks survive but the
# Kconfig/meson entries are appended only if not already present.
#
# Anchor: issue #795 Task 0.2.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <path-to-qemu-source>" >&2
    exit 2
fi

QEMU_SRC="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/src"

if [[ ! -d "${QEMU_SRC}/hw/misc" ]]; then
    echo "error: ${QEMU_SRC}/hw/misc not found — is this a QEMU source tree?" >&2
    exit 1
fi

if [[ ! -f "${SRC_DIR}/hailo8.c" ]]; then
    echo "error: ${SRC_DIR}/hailo8.c not found" >&2
    exit 1
fi

echo "==> Symlinking hailo8 sources into ${QEMU_SRC}/hw/misc/"
# Refuse to clobber a regular file at the target path — guards against
# the (unlikely but possible) future case where QEMU upstream ships its
# own hw/misc/hailo8.c and `ln -sf` would silently destroy it.
for f in hailo8.c hailo8_corpus.c hailo8_corpus.h; do
    target="${QEMU_SRC}/hw/misc/${f}"
    if [[ -e "${target}" && ! -L "${target}" ]]; then
        echo "error: ${target} exists and is not a symlink — refusing to overwrite" >&2
        echo "       (delete or move it manually if you intend to replace it)" >&2
        exit 1
    fi
done
ln -sf "${SRC_DIR}/hailo8.c"        "${QEMU_SRC}/hw/misc/hailo8.c"
ln -sf "${SRC_DIR}/hailo8_corpus.c" "${QEMU_SRC}/hw/misc/hailo8_corpus.c"
ln -sf "${SRC_DIR}/hailo8_corpus.h" "${QEMU_SRC}/hw/misc/hailo8_corpus.h"

KCONFIG="${QEMU_SRC}/hw/misc/Kconfig"
if ! grep -q '^config HAILO8$' "${KCONFIG}"; then
    echo "==> Appending CONFIG_HAILO8 to ${KCONFIG}"
    cat >> "${KCONFIG}" <<'EOF'

config HAILO8
    bool
    default y if TEST_DEVICES
    depends on PCI && MSI_NONBROKEN
EOF
else
    echo "==> CONFIG_HAILO8 already present in ${KCONFIG} — skipping"
fi

MESON="${QEMU_SRC}/hw/misc/meson.build"
if ! grep -q "CONFIG_HAILO8" "${MESON}"; then
    echo "==> Appending hailo8 to ${MESON}"
    cat >> "${MESON}" <<'EOF'

system_ss.add(when: 'CONFIG_HAILO8', if_true: files('hailo8.c', 'hailo8_corpus.c'))
EOF
else
    echo "==> hailo8 entry already present in ${MESON} — skipping"
fi

echo "==> Done. To build the device, run:"
echo "      cd ${QEMU_SRC}"
echo "      ./configure --target-list=x86_64-softmmu --enable-debug"
echo "      make -j\$(nproc)"
echo "    Then launch with:"
echo "      ./build/qemu-system-x86_64 ... \\"
echo "        -device hailo8,corpus=/path/to/corpus.jsonl"
