#!/bin/bash
# classify.sh <known-failures file> <suite capture>...
#
# Classifies the [FAIL] lines of saved test-kernel suite captures against a
# known-failures list. Works on saved files only; it builds and boots nothing.
#
# Known-failures file, one entry per line, TAB-separated:
#   <test name><TAB>#<issue>     a counted test that fails, and the issue that owns it
#   check:<text><TAB>#<issue>    a [FAIL] line printed inside a listed test
#                                (not a test, not counted)
#   lines starting with '#' and blank lines are ignored
# Every entry must name an issue; an entry without one is an error (exit 2).
#
# Prints one line per capture: build, boot mode, the suite's summary line, the
# number of failing tests, failing tests per issue, any failing test that is
# not in the list, listed tests that did not fail (per issue), and halts.
# Exit status: 0 when no capture has an unlisted failure, 1 otherwise.

set -u

if [ $# -lt 2 ]; then
    echo "usage: $0 <known-failures file> <suite capture>..." >&2
    exit 2
fi

K=$1; shift
TAB=$'\t'

entries=$(grep -v -E '^(#|[[:space:]]*$)' "$K")
bad=$(echo "$entries" | grep -v -E "^[^${TAB}]+${TAB}#[0-9]+\$")
if [ -n "$bad" ]; then
    echo "$K: entries without an issue number (want <name><TAB>#<issue>):" >&2
    echo "$bad" >&2
    exit 2
fi

status=0
for f in "$@"; do
    fails=$(grep -a '^  \[FAIL\] ' "$f" | tr -d '\r' | sed 's/^  \[FAIL\] //' | sort -u)

    # One record per line: "F <issue>" known failing test, "N <name>" failing
    # test not in the list, "S <issue>" listed test that did not fail.
    recs=$(awk -F'\t' '
        NR == FNR {
            if ($1 ~ /^check:/) { chk[substr($1, 7)] = $2 } else { known[$1] = $2 }
            next
        }
        $0 == ""       { next }
        $0 in chk      { next }
        $0 in known    { print "F " known[$0]; seen[$0] = 1; next }
                       { print "N " $0 }
        END { for (k in known) if (!(k in seen)) print "S " known[k] }
    ' <(echo "$entries") <(echo "$fails"))

    per_issue() {   # $1 = record tag; prints "#a xN, #b xM" or "none"
        local out
        out=$(echo "$recs" | grep "^$1 " | cut -d' ' -f2 | sort -t'#' -k2 -n | uniq -c |
              awk '{ printf "%s%s x%s", (NR > 1 ? ", " : ""), $2, $1 }')
        echo "${out:-none}"
    }

    n=$(echo "$recs" | grep -c -E '^(F|N) ')
    new=$(echo "$recs" | grep '^N ' | cut -d' ' -f2- | paste -sd';' | sed 's/;/; /g')
    [ -n "$new" ] && status=1

    echo "$(basename "$f")" \
         "| $(grep -a -m1 -oE 'build [0-9]{14} \([0-9a-f]+\)' "$f")" \
         "| $(grep -a -m1 -o 'Boot-mode : [A-Za-z]*' "$f")" \
         "| $(grep -a -E 'All test suites passed|total failure' "$f" | tr -d '\r' | tail -1)" \
         "| failing tests $n" \
         "| by issue: $(per_issue F)" \
         "| not in known list: ${new:-none}" \
         "| listed, not failing: $(per_issue S)" \
         "| halts: $(grep -a -c -E 'System halted|PAGE FAULT|KERNEL PANIC' "$f")"
done
exit $status
