#!/bin/bash
# Apply the hailo-stub-stub patch to a freshly extracted QEMU source tree.
#
# Usage: ./apply.sh <qemu-source-dir>
#
# Idempotent: re-running on an already-patched tree is a no-op.

set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <qemu-source-dir>" >&2
    exit 2
fi

QEMU_SRC="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -f "${QEMU_SRC}/hw/misc/edu.c" ]]; then
    echo "ERROR: ${QEMU_SRC}/hw/misc/edu.c not found — does not look like a QEMU source tree." >&2
    exit 3
fi

# 1. Copy the device source into hw/misc/.
cp -v "${HERE}/hw/misc/hailo-stub-stub.c" "${QEMU_SRC}/hw/misc/hailo-stub-stub.c"

# 2. Hook it into the build system. hw/misc/meson.build registers device
#    sources against a CONFIG_PCI condition that's true for x86_64-softmmu —
#    add ours next to edu.c the same way.
MESON="${QEMU_SRC}/hw/misc/meson.build"
if grep -q "hailo-stub-stub.c" "${MESON}"; then
    echo "meson.build already references hailo-stub-stub.c — skipping injection."
else
    # Find the line that adds edu.c and add hailo-stub-stub.c right after.
    if grep -q "edu.c" "${MESON}"; then
        sed -i "/files('edu.c')/a system_ss.add(when: 'CONFIG_PCI', if_true: files('hailo-stub-stub.c'))" "${MESON}"
    else
        echo "system_ss.add(when: 'CONFIG_PCI', if_true: files('hailo-stub-stub.c'))" >> "${MESON}"
    fi
    echo "Injected hailo-stub-stub.c entry into ${MESON}"
fi

echo "Patch applied to ${QEMU_SRC}"
