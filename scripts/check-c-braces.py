#!/usr/bin/env python3
"""Check C control-flow braces on changed lines with clang-tidy.

Runs the .clang-tidy brace rule over every compile database, because code
under #ifdef PLATFORM_* is only visible to the platform that enables it.
Findings are limited to lines changed since the merge base with --base,
including uncommitted and untracked files; --all reports the whole tree, which
is clean, so any finding there is a new regression. Vendored kernel/lib/ and
nanopb output are exempt.

Only code the databases compile is seen, so `make check-braces` adds a wide
configuration per platform with default-off features on. Blocks that are off even
there aren't checked, and neither is code needing both a wide-only option and
ENABLE_BOOT_TESTS, since the wide configuration builds the main kernel. The rule also skips statements that come from a macro
expansion, so macro bodies aren't checked. A changed file that no
parse reached (host tools, for example) is listed as not checked. A file that
clang-tidy gives up on in any database fails the run rather than passing
silently, because that database's #ifdef blocks would otherwise go unchecked: a
missing header, a rejected flag or a crash. An undeclared name or syntax error
on a changed line fails it too, since clang discards that statement. Other
compile errors are tolerated (GCC's system headers cause most), since clang
still analyzes the code around them.

Exit status: 0 clean, 1 violations, 2 could not check.

Used by `make check-braces`; scripts/tests/test-check-c-braces.sh covers it.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

C_SUFFIXES = (".c", ".h")
# Generated during a build. The check only configures, so stand-ins keep clang
# parsing. They define every macro the real headers do, because an undeclared
# name on a changed line fails the run and nothing in the changed file could fix
# that. The values don't matter for brace analysis: a static_assert that fails
# on a weight count is a tolerated error.
WEIGHT_MACROS = [f"GA10B_EVICT_MLP_{kind}_{layer}{suffix}"
                 for layer in ("L1", "L2", "L3", "OUT")
                 for kind in ("W", "B")
                 for suffix in ("", "_COUNT")]
GENERATED_HEADERS = {
    "build_info.h": ('#define SLMOS_VERSION ""\n'
                     '#define SLMOS_BUILD_STAMP ""\n'
                     '#define SLMOS_BUILD_SHA ""\n'),
    "ga10b_eviction_mlp_weights_gen.h": "".join(
        f"#define {name} 0\n" for name in WEIGHT_MACROS),
}
RULE = "readability-braces-around-statements"
DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*)$")
# With -H, clang prints each opened header as dots, a space and the path.
INCLUDED = re.compile(r"^\.+ (.+)$")


def git(*args):
    return subprocess.run(["git", "-c", "core.quotePath=false", *args],
                          check=True, capture_output=True,
                          text=True, encoding="utf-8",
                          errors="replace").stdout


def exempt(path):
    return path.startswith("kernel/lib/") or path.endswith((".pb.c", ".pb.h"))


def unescape(match):
    """Decode one C-style escape from a path git quoted."""
    if len(match[1]) == 3:
        return chr(int(match[1], 8))
    names = {"a": "\a", "b": "\b", "t": "\t", "n": "\n", "v": "\v", "f": "\f",
             "r": "\r"}
    return names.get(match[1], match[1])


def diff_path(header):
    """Return the path from a "+++ b/path" line, or None for /dev/null.

    Git appends a tab to a path containing whitespace, and quotes one containing
    a backslash, a double quote or a control character.
    """
    text = header[4:]
    if text.startswith('"') and text.endswith('"'):
        text = re.sub(r"\\([0-7]{3}|.)", unescape, text[1:-1])
    else:
        text = text.rstrip("\t")
    return text[2:] if text.startswith("b/") else None


def changed_lines(base):
    """Map each changed C path to the set of its added or modified lines."""
    merge_base = git("merge-base", base, "HEAD").strip()
    changed = collections.defaultdict(set)
    path = None
    in_header = False
    for line in git("diff", "-U0", "--no-color", "--no-ext-diff", "--src-prefix=a/",
                    "--dst-prefix=b/", merge_base).splitlines():
        # Only a file header counts: an added line of C starting with "++ " also
        # appears as "+++ ".
        if line.startswith("diff --git "):
            in_header = True
            path = None
        elif in_header and line.startswith("+++ "):
            path = diff_path(line)
        elif line.startswith("@@"):
            in_header = False
            if not path:
                continue
            match = re.search(r"\+(\d+)(?:,(\d+))? @@", line)
            start, count = int(match[1]), int(match[2] or 1)
            changed[path].update(range(start, start + count))
    for path in git("ls-files", "-z", "--others",
                    "--exclude-standard").split("\0"):
        if path.endswith(C_SUFFIXES):
            count = len(Path(path).read_text(errors="replace").splitlines())
            changed[path].update(range(1, count + 1))
    return {path: lines for path, lines in changed.items()
            if path.endswith(C_SUFFIXES) and not exempt(path)}


def build_files_changed(base):
    """True if the diff touches what decides which options reach the compiler."""
    merge_base = git("merge-base", base, "HEAD").strip()
    names = git("diff", "--name-only", "--no-renames", merge_base).splitlines()
    return any(name in ("Makefile", "CMakeLists.txt") or name.startswith("cmake/")
               for name in names)


def compiler_args(compiler):
    """Clang needs the cross compiler's target and system include paths."""
    machine = subprocess.run([compiler, "-dumpmachine"], capture_output=True,
                             text=True).stdout.strip()
    args = ["--extra-arg-before=--target=" + machine]
    if machine.startswith("aarch64"):
        # Clang may not know the -mcpu GCC was given (cortex-a78ae on Jetson),
        # and the CPU has no bearing on brace analysis.
        args.append("--extra-arg=-mcpu=generic")
    result = subprocess.run([compiler, "-E", "-Wp,-v", "-xc", os.devnull],
                            capture_output=True, text=True)
    in_list = False
    for line in result.stderr.splitlines():
        if line.startswith("#include <...>"):
            in_list = True
        elif line.startswith("End of search list"):
            break
        elif in_list:
            args.append("--extra-arg=-isystem" + line.strip())
    return args


