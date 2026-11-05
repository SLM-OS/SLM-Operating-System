#!/bin/bash
#
# test-check-c-braces.sh - Host-only test for scripts/check-c-braces.py.
#
# Builds a small git repository with its own compile database, so no cross
# toolchain or kernel configure is needed. clang-tidy is required; the test
# skips without it, since `make check-braces` reports that case itself.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CHECK="$SCRIPT_DIR/check-c-braces.py"
CONFIG="$SCRIPT_DIR/../.clang-tidy"

if ! command -v clang-tidy >/dev/null; then
    echo "SKIP: clang-tidy not installed"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
git init -q -b main .
git config user.email test@example.invalid
git config user.name test
cp "$CONFIG" .clang-tidy
echo db/ > .gitignore
mkdir -p kernel/lib kernel/src db

fails=0
expect() {  # expect <name> <exit status> <grep pattern or "-"> [absent pattern]
    local name="$1" want="$2" pattern="$3" absent="${4:-}" got=0 output
    output="$(python3 "$CHECK" --db db --base main 2>&1)" || got=$?
    if [[ "$got" != "$want" ]]; then
        echo "FAIL: $name: exit $got, wanted $want"; echo "$output"
        fails=$((fails + 1))
    elif [[ "$pattern" != "-" ]] && ! grep -qE "$pattern" <<<"$output"; then
        echo "FAIL: $name: no match for '$pattern'"; echo "$output"
        fails=$((fails + 1))
    elif [[ -n "$absent" ]] && grep -qE "$absent" <<<"$output"; then
        echo "FAIL: $name: unexpected match for '$absent'"; echo "$output"
        fails=$((fails + 1))
    else
        echo "PASS: $name"
    fi
}

FLAGS=""
write_db() {
    python3 - "$WORK" "$FLAGS" "$@" <<'PY'
import json, sys
root, flags, files = sys.argv[1], sys.argv[2], sys.argv[3:]
json.dump([{"directory": root, "file": f, "command": f"gcc {flags} -c {f}"}
           for f in files], open("db/compile_commands.json", "w"))
PY
}

# An old violation on line 3 of a committed file.
cat > kernel/src/a.c <<'C'
int a(int x)
{
    if (x)
        return 1;
    return 0;
}
C
cat > kernel/lib/vendored.c <<'C'
int v(int x)
{
    return x;
}
C
cat > kernel/src/gen.pb.c <<'C'
int g(int x)
{
    return x;
}
C
write_db kernel/src/a.c kernel/lib/vendored.c kernel/src/gen.pb.c
git add -A
git commit -qm base

expect "no changes" 0 "no changed C lines"

# A new line elsewhere in the file: the old violation isn't reported.
sed -i 's/return 0;/int y = x + 1; return y;/' kernel/src/a.c
expect "older violation in a touched file is ignored" 0 "0 violation" "a.c:3"
git checkout -q kernel/src/a.c

cat > kernel/src/b.c <<'C'
int guard(int x)
{
    if (x) return 1;
    return 0;
}
int braced(int x)
{
    if (x) {
        return 1;
    } else {
        return 2;
    }
}
C
write_db kernel/src/a.c kernel/lib/vendored.c kernel/src/gen.pb.c kernel/src/b.c
expect "single-line guard and braced code pass (untracked file)" 0 "0 violation"

cat >> kernel/src/b.c <<'C'
int multi(int x)
{
    if (x)
        return 1;
    return 0;
}
C
expect "multi-line body is flagged" 1 "kernel/src/b.c:16:.*readability-braces"

cat > kernel/src/b.c <<'C'
int chain(int x)
{
    if (x) return 1;
    else return 2;
}
C
expect "chained if/else is flagged" 1 "kernel/src/b.c:[34]:.*readability-braces"

cat > kernel/src/b.c <<'C'
int loop(int x)
{
    for (int i = 0; i < x; i++)
        x--;
    return x;
}
C
expect "multi-line loop body is flagged" 1 "kernel/src/b.c:3:.*readability-braces"
rm kernel/src/b.c
write_db kernel/src/a.c kernel/lib/vendored.c kernel/src/gen.pb.c

# Editing the body of an older unbraced statement counts as a change to it.
sed -i 's/return 1;/return 2;/' kernel/src/a.c
expect "edit to an older unbraced body is flagged" 1 "kernel/src/a.c:3:.*readability-braces"

# Git settings that change the diff prefixes must not hide changes.
git config diff.noprefix true
expect "diff.noprefix does not hide changes" 1 "kernel/src/a.c:3:"
git config --unset diff.noprefix
git config diff.mnemonicPrefix true
expect "diff.mnemonicPrefix does not hide changes" 1 "kernel/src/a.c:3:"
git config --unset diff.mnemonicPrefix
git checkout -q kernel/src/a.c

# Exempt paths, even with a violation on a changed line.
cat > kernel/lib/vendored.c <<'C'
int v(int x)
{
    if (x)
        return 1;
    return x;
}
C
cp kernel/lib/vendored.c kernel/src/gen.pb.c
expect "kernel/lib and .pb.c are exempt" 0 "no changed C lines"
git checkout -q kernel/lib/vendored.c kernel/src/gen.pb.c

# Changed lines in a header are checked through the files that include it.
cat > kernel/src/h.h <<'C'
static inline int h(int x)
{
    if (x)
        return 1;
    return 0;
}
C
cat > kernel/src/c.c <<'C'
#include "h.h"
int c(int x) { return h(x); }
C
write_db kernel/src/a.c kernel/src/c.c
expect "violation in a changed header is flagged" 1 "kernel/src/h.h:3:.*readability-braces"
rm kernel/src/c.c
write_db kernel/src/a.c
expect "changed header no parse reaches is reported" 0 "not checked: kernel/src/h.h"
rm kernel/src/h.h

# A file clang can't parse must not pass silently.
cat > kernel/src/d.c <<'C'
#include "does_not_exist.h"
int d(int x) { return x; }
C
write_db kernel/src/a.c kernel/src/d.c
expect "unparsable file fails the check" 2 "could not parse kernel/src/d.c"
rm kernel/src/d.c

# A compile flag clang rejects stops it analyzing the file, with an error that
# has no file:line prefix. This is how Jetson's -mcpu=cortex-a78ae first failed.
sed -i 's/return 0;/int y = x + 1; return y;/' kernel/src/a.c
FLAGS="-march=not-a-cpu"
write_db kernel/src/a.c
expect "flag clang rejects fails the check" 2 "could not parse kernel/src/a.c"
FLAGS=""
write_db kernel/src/a.c
git checkout -q kernel/src/a.c

# A changed C file outside the database is reported, not failed.
mkdir -p tools
cat > tools/host.c <<'C'
int host(int x) { return x; }
C
write_db kernel/src/a.c
expect "file outside the database is reported" 0 "not checked: tools/host.c"

if [[ "$fails" != 0 ]]; then
    echo "$fails test(s) failed"
    exit 1
fi
echo "All check-c-braces tests passed"
