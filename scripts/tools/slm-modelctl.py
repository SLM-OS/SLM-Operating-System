#!/usr/bin/env python3
"""
Stage and activate runtime policy blobs on a running SLM-OS instance.

This is an operator wrapper around `slm-put.py` plus the shell-level
`eviction model ...` and `sched model ...` commands.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import pty
import select
import socket
import subprocess
import sys
import tempfile
import time
from typing import Callable


IAC = 255
DO = 253
DONT = 254
WILL = 251
WONT = 252
SB = 250
SE = 240
TOOL_DIR = pathlib.Path(__file__).resolve().parent
SHELL_MAX_LINE = 1024
SHELL_LINE_HEADROOM = 16


class TelnetShell:
    def __init__(self, host: str, port: int, prompt: bytes, timeout: float):
        self.host = host
        self.port = port
        self.prompt = prompt
        self.timeout = timeout
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.25)
        self.buf = bytearray()
        self.iac_pending = bytearray()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def _handle_iac(self, data: bytes, idx: int) -> int | None:
        if idx + 1 >= len(data):
            return None

        cmd = data[idx + 1]
        if cmd == IAC:
            self.buf.append(IAC)
            return idx + 2

        if cmd in (DO, DONT, WILL, WONT):
            if idx + 2 >= len(data):
                return None
            opt = data[idx + 2]
            if cmd in (DO, DONT):
                reply = bytes([IAC, WONT, opt])
            else:
                reply = bytes([IAC, DONT, opt])
            self.sock.sendall(reply)
            return idx + 3

        if cmd == SB:
            j = idx + 2
            while j + 1 < len(data):
                if data[j] == IAC and data[j + 1] == SE:
                    return j + 2
                j += 1
            return None

        return idx + 2

    def _consume_telnet(self, data: bytes) -> None:
        if self.iac_pending:
            data = bytes(self.iac_pending) + data
            self.iac_pending.clear()

        i = 0
        while i < len(data):
            if data[i] != IAC:
                self.buf.append(data[i])
                i += 1
                continue

            next_i = self._handle_iac(data, i)
            if next_i is None:
                self.iac_pending.extend(data[i:])
                break
            i = next_i

    def _recv_some(self) -> None:
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return
        if not data:
            raise RuntimeError("connection closed by remote host")
        self._consume_telnet(data)

    def read_until_prompt(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        while True:
            if self.prompt in self.buf:
                prompt_idx = self.buf.rfind(self.prompt)
                out = bytes(self.buf[: prompt_idx + len(self.prompt)])
                del self.buf[: prompt_idx + len(self.prompt)]
                return out
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"timed out waiting for shell prompt {self.prompt!r}"
                )
            self._recv_some()

    def run_command(self, command: str) -> bytes:
        self.sock.sendall(command.encode("ascii") + b"\n")
        return self.read_until_prompt()


class SerialShell:
    def __init__(self, target: str, prompt: bytes, timeout: float):
        self.target = target
        self.prompt = prompt
        self.timeout = timeout
        self.proc = subprocess.Popen(
            ["labctl", "connect", target],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("failed to open labctl serial console pipes")
        self.buf = bytearray()

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2.0)

    def _recv_some(self) -> None:
        if self.proc.stdout is None:
            raise RuntimeError("serial stdout is not available")
        deadline = time.monotonic() + 0.25
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not ready:
                return
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                if self.proc.poll() is not None:
                    raise RuntimeError("labctl serial console exited unexpectedly")
                return
            self.buf.extend(chunk)
            return

    def read_until_prompt(self) -> bytes:
        deadline = time.monotonic() + self.timeout
        nudged = False
        while True:
            if self.prompt in self.buf:
                prompt_idx = self.buf.rfind(self.prompt)
                out = bytes(self.buf[: prompt_idx + len(self.prompt)])
                del self.buf[: prompt_idx + len(self.prompt)]
                return out
            if time.monotonic() >= deadline:
                if not nudged and self.proc.stdin is not None:
                    self.proc.stdin.write(b"\n")
                    self.proc.stdin.flush()
                    nudged = True
                    deadline = time.monotonic() + self.timeout
                    continue
                raise RuntimeError(
                    f"timed out waiting for shell prompt {self.prompt!r}"
                )
            self._recv_some()

    def run_command(self, command: str) -> bytes:
        assert self.proc.stdin is not None
        self.proc.stdin.write(command.encode("ascii") + b"\n")
        self.proc.stdin.flush()
        return self.read_until_prompt()


Shell = TelnetShell | SerialShell
PROBE_REMOTE_PATH = "/mnt/files/slm-modelctl-probe.lua"
HTTP_FETCH_REMOTE_PATH = "/mnt/files/slm-modelctl-http.lua"
POLICY_BLOB_ROOT = "/mnt/files/policies"
MODEL_BLOB_ROOT = "/mnt/files/models"
AUTOLOAD_BLOB_ROOT = "/mnt/files/autoload"
STANDARD_BLOB_DIRS = (
    POLICY_BLOB_ROOT,
    MODEL_BLOB_ROOT,
    AUTOLOAD_BLOB_ROOT,
)


def add_common_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--target",
        default="pi-5-2",
        help="Board/IP target for upload and shell control (default: %(default)s)",
    )
    p.add_argument(
        "--transport",
        choices=("telnet", "serial"),
        default="telnet",
        help="Transport to use for upload and control (default: %(default)s)",
    )
    p.add_argument(
        "--protocol",
        choices=("auto", "framed", "legacy"),
        default="auto",
        help="Upload protocol passed to slm-put.py (default: %(default)s)",
    )
    p.add_argument("--labctl", action="store_true", help="Resolve telnet target through `labctl info`")
    p.add_argument("--port", type=int, default=2323, help="Telnet port (default: %(default)s)")
    p.add_argument("--prompt", default="slmos> ", help="Shell prompt (default: %(default)s)")
    p.add_argument("--timeout", type=float, default=10.0, help="Prompt timeout in seconds")
    p.add_argument(
        "--connect-retries",
        type=int,
        default=1,
        help="Shell connect retries before giving up (default: %(default)s)",
    )
    p.add_argument(
        "--retry-delay",
        type=float,
        default=1.0,
        help="Delay between connection retries in seconds (default: %(default)s)",
    )
    p.add_argument(
        "--chunk-bytes",
        type=int,
        default=512,
        help="Bytes per upload chunk before hex encoding (default: %(default)s)",
    )
    p.add_argument(
        "--tryboot",
        action="store_true",
        help="For labctl-managed boards, reboot Pi OS into one-shot SLM-OS before running",
    )
    p.add_argument("--debug", action="store_true", help="Print raw shell responses to stderr")


def add_domain_kind_args(p: argparse.ArgumentParser, *, include_kind: bool = True) -> None:
    p.add_argument("domain", choices=("eviction", "sched"), help="Policy domain to manage")
    if include_kind:
        p.add_argument("kind", help="Model kind within the selected domain")


def add_upload_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("local_path", nargs="?", help="Local blob file to upload")
    p.add_argument(
        "--http-url",
        help="Fetch the blob directly on-device with `http get` instead of uploading it from the host",
    )
    p.add_argument(
        "--sha256",
        help="Expected SHA-256 for `--http-url`; the on-device fetch fails if it does not match",
    )
    p.add_argument(
        "remote_path",
        nargs="?",
        help="Destination path on the SLM-OS VFS (default: /mnt/files/policies/<filename>)",
    )
    p.add_argument(
        "--clear-first",
        action="store_true",
        help="Run `<domain> model clear <kind>` before loading the new blob",
    )
    p.add_argument(
        "--autoload",
        action="store_true",
        help="After a successful load/apply, persist this blob for boot autoload too",
    )


def add_probe_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--expect-raw",
        type=int,
        help="Fail if the live decision raw action does not match",
    )
    p.add_argument(
        "--sleep-ms",
        type=int,
        default=50,
        help="Milliseconds to let the probe task run before sampling (default: %(default)s)",
    )


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Operate runtime scheduler/eviction model blobs on a running SLM-OS instance."
        )
    )
    sub = p.add_subparsers(dest="command", required=True)

    apply_p = sub.add_parser("apply", help="Upload, load, activate, and show status")
    add_common_args(apply_p)
    add_domain_kind_args(apply_p)
    add_upload_args(apply_p)
    apply_p.add_argument(
        "--probe-raw",
        type=int,
        help="After activate, run a scheduler probe and require this raw action",
    )
    apply_p.add_argument(
        "--probe-policy",
        choices=("ai_mlp", "ai_ppo"),
        help="Override the scheduler policy used for `--probe-raw`",
    )
    apply_p.add_argument(
        "--probe-sleep-ms",
        type=int,
        default=50,
        help="Probe task runtime before sampling when using `--probe-raw` (default: %(default)s)",
    )

    load_p = sub.add_parser("load", help="Upload, load, and show status without activation")
    add_common_args(load_p)
    add_domain_kind_args(load_p)
    add_upload_args(load_p)

    activate_p = sub.add_parser("activate", help="Activate a staged blob and show status")
    add_common_args(activate_p)
    add_domain_kind_args(activate_p)

    rollback_p = sub.add_parser("rollback", help="Rollback a blob kind and show status")
    add_common_args(rollback_p)
    add_domain_kind_args(rollback_p)

    clear_p = sub.add_parser("clear", help="Clear a blob kind and show status")
    add_common_args(clear_p)
    add_domain_kind_args(clear_p)

    autoload_status_p = sub.add_parser(
        "autoload-status",
        help="Show persisted autoload configuration for a domain",
    )
    add_common_args(autoload_status_p)
    add_domain_kind_args(autoload_status_p, include_kind=False)

    autoload_set_p = sub.add_parser(
        "autoload-set",
        help="Persist an already-present on-device blob for boot autoload",
    )
    add_common_args(autoload_set_p)
    add_domain_kind_args(autoload_set_p)
    autoload_set_p.add_argument(
        "remote_path",
        help="Existing blob path on the SLM-OS VFS to record for boot autoload",
    )

    autoload_clear_p = sub.add_parser(
        "autoload-clear",
        help="Clear a persisted boot autoload entry and show autoload status",
    )
    add_common_args(autoload_clear_p)
    add_domain_kind_args(autoload_clear_p)

    status_p = sub.add_parser("status", help="Show model status for a domain")
    add_common_args(status_p)
    add_domain_kind_args(status_p, include_kind=False)

    probe_p = sub.add_parser("probe", help="Probe live scheduler behavior after activation")
    add_common_args(probe_p)
    probe_p.add_argument(
        "policy",
        choices=("ai_mlp", "ai_ppo"),
        help="Scheduler AI policy to probe",
    )
    add_probe_args(probe_p)

    doctor_p = sub.add_parser(
        "doctor",
        help="Check standard writable directories and basic network readiness",
    )
    add_common_args(doctor_p)

    return p


def parse_args() -> argparse.Namespace:
    parser = build_parser()
    argv = list(sys.argv[1:])
    if argv:
        subcommands = {
            "apply",
            "load",
            "activate",
            "rollback",
            "clear",
            "autoload-status",
            "autoload-set",
            "autoload-clear",
            "status",
            "probe",
            "doctor",
        }
        global_flags = {"--labctl", "--tryboot", "--debug", "--clear-first", "--autoload"}
        global_opts_with_values = {
            "--target", "--transport", "--protocol", "--port", "--prompt",
            "--timeout", "--connect-retries", "--retry-delay", "--chunk-bytes",
            "--http-url", "--sha256",
            "--probe-raw", "--probe-policy", "--probe-sleep-ms",
            "--expect-raw", "--sleep-ms",
        }

        idx = 0
        while idx < len(argv):
            arg = argv[idx]
            if arg == "--":
                idx += 1
                break
            if arg in global_flags:
                idx += 1
                continue
            if arg in global_opts_with_values:
                idx += 2
                continue
            break

        if idx < len(argv) and argv[idx] in subcommands:
            if idx != 0:
                argv = [argv[idx]] + argv[:idx] + argv[idx + 1:]
        else:
            argv = ["apply"] + argv
    args = parser.parse_args(argv)
    if args.command in ("apply", "load"):
        has_local = getattr(args, "local_path", None) is not None
        has_http = getattr(args, "http_url", None) is not None
        if has_http and getattr(args, "remote_path", None) is None and has_local:
            args.remote_path = args.local_path
            args.local_path = None
            has_local = False
        if has_local == has_http:
            parser.error("exactly one of LOCAL_PATH or --http-url is required for apply/load")
        if getattr(args, "sha256", None) is not None and not has_http:
            parser.error("--sha256 requires --http-url")
    return args


def log_response(debug: bool, label: str, output: bytes) -> None:
    if not debug:
        return
    text = output.decode("utf-8", errors="replace")
    print(f"--- {label} ---", file=sys.stderr)
    print(text, file=sys.stderr, end="" if text.endswith("\n") else "\n")


def shell_command_failed(output: bytes) -> bool:
    text = output.decode("utf-8", errors="replace")
    return "Command returned error:" in text


def mkdir_already_exists(output: bytes) -> bool:
    text = output.decode("utf-8", errors="replace")
    return "Already exists" in text


def connect_telnet(host: str, port: int, prompt: bytes, timeout: float,
                   retries: int, retry_delay: float) -> TelnetShell:
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            return TelnetShell(host=host, port=port, prompt=prompt, timeout=timeout)
        except OSError as exc:
            last_error = exc
            if attempt == retries:
                break
            time.sleep(retry_delay)
    assert last_error is not None
    raise RuntimeError(
        f"failed to connect to {host}:{port} after {retries} attempt(s): {last_error}"
    )


def connect_serial(target: str, prompt: bytes, timeout: float,
                   retries: int, retry_delay: float) -> SerialShell:
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        shell: SerialShell | None = None
        try:
            shell = SerialShell(target=target, prompt=prompt, timeout=timeout)
            shell.initial_output = shell.read_until_prompt()
            return shell
        except Exception as exc:
            last_error = exc
            if shell is not None:
                try:
                    shell.close()
                except Exception:
                    pass
            if attempt == retries:
                break
            time.sleep(retry_delay)
    assert last_error is not None
    raise RuntimeError(
        f"failed to connect to serial target {target} after {retries} attempt(s): {last_error}"
    )


def resolve_labctl_target(name: str) -> str:
    proc = subprocess.run(
        ["labctl", "info", name],
        check=True,
        capture_output=True,
        text=True,
    )
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("ethernet:") or line.startswith("wifi:"):
            _, _, value = line.partition(":")
            target = value.strip().split()[0]
            if target:
                return target
    raise RuntimeError(f"no network address found in `labctl info {name}`")


def tryboot_to_slmos(target: str) -> None:
    pid, master_fd = pty.fork()
    if pid == 0:
        os.execvp("labctl", ["labctl", "ssh", target])
        raise RuntimeError("execvp returned unexpectedly")

    deadline = time.monotonic() + 45.0
    sent_password = False
    sent_reboot = False
    saw_disconnect = False
    buf = bytearray()
    child_alive = True

    def write_line(line: bytes) -> None:
        os.write(master_fd, line + b"\n")

    def poll_child() -> bool:
        nonlocal child_alive
        if not child_alive:
            return False
        try:
            child_pid, _ = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            child_alive = False
            return False
        if child_pid == pid:
            child_alive = False
            return False
        return True

    try:
        while time.monotonic() < deadline:
            if not poll_child():
                if sent_reboot:
                    saw_disconnect = True
                break

            ready, _, _ = select.select([master_fd], [], [], 0.25)
            if not ready:
                if not sent_password and time.monotonic() + 5.0 >= deadline:
                    os.write(master_fd, b"\n")
                continue

            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                chunk = b""
            if not chunk:
                if not poll_child() and sent_reboot:
                    saw_disconnect = True
                    break
                continue

            buf.extend(chunk)
            lower = bytes(buf).lower()

            if not sent_password and b"password:" in lower:
                write_line(b"slmos")
                sent_password = True
                continue

            if sent_password and not sent_reboot:
                if b"$" in buf or b"#" in buf or b"pi@pi-5-2" in lower:
                    write_line(b"sudo reboot '0 tryboot'")
                    sent_reboot = True
                    continue

            if sent_reboot and (
                b"closed by remote host" in lower
                or b"connection to " in lower
                or b"the system will reboot now!" in lower
            ):
                saw_disconnect = True
                continue
    finally:
        try:
            os.close(master_fd)
        except OSError:
            pass

    try:
        end_deadline = time.monotonic() + 10.0
        while time.monotonic() < end_deadline:
            child_pid, _ = os.waitpid(pid, os.WNOHANG)
            if child_pid == pid:
                break
            time.sleep(0.1)
        else:
            os.kill(pid, 15)
            time.sleep(1.0)
            child_pid, _ = os.waitpid(pid, os.WNOHANG)
            if child_pid != pid:
                os.kill(pid, 9)
                os.waitpid(pid, 0)
    except ChildProcessError:
        pass

    if not sent_password:
        raise RuntimeError(f"failed to authenticate to {target} for tryboot")
    if not sent_reboot:
        raise RuntimeError(f"failed to trigger tryboot reboot on {target}")
    if not saw_disconnect:
        raise RuntimeError(f"did not observe SSH disconnect after tryboot on {target}")

    time.sleep(5.0)


def run_shell_command(shell: Shell, command: str, debug: bool) -> bytes:
    out = shell.run_command(command)
    log_response(debug, command, out)
    if shell_command_failed(out):
        raise RuntimeError(f"remote command failed: {command}")
    return out


def autoload_command(domain: str, action: str, kind: str | None = None, remote_path: str | None = None) -> str:
    parts = [domain, "model", "autoload", action]
    if kind is not None:
        parts.append(kind)
    if remote_path is not None:
        parts.append(remote_path)
    return " ".join(parts)


def ensure_parent_dir(shell: Shell, remote_path: str, debug: bool) -> None:
    parent = str(pathlib.PurePosixPath(remote_path).parent)
    if not parent or parent == "." or parent == "/":
        return
    out = shell.run_command(f"mkdir {parent}")
    log_response(debug, f"mkdir {parent}", out)
    if shell_command_failed(out) and not mkdir_already_exists(out):
        raise RuntimeError(f"failed to create remote parent directory: {parent}")


def run_upload(args: argparse.Namespace, local_path: str, remote_path: str) -> None:
    cmd = [
        "python3",
        str(TOOL_DIR / "slm-put.py"),
        "--protocol",
        args.protocol,
        "--transport",
        args.transport,
        "--port",
        str(args.port),
        "--prompt",
        args.prompt,
        "--timeout",
        str(args.timeout),
        "--connect-retries",
        str(args.connect_retries),
        "--retry-delay",
        str(args.retry_delay),
        "--chunk-bytes",
        str(args.chunk_bytes),
    ]
    if args.labctl:
        cmd.append("--labctl")
    if args.debug:
        cmd.append("--debug")
    cmd.extend([args.target, local_path, remote_path])
    subprocess.run(cmd, check=True)


def upload_blob(args: argparse.Namespace, remote_path: str) -> None:
    assert getattr(args, "local_path", None) is not None
    run_upload(args, args.local_path, remote_path)


def default_remote_path(source: str) -> str:
    if "://" in source:
        trimmed = source.split("?", 1)[0].rstrip("/")
        name = pathlib.PurePosixPath(trimmed).name
    elif "\\" in source or (len(source) >= 2 and source[1] == ":"):
        name = pathlib.PureWindowsPath(source).name
    else:
        name = pathlib.Path(source).name or pathlib.PurePosixPath(source).name
    if not name:
        name = "blob.bin"
    return f"{POLICY_BLOB_ROOT}/{name}"


def normalize_blob_path(path: str) -> str:
    normalized = str(pathlib.PurePosixPath(path))
    if not normalized.startswith("/"):
        raise RuntimeError(
            "remote blob paths must be absolute device paths under "
            f"{POLICY_BLOB_ROOT}/ or {MODEL_BLOB_ROOT}/"
        )
    return normalized


def validate_operator_blob_path(path: str) -> str:
    normalized = normalize_blob_path(path)
    if normalized == AUTOLOAD_BLOB_ROOT or normalized.startswith(f"{AUTOLOAD_BLOB_ROOT}/"):
        raise RuntimeError(
            f"{AUTOLOAD_BLOB_ROOT}/ is system-managed; use "
            f"{POLICY_BLOB_ROOT}/ or {MODEL_BLOB_ROOT}/ for operator-managed blobs"
        )
    if (
        normalized.startswith(f"{POLICY_BLOB_ROOT}/")
        or normalized.startswith(f"{MODEL_BLOB_ROOT}/")
    ):
        return normalized
    raise RuntimeError(
        "remote blob paths managed by slm-modelctl.py must live under "
        f"{POLICY_BLOB_ROOT}/ or {MODEL_BLOB_ROOT}/"
    )


def fetch_blob(shell: Shell, args: argparse.Namespace, remote_path: str) -> None:
    assert getattr(args, "http_url", None) is not None
    ensure_parent_dir(shell, remote_path, args.debug)
    run_shell_command(shell, "net init", args.debug)
    deadline = time.monotonic() + max(args.timeout, 15.0)
    while True:
        status = run_shell_command(shell, "ifconfig", args.debug)
        status_text = status.decode("utf-8", errors="replace")
        if (
            "DHCP(bound)" in status_text
            or "STATIC" in status_text
        ):
            if "flags=UP" not in status_text and "UP," not in status_text:
                if time.monotonic() >= deadline:
                    raise RuntimeError("network link did not come up before HTTP fetch")
                time.sleep(0.5)
                continue
            break
        if "DHCP(failed)" in status_text:
            raise RuntimeError("network DHCP failed before HTTP fetch")
        if time.monotonic() >= deadline:
            raise RuntimeError("network did not become usable before HTTP fetch")
        time.sleep(0.5)
    command = f"http get {args.http_url} {remote_path}"
    if getattr(args, "sha256", None):
        command += f" {args.sha256}"
    if len(command) + 1 + SHELL_LINE_HEADROOM <= SHELL_MAX_LINE:
        run_shell_command(shell, command, args.debug)
        return

    def lua_quote(text: str) -> str:
        return (
            '"'
            + text.replace("\\", "\\\\")
                  .replace('"', '\\"')
                  .replace("\n", "\\n")
                  .replace("\r", "\\r")
                  .replace("\t", "\\t")
            + '"'
        )

    script = [
        f"local r = slm.http_get({lua_quote(args.http_url)}, {lua_quote(remote_path)}, "
        + (lua_quote(args.sha256) if getattr(args, "sha256", None) else "nil")
        + ")",
        "if r then",
        "  print(string.format('HTTP_FETCH_OK %d', r.status or -1))",
        "else",
        "  print('HTTP_FETCH_FAIL')",
        "end",
    ]
    ensure_parent_dir(shell, HTTP_FETCH_REMOTE_PATH, args.debug)
    upload_bytes_via_shell(
        shell,
        HTTP_FETCH_REMOTE_PATH,
        ("\n".join(script) + "\n").encode("utf-8"),
        args.debug,
        args.chunk_bytes,
    )
    try:
        out = run_shell_command(shell, f"lua-admin {HTTP_FETCH_REMOTE_PATH}", args.debug)
        text = out.decode("utf-8", errors="replace")
        if "HTTP_FETCH_OK " not in text:
            raise RuntimeError("HTTP fetch helper script did not report success")
    finally:
        try:
            run_shell_command(shell, f"rm {HTTP_FETCH_REMOTE_PATH}", args.debug)
        except Exception:
            pass


def read_initial(shell: Shell, debug: bool) -> None:
    initial = getattr(shell, "initial_output", None)
    if initial is None:
        initial = shell.read_until_prompt()
    log_response(debug, "initial", initial)


def print_output(output: bytes) -> None:
    sys.stdout.write(output.decode("utf-8", errors="replace"))


def write_temp_lua(script: str) -> str:
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix="slm-modelctl-probe-", suffix=".lua", delete=False
    ) as f:
        f.write(script)
        return f.name


def make_probe_create_script() -> str:
    return """local tid=slm.task_create('ai-probe',function()
while true do
  slm.yield()
end
end)
print(string.format('AI_PROBE_TID %d', tid or -1))
"""


def make_probe_read_script(tid: int) -> str:
    return f"""local tid={tid}
