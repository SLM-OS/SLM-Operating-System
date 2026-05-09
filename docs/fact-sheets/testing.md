# Testing / CI — Fact Sheet

Test infrastructure: unit tests, integration tests, hardware-in-loop, CI.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| `make test` target | ✅ | Build + deploy via labctl | Build + deploy via labctl | ✅ (QEMU with GRUB ISO + `isa-debug-exit`) |
| Test kernel gate | `ENABLE_BOOT_TESTS=ON` | Same | Same | Same |
| Unit + integration tests | 600+ total | Subset run on hardware | Subset run on hardware | 120+ (specific x86-64 suite) |
| Test framework | `kernel/tests/*.c` with `TEST_ASSERT_*` macros | Same | Same | Same |
| Test harness | ARM semihosting | ARM semihosting (limited on real HW) | Serial pattern match via labctl | `isa-debug-exit` (exit-code 1 = pass) |
| Safety wrapper | `systemd-run --scope MemoryMax=3G CPUQuota=200%` + `timeout 120` | Hardware-timed | Hardware-timed | Same wrapper |
| Host-side tests | `make test-vbios`, `test-falcon`, `test-nvfw`, `test-bringup`, `test-ga10b-bringup` | Run on dev host | Run on dev host | Run on dev host |
| GSP harness | `./build/host-tools/gsp-harness` (Linux VFIO) | — | — | ✅ against real GA107 |
| Hardware test target | — | ⏸️ (`make hw-test-pi5` planned, #251) | Ad-hoc `labctl boot_test` | `make x86-hw-validate PLATFORM=X86_64 SLMOS_LABCTL=1` |
| Reliability target | `bench smp` distribution | `labctl boot_test --count 10` | Same | Same |
| CI pipeline | `/ci` slash command (manual, local) | Same | Same | Same |
| Pre-commit hook | `.pre-commit-config.yaml` if present | Same | Same | Same |
| Flaky-test tracking | Per-issue (#200 noted during net expansion) | #216 — secondary dormancy | No standing flakes | #171 — closed |

## Skipped / Blocked

- **#251 — `make hw-test-pi5`** target: automated `boot_test` + net init/ping regression. Planned; not implemented. Current Pi 5 HW testing is manual via labctl.
- **Automated Jetson hardware test loop** — labctl `boot_test --count N` works, but there's no `make hw-test-jetson` shortcut like there is for x86-64. Tracked implicitly in the broader lab-automation space.
- **GPU pipeline end-to-end test on hardware** — blocked on GPU channel submit (#258 Jetson, #185 x86-64). Host-side test coverage is complete; hardware-integrated compute inference is not testable until those unblock.
- **Networking hardware regression on Jetson** — blocked on #25 / #266.
- **x86-64 QEMU baseline failures** — historical entry. PR #366's `make test PLATFORM=X86_64` run shows zero `[FAIL]` lines and a clean "PASSED - All tests passed" summary. Issue #237 (last updated 2026-04-17, claimed 10 failures) is stale and needs re-verification before close.
- **Pi 5 intermittent `test_work_stealing_distributes_load` flake** — tied to #216 (secondary dormancy). Assertion relaxed to "at least one task ran off the owner CPU" so the test passes whenever stealing actually moves work.
- **`test_lua.c` cascade-on-failure** — Unity's `TEST_ASSERT_*` long-jumps past `lua_slm_close(L)`, leaking the state. One real failing Lua test will exhaust the test heap and cascade into apparent failures of subsequent tests, hiding the actual root cause. Surfaced during the PR #366 investigation. Tracked: #374.
- **Full SMP preemption integration test** — exists on QEMU and x86-64; meaningfully cannot run under cooperative preemption on Pi 5 / Jetson without mid-task-preemption semantics. Five integration tests are marked accordingly.
- **Fuzz testing / property-based tests** — none. Unit tests are all deterministic.
- **Code coverage measurement** — not wired. No lcov/gcov reporting.

## See also

- `docs/testing.md` (narrative)
- `.claude/commands/ci.md` — `/ci` slash command (local CI pipeline)
- `docs/archive/ci-cd.md` — archived GitHub Actions config (retired)
- `docs/code-review-standards.md` (post-change checklist)
- `kernel/tests/` (test source)
- `docs/testing/` (HW validation logs, live)
- `docs/archive/test-runs/` (dated HW validation snapshots)

*Last updated: 25 April 2026*
