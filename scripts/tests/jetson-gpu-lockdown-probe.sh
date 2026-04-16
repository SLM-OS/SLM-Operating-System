#!/bin/bash
#
# jetson-gpu-lockdown-probe.sh — reproducible probe for GSP RISCV
# priv-lockdown state across the Linux → SLM-OS kexec transition.
#
# Issue #190 blocker: after kexec-from-Linux on Jetson Orin Nano,
# NV_PGSP_HWCFG2 (BAR0 + 0x1100f4) has bit 13 (RISCV_BR_PRIV_LOCKDOWN)
# asserted, which silently drops our EL2-NS PIO writes to GSP IMEM/DMEM.
# A 2026-04-16 fast-test confirmed bit 13 is 0 in live Linux with nvgpu
# running, so the lockdown is software-induced during the kexec
# transition. This script makes that measurement reproducible.
#
# Flow:
#   Phase A: From live Linux (via SSH), read HWCFG2 via /dev/mem
#   Phase B: kexec into SLM-OS via the standard slmos-kexec helper
#   Phase C: From the SLM-OS shell (via labctl serial), `gpu read 1100f4`
#   Phase D: Decode bit 13 on both sides and report the delta
#
# Requires: `labctl` CLI, SSH access to jetson-nano-2 (root/slmos),
# and a built `slmos.elf` already present on the Jetson at /root/slmos.elf
# (use `scp build/kernel/slmos.elf root@192.168.4.93:/root/` to deploy).
#
# Usage:
#   scripts/tests/jetson-gpu-lockdown-probe.sh [--skip-deploy] [--kernel PATH]
#
# Exit codes:
#   0 — probe completed (report whatever state was observed)
#   1 — infrastructure failure (SSH / labctl / kexec)
#   2 — SLM-OS prompt never appeared (kexec hung or crashed)
set -euo pipefail

JETSON_IP="${JETSON_IP:-192.168.4.93}"
JETSON_USER="${JETSON_USER:-root}"
JETSON_PORT="${JETSON_PORT:-jetson-nano-2}"
JETSON_KERNEL="${JETSON_KERNEL:-/root/slmos.elf}"

# HWCFG2 BAR0 offset for the GSP Falcon (base 0x00110000 + 0xf4).
# Physical address when BAR0 is at 0x17000000 on Jetson.
HWCFG2_OFFSET=0x1100f4
HWCFG2_PHYS=0x171100f4

SKIP_DEPLOY=0
LOCAL_KERNEL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-deploy) SKIP_DEPLOY=1; shift ;;
        --kernel)      LOCAL_KERNEL="$2"; shift 2 ;;
        --help)
            sed -n '2,35p' "$0" | sed 's|^# \?||'
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

echo "=== Jetson GSP priv-lockdown probe ==="
echo "Target:  ${JETSON_USER}@${JETSON_IP}  serial=${JETSON_PORT}"
echo "Kernel:  ${JETSON_KERNEL} on Jetson"
echo

# ---- Phase A: live-Linux HWCFG2 baseline --------------------------------
echo "[A] Reading HWCFG2 (${HWCFG2_PHYS}) from live Linux via /dev/mem..."
LINUX_HWCFG2="$(ssh -o StrictHostKeyChecking=no "${JETSON_USER}@${JETSON_IP}" \
    "python3 -c '
import mmap, os
fd = os.open(\"/dev/mem\", os.O_RDONLY)
buf = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x17110000)
val = int.from_bytes(buf[0xf4:0xf8], \"little\")
print(hex(val))
'" 2>&1)" || {
    echo "  SSH / /dev/mem read failed: $LINUX_HWCFG2"
    echo "  (nvgpu must be live and CONFIG_STRICT_DEVMEM unset)"
    exit 1
}
echo "    Linux HWCFG2 = ${LINUX_HWCFG2}"
LINUX_BIT13=$(( (${LINUX_HWCFG2} >> 13) & 1 ))
echo "    bit 13 (RISCV_BR_PRIV_LOCKDOWN) = ${LINUX_BIT13}"
echo

