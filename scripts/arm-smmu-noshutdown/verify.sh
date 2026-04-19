#!/bin/bash
#
# verify.sh — functional test for arm_smmu_noshutdown + smmu_probe
#             on a live Jetson Orin Nano running L4T.
#
# Runs the README's four-stage "Verification procedure" as a single
# scripted test: before / apply / after / cleanup. Asserts expected
# dmesg output at each stage and exits non-zero on any mismatch.
#
# This is the functional test for PR #303 (SLM-OS #266 Phase 3A
# Path 1, Option 1). A kernel module that patches another built-in
# driver's struct can't be unit-tested in isolation; hardware
# verification is the only meaningful gate, and the dmesg strings
# asserted below are the test oracle.
#
# Usage:
#   On a Jetson with L4T kernel headers installed:
#     cd scripts/arm-smmu-noshutdown/
#     make && sudo ./verify.sh          # pre-flight (stages 1-3)
#     make && sudo ./verify.sh --full   # also test rmmod cleanup (stages 1-4)
#
#   From a host over SSH:
#     rsync -a scripts/arm-smmu-noshutdown/ root@jetson:/root/arm-smmu-noshutdown/
#     ssh root@jetson 'cd /root/arm-smmu-noshutdown && make && ./verify.sh'
#
# Stages:
#   1  baseline probe (before any module is loaded)
#   2  load arm_smmu_noshutdown and inspect its dmesg output
#   3  probe again, verifying .shutdown is NULL and the identity
#      mapping translates end-to-end via iommu_iova_to_phys
#   4  (--full only) rmmod arm_smmu_noshutdown and verify the exit
#      path correctly calls iommu_unmap
#
# DEFAULT MODE (no --full) leaves arm_smmu_noshutdown loaded on
# success, so the next step ("kexec into SLM-OS") works immediately
# without having to re-insmod. If you run with --full, you MUST
# re-insmod arm_smmu_noshutdown before kexec — otherwise the
# identity mapping is gone and SLM-OS's xHCI NO_OP round-trip will
# time out.
#
# Exit codes:
#   0  — all stages pass (field fix + identity mapping verified)
#   1  — any assertion failed
#   2  — setup error (missing modules, can't run dmesg, etc.)

set -uo pipefail

FULL=0
for arg in "$@"; do
    case "$arg" in
        --full) FULL=1 ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
MOD_NOSHUTDOWN="$SCRIPT_DIR/arm_smmu_noshutdown.ko"
MOD_PROBE="$SCRIPT_DIR/smmu_probe.ko"

RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'
pass() { printf '%s[PASS]%s %s\n'  "$GRN" "$RST" "$*"; }
fail() { printf '%s[FAIL]%s %s\n'  "$RED" "$RST" "$*"; FAILED=1; }
warn() { printf '%s[WARN]%s %s\n'  "$YEL" "$RST" "$*"; }
info() { printf '       %s\n' "$*"; }

FAILED=0
need_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "must run as root (insmod/rmmod needed)" >&2
        exit 2
    fi
}
need_file() {
    if [[ ! -f "$1" ]]; then
        echo "missing $1 — run 'make' first" >&2
        exit 2
    fi
}

# dmesg_grep <regex> <description>
#   Succeeds if regex appears in dmesg since the last dmesg -C (or boot).
dmesg_grep() {
    local re="$1" desc="$2"
    if dmesg | grep -qE "$re"; then
        pass "$desc"
        return 0
    fi
    fail "$desc — expected regex: $re"
    return 1
}

# dmesg_no_match <regex> <description>
dmesg_no_match() {
    local re="$1" desc="$2"
    if dmesg | grep -qE "$re"; then
        fail "$desc — unexpected match on: $re"
        return 1
    fi
    pass "$desc"
    return 0
}

unload_if_loaded() {
    local name="$1"
    if lsmod | awk '{print $1}' | grep -qx "$name"; then
        rmmod "$name" 2>/dev/null || warn "rmmod $name failed"
    fi
}

