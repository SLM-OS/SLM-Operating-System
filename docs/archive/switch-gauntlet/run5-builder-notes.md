# Run 5: operating notes for builders

These notes add mechanics to `run5-brief.md`; the brief's bars and rules
govern. Base: main **8ec0e330** (#1239 merged in C0).

## Worktrees and branches

- Repo root: `/home/john/projects/CS-496-Capstone-SLM-Operating-System`.
  Worktrees live in `.claude/worktrees/`. Create your own with
  `git -C <repo> worktree add <repo>/.claude/worktrees/s5-c<N> -b <branch> <base>`.
  Use `git -C <path>` or absolute paths; avoid `cd <dir> && cmd` chains.
- Never use bare `git stash`. Never touch another component's worktree.
- Branch names: C1 `c1/harness`, C2 `c2/switch-window` (plus temporary
  `c2/...` branches as needed), C3 `c3/pmm-halt`. Temporary branches are
  listed in your report so the close-out can archive and delete them.
- Evidence: make your own worktree of `gauntlet-5-hw-pi5` or
  `gauntlet-5-hw-jetson` (for example `s5-hw-pi5-c2`), write only under
  `docs/archive/switch-gauntlet/run5-hw/c<N>/`, `git add -f` for .log files,
  and `git pull --rebase` before every push (other components push to the same
  branch in their own directories). One file per run or boot, banner hash at
  the top. QEMU evidence goes on `gauntlet-5-hw-pi5`.

## Builds and QEMU

- Toolchain PATH for non-login shells: `export PATH="$HOME/.cargo/bin:$PATH"`
  (the ARM toolchain is already on PATH in this environment; check with
  `which aarch64-none-elf-gcc`).
- One build at a time per worktree (runtime/target is shared across
  BUILD_DIRs in a checkout; parallel builds mix cargo features).
- Every QEMU run goes through `make test` / `make run` or the systemd-run
  wrapper from CLAUDE.md (MemoryMax=3G, CPUQuota=200%, timeout). Kill only
  PIDs you started; never pkill or killall.
- The host has 8 cores; at most two components run QEMU at once. Record the
  load average with each QEMU rate.
- Until the orchestrator says the C0 baseline (/ci + 3x WORK_STEALING=ON on
  8ec0e330, started 22:58 UTC) is complete, do not build or run QEMU; read,
  design and edit only.

## Lab

- Do not call any labctl tool until the orchestrator's message says you hold
  the lab, and stop calling it when the handover window ends or when the
  orchestrator takes it back. Use labctl MCP tools only; never the labctl CLI.
- Before each session: run_health_check, bindings_list and actuation_status
  for jetson-nano-2's `recovery_mode` (must be released), list_claims. Then
  claim_sbc with agent_name `switch-5-c<N>`; release_sbc at the end. Restore
  power state (both boards were on at start).
- Pi 5 (pi-5-1): `sdwire_info` before every `sdwire_update`; labctl reads
  images only from the main checkout's `build/kernel/` (stage the image there
  under a unique name, for example `kernel_2712-s5c2-<hash>.img`); the Pi 5
  firmware loads `kernel_2712.img`, so update both `kernel_2712.img` and
  `kernel8.img` and restore the original kernels at the end (record their
  sha256 first). On silent boots, do a sdwire round trip and wait 20 s off
  before blaming the kernel (#1195).
- Jetson (jetson-nano-2): kexec path uses a separate
  `/root/slmos-s5c<N>-<hash>.elf` (scp, then `ssh root@192.168.4.5 sync`
  before any power cycle), never overwriting `/root/slmos.elf`. UEFI-direct
  uses `scripts/jetson-uefi-deploy.sh`; restore the ESP to `SLMOS.efi`
  789cba62 with no BootNext when done (check with fsck.vfat -n and a
  fresh-mount sha). Boots are triggered by the host-side loop from run 4b
  (`docs/archive/lifecycle-gauntlet/run4-hw/closeout/trigger.sh.txt` on
  `gauntlet-4-hw-jetson`; copy it to the scratchpad, change only the tmp
  path), run in the background while one `boot_test` call records 2 or 3
  boots from power-on into its own output_dir under `/var/lib/labctl/output/`.
  Nothing is installed on the board. No other ssh than these documented uses.
- Kernel command line / boot args: C1 documents how the filter and repeat
  count reach the test kernel on Pi 5 (cmdline.txt or equivalent), Jetson
  kexec (slmos-kexec append) and Jetson UEFI-direct; use exactly that.
- Pass `capture_until="slmos>"` on serial_send/serial_capture for shell
  commands. A long boot_test on one board stalls labctl calls for the other
  (stdio serializes calls).
- Say in your report how long each labctl session took.
- Single deploy-and-boot checks may use the `/deploy-and-test` skill (labctl MCP under the hood).
- No BL31 or TF-A flashing. pi-5-1 and jetson-nano-2 only.

## Reporting

- Do not write to `switch-5.md`; the orchestrator owns the log. Your final
  message is your report: branch heads (pushed), evidence file paths, every
  number with its file, what is open, temporary branches, and a draft log
  entry the orchestrator will append after the critic files.
- Ask the orchestrator (SendMessage to `main`) for: the lab, a tool family not
  exercised in C0, a decision outside your component, or a handoff to another
  component. With the user away, a step that needs a permission prompt is
  marked open, not left waiting.
- Do not spawn subagents. No em-dashes, no second person in docs or logs,
  hypotheses labelled until tested. Defects found along the way are filed as
  issues (label, milestone Backlog, evidence), not fixed in passing. Never
  write "fix/close/resolve #N" in commit text unless it truly closes #N. No
  PRs: the orchestrator opens draft PRs after the critic passes.
