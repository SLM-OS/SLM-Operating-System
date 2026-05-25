#!/usr/bin/env bash
# capture-hailort-ftrace.sh — capture HailoRT's BAR ops via Linux ftrace.
#
# Runs on a Pi 5 booted into Pi OS (NOT SLM-OS), with the Hailo AI HAT+
# attached and the `hailo_pci` driver loaded. Uses Linux ftrace to log
# every iowrite32 / ioread32 / iowrite16 / ioread16 / iowrite8 / ioread8
# call while a HailoRT workload runs (typically `hailortcli run model.hef`).
#
# Why this exists: the QEMU stub used for the #795 Phase 2 grind doesn't
# model the BAR2 (per-channel VDMA) registers, so the resulting corpus
# has zero BAR2 ops — exactly where the #682 wedge happens. To diff
# against HailoRT's real BAR2 behaviour, we need a capture path that
# bypasses QEMU and instruments the real Linux driver. See the memory
# entry `hailo_re_corpus_bar_coverage` for the structural-limit
# discussion.
#
# Output: a single text file at the path given via --out, formatted as
# raw ftrace dump (`cat /sys/kernel/debug/tracing/trace`). The host-
# side tool `hailo-re-ftrace-translate` consumes this and emits the
# corpus-comparable `{bar, offset, dir, size, value}` JSONL.
#
# Usage:
#   sudo ./scripts/capture-hailort-ftrace.sh \
#       --workload "hailortcli run mnist.hef --input-file input.bin" \
#       --out captured.ftrace
#
# Requirements (Pi OS side):
#   - root (ftrace needs writes to /sys/kernel/debug/tracing)
#   - kernel built with CONFIG_FUNCTION_TRACER + CONFIG_KPROBES (default
#     on RPi OS bullseye/bookworm)
#   - debugfs mounted at /sys/kernel/debug (default; mount on demand if
#     missing)
#   - `hailo_pci` kernel module loaded; `hailortcli` + a .hef on PATH
#
# Exit codes:
#   0  success — capture file written to --out
#   1  bad arguments / preflight failure
#   2  ftrace setup failed (kernel config missing? not root?)
#   4  trace file empty (nothing got captured — check kprobe targets)
#
# Note on workload exit code: the script does NOT exit with the
# workload's rc. Capturing the #682 wedge means HailoRT *will* return
# non-zero, and we want the trace dumped regardless. The workload's rc
# is recorded in the capture file's `# workload_rc:` header line for
# the operator to inspect.

set -euo pipefail

# ---- defaults -------------------------------------------------------------

OUT_FILE=""
WORKLOAD=""
TRACE_DIR=/sys/kernel/debug/tracing
# Tracing iowrite/ioread captures every MMIO across the whole kernel,
# not just hailo_pci. That's fine — the host-side translator filters
# down to BAR0/2/4 addresses by querying /sys/bus/pci/devices/.../resource
# for the Hailo PCIe device. Trying to filter to one driver inside
# ftrace requires kprobes with module-name filters which is brittle
# across kernel versions; the post-filter approach is robust.
TRACE_FUNCS=(
    iowrite8 ioread8
    iowrite16 ioread16
    iowrite32 ioread32
    iowrite64 ioread64
)

# ---- arg parse ------------------------------------------------------------

usage() {
    awk 'NR==1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --workload)   shift; WORKLOAD="$1"; shift ;;
        --out)        shift; OUT_FILE="$1"; shift ;;
        -h|--help)    usage ;;
        *)            echo "error: unknown arg '$1'" >&2; usage ;;
    esac
done

[[ -n "$WORKLOAD" ]] || { echo "error: --workload required" >&2; usage; }
[[ -n "$OUT_FILE" ]] || { echo "error: --out required" >&2; usage; }

# ---- preflight ------------------------------------------------------------

[[ "$EUID" -eq 0 ]] || { echo "error: must run as root (sudo)" >&2; exit 1; }
[[ -d "$TRACE_DIR" ]] || {
    echo "error: ftrace directory $TRACE_DIR not found" >&2
    echo "       (mount debugfs? CONFIG_FUNCTION_TRACER set?)" >&2
    exit 2
}

# Hailo PCIe device must be visible — if it isn't, the capture would
# include all sorts of unrelated MMIO and the post-filter would have
# nothing to match on. Fail fast so the operator doesn't waste a run.
HAILO_DEV=$(lspci -d 1e60: -n 2>/dev/null | awk '{print $1}' | head -1)
[[ -n "$HAILO_DEV" ]] || {
    echo "error: no Hailo PCIe device found (vendor 1e60)" >&2
    echo "       lspci -d 1e60: returns nothing — driver not bound?" >&2
    exit 1
}
echo "==> Hailo PCIe device: $HAILO_DEV" >&2

