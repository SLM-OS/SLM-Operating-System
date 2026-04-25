#!/usr/bin/env python3
"""
Direct-run functional tests for the runtime blob host tools.

No pytest dependency:
    python3 scripts/tools/test_slm_tooling.py
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import sys
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "scripts/tools"


def _import(name: str):
    path = TOOLS_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    assert spec and spec.loader, f"failed to load {path}"
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


slm_put = _import("slm-put")
slm_modelctl = _import("slm-modelctl")


class TestRunner:
    def __init__(self) -> None:
        self.passes = 0
        self.fails: list[str] = []

    def run(self, name: str, fn) -> None:
        try:
            fn()
            print(f"  PASS  {name}")
            self.passes += 1
        except AssertionError as e:
            print(f"  FAIL  {name}: {e}")
            self.fails.append(name)
        except Exception as e:
            print(f"  ERROR {name}: {type(e).__name__}: {e}")
            self.fails.append(name)

    def summary(self) -> int:
        total = self.passes + len(self.fails)
        print(f"\n{self.passes}/{total} passed")
        if self.fails:
            print("Failures:")
            for name in self.fails:
                print(f"  - {name}")
            return 1
        return 0


@contextlib.contextmanager
def patched_argv(module, argv: list[str]):
    old = module.sys.argv
    module.sys.argv = [old[0]] + argv
    try:
        yield
    finally:
        module.sys.argv = old


class FakeShell:
    def __init__(self, responses: dict[str, list[bytes]]):
        self.responses = {cmd: list(outputs) for cmd, outputs in responses.items()}
        self.commands: list[str] = []
        self.closed = False
        self.initial_output = b"slmos> "

    def run_command(self, command: str) -> bytes:
        self.commands.append(command)
        outputs = self.responses.get(command)
        assert outputs, f"unexpected command: {command}"
        return outputs.pop(0)

    def close(self) -> None:
        self.closed = True


def test_put_chunk_limits_respect_shell_line_budget():
    remote = "/mnt/files/policies/test.bin"
    line_bytes = slm_put.max_legacy_chunk_bytes(remote, 0)
    cmd = f"put {remote} " + ("ab" * line_bytes)
    assert len(cmd) + 1 + slm_put.SHELL_LINE_HEADROOM <= slm_put.SHELL_MAX_LINE

    framed_bytes = slm_put.max_framed_chunk_bytes(1234)
    framed_cmd = f"xput chunk 1234 " + ("ab" * framed_bytes)
    assert len(framed_cmd) + 1 + slm_put.SHELL_LINE_HEADROOM <= slm_put.SHELL_MAX_LINE


def test_put_upload_framed_resumes_and_finishes():
    responses = {
        "xput status": [b"XPUT active path=/tmp/blob size=20 received=10 checksum=797261938\nslmos> "],
        "xput chunk 10 0a0b0c0d0e0f10111213": [b"XPUT ok next=20\nslmos> "],
        "xput finish": [b"XPUT complete path=/tmp/blob size=20\nslmos> "],
        "stat /tmp/blob": [b"  Size: 20 bytes\nslmos> "],
    }
    shell = FakeShell(responses)
    args = argparse.Namespace(
        debug=False,
        remote_path="/tmp/blob",
        transport="telnet",
        no_resume=False,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )
    data = bytes(range(20))

    rc = slm_put.upload_framed(shell, args, data, len(data), "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "xput status",
        "xput chunk 10 0a0b0c0d0e0f10111213",
        "xput finish",
        "stat /tmp/blob",
    ]


def test_put_framed_resume_requires_exact_path_match():
    responses = {
        "xput status": [b"XPUT active path=/tmp/blob.bak size=20 received=10 checksum=797261938\nslmos> "],
        "xput begin /tmp/blob 20": [b"XPUT begin path=/tmp/blob size=20\nslmos> "],
        "xput chunk 0 000102030405060708090a0b0c0d0e0f10111213": [b"XPUT ok next=20\nslmos> "],
        "xput finish": [b"XPUT complete path=/tmp/blob size=20\nslmos> "],
        "stat /tmp/blob": [b"Size: 20 bytes\nslmos> "],
    }
    shell = FakeShell(responses)
    args = argparse.Namespace(
        debug=False,
        remote_path="/tmp/blob",
        transport="telnet",
        no_resume=False,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )
    data = bytes(range(20))

    rc = slm_put.upload_framed(shell, args, data, len(data), "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "xput status",
        "xput begin /tmp/blob 20",
        "xput chunk 0 000102030405060708090a0b0c0d0e0f10111213",
        "xput finish",
        "stat /tmp/blob",
    ]


def test_put_framed_resume_requires_matching_prefix_checksum():
    responses = {
        "xput status": [b"XPUT active path=/tmp/blob size=20 received=10 checksum=12345\nslmos> "],
        "xput begin /tmp/blob 20": [b"XPUT begin path=/tmp/blob size=20\nslmos> "],
        "xput chunk 0 000102030405060708090a0b0c0d0e0f10111213": [b"XPUT ok next=20\nslmos> "],
        "xput finish": [b"XPUT complete path=/tmp/blob size=20\nslmos> "],
        "stat /tmp/blob": [b"Size: 20 bytes\nslmos> "],
    }
    shell = FakeShell(responses)
    args = argparse.Namespace(
        debug=False,
        remote_path="/tmp/blob",
        transport="telnet",
        no_resume=False,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )
    data = bytes(range(20))

    rc = slm_put.upload_framed(shell, args, data, len(data), "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "xput status",
        "xput begin /tmp/blob 20",
        "xput chunk 0 000102030405060708090a0b0c0d0e0f10111213",
        "xput finish",
        "stat /tmp/blob",
    ]


def test_put_main_auto_falls_back_to_legacy_when_xput_missing():
    legacy_calls: list[tuple[str, int]] = []

    class MainShell(FakeShell):
        def read_until_prompt(self) -> bytes:
            return b"slmos> "

    shell = MainShell({"xput status": [b"Unknown command: xput\nslmos> "]})

    def fake_upload_legacy(shell_obj, args, data, total, target_desc, shell_factory):
        legacy_calls.append((target_desc, total))
        assert shell_obj is shell
        assert data == b"abc"
        return 0

    with mock.patch.object(slm_put.pathlib.Path, "read_bytes", return_value=b"abc"):
        with mock.patch.object(slm_put, "connect_telnet", return_value=shell):
            with mock.patch.object(slm_put, "upload_legacy", side_effect=fake_upload_legacy):
                with patched_argv(
                    slm_put,
                    ["127.0.0.1", "/tmp/local.bin", "/tmp/remote.bin"],
                ):
                    rc = slm_put.main()

    assert rc == 0
    assert legacy_calls == [("127.0.0.1", 3)]


def test_put_main_preserves_relative_remote_path():
    class MainShell(FakeShell):
        def read_until_prompt(self) -> bytes:
            return b"slmos> "

    shell = MainShell({"xput status": [b"Unknown command: xput\nslmos> "]})
    seen_remote_paths: list[str] = []

    def fake_upload_legacy(shell_obj, args, data, total, target_desc, shell_factory):
        assert shell_obj is shell
        seen_remote_paths.append(args.remote_path)
        assert data == b"abc"
        return 0

    with mock.patch.object(slm_put.pathlib.Path, "read_bytes", return_value=b"abc"):
        with mock.patch.object(slm_put, "connect_telnet", return_value=shell):
            with mock.patch.object(slm_put, "upload_legacy", side_effect=fake_upload_legacy):
                with patched_argv(
                    slm_put,
                    ["127.0.0.1", "/tmp/local.bin", "blob.bin"],
                ):
                    rc = slm_put.main()

    assert rc == 0
    assert seen_remote_paths == ["blob.bin"]


def test_put_main_serial_labctl_does_not_resolve_network():
    shell = FakeShell({"xput status": [b"Unknown command: xput\nslmos> "]})

    with mock.patch.object(slm_put.pathlib.Path, "read_bytes", return_value=b"abc"):
        with mock.patch.object(slm_put, "resolve_labctl_target", side_effect=AssertionError("should not resolve network")):
            with mock.patch.object(slm_put, "connect_serial", return_value=shell):
                with mock.patch.object(slm_put, "upload_legacy", return_value=0) as upload_legacy:
                    with patched_argv(
                        slm_put,
                        ["--transport", "serial", "--labctl", "pi-5-2", "/tmp/local.bin", "/tmp/remote.bin"],
                    ):
                        rc = slm_put.main()

    assert rc == 0
    upload_legacy.assert_called_once()


def test_put_legacy_no_resume_skips_truncate_for_absent_destination():
    shell = FakeShell(
        {
            "stat /tmp/blob": [
                b"Command returned error: No such file or directory\nslmos> ",
                b"Size: 3 bytes\nslmos> ",
            ],
            "put /tmp/blob 616263": [b"ok\nslmos> "],
        }
    )
    args = argparse.Namespace(
        debug=False,
        remote_path="/tmp/blob",
        transport="telnet",
        no_resume=True,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )

    rc = slm_put.upload_legacy(shell, args, b"abc", 3, "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "stat /tmp/blob",
        "put /tmp/blob 616263",
        "stat /tmp/blob",
    ]


def test_put_legacy_resume_restarts_when_destination_is_nonempty():
    shell = FakeShell(
        {
            "stat /tmp/blob": [
                b"Size: 2 bytes\nslmos> ",
                b"Size: 3 bytes\nslmos> ",
            ],
            "truncate /tmp/blob 0": [b"ok\nslmos> "],
            "put /tmp/blob 616263": [b"ok\nslmos> "],
        }
    )
    args = argparse.Namespace(
        debug=False,
        remote_path="/tmp/blob",
        transport="telnet",
        no_resume=False,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )

    rc = slm_put.upload_legacy(shell, args, b"abc", 3, "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "stat /tmp/blob",
        "truncate /tmp/blob 0",
        "put /tmp/blob 616263",
        "stat /tmp/blob",
    ]


def test_put_framed_resume_normalizes_relative_remote_path():
    responses = {
        "xput status": [b"XPUT active path=/tmp/blob size=20 received=10 checksum=797261938\nslmos> "],
        "xput chunk 10 0a0b0c0d0e0f10111213": [b"XPUT ok next=20\nslmos> "],
        "xput finish": [b"XPUT complete path=/tmp/blob size=20\nslmos> "],
        "stat tmp/blob": [b"Size: 20 bytes\nslmos> "],
    }
    shell = FakeShell(responses)
    args = argparse.Namespace(
        debug=False,
        remote_path="tmp/blob",
        transport="telnet",
        no_resume=False,
        no_verify_size=False,
        chunk_bytes=64,
        chunk_retries=2,
        retry_delay=0.0,
    )
    data = bytes(range(20))

    rc = slm_put.upload_framed(shell, args, data, len(data), "pi-5-2", lambda: shell)

    assert rc == 0
    assert shell.commands == [
        "xput status",
        "xput chunk 10 0a0b0c0d0e0f10111213",
        "xput finish",
        "stat tmp/blob",
    ]


def test_modelctl_parse_args_supports_legacy_apply_form():
    with patched_argv(
        slm_modelctl,
        ["--target", "pi-5-2", "sched", "mlp", "local.blob"],
    ):
        args = slm_modelctl.parse_args()

    assert args.command == "apply"
    assert args.target == "pi-5-2"
    assert args.domain == "sched"
    assert args.kind == "mlp"
    assert args.local_path == "local.blob"


def test_modelctl_parse_args_accepts_http_source():
    with patched_argv(
        slm_modelctl,
        ["load", "--target", "pi-5-2", "--http-url", "http://10.0.2.2/blob.bin", "sched", "mlp"],
    ):
        args = slm_modelctl.parse_args()

    assert args.command == "load"
    assert args.target == "pi-5-2"
    assert args.http_url == "http://10.0.2.2/blob.bin"
    assert args.local_path is None


def test_modelctl_parse_args_rejects_sha256_without_http_url():
    with patched_argv(
        slm_modelctl,
        ["load", "--target", "pi-5-2", "--sha256", "abcd", "sched", "mlp", "local.blob"],
    ):
        try:
            slm_modelctl.parse_args()
        except SystemExit as e:
            assert e.code != 0
        else:
            assert False, "expected parse_args to reject --sha256 without --http-url"


def test_modelctl_parse_args_reorders_global_options_before_subcommand():
    with patched_argv(
        slm_modelctl,
        ["--target", "pi-5-2", "--labctl", "clear", "sched", "mlp"],
    ):
        args = slm_modelctl.parse_args()

    assert args.command == "clear"
    assert args.target == "pi-5-2"
    assert args.labctl is True
    assert args.domain == "sched"
    assert args.kind == "mlp"


def test_modelctl_legacy_path_named_like_subcommand_stays_positional():
    with patched_argv(
        slm_modelctl,
        ["--target", "pi-5-2", "sched", "mlp", "status", "/mnt/files/policies/load"],
    ):
        args = slm_modelctl.parse_args()

    assert args.command == "apply"
    assert args.local_path == "status"
    assert args.remote_path == "/mnt/files/policies/load"


def test_modelctl_infer_probe_policy_maps_scheduler_kinds():
    assert slm_modelctl.infer_probe_policy("sched", "mlp", None) == "ai_mlp"
    assert slm_modelctl.infer_probe_policy("sched", "ppo", None) == "ai_ppo"
    assert slm_modelctl.infer_probe_policy("sched", "mlp", "ai_ppo") == "ai_ppo"

    try:
        slm_modelctl.infer_probe_policy("eviction", "mlp", None)
    except RuntimeError as e:
        assert "scheduler apply flows" in str(e)
    else:
        assert False, "expected RuntimeError for non-scheduler probe inference"


def test_modelctl_probe_scheduler_runs_create_sample_and_cleanup():
    shell = FakeShell(
        {
            "sched policy ai_mlp": [b"policy set\nslmos> "],
            f"lua-admin {slm_modelctl.PROBE_REMOTE_PATH}": [
                b"AI_PROBE_TID 23\nslmos> ",
                b"AI_PROBE tid=23 raw=7 core=1 pri=0 preempt=1\nslmos> ",
                b"AI_PROBE_KILL true\nslmos> ",
            ],
            "sleep 50": [b"ok\nslmos> "],
            f"rm {slm_modelctl.PROBE_REMOTE_PATH}": [b"ok\nslmos> "],
        }
    )
    uploads: list[str] = []
    args = argparse.Namespace(
        policy="ai_mlp",
        sleep_ms=50,
        expect_raw=7,
        debug=False,
        target="pi-5-2",
        transport="telnet",
        protocol="auto",
        labctl=True,
        port=2323,
        prompt="slmos> ",
        timeout=10.0,
        connect_retries=1,
        retry_delay=0.0,
        chunk_bytes=128,
        tryboot=False,
    )

    with mock.patch.object(slm_modelctl, "upload_probe_script", side_effect=lambda _a, script: uploads.append(script)):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            rc = slm_modelctl.probe_scheduler(shell, args)

    assert rc == 0
    assert len(uploads) == 3
    assert "AI_PROBE tid=23 raw=7 core=1 pri=0 preempt=1" in stdout.getvalue()
    assert shell.commands == [
        "sched policy ai_mlp",
        f"lua-admin {slm_modelctl.PROBE_REMOTE_PATH}",
        "sleep 50",
        f"lua-admin {slm_modelctl.PROBE_REMOTE_PATH}",
        f"lua-admin {slm_modelctl.PROBE_REMOTE_PATH}",
        f"rm {slm_modelctl.PROBE_REMOTE_PATH}",
    ]


def test_modelctl_probe_scheduler_serial_reuses_existing_shell():
    shell = FakeShell(
        {
            "sched policy ai_mlp": [b"policy set\nslmos> "],
            f"lua-admin {slm_modelctl.PROBE_REMOTE_PATH}": [
                b"AI_PROBE_TID 23\nslmos> ",
                b"AI_PROBE tid=23 raw=7 core=1 pri=0 preempt=1\nslmos> ",
                b"AI_PROBE_KILL true\nslmos> ",
            ],
            "sleep 50": [b"ok\nslmos> "],
            f"rm {slm_modelctl.PROBE_REMOTE_PATH}": [b"ok\nslmos> "],
        }
    )
    args = argparse.Namespace(
        policy="ai_mlp",
        sleep_ms=50,
        expect_raw=7,
        debug=False,
        target="pi-5-2",
        transport="serial",
        protocol="auto",
        labctl=True,
        port=2323,
        prompt="slmos> ",
        timeout=10.0,
        connect_retries=1,
        retry_delay=0.0,
        chunk_bytes=512,
        tryboot=False,
    )

    uploads: list[str] = []
    with mock.patch.object(slm_modelctl, "upload_probe_script", side_effect=AssertionError("should not spawn slm-put over serial")):
        with mock.patch.object(
            slm_modelctl,
            "upload_probe_script_via_shell",
            side_effect=lambda _shell, _args, script: uploads.append(script),
        ):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                rc = slm_modelctl.probe_scheduler(shell, args)

    assert rc == 0
    assert len(uploads) == 3
    assert "AI_PROBE tid=23 raw=7 core=1 pri=0 preempt=1" in stdout.getvalue()


def test_modelctl_apply_probe_raw_invokes_probe_with_inferred_policy():
    shell = FakeShell(
        {
            "sched model clear mlp": [b"cleared\nslmos> "],
            "sched model load mlp /mnt/files/policies/probe.blob": [b"loaded\nslmos> "],
            "sched model activate mlp": [b"activated\nslmos> "],
            "sched model status": [b"mlp: active\nslmos> "],
        }
    )
    probes: list[tuple[str, int]] = []

    with mock.patch.object(slm_modelctl, "open_shell", return_value=shell):
        with mock.patch.object(slm_modelctl, "ensure_parent_dir") as ensure_parent_dir:
            with mock.patch.object(slm_modelctl, "upload_blob") as upload_blob:
                with mock.patch.object(
                    slm_modelctl,
                    "probe_scheduler",
                    side_effect=lambda _shell, probe_args: probes.append((probe_args.policy, probe_args.expect_raw)) or 0,
                ):
                    with patched_argv(
                        slm_modelctl,
                        [
                            "apply",
                            "--target",
                            "pi-5-2",
                            "--clear-first",
                            "--probe-raw",
                            "7",
                            "sched",
                            "mlp",
                            "local.blob",
                            "/mnt/files/policies/probe.blob",
                        ],
                    ):
                        stdout = io.StringIO()
                        with contextlib.redirect_stdout(stdout):
                            rc = slm_modelctl.main()

    assert rc == 0
    ensure_parent_dir.assert_called_once()
    upload_blob.assert_called_once()
    assert "mlp: active" in stdout.getvalue()
    assert probes == [("ai_mlp", 7)]


def test_modelctl_apply_http_url_fetches_on_device():
    shell = FakeShell(
        {
            "mkdir /mnt/files/policies": [b"Already exists\nslmos> "],
            "http get http://10.0.2.2/models/blob.bin /mnt/files/policies/blob.bin 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef": [b"downloaded\nslmos> "],
            "eviction model clear xgboost": [b"cleared\nslmos> "],
            "eviction model load xgboost /mnt/files/policies/blob.bin": [b"loaded\nslmos> "],
            "eviction model activate xgboost": [b"activated\nslmos> "],
            "eviction model status": [b"xgboost: active\nslmos> "],
        }
    )

    with mock.patch.object(slm_modelctl, "open_shell", return_value=shell):
        with mock.patch.object(slm_modelctl, "run_upload", side_effect=AssertionError("should not upload when --http-url is used")):
            with patched_argv(
                slm_modelctl,
                [
                    "apply",
                    "--target",
                    "pi-5-2",
                    "--http-url",
                    "http://10.0.2.2/models/blob.bin",
                    "--sha256",
                    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                    "--clear-first",
                    "eviction",
                    "xgboost",
                ],
            ):
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    rc = slm_modelctl.main()

    assert rc == 0
    assert "xgboost: active" in stdout.getvalue()
    assert shell.commands == [
        "mkdir /mnt/files/policies",
        "http get http://10.0.2.2/models/blob.bin /mnt/files/policies/blob.bin 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "eviction model clear xgboost",
        "eviction model load xgboost /mnt/files/policies/blob.bin",
        "eviction model activate xgboost",
        "eviction model status",
    ]


def test_modelctl_run_upload_uses_tool_dir_not_cwd():
    args = argparse.Namespace(
        protocol="auto",
        transport="telnet",
        port=2323,
        prompt="slmos> ",
        timeout=10.0,
        connect_retries=1,
        retry_delay=0.0,
        chunk_bytes=128,
        labctl=True,
        debug=False,
        target="pi-5-2",
    )

    with mock.patch.object(slm_modelctl.subprocess, "run") as run:
        slm_modelctl.run_upload(args, "local.blob", "/mnt/files/blob")

    cmd = run.call_args.args[0]
    assert cmd[0] == "python3"
    assert Path(cmd[1]).resolve() == (TOOLS_DIR / "slm-put.py").resolve()
    assert cmd[-3:] == ["pi-5-2", "local.blob", "/mnt/files/blob"]


def main() -> int:
    runner = TestRunner()
    runner.run("put_chunk_limits_respect_shell_line_budget", test_put_chunk_limits_respect_shell_line_budget)
    runner.run("put_upload_framed_resumes_and_finishes", test_put_upload_framed_resumes_and_finishes)
    runner.run("put_framed_resume_requires_exact_path_match", test_put_framed_resume_requires_exact_path_match)
    runner.run("put_framed_resume_requires_matching_prefix_checksum", test_put_framed_resume_requires_matching_prefix_checksum)
    runner.run("put_main_auto_falls_back_to_legacy_when_xput_missing", test_put_main_auto_falls_back_to_legacy_when_xput_missing)
    runner.run("put_main_preserves_relative_remote_path", test_put_main_preserves_relative_remote_path)
    runner.run("put_main_serial_labctl_does_not_resolve_network", test_put_main_serial_labctl_does_not_resolve_network)
    runner.run("put_legacy_no_resume_skips_truncate_for_absent_destination", test_put_legacy_no_resume_skips_truncate_for_absent_destination)
    runner.run("put_legacy_resume_restarts_when_destination_is_nonempty", test_put_legacy_resume_restarts_when_destination_is_nonempty)
    runner.run("put_framed_resume_normalizes_relative_remote_path", test_put_framed_resume_normalizes_relative_remote_path)
    runner.run("modelctl_parse_args_supports_legacy_apply_form", test_modelctl_parse_args_supports_legacy_apply_form)
    runner.run("modelctl_parse_args_accepts_http_source", test_modelctl_parse_args_accepts_http_source)
    runner.run("modelctl_parse_args_rejects_sha256_without_http_url", test_modelctl_parse_args_rejects_sha256_without_http_url)
    runner.run("modelctl_parse_args_reorders_global_options_before_subcommand", test_modelctl_parse_args_reorders_global_options_before_subcommand)
    runner.run("modelctl_legacy_path_named_like_subcommand_stays_positional", test_modelctl_legacy_path_named_like_subcommand_stays_positional)
    runner.run("modelctl_infer_probe_policy_maps_scheduler_kinds", test_modelctl_infer_probe_policy_maps_scheduler_kinds)
    runner.run("modelctl_probe_scheduler_runs_create_sample_and_cleanup", test_modelctl_probe_scheduler_runs_create_sample_and_cleanup)
    runner.run("modelctl_probe_scheduler_serial_reuses_existing_shell", test_modelctl_probe_scheduler_serial_reuses_existing_shell)
    runner.run("modelctl_apply_probe_raw_invokes_probe_with_inferred_policy", test_modelctl_apply_probe_raw_invokes_probe_with_inferred_policy)
    runner.run("modelctl_apply_http_url_fetches_on_device", test_modelctl_apply_http_url_fetches_on_device)
    runner.run("modelctl_run_upload_uses_tool_dir_not_cwd", test_modelctl_run_upload_uses_tool_dir_not_cwd)
    return runner.summary()


if __name__ == "__main__":
    sys.exit(main())
