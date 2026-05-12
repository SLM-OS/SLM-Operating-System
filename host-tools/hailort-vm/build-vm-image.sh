#!/bin/bash
# Build a reproducible HailoRT capture VM image.
#
# 1. Downloads the Ubuntu 24.04 (noble) cloud image if not cached.
# 2. Verifies the HailoRT .deb packages live where expected (manual pre-stage
#    per README — Hailo does not allow redistribution).
# 3. Builds a cloud-init NoCloud seed ISO bundling user-data + meta-data + .debs
#    + guest scripts.
# 4. Resizes a copy of the cloud image to 8 GB.
# 5. Boots it once under QEMU with the seed ISO attached. cloud-init installs
#    everything, applies determinism, then powers off.
# 6. The customized qcow2 is saved to $VM_IMG_DIR.
#
# Idempotent: re-running with the same inputs returns the cached image unless
# --force is passed. Outputs land in ~/slmos-ref/derivatives/hailort-vm-images/
# so the VM disk stays out of git.

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration (override via env)
# ---------------------------------------------------------------------------
HAILORT_VERSION="${HAILORT_VERSION:-4.23.0}"
UBUNTU_RELEASE="${UBUNTU_RELEASE:-noble}"
UBUNTU_IMG_URL="${UBUNTU_IMG_URL:-https://cloud-images.ubuntu.com/${UBUNTU_RELEASE}/current/${UBUNTU_RELEASE}-server-cloudimg-amd64.img}"
HAILORT_DEB_DIR="${HAILORT_DEB_DIR:-$HOME/Downloads}"
VM_IMG_DIR="${VM_IMG_DIR:-$HOME/slmos-ref/derivatives/hailort-vm-images}"
VM_IMG_NAME="${VM_IMG_NAME:-hailort-vm-${HAILORT_VERSION}-ubuntu-${UBUNTU_RELEASE}.qcow2}"
VM_DISK_SIZE="${VM_DISK_SIZE:-8G}"
BUILD_TIMEOUT="${BUILD_TIMEOUT:-1800}"  # 30 minutes — cloud-init runs apt-update + installs ~50 packages + DKMS-builds hailo_pci; under TCG this is the floor
FORCE_REBUILD=0

# Hailo-side pinned sha256 (driver .deb is arch-independent and stable).
HAILORT_DRIVER_DEB_SHA256="36e308eb492808db9db7c64046e3fdfb3e4e02fdfd67550db678f068869c1fbf"

# The amd64 userspace sha is recorded into a stamp file on first successful
# build and then required to match on subsequent builds. There is no upstream
# canonical sha published for this artifact.
HAILORT_AMD64_SHA_STAMP="${VM_IMG_DIR}/hailort_${HAILORT_VERSION}_amd64.sha256"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE_REBUILD=1; shift;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# //; s/^#//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
HERE="$(cd "$(dirname "$0")" && pwd)"
CLOUD_INIT_DIR="${HERE}/cloud-init"
GUEST_SCRIPTS_DIR="${HERE}/guest-scripts"
WORK_DIR="${VM_IMG_DIR}/.work"
SEED_ISO="${WORK_DIR}/seed.iso"
BASE_IMG="${VM_IMG_DIR}/ubuntu-${UBUNTU_RELEASE}-cloudimg-amd64.qcow2"
VM_IMG="${VM_IMG_DIR}/${VM_IMG_NAME}"

mkdir -p "${VM_IMG_DIR}" "${WORK_DIR}"

# ---------------------------------------------------------------------------
# Step 1: tools
# ---------------------------------------------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1" >&2; exit 2; }; }
need qemu-system-x86_64
need qemu-img
need curl
need sha256sum
if command -v xorriso >/dev/null 2>&1; then
    MKISO=(xorriso -as mkisofs)
elif command -v genisoimage >/dev/null 2>&1; then
    MKISO=(genisoimage)
