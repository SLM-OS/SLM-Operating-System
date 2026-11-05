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
DB_ARGS=(--db db)
BASE_ARGS=(--base main)
EXTRA_ARGS=()
expect() {  # expect <name> <exit status> <grep pattern or "-"> [absent pattern]
    local name="$1" want="$2" pattern="$3" absent="${4:-}" got=0 output
    output="$(python3 "$CHECK" "${DB_ARGS[@]}" "${BASE_ARGS[@]}" \
              ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} 2>&1)" || got=$?
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
import json, shlex, sys
root, flags, files = sys.argv[1], sys.argv[2], sys.argv[3:]
json.dump([{"directory": root, "file": f,
            "command": f"gcc {flags} -c {shlex.quote(f)}"}
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
cat > kernel/src/h.h <<'C'
static inline int h(int x) { return x; }
C
cat > kernel/src/c.c <<'C'
#include "h.h"
int c(int x) { return h(x); }
C
write_db kernel/src/a.c kernel/src/c.c
expect "clean header a parse reaches isn't listed" 0 "0 violation" "not checked"
rm kernel/src/h.h kernel/src/c.c

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

# A clang-tidy crash must not pass as clean. The stand-in only passes through
# the config check.
sed -i 's/return 0;/int y = x + 1; return y;/' kernel/src/a.c
mkdir -p fake
REAL_TIDY="$(command -v clang-tidy)"
cat > fake/clang-tidy <<SH
#!/bin/bash
case "\$*" in *--list-checks*) exec "$REAL_TIDY" "\$@";; esac
kill -SEGV \$\$
SH
chmod +x fake/clang-tidy
echo fake/ >> .gitignore
git add .gitignore
git commit -qm ignore-fake
SAVED_PATH="$PATH"
PATH="$WORK/fake:$PATH"
expect "clang-tidy crash fails the check" 2 "could not parse kernel/src/a.c"
PATH="$SAVED_PATH"
git checkout -q kernel/src/a.c

# A bad config would otherwise switch the rule off.
cp .clang-tidy config.saved
sed -i 's/readability-braces-around-statements/readability-braces-typo/' .clang-tidy
expect "misspelled check in the config fails" 2 "isn't enabled"
echo 'Checks: [unterminated' > .clang-tidy
expect "malformed config fails" 2 "isn't enabled"
mv config.saved .clang-tidy

# An unexpected exception must not read as "violations" (status 1).
sed -i 's/return 0;/int y = x + 1; return y;/' kernel/src/a.c
mv db/compile_commands.json db/saved.json
expect "unexpected exception exits 2" 2 "check-braces:"
mv db/saved.json db/compile_commands.json
git checkout -q kernel/src/a.c

# --all reports the older violation with nothing changed.
EXTRA_ARGS=(--all)
expect "--all reports older violations" 1 "kernel/src/a.c:3:.*readability-braces"
EXTRA_ARGS=()

# A base that doesn't exist can't be diffed.
BASE_ARGS=(--base no-such-ref)
expect "unknown base fails the check" 2 "can't diff against no-such-ref"
BASE_ARGS=(--base main)

# A statement using a generated macro must not be discarded. The stand-in
# header defines them, so this unbraced body is flagged.
cat > kernel/src/e.c <<'C'
#include "build_info.h"
void f(const char *s);
void e(int x)
{
    if (x)
        f("v " SLMOS_VERSION SLMOS_BUILD_STAMP SLMOS_BUILD_SHA);
}
C
write_db kernel/src/a.c kernel/src/e.c
expect "body using a generated macro is flagged" 1 "kernel/src/e.c:5:.*readability-braces"
rm kernel/src/e.c

# Any other undeclared name on a changed line discards the statement, so the
# line can't be checked.
cat > kernel/src/e.c <<'C'
void e(int x)
{
    if (x)
        x = NOT_DECLARED_ANYWHERE;
}
C
write_db kernel/src/a.c kernel/src/e.c
expect "undeclared name on a changed line fails" 2 "could not parse kernel/src/e.c.*undeclared"
rm kernel/src/e.c

cat > kernel/src/e.c <<'C'
void f(const char *s);
void e(int x)
{
    if (x)
        f("v " NOT_A_MACRO "\n");
}
C
write_db kernel/src/a.c kernel/src/e.c
expect "syntax error on a changed line fails" 2 "could not parse kernel/src/e.c.*expected"
rm kernel/src/e.c

# Every macro in the weights stand-in is defined, or edits to that file could
# never pass.
cat > kernel/src/e.c <<'C'
#include "ga10b_eviction_mlp_weights_gen.h"
const float w[1] = { GA10B_EVICT_MLP_W_L1 };
const float b[1] = { GA10B_EVICT_MLP_B_OUT };
_Static_assert(GA10B_EVICT_MLP_W_OUT_COUNT == 16, "W_OUT size");
C
write_db kernel/src/a.c kernel/src/e.c
expect "generated weight macros on changed lines pass" 0 "0 violation"
rm kernel/src/e.c

# Other compile errors are tolerated: the finding beside one is still reported.
cat > kernel/src/e.c <<'C'
struct s { int a; };
void e(struct s *p, int x)
{
    if (x)
        p->bogus = 1;
}
C
write_db kernel/src/a.c kernel/src/e.c
expect "body beside a tolerated error is flagged" 1 "kernel/src/e.c:4:.*readability-braces"
rm kernel/src/e.c

# An undeclared name only matters on a changed line.
cat > kernel/src/e.c <<'C'
int e(void)
{
    int y = 1;
    return NOT_DECLARED_ANYWHERE + y;
}
C
write_db kernel/src/a.c kernel/src/e.c
git add kernel/src/e.c
git commit -qm undeclared
sed -i 's/int y = 1;/int y = 2;/' kernel/src/e.c
expect "undeclared name on an unchanged line is tolerated" 0 "0 violation"
git rm -qf kernel/src/e.c
git commit -qm remove-undeclared
write_db kernel/src/a.c

# An added line starting "++ " appears as "+++ " in the diff, and must not read
# as a file header that hides the file's later hunks.
cat > kernel/src/g.c <<'C'
int g(int x, int y)
{
    x = 1;
    x = 2;
    x = 3;
    x = 4;
    x = 5;
    if (x)
        return y;
    return x;
}
C
write_db kernel/src/a.c kernel/src/g.c
git add kernel/src/g.c
git commit -qm later-hunk
sed -i '3a ++ x;' kernel/src/g.c
sed -i 's/return y;/return y + 1;/' kernel/src/g.c
expect "added line starting with ++ keeps later hunks" 1 "kernel/src/g.c:9:.*readability-braces"
git rm -qf kernel/src/g.c
git commit -qm remove-later-hunk
write_db kernel/src/a.c

# Git decorates some paths in a diff: a tab after one with a space, and quotes
# around one with a backslash or control character. None may drop out of the
# check.
for name in 'sp ace' 'caf\xc3\xa9' 'back\\slash' 'ct\001l'; do
    file="kernel/src/$(printf "$name").c"
    pattern="$(printf "$name" | sed 's/[^a-z]/./g')\.c:3:.*readability-braces"
    printf 'int n(int x)\n{\n    if (x)\n        return 1;\n    return 0;\n}\n' > "$file"
    write_db kernel/src/a.c "$file"
    expect "untracked unusual path is checked: $file" 1 "$pattern"
    git add "$file"
    git commit -qm unusual-path
    sed -i 's/return 1;/return 2;/' "$file"
    expect "changed unusual path is checked: $file" 1 "$pattern"
    git rm -qf "$file"
    git commit -qm remove-unusual-path
done
write_db kernel/src/a.c

# WarningsAsErrors turns findings into errors, which must still count.
cat >> .clang-tidy <<'YAML'
WarningsAsErrors: '*'
YAML
sed -i '/^\.\.\.$/d' .clang-tidy
sed -i 's/return 1;/return 2;/' kernel/src/a.c
write_db kernel/src/a.c
expect "WarningsAsErrors does not hide violations" 1 "kernel/src/a.c:3:.*readability-braces"
git checkout -q kernel/src/a.c .clang-tidy

# Non-UTF-8 text, as in some vendored lwIP files, must not abort the run.
printf '/* caf\xe9 */\n' >> kernel/lib/vendored.c
printf '/* caf\xe9 */\n' >> kernel/src/a.c
expect "non-UTF-8 text is tolerated" 0 "0 violation"
git checkout -q kernel/lib/vendored.c kernel/src/a.c

# --make-db generates each database through `make tidy-db`, naming the variant.
sed -i 's/return 0;/int y = x + 1; return y;/' kernel/src/a.c
cat > Makefile <<'MK'
tidy-db:
	@test "$(PLATFORM)" != BROKEN
	@mkdir -p db/$(PLATFORM)$(if $(filter test,$(TIDY_VARIANT)),-test)
	@cp db/compile_commands.json db/$(PLATFORM)$(if $(filter test,$(TIDY_VARIANT)),-test)/
MK
git add Makefile
git commit -qm makefile
DB_ARGS=(--db db/PLAT --db db/PLAT-test)
EXTRA_ARGS=(--make-db)
expect "--make-db builds main and test databases" 0 "parses: PLAT 1, PLAT-test 1"
DB_ARGS=(--db db/BROKEN)
expect "--make-db failure fails the check" 2 "configuring BROKEN failed"
DB_ARGS=(--db db)
EXTRA_ARGS=()
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
