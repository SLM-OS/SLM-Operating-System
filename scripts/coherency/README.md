# Linux coherency reference harness (#1193)

Userspace implementation of `docs/archive/coherency-gauntlet/litmus-spec.md`
(revision 3), used as the Linux reference bar for the cross-CPU coherency
investigation. The pattern bodies live in `litmus_patterns.h`, written
without libc so the same logic can be read against the in-kernel suite in
`kernel/include/coherency_litmus.h`.

## Build and test

- Build: `make -C scripts/coherency` (aarch64-linux-gnu-gcc -O2 -static
  -pthread).
- Test: `make -C scripts/coherency test` runs the detector self-test and a
  reduced pass under qemu-aarch64-static on an x86 host, including the
  injected faults (P6 sum, weak P4, stale previous-row values) that must end
  in `VERDICT FAIL`.

## Run

`litmus -m <minutes>` loops P1 to P6, P6L (LSE atomics), S1 and S2 over
every ordered CPU pair until the time elapses, then prints TOTAL, SENS,
VERDICT and LITMUS_DONE lines. `-d <n>` divides iterations for smoke runs
only (the header then says `spec=no`); `-I <fault>` injects a fault for
self-testing.

## Boards

- Jetson: copy the static binary to the board over ssh and run it under
  JetPack Linux with all CPUs online.
- Pi 5: `pi5/` builds an initramfs (`pi5/mkcpio.py`) whose `/init` runs the
  harness under the Raspberry Pi OS kernel and serves the results over HTTP
  at `http://<board>/status.txt`, with a copy on the console. Deploy on
  pi-5-1 with `sdwire_update reboot=false`, then `sdwire_info`, then
  `power_cycle delay=20` (#1195).
- `ectlr_mod/` is a kernel module that reads CPUECTLR_EL1 and
  CLUSTERECTLR_EL1 on every CPU. It contains no MSR instructions; check with
  `objdump -d ectlr.ko | grep -ci msr` before loading.

Results: `results/summary.md`, raw output in `results/<board>.txt`.
