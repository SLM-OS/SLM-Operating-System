#!/bin/bash
# Self-test for scripts/check-cpu-cache-maint.py: planted maintenance on
# shared state must fail, the allowed secondary_mmu_* clean must pass.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
chk="$here/scripts/check-cpu-cache-maint.py"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/kernel/sched" "$tmp/kernel/ipc"

expect_pass() {  # expect_pass <what> [args]
    local what=$1; shift
    if ! python3 "$chk" "$@" --root "$tmp" >/dev/null; then
        echo "FAIL: $what was rejected"; exit 1
    fi
}
expect_fail() {
    local what=$1; shift
    if python3 "$chk" "$@" --root "$tmp" >/dev/null; then
        echo "FAIL: $what was not flagged"; exit 1
    fi
}

printf 'void f(void) { cache_clean(&secondary_mmu_ttbr); }\n' > "$tmp/kernel/sched/smp.c"
expect_pass "allowed secondary_mmu_ttbr clean" --partial

printf 'void f(void) { /* cache_clean(&x) in a comment */ }\n' > "$tmp/kernel/sched/sched.c"
expect_pass "a comment" --partial

printf 'void f(struct task *t) { cache_clean(&t->state); }\n' > "$tmp/kernel/sched/sched.c"
expect_fail "cache_clean on task state" --partial

printf 'void f(void) { __asm__ volatile("dc civac, %%0" :: "r"(0)); }\n' > "$tmp/kernel/sched/sched.c"
expect_fail "raw dc civac" --partial

printf 'void f(void) { __asm__ volatile("dsb sy\\n\\tdc civac, %%0" :: "r"(0)); }\n' > "$tmp/kernel/sched/sched.c"
expect_fail "dc civac after another instruction" --partial

rm -f "$tmp/kernel/sched/sched.c"
printf 'void g(struct task *t) { cache_invalidate(&t->pi_priority); }\n' > "$tmp/kernel/ipc/new_ipc.c"
expect_fail "a new file under kernel/ipc" --partial
rm -f "$tmp/kernel/ipc/new_ipc.c"

expect_fail "missing listed files"

echo "PASS: check-cpu-cache-maint self-test"