# ------------------------------------------------------------------ #
# Stage 0 — setup
# ------------------------------------------------------------------ #
need_root
need_file "$MOD_NOSHUTDOWN"
need_file "$MOD_PROBE"

# Clean slate: unload previously-loaded copies of the modules, then
# clear dmesg so our assertions only see fresh output.
unload_if_loaded smmu_probe
unload_if_loaded arm_smmu_noshutdown
dmesg -C >/dev/null

# ------------------------------------------------------------------ #
# Stage 1 — baseline: probe BEFORE arm_smmu_noshutdown
#
#   Two acceptable runtime states here:
#
#     a) `.shutdown = arm_smmu_device_shutdown+0x0/0x40`
#        (observed on L4T 36.4.7; the struct-field fix is load-
#        bearing and the former template was a no-op.)
#     b) `.shutdown = (null)`
#        (observed on L4T 36.4.4, which ships with the pointer
#        already NULL. The field fix is a no-op on this L4T but
#        still documents the correct field for future kernels.)
#
#   The test treats either as a valid baseline but records which
#   one applied so Stage 3's "post-fix" assertion can adjust its
#   expectation: if baseline was already NULL, the fix is not
#   observably changing anything, so Stage 3 asserts NULL-stays-NULL
#   rather than non-NULL→NULL.
# ------------------------------------------------------------------ #
printf '\n-- Stage 1: baseline probe --\n'
insmod "$MOD_PROBE"
sleep 0.1

# Extract the actual `.shutdown = ...` line once so both paths share it.
SHUTDOWN_LINE=$(dmesg | grep -E 'smmu-probe: +\.shutdown =' | tail -1 || true)
if [[ -z "$SHUTDOWN_LINE" ]]; then
    fail 'smmu_probe did not log a .shutdown line — module failed to load?'
elif echo "$SHUTDOWN_LINE" | grep -q 'arm_smmu_device_shutdown'; then
    BASELINE_SHUTDOWN='non-null'
    pass 'baseline: platform_driver.shutdown = arm_smmu_device_shutdown (field-fix relevant)'
elif echo "$SHUTDOWN_LINE" | grep -q '(null)'; then
    BASELINE_SHUTDOWN='null'
    pass 'baseline: platform_driver.shutdown = (null) (L4T already ships with NULL — field-fix is a no-op on this kernel)'
else
    fail "baseline: unexpected .shutdown value: $SHUTDOWN_LINE"
    BASELINE_SHUTDOWN='unknown'
fi

# Record whether an identity mapping already exists from a prior load
if dmesg | grep -q 'IOMMU identity mapping verified across the full 2 MB range'; then
    warn 'IOMMU identity mapping already present from a prior load — '\
'state is preserved across rmmod only if the arm_smmu_noshutdown '\
'exit path ran; expected if a prior session crashed or was SIGKILLed'
fi
rmmod smmu_probe
dmesg -C >/dev/null

# ------------------------------------------------------------------ #
# Stage 2 — apply: load arm_smmu_noshutdown
#   Expects three log lines announcing:
#     - NULLed platform_driver.shutdown
#     - xusb iommu_domain type (= IOMMU_DOMAIN_DMA)
#     - iommu_map success over 0xbde00000..0xbe000000
# ------------------------------------------------------------------ #
printf '\n-- Stage 2: load arm_smmu_noshutdown --\n'
insmod "$MOD_NOSHUTDOWN"
sleep 0.1
# On L4T 36.4.7 the module logs "NULLed arm-smmu platform_driver->shutdown";
# on L4T 36.4.4 (where the pointer is already NULL at boot) it logs
# "platform_driver->shutdown already NULL — no-op". Either is fine — the
# post-condition we care about ('shutdown is NULL') is asserted in Stage 3.
dmesg_grep 'arm-smmu-noshutdown: (NULLed|platform_driver->shutdown already NULL)' \
           'arm_smmu_noshutdown init handled .shutdown (either NULLed or already-NULL)'
