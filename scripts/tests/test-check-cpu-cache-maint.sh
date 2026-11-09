#!/bin/bash
# Self-test for scripts/check-cpu-cache-maint.py: a planted cache_clean on
# shared state must fail, the allowed secondary_mmu_* clean must pass.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/kernel/sched"

printf 'void f(void) { cache_clean(&secondary_mmu_ttbr); }\n' > "$tmp/kernel/sched/smp.c"
if ! python3 "$here/scripts/check-cpu-cache-maint.py" --root "$tmp" >/dev/null; then
    echo "FAIL: allowed secondary_mmu_ttbr clean was rejected"; exit 1
fi

printf 'void f(void) { /* cache_clean(&x) in a comment is fine */ }\n' > "$tmp/kernel/sched/sched.c"
if ! python3 "$here/scripts/check-cpu-cache-maint.py" --root "$tmp" >/dev/null; then
    echo "FAIL: a comment was flagged"; exit 1
fi

printf 'void f(struct task *t) { cache_clean(&t->state); }\n' > "$tmp/kernel/sched/sched.c"
if python3 "$here/scripts/check-cpu-cache-maint.py" --root "$tmp" >/dev/null; then
    echo "FAIL: cache_clean on task state was not flagged"; exit 1
fi

printf 'void f(void) { __asm__ volatile("dc civac, %%0" :: "r"(0)); }\n' > "$tmp/kernel/sched/sched.c"
if python3 "$here/scripts/check-cpu-cache-maint.py" --root "$tmp" >/dev/null; then
    echo "FAIL: raw dc civac was not flagged"; exit 1
fi

echo "PASS: check-cpu-cache-maint self-test"
