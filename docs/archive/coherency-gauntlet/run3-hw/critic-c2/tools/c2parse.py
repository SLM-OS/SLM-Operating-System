# Critic C2 parser (coherency run 3). Written by the critic; does not use
# the builder's tools. Usage: c2parse.py <dir> <glob-prefix> <head8> [...]
import glob, os, re, statistics as st, sys, json

FIELDS = ("cpu exec att diag_att ok diag_ok iso f_stale f_pd f_empty f_other chk "
          "v_pd v_pd_work v_busy v_empty v_gen v_state v_cpu v_aff v_rm pushfull "
          "dq_push dq_drains dq_drain_us dq_last_push_us first_att_us first_ok_us "
          "last_att_us ticks idle sched lk_hw log w_in w_in_unk w_off ph_w ph_w_pd "
          "ph_own tick_n tick_first_us tick_last_us win_n win_sum_us win_max_us wph wlog").split()
VEC = {"w_in": 5, "w_off": 8, "ph_w": 11, "ph_w_pd": 11, "ph_own": 11,
       "win_n": 5, "win_sum_us": 5, "win_max_us": 5, "wph": 11}


def parse_file(path, head):
    txt = open(path, encoding="utf-8", errors="replace").read()
    probs = []
    m = re.search(r"^commit:\s+([0-9a-f]+)", txt, re.M)
    commit = m.group(1) if m else None
    if commit != head:
        probs.append("banner %s != head %s" % (commit, head))
    # version must come before the bench
    vpos = txt.find("commit:")
    bpos = txt.find("Work-Stealing Sustained-Supply")
    if vpos < 0 or bpos < 0 or vpos > bpos:
        probs.append("version not at top / bench missing")
    hm = re.search(r"c2t-format v(\d+) mode=(\S+) run=(\d+) cntfrq=(\d+) cpus=(\d+) tasks=(\d+)", txt)
    if not hm or hm.group(1) != "2":
        probs.append("no c2t-format v2 header")
    cm = re.search(r"Completed:\s+(\d+) / (\d+)", txt)
    dist = {int(a): int(b) for a, b in re.findall(r"CPU (\d): (\d+) tasks", txt)}
    rows = {}
    for line in txt.splitlines():
        if not line.startswith("  c2t cpu="):
            continue
        toks = line.split()[1:]
        d = {}
        for t in toks:
            k, _, v = t.partition("=")
            d[k] = v
        missing = [f for f in FIELDS if f not in d]
        if missing:
            probs.append("cpu %s missing %s" % (d.get("cpu"), missing))
        r = {}
        for k, v in d.items():
            if k in VEC:
                xs = v.split(",")
                if len(xs) != VEC[k]:
                    probs.append("cpu %s %s has %d bins" % (d.get("cpu"), k, len(xs)))
                r[k] = [int(x) for x in xs]
            elif k in ("log", "wlog"):
                r[k] = v
            elif v == "-":
                r[k] = None
            else:
                try:
                    r[k] = int(v)
                except ValueError:
                    r[k] = v
        rows[r["cpu"]] = r
    if sorted(rows) != list(range(6)):
        probs.append("cpus present %s" % sorted(rows))
    tasks = int(hm.group(6)) if hm else None
    if cm and (cm.group(1) != cm.group(2)):
        probs.append("completed %s/%s" % cm.groups())
    ex = sum(r.get("exec", 0) for r in rows.values())
    if tasks is not None and ex != tasks:
        probs.append("sum exec %d != tasks %d" % (ex, tasks))
    for c, r in rows.items():
        if dist.get(c) != r.get("exec"):
            probs.append("cpu %d dist %s != exec %s" % (c, dist.get(c), r.get("exec")))
        s = sum(r.get(k, 0) or 0 for k in ("ok", "iso", "f_stale", "f_pd", "f_empty", "f_other"))
        if r.get("att", 0) - s != r.get("chk"):
            probs.append("cpu %d chk printed %s recomputed %d" % (c, r.get("chk"), r["att"] - s))
    return dict(path=path, commit=commit, run=int(hm.group(3)) if hm else None,
                tasks=tasks, rows=rows, probs=probs)