dmesg_grep 'arm-smmu-noshutdown: xusb iommu_domain type=[0-9]+' \
           'xusb iommu_domain located'
dmesg_grep 'added identity IOMMU mapping IOVA 0xbde00000\.\.0xbe000000' \
           'iommu_map(0xBDE00000, 0xBDE00000, 2MB) returned 0'
dmesg_no_match 'iommu_map.*failed' \
               'no iommu_map failure logged'
dmesg -C >/dev/null

# ------------------------------------------------------------------ #
# Stage 3 — verify: probe AFTER arm_smmu_noshutdown
#   Expects:
#     .shutdown = (null)
#     iommu_iova_to_phys translates all three sampled IOVAs to
#       themselves ("identity mapping verified across the full 2 MB
#       range")
# ------------------------------------------------------------------ #
printf '\n-- Stage 3: post-fix probe --\n'
insmod "$MOD_PROBE"
sleep 0.1
dmesg_grep 'smmu-probe: +\.shutdown = \(null\)' \
           'platform_driver.shutdown is NULL after fix'
dmesg_grep 'IOMMU identity mapping verified across the full 2 MB range' \
           'iommu_iova_to_phys confirms identity translation for full 2 MB range'
rmmod smmu_probe
dmesg -C >/dev/null

# ------------------------------------------------------------------ #
# Stage 4 — (--full only) cleanup test: rmmod arm_smmu_noshutdown
#
#   Expects:
#     iommu_unmap returns 2097152 (== SLMOS_NC_SIZE)
#     .shutdown stays NULL (load-and-forget model — do NOT restore)
#
#   WARNING: Running this stage leaves the system WITHOUT the
#   identity mapping installed. DO NOT kexec into SLM-OS after a
#   --full run without first re-insmod-ing arm_smmu_noshutdown,
#   or xHCI NO_OP will time out (no identity translation means the
#   HC's DMA reads of SLM-OS's command ring land on an unmapped
#   IOVA and silently drop).
# ------------------------------------------------------------------ #
if [[ "$FULL" -eq 1 ]]; then
    printf '\n-- Stage 4: unload + verify exit-path cleanup (--full) --\n'
    rmmod arm_smmu_noshutdown
    sleep 0.1
    dmesg_grep 'iommu_unmap returned 2097152 bytes \(expected 2097152\)' \
               'exit-path iommu_unmap returns full 2 MB'

    insmod "$MOD_PROBE"
    sleep 0.1
    dmesg_grep 'smmu-probe: +\.shutdown = \(null\)' \
               'platform_driver.shutdown stays NULL after rmmod (load-and-forget)'
    dmesg_grep 'IOMMU identity mapping NOT installed' \
               'iommu_unmap successfully removed the identity range'
    rmmod smmu_probe
fi

# ------------------------------------------------------------------ #
# Summary
# ------------------------------------------------------------------ #
printf '\n-------------------------------------\n'
if [[ "$FAILED" -eq 0 ]]; then
    printf '%sALL STAGES PASSED%s\n' "$GRN" "$RST"
    if [[ "$FULL" -eq 1 ]]; then
        printf 'arm_smmu_noshutdown is NOT loaded (Stage 4 unloaded it).\n'
        printf 'Re-insmod it before kexec:\n'
        printf '    sudo insmod %s\n' "$MOD_NOSHUTDOWN"
    else
        printf 'arm_smmu_noshutdown is loaded; identity mapping is in place.\n'
        printf 'Next step: kexec into SLM-OS and capture "NO_OP round-trip OK"\n'
        printf '           from the xhci init log (sudo slmos-kexec /path/to/slmos.elf).\n'
    fi
    exit 0
else
    printf '%sFAILURES DETECTED%s — see [FAIL] lines above\n' "$RED" "$RST"
    exit 1
fi