else
    echo "neither xorriso nor genisoimage installed" >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Step 2: verify HailoRT .debs (manual pre-stage)
# ---------------------------------------------------------------------------
USERSPACE_DEB="${HAILORT_DEB_DIR}/hailort_${HAILORT_VERSION}_amd64.deb"
DRIVER_DEB="${HAILORT_DEB_DIR}/hailort-pcie-driver_${HAILORT_VERSION}_all.deb"

if [[ ! -f "${USERSPACE_DEB}" ]]; then
    cat >&2 <<EOF
HailoRT amd64 userspace .deb not found at:
  ${USERSPACE_DEB}

Download it manually from Hailo's developer portal (login required) and place
it at the path above, or set HAILORT_DEB_DIR=<path>. The file is not
redistributable; do not commit it.

The driver .deb is architecture-independent and the same file used on pi-5-1
(sha256 ${HAILORT_DRIVER_DEB_SHA256}).
EOF
    exit 3
fi

if [[ ! -f "${DRIVER_DEB}" ]]; then
    echo "HailoRT PCIe driver .deb not found at: ${DRIVER_DEB}" >&2
    exit 3
fi

OBSERVED_DRIVER_SHA="$(sha256sum "${DRIVER_DEB}" | awk '{print $1}')"
if [[ "${OBSERVED_DRIVER_SHA}" != "${HAILORT_DRIVER_DEB_SHA256}" ]]; then
    echo "Driver .deb sha256 mismatch:"  >&2
    echo "  expected: ${HAILORT_DRIVER_DEB_SHA256}" >&2
    echo "  observed: ${OBSERVED_DRIVER_SHA}"      >&2
    exit 4
fi

OBSERVED_USERSPACE_SHA="$(sha256sum "${USERSPACE_DEB}" | awk '{print $1}')"
if [[ -f "${HAILORT_AMD64_SHA_STAMP}" ]]; then
    EXPECTED_USERSPACE_SHA="$(cat "${HAILORT_AMD64_SHA_STAMP}")"
    if [[ "${OBSERVED_USERSPACE_SHA}" != "${EXPECTED_USERSPACE_SHA}" ]]; then
        echo "Userspace .deb sha256 changed since last build:" >&2
        echo "  recorded: ${EXPECTED_USERSPACE_SHA}" >&2
        echo "  observed: ${OBSERVED_USERSPACE_SHA}" >&2
        echo "If this is intentional, rm ${HAILORT_AMD64_SHA_STAMP} and rebuild." >&2
        exit 4
    fi
else
    echo "Recording first-seen userspace .deb sha256: ${OBSERVED_USERSPACE_SHA}"
    echo "${OBSERVED_USERSPACE_SHA}" > "${HAILORT_AMD64_SHA_STAMP}"
fi

# ---------------------------------------------------------------------------
# Step 3: short-circuit if already built
# ---------------------------------------------------------------------------
if [[ -f "${VM_IMG}" && "${FORCE_REBUILD}" -eq 0 ]]; then
    echo "VM image already built: ${VM_IMG}"
    echo "Pass --force to rebuild."
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 4: download Ubuntu cloud image
# ---------------------------------------------------------------------------
if [[ ! -f "${BASE_IMG}" ]]; then
    echo "Downloading Ubuntu ${UBUNTU_RELEASE} cloud image..."
    curl -fL --output "${BASE_IMG}.tmp" "${UBUNTU_IMG_URL}"
    mv "${BASE_IMG}.tmp" "${BASE_IMG}"
fi

# ---------------------------------------------------------------------------
# Step 5: build cloud-init seed ISO
# ---------------------------------------------------------------------------
SEED_STAGING="${WORK_DIR}/seed-staging"
rm -rf "${SEED_STAGING}"
mkdir -p "${SEED_STAGING}/stage"