def med(xs):
    return st.median(xs) if xs else None


def main():
    d, prefix, head = sys.argv[1], sys.argv[2], sys.argv[3]
    files = sorted(glob.glob(os.path.join(d, prefix + "-run*.txt")))
    runs = [parse_file(f, head) for f in files]
    out = dict(cell=prefix, n=len(runs), files=[os.path.basename(r["path"]) for r in runs])
    out["problems"] = {os.path.basename(r["path"]): r["probs"] for r in runs if r["probs"]}
    out["chk"] = {}
    for r in runs:
        for c, row in r["rows"].items():
            if row.get("chk"):
                out["chk"].setdefault(os.path.basename(r["path"]), {})[c] = row["chk"]
            if row.get("att") != row.get("diag_att"):
                out["chk"].setdefault(os.path.basename(r["path"]), {})["att-diag cpu%d" % c] = row["att"] - row["diag_att"]
    per = {}
    for c in range(6):
        sh = [100.0 * r["rows"][c]["exec"] / r["tasks"] for r in runs if c in r["rows"]]
        att = [r["rows"][c]["att"] for r in runs]
        ok = [r["rows"][c]["ok"] for r in runs]
        fo = [r["rows"][c]["first_ok_us"] for r in runs]
        zero = [i + 1 for i, r in enumerate(runs) if r["rows"][c]["ok"] == 0]
        A = sum(att) or 1
        p = dict(
            share_med=med(sh), share_min=min(sh), share_max=max(sh),
            att_sum=sum(att), att_med=med(att), ok_sum=sum(ok), ok_med=med(ok),
            exec_sum=sum(r["rows"][c]["exec"] for r in runs),
            frac={k: 100.0 * sum(r["rows"][c][k] for r in runs) / A
                  for k in ("f_stale", "f_pd", "f_empty", "f_other", "iso")},
            fpd_run_med=med([100.0 * r["rows"][c]["f_pd"] / r["rows"][c]["att"]
                             for r in runs if r["rows"][c]["att"]]),
            first_ok_med_excl=med([x for x in fo if x is not None]),
            first_ok_max_excl=max([x for x in fo if x is not None], default=None),
            first_ok_med_cens=med([x if x is not None else 10**12 for x in fo]),
            zero_runs=zero,
            w_in=[sum(r["rows"][c]["w_in"][i] for r in runs) for i in range(5)],
            w_in_unk=sum(r["rows"][c]["w_in_unk"] for r in runs),
            w_in_pct=100.0 * sum(sum(r["rows"][c]["w_in"]) for r in runs) / A,
            w_in_run_med=med([100.0 * sum(r["rows"][c]["w_in"]) / r["rows"][c]["att"]
                              for r in runs if r["rows"][c]["att"]]),
        )
        for k in ("w_off", "ph_w", "ph_w_pd", "ph_own", "wph", "win_n", "win_sum_us"):
            p[k] = [sum(r["rows"][c][k][i] for r in runs) for i in range(VEC[k])]
        p["win_max_us"] = [max(r["rows"][c]["win_max_us"][i] for r in runs) for i in range(5)]
        per[c] = p
    out["per"] = per
    # watched CPU windows from wlog (CPU 1)
    wl = {"T": [], "C": [], "P": [], "O": [], "B": []}
    for r in runs:
        s = r["rows"][1]["wlog"]
        if s and s != "-":
            for e in s.split(","):
                a, b, rr = e.split(":")
                wl.setdefault(rr, []).append(int(b))
    out["wlog"] = {k: (len(v), med(v), max(v) if v else None) for k, v in wl.items()}
    print(json.dumps(out, indent=1, default=str))


if __name__ == "__main__":
    main()