def load_database(db_dir, root):
    """Return (extra clang-tidy args, repo-relative C files) for a database."""
    entries = json.loads((db_dir / "compile_commands.json").read_text())
    stubs = db_dir / "tidy-stubs"
    stubs.mkdir(exist_ok=True)
    for header, content in GENERATED_HEADERS.items():
        (stubs / header).write_text(content)
    args = ["--extra-arg=-w", "--extra-arg=-ferror-limit=0", "--extra-arg=-H",
            "--extra-arg=-I" + str(stubs)]
    files = []
    compiler = None
    for entry in entries:
        command = entry.get("arguments") or shlex.split(entry["command"])
        compiler = compiler or command[0]
        path = Path(entry["directory"], entry["file"]).resolve()
        if path.suffix == ".c" and path.is_relative_to(root):
            relative = str(path.relative_to(root))
            if not exempt(relative):
                files.append(relative)
    return args + compiler_args(compiler), files


def unused_defines(required):
    """Return the macros no tracked C file mentions, such as after a rename."""
    return [macro for macro in sorted(set(required))
            if subprocess.run(["git", "grep", "-qw", macro, "--", "*.c", "*.h"],
                              capture_output=True).returncode != 0]


def missing_defines(db_dir, required):
    """Return the macros in required that no compile command in db_dir defines."""
    commands = " ".join(
        entry.get("command") or " ".join(entry.get("arguments", []))
        for entry in json.loads((db_dir / "compile_commands.json").read_text()))
    return [macro for macro in required
            if not re.search(rf"-D{re.escape(macro)}(=|\s|$)", commands)]


