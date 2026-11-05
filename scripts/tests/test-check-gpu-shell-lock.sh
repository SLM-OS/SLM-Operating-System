#!/bin/bash
#
# test-check-gpu-shell-lock.sh - Host-only test for scripts/check-gpu-shell-lock.py.
set -euo pipefail

CHECK="$(cd "$(dirname "$0")/.." && pwd)/check-gpu-shell-lock.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

expect() {  # expect <name> <exit status> <grep pattern or "-">; source on stdin
    local name="$1" want="$2" pattern="$3" got=0 output
    cat > "$WORK/shell.c"
    output="$(python3 "$CHECK" "$WORK/shell.c" 2>&1)" || got=$?
    if [[ "$got" != "$want" ]]; then
        echo "FAIL: $name: exit $got, wanted $want"; echo "$output"
        fails=$((fails + 1))
    elif [[ "$pattern" != "-" ]] && ! grep -qE "$pattern" <<<"$output"; then
        echo "FAIL: $name: no match for '$pattern'"; echo "$output"
        fails=$((fails + 1))
    else
        echo "PASS: $name"
    fi
}

expect "call inside a locked callback passes" 0 - <<'C'
static int run_locked_cb(void *arg)
{
    return slm_oplib_dispatch(arg, 0, 0, 0);
}
int verb(void)
{
    return slm_gpu_call_locked(run_locked_cb, 0);
}
C

for fn in slm_oplib_dispatch slm_oplib_prepare_dispatch ga10b_bringup_launch_kernel \
          ga10b_bringup_read_pipeline_output ga10b_bringup_smoke_test \
          ga10b_bringup_smoke_test_compute ga10b_ce_memcpy ga10b_submit_and_poll; do
    expect "direct call to $fn fails" 1 "$fn called from verb" <<C
int verb(void)
{
    return $fn(0);
}
C
done

expect "callback never passed to the lock fails" 1 "run_locked_cb isn't run" <<'C'
static int run_locked_cb(void *arg)
{
    return slm_oplib_dispatch(arg, 0, 0, 0);
}
int verb(void)
{
    return run_locked_cb(0);
}
C

expect "call in a helper that merely looks locked fails" 1 "called from run_locked" <<'C'
static int run_locked(void *arg)
{
    return ga10b_ce_memcpy(arg, 0, 0, 0);
}
C

expect "comments and strings don't count" 0 - <<'C'
/* slm_oplib_dispatch(b) used to be called here */
int verb(void)
{
    shell_puts("slm_oplib_dispatch(b) failed\r\n"); // ga10b_ce_memcpy(b)
    return 0;
}
C

if [[ "$fails" != 0 ]]; then
    echo "$fails test(s) failed"
    exit 1
fi
echo "All check-gpu-shell-lock tests passed"
