# Rules for every #1193 builder and critic

- Hardware: jetson-nano-2 and pi-5-1 only. Never touch jetson-nano-1, pi-5-2 or any other board.
- Hardware only through labctl MCP tools (never the labctl CLI) and /deploy-and-test. ssh to a
  board's SD Linux only for documented steps and the C1 harness.
- Only touch a board when your brief says you own it; the orchestrator holds the claim. Check live
  board state first. Never leave a Jetson `recovery_mode` binding asserted.
- Jetson firmware (BL31, UEFI, anything in QSPI) is measurement-only: never flash it (#976 open).
  Pi 5 BL31 changes only through `tools/tfa-patches/`, and only if the orchestrator says so.
- Never use set/way or touch the stack after the MMU goes off on Jetson UEFI-direct. Never touch the
  Tegra NDIV +4 words. Reading an IMPLEMENTATION DEFINED register that may trap to EL3 needs the
  C0 answer first.
- Work only on your own branch or worktree; the orchestrator merges into gauntlet/coherency-1193.
  Read-only on main. Never force-push or reopen an issue the user closed.
- Shared files with one owner (C3): kernel/arch/arm64/boot.S, smp_boot.S, mmu.S,
  kernel/include/spinlock.h, ncmem.h, kernel/sched/sched.c, task.c, smp.c.
- Build: export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH. One build at a time per
  worktree. QEMU only through make targets.
- Reference material stays in ~/slmos-ref/ (fetch there, never into the repo). Cite as
  ~/slmos-ref/<vendor>/<file>:<line> or TRM section.
- Docs: no second person, no em-dashes, ADR under a page, TODO markers per CLAUDE.md.
- Commits end with: Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
- Regression tests with every code change; report results with command output, failures included.
  File unrelated defects with gh issue (labels, milestone Backlog), leave them alone. Link defects in
  this area to #1193.
