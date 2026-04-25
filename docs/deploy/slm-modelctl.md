# `slm-modelctl.py` — Runtime Blob Operator Wrapper

`slm-modelctl.py` is the first operator-focused wrapper around the
runtime blob path. It now defaults to a subcommand-oriented CLI and
still preserves the old positional upload form as compatibility mode.

## Usage

```bash
python3 scripts/tools/slm-modelctl.py apply [options] <domain> <kind> <local_path> [remote_path]
python3 scripts/tools/slm-modelctl.py load [options] <domain> <kind> <local_path> [remote_path]
python3 scripts/tools/slm-modelctl.py activate [options] <domain> <kind>
python3 scripts/tools/slm-modelctl.py rollback [options] <domain> <kind>
python3 scripts/tools/slm-modelctl.py clear [options] <domain> <kind>
python3 scripts/tools/slm-modelctl.py status [options] <domain>
python3 scripts/tools/slm-modelctl.py probe [options] {ai_mlp,ai_ppo}
```

Compatibility mode still works:

```bash
python3 scripts/tools/slm-modelctl.py [options] <domain> <kind> <local_path> [remote_path]
```

That legacy form is treated as `apply ...`.

Examples:

```bash
python3 scripts/tools/slm-modelctl.py \
  apply \
  --target pi-5-2 \
  --labctl \
  eviction cacheus_config \
  build/policies/cacheus.blob

python3 scripts/tools/slm-modelctl.py \
  load \
  --target pi-5-2 \
  --transport serial \
  --clear-first \
  sched config \
  build/policies/sched-config.blob

python3 scripts/tools/slm-modelctl.py \
  activate \
  --target pi-5-2 \
  sched mlp

python3 scripts/tools/slm-modelctl.py \
  rollback \
  --target pi-5-2 \
  eviction cacheus_config

python3 scripts/tools/slm-modelctl.py \
  status \
  --target pi-5-2 \
  sched

python3 scripts/tools/slm-modelctl.py \
  probe \
  --target pi-5-2 \
  --labctl \
  ai_mlp

python3 scripts/tools/slm-modelctl.py \
  apply \
  --target pi-5-2 \
  --labctl \
  --clear-first \
  --probe-raw 7 \
  sched mlp \
  /tmp/sched-mlp-probe.blob
```

## Useful flags

- `--target <name-or-host>` — board name, host, or IP
- `--transport {telnet,serial}` — use telnetd or the serial console
- `--protocol {auto,framed,legacy}` — upload protocol passed through to `slm-put.py`
- `--labctl` — resolve telnet targets through `labctl info`
- `--clear-first` — clear prior runtime state for that kind before loading
- `--tryboot` — reboot a Pi OS maintenance boot into one-shot SLM-OS first
- `--expect-raw <n>` — for `probe`, fail unless the sampled raw action matches
- `--probe-raw <n>` — for `apply`, activate and then run a scheduler probe that must match
- `--probe-policy {ai_mlp,ai_ppo}` — override the scheduler policy used by `--probe-raw`
- `--probe-sleep-ms <n>` — control the probe task run window used by `probe` / `--probe-raw`
- `--debug` — print raw shell responses

## Notes

- Default remote path is `/mnt/files/policies/<filename>`.
- `slm-put.py` now auto-caps chunk sizes to stay under the shell line
  limit while still allowing larger requested chunk sizes.
- `--tryboot` is intended for dual-boot Pi maintenance workflows such as
  `pi-5-2`; it uses `labctl ssh` and the current `pi` password expected in
  the lab (`slmos`).
- With `--transport serial`, `slm-modelctl.py` now sequences shell
  access and uploads instead of trying to hold two serial-console
  sessions at once.
- This wrapper intentionally does not hide the underlying shell
  contract. It is a workflow convenience layer, not a new control
  plane.

## Validation status

- direct-run host-side functional coverage exists in:
  - `python3 scripts/tools/test_slm_tooling.py`
- hardware-validated on `pi-5-2` for explicit lifecycle subcommands:
  - `load`
  - `activate`
  - `rollback`
  - `clear`
  - `status`
- hardware-validated on `pi-5-2` for both upload/control transports:
  - telnet + `--labctl`
  - serial fallback
- hardware-validated on `pi-5-2` for the maintenance-OS workflow too:
  - authenticate to Pi OS over `labctl ssh`
  - trigger one-shot `sudo reboot '0 tryboot'`
  - wait for the SLM-OS shell to become ready
  - run the same upload + lifecycle flow automatically
- `probe` is hardware-validated on `pi-5-2` for:
  - `ai_mlp` with a deterministic runtime `sched_mlp` blob forcing raw action `7`
  - `ai_ppo` with a deterministic runtime `sched_ppo` blob forcing raw action `13`
- the multi-step remote-script flow is in place and working for both
  current scheduler AI policies
- `apply --probe-raw` is hardware-validated on `pi-5-2` for the
  `sched mlp` one-shot load + activate + probe flow
