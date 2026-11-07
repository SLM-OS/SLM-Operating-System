#!/bin/bash
# run-board.sh - build the forward harness and run it on the board (#475).
# Run from anywhere on the board, in the MAXN power mode:
#   run-board.sh fast   tiny-model checks and golden, then real-model timing
#   run-board.sh full   the same, then all checks on the real model
cd "$(dirname "$0")" || exit 2
export PATH=/usr/local/cuda/bin:$PATH
M=/root/qwen2.5-1.5b-instruct-q4_k_m.gguf
nvcc -O3 -std=c++17 -arch=sm_87 -Xcompiler -fopenmp -lgomp -I../slm slm_forward.cu -o slm_forward 2>&1 |
    grep -v -E "warning|Remark|^$" | tail -5
OMP_NUM_THREADS=6 timeout 90 ./slm_forward ../fixtures/tiny_qwen2.gguf \
    --golden ../fixtures/tiny_qwen2_greedy.rec --prefill 8 --decode 8 --ctx 128 > fwd-tiny.log 2>&1
echo "tiny: $(grep -c PASS fwd-tiny.log) PASS"
grep -E "FAIL|llama.cpp|^PASS" fwd-tiny.log | cut -c1-110
timeout 200 ./slm_forward "$M" --no-check > fwd-time.log 2>&1
grep -E "prefill 63|decode 32|mma issue" fwd-time.log | cut -c1-105
sed -n '/per-op prefill/,/ALL/p' fwd-time.log | cut -c1-70
if [ "${1:-fast}" = full ]; then
    OMP_NUM_THREADS=6 timeout 900 ./slm_forward "$M" --ctx 256 > fwd-full.log 2>&1
    echo "real: rc=$? $(grep -c PASS fwd-full.log) PASS"
    grep -E "FAIL|reference prefill|^  prefill  |^  decode  |^PASS|^FAIL|issue cycles|MAC share" fwd-full.log | cut -c1-125 | head -20
fi
date -u
