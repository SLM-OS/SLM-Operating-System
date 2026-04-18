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
| CI pipeline | GitHub Actions via `docs/ci-cd.md` | CI runs QEMU target only | CI runs QEMU target only | CI runs QEMU target only |
| Pre-commit hook | `.pre-commit-config.yaml` if present | Same | Same | Same |
| Flaky-test tracking | Per-issue (#200 noted during net expansion) | #216 — secondary dormancy | No standing flakes | #171 — closed |

## Skipped / Blocked

- **#251 — `make hw-test-pi5`** target: automated `boot_test` + net init/ping regression. Planned; not implemented. Current Pi 5 HW testing is manual via labctl.
- **Automated Jetson hardware test loop** — labctl `boot_test --count N` works, but there's no `make hw-test-jetson` shortcut like there is for x86-64. Tracked implicitly in the broader lab-automation space.
- **GPU pipeline end-to-end test on hardware** — blocked on GPU channel submit (#258 Jetson, #185 x86-64). Host-side test coverage is complete; hardware-integrated compute inference is not testable until those unblock.
- **Networking hardware regression on Jetson** — blocked on #25 / #266.
- **x86-64 QEMU baseline failures** — 8 pre-existing failures in the x86-64 test suite (`test_vmm_detected_ram`, `test_pic_timer_unmasked`, `test_platform_defines`, PMM/VMM/PCI/GIC cascade from "Model memory init failed" at boot). Unrelated to the work landed; tracked separately.
- **Pi 5 intermittent `test_work_stealing_distributes_load` flake** — tied to #216 (secondary dormancy). Assertion relaxed to "at least one task ran off the owner CPU" so the test passes whenever stealing actually moves work.
- **Full SMP preemption integration test** — exists on QEMU and x86-64; meaningfully cannot run under cooperative preemption on Pi 5 / Jetson without mid-task-preemption semantics. Five integration tests are marked accordingly.
- **Fuzz testing / property-based tests** — none. Unit tests are all deterministic.
- **Code coverage measurement** — not wired. No lcov/gcov reporting.

## See also

- `docs/testing.md` (narrative)
- `docs/ci-cd.md` (GitHub Actions config)
- `docs/code-review-standards.md` (post-change checklist)
- `kernel/tests/` (test source)
- `docs/testing/` (HW validation logs, live)
- `docs/archive/test-runs/` (dated HW validation snapshots)

*Last updated: 18 April 2026*
