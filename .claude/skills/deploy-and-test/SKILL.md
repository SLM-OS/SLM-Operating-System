---
name: deploy-and-test
description: Build SLM-OS, deploy it to a lab board through labctl (Pi 5 via SDWire, Jetson via scp + slmos-kexec), boot to the shell, run shell commands, and report. Use when the user says "deploy", "test on hardware", "flash and boot", "check on pi-5-N / jetson-nano-N", "boot test N times", or invokes /deploy-and-test.
argument-hint: "<sbc-name> [build flags, e.g. AI_SCHED=ON] [--runs N] [--cmd \"<shell cmd>\" ...] [--keep]"
---

# Deploy and Test (SLM-OS)

Build, deploy, boot, run shell commands and report, on one lab board. Adapted from
the labctl example (`~/projects/Embedded-Lab-Control/skills/deploy-and-test.md`)
with the SLM-OS rules from CLAUDE.md and memory added.

**Hard rules (CLAUDE.md "labctl Is the Only Hardware Interface"):**
- Use the labctl MCP tools only, never the labctl CLI.
- Never `mount`, never touch `/dev/sd*`, no `nc` / `picocom`, never stop or restart ser2net.
- The one non-labctl step is the Jetson deploy (scp + ssh to the board's Linux), as documented for `slmos-kexec`.
- If labctl misbehaves, record the defect. If it blocks the run, stop and tell the user rather than working around it.

**Destructive tools** (`sdwire_update`, `power_cycle`, `power_off`, `serial_send`, `boot_test`):
- The first call returns `status: confirmation_required` with a `confirm_token`.
- Check the plan, then repeat the call with identical arguments plus the token. The token is single-use and lasts 120 s.
- In the steps below, "call X" means both calls.

**Announce waits.** Before any call that leaves the user waiting more than 5 s (builds, boot captures, long `serial_send`), say what is running and roughly how long.

## Step 0: Resolve the run

From `$ARGUMENTS` and the request, work out the following. Ask only if the target board is unknown.

| Item | Pi 5 (`pi-5-1`, `pi-5-2`) | Jetson (`jetson-nano-1`, `jetson-nano-2`) |
|---|---|---|
| Build | `make kernel PLATFORM=RASPI5 <flags>` | `make kernel PLATFORM=JETSON_ORIN_NANO <flags>` |
| Artifact | `build/kernel/slmos.bin` | `build/kernel/slmos.elf` |
| Deploy | `sdwire_update`, partition 1 | scp to `/root/slmos.elf`, `sync`, `slmos-kexec` |
| Shell prompt | `slmos>` | `slmos>` |
| Boot to prompt | ~15 s | ~60 s Linux, then ~60 s kexec to shell |

- Flags are whatever the change needs: `AI_SCHED=ON`, `EVICTION_MODELS=ON`, `JETSON_HW_TICK=OFF`, and so on.
- Always run `make kernel-clean` first when switching platform or flags.
- `--keep` leaves the new kernel installed. The default is to restore the board's previous kernel and power state at the end (Step 5).
- **Rebase check.** If the result will be reported as "hardware-verified" for shared infrastructure, `git log HEAD..origin/main` must be empty first. A self-contained check on an un-rebased branch is fine; say which it was.
- Board IPs and other lab config live in labctl; don't hard-code them here or in memory. For a Jetson, read the address from the labctl MCP resource `lab://sbcs` (it lists each SBC's IP).

## Step 1: Build

1. Run the build in the background; Jetson builds can exceed the 10-minute Bash limit.
2. On failure, show the last 30 lines and stop. No board is claimed yet.
3. Record the artifact's sha256, the first 16 hex digits, for the report.

Pi 5 deploys need no copy: `build/kernel` is inside the MCP server's `mcp.allowed_read_paths`, so pass the absolute path. Run `list_images` if that ever changes.

## Step 1b: Verify live state, then claim

Before trusting any docs (CLAUDE.md "Verify live board state first"):

1. **Health:** `run_health_check(sbc_name)` reports power, ping and serial. Note the **starting power state**; Step 5 restores it.
2. **Claims:** `list_claims`. If another agent holds the board, report who and stop. `request_sbc_release` may be used; don't force anything.
3. **Bindings:** `bindings_list(sbc_name)`, then `actuation_status` for each binding. A Jetson `recovery_mode` binding left `asserted` holds the board in USB Force Recovery: silent console, no network. Fix that, or report it, before going further.
4. **Claim:** `claim_sbc(sbc_name, duration_minutes, reason="deploy-and-test: <what>", agent_name="claude-code", context={branch, issue})`. About 30 min covers a single run; for `--runs N`, use roughly N × 2 min on Pi 5 or N × 3 min on Jetson, plus 10.
5. **Renew** with `renew_sbc_claim` before any long gap.

