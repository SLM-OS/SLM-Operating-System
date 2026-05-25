# Testing / CI — Fact Sheet

Test infrastructure: unit tests, integration tests, hardware-in-loop, CI.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| `make test` target | ✅ | Build + deploy via labctl | Build + deploy via labctl | ✅ (QEMU with GRUB ISO + `isa-debug-exit`) |
| `make test NET_SSHD=ON` (SSH stack) | ✅ (58 SSH-suite cases) | Build + labctl | Build + labctl | n/a (no SSH on x86 today) |
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
- **Pi 5 intermittent `test_work_stealing_distributes_load` flake** — tied to #216 (secondary dormancy). Assertion relaxed to "at least one task ran off the owner CPU" so the test passes whenever stealing actually moves work.
- **`test_lua.c` cascade-on-failure** — Unity's `TEST_ASSERT_*` long-jumps past `lua_slm_close(L)`, leaking the state. One real failing Lua test will exhaust the test heap and cascade into apparent failures of subsequent tests, hiding the actual root cause. Tracked: #374.
- **Full SMP preemption integration test** — exists on QEMU and x86-64; meaningfully cannot run under cooperative preemption on Pi 5 / Jetson without mid-task-preemption semantics. Five integration tests are marked accordingly.
- **Fuzz testing / property-based tests** — none. Unit tests are all deterministic.
- **Code coverage measurement** — not wired. No lcov/gcov reporting.

## SSH-feature test layout (#199)

Six dedicated suites gated on `NET_SSHD=ON`:

| Suite | Cases | Coverage |
|---|---|---|
| `test_rng` | 9 | RNG init / source dispatch / jitter pool / chi-squared self-test |
| `test_sshd` | 7 | Listener + per-conn ring buffer (via `sshd_test.h` hooks) |
| `test_host_key` | 6 | Ed25519 gen / persist / load / fingerprint round-trip |
| `test_passwd` | 10 | scrypt round-trip + length-bound + bootstrap-gate behaviour |
| `test_wolf_heap` | 11 | Dedicated wolfssl allocator: alloc/free/realloc + magic-stamp violation + double-free guard |
| `test_sshd_autostart` | 15 | `/etc/sshd.conf` parser internals (decimal / bool / line tokeniser via `sshd_autostart_test.h`) |

All 58 cases pass on `make test NET_SSHD=ON`. Tests use `TEST_IGNORE_MESSAGE` to skip cleanly on targets where `/mnt/files` isn't mounted.

End-to-end hardware validation (full KEX + scrypt auth + REPL over the SSH channel) was performed manually on `jetson-nano-1` and `pi-5-2` during the #199 hardware-validation pass; no automated `make hw-test-ssh` shortcut yet.

## See also

- `docs/testing.md` (narrative)
- `.claude/commands/ci.md` — `/ci` slash command (local CI pipeline)
- `docs/code-review-standards.md` (post-change checklist)
- `kernel/tests/` (test source)
- `docs/testing/` (HW validation logs, live)
- `docs/ssh.md` (SSH design + test surface)

*Last updated: 25 April 2026*