local d=slm.ai_sched_decision(tid)
if d then
  print(string.format('AI_PROBE tid=%d raw=%d core=%d pri=%d preempt=%d',tid,d.raw,d.core,d.priority_adj,d.preempt))
else
  print(string.format('AI_PROBE tid=%d decision=nil',tid))
end
"""


def make_probe_kill_script(tid: int) -> str:
    return f"""local tid={tid}
print(string.format('AI_PROBE_KILL %s', tostring(slm.task_kill(tid))))
"""


def upload_probe_script(args: argparse.Namespace, script: str) -> None:
    local_path = write_temp_lua(script)
    try:
        run_upload(args, local_path, PROBE_REMOTE_PATH)
    finally:
        try:
            os.unlink(local_path)
        except OSError:
            pass


def upload_bytes_via_shell(shell: Shell, remote_path: str, data: bytes, debug: bool,
                           chunk_bytes: int) -> None:
    if not data:
        run_shell_command(shell, f"put {remote_path} 00", debug)
        run_shell_command(shell, f"truncate {remote_path} 0", debug)
        return

    offset = 0
    while offset < len(data):
        chunk_limit = min(chunk_bytes, max(1, (1024 - 1 - 16 - (len("put -a") + 1 + len(remote_path) + 1)) // 2))
        chunk = data[offset : offset + chunk_limit]
        verb = "put" if offset == 0 else "put -a"
        run_shell_command(shell, f"{verb} {remote_path} {chunk.hex()}", debug)
        offset += len(chunk)


def upload_probe_script_via_shell(shell: Shell, args: argparse.Namespace, script: str) -> None:
    ensure_parent_dir(shell, PROBE_REMOTE_PATH, args.debug)
    upload_bytes_via_shell(
        shell,
        PROBE_REMOTE_PATH,
        script.encode("utf-8"),
        args.debug,
        args.chunk_bytes,
    )


def extract_line(text: str, prefix: str) -> str | None:
    for line in text.splitlines():
        if line.startswith(prefix):
            return line
    return None


def infer_probe_policy(domain: str, kind: str, override: str | None) -> str:
    if override is not None:
        return override
    if domain != "sched":
        raise RuntimeError("--probe-raw is only supported for scheduler apply flows")
    if kind == "mlp":
        return "ai_mlp"
    if kind == "ppo":
        return "ai_ppo"
    raise RuntimeError(
        f"--probe-raw is not supported for scheduler kind '{kind}'; "
        "use mlp or ppo, or run `probe` explicitly"
    )


def probe_scheduler(shell: Shell, args: argparse.Namespace) -> int:
    run_shell_command(shell, f"sched policy {args.policy}", args.debug)
    if args.transport == "serial":
        upload_probe_script_via_shell(shell, args, make_probe_create_script())
    else:
        upload_probe_script(args, make_probe_create_script())
    out = run_shell_command(shell, f"lua-admin {PROBE_REMOTE_PATH}", args.debug)
    text = out.decode("utf-8", errors="replace")
    tid_line = extract_line(text, "AI_PROBE_TID ")
    if tid_line is None:
        raise RuntimeError("scheduler probe did not produce an AI_PROBE_TID line")
    tid = int(tid_line.split()[-1])
    if tid <= 0:
        raise RuntimeError(f"scheduler probe returned invalid task id: {tid}")

    # Let the probe task run under the selected policy before sampling.
    run_shell_command(shell, f"sleep {args.sleep_ms}", args.debug)

    if args.transport == "serial":
        upload_probe_script_via_shell(shell, args, make_probe_read_script(tid))
    else:
        upload_probe_script(args, make_probe_read_script(tid))
    out = run_shell_command(shell, f"lua-admin {PROBE_REMOTE_PATH}", args.debug)
    text = out.decode("utf-8", errors="replace")
    probe_line = extract_line(text, "AI_PROBE ")
    if probe_line is None:
        raise RuntimeError("scheduler probe did not produce an AI_PROBE line")

    # Best-effort cleanup. A failed kill is informative but should not hide
    # a successful probe result.
    try:
        if args.transport == "serial":
            upload_probe_script_via_shell(shell, args, make_probe_kill_script(tid))
        else:
            upload_probe_script(args, make_probe_kill_script(tid))
        run_shell_command(shell, f"lua-admin {PROBE_REMOTE_PATH}", args.debug)
    except Exception:
        pass
    try:
        run_shell_command(shell, f"rm {PROBE_REMOTE_PATH}", args.debug)
    except Exception:
        pass

    print(probe_line)
    if args.expect_raw is not None:
        raw_token = next((tok for tok in probe_line.split() if tok.startswith("raw=")), None)
        if raw_token is None:
            raise RuntimeError("scheduler probe did not report raw=")
        raw = int(raw_token.split("=", 1)[1])
        if raw != args.expect_raw:
            raise RuntimeError(
                f"scheduler probe raw mismatch: expected {args.expect_raw}, got {raw}"
            )
    return 0


def open_shell(shell_factory: Callable[[], Shell], debug: bool) -> Shell:
    shell = shell_factory()
    read_initial(shell, debug)
    return shell


def summarize_network_state(output: bytes) -> str:
    text = output.decode("utf-8", errors="replace")
    if "flags=UP" not in text and "UP," not in text:
        return "down"
    if "DHCP(bound)" in text or "STATIC" in text:
        return "ready"
    if "DHCP(pending)" in text:
        return "pending"
    if "DHCP(failed)" in text:
        return "failed"
    return "up"


def run_doctor(shell: Shell, args: argparse.Namespace) -> int:
    rc = 0
    for path in STANDARD_BLOB_DIRS:
        out = shell.run_command(f"stat {path}")
        log_response(args.debug, f"stat {path}", out)
        if shell_command_failed(out):
            print(f"DIR MISSING {path}")
            rc = 1
        else:
            print(f"DIR OK {path}")

    net_out = shell.run_command("ifconfig")
    log_response(args.debug, "ifconfig", net_out)
    if shell_command_failed(net_out):
        print("NETWORK unavailable")
    else:
        print(f"NETWORK {summarize_network_state(net_out)}")
    return rc


def main() -> int:
    args = parse_args()
    remote_path = None
    if args.command in ("apply", "load"):
        source = args.http_url if getattr(args, "http_url", None) is not None else args.local_path
        assert source is not None
        remote_path = validate_operator_blob_path(
            args.remote_path or default_remote_path(source)
        )
    elif args.command == "autoload-set":
        args.remote_path = validate_operator_blob_path(args.remote_path)
    prompt = args.prompt.encode("ascii")
    connect_retries = args.connect_retries
    retry_delay = args.retry_delay

    if args.tryboot:
        connect_retries = max(connect_retries, 12)
        retry_delay = max(retry_delay, 2.0)

    if args.tryboot:
        tryboot_to_slmos(args.target)

    if args.transport == "telnet":
        host = resolve_labctl_target(args.target) if args.labctl else args.target

        def shell_factory() -> Shell:
            return connect_telnet(
                host=host,
                port=args.port,
                prompt=prompt,
                timeout=args.timeout,
                retries=connect_retries,
                retry_delay=retry_delay,
            )
    else:
        def shell_factory() -> Shell:
            return connect_serial(
                target=args.target,
                prompt=prompt,
                timeout=args.timeout,
                retries=connect_retries,
                retry_delay=retry_delay,
            )

    shell: Shell | None = None
    try:
        if (
            args.command in ("load", "apply")
            and args.transport == "serial"
            and getattr(args, "local_path", None) is not None
        ):
            assert remote_path is not None
            shell = open_shell(shell_factory, args.debug)
            ensure_parent_dir(shell, remote_path, args.debug)
            shell.close()
            shell = None
            upload_blob(args, remote_path)
            shell = open_shell(shell_factory, args.debug)
        elif args.command == "probe":
            shell = open_shell(shell_factory, args.debug)
        else:
            shell = open_shell(shell_factory, args.debug)

        if args.command == "status":
            print_output(run_shell_command(shell, f"{args.domain} model status", args.debug))
            return 0

        if args.command == "activate":
            run_shell_command(shell, f"{args.domain} model activate {args.kind}", args.debug)
            print_output(run_shell_command(shell, f"{args.domain} model status", args.debug))
            return 0

        if args.command == "rollback":
            run_shell_command(shell, f"{args.domain} model rollback {args.kind}", args.debug)
            print_output(run_shell_command(shell, f"{args.domain} model status", args.debug))
            return 0

        if args.command == "clear":
            run_shell_command(shell, f"{args.domain} model clear {args.kind}", args.debug)
            print_output(run_shell_command(shell, f"{args.domain} model status", args.debug))
            return 0

        if args.command == "autoload-status":
            print_output(run_shell_command(shell, autoload_command(args.domain, "status"), args.debug))
            return 0

        if args.command == "autoload-set":
            run_shell_command(
                shell,
                autoload_command(args.domain, "set", args.kind, args.remote_path),
                args.debug,
            )
            print_output(run_shell_command(shell, autoload_command(args.domain, "status"), args.debug))
            return 0

        if args.command == "autoload-clear":
            run_shell_command(
                shell,
                autoload_command(args.domain, "clear", args.kind),
                args.debug,
            )
            print_output(run_shell_command(shell, autoload_command(args.domain, "status"), args.debug))
            return 0

        if args.command == "probe":
            return probe_scheduler(shell, args)

        if args.command == "doctor":
            return run_doctor(shell, args)

        assert remote_path is not None
        if getattr(args, "http_url", None) is not None:
            fetch_blob(shell, args, remote_path)
        elif args.transport != "serial":
            ensure_parent_dir(shell, remote_path, args.debug)
            upload_blob(args, remote_path)

        if args.clear_first:
            run_shell_command(shell, f"{args.domain} model clear {args.kind}", args.debug)

        run_shell_command(shell, f"{args.domain} model load {args.kind} {remote_path}", args.debug)
        if args.command == "apply":
            run_shell_command(shell, f"{args.domain} model activate {args.kind}", args.debug)
        if getattr(args, "autoload", False):
            run_shell_command(
                shell,
                autoload_command(args.domain, "set", args.kind, remote_path),
                args.debug,
            )
        status_out = run_shell_command(shell, f"{args.domain} model status", args.debug)
        print_output(status_out)
        if getattr(args, "autoload", False):
            print_output(run_shell_command(shell, autoload_command(args.domain, "status"), args.debug))
        if args.command == "apply" and getattr(args, "probe_raw", None) is not None:
            probe_args = argparse.Namespace(
                policy=infer_probe_policy(args.domain, args.kind, args.probe_policy),
                sleep_ms=args.probe_sleep_ms,
                expect_raw=args.probe_raw,
                debug=args.debug,
                target=args.target,
                transport=args.transport,
                protocol=args.protocol,
                labctl=args.labctl,
                port=args.port,
                prompt=args.prompt,
                timeout=args.timeout,
                connect_retries=args.connect_retries,
                retry_delay=args.retry_delay,
                chunk_bytes=args.chunk_bytes,
                tryboot=False,
            )
            probe_scheduler(shell, probe_args)
        return 0
    finally:
        if shell is not None:
            shell.close()


if __name__ == "__main__":
    sys.exit(main())
