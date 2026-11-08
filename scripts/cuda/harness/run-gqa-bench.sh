#!/bin/bash
# run-gqa-bench.sh - HMMA GQA against slm_gqa_w8 on the board, JetPack (#1167).
#
# Copy scripts/cuda/{slm,staging,harness} and scripts/cuda/oplib-extract.py to
# the board (same layout) and run this from anywhere there, as root. It changes
# no power mode or clock: it reads nvpmodel's mode and the GPU and EMC clocks
# before and after, and the benchmark reads them around every point.
#
# Steps, each logged to its own file in this directory:
#   env.log        toolkit, power mode, clocks before
#   build.log      nvcc of gqa_bench (CUDA from /usr/local/cuda)
#   sass.log       gqa-sass-check.py on the board-built cubin
#   checks.log     every gqa_check.h case, exact reference
#   mutant.log     the same checks on a mask-off-by-one mutant (must fail)
#   timing.log     timing (gqa_bench_raw.csv holds every launch)
#   env-after.log  clocks after
#   memcheck.log   compute-sanitizer memcheck over the kv 2048 checks
#                  (MEMCHECK_TIMEOUT seconds, default 900)
cd "$(dirname "$0")" || exit 2
export PATH=/usr/local/cuda/bin:$PATH
GPUF=$(ls -d /sys/class/devfreq/*17000000.gpu* /sys/devices/platform/bus@0/17000000.gpu/devfreq/* \
    2>/dev/null | head -1)
EMC=/sys/kernel/debug/bpmp/debug/clk/emc/rate
export GQA_GPU_FREQ="$GPUF/cur_freq" GQA_EMC_RATE="$EMC"

clocks() {
    echo "date: $(date -u +%FT%TZ)"
    echo "nvpmodel: $(nvpmodel -q 2>&1 | tr '\n' ' ')"
    for f in cur_freq min_freq max_freq governor available_frequencies; do
        echo "gpu $f: $(cat "$GPUF/$f" 2>&1)"
    done
    echo "emc rate: $(cat "$EMC" 2>&1)"
}
{
    echo "host: $(uname -a)"
    echo "l4t: $(head -1 /etc/nv_tegra_release 2>&1)"
    nvcc --version | tail -2
    echo "gpu devfreq: $GPUF"
    clocks
} > env.log 2>&1
cat env.log

FL=(-O3 -std=c++17 -arch=sm_87 -Xcompiler -fopenmp -lgomp -I../slm -I../staging)
nvcc "${FL[@]}" gqa_bench.cu -o gqa_bench > build.log 2>&1
echo "build rc=$?"
nvcc -arch=sm_87 -O3 --cubin -I../slm ../staging/slm_gqa_hmma.cu -o gqa.cubin >> build.log 2>&1 &&
    python3 gqa-sass-check.py gqa.cubin > sass.log 2>&1
echo "sass rc=$?"; cat sass.log

OMP_NUM_THREADS=6 timeout 600 ./gqa_bench --checks-only > checks.log 2>&1
echo "checks rc=$? $(tail -1 checks.log)"

mkdir -p mut
sed 's/t < lim\[e >> 1\]/t <= lim[e >> 1]/' ../staging/slm_gqa_hmma.cu > mut/slm_gqa_hmma.cu
cmp -s ../staging/slm_gqa_hmma.cu mut/slm_gqa_hmma.cu && echo "mutant: sed changed nothing"
nvcc "${FL[@]}" -DGQA_KERNEL_SRC='"mut/slm_gqa_hmma.cu"' gqa_bench.cu -o gqa_bench_mut >> build.log 2>&1
OMP_NUM_THREADS=6 timeout 300 ./gqa_bench_mut --checks-only --only "opcheck rows" > mutant.log 2>&1
echo "mutant rc=$? (must be 1) $(tail -1 mutant.log)"

timeout 900 ./gqa_bench --no-checks --runs 21 --raw gqa_bench_raw.csv > timing.log 2>&1
echo "timing rc=$?"
sed -n '/^summary/,$p' timing.log
clocks > env-after.log 2>&1
cat env-after.log

# Last, so a slow sanitizer cannot crowd out the timing in a short board slot.
if command -v compute-sanitizer > /dev/null; then
    OMP_NUM_THREADS=6 timeout "${MEMCHECK_TIMEOUT:-900}" compute-sanitizer --tool memcheck \
        ./gqa_bench --checks-only --only "kv 2048" > memcheck.log 2>&1
    echo "memcheck rc=$? $(grep -E 'ERROR SUMMARY' memcheck.log | tail -1)"
else
    echo "memcheck: compute-sanitizer not found" | tee memcheck.log
fi
echo "run-gqa-bench: done $(date -u +%FT%TZ)"
