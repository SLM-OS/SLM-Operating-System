#!/usr/bin/env python3
"""Check C control-flow braces on changed lines with clang-tidy.

Runs the .clang-tidy brace rule over every compile database, because code
under #ifdef PLATFORM_* is only visible to the platform that enables it.
Findings are limited to lines changed since the merge base with --base,
including uncommitted and untracked files; the tree has older violations that
aren't reported unless --all is given. Vendored kernel/lib/ and nanopb output
are exempt.

A file with no database entry (host tools, for example) is listed as not
covered. A file clang can't parse on any platform can't be checked, so it
fails the run rather than passing silently.

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
# Generated during a build. The check only configures, so empty stand-ins keep
# clang parsing; a real generated header takes precedence if one exists.
GENERATED_HEADERS = ("build_info.h", "ga10b_eviction_mlp_weights_gen.h")
DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*)$")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True,
                          text=True).stdout


def exempt(path):
    return path.startswith("kernel/lib/") or path.endswith((".pb.c", ".pb.h"))


def changed_lines(base):
    """Map each changed C path to the set of its added or modified lines."""
    merge_base = git("merge-base", base, "HEAD").strip()
    changed = collections.defaultdict(set)
    path = None
    for line in git("diff", "-U0", "--no-color", merge_base).splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and path:
            match = re.search(r"\+(\d+)(?:,(\d+))? @@", line)
            start, count = int(match[1]), int(match[2] or 1)
            changed[path].update(range(start, start + count))
    for path in git("ls-files", "--others", "--exclude-standard").splitlines():
        if path.endswith(C_SUFFIXES):
            count = len(Path(path).read_text(errors="replace").splitlines())
            changed[path].update(range(1, count + 1))
    return {path: lines for path, lines in changed.items()
            if path.endswith(C_SUFFIXES) and not exempt(path)}


def compiler_args(compiler):
    """Clang needs the cross compiler's target and system include paths."""
    args = []
    name = os.path.basename(compiler)
    if name.startswith("aarch64-"):
        args.append("--extra-arg-before=--target=aarch64-none-elf")
    elif name.startswith("x86_64-"):
        args.append("--extra-arg-before=--target=x86_64-none-elf")
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
    for header in GENERATED_HEADERS:
        (stubs / header).touch()
    args = ["--extra-arg=-w", "--extra-arg=-ferror-limit=0",
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


def run_tidy(db_dir, args, config, path):
    result = subprocess.run(
        ["clang-tidy", "-p", str(db_dir), "--quiet", "--config-file=" + config,
         "--header-filter=.*", *args, path],
        capture_output=True, text=True)
    return db_dir, path, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--db", action="append", required=True, type=Path,
                        help="compile database directory; repeat per platform")
    parser.add_argument("--base", default="origin/main",
                        help="ref to diff against (default: origin/main)")
    parser.add_argument("--all", action="store_true",
                        help="report every violation, not just changed lines")
    parser.add_argument("--make-db", action="store_true",
                        help="generate databases with `make tidy-db` first; "
                             "each directory's name is its PLATFORM")
    options = parser.parse_args()

    if not shutil.which("clang-tidy"):
        print("check-braces: clang-tidy not found; install it to run this "
              "check", file=sys.stderr)
        return 2
    root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
    os.chdir(root)

    changed = {}
    if not options.all:
        try:
            changed = changed_lines(options.base)
        except subprocess.CalledProcessError as error:
            print(f"check-braces: can't diff against {options.base}: "
                  f"{error.stderr.strip()}", file=sys.stderr)
            return 2
        if not changed:
            print("check-braces: no changed C lines")
            return 0

    if options.make_db:
        for db_dir in options.db:
            result = subprocess.run(
                ["make", "--no-print-directory", "tidy-db",
                 "PLATFORM=" + db_dir.name], capture_output=True, text=True)
            if result.returncode:
                print(f"check-braces: configuring {db_dir.name} failed "
                      f"(is its cross compiler on PATH?)\n{result.stdout}"
                      f"{result.stderr}", file=sys.stderr)
                return 2

    # A changed header can affect any file, so only a pure .c change narrows
    # the list of files to run.
    headers_changed = options.all or any(p.endswith(".h") for p in changed)
    config = str(root / ".clang-tidy")
    jobs = []
    in_database = set()
    for db_dir in options.db:
        args, files = load_database(db_dir.resolve(), root)
        in_database.update(files)
        for path in sorted(set(files)):
            if headers_changed or path in changed:
                jobs.append((db_dir.resolve(), args, config, path))

    findings = {}
    parsed = set()
    failed = collections.defaultdict(set)
    workers = min(8, os.cpu_count() or 1)
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for db_dir, path, output in pool.map(lambda j: run_tidy(*j), jobs):
            fatal = None
            for line in output.splitlines():
                match = DIAGNOSTIC.match(line)
                if not match:
                    continue
                if match[4] == "error" and "file not found" in match[5]:
                    fatal = f"{match[1]}:{match[2]}: {match[5]}"
                elif (match[4] == "warning" and
                      "readability-braces-around-statements" in match[5]):
                    location = Path(match[1]).resolve()
                    if not location.is_relative_to(root):
                        continue
                    relative = str(location.relative_to(root))
                    number = int(match[2])
                    if not exempt(relative) and (
                            options.all or number in changed.get(relative, ())):
                        findings[(relative, number)] = match[5]
            if fatal:
                failed[path].add(f"{db_dir.name}: {fatal}")
            else:
                parsed.add(path)

    for (path, number), message in sorted(findings.items()):
        print(f"{path}:{number}: {message}")

    not_covered = sorted(p for p in changed if p.endswith(".c")
                         and p not in in_database)
    if not_covered:
        print("check-braces: not in any compile database, not checked: "
              + ", ".join(not_covered))
    unchecked = sorted(set(failed) - parsed)
    for path in unchecked:
        for reason in sorted(failed[path]):
            print(f"check-braces: could not parse {path} ({reason})",
                  file=sys.stderr)

    print(f"check-braces: {len(findings)} violation(s), {len(jobs)} parses, "
          f"{len(unchecked)} file(s) unparsed")
    if findings:
        return 1
    return 2 if unchecked else 0


if __name__ == "__main__":
    sys.exit(main())
