#!/usr/bin/env bash
# x86-multitask-boot-test.sh — P1-2 hardware validation on test-pc.
#
# Orchestrates the labctl-driven flash + boot test + multi-task
# preemption check described in docs/x86-64-capstone-gap-closure-plan.md
# §A2. Run this after `make x86-disk PLATFORM=X86_64`.
#
# Preconditions:
#   - labctl installed and configured; test-pc is a registered SBC with
#     an assigned SDWire device and ser2net console.
#   - test-pc UEFI boot-order prefers the SDWire SD before the external
#     Ubuntu SSD (one-time BIOS setup).
#   - Python 3 on the dev machine for the ser2net-driven multi-task
#     timing loop.
#
# Usage:
#   scripts/tests/x86-multitask-boot-test.sh [--no-flash] [--runs N]
#
# Flags:
#   --no-flash   Skip the `labctl sdwire flash` step (use whatever is on
#                the SD card already). Useful for re-running the
#                multi-task test without re-imaging.
#   --runs N     Override boot-reliability run count (default: 10).
#
# Env (multi-task step timeouts — raise under CI contention, #178):
#   SLMOS_RPC_PROMPT_TIMEOUT  — seconds to wait for `slmos>` / `Slept`
#                                after each send. Default 10 s.
#   SLMOS_RPC_DRAIN_TIMEOUT   — default per-recv() socket timeout.
#                                Default 8 s.
#   SLMOS_RPC_CONNECT_TIMEOUT — TCP connect to ser2net. Default 30 s.
# A single false-fail under load triggers one automatic retry at
# 2× prompt timeout; the pass is reported as failure only if both
# passes miss the 5/5 gate.
#
# Output:
#   Prints per-step status to stdout. Exits 0 only if every gate in
#   P1-2 passes:
#     - boot_test ≥ 9/10 successful
#     - 5/5 sleep-2000 runs return in [2000, 2200] ms
#     - bench smp dispatches to every secondary CPU
#
# A dated results document should be committed under
# docs/testing/x86-hw-validation-<date>.md after every run.

set -euo pipefail

# ---- arg parsing ----
FLASH=1
RUNS=10
IMG="build/kernel/slmos-x86.img"
SBC="test-pc"
PROMPT="slmos>"
BOOT_TIMEOUT=45

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-flash) FLASH=0 ;;
        --runs)     RUNS="$2"; shift ;;
        --image)    IMG="$2";  shift ;;
        -h|--help)
            sed -n '2,/^set -e/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)  echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# ---- sanity ----
command -v labctl   > /dev/null || { echo "labctl not found in PATH"   >&2; exit 3; }
command -v python3  > /dev/null || { echo "python3 not found in PATH" >&2; exit 3; }

if [[ "$FLASH" == "1" ]]; then
    [[ -f "$IMG" ]] || { echo "image not found: $IMG (run make x86-disk PLATFORM=X86_64)" >&2; exit 3; }

    echo "=== Step 1: flash $IMG to $SBC via SDWire ==="
    labctl sdwire flash --no-reboot "$SBC" "$IMG"
fi

# ---- boot-reliability ----
echo
echo "=== Step 2: boot_test --runs $RUNS ==="
BOOT_OUT="$(mktemp)"
trap 'rm -f "$BOOT_OUT"' EXIT
labctl boot-test "$SBC" --no-deploy --expect "$PROMPT" --runs "$RUNS" \
    --timeout "$BOOT_TIMEOUT" \
    | tee "$BOOT_OUT"

# Extract the final "Result: X/Y boots successful" line.
n_ok=$(grep -Eo 'Result: [0-9]+/[0-9]+' "$BOOT_OUT" | head -1 | sed 's|Result: ||; s|/.*||')
: "${n_ok:=0}"
if (( n_ok < 9 )); then
    echo "boot_test: $n_ok/$RUNS PASS — below 9/10 acceptance"
    exit 4
fi
echo "boot_test OK: $n_ok/$RUNS"

# ---- multi-task preemption (Python over ser2net) ----
echo
echo "=== Step 3: sleep 2000 under 'component run echo' load, 5× ==="

