#!/usr/bin/env python3
"""Summarise the build-matrix cells written by scripts/ci/build_cell.sh.

usage: summarize.py <dir with one sub-directory per cell> [summary.json]

Prints a Markdown table (for $GITHUB_STEP_SUMMARY): cell, build ok/fail,
warning count, and "new" warnings -- unique warning lines that the cell has
and its platform's default cell (`<platform>-default`) does not.
"""

import json
import os
import sys
from collections import defaultdict


def load(root):
    cells = {}
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        rj = os.path.join(d, "result.json")
        if not os.path.isfile(rj):
            continue
        with open(rj) as f:
            r = json.load(f)
        wp = os.path.join(d, "warnings.txt")
        r["warnings"] = set(open(wp).read().splitlines()) if os.path.isfile(wp) else set()
        cells[r["id"]] = r
    return cells


def main():
    root = sys.argv[1]
    cells = load(root)
    base = {r["platform"]: r for r in cells.values() if r["id"].endswith("-default")}
    rows, out = [], []
    by_status = defaultdict(int)
    for cid in sorted(cells, key=lambda c: (cells[c]["platform"], c)):
        r = cells[cid]
        b = base.get(r["platform"])
        if b is None or cid == b["id"]:
            new = set()
            new_s = "baseline" if b is not None else "n/a (no baseline)"
        elif b["status"] != "ok":
            new = set()
            new_s = "n/a (baseline failed)"
        else:
            new = r["warnings"] - b["warnings"]
            new_s = str(len(new))
        by_status[r["status"]] += 1
        rows.append(f"| `{cid}` | {r['target']} | {'ok' if r['status'] == 'ok' else '**FAIL**'} "
                    f"| {r['warnings_total']} | {new_s} | {r['seconds']} | {'; '.join(r['notes'])} |")
        out.append(dict(id=cid, platform=r["platform"], target=r["target"], status=r["status"],
                        rc=r["rc"], warnings_total=r["warnings_total"],
                        warnings_unique=r["warnings_unique"], new_warnings=sorted(new),
                        notes=r["notes"], first_errors=r["first_errors"],
                        make_args=r["make_args"], extra_cmake=r["extra_cmake"]))
    print(f"## Build matrix: {by_status['ok']} ok, {by_status['fail']} failed, {len(cells)} cells\n")
    print("Report-only: failures and warnings do not fail the workflow. "
          "\"New\" = unique warning lines absent from the platform's default cell.\n")
    print("| Cell | Target | Build | Warnings | New warnings | Seconds | Notes |")
    print("|---|---|---|---|---|---|---|")
    print("\n".join(rows))
    fails = [o for o in out if o["status"] != "ok"]
    if fails:
        print("\n### First error lines of failed cells\n")
        for o in fails:
            print(f"<details><summary><code>{o['id']}</code> (rc={o['rc']})</summary>\n\n```")
            print("\n".join(o["first_errors"][:15]) or "(no error lines matched; see build.log)")
            print("```\n</details>\n")
    neww = [o for o in out if o["new_warnings"]]
    if neww:
        print("\n### New warnings\n")
        for o in neww:
            print(f"<details><summary><code>{o['id']}</code>: {len(o['new_warnings'])}</summary>\n\n```")
            print("\n".join(o["new_warnings"][:50]))
            print("```\n</details>\n")
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
