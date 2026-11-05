#!/usr/bin/env bash
#
# scripts/check-oplib-shell-lock.sh - keep shell operator dispatches under the GPU
# dispatch lock (#1111).
#
# The `nvgpu oplib` verbs race other GPU consumers if they call slm_oplib_dispatch
# directly, and nothing at compile or QEMU test time shows it: the race needs real
# hardware with a consumer running. kernel/src/shell_sys.c must call it only from
# oplib_shell_dispatch_cb, which runs under slm_gpu_call_locked.
set -euo pipefail

SHELL_C="${1:-kernel/src/shell_sys.c}"
[[ -f "$SHELL_C" ]] || { echo "FAIL: $SHELL_C not found"; exit 2; }

calls="$(grep -nE '\bslm_oplib_dispatch\(' "$SHELL_C" || true)"
count="$(grep -cE '\bslm_oplib_dispatch\(' "$SHELL_C" || true)"
callback="$(grep -n 'return slm_oplib_dispatch(d->b,' "$SHELL_C" || true)"

if [[ "$count" != 1 || -z "$callback" ]]; then
    echo "FAIL: $SHELL_C must call slm_oplib_dispatch only from" \
         "oplib_shell_dispatch_cb. Use oplib_shell_dispatch_locked. Found:"
    echo "$calls"
    exit 1
fi
if ! grep -q 'slm_gpu_call_locked(oplib_shell_dispatch_cb,' "$SHELL_C"; then
    echo "FAIL: $SHELL_C must run oplib_shell_dispatch_cb under slm_gpu_call_locked"
    exit 1
fi
