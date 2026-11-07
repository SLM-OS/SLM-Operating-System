#!/bin/bash
# ncu-tc.sh - tensor-core and throughput counters for one kernel filter (#475).
# Usage: ncu-tc.sh <kernel-regex> <instance-range|all> <label> -- <command...>
# Writes <label>.csv here and prints one summary line per kernel instance.
export PATH=/usr/local/cuda/bin:$PATH
regex="$1"; range="$2"; label="$3"; shift 4
M=gpu__time_duration.sum
M=$M,sm__cycles_active.avg,sm__cycles_active.sum
M=$M,sm__pipe_tensor_cycles_active.avg,sm__pipe_tensor_cycles_active.sum
M=$M,sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active
M=$M,sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_elapsed
M=$M,sm__inst_executed_pipe_tensor.sum,sm__inst_executed.sum
M=$M,sm__instruction_throughput.avg.pct_of_peak_sustained_active
M=$M,sm__warps_active.avg.pct_of_peak_sustained_active
M=$M,lts__t_bytes.sum
args=(--csv --print-units base -k "regex:$regex")
[ "$range" != all ] && args+=(--kernel-id "::regex:$regex:$range")
timeout "${NCU_TIMEOUT:-280}" ncu "${args[@]}" --metrics "$M" "$@" > "$label.csv" 2>&1
rc=$?
echo "ncu rc=$rc"
python3 - "$label.csv" <<'EOF'
import csv, sys, collections
rows = [l for l in open(sys.argv[1], errors="replace") if l.startswith('"')]
res = collections.OrderedDict()
hdr = None
for r in csv.reader(rows):
    if hdr is None:
        hdr = r
        continue
    if len(r) < 15:
        continue
    key = (r[0], r[4].split("(")[0], r[8])
    res.setdefault(key, {})[r[12]] = r[14].replace(",", "")
print("id kernel grid time_us tc_avg tc_sum cyc_avg tc%active tc%elapsed tc/cyc% tensor_inst inst inst/tensor ipc%peak warps% lts_MB")
for (i, k, g), m in res.items():
    f = lambda n: float(m.get(n, "nan")) if m.get(n, "n/a") != "n/a" else float("nan")
    ca, ta = f("sm__cycles_active.avg"), f("sm__pipe_tensor_cycles_active.avg")
    ti, ins = f("sm__inst_executed_pipe_tensor.sum"), f("sm__inst_executed.sum")
    print(i, k[:26], g.replace(" ", ""), "%.0f" % (f("gpu__time_duration.sum") / 1e3), "%.0f" % ta,
          "%.0f" % f("sm__pipe_tensor_cycles_active.sum"), "%.0f" % ca,
          "%.1f" % f("sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active"),
          "%.1f" % f("sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_elapsed"),
          "%.1f" % (100 * ta / ca if ca else 0), "%.0f" % ti, "%.0f" % ins,
          "%.1f" % (ins / ti if ti else 0),
          "%.1f" % f("sm__instruction_throughput.avg.pct_of_peak_sustained_active"),
          "%.1f" % f("sm__warps_active.avg.pct_of_peak_sustained_active"),
          "%.1f" % (f("lts__t_bytes.sum") / 1e6))
EOF
