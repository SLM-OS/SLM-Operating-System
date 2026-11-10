# Rules for every coherency run 2 builder and critic (#1193)

Read `CLAUDE.md` and `docs/archive/475-gauntlet/builder-rules.md` first.
Where this file is stricter, this file wins.

## Environment

- Repository: `/home/john/projects/CS-496-Capstone-SLM-Operating-System`.
  Work in the worktree the orchestrator gave the agent. Use `git -C <path>`
  or absolute paths, not `cd <worktree> && ...` chains.
- `export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH` in every
  shell. One build at a time per worktree (`runtime/target` is shared by
  every build dir of a checkout). Run long builds in the background; Jetson
  builds and `/ci` can exceed the 10-minute Bash limit.
- QEMU only through the Makefile targets (`make test`, `make run`).

## Hardware

- Boards: pi-5-1 and jetson-nano-2 only. Touch a board only while the
  orchestrator has granted it to the agent, in writing, in the prompt or a
  follow-up message.
- labctl MCP tools only (`mcp__labctl__*`, `lab://sbcs` for IPs); never the
  labctl CLI. The orchestrator holds both claims as
  `coherency-2-orchestrator`. Do not claim, renew or release.
- The `/deploy-and-test` skill (`.claude/skills/deploy-and-test/SKILL.md`)
  is the deploy path. Its scp/ssh to the Jetson's SD Linux and
  `scripts/jetson-uefi-deploy.sh` are the only permitted ssh. No other
  script talks to a board or the lab host.
- Pi 5: labctl reads images only from the main checkout's
  `/home/john/projects/CS-496-Capstone-SLM-Operating-System/build/kernel/`.
  Copy a worktree image there under a unique name (`slmos-<comp>-<tag>.bin`)
  and pass that absolute path. Run `sdwire_info` before every
  `sdwire_update`. Pi 5 loads `kernel_2712.img`; update both it and
  `kernel8.img`, or check which one the card uses.
- Jetson: scp to `/root/slmos-<comp>-<tag>.elf`, `ssh board sync`, check the
  sha256, then `slmos-kexec /root/slmos-<comp>-<tag>.elf` detached. Never
  overwrite `/root/slmos.elf`. A power cycle without `sync` loses the file.
  UEFI-direct: `scripts/jetson-uefi-deploy.sh` and
  `docs/deploy/jetson-uefi-direct.md`; after any ESP write, `fsck.vfat -n`
  and a fresh-mount sha check (#1187). Never power-cycle with the ESP
  mounted.
- `boot_test --count 10` (labctl) is the minimum for a claim that a kernel
  boots. It cannot kexec or BootNext; on Jetson loop by hand as the skill
  describes, with a capture started before each boot.
- If a deploy does not come back, apply #1195 once: SDWire round trip while
  off, 20 s off, power on. One retry, then record the failure and move on.
- `serial_send` and `serial_capture` always set
  `capture_until="slmos>"` (literal `>`).
- No BL31 or TF-A flashing (#976). Never leave Jetson `recovery_mode`
  asserted. Check `run_health_check` and `actuation_status` before
  debugging a silent board.
- When a hardware turn ends, leave the board at the SLM-OS shell or at
  Linux, powered on, and say which.

## Evidence

- Raw captures go on `gauntlet-2-hw-pi5` or `gauntlet-2-hw-jetson` under
  `docs/archive/coherency-gauntlet/run2-hw/c<N>/`, one file per run. The
  filename names build, boot path and run number, for example
  `c2/removal-uefi-burst-run07.txt`.
- A capture is the whole output: power-on (or kexec trigger) to prompt for
  a boot, every test name for a test run, one line per CPU for a benchmark.
  No `tail`, `grep`, `head`, trimming or reassembly. A register read
  carries the boot's banner (commit hash) and a timestamp.
- Other components push to the same evidence branch. Before every push:
  `git pull --rebase origin <branch>`. Plain `git push`, never force.
- The commit hash printed in each boot banner must match the branch head
  the evidence is claimed for.

## Branches, commits, PRs

- No merge to main. New PRs are drafts (`gh pr create --draft`). PR bodies
  end with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
- Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Write "Fixes #N" only where the PR really closes #N; negation is ignored
  by GitHub.
- Each component edits only the files its prompt lists. Ask the
  orchestrator (in the final report) for anything else.
- Do not edit `docs/archive/coherency-gauntlet/coherency-2.md`. Return log
  entries in the final report; the orchestrator appends them.

## Writing

- No em-dashes. No second person. Imperative for instructions, past tense
  for results.
- Every number has its denominator and its source file:
  "20/20, `c1/removal-int-run{01..20}.txt`", never "passed".
- A causal claim is a hypothesis until the experiment that separates it
  from the brief's alternative has run. Label it so.

## Defects found along the way

- Outside the component's scope: file a GitHub issue (labels for platform,
  subsystem, priority; milestone Backlog; evidence file linked). Do not fix.
- Inside scope and changing the component's conclusion: stop and report to
  the orchestrator before attempting the fix.

## Reporting back

An agent stops and returns a report when it finishes, when it needs a board
it has not been granted, or when it hits a stop condition. The report
lists: state; branches and commit hashes; evidence filenames with counts;
every failure seen, unedited; open questions; the log entry text to append.
