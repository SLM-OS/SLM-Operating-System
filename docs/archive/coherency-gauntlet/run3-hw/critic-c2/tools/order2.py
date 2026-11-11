import sys, glob, os, re
d = sys.argv[1]
for prefix in sys.argv[2:]:
    bad, prev_t, prev_id, nonmono = [], "", 0, []
    for f in sorted(glob.glob(os.path.join(d, prefix + "-run*.txt"))):
        n = int(re.search(r"-run(\d+)\.txt$", f).group(1))
        t = open(f, encoding="utf-8", errors="replace").read()
        run = int(re.search(r"mode=sustained run=(\d+)", t).group(1))
        calls = re.findall(r"^# call (\S+)", t, re.M)
        ids = [int(x) for x in re.findall(r"\(id=(\d+)\) exiting", t)]
        vers = t.find("cat /sys/version")
        if run != n:
            bad.append((n, run))
        if calls[0] < prev_t or min(ids) < prev_id:
            nonmono.append(n)
        prev_t, prev_id = calls[-1], max(ids)
    print(prefix, "run-counter mismatches:", bad or "none", "| non-monotonic time/ids:", nonmono or "none")
