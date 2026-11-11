# C2 runner and analyzer, coherency run 3 (#1216, #1203)

Adapted from run 2's `run2-tools/c2/` (same extraction, same ESP check).
Paths below are relative to this directory. Board IP from `lab://sbcs`.

## Scripts

| File | Purpose |
|---|---|
| `kexec-stage.sh <ip> <elf> <tag> [sha]` | scp to `/root/slmos-r3-<tag>.elf`, `sync`, compare sha256 (and the MANIFEST sha if given). Never touches `/root/slmos.elf`. |
| `kexec-fire.sh <ip> <tag> <delay> [sha]` | Re-check the staged sha, wait `delay` s, start `slmos-kexec /root/slmos-r3-<tag>.elf` detached (deploy skill form). Run in the background before the `serial_capture` call. |
| `esp-check.sh <ip>` | Run 2's read-only ESP check (#1187): BootCurrent/Next/Order, `fsck.vfat -n`, read-only mount sha of `SLMOS.efi` and `l4t.efi`, not mounted. |
| `extract.py list/dump` | labctl replies verbatim from the agent transcript (`$C2_TRANSCRIPT`). Checked byte-identical against run 2's `ncoff-kexec-sustained-run16.txt` from run 2's transcript. |
| `assign.py cell <cell> <since> <until> <outdir>` | Writes `<cell>-boot.txt` (each `slmos>` capture) and `<cell>-runNN.txt` = confirmed `cat /sys/version` reply + the next confirmed `bench stealing --sustain` reply. Anything else becomes `<cell>-other-<id>.txt`. Refuses to overwrite. |
| `assign.py boots <prefix> <since> <until> <outdir>` | Step 0: each `slmos>` capture becomes `<prefix>-bootNN.txt`. |
| `analyze.py [--head SHA] [--per-run] <label> <files>` | Tables T1 to T4 from the files alone. Drops and lists files whose banner (`commit:` line of `cat /sys/version`) does not match `--head`, or whose c2t lines are incomplete. |
| `bars.py q1 / q2` | The numbers each clause of questions 1 and 2 names, from the files alone. |
| `idle-disasm.sh`, `fn-disasm.sh`, `v1-qemu-check.sh` | The V1 checks: `idle_task_func` disassembly in each cell image, and the QEMU build and `make test` at the V1 head. |
| `sens.py <files>` | Labelled sensitivity view, not a bar: per-CPU median share, f_pd/att and first-success median two ways, with no chk filter (used only for `analysis/Q2-sensitivity.txt`). |
| `dump-saved.py <tool_use_id> <saved-json> <out>` | Writes an evidence file from a labctl reply that Claude Code saved to disk because it was too large to return inline (the D and F UEFI boot logs). |
| `cell-finish.sh <cell> <since> <head> [--no-assign]` | assign + analyze + bars q1 for one cell into the staging directory. Paths are hard-coded to the C2 session scratchpad. |
| `push-ev.sh "<subject>" "<body>"` | Copies staged evidence and tables into the worktree, commits, `git pull --rebase`, plain push to `gauntlet-3-hw-jetson`. Paths hard-coded as above. |
| `qemu-smoke.sh <wt> <log>` | `make kernel WORK_STEALING=ON`, then `make run` (QEMU_GUARD) driven with `cat /sys/version` and the two benches. |

## Per cell (kexec)

1. Board at SD Linux (power_cycle, then `serial_capture until=login:` when
   coming from SLM-OS; allowed because C2 is the only labctl caller while
   it holds the lab).
2. `kexec-stage.sh` with the MANIFEST elf sha.
3. `kexec-fire.sh <ip> <tag> 8 <sha>` in the background, then
   `serial_capture(timeout=150, until_pattern="slmos>|PANIC|panic|Unhandled Exception")`.
4. 20 times: `serial_send("cat /sys/version", capture_until="slmos>")`, then
   `serial_send("bench stealing --sustain 2500", capture_until="slmos>",
   capture_timeout=45)`, each with its confirm round trip. Re-send a
   reply that slipped (deploy skill, Jetson) and keep both in the file.
5. `assign.py cell <cell> <since> <until> <evidence-dir>`, then
   `analyze.py --head <head>` and `bars.py`; commit to
   `gauntlet-3-hw-jetson` under `run3-hw/c2/` (`git pull --rebase`, plain push).

## Per cell (UEFI-direct)

As above, but step 2 and 3 are `scripts/jetson-uefi-deploy.sh <ip> <bin>`
then `serial_capture(timeout=150, until_pattern="slmos>|Unhandled Exception|Powering off|ABORT")`.
After the cell, power_cycle to SD Linux and run `esp-check.sh` (the ESP
write is checked from the next Linux boot, since the deploy script reboots).
