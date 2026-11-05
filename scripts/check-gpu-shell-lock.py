#!/usr/bin/env python3
"""Keep shell verbs that use the GPU channel under the dispatch lock (#1111).

These verbs race other GPU consumers if they submit work or touch the shared cbuf
and scratch slots without g_gpu_dispatch_lock, and nothing at compile or QEMU test
time shows it: the race needs real hardware with a consumer running.

In kernel/src/shell_sys.c, each function in CHANNEL_FUNCTIONS may only be called
from a function named *_locked_cb, and each of those must be passed to
slm_gpu_call_locked.

Exit status: 0 clean, 1 violations, 2 usage. Tested by
scripts/tests/test-check-gpu-shell-lock.sh.
"""
import re
import sys
from pathlib import Path

CHANNEL_FUNCTIONS = (
    "slm_oplib_dispatch",
    "slm_oplib_prepare_dispatch",
    "ga10b_bringup_launch_kernel",
    "ga10b_bringup_read_pipeline_output",
    "ga10b_bringup_smoke_test",
    "ga10b_bringup_smoke_test_compute",
    "ga10b_ce_memcpy",
    "ga10b_submit_and_poll",
    "ga10b_dispatch_v7_pipeline_inline",
    "ga10b_bringup_set_input",
    "ga10b_bringup_set_input_fill",
)
CALL = re.compile(r"\b(" + "|".join(CHANNEL_FUNCTIONS) + r")\s*\(")
# A column-0 line up to its first "(": a function's return type and name, or
# just its name when the type is on the line above.
DEFINITION = re.compile(r"^(?:[A-Za-z_][\w\s\*]*[\s\*])?([A-Za-z_]\w*)\s*\(")


def strip_noise(text):
    """Blank comments and strings, keeping line numbers."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match[0])
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"', blank, text,
                  flags=re.S)


def check(path):
    source = strip_noise(Path(path).read_text(errors="replace"))
    problems = []
    current = None
    callbacks = set()
    for number, line in enumerate(source.splitlines(), 1):
        definition = DEFINITION.match(line)
        if definition and not line.startswith("typedef"):
            current = definition[1]
            if current.endswith("_locked_cb") and not line.rstrip().endswith(";"):
                callbacks.add(current)
        for call in CALL.finditer(line):
            if current is None or not current.endswith("_locked_cb"):
                problems.append(f"{path}:{number}: {call[1]} called from "
                                f"{current}, not a *_locked_cb")
    for name in sorted(callbacks):
        if not re.search(r"\bslm_gpu_call_locked\s*\(\s*" + name + r"\s*,", source):
            problems.append(f"{path}: {name} isn't run by slm_gpu_call_locked")
    return problems


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "kernel/src/shell_sys.c"
    if not Path(path).is_file():
        print(f"check-gpu-shell-lock: {path} not found", file=sys.stderr)
        return 2
    problems = check(path)
    for problem in problems:
        print(f"check-gpu-shell-lock: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
