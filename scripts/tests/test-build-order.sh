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
# Two targets that use the build directory must also wait for the kernel: the SD test
# image, which a wipe would delete, and the x86-64 GRUB ISO, which packages the ELF.
#
# A stand-in for cmake stops each run at the compile step. It needs the cross compilers
# and cargo, and it compiles no kernel code. It uses the real runtime/target: it deletes
# each platform's libslm_runtime.a and leaves one built with AI_SCHED=ON, so it must
# not overlap another build in the same checkout.
set -euo pipefail

cd "$(dirname "$0")/../.."
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

REAL_CARGO="$(command -v cargo)" || { echo "FAIL: cargo not found on PATH"; exit 1; }
REAL_CMAKE="$(command -v cmake)" || { echo "FAIL: cmake not found on PATH"; exit 1; }
mkdir "$WORK/bin"
cat > "$WORK/bin/cargo" <<EOF
#!/bin/bash
sleep 3
"$REAL_CARGO" "\$@"
rc=\$?
touch "$WORK/cargo-done"
exit \$rc
EOF
cat > "$WORK/bin/cmake" <<EOF
#!/bin/bash
if [[ "\$1" == --build ]]; then
    [[ -f "\$2/Makefile" ]] && echo "BUILD_STEP_CONFIGURED"
    exit 1   # Stop here: kernels aren't compiled, so later steps can't run.
fi
# Checked here, not by when cmake reaches the library, so a slow host can't hide it.
[[ -e "$WORK/cargo-done" ]] || echo "CONFIGURED_BEFORE_RUST"
exec "$REAL_CMAKE" "\$@"
EOF
chmod +x "$WORK/bin/cargo" "$WORK/bin/cmake"

fail() {
    echo "FAIL: $1"
    fails=$((fails + 1))
}

# Run in a minimal environment. Under `make test-build-order AI_SCHED=ON`, the parent make
# exports its command-line variables and MAKEFLAGS to this script, so the first run
# would already have the flag and the flag change below would do nothing. Under
# BUILD_TYPE=Debug it would build a library other than the one this script deletes.
# The run stops at the compile step, so make's own exit status is always a failure.
run() {  # run <log> <make arguments...>
    local log="$1"; shift
    rm -f "$WORK/cargo-done"
    env -i PATH="$WORK/bin:$PATH" HOME="$HOME" \
        ${RUSTUP_HOME:+RUSTUP_HOME="$RUSTUP_HOME"} ${CARGO_HOME:+CARGO_HOME="$CARGO_HOME"} \
        make -j8 "BUILD_DIR=$WORK/b" "CMAKE=$WORK/bin/cmake" "$@" > "$log" 2>&1 || true
}

check() {  # check <platform> <rust target> <make target>
    local platform="$1" target="$2" goal="$3" name="$1 $3"
    local log="$WORK/$1-$3.log"
    rm -f "runtime/target/$target/release/libslm_runtime.a"

    run "$log" "PLATFORM=$platform" "$goal"
    if grep -q "CONFIGURED_BEFORE_RUST" "$log"; then
        fail "$name: configured before the Rust build finished"
    elif ! grep -q "BUILD_STEP_CONFIGURED" "$log"; then
        fail "$name: didn't reach the compile step configured"; tail -5 "$log"
    elif ! grep -q "Rust lib:   .*libslm_runtime.a" "$log"; then
        fail "$name: configured without the Rust library"; grep "Rust lib" "$log" || true
    else
        echo "PASS: $name: configures after the Rust build"
    fi

    run "$log" "PLATFORM=$platform" "$goal"
    if grep -q "reconfiguring" "$log"; then
        fail "$name: reconfigured with unchanged flags"
    elif ! grep -q "BUILD_STEP_CONFIGURED" "$log"; then
        fail "$name: an unchanged rebuild didn't reach the compile step"; tail -5 "$log"
    elif grep -q "is up to date" "$log"; then
        fail "$name: an unchanged rebuild printed make's up-to-date message"
    else
        echo "PASS: $name: an unchanged rebuild stays configured and quiet"
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

# The stand-in cmake fails the kernel build, so a target that waits for it never runs.
check_waits() {  # check_waits <name> <platform> <goal> <text only the target prints>
    local name="$1" platform="$2" goal="$3" text="$4" log="$WORK/waits.log"
    run "$log" "PLATFORM=$platform" "$goal"
    if ! grep -q "BUILD_STEP_CONFIGURED" "$log"; then
        fail "$name: didn't start the kernel build"; tail -5 "$log"
    elif grep -q "$text" "$log"; then
        fail "$name: ran without waiting for the kernel"
    else
        echo "PASS: $name: waits for the kernel"
    fi
    rm -rf "$WORK/b"
}

check QEMU_VIRT aarch64-unknown-none kernel
check QEMU_VIRT aarch64-unknown-none kernel-test
check X86_64 x86_64-unknown-none kernel-kexec
check X86_64 x86_64-unknown-none kernel-bzimage
check_waits "SD test image" QEMU_VIRT "$WORK/b/kernel-test/sdhci-test.img" "sparse"
check_waits "GRUB ISO" X86_64 grub-iso "cannot stat"

if [[ "$fails" != 0 ]]; then
    echo "$fails check(s) failed"
    exit 1
fi
echo "All build-order tests passed"
