#!/bin/bash
# Capture-time entry point. Invoked once per VM boot by hailort-capture.service.
#
# Sequence:
#   1. Apply runtime determinism knobs that don't survive snapshot/reboot.
#   2. Bring up hailo_pci against the QEMU placeholder via 10-bind-stub.sh
#      (auto-probe on the hailo-stub-stub device; new_id fallback on edu).
#   3. Run `hailortcli fw-control identify` pinned to CPU 0.
#   4. Emit a single boundary marker on the serial console so the host knows
#      capture is complete and QEMU can be shut down.
#   5. Power off.
#
# All stdout/stderr from steps 1-3 lands in /var/log/hailort-capture.log AND
# the serial console. The host's launch-capture.sh greps the console for the
# boundary marker — do not change the marker string without also updating the
# host script.

set -uo pipefail
exec > >(tee -a /var/log/hailort-capture.log) 2>&1

echo "===== HAILORT_CAPTURE_BEGIN $(date -u +%FT%TZ) ====="

# Runtime determinism.
echo 0 > /proc/sys/kernel/randomize_va_space || true
echo 1 > /proc/irq/default_smp_affinity      || true

# Force-bind. Failures here are fatal.
if ! /opt/hailort-stage/10-bind-stub.sh; then
    echo "===== HAILORT_CAPTURE_FAILED bind-stub ====="
    sync
    shutdown -h now
    exit 1
fi

# Sanity-check HailoRT presence.
if ! command -v hailortcli >/dev/null 2>&1; then
    echo "===== HAILORT_CAPTURE_FAILED hailortcli-missing ====="
    sync
    shutdown -h now
    exit 1
fi
hailortcli --version || true

# The real capture: a single IDENTIFY round-trip pinned to CPU 0.
# Exit status is captured but we don't fail the run on non-zero — the trace is
# the deliverable, not the exit code. (Identify is expected to fail against the
# placeholder edu device; what matters is that HailoRT got far enough to issue
# the first BAR read, which is the capture point.)
set +e
taskset -c 0 hailortcli fw-control identify
echo "IDENTIFY_EXIT=$?"
set -e

echo "===== HAILORT_CAPTURE_DONE $(date -u +%FT%TZ) ====="
sync
shutdown -h now
