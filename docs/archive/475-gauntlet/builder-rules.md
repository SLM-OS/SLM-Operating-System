# Rules for every #475 builder and critic

- Hardware: only jetson-nano-2. Never touch jetson-nano-1 or any other board.
- Hardware only through labctl MCP tools (never the labctl CLI) and /deploy-and-test;
  ssh to the board's SD Linux only for scripts/jetson-uefi-deploy.sh and documented steps.
- Only touch the board when your brief says you own it; the orchestrator holds the claim.
- Check live board state first; never leave recovery_mode asserted.
- Kexec path is out of scope and may break: list what breaks. Do not touch #819, #844, #539.
- Do not merge to main, force-push, or reopen an issue closed by the user.
- Shared files have one owner at a time: kernel/sched/sched.c, kernel/include/slm_ffi.h, GPU dispatch code
  (kernel/gpu/*, kernel/src/slm_ffi.c). Do not edit them unless your brief names you owner.
- Build: export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH. One build at a time per worktree.
  QEMU only through make targets. Git commits end with the attribution line
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
- Writing style: no "you/your" in docs; ADRs under a page, no em-dashes; TODOs use CLAUDE.md markers.
- Add regression tests with every change and report results honestly, with command output.
- File unrelated defects with gh issue, leave them alone. Link #475 defects to #475.
