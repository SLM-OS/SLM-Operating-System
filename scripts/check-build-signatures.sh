#!/usr/bin/env bash
#
# scripts/check-build-signatures.sh — check that every kernel build
# variant reconfigures when a configure option changes (#1072).
#
# Each build directory (build/kernel, kernel-kexec, kernel-bzimage,
# kernel-test) is wiped and reconfigured when its build signature
# changes. The signatures used to be hand-written lists that drifted
# from the CMake arguments (STAGE25_TRACE_PI5 and the blob paths were
# missing from some), so a changed option could build with a stale
# configuration. The Makefile now derives each signature from the
# variant's full CMake argument list. This script toggles each option
# below and fails if any variant's signature stays the same.
#
#     ./scripts/check-build-signatures.sh      (or: make check-build-signatures)
#
# Exits 0 when every option changes every applicable signature.

set -euo pipefail
cd "$(dirname "$0")/.."

# Run in a minimal environment: under `make test AI_SCHED=ON` the parent
# make exports its command-line variables (and MAKEFLAGS) to this
# script, so the "default" baseline already had AI_SCHED=ON and every
# toggle of it looked like a no-op. PLATFORM is dropped too, so the
# check always runs against the default platform's variants; that is
# enough, since it only asks whether toggling an option changes them.
sigs() {
    env -i PATH="$PATH" HOME="$HOME" \
        make --no-print-directory -s print-build-signatures "$@"
}

base="$(sigs)"
fail=0

# option=value pairs that differ from the defaults. Main-kernel-only
# options are checked against the kernel variant alone.
common=(
    BUILD_TYPE=Debug AI_SCHED=ON HAILO_WIRE_DEBUG=ON
    HAILO_TRACE_PHASE_DEFAULT=1 HAILO_TRACE_MECH_DEFAULT=1
    WORK_STEALING=OFF SECONDARY_PREEMPT=ON STAGE25_TRACE_PI5=ON
    DISABLE_EVICTION=ON EVICTION_MODELS=ON EVICTION_DEFAULT_POLICY=arc
    EMBED_DEMO_SCRIPTS=OFF NET_SSHD=ON NET_SSHD_AUTOSTART=ON
    NET_SSHD_DEMO_ALLOW_ALL=ON EXTRA_KERNEL_CMAKE_ARGS=-DFOO=1
)
main_only=(
    JETSON_EL1_SMOKE=ON HAILO_FW_BLOB=/x/fw.bin SCHEDULER_HEF_BLOB=/x/s.hef
    USER_HEF_BLOB=/x/u.hef OPLIB_BLOB=/x/op.bin MNIST_DIGITS_DIR=/x/mnist
)

check() {  # check <option=value> <variant...>
    local opt="$1"; shift
    local new
    new="$(sigs "$opt")"
    for v in "$@"; do
        local a b
        a="$(grep "^$v=" <<<"$base")"
        b="$(grep "^$v=" <<<"$new")"
        if [ "$a" = "$b" ]; then
            echo "FAIL $opt does not change the $v signature"
            fail=1
        fi
    done
}

for o in "${common[@]}"; do
    check "$o" kernel kernel-kexec kernel-bzimage kernel-test
done
for o in "${main_only[@]}"; do
    check "$o" kernel
done

# Per-platform AI scheduler default: ON for Pi 5 and Jetson (the
# Jetson sched-MLP GPU pipeline runs the ai_mlp weights, #1092), OFF
# for QEMU and x86-64; AI_SCHED=OFF still drops it.
ai_default() {  # ai_default <want ON|OFF> <make args...>
    local want="$1"; shift
    local out got=OFF
    out="$(sigs "$@")"   # a failing make aborts here (set -e), not as "OFF"
    if grep "^kernel=" <<<"$out" | grep -q -- "-DENABLE_AI_SCHEDULER=ON"; then
        got=ON
    fi
    if [ "$got" != "$want" ]; then
        echo "FAIL AI scheduler for '$*' is $got, expected $want"
        fail=1
    fi
}
ai_default ON  PLATFORM=RASPI5
ai_default ON  PLATFORM=JETSON_ORIN_NANO
ai_default OFF PLATFORM=JETSON_ORIN_NANO AI_SCHED=OFF
ai_default OFF PLATFORM=QEMU_VIRT
ai_default OFF PLATFORM=X86_64

if [ "$fail" -eq 0 ]; then
    echo "All $(( ${#common[@]} + ${#main_only[@]} )) build-signature options change their variants' signatures; AI scheduler defaults match"
fi
exit "$fail"
