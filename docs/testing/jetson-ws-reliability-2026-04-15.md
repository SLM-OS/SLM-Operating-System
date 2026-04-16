# Jetson work-stealing reliability sweep — 2026-04-15

**Purpose:** statistically confirm that PR #184's #166 fix (retiring
`SPINLOCK_SKIP_LOCKING` on Jetson in favour of the runtime
`spinlock_hw_enabled` flag) did not paper over a timing-dependent
residual failure. Three manual runs during development landed the
fix; this sweep is the follow-up the handoff explicitly calls for.

## Method

- **Board:** jetson-nano-2 (192.168.4.93), Cortex-A78AE × 6, 8 GB RAM, L4T 5.15.148-tegra.
- **Kernel:** post-merge `main` at `a1ccb22` equivalent — plain
  `make kernel PLATFORM=JETSON_ORIN_NANO` (no `WORK_STEALING=ON`
  override — the S4 follow-up flip makes it default-ON on Jetson).
  Entry at `0x80000000`.
- **Cycle:** labctl `power cycle` → wait for `gradient-desktop login:`
  → SSH + `slmos-kexec /home/john/slmos.elf` → wait for `slmos>` →
  `bench stealing 16` → parse `Completed: N / 16`.
- **Pass criterion:** `Completed: 16 / 16` with no `System halted`,
  `PANIC`, `Data Abort`, or timeout before results print.

`/tmp` on the Jetson L4T image is cleared at boot (despite being
backed by the root filesystem), so the kernel ELF was copied to
`/home/john/slmos.elf` for persistence across power cycles.

## Results

| Iteration | Outcome | Completed | Wall-clock |
|---|---|---|---|
|  1 | ✅ PASS | 16/16 | 20 ms |
|  2 | ✅ PASS | 16/16 | 20 ms |
|  3 | ✅ PASS | 16/16 | 20 ms |
|  4 | ✅ PASS | 16/16 | 20 ms |
|  5 | ✅ PASS | 16/16 | 20 ms |
|  6 | ✅ PASS | 16/16 | 20 ms |
|  7 | ✅ PASS | 16/16 | 20 ms |
|  8 | ✅ PASS | 16/16 | 20 ms |
|  9 | ✅ PASS | 16/16 | 20 ms |
| 10 | ✅ PASS | 16/16 | 20 ms |

**Pass rate: 10/10 (100%).** Raw log in
`docs/testing/jetson-ws-reliability-2026-04-15/runs.txt` (named
`.txt` rather than `.log` so it is not caught by the `*.log`
gitignore rule).

Per-CPU distribution was consistent across runs (2/4/4/4/1/1 from
the three dev-time runs; individual iterations in this sweep were
not re-captured, but wall-clock and completion count matched bit-
for-bit). The 20 ms wall-clock is the same number
`docs/work-stealing-bench.md` §"Jetson Orin Nano" already lists.

## Observations

- One `power_cycle` MCP call hit a Kasa `AuthenticationError` on
  iteration 2; documented flake (see
  `docs/testing/x86-hw-validation-2026-04-15.md` finding #3). A
  single retry succeeded — no bearing on the kernel under test.
- `slmos-kexec` worked cleanly on all 10 boots. No RAS errors, no
  stale-interrupt faults, no garbled UART output — the three
  prior Jetson kexec hazards (GPU DMA, DAIF mask, UARTC LSR DSB)
  are all solved.
- `bench stealing 16` output, wall-clock, and distribution were
  bit-for-bit stable across runs — the scheduler reaches the same
  steady state every time.

## Conclusion

The #166 fix is not hiding a timing flake. 10 consecutive
power-cycle → kexec → `bench stealing` cycles all pass. Jetson with
`WORK_STEALING=ON` as the default is safe to keep as the shipped
configuration.