## Step 2: Deploy

### Pi 5 (SDWire)

1. **Check the card:** run `sdwire_info(sbc_name)`. An SLM-OS card has a single FAT partition labelled `SLMOS`. Pi OS cards are multi-partition, and pi-5-1 has carried both. If the layout is not what the run expects, stop and ask.
2. **Find the kernel name(s):** `sdwire_cat(sbc_name, 1, "config.txt")`.
   - If it sets `kernel=<name>`, deploy to that name only.
   - If there is no `kernel=` line, deploy to both `kernel_2712.img` and `kernel8.img`. The firmware loads `kernel_2712.img`, and a stale copy under either name can keep booting silently.
3. **Check for a leftover backup:** `sdwire_ls(sbc_name, 1)`.
   - If any `<kernel>.prev` already exists, an earlier run never restored. That file may be the board's real original kernel, and this deploy would overwrite it.
   - Stop and ask whether to restore it first (Step 5) or treat the current kernel as the one to keep.
   - Note which of the kernel names from Step 2 exist on the card; Step 5 needs that list.
4. **Deploy and back up in one call.** Copies run first, then renames. For each kernel name:
   - **If it exists on the card:** copy `"<abs path>/build/kernel/slmos.bin:<kernel>.new"`, then rename `"<kernel>:<kernel>.prev"` and `"<kernel>.new:<kernel>"`.
   - **If it doesn't exist:** copy straight to `"<abs path>/build/kernel/slmos.bin:<kernel>"`, with no renames. Step 5 deletes it.

   For example, with a single existing name:
   ```
   sdwire_update(sbc_name, partition=1,
     copies=["<abs path>/build/kernel/slmos.bin:<kernel>.new"],
     renames=["<kernel>:<kernel>.prev", "<kernel>.new:<kernel>"],
     reboot=true)
   ```
   `reboot=true` power-cycles the board when done, which also powers on a board that started off.
5. **If the result says `Not power cycled: ...`,** the claim was lost: report it and stop.

### Jetson (Linux, then kexec)

1. **Get Linux up.** If the board is not already at its Linux login (for example, still running SLM-OS):
   - call `power_cycle(sbc_name)`;
   - then `serial_capture(sbc_name, timeout=120, until_pattern="login:")`, about 60 s.
2. **Copy, sync and kexec** with one Bash script. Keep it in the scratchpad, not the repo. It should:
   - wait for ssh;
   - back up the board's current kernel, and stop the script on any problem. `/root/slmos.elf` is the default kernel `slmos-kexec` boots, and other sessions rely on it:
     ```
     ssh root@<ip> 'if [ -e /root/slmos.elf.prev ]; then echo PREV_EXISTS; exit 3; fi
                    if [ -e /root/slmos.elf ]; then cp -p /root/slmos.elf /root/slmos.elf.prev
                    else echo NO_ORIGINAL; fi; sync' || exit 1
     ```
     - `PREV_EXISTS` (exit 3): an earlier run left a backup. Stop and ask, as for Pi 5; if the user says the earlier run was a deliberate `--keep`, the backup can be deleted.
     - `NO_ORIGINAL`: the board had no kernel there. Note it for Step 5, which then deletes the new file instead of restoring;
   - `scp` the ELF to `root@<ip>:/root/slmos.elf`;
   - run `ssh root@<ip> 'sync; sha256sum /root/slmos.elf'`, and compare the hash with Step 1;
   - start `setsid nohup slmos-kexec /root/slmos.elf > /tmp/slmos-kexec.log 2>&1 < /dev/null &` over ssh, with `-o ServerAliveInterval=2 -o ServerAliveCountMax=2`, and give up on the ssh session after about 60 s.

   An ssh "server not responding" message at the end is expected: kexec has dropped Linux. Notes:
   - The `sync` is mandatory. Without it, a later hard power-cycle loses the write and the board boots the last-synced kernel.
   - Plain `slmos-kexec` preserves the MNIST and sched-MLP GPU channels. Add `--no-helper` only when the run needs the eviction producer alone.

## Step 3: Boot and capture