cp "${CLOUD_INIT_DIR}/user-data" "${SEED_STAGING}/user-data"
cp "${CLOUD_INIT_DIR}/meta-data" "${SEED_STAGING}/meta-data"
cp "${USERSPACE_DEB}" "${SEED_STAGING}/stage/hailort_${HAILORT_VERSION}_amd64.deb"
cp "${DRIVER_DEB}"    "${SEED_STAGING}/stage/hailort-pcie-driver_${HAILORT_VERSION}_all.deb"
cp "${GUEST_SCRIPTS_DIR}"/*.sh "${SEED_STAGING}/stage/"

rm -f "${SEED_ISO}"
"${MKISO[@]}" -output "${SEED_ISO}" \
    -volid CIDATA -joliet -rock -quiet \
    "${SEED_STAGING}"

# ---------------------------------------------------------------------------
# Step 6: prepare working qcow2 (copy + resize)
# ---------------------------------------------------------------------------
echo "Preparing working qcow2..."
qemu-img convert -O qcow2 "${BASE_IMG}" "${VM_IMG}.work"
qemu-img resize "${VM_IMG}.work" "${VM_DISK_SIZE}"

# ---------------------------------------------------------------------------
# Step 7: run cloud-init build boot
# ---------------------------------------------------------------------------
echo "Running cloud-init build boot (timeout ${BUILD_TIMEOUT}s)..."

ACCEL_ARGS=(-accel tcg,thread=single)
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL_ARGS=(-accel kvm -cpu host)
else
    echo "WARNING: /dev/kvm not accessible; falling back to TCG (slow). Add the invoking user to the 'kvm' group to speed this up." >&2
fi

# Per-invocation log path so re-runs don't overwrite forensic evidence.
# A `latest` symlink always points at the most recent run for convenience.
BUILD_LOG_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
BUILD_LOG="${WORK_DIR}/build-boot-${BUILD_LOG_STAMP}.log"
ln -sfn "$(basename "${BUILD_LOG}")" "${WORK_DIR}/build-boot.log"
set +e
timeout "${BUILD_TIMEOUT}" qemu-system-x86_64 \
    "${ACCEL_ARGS[@]}" \
    -smp 2 \
    -m 2G \
    -drive "file=${VM_IMG}.work,format=qcow2,if=virtio" \
    -drive "file=${SEED_ISO},format=raw,if=virtio,readonly=on" \
    -nic user \
    -nographic \
    -serial "file:${BUILD_LOG}" \
    -monitor none \
    -no-reboot
QEMU_STATUS=$?
set -e

if [[ ${QEMU_STATUS} -eq 124 ]]; then
    echo "Build boot timed out after ${BUILD_TIMEOUT}s. cloud-init never reached poweroff. Tail of build log:" >&2
    tail -50 "${BUILD_LOG}" >&2 || true
    rm -f "${VM_IMG}.work"
    exit 5
fi
if [[ ${QEMU_STATUS} -ne 0 ]]; then
    echo "QEMU exited with status ${QEMU_STATUS}. Tail of build log:" >&2
    tail -50 "${BUILD_LOG}" >&2 || true
    rm -f "${VM_IMG}.work"
    exit 5
fi

# ---------------------------------------------------------------------------
# Step 8: verify build success
# ---------------------------------------------------------------------------
# cloud-init shutdown is the success signal; the guest writes
# /opt/hailort-stage/build-complete.stamp before calling shutdown -h.
# If the Power-Off target was reached, treat the build as successful.
# The stamp itself is verified at first capture (lives inside the qcow2).
if ! grep -qE 'reboot: Power down|Reached target .*Power-Off|systemd-poweroff.service' "${BUILD_LOG}"; then
    echo "cloud-init did not reach Power-Off cleanly. Last 80 lines of build log:" >&2
    tail -80 "${BUILD_LOG}" >&2 || true
    rm -f "${VM_IMG}.work"
    exit 5
fi

mv "${VM_IMG}.work" "${VM_IMG}"
echo
echo "VM image built: ${VM_IMG}"
echo "Build log:      ${BUILD_LOG}"