# Dump the BARs so the translator can map kernel-virtual addresses
# back to (bar, offset). The captured-trace file embeds this as a
# header so it's self-contained — no need to re-query after the fact.
HAILO_PCI_DIR="/sys/bus/pci/devices/0000:${HAILO_DEV}"
[[ -d "$HAILO_PCI_DIR" ]] || {
    echo "error: $HAILO_PCI_DIR missing" >&2; exit 1; }
echo "==> Hailo BARs (will be embedded in output for the translator):" >&2
# /sys/.../resource has exactly 6 BAR rows + I/O + ROM rows on PCIe;
# we print all data rows the awk filter matches. The earlier `>&2 |
# head -7` form looked like a 7-row truncation but actually broke
# the pipe (>&2 redirected stdout to fd2 BEFORE the pipe could read
# from it), so head got starved and printed nothing. Just emit
# everything straight to stderr.
awk '
NR>0 {
    printf "    BAR%d: phys=0x%016x size=0x%x\n",
           NR-1, strtonum("0x"$1), strtonum("0x"$2) - strtonum("0x"$1) + 1
}' "$HAILO_PCI_DIR/resource" >&2

# ---- ftrace setup ---------------------------------------------------------

# We use kprobes (NOT the plain function tracer) because the latter
# only records "function was called" — no args. The host-side
# translator (`hailo_re_driver.ftrace_translate`) needs both the
# value being written/read AND the kernel-virtual address; without
# them every line gets silently dropped.
#
# Kernel ABI for the iowrite/ioread accessors on ARM64:
#   void iowrite32(u32 value, void __iomem *addr);   v=%x0 addr=%x1
#   u32  ioread32(void __iomem *addr);               addr=%x0 v=$retval
#
# Read return-values come back via a kretprobe (r:) — that's what the
# `iowrite_entry`/`ioread_entry` naming convention below maps onto.
# The translator's regex matches `<fn>_entry:` (writes) and
# `<fn>_entry:` for reads with `v=$retval addr=...`. Both shapes feed
# the same `(fn, val, addr)` tuple shape downstream.
#
# WARNING: on some kernels these accessors are inlined and won't
# expose a kprobe symbol. Verify before a real capture:
#   cat /proc/kallsyms | grep -wE 'iowrite32|ioread32'
# If they're missing, the operator needs to switch to per-driver
# helpers (e.g. `hailo_pcie_*`) instead. The script warns + exits
# rather than producing an empty capture.

echo "==> resetting ftrace state" >&2
echo 0 > "$TRACE_DIR/tracing_on"
echo > "$TRACE_DIR/trace"
echo nop > "$TRACE_DIR/current_tracer"
# Clear any leftover kprobe definitions from a previous run.
> "$TRACE_DIR/kprobe_events"

# Build the kprobe event list. iowrite/ioread come in pairs (entry +
# return for reads). We only emit kprobes for symbols that show up
# in /proc/kallsyms — silently skipping inlined ones — and bail if
# zero kprobes got installed.
declare -i KPROBES_INSTALLED=0
for w in 8 16 32 64; do
    if grep -qwE "iowrite${w}" /proc/kallsyms; then
        # Writes: v=arg0, addr=arg1 (ARM64 calling convention).
        echo "p:iowrite${w}_entry iowrite${w} v=%x0 addr=%x1" \
            >> "$TRACE_DIR/kprobe_events"
        KPROBES_INSTALLED+=1
    fi
    if grep -qwE "ioread${w}" /proc/kallsyms; then
        # Reads: addr=arg0 at entry, return value via kretprobe.
        # The translator looks for `<fn>_entry: ... v=... addr=...`
        # in BOTH cases, so we emit a kretprobe that prints both.
        echo "r:ioread${w}_entry ioread${w} v=\$retval addr=%x0" \
            >> "$TRACE_DIR/kprobe_events"
        KPROBES_INSTALLED+=1
    fi
done
if (( KPROBES_INSTALLED == 0 )); then
    echo "error: no iowrite/ioread kprobes installed (all inlined?)" >&2
    echo "       Check /proc/kallsyms for the symbols. If missing," >&2
    echo "       switch to per-driver hailo_pcie_* kprobes instead." >&2
    exit 2
