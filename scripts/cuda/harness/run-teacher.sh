#!/bin/bash
# run-teacher.sh - compare greedy decoding with llama.cpp CUDA on the bench prompts (#475).
# On the board, in MAXN, from anywhere. Steps:
#   build   build llama_ref_driver against /root/llama-475/llama.cpp
#   ref     record llama.cpp logits for p1..p3 (256 tokens) into ref-p*.rec
#   check   driver tokens against llama-completion's stored outputs, if present
#   teach   teacher forcing and free run, FP16 and q8_1
#   time    timings with q8_1 activations, plus all q8_1 checks
#   all     every step above
cd "$(dirname "$0")" || exit 2
export PATH=/usr/local/cuda/bin:$PATH
L=/root/llama-475/llama.cpp
M=/root/qwen2.5-1.5b-instruct-q4_k_m.gguf
P=/root/llama-475
N=${N:-256}
step=${1:-all}

if [ "$step" = build ] || [ "$step" = all ]; then
    g++ -O2 -std=c++17 llama_ref_driver.cpp -I$L/include -I$L/ggml/include -L$L/build/bin \
        -lllama -lggml -lggml-base -Wl,-rpath,$L/build/bin -o llama_ref_driver || exit 1
    nvcc -O3 -std=c++17 -arch=sm_87 -Xcompiler -fopenmp -lgomp -I../slm slm_forward.cu -o slm_forward 2>&1 |
        grep -v -E "warning|Remark|^$" | tail -5
fi
if [ "$step" = ref ] || [ "$step" = all ]; then
    for p in 1 2 3; do
        ./llama_ref_driver run $M $P/p$p.txt $N ref-p$p.rec || exit 1
    done
fi
if [ "$step" = check ] || [ "$step" = all ]; then
    # llama-completion's own greedy output for the same prompt, from the baseline.
    for p in 1 2 3; do
        f=$P/results/MAXN-ngl99-p$p-r1-fixed.out; [ -f "$f" ] || f=""
        [ -n "$f" ] || { echo "p$p: no stored llama-completion output"; continue; }
        ids=$(python3 - ref-p$p.rec <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<I", d, 4)[0]; o = 8; r = {}
for _ in range(n):
    l = struct.unpack_from("<I", d, o)[0]; o += 4; name = d[o:o + l].decode(); o += l
    dl = struct.unpack_from("<I", d, o)[0]; o += 4; r[name] = d[o:o + dl]; o += (dl + 3) & ~3
t = struct.unpack("<%di" % (len(r["greedy.tokens"]) // 4), r["greedy.tokens"])
print(",".join(map(str, t)))
EOF
)
        ./llama_ref_driver detok $M "$ids" > ref-p$p.txt
        if cmp -s <(head -c 400 ref-p$p.txt) <(head -c 400 "$f"); then
            echo "p$p: driver output matches llama-completion (first 400 bytes)"
        else
            echo "p$p: driver output DIFFERS from llama-completion"
            head -c 200 ref-p$p.txt; echo; echo ---; head -c 200 "$f"; echo
        fi
    done
fi
if [ "$step" = teach ] || [ "$step" = all ]; then
    for mode in fp16 q8; do
        x=""
        [ $mode = q8 ] && x="--q8"
        for p in 1 2 3; do
            ./slm_forward $M --no-check $x --ctx 512 --teacher ref-p$p.rec > teach-$mode-p$p.log 2>&1
            echo "== $mode p$p"
            grep -E "top-1 agreement|logit difference|first token different|step +[0-9]+:" teach-$mode-p$p.log | head -8
            ours=$(grep "^  ours:" teach-$mode-p$p.log | sed 's/^  ours: *//')
            ./llama_ref_driver detok $M "$ours" > out-$mode-p$p.txt
        done
    done
fi
if [ "$step" = time ] || [ "$step" = all ]; then
    timeout 200 ./slm_forward $M --no-check --q8 > fwd-q8-time.log 2>&1
    grep -E "prefill 63|decode 32" fwd-q8-time.log | cut -c1-105
    OMP_NUM_THREADS=6 timeout 900 ./slm_forward $M --q8 --ctx 256 > fwd-q8-full.log 2>&1
    echo "q8 checks: rc=$? $(grep -c PASS fwd-q8-full.log) PASS"
    grep -E "FAIL|^  prefill  |^  decode  |q8 B=|_q8 " fwd-q8-full.log | cut -c1-110 | head -30
fi
date -u