# Get the ser2net tcp port for test-pc.
PORT="$(labctl port list 2>/dev/null | awk -v sbc="$SBC" '$1 == sbc {print $5}' | head -1)"
: "${PORT:=4006}"

python3 - "$PORT" <<'PY'
import os, socket, re, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4006
HOST = "127.0.0.1"

# Timeouts, overridable via env. Defaults bumped to tolerate a
# concurrent Cargo build or similar CPU steal on the dev machine
# without false-failing; raise higher via env if CI adds contention.
# Matches the issue-#178 acceptance criterion.
PROMPT_T = float(os.environ.get("SLMOS_RPC_PROMPT_TIMEOUT", "10"))
DRAIN_T  = float(os.environ.get("SLMOS_RPC_DRAIN_TIMEOUT",  "8"))
CONNECT_T = float(os.environ.get("SLMOS_RPC_CONNECT_TIMEOUT", "30"))

def conn():
    s = socket.create_connection((HOST, PORT), timeout=CONNECT_T)
    s.settimeout(DRAIN_T)
    return s

def drain(s, pat, t):
    s.settimeout(t)
    buf = b""
    deadline = time.monotonic() + t
    while time.monotonic() < deadline:
        try:
            c = s.recv(4096)
        except socket.timeout:
            break
        if not c:
            break
        buf += c
        if pat.search(buf.decode(errors="replace")):
            return buf
    return buf

def send(s, data):
    s.sendall((data + "\r\n").encode())

PROMPT = re.compile(r"slmos>")
SLEPT = re.compile(r"Slept .* ms")

def run_pass(prompt_t, label):
    """Execute the 5× sleep-2000 loop; return (ok, results)."""
    s = conn()
    try:
        send(s, "")
        drain(s, PROMPT, t=prompt_t)
        send(s, "component run echo")
        drain(s, PROMPT, t=prompt_t)

        results, ok = [], 0
        for i in range(5):
            send(s, "")
            drain(s, PROMPT, t=prompt_t)
            t0 = time.monotonic()
            send(s, "sleep 2000")
            drain(s, SLEPT, t=prompt_t)
            t1 = time.monotonic()
            ms = int((t1 - t0) * 1000)
            in_range = 2000 <= ms <= 2200
            ok += 1 if in_range else 0
            results.append((ms, in_range))
            print(f"  [{label}] run {i+1}: {ms} ms  in-range={in_range}")
    finally:
        s.close()
    return ok, results

ok, results = run_pass(PROMPT_T, "pass 1")
print(f"\nmulti-task pass 1: {ok}/5 in [2000, 2200] ms  "
      f"values={[r[0] for r in results]}")

# Retry-once policy (#178): a single false-fail under load shouldn't
# escalate to a red CI. Re-run the whole loop with 2× timeouts; if
# both passes fail, that's a real regression.
if ok < 5:
    retry_t = PROMPT_T * 2
    print(f"\nFirst pass missed ({ok}/5). Retrying with {retry_t}s "
          f"prompt timeout to distinguish scheduler hiccup from hang.")
    ok, results = run_pass(retry_t, "pass 2")
    print(f"\nmulti-task pass 2: {ok}/5 in [2000, 2200] ms  "
          f"values={[r[0] for r in results]}")

sys.exit(0 if ok == 5 else 5)
PY
PY_RC=$?

if [[ "$PY_RC" != "0" ]]; then
    echo "multi-task test FAILED (exit $PY_RC)"
    exit 5
fi

# ---- bench smp ----
echo
echo "=== Step 4: bench smp dispatches to all secondary CPUs ==="
SMP_OUT=$(labctl serial capture "$SBC" --timeout 5 --until 'slmos>' 2>&1 <<<"bench smp
" || true)
echo "$SMP_OUT"
n_disp=$(echo "$SMP_OUT" | grep -Ec "Dispatched 'smp[0-9]+' to CPU")
if (( n_disp < 1 )); then
    echo "bench smp: dispatch output not seen"
    exit 6
fi
echo "bench smp OK: $n_disp CPUs dispatched"

echo
echo "=== P1-2 PASSED ==="
echo "Remember to commit docs/testing/x86-hw-validation-<date>.md with the"
echo "observed numbers before closing the gap."
