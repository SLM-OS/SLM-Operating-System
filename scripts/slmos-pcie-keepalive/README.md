# PCIe C8 Clock Keepalive Experiment (#25)

**Status: NEGATIVE RESULT.** Neither this kernel module nor any
combination of user-space mitigations preserves the Tegra PCIe C8
clocks through a `kexec -e` transition. SLM-OS still reads
`0xFFFFFFFF` from APPL/DBI post-kexec. See
`docs/jetson-pcie-investigation.md` for the full evidence chain.

## What this is

A Linux kernel module (`pcie_clk_keepalive.ko`) that grabs a
`clk_prepare_enable` reference on `pex2_c8_core` (and optionally
`pex2_c8_core_m`) at init and **deliberately does not release it on
exit.** The goal: test whether a kernel-level clock ref outranks
BPMP's teardown on kexec.

## Build (on the target Jetson)

Kernel headers are available from `nvidia-l4t-kernel-headers`.

```sh
cd scripts/slmos-pcie-keepalive
make
```

Output: `pcie_clk_keepalive.ko`.

## Usage (was)

```sh
insmod ./pcie_clk_keepalive.ko
dmesg | tail -2   # verify "holding core clock at 62500000 Hz"
slmos-kexec /path/to/slmos.elf   # or kexec -l + kexec -e
```

## What it tested

1. Module grabs `pex2_c8_core` via `clk_get("core")` on the pcie
   platform device and calls `clk_prepare_enable`.
2. On module exit, the ref is **not** released.
3. Post-kexec, SLM-OS's `rtldiag` should show APPL != 0xFFFFFFFF
   if the clock survived.

## Result

APPL still reads `0xFFFFFFFF` post-kexec, identical to the
no-module case. Attempted permutations that all failed:

- Module alone (kernel-level `clk_prepare_enable`).
- Module + sysfs refcount bump (100+ via `echo 1 > state`).
- Module + `echo on > power/control` (runtime-PM override).
- Module + `echo 1 > mrq_rate_locked`.
- Module + `echo 1 > powergate/pciex8a/state` + `.../pciex8b/state`.
- All of the above combined.

## Interpretation

BPMP firmware, not Linux's clock framework, is gating the clock
during the kexec shutdown path. No Linux-side intervention (sysfs,
module, runtime PM, powergate override) changes this behavior.

## Remaining options for future work

1. **Crash-kernel path** (`kexec -p` + `sysrq-c`) — skips
   `device_shutdown()` entirely; may also skip whatever BPMP
   notification is triggering the gate. Requires `crashkernel=N` on
   the L4T boot cmdline and SLM-OS adaptation to run from the
   reserved region.
2. **Custom BPMP firmware** — modify `brq_reboot_handler` or
   similar to not force PCIe gating on reboot. Requires NVIDIA
   BPMP source access.
3. **Custom TF-A** — if TF-A's SMC handling during kexec triggers
   the gate, patching TF-A would fix it. Requires a signed flash.
