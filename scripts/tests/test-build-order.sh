#!/bin/bash
#
# test-build-order.sh - each kernel must configure after the Rust build and the
# config check, under `make -j` (#1144).
#
# CMake decides at configure time whether to link libslm_runtime.a. Configuring beside
# the Rust build on a tree without the library links the C stubs and fails with
# hundreds of undefined references. A delayed cargo makes that order deterministic.
# Separately, the config check wipes the build directory when flags change, and the
# kernel must then be configured again.
#
# A stand-in for cmake stops each run at the compile step. It needs the cross compilers
# and cargo, and it compiles no kernel code.
set -euo pipefail

cd "$(dirname "$0")/../.."
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

REAL_CARGO="$(command -v cargo)"
REAL_CMAKE="$(command -v cmake)"
mkdir "$WORK/bin"
cat > "$WORK/bin/cargo" <<EOF
#!/bin/bash
sleep 3
exec "$REAL_CARGO" "\$@"
EOF
cat > "$WORK/bin/cmake" <<EOF
#!/bin/bash
if [[ "\$1" == --build ]]; then
    [[ -f "\$2/Makefile" ]] && echo "BUILD_STEP_CONFIGURED"
    exit 1   # Stop here: kernels aren't compiled, so later steps can't run.
fi
exec "$REAL_CMAKE" "\$@"
EOF
chmod +x "$WORK/bin/cargo" "$WORK/bin/cmake"

fail() {
    echo "FAIL: $1"
    fails=$((fails + 1))
}

# The run stops at the compile step, so make's own exit status is always a failure.
run() {  # run <log> <make arguments...>
    local log="$1"; shift
    PATH="$WORK/bin:$PATH" make -j8 "BUILD_DIR=$WORK/b" "CMAKE=$WORK/bin/cmake" "$@" \
        > "$log" 2>&1 || true
}

check() {  # check <platform> <rust target> <make target>
    local platform="$1" target="$2" goal="$3" name="$1 $3"
    local log="$WORK/$1-$3.log"
    rm -f "runtime/target/$target/release/libslm_runtime.a"

    run "$log" "PLATFORM=$platform" "$goal"
    if ! grep -q "BUILD_STEP_CONFIGURED" "$log"; then
        fail "$name: didn't reach the compile step configured"; tail -5 "$log"
    elif ! grep -q "Rust lib:   .*libslm_runtime.a" "$log"; then
        fail "$name: configured without the Rust library"; grep "Rust lib" "$log" || true
    else
        echo "PASS: $name: configures after the Rust build"
    fi

    run "$log" "PLATFORM=$platform" "AI_SCHED=ON" "$goal"
    if ! grep -q "reconfiguring" "$log"; then
        fail "$name: changing flags didn't trigger the config check"; tail -5 "$log"
    elif ! grep -q "BUILD_STEP_CONFIGURED" "$log"; then
        fail "$name: not configured again after the config check wiped it"; tail -5 "$log"
    else
        echo "PASS: $name: reconfigures after a flag change"
    fi
    rm -rf "$WORK/b"
}

check QEMU_VIRT aarch64-unknown-none kernel
check QEMU_VIRT aarch64-unknown-none kernel-test
check X86_64 x86_64-unknown-none kernel-kexec
check X86_64 x86_64-unknown-none kernel-bzimage

if [[ "$fails" != 0 ]]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "All build-order tests passed"
