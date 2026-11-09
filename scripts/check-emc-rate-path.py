#!/usr/bin/env python3
"""check-emc-rate-path.py - no MRQ_CLK set_rate on the Jetson memory clock (#1161).

On Tegra234 the EMC rate belongs to BPMP's bandwidth manager. A rate set
with MRQ_CLK (bpmp_clk_set_rate) is put back to the bandwidth manager's
choice at its next recompute, which after a UEFI-direct boot happens
within about 250 ms and again whenever any client sends MRQ_BWMGR_INT.
Ask for a floor instead: emc_hold_set_target() or bpmp_emc_floor_khz().

Usage: check-emc-rate-path.py [file or directory ...]   (default: kernel/)
Exits 1 and names each call that sets the EMC clock through MRQ_CLK.
"""
import os
import re
import sys

# The EMC clock id under the names the tree uses, or the bare number.
EMC_IDS = r"(?:TEGRA234_CLK_EMC|T234_CLK_EMC|31[uU]?)"
CALL = re.compile(r"\bbpmp_clk_set_rate\s*\(\s*" + EMC_IDS + r"\s*,")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def strip_comments(text):
    # Keep newlines so line numbers still match.
    return COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def sources(paths):
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, files in os.walk(path):
                for name in sorted(files):
                    if name.endswith((".c", ".h")):
                        yield os.path.join(root, name)
        else:
            yield path


def main(argv):
    paths = argv[1:] or ["kernel"]
    bad = []
    for path in sources(paths):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = strip_comments(f.read())
        for m in CALL.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            bad.append(f"{path}:{line}")
    if bad:
        for where in bad:
            print(f"check-emc-rate-path: {where}: EMC rate set through MRQ_CLK; "
                  "BPMP's bandwidth manager discards it (#1161). Request a floor "
                  "with emc_hold_set_target() or bpmp_emc_floor_khz().")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
