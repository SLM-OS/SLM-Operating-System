#!/bin/bash
#
# test-check-emc-rate-path.sh - Host-only test for scripts/check-emc-rate-path.py.
set -euo pipefail

CHECK="$(cd "$(dirname "$0")/.." && pwd)/check-emc-rate-path.py"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

expect() {  # expect <name> <exit status> <grep pattern or "-">; source on stdin
    local name="$1" want="$2" pattern="$3" got=0 output
    cat > "$WORK/src.c"
    output="$(python3 "$CHECK" "$WORK/src.c" 2>&1)" || got=$?
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

# The post-xhci request main.c made before #1161.
expect "main.c's MRQ_CLK request fails" 1 "src.c:3: EMC rate set through MRQ_CLK" <<'C'
        if (boot_source_is_uefi()) {
            uint64_t before = 0, after = 0;
            int emc_rc = bpmp_clk_set_rate(TEGRA234_CLK_EMC,
                                           TEGRA234_EMC_MAX_HZ, &after);
        }
C

expect "the shell's old clocks emc request fails" 1 "src.c:2:" <<'C'
        uint64_t got = 0;
        int rc = bpmp_clk_set_rate(TEGRA234_CLK_EMC,
                                   (uint64_t)mhz * 1000000u, &got);
C

expect "slm_prog's local id fails" 1 "src.c:1:" <<'C'
int rc = bpmp_clk_set_rate( T234_CLK_EMC , 3199000000u, &got);
C

expect "the bare id fails" 1 "src.c:1:" <<'C'
int rc = bpmp_clk_set_rate(31u, 3199000000u, NULL);
C

expect "a floor passes" 0 - <<'C'
int rc = bpmp_emc_floor_khz(BPMP_BWMGR_CLIENT_CPU_CLUSTER0, 3199000u, &got);
C

expect "reading the rate passes" 0 - <<'C'
(void)bpmp_clk_get_rate(TEGRA234_CLK_EMC, &before);
C

expect "other clocks pass" 0 - <<'C'
int rrc = bpmp_clk_set_rate(id, GA10B_COLD_CLK_RATE_MAX_HZ, &actual);
int r2 = bpmp_clk_set_rate(TEGRA234_CLK_PWM3, want, &got);
int r3 = bpmp_clk_set_rate(310, want, &got);
C

expect "a comment passes" 0 - <<'C'
/* never bpmp_clk_set_rate(TEGRA234_CLK_EMC, ...) here */
// nor bpmp_clk_set_rate(T234_CLK_EMC, x, y)
C

if [[ "$fails" -ne 0 ]]; then
    echo "test-check-emc-rate-path: $fails failure(s)"
    exit 1
fi
echo "test-check-emc-rate-path: all passed"