# ---- Phase B: deploy + kexec --------------------------------------------
if [[ "$SKIP_DEPLOY" == "0" ]]; then
    if [[ -n "$LOCAL_KERNEL" ]]; then
        echo "[B] scp ${LOCAL_KERNEL} → ${JETSON_IP}:${JETSON_KERNEL}"
        scp -o StrictHostKeyChecking=no "$LOCAL_KERNEL" \
            "${JETSON_USER}@${JETSON_IP}:${JETSON_KERNEL}" >/dev/null
    fi
fi

echo "[B] Triggering slmos-kexec (background, SSH will drop)..."
# nohup + detach: kexec tears down sshd so the connection always dies.
# Capture what the helper prints before the tear-down, but don't block on it.
ssh -o StrictHostKeyChecking=no "${JETSON_USER}@${JETSON_IP}" \
    "nohup /usr/local/bin/slmos-kexec ${JETSON_KERNEL} > /tmp/slmos-kexec.log 2>&1 &" \
    || true
echo "    (connection drop is expected — moving to serial capture)"
echo

# ---- Phase C: SLM-OS shell ---------------------------------------------
echo "[C] Waiting for 'slmos>' prompt on serial (up to 60s)..."
if ! labctl serial capture "${JETSON_PORT}" --timeout 60 --until 'slmos>' --tail 5 \
     > /tmp/slmos-prompt.log 2>&1; then
    echo "  SLM-OS prompt never appeared. Tail of capture:"
    tail -20 /tmp/slmos-prompt.log || true
    exit 2
fi
cat /tmp/slmos-prompt.log
echo

echo "[C] Issuing 'gpu read ${HWCFG2_OFFSET}' from SLM-OS shell..."
labctl serial send "${JETSON_PORT}" --text "gpu read ${HWCFG2_OFFSET#0x}" \
    --newline >/dev/null
# Capture the response line. The shell prints "GPU[0xNNNNNN] = 0xNNNNNNNN".
labctl serial capture "${JETSON_PORT}" --timeout 10 \
    --until 'GPU\[' --tail 5 > /tmp/slmos-hwcfg2.log 2>&1 || true
cat /tmp/slmos-hwcfg2.log

SLMOS_HWCFG2="$(grep -oE 'GPU\[[^]]+\] = 0x[0-9a-fA-F]+' /tmp/slmos-hwcfg2.log \
    | tail -1 | awk '{print $3}')"
if [[ -z "$SLMOS_HWCFG2" ]]; then
    echo "  Could not parse HWCFG2 from SLM-OS shell output."
    exit 2
fi
SLMOS_BIT13=$(( (${SLMOS_HWCFG2} >> 13) & 1 ))
echo "    SLM-OS HWCFG2 = ${SLMOS_HWCFG2}"
echo "    bit 13 (RISCV_BR_PRIV_LOCKDOWN) = ${SLMOS_BIT13}"
echo

# ---- Phase D: diff + verdict --------------------------------------------
echo "=== Verdict ==="
printf "  Linux HWCFG2:  %s  (bit13=%d)\n" "$LINUX_HWCFG2" "$LINUX_BIT13"
printf "  SLM-OS HWCFG2: %s  (bit13=%d)\n" "$SLMOS_HWCFG2" "$SLMOS_BIT13"
echo
if [[ "$LINUX_BIT13" == "0" && "$SLMOS_BIT13" == "0" ]]; then
    echo "  PASS: priv-lockdown stayed clear through kexec."
    echo "        Path 2 / Path 3 unlock work can proceed."
elif [[ "$LINUX_BIT13" == "0" && "$SLMOS_BIT13" == "1" ]]; then
    echo "  BLOCKED: priv-lockdown was SET during the kexec transition."
    echo "           This matches the current #190 diagnosis."
elif [[ "$LINUX_BIT13" == "1" ]]; then
    echo "  UNEXPECTED: priv-lockdown was ALREADY set in Linux."
    echo "              Something upstream already locked the Falcon."
fi
