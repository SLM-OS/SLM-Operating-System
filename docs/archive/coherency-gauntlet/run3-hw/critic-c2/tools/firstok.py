import sys, glob, os, statistics as st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from c2parse import parse_file  # noqa: E402

d, prefix, head = sys.argv[1], sys.argv[2], sys.argv[3]
runs = [parse_file(f, head) for f in sorted(glob.glob(os.path.join(d, prefix + "-run*.txt")))]
for c in (2, 3, 4, 5):
    fo = [r["rows"][c]["first_ok_us"] for r in runs]
    ok = [r["rows"][c]["ok"] for r in runs]
    sh = [round(100.0 * r["rows"][c]["exec"] / r["tasks"], 1) for r in runs]
    pd = [round(100.0 * r["rows"][c]["f_pd"] / r["rows"][c]["att"], 1) for r in runs]
    excl = [x for x in fo if x is not None]
    c25 = [x if x is not None else 2500000 for x in fo]
    cinf = [x if x is not None else float("inf") for x in fo]
    def m(xs):
        try:
            return st.median(xs)
        except Exception:
            return None
    print("cpu%d first_ok_us per run:" % c, fo)
    print("   ok per run:", ok)
    print("   share per run:", sh)
    print("   pd%% per run:", pd)
    print("   median excl=%s  zero=2.5s:%s  zero=inf:%s  max excl=%s" % (m(excl), m(c25), m(cinf), max(excl)))
fo23 = [r["rows"][c]["first_ok_us"] for r in runs for c in (2, 3)]
print("pooled cpu2+3 median excl=%s zero=2.5s:%s" % (st.median([x for x in fo23 if x is not None]),
      st.median([x if x is not None else 2500000 for x in fo23])))
