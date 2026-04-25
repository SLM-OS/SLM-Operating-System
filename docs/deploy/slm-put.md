# `slm-put.py` — Shell Upload Helper

`slm-put.py` is the first host-side tool for staging binary files onto a
running SLM-OS instance without depending on Pi OS, SD card removal, or SSH.

It prefers the shell's framed `xput` upload protocol when available, with
fallback to the older binary-safe `put` / `put -a` chunk commands.

It can write bytes into `/mnt/files` over either:

- `telnetd` on the network
- `labctl`-mediated serial console as a fallback

---

## Usage

```bash
python3 scripts/tools/slm-put.py <host> <local_path> <remote_path>
python3 scripts/tools/slm-put.py --labctl <sbc_name> <local_path> <remote_path>
python3 scripts/tools/slm-put.py --transport serial <serial_target> <local_path> <remote_path>
```

Example:

```bash
python3 scripts/tools/slm-put.py --labctl pi-5-2 \
  build/policies/xgb.blob \
  /mnt/files/policies/xgb.blob
```

The standard writable directory contract under `/mnt/files` is
documented in `docs/specs/device-file-contract.md`.

Serial fallback example:

```bash
python3 scripts/tools/slm-put.py --transport serial pi-5-2 \
  build/policies/xgb.blob \
  /mnt/files/policies/xgb.blob
```

Optional flags:

- `--protocol {auto,framed,legacy}` — prefer framed `xput`, force framed,
  or force legacy `put` / `put -a`
- `--transport {telnet,serial}` — transport selection
- `--port <n>` — telnetd port (default `2323`)
- `--labctl` — resolve the target through `labctl info`
- `--chunk-bytes <n>` — raw bytes per upload chunk before hex encoding
- `--timeout <seconds>` — prompt wait timeout
- `--connect-retries <n>` — retry count for telnet connection setup
- `--retry-delay <seconds>` — delay between retries
- `--chunk-retries <n>` — recovery attempts after a chunk transport failure
- `--no-resume` — restart from byte `0` instead of resuming an existing remote file
- `--no-verify-size` — skip the final `stat` size check
- `--debug` — print raw shell responses for transport debugging

Resume behavior:

- in `--protocol auto` or `--protocol framed`, `slm-put.py` probes
  `xput status`
- if the target supports `xput`, the uploader uses framed upload and resumes
  from the active session's reported `received=` offset when the path and total
  size match
- if framed upload is unavailable, the uploader falls back to `put` / `put -a`
  and uses `stat`-based resume from the current remote size
- if a chunk loses its transport connection, the uploader reconnects and
  continues from the confirmed framed/session offset or legacy file size
- `--no-resume` starts a fresh upload instead of resuming existing state

---

## Device-side prerequisites

The target must already have:

For `--transport telnet`, the target must already have:

1. networking initialized
2. `telnetd` running
3. a writable mounted filesystem such as `/mnt/files`

Typical manual bring-up:

```text
net init
telnetd start
mkdir /mnt/files/policies
```

When `--labctl` is used, the uploader resolves the board's current
network address through `labctl info <sbc>`.

For `--transport serial`, the uploader opens the shell through
`labctl connect <target>` and waits for the standard `slmos> ` prompt.
That path is intended for recovery and early bring-up when networking or
`telnetd` is not available.

---

## Validation status

The first-cut uploader has been hardware-validated on `pi-5-2` in all
currently supported modes:

- direct-run host-side functional coverage exists in:
  - `python3 scripts/tools/test_slm_tooling.py`
- direct telnet to the board IP
- telnet with `--labctl` target resolution
- serial fallback with `--transport serial`
- framed `xput` auto-probing over telnet on a live SLM-OS boot
- framed resume-from-partial over telnet on a live SLM-OS boot
- framed `xput` auto-probing over serial on a live SLM-OS boot
- framed resume-from-partial over serial on a live SLM-OS boot
- legacy resume-from-partial over telnet on a live SLM-OS boot

---

## Notes

- This is intentionally a thin transport on top of the shell. It is now
  framed at the shell-command level via `xput`, but it is still not an
  authenticated transport.
- Binary data is sent as hex, so transfers are larger and slower than a
  native framing protocol. That is acceptable for the first dynamic-model
  staging path.
- The uploader waits for the standard shell prompt `slmos> ` after each
  chunk, so it expects a clean shell session on the remote end.
