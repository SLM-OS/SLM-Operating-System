#!/bin/bash
# Reference run: llama.cpp CUDA under JetPack on jetson-nano-2 (#475).
# usage: run-llama.sh <out-dir> <label>   Run on the board; assumes the power mode is already set.
set -u
OUT=$1; LABEL=$2; B=/root/llama-475/llama.cpp/build/bin; M=/root/qwen2.5-1.5b-instruct-q4_k_m.gguf
GF=$(ls -d /sys/devices/platform/bus@0/17000000.gpu/devfreq/*/)
mkdir -p "$OUT"
{ nvpmodel -q; echo "gpu cur/max: $(cat $GF/cur_freq) $(cat $GF/max_freq)"; jetson_clocks --show 2>/dev/null | grep -E 'GPU|CPU Cluster|EMC' ; } > "$OUT/$LABEL.env" 2>&1
tegrastats --interval 1000 --logfile "$OUT/$LABEL.tegra" & TS=$!
for ngl in 99; do for p in 1 2 3; do for r in 1 2 3; do
  for mode in natural fixed; do
    x=""; [ $mode = fixed ] && x="--ignore-eos"
    f="$OUT/$LABEL-ngl$ngl-p$p-r$r-$mode"
    $B/llama-completion -m $M -f /root/llama-475/p$p.txt -no-cnv -n 256 --temp 0 --seed 1 -ngl $ngl -t 4 -c 4096 -b 64 \
       --no-display-prompt $x > $f.out 2> $f.err
    (sleep 0; cat $GF/cur_freq) >> "$OUT/$LABEL.freq"
  done; done; done; done
kill $TS 2>/dev/null