fi
echo "==> installed $KPROBES_INSTALLED kprobes:" >&2
sed 's/^/    /' "$TRACE_DIR/kprobe_events" >&2

# Enable just the kprobe events (other tracepoints stay off).
echo 1 > "$TRACE_DIR/events/kprobes/enable"

# Bigger ring buffer per CPU so a multi-second run doesn't wrap on
# busy MMIO traffic. 32 MB / CPU × 4 CPUs ≈ 128 MB total — fits
# comfortably in Pi 5's RAM.
echo 32768 > "$TRACE_DIR/buffer_size_kb" 2>/dev/null || \
    echo "warn: couldn't enlarge buffer_size_kb; default 1 MB/CPU will be tight" >&2

# ---- cleanup trap ---------------------------------------------------------

cleanup() {
    local rc=$?
    echo 0 > "$TRACE_DIR/tracing_on" 2>/dev/null || true
    echo 0 > "$TRACE_DIR/events/kprobes/enable" 2>/dev/null || true
    # Removing kprobe_events with echo "" (or `>`) un-installs every
    # kprobe we added — leaves the system in its pre-capture state so
    # a follow-on run doesn't accidentally accumulate duplicates.
    > "$TRACE_DIR/kprobe_events" 2>/dev/null || true
    echo nop > "$TRACE_DIR/current_tracer" 2>/dev/null || true
    exit $rc
}
trap cleanup EXIT INT TERM

# ---- run + capture --------------------------------------------------------

echo "==> starting trace + running workload" >&2
echo 1 > "$TRACE_DIR/tracing_on"
# TRUST MODEL: $WORKLOAD is an operator-supplied command line that
# will execute as root (this whole script runs as root for ftrace
# access). The operator is responsible for the content — no escaping
# or sandboxing happens here. This tool is meant for lab use by the
# person physically holding the SD card.
#
# Capture the rc WITHOUT letting `set -e` terminate the script.
# The primary use case is capturing the #682 wedge — i.e. HailoRT
# *will* return non-zero. If we let set -e fire, the script exits
# before turning tracing off and dumping the buffer, throwing away
# the entire reason we ran the capture. Use `|| WORKLOAD_RC=$?` so
# the assignment runs unconditionally and the trace dump that follows
# always executes.
WORKLOAD_RC=0
# shellcheck disable=SC2086 # WORKLOAD is operator-supplied command line
bash -c "$WORKLOAD" || WORKLOAD_RC=$?
echo 0 > "$TRACE_DIR/tracing_on"

if [[ $WORKLOAD_RC -ne 0 ]]; then
    echo "warn: workload exited rc=$WORKLOAD_RC — capturing trace anyway" >&2
fi

# ---- dump --------------------------------------------------------------

echo "==> dumping trace to $OUT_FILE" >&2
{
    echo "# hailort ftrace capture"
    echo "# date: $(date -Iseconds)"
    echo "# host: $(hostname)"
    echo "# kernel: $(uname -r)"
    echo "# pci_device: 0000:${HAILO_DEV}"
    echo "# bar_resources:"
    awk '
    NR>0 {
        printf "#   BAR%d: phys=0x%016x size=0x%x flags=0x%s\n",
               NR-1, strtonum("0x"$1), strtonum("0x"$2) - strtonum("0x"$1) + 1, $3
    }' "$HAILO_PCI_DIR/resource" | head -7
    echo "# workload: $WORKLOAD"
    echo "# workload_rc: $WORKLOAD_RC"
    echo "# --- ftrace data follows ---"
} > "$OUT_FILE"

cat "$TRACE_DIR/trace" >> "$OUT_FILE"

# A nearly-empty trace usually means the filter functions don't exist
# on this kernel (e.g. inlined iowrite32 on some ARM builds). Surface
# that loudly rather than letting the operator wonder why the diff
# finds nothing.
TRACE_LINES=$(grep -c -v '^#' "$OUT_FILE" || true)
if [[ "$TRACE_LINES" -lt 10 ]]; then
    echo "error: trace captured only $TRACE_LINES non-header lines" >&2
    echo "       (filter functions may be inlined on this kernel; try" >&2
    echo "        attaching kprobes to hailo_pci-specific symbols instead)" >&2
    exit 4
fi

echo "==> capture complete: $OUT_FILE" >&2
echo "    $(wc -l < "$OUT_FILE") lines, $(wc -c < "$OUT_FILE") bytes" >&2
echo "    workload rc=$WORKLOAD_RC" >&2