1. Wait for the shell: `serial_capture(sbc_name, timeout=<90 Pi 5 | 120 Jetson>, until_pattern="slmos>|PANIC|panic", tail=40)`.
2. **On a timeout or panic,** report the last captured lines, mark the run FAIL, and go to Step 5.
3. **Scan the capture for problems:**
   - errors: `[ERROR]`, `[FAIL]`, `panic`, `fault`, `exception`, `Synchronous`;
   - warnings: `[WARN]`, `warning`.

   Expected noise to ignore:
   - `[TELNETD] autostart: net_init failed` on Pi 5 without a network;
   - `cdc_ecm: network connection down` on Jetson.

## Step 4: Run shell commands

For each requested command:
```
serial_send(sbc_name, data="<cmd>", capture_until="slmos>", capture_timeout=<s>)
```

- **Always set `capture_until`** to a literal `slmos>`, not `&gt;`. Without it the call blocks for the whole timeout.
- For commands whose output ends with a known line (for example, a bench footer), match that line instead.
- **Garbled first characters** of the echoed command are a known cosmetic labctl issue.
- **On Jetson, output can slip to the next send.** Right after kexec, the first command's output sometimes arrives with the following send. If a reply is empty or belongs to the previous command, send it again.
- **Bench timeouts:** give `bench sched-policy` up to 90 s and set `capture_until` to its last line, `no winning policy`.

## Step 5: Restore, release, report

**Restore** unless `--keep` was given or the user asked to leave the new kernel installed:
- **Pi 5:** one `sdwire_update(sbc_name, 1, ..., reboot=<started on>)` call. It powers the board off before switching the card, so no separate `power_off` is needed, and `reboot=false` leaves the board off.
  - For each name that existed before the deploy: rename `"<kernel>:<kernel>.test"` and `"<kernel>.prev:<kernel>"`, then delete `"<kernel>.test"`.
  - For each name the deploy created: delete `"<kernel>"`.

  This leaves the card exactly as found. Example with one existing name: `renames=["<kernel>:<kernel>.test", "<kernel>.prev:<kernel>"], deletes=["<kernel>.test"]`.
- **Jetson:** the deploy replaced `/root/slmos.elf` on the board's disk, so put it back:
  1. Call `power_cycle` and wait for `login:` (Step 2).
  2. Restore the file:
     - **After a backup:** `ssh root@<ip> 'mv /root/slmos.elf.prev /root/slmos.elf && sync; sha256sum /root/slmos.elf'`.
     - **After `NO_ORIGINAL`:** `ssh root@<ip> 'rm -f /root/slmos.elf && sync'`.

  The board is then left at Linux with `/root/slmos.elf` as it was found. With `--keep`, there are two options:
  - **Delete the backup:** once Linux is up, run `ssh root@<ip> 'rm -f /root/slmos.elf.prev && sync'`.
  - **Leave the board at the SLM-OS shell:** the backup is still on disk, and the next run's backup check will stop on it. Say so in the report and tell the user that `/root/slmos.elf.prev` needs deleting or restoring.

**Release on every path,** including failures and early stops: `release_sbc(sbc_name)`.

**Report:**
```
Deploy & Test: <sbc>  (<branch> @ <short sha>, rebased: yes/no)
-----------------------------------------------------------------
Build:   make kernel PLATFORM=<P> <flags> ... OK  (sha <16 hex>)
Deploy:  <how> ... OK
Boot:    'slmos>' after <N> s ... PASS | FAIL (<last lines>)
Command: <cmd>
         <relevant output lines>
Errors:  <lines> | none
Board:   <restored to previous kernel, off/on | left on new kernel (--keep) | Jetson at Linux, kernel restored>
```
Post hardware results on the GitHub issue being worked on, if there is one.

## Boot reliability mode (`--runs N`)

CLAUDE.md asks for at least 10 runs.

- **Pi 5:** first do the Step 2 deploy with the backup renames and `reboot=false`. Then:
  ```
  boot_test(sbc_name, expect_pattern="slmos>", runs=N, timeout=90)
  ```
  Leave out `image` and `dest`: the kernel is already on the card, and passing them makes `boot_test` overwrite it with no backup. If the result starts with `Stopped:`, report the run as incomplete, not as a pass rate.
- **Jetson:** `boot_test` can't kexec, so loop by hand.
  - **Once, before run 1:** do Step 2.2's backup, `scp` and `sync`.
  - **Each run:** power-cycle to Linux (Step 2.1), start `slmos-kexec /root/slmos.elf` the same detached way, then capture (Step 3). Don't repeat the backup: from run 2 on, `/root/slmos.elf.prev` exists and the check would stop the run.
  - **Once, after the last run:** Step 5.

Either way, report pass/fail with the failure pattern, for example "3/10 failed, all stalled after `[GA10B-P6]`". Then do Step 5.