def run_tidy(db_dir, args, config, path):
    result = subprocess.run(
        ["clang-tidy", "-p", str(db_dir), "--quiet", "--config-file=" + config,
         "--header-filter=.*", *args, path],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    return db_dir, path, result.returncode, result.stdout + result.stderr


def parse_output(output, status, root, changed, report_all):
    """Read one clang-tidy run: (failure reason or None, findings, headers)."""
    fatal = None
    located_error = False
    findings = {}
    headers = set()
    for line in output.splitlines():
        match = INCLUDED.match(line)
        if match:
            header = Path(match[1]).resolve()
            if header.is_relative_to(root):
                headers.add(str(header.relative_to(root)))
            continue
        if line.startswith("error: "):
            # No location: clang-tidy gave up on the whole file.
            fatal = line
            continue
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        if match[4] == "error":
            located_error = True
        if match[4] == "error" and "file not found" in match[5]:
            fatal = f"{match[1]}:{match[2]}: {match[5]}"
            continue
        location = Path(match[1]).resolve()
        if not location.is_relative_to(root):
            continue
        relative = str(location.relative_to(root))
        number = int(match[2])
        lines = changed.get(relative, ())
        # An unknown macro beside a string literal reads as a syntax error
        # ("expected ')'"), not as an undeclared name.
        if match[4] == "error" and ("undeclared" in match[5]
                                    or match[5].startswith("expected")):
            if number in lines:
                fatal = f"{relative}:{number}: {match[5]}"
        # Either severity: WarningsAsErrors turns findings into errors.
        elif RULE in match[5]:
            # The finding sits on the line before the unbraced body, so an edit
            # to that body counts too.
            if not exempt(relative) and (report_all or number in lines
                                         or number + 1 in lines):
                findings[(relative, number)] = match[5]
    # Status 1 with a compile error is a tolerated error, not a failure.
    if status < 0 or status >= 128 or (status and not located_error):
        fatal = fatal or f"clang-tidy exit status {status}"
    return fatal, findings, headers


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--db", action="append", required=True, type=Path,
                        help="compile database directory; repeat per platform")
    parser.add_argument("--base", default="origin/main",
                        help="ref to diff against (default: origin/main)")
    parser.add_argument("--all", action="store_true",
                        help="report every violation, not just changed lines")
    parser.add_argument("--require-define", action="append", default=[],
                        metavar="DB=MACRO",
                        help="fail unless database DB (its directory name) "
                             "defines MACRO; repeatable")
    parser.add_argument("--make-db", action="store_true",
                        help="generate databases with `make tidy-db` first; "
                             "each directory is named PLATFORM, "
                             "PLATFORM-test or PLATFORM-wide")
    options = parser.parse_args()

    if not shutil.which("clang-tidy"):
        print("check-braces: clang-tidy not found; install it to run this "
              "check", file=sys.stderr)
        return 2
    root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
    os.chdir(root)
    config = str(root / ".clang-tidy")
    # A typo or bad YAML in the config otherwise disables the rule silently.
    listed = subprocess.run(["clang-tidy", "--list-checks",
                             "--config-file=" + config],
                            capture_output=True, text=True)
    if RULE not in listed.stdout:
        print(f"check-braces: {RULE} isn't enabled by .clang-tidy\n"
              f"{listed.stderr}", file=sys.stderr)
        return 2

    changed = {}
    if not options.all:
        try:
            changed = changed_lines(options.base)
        except subprocess.CalledProcessError as error:
            print(f"check-braces: can't diff against {options.base}: "
                  f"{error.stderr.strip()}", file=sys.stderr)
            return 2
        # A build change can drop an option even with no C change, so the required
        # defines still get checked below. Otherwise skip the configure step.
        if not changed and not (options.require_define
                                and build_files_changed(options.base)):
            print("check-braces: no changed C lines")
            return 0

    if options.make_db:
        for db_dir in options.db:
            platform, _, variant = db_dir.name.partition("-")
            result = subprocess.run(
                ["make", "--no-print-directory", "tidy-db",
                 "PLATFORM=" + platform, "TIDY_VARIANT=" + (variant or "kernel")],
                capture_output=True, text=True)
            if result.returncode:
                print(f"check-braces: configuring {db_dir.name} failed "
                      f"(is its cross compiler on PATH?)\n{result.stdout}"
                      f"{result.stderr}", file=sys.stderr)
                return 2

    # A changed header can affect any file, so only a pure .c change narrows
    # the list of files to run.
    headers_changed = options.all or any(p.endswith(".h") for p in changed)
    required = collections.defaultdict(list)
    for item in options.require_define:
        name, _, macro = item.partition("=")
        required[name].append(macro)
    for db_dir in options.db:
        for macro in missing_defines(db_dir, required[db_dir.name]):
            print(f"check-braces: {db_dir.name} doesn't define {macro}, so code "
                  "under it wouldn't be checked", file=sys.stderr)
            return 2
    for macro in unused_defines(m for ms in required.values() for m in ms):
        print(f"check-braces: no C file mentions {macro}; update the required "
              "defines in the Makefile", file=sys.stderr)
        return 2
    if not options.all and not changed:
        print("check-braces: no changed C lines")
        return 0

    jobs = []
    for db_dir in options.db:
        args, files = load_database(db_dir.resolve(), root)
        for path in sorted(set(files)):
            if headers_changed or path in changed:
                jobs.append((db_dir.resolve(), args, config, path))

    findings = {}
    reached = set()
    parses = collections.Counter()
    failed = collections.defaultdict(set)
    workers = min(8, os.cpu_count() or 1)
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for db_dir, path, status, output in pool.map(lambda j: run_tidy(*j),
                                                     jobs):
            reached.add(path)
            parses[db_dir.name] += 1
            fatal, found, headers = parse_output(output, status, root, changed,
                                                 options.all)
            findings.update(found)
            reached.update(headers)
            if fatal:
                failed[path].add(f"{db_dir.name}: {fatal}")

    for (path, number), message in sorted(findings.items()):
        print(f"{path}:{number}: {message}")

    not_covered = sorted(set(changed) - reached)
    if not_covered:
        print("check-braces: no parse reached these, not checked: "
              + ", ".join(not_covered))
    for path in sorted(failed):
        for reason in sorted(failed[path]):
            print(f"check-braces: could not parse {path} ({reason})",
                  file=sys.stderr)

    counts = ", ".join(f"{name} {count}" for name, count in sorted(parses.items()))
    print(f"check-braces: {len(findings)} violation(s), {len(failed)} file(s) "
          f"unparsed; parses: {counts or 'none'}")
    if failed:
        return 2
    return 1 if findings else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # Status 1 would read as "violations found".
        print(f"check-braces: {error!r}", file=sys.stderr)
        sys.exit(2)
